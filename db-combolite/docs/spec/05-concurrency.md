# Section 5: Concurrency and coordination

Status: **Draft. The decisions below are confirmed; the open questions are not.**
This section was moved here from Section 1.8. Section 1 describes the archive, the companion files and the two header copies that this section depends on.

## 5.1 Principles

- **Serverless.** db-combolite has no daemon, no broker and no server process. Sessions, CRUD, multi-tenancy and coordination are all handled by the library running inside each process that opens the database. Coordination works only through the archive and its companion files. It does not depend on operating-system file locks.
- **Single host.** A database instance is accessed from one machine only. There is no clustering.
- **Reads.** A process can read the archive at any time. What it sees is the state as of the most recent reconciliation.
- **Shared writers.** Every process that writes to a shared database has an **identity key**. Its edits are recorded in a **keyed transaction tree** that attributes each edit to that identity. Those edits are coordinated with any pending delta reconciliation.
- **Two kinds of identity (review G4).**
  - A **session identity** is created per coordinator and lasts only for that session. It is used for coordination: slots, claims and tie-breaks.
  - A **principal identity** is registered by an operator (Section 12.5). It is used for attribution, signing and RBAC.
  - A session authenticates as a principal.
- **Writes don't need the baton (review R3).** Each process appends to its **own** delta tracks (Section 1.3). The baton is needed only to reconcile and compact.

## 5.2 The coordinator

Serverless does not mean uncoordinated. Loading the library creates an in-process **coordinator**, whose only job is to fill the role a database server would normally play:

- it tracks delta state;
- it notifies crash handlers;
- it coordinates the baton and request priorities.

The coordinator is not a separate process or daemon. It runs on the caller's stack as a lightweight push/pop operation, which keeps stack use small enough for microcontrollers.

### One primary, the rest subordinate

- The first coordinator to start becomes the **primary** and sets the `PRIMARY_COORD` bit. `PRIMARY_COORD` is claimed through the append → settle → choose protocol (5.5), so two coordinators starting at the same moment still produce exactly one primary.
- Every coordinator that starts later is **subordinate** to the primary. If a subordinate tries to claim primary, the claim is rejected.
- The primary is the **clock source** that every coordinator synchronizes to.
- Coordinators exchange heartbeats. If the primary's heartbeat stops, the subordinates **elect a new primary from among themselves by priority value**.
- **Stale primary bit.** A process may open the database and find `PRIMARY_COORD` set while no primary is running, for example because the primary crashed when no subordinates existed. In that case the opener checks the primary's heartbeat. If the heartbeat is dead, the opener takes over as primary.

## 5.3 The baton

- The right to run reconciliation and compaction is called the **baton**. Only the process holding the baton may reconcile or compact.
- The baton is passed between processes in one of two ways:
  - **round-robin**, or
  - through a request queue with **early congestion detection** (in the style of RED). Under load, this queue pushes back on requesters before it fills up.
- **Multiple heats.** If a single baton becomes a bottleneck, more than one baton can exist, for example one per delta track. Each heat then reconciles independently.
- **Request → process → handoff.** Before a process requests the baton, it reads three things: the committed state, the pending deltas and the reconciliation state. Only then does it queue its own operations. The baton queue is part of the write-ahead log (WAL) queue.

## 5.4 Baton ticket: lifecycle and crash recovery

A baton works like a two-part claim ticket:

1. **Request.** A process requests a baton because it has reconciliation or compaction work pending. The request declares *x-total*, the size of that work.
2. **Issue.** The baton is time-limited. Each baton has a unique ID that only ever increases, held in its **CTS** (the start-after marker, which is the previous baton's expiration token). Any write stamped with a CTS older than the current baton's is rejected. This fences out a holder that stalled and later wakes up.
3. **Start window.** The holder must begin writing within a fixed time. If it doesn't, a roll-off countdown starts.
4. **Heartbeats.** While it writes, the holder emits heartbeats. Each heartbeat carries its serialized progress (*x / x-total*).
5. **Close.** The holder ends the session with a closing pattern that matches its heartbeat sequence.

**Service window.** The coordinator gives each baton a service window, measured in milliseconds and sized from the declared *x-total*.

**Crash watch.** The coordinator tracks the heartbeats and the *x / x-total* progress. If a crash or stall makes the baton time out, the coordinator closes the baton and moves on to the next request in the queue.

**Crashed sessions are kept, not discarded.** When the crashed or stalled process recovers and asks for a new baton, it is told that a session of its own is still pending. It receives that session's baton ID and the status of its last write before the crash. The process can then either:

- **renew**, replaying the baton's write sequence from the start, or
- **resume**, continuing from where it stopped.

## 5.5 Claim protocol (append → settle → choose)

The same protocol is used to claim a baton, to claim `PRIMARY_COORD` and to assign priority values:

1. **Append.** The requester writes a request record into **its own coordinator slot** (5.7). The record holds a microsecond timestamp (from the primary's clock), the requester's priority bits and its session identity.
2. **Settle.** The requester waits for a short settle interval.
3. **Choose.** Every coordinator applies the same rule to the records it can see. The earliest timestamp wins. Ties go to the higher priority, and then to the session identity.

**Priority values are unique.** Two live processes never hold the same priority value, so only one process can ever be priority 1. Each new process is weighed against the priority values already in use and receives the next one available. **Operators can rebalance priority values by hand.**

If two processes ever turn out to be truly simultaneous and this rule still causes trouble, it will be revisited then.

## 5.6 Read-your-own-writes and write conflicts

These are defined in Section 11.3. In brief:

- A writer sees its own unreconciled changes. Every other process sees the last reconciled state.
- When two writes collide, the policy is configurable. A rejected writer is notified of the collision, in the same way as a semaphore lock.

## 5.7 Single-writer coordinator slots

- Every coordinator owns one **slot**. No process other than the owner ever writes to it.
- A coordinator writes its requests (5.5) and its heartbeats (5.8) to its own slot.
- Its **delta tracks** follow the same rule: each process owns its own tracks and is their only writer (Section 1.3). At reconciliation, the baton holder merges every process's tracks in commit order, using commit timestamps from the primary's clock.
- The primary reads every slot and puts the requests in order. Subordinates read the slots when they need to detect that the primary has failed, or to elect a new primary.

No two processes ever write to the same bytes, so the correctness of the data never depends on atomic appends, on OS file locks or on any other filesystem convention. A lock only answers whether a slot's owner is alive (5.8).

## 5.8 Liveness and heartbeats

Status: **confirmed by the project owner on 2026-11-05.** It replaces liveness by heartbeat timeout, after the demo's parallel writers showed a live process judged dead: a long engine call held the lock its heartbeat needed, another process merged and discarded its journal, and its later commits went to a file nobody would read. Being slow is not being dead.

- **Alive means holding the slot lock.** For its whole session, a coordinator holds an exclusive OS lock on its own slot file (5.7). A slot is alive exactly while its lock is held. Another process tests a slot with an attempt to take its lock that does not wait, and releases the lock at once when it gets it, unless it is taking the slot over (5.9.5). A death is seen as soon as the OS releases the lock, and a live process is never judged dead, however long it stalls.
- **The lock hook.** The environment provides the lock through an optional hook (`cdb_env`). The lock belongs to the open file, not to the process: the OS releases it when the process ends, however it ends, and closing other handles to the same file does not release it. That is how `flock` behaves on POSIX systems and `LockFileEx` on Windows; POSIX record locks (`fcntl`) do not qualify, because closing any handle to a file releases all of a process's locks on it. Locks are reliable only on a local filesystem, which the single-host scope already requires (hard constraint 1); network filesystems are not supported.
- **Mixed processes.** A coordinator records in its slot's flags whether it uses lock liveness (14.5.2). Others judge a slot with the flag by its lock alone, and a slot without it by the heartbeat rules of the fallback below.
- **Fallback without the hook.** In an environment without the lock hook, liveness comes from heartbeats as before: a coordinator whose heartbeat is older than the dead timeout is dead (5.9.6), and the lease margin applies (5.9.8). Microcontrollers, where usually only one process runs, keep their heartbeats **in memory** and never write them to flash, which avoids flash wear.
- **Heartbeats and status.** Every coordinator, with the lock or without it, keeps writing heartbeats to **its own slot** (5.7). A heartbeat carries the primary's clock for commit stamps (14.5.3), crash notices, and the coordinator's **status**: the operation it is busy with, when it started, and its estimate of when it will finish (for the baton holder, its progress *x / x-total*, 5.9.8). The status is written when it changes, and at least once per heartbeat interval while the coordinator is busy. The **heartbeat interval can be configured**; each device class in the table in Section 2.8 has a default.
- **The responder.** Where the environment has thread hooks, a background thread of the coordinator writes its heartbeats and status. It never takes the database's lock, only a small lock of the coordinator's own, so no engine call, however long, can delay it. Without thread hooks, heartbeats are cooperative: library calls and `cdb_coord_tick()` write them (5.9.6).
- **Stalls are waited out, not timed out.** A process that waits on another live coordinator (for the baton, or for a transaction that coordinator has prepared, 5.9.4) reads that coordinator's status. When the estimate shows the wait outlasting the call's cap, or the wait reaches the cap, the call returns `CDB_E_BUSY` instead of waiting further. The coordinator waited on is never treated as dead. Closing a session and asking for reconciliation don't wait for the baton at all; 5.9.1 and 5.9.8 say how.

## 5.9 How processes share one archive (M19)

Status: **confirmed by the project owner on 2026-10-30.** The four choices marked *(confirmed)* were asked as questions; the rest, which carries them out, was approved as a whole. Values marked *provisional* move to the device-class table (Section 2.8) once they have been measured.

### 5.9.1 Applying and merging

- **Private pages.** Each process applies its own committed transactions to private copy-on-write pages over the archive. That is how a writer sees its own unreconciled changes (5.6). No other process ever sees those pages.
- **Only the baton holder writes the archive (confirmed).** Free space, the append point and the header are shared, so a process that does not hold the baton (5.9.8) writes its private pages to its **overlay file**, `pages.<slot>.cdb` in the working directory, and never commits. The overlay is emptied at every rebase and removed at close. A process that holds the baton writes its private pages into the archive, as a single process always has.
- **Merging is logical (confirmed).** At reconciliation, the baton holder replays the committed transactions of **every** slot's journal onto the archive, in merge order (5.9.2), through the same apply hook as recovery. Pages are never merged.
- **Rebase.** When a process sees that the archive has moved (its committed sequence changed), it drops its private pages and re-applies its own committed transactions that are not yet reconciled (5.9.2) on top of the new archive. It checks at transaction begin, at the start of an operation scope and at every heartbeat.
- **Journals belong to slots, not sessions.** The baton holder also merges the journals of dead coordinators. A process that later takes over a dead slot adopts its journal.

### 5.9.2 Merge order and per-slot marks (confirmed)

- **Commit stamp.** Every commit record carries a stamp from the primary's clock (14.5.3). The merge order is (stamp, slot, transaction counter), which is unique because the counter is.
- **Commit numbers are given at reconciliation.** The baton holder numbers the transactions it merges contiguously, in merge order. The header watermark (14.1.8) remains "reconciled through commit number X". A writer leaves the commit-number field of its commit record at 0.
- **Per-slot marks.** The archive keeps a **reconciliation-marks index**: one record per coordinator slot, saying that slot's journal is reconciled through transaction counter N. It is committed together with the pages it describes, so after a crash a transaction is replayed only if its counter is above its slot's mark.
- **Before reconciliation, a revision has no commit number.** Model data that records the commit number (for example document revisions, 14.9) holds 0 on private pages. The real number is written when the transaction is merged into the archive.
- **Counters survive slot reuse.** A process that takes a slot continues its transaction counter above both the slot's high-water mark (14.5.2) and the slot's mark in the archive.

### 5.9.3 Entry-ID blocks (confirmed)

- A store's entry-ID counter in the catalog is the floor below which no process may hand out IDs.
- **Claiming a block.** A process that inserts into a store claims a block of **1024** consecutive IDs for that store. It lists the block in its coordinator slot's held-blocks table (14.5.2) and makes the claim through append → settle → choose (5.5).
- **Which claim wins.** The claim fails if any other slot lists an overlapping block that it already holds, or that it claimed with an earlier request. The process then claims the next free block.
- **No wait when alone.** When no other coordinator is alive, the block is listed without a settle wait.
- **Reconciliation raises the floor.** Reconciliation raises each store's counter in the catalog past the end of every block that any slot lists for that store. Once the counter is past a block's end, the holder may drop the block from its table and keep using the rest of it from memory. Unused IDs are never reused.

### 5.9.4 Collisions at commit (confirmed)

This implements the default policy of 11.3, where the later write is rejected. Configurable policies and the identity carried in a collision notice come with the query-layer milestones.

- **Write set.** A transaction's write set is the (store, entry) of each of its data records. It also includes a hash of every unique-key value it writes (tables, 9.9), each journaled as a *write-set key* record (14.4.2, kind 4).
- **Prepare, settle, scan.** When another coordinator is alive, the committer:
  1. appends a *prepare* record (kind 5) holding its stamp;
  2. waits the settle interval;
  3. reads the other slots' journals from where it last stopped;
  4. checks its write set against every transaction there that is committed, or prepared and not aborted, and that is not reconciled yet.
- **Who loses.** An overlap with a committed transaction, or with one that was prepared with an earlier (stamp, slot), makes the committer the later writer. It appends an abort record and gets `CDB_E_COLLISION`. An overlap with a transaction prepared with a *later* (stamp, slot) that has neither committed nor aborted makes the committer wait until it does, or until its coordinator dies (a transaction with no commit record never counts). A commit by that transaction then makes the committer lose. Waits only run from earlier stamps to later ones, so they can't form a cycle, and the outcome is safe even when a process stalls between taking its stamp and writing its prepare record. Otherwise it writes its commit record, which carries the prepare stamp.
- **Changes merged during the transaction.** If another process reconciled changes from other slots into the archive while the transaction was open, the transaction is rejected the same way. Its view was older than the archive, and the reconciled write sets are no longer in any journal.
- **Joining.** A coordinator that has just joined waits one heartbeat interval plus the settle interval before its first commit. Every coordinator refreshes its list of live coordinators at each heartbeat. A commit that starts with a list older than half a heartbeat interval refreshes it first.
- **Only coordinator.** With no other live coordinator, there is no prepare record, no settle wait and no scan.

### 5.9.5 Joining and slot ownership

- **Lobby.** A new coordinator first writes a join request to a lobby file, `coord/join.<session identity>`. Session identities are random, so no two processes ever write the same lobby file. The request names the slot it wants: the lowest free one, or the lowest one whose coordinator is dead. The join then follows append → settle → choose, and a loser tries the next slot. The winner writes its slot file and removes its lobby file.
- **Single writer.** No process ever writes another process's slot file, even a dead one's. A dead slot keeps its contents, including its held blocks and its journal, until a new owner takes it over.
- **Journal in use.** A slot records whether its journal is in use (14.5.2, state flags). A slot that says its journal is in use, but whose journal is missing, means possible loss, as in 1.12.5.

### 5.9.6 Liveness and concurrency (spec 5.9.6, M20)

- **When heartbeats are written.** The responder writes them (5.8). Without thread hooks, every library call writes a heartbeat if one is due, at most once per heartbeat interval, and `cdb_coord_tick()` lets an idle application keep its coordinator's heartbeat current.
- **Dead coordinators.** Liveness is 5.8: a slot is dead when its lock can be taken. In the fallback without the lock hook, a coordinator whose heartbeat is older than the dead timeout is dead. *Provisional:* interval 1 s, fallback dead timeout 3 intervals, settle interval 2 ms.
- **Coming back (fallback only).** With the lock, an idle process never loses its slot. Without it, a process that has been idle past the dead timeout finds this out on its next call. It rejoins: if its slot is still free it takes it back, otherwise it takes another slot. It does so as a subordinate, then rebases.
- **Background threads and synchronization (M20).**
  - **Environment hooks:** `cdb_env` provides opaque handles and function pointers for recursive mutexes (`cdb_mutex`), monotonic condition variables (`cdb_cond`), and threads (`cdb_thread`). POSIX environments implement these with `pthreads` (`PTHREAD_MUTEX_RECURSIVE`, `CLOCK_MONOTONIC`).
  - **The responder thread:** When concurrency hooks are present, `cdb_db_open` starts the coordinator's responder (5.8), which writes heartbeats and status at half the heartbeat interval and whenever the status changes. It takes only the coordinator's own lock, never the database's. Public database operations acquire the database's re-entrant mutex.
  - **Worker thread pool:** A caller-configured pool of background worker threads (`cdb_worker_pool`, `cdb_db_config.worker_threads`) backed by a fixed ring-buffer queue offloads heavy maintenance tasks (such as `cdb_vec_refresh_async`) without heap allocation in the core engine.
  - **Single-threaded fallback:** When thread hooks in `cdb_env` are `NULL` (single-threaded environments or microcontrollers), or when `worker_threads` is 0, heartbeats remain cooperative, and submitted worker tasks execute immediately and synchronously on the caller thread with identical semantics.

### 5.9.7 Readers and space reuse

- Each coordinator publishes in its slot the archive's committed sequence that its readers are using (14.5.2, offset 144).
- A page freed by the commit with sequence *f* is reused only when *f* is below the committed sequence (3.4) **and** below the sequence published by every live coordinator. That way, no process reading an older archive state has a page overwritten under it.
- **Publishing is checked.** A process reads the header, publishes its committed sequence *P*, then reads the header again. If the sequence is no longer *P*, it starts over. The baton holder reads every slot's published sequence before each archive commit it prepares, so a page that state *P* uses, freed at *P* + 1 or later, is never reused while *P* is published.

### 5.9.8 The baton (first version)

- **One heat, round-robin.** Requests are served in merge order of their request stamps. The congestion-detecting queue and multiple heats come later.
- **What needs the baton.** Reconciliation, compaction and catalog changes. Catalog changes and compaction already reconcile first (14.4.6, rule 4), so each of them runs as one baton session.
- **Fencing.** Each baton's ID (CTS) is one above the previous baton's, which is published in the holder's slot (offset 112). Before committing the archive, the holder checks that its CTS is still the highest one published by any slot.
- **Request and grant.** A requester writes request kind 1, its request stamp and *x-total* into its slot. When no live slot holds a baton, the request that is first by the rule of 5.5 takes it: it publishes the next CTS and the time it was issued (offset 128), waits the settle interval and checks again that no other slot published the same or a higher CTS. A slot holds the baton while its issue time is not 0. Releasing sets the issue time to 0 and keeps the CTS.
- **Standing baton (confirmed).** A process that is the only live coordinator takes the baton without a settle wait and keeps it between reconciliations. When another process requests the baton, the holder reconciles what it has applied at its next library call or `cdb_db_tick`, then releases.
- **Service window and timeout.** The window is the dead timeout plus a share sized from *x-total*. The holder publishes its progress *x* with every heartbeat. A holder that is dead, or that has gone a whole window without progress, has lost the baton: the next requester takes a CTS above it.
- **Lease margin (confirmed).** Before each archive page write and before its commit, the holder checks on its own monotonic clock how long it has gone without a heartbeat. Past half the dead timeout, it gives the session up, releases and rebases. Others wait the whole timeout, so a stalled holder that wakes up stops writing before anyone else starts. What remains is a stall that falls between that check and the write itself.
- **Waiting (confirmed).** A call that needs the baton while another live process holds it waits: it sleeps through the environment, heartbeats, and checks again, until it is granted or the holder times out. After a configurable cap (default: the service window plus the dead timeout), it returns `CDB_E_BUSY`.
- **Renew and resume are the same.** A reconciliation is one atomic archive commit, so a session that crashes leaves nothing to resume: its process starts over from the archive state. Compaction commits at each step and simply continues from the last one.

## Open questions (do not specify until answered)

None at present. How identity keys are issued is covered in [Section 12.5](12-security.md).
