# Section 3: Crash safety and recovery

Status: **Draft. The decisions below are confirmed; the open questions are not.**
This section relies on the archive model and breadcrumbs (Section 1.3), append-only merges (Section 1.7), the two header copies (Section 1.9), pages (Section 2) and the baton with its CTS fencing (Section 5).

## Purpose

This section defines the order in which writes happen, so that a crash or a power loss at any moment leaves each change either entirely in its old state or entirely in its new state, never partly applied. It also defines what the database does when it detects damage that a crash did not cause.

In this section, a **barrier** means a flush to durable storage. A step after a barrier does not begin until everything written before the barrier has been made durable.

## Confirmed decisions

### 3.1 The operator sets the durability level

The operator decides when a write is reported to the caller as successful.

| Level | Write acknowledged when... | What a crash can lose |
|---|---|---|
| **full** | its transaction's commit record has been written and a barrier has completed | nothing that was acknowledged |
| **normal** | its delta record has been written; barriers happen at checkpoints | the most recent writes since the last checkpoint, but nothing is ever corrupted |
| **off** | its delta record has been handed to the OS or the device | anything not yet flushed; meant for scratch data only |

Whatever the level, a crash never leaves the database corrupted.

**The default level is normal.**

**The user chooses how strict to be.** The level can be set for the whole database, for a single store or for a single transaction. The most specific setting wins: a transaction setting beats a store setting, and a store setting beats the database setting.

### 3.2 The commit sequence for reconciliation

1. Append the reconciled data pages to the archive. **Barrier.**
2. Write the new anchor-map pages. **Barrier.**
3. If the catalog has changed, write the next **catalog root slot** (Section 4.7), carrying the new commit sequence. **Barrier.**
4. Write the working header copy. It carries the new synced-up-to watermarks, a higher committed sequence and a valid checksum. **Barrier.**

**Step 4 is the commit.** When the database is opened, it chooses a header copy according to Section 1.9. It then uses the newest catalog root slot whose sequence is **not higher than** the committed header sequence. Until step 4 completes, the previous committed state is still in force; once step 4 completes, the new state is in force (review R6).

### 3.3 Torn writes in delta tracks

Every delta record carries its own checksum, or its own authentication tag if it is encrypted. During recovery, each track is read up to the first record that fails verification, and everything from that record onward is discarded. A record that fails verification was never fully written, so it was never acknowledged under the **full** level.

**Only committed transactions are replayed (review R2).** A transaction is replayed only if its commit record survives. A transaction without a commit record is discarded in every track it touched.

### 3.4 Compaction never overwrites live data

1. Copy the live data to its new location. **Barrier.**
2. Update the anchors so they point to the new location. **Barrier.**
3. Commit by writing the header, as in step 4 of 3.2. **Barrier.**
4. Only now release the old space.

**Space reuse and the compaction procedure (implementation, M6).**

- **When space can be reused.** A freed page is reused only once the commit that freed it is older than the latest commit: its freed sequence must be below the committed sequence. Until the next commit, the other header slot may still hold the previous state (Section 1.9), and that state can reference pages the latest commit freed.
- **Allocation order.** New writes always take the lowest reusable page. The file grows only when no reusable page exists.
- **Procedure.** Compaction runs in this order:
  1. **Fence commit.** If any freed space is not reusable yet, a commit is made first, so that it becomes reusable.
  2. **Target.** The target end of the file is the current end minus all reusable pages.
  3. **Move.** Live data pages, and anchor-map nodes, that sit at or beyond the target are rewritten into reusable holes below it. Pages already below the target stay where they are, so compacting a file that is already compact moves nothing.
  4. **Two commits.** The second makes the vacated pages reusable.
  5. **Release the tail** (3.7).

### 3.5 Damage that was not caused by a crash

Bit rot, failing flash cells and similar damage are detected when a page is read, because the page's checksum or authentication tag no longer verifies. The response escalates in three tiers:

1. **Self-repair from redundant copies.** The **anchor map, catalog, dedup index and free-space tracking** each keep a second copy, in the same way as the header, so they can repair themselves. For the catalog, every **body** page is written twice (mirrored appends), and the A/B root slots provide the root's redundancy (review R5).
2. **Scrubbing.** Maintenance windows and the analyze phase verify pages ahead of time, so damage is found before a query needs that data. Each window scrubs a **fixed budget** of pages or time, working through the file in rotation:
   - A **benchmarking routine** measures the maximum sustained scrub rate on the host, and the default budget is **85% of that rate**.
   - A **slider** lets the operator raise the budget above the default, and the operator accepts the added risk of doing so.
3. **Report and stop.** This is the last resort. It is used when damage can't be repaired from a redundant copy. The database reports the damage and stops, and the operator restores from a backup.

### 3.6 A crash-point test matrix is part of the spec

Each write sequence in this spec lists its crash points, together with the exact state that recovery must produce for each one. A test harness has to inject a crash at every listed point and check the state after recovery. An implementation doesn't conform unless it passes the whole matrix.

The initial matrix below covers only the sequences confirmed so far. It grows as later sections add write sequences.

| ID | Sequence | Crash point | Required state after recovery |
|---|---|---|---|
| C-D1 | Delta append | during the record write | track ends at the last record that verifies; the torn tail is discarded |
| C-D2 | Delta append | after the record is written, before its barrier | under **full**: not acknowledged, so it may be lost. Under **normal** and **off**: it may be lost |
| C-R1 | Reconciliation (3.2) | during step 1 or 2 | the previous committed state; appended pages are unreferenced; deltas above the watermark are replayed |
| C-R2 | Reconciliation (3.2) | during step 4 (torn header write) | the torn copy fails its checksum; the previous committed copy is used |
| C-R3 | Reconciliation (3.2) | after step 4's barrier | the new committed state; the reconciled deltas are at or below the new watermark and are not replayed |
| C-K1 | Compaction (3.4) | before step 3 completes | the previous state; the copied data is unreferenced |
| C-K2 | Compaction (3.4) | after step 3, before step 4 | the new state; the old space is still held and is released on the next maintenance pass |
| C-R4 | Reconciliation (3.2) | after step 3 (root slot written), before step 4 | the previous committed state; the new root slot's sequence is above the committed sequence, so it is ignored |
| C-T1 | Transaction | deltas written to some tracks, commit record not written | the transaction is absent from every track |
| C-T2 | Transaction | after the commit record's barrier | the transaction is present in every track it touched |
| C-N1 | Sequence reservation (12.1) | after pages are encrypted with sequences *n…m*, before commit | after recovery, sequence numbering resumes at or above the persisted high-water mark; no nonce is ever reused |

**Still to be added during the byte-layout pass (review G10):** key-rotation re-encryption; baton resume and renew (Section 5.4); unseal and reseal; handling a missing companion (Section 1.12.5); `danger_mode` working on a copy.

### 3.7 Reclaiming unreferenced pages

A crash before commit can leave pages that were written but never committed. These **unreferenced pages** (cases C-R1 and C-K1) are reclaimed in one of two ways:

- **by default**, during the next maintenance window;
- **on demand**, through a **reclaim-now** operation that the operator can run at any time.

**Releasing the free tail.** Reusable free extents that reach the end of the file are removed from the free-space list. The append point is lowered, and that change is committed. The storage is then truncated and synced. The same operation trims pages that a crash left past the append point. Neither header slot's state references anything past the append point. On media that cannot shrink, such as raw flash, the truncation step is skipped.

## Open questions (do not specify until answered)

None at present.
