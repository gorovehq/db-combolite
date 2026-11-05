# Section 19: Replication between copies

Status: **Draft. The decisions below are confirmed; the open questions are not.**

Depends on: 4 (catalog), 5.9 (processes and reconciliation), 6 (keys and links), 8 (documents), 9 (relational), 10 (time series), 12 (security), 14.4 (delta records).

## Purpose

A database is a single file on one host, and processes on that host share it through the file (Section 5). Replication is something else: **separate copies** of a database, usually on different hosts, that exchange their changes so that they converge. Governance records and agent memory can then follow a developer between machines, or be shared by a team.

Replication adds no server, daemon or broker, and no copy ever opens another copy's file. The library produces and consumes **slices** of changes; how a slice travels (a file, a socket, a sync service) is the application's business. Scope stays as in the hard constraints: one database is never accessed from more than one host.

This section was written after a first implementation (the CRDT helpers and `cdb_db_crdt_export_slice` / `cdb_db_crdt_import_slice`), which replays changes in arrival order and keeps its state in memory. That implementation is to be reworked to match. Its state was never persisted, so no file holds anything to migrate.

## Confirmed decisions

### 19.1 Copies and the replication group

- **A copy** is a database file that takes part in replication. Copies of one database form its **replication group**.
- **Creating a replica.** A new copy is made from an existing one by a dedicated call, never by copying the file alone. The call gives the new copy its own **copy number** and its own database ID; the group shares a **group ID** (14.21.1).
- **Copying the file is not a replica.** A plain copy of the file that goes on being written beside the original is a fork, not a member of the group: it has the original's copy number, so the IDs it allocates (19.2) can collide with the original's. Its encryption is safe either way, because every session writes under its own salt (12.1).
- **Copy numbers** are 16 bits. The first copy holds all of them. A copy creating a replica hands it **half of its remaining copy numbers**, so numbers are never handed out twice and no copy needs to ask another.
- **Group size.** A per-database limit on the number of copies in the group, set when the database is created (default 32). A replica beyond it is refused. The version vector (19.4) is bounded by it, so it fits in fixed memory and in one page.
- **Retiring a copy** removes it from the group, so that tombstones it has not seen can be purged (19.6).
- **64-bit files only.** IDs carry the copy number (19.2), which needs 64-bit IDs. Files with 32-bit addresses and compact files (Section 13.4) can't replicate.

### 19.2 Unique IDs across copies

Every ID a replicated object or entry takes is unique across the group: **its top 16 bits are the copy number** of the copy that allocated it, and a counter local to that copy fills the rest. This covers stores, entries, link types, procedures, roles and principals. IDs keep their width, and every existing reference format stays as it is. An ID allocated by a database that never replicates has 0 in its top bits.

### 19.3 What replicates

- **Stores opt in.** A store is marked replicated when it is created, or later. Its data replicates, and so do the catalog objects it needs: its record, schema history, field dictionary, and the link types whose ends it holds.
- **Other stores stay local.** Only replicated stores pay for per-field clocks and tombstones (19.5, 19.6).
- **Relaying.** A copy exports every change it holds for its replicated stores, its own and those it received, filtered by what the receiver has already seen (19.4). Any connected pattern of syncs then converges: A with B and B with C is enough for A and C.

### 19.4 Clocks and version vectors

- **Hybrid logical clock.** Every replicated change carries a hybrid-logical-clock time: physical time, a logical counter and the copy number. Times are totally ordered. A copy's clock moves past every time it receives, up to the drift bound (below), and **every write is stamped later than what it overwrites**: a write to a path takes a time after both the copy's clock and the path's current clock time (19.5), so a local change always supersedes the value it replaces, even one stamped by a fast clock.
- **Version vector.** Each copy keeps, per origin copy, the latest change it has applied from that origin. A slice for a receiver holds the changes above the receiver's vector.
- **Persistence.** The copy number, the clock and the vector are stored in the database, and the vector advances only when the transaction that applied the changes commits: each such transaction journals its clock and vector advances with its commit, and reconciliation folds them into the copy's group record (14.21.1). A reopened copy remembers what it has seen.
- **Clock readings.** Copies are not continuously connected, so they exchange no heartbeats. Instead, every sync handshake carries the sender's wall-clock reading and the clock offsets it has measured for other copies. The receiver records, for each copy, its estimated offset from the local clock with an error bound (the transit time can't be known), in a built-in **replication-clock time series**, and relays the estimates in its own handshakes. Every copy thus knows the estimated drift of the whole group, and governance can show which copy was off, by how much, and when.
- **Clocks are never adjusted** from these readings: the library sets no system clock, and changing clock times would change who won past conflicts.
- **Drift bound.** A configured bound limits how far ahead of the local clock a received change may be stamped. A change beyond it is still applied, since every copy must treat it alike to converge, but it is recorded as an audit event naming the copy and its offset, and the local hybrid clock moves at most to the local time plus the bound because of it. A fast copy still wins its own conflicts, visibly.

### 19.5 Merging concurrent changes

All copies end up identical, whatever order the syncs happen in. Each data model merges by its own rule, with clock times deciding between writes:

- **Documents and table rows** merge **path by path**: every leaf path of a document (a sequence of field IDs into nested objects), and every column of a row, keeps the write with the later clock time. Concurrent edits to different keys of one nested object both survive. Arrays are leaves: an array is replaced whole by the later write.
  - Writing an object at a path writes each leaf under it at the write's clock time, and removes, at that time, the leaves under the path that the new object lacks.
  - Removing a field or path leaves a tombstone for it at the remove's clock time, so an older write arriving later can't bring it back.
- **Clock index.** Each replicated store keeps the clock times in an ordered index of its own, keyed by entry and path, holding each path's latest clock time and whether it is a tombstone. Revisions are unchanged and local stores pay nothing. The index holds only the latest clocks; revision history keeps the values, not their past clocks.
- **Deletes** compare with writes like any write: a delete newer than every write to the entry removes it; a write newer than the delete revives the entry, with only the fields written after the delete.
- **Unique keys and primary keys.** When two copies insert rows with the same unique key, the row with the earlier clock time keeps the key. The other row is kept, with its key set aside, in a **per-store conflict list**, and a conflict event goes to the audit series (12.7). Nothing is lost silently; the application resolves listed conflicts.
- **Links** merge as an **add-wins observed-remove set**: a link added on one copy and removed concurrently on another stays. Weight and time take the later write.
- **Vector entries** are replaced whole by the later write.
- **Time-series points** with the same series and timestamp merge by the collision rule of Section 10 (field by field, later clock time wins), and the replaced values go to the audit series.
- **Schemas** merge too: added columns and fields are united, drops are tombstones, and two incompatible changes to one column keep the change with the earlier clock time and record a conflict event. Data keeps reading through the merged schema (Section 9).

### 19.6 Tombstones

A replicated delete leaves a tombstone, so that an old change arriving late can't undo it. A copy purges a tombstone once **every member copy's version vector**, as last reported to it, covers the delete. A copy that stays offline keeps tombstones alive everywhere until it syncs or is retired (19.1).

### 19.7 Slices

- **Logical changes.** A slice carries changes in merge form, independent of the journal's layout: set or remove a path, delete an entry, add or remove a link, a time-series point, replace a vector, and the catalog objects and schema versions replicated stores need (14.21.5). Export derives them from committed transactions.
- **Signed by the sender.** Every message is signed with the sending copy's principal key (12.5). The private key stays in the application's keystore, found through a key reference in the copy's group record (12.3); it is never in the file. Messages carry a per-sender sequence number, so a replayed message changes nothing (14.21.4). A receiver refuses messages from unknown or retired principals, and messages whose signature fails, so slices are tamper-evident and every change is attributable. Keeping public changes confidential in transit is the transport's job (TLS, for example).

### 19.8 Encryption

- **Nonces across copies.** Every session that writes a copy draws its own salt, and subkeys are derived with it (12.1). Two copies sharing an enclave key therefore encrypt under different subkeys, and their nonces may repeat harmlessly. The nonce layout (14.2.5) does not change, and replicas need no subkeys of their own.
- **Sealed for the receiver.** A domain's changes are exported only while the domain is open on the sending copy, sealed in the slice under the domain's key, never in plaintext (12.8). The receiving copy needs the same domain key to apply them; until it has it, it holds them in the domain's held log (14.4.10).

## Open questions (do not specify until answered)

None at present. The byte layouts are in 14.21; they are proposals until reviewed with the implementation plan.
