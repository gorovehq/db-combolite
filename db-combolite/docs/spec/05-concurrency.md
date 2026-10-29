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

No two processes ever write to the same bytes. As a result, coordination doesn't depend on atomic appends, on OS file locks or on any other filesystem convention.

## 5.8 Heartbeats

- Each coordinator writes its heartbeat and its crash notices to **its own slot** (5.7).
- The **heartbeat interval can be configured**. Each device class in the table in Section 2.8 has a default interval.
- **On microcontrollers**, where usually only one process runs, heartbeats stay **in memory** and are never written to flash. This avoids flash wear.

## Open questions (do not specify until answered)

None at present. How identity keys are issued is covered in [Section 12.5](12-security.md).
