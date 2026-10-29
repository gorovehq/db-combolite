# Section 1: File layout and header

Status: **Draft. The decisions below are confirmed; the open questions are not.**
Byte offsets, field sizes and magic values are not defined yet. They are deferred to the byte-layout pass.

## Purpose

The header is what a program reads first when it opens a db-combolite database. It identifies the file as db-combolite and says which format version and features the file uses. It also says how to locate everything else, and whether this reader may read or write the file.

## Confirmed decisions

### 1.1 Byte order: declared per file

The header has a byte-order flag, and the program that creates the file chooses the value. Every reader must handle both orders. The flag sits at a fixed position, encoded so that it can be read the same way in either byte order.

### 1.2 Compatibility: feature flags in three classes

There is no single format version number that decides compatibility. Instead the header carries feature flags in three classes:

| Class | A reader that doesn't recognize the flag must... |
|---|---|
| **Compatible** | open the file normally and ignore the feature |
| **Read-only compatible** | open the file read-only |
| **Incompatible** | refuse to open the file |

Every optional capability below is announced through these flags, including companion files, encryption, deduplication, compression and reserved regions.

### 1.3 One archive at rest, a main file plus companions in operation

The database behaves like an archive. **At rest it is always exactly one file**, for example `<project>.cdbarch`.

The archive stays in place while the database is open. It is never copied out in bulk.

When the database is opened or analyzed, it **unseals** the companion files and creates them next to the archive. Which companions it creates depends on the feature flags and their enabled state. Every companion, `.cdb` included, is a **just-in-time delta track**:

- Working data is pulled in only when it is needed.
- A companion records only changes (deltas) against the archive. It never holds a full copy.

How the companions are laid out on disk, and what they are called, is described in 1.12.1.

**Delta tracks belong to one process (review R3).** Each coordinator (Section 5) writes its **own** delta tracks, for example `rel.<slot>.wal`, and no other process writes to them. At reconciliation the baton holder merges every process's tracks in commit order.

**Transactions (review R2).** Every delta carries a **transaction ID**. A transaction counts only once its **commit record** has been written. When deltas are replayed, a transaction is applied only if its commit record exists; incomplete transactions are discarded.

**Reconciliation.** During periodic sync windows, at close and during maintenance, the deltas are folded into the archive. Once every track has been reconciled, the companions are removed and the database is a single file again.

**Breadcrumbs.** Every companion type follows the same scheme:

- Each delta track numbers its deltas with a **monotonically increasing sequence number**.
- Every committed transaction receives a **global commit number**, assigned through the primary coordinator.
- Each companion slot in the header stores a **watermark**: the global commit number through which that companion type has been reconciled (Section 14.1.8).
- After a crash, the next open replays only committed transactions whose commit number is above the watermark, in commit-number order.

**Commit.** Reconciliation first writes its new pages by appending them (see 1.7). It then commits by promoting the working header copy (see 1.9), which carries the new watermarks. Section 3.2 gives the full order, including the catalog root slot. A crash before the commit leaves the previous committed header valid.

**Encryption of companions.** A delta that belongs to an encrypted enclave is written to its companion file encrypted under that enclave's key. Each delta's nonce is *(track ID, delta sequence)*, and delta sequence numbers are reserved durably, so a nonce is never reused (Section 12.1).

**Reclaiming space (hybrid).** During a session, space freed by superseded data is not reused. It stays held until the lock state that covers it has been reconciled. After that, the freed space is compacted, so the stored data stays contiguous and clean.

**Encryption keys are never companions.** They are never written into the archive, and they are never spawned next to it (see 1.4).

The full set of companion types is not fixed yet, so the header holds **reservable companion slots**. Each slot records whether a companion type is *provisioned* and whether it is *in use*. New companion types can be added later without changing the format.

### 1.4 Encryption at rest: a master key over per-enclave keys

There are two levels of keys:

- **The master key** unlocks the database for an application. It is never stored in the archive or in a companion file. It is fetched from an external secrets store that can be addressed and retrieved by common means, such as HashiCorp Vault, NSS, a hardware root of trust, GitHub secrets or Docker secrets.
- **Enclave keys.** Each encrypted enclave has its own key. The master key controls these keys: it creates them and unlocks them for use. This supports **multi-tenancy**, because one tenant's enclave can be unlocked without exposing the others. It also allows **each enclave to be re-keyed, encrypted or decrypted on its own**.

**Enclave keys live in a keystore, never in the archive.** Each enclave's key is a separate entry in the keystore and is fetched only when that enclave is needed. The archive and the header record only *references* to keys, never key material.

### 1.5 Decrypt cache: optional, encrypted, behind a flag

The decrypt cache exists only when its feature flag is set. A type flag selects where it lives: **on disk** or on a **RAM disk**. Either way, its contents are encrypted under a **key generated for the current session only**, which is never persisted. Plaintext is never written to the cache.

### 1.6 Deduplication stays inside one encryption boundary

Deduplication never crosses an encryption boundary. Each public store and each encrypted enclave keeps its own deduplication domain. Identical bytes in two different domains are stored twice. This prevents one domain from learning what another domain contains.

### 1.7 Thin provisioning through companion files

A thin-provisioned region works as follows:

1. The region is given an entry in an **anchor list**. The anchor is a reference to where the region's data will live.
2. A companion file is created for the region. It starts with **zero bytes of data**, plus its anchor reference.
3. Live data is written into the companion while the database is in operation.
4. At merge time the data is **appended at the end of the file**, at the next available anchor address. The region's anchor is then pointed at that location.

Existing data never moves, and no other anchor changes. Merging new data into the archive is append-only.

Anchor addresses and offsets are **aligned to pages**.

### 1.8 More than one process

Moved to [Section 5: Concurrency and coordination](05-concurrency.md).

### 1.9 Two copies of the header

The header is stored twice, and each copy carries its own checksum and sequence counter. The two copies play different roles:

- **Committed copy.** This is the record of the last reconciled state.
- **Working copy.** This is a *digital twin* that describes the working deltas and how far reconciliation has progressed.

Each copy records its **role** (committed or working), a **committed sequence** (review R4) and a **write counter**.

- **Writes rotate.** Every header write, whether a checkpoint or a commit, goes to the slot that was **not** written most recently. This is the ring of 1.11.3 with N = 2. A torn write can therefore damage only the older slot, and the most recent durable write always survives.
- **Every write carries the full committed record.**
  - A **checkpoint** rewrites the committed record unchanged, except for the reservation high-water marks. It keeps the committed sequence and has the role *working*.
  - A **commit** writes the new committed record with a higher committed sequence and the role *committed*.
- **Choosing the record on open.** The valid copy with the highest committed sequence wins. If both copies have the same committed sequence, the copy whose role is *committed* wins. If the roles are also equal, the higher write counter wins.

A torn write or a bad sector can damage only one copy.

**Sequence high-water marks (review R1).** The header also stores the high-water marks of the reserved ranges of sequence numbers (Section 12.1). On open, each mark is the **maximum across all valid copies**, because a checkpoint written after the last commit may have raised it. No header write ever lowers a mark.

*Why writes rotate:* if checkpoints and commits both rewrote one fixed working slot, a crash that tore a commit would also destroy the reservation saved by the checkpoint before it, and sequence numbers (nonces) could then be issued twice. The crash sweep in `tests/test_header.c` checks for exactly this.

**Slot rings on raw flash.** On raw flash, a ring of N slots replaces the A/B pair (1.11.3).

**Limiting wear.** The working copy is rewritten only at **checkpoints**, not on every delta. The detail of each individual delta stays in the delta tracks.

### 1.10 What the header records

Each header epoch has a fixed length (Section 4.7), so the header holds only **fixed-size** items (review G1):

- magic number, epoch, byte order, page geometry, address width and compact-header flag;
- feature flags in the three classes (1.2), including deduplication, compression, decrypt cache and tamper-evident mode;
- algorithm flags: checksum (Section 2.9) and AEAD (Section 12.1);
- the database's unique ID;
- role and committed sequence (1.9), and the sequence high-water marks (Section 12.1);
- the **companion slots** (a fixed number per epoch), each with its type, class, provisioned and in-use state, and synced-up-to watermark;
- **reserved-region anchors** for thin-provisioned features (1.7): a feature flag can be set before the feature is in use, and its region takes no storage until it is first written;
- the location of the anchor-map root.

Information that is per store or per domain is **not** kept in the header. This covers each store's encryption state, the locations of the deduplication indexes (one per encryption domain) and the location of the quick-query index. All of it lives in the catalog (Section 4): in the public catalog for public stores, and in the enclave's own catalog for an enclave (Section 4.4).

### 1.11 Storage allocation is owned by the library

db-combolite does its own allocation inside the library stack and does not depend on host filesystem conventions. Section 2 defines the allocation model.

#### 1.11.1 Storage backends

The library reaches storage only through a small backend interface:

- read a page;
- program (write) a page;
- erase a block;
- sync;
- report geometry: erase-block size, smallest programmable unit and total size.

Two backends ship with the library:

- **Host file.** For desktops, servers and single-board computers. This backend also runs on top of an existing flash filesystem, such as LittleFS or FAT, for operators who prefer one.
- **Raw flash partition.** For example an ESP-IDF data partition. It is used in the same way from ESP-IDF, Arduino and PlatformIO.

**The bytes are identical on every backend.** A partition dumped from a device can be opened on a workstation, and the same post-mortem tools work on it.

#### 1.11.2 Raw-flash layout

On raw flash the partition is divided into **fixed regions**:

- the header slots;
- the location-0 catalog root slots (Section 4.7);
- the archive area;
- one reserved region for each companion type.

Region sizes default from the device-class table (Section 2.8) and can be overridden. Raw flash has no files or directories: each companion is a region.

#### 1.11.3 Slot rings for hot fixed structures on flash

The header copies (1.9) and the catalog root slots (Section 4.7) are rewritten on every commit. On raw flash, each of these uses a **fixed ring of N slots** instead of a strict A/B pair:

- each write goes to the next slot in the ring;
- on open, the valid slot with the highest sequence number wins.

The location stays fixed and known. A/B is simply a ring with N = 2, which is what host files use.

#### 1.11.4 Pages and erase blocks

On raw flash, every page size must divide the erase-block size evenly. Compaction works one erase block at a time, so it never erases a block that still holds live data.

#### 1.11.5 Preferred storage media on microcontrollers

- **An SD card is preferred over the device's internal flash chips** when the device supports one.
  - **Preference list.** The order of preferred media is a configurable list. By default it is: SD card, then internal flash.
  - **One medium per database.** A database lives entirely on a single medium. It is never split between a removable card and internal flash.
  - **Card removal.** Removing the card while the database is open is handled as a crash. Recovery runs on the next open.
  - **Power-loss testing.** The crash-point matrix (Section 3.6) must also be run against SD media, because cheap cards can corrupt data on power loss.
- **PSRAM, where present, is used as operational cache space.**
  - **What goes in PSRAM:** bulk caches, meaning the page cache, the RAM-disk decrypt cache (1.5) and working sets for vector search.
  - **What stays in internal SRAM:** small, hot state, meaning the coordinator's state and its push/pop stack (Section 5.2). PSRAM is slower than internal SRAM.
  - **PSRAM is volatile.** Nothing held only in PSRAM counts as durable.

### 1.12 Companion files

#### 1.12.1 A working directory next to the archive

On host backends, companions live in a working directory beside the archive:

```
<project>.cdbarch          the archive (the only file at rest)
<project>.cdbwork/         exists only while the database is unsealed
    main.<slot>.cdb        per-process working journal
    rel.<slot>.wal         per-process delta tracks (one writer each)
    ts.<slot>.wal
    e-0007.vec.<slot>.wal  an enclave companion, named by opaque ID
    catalogue
    validation.map         validation map (Section 13.5)
    coord/                 single-writer coordinator slots (Section 5.7)
```

- **Detecting state.** If the working directory exists, the database is unsealed. If it exists at open time but no live coordinator is present, the previous session crashed, and recovery runs.
- **Clean-up.** Resealing removes the whole directory, so no stray files are left behind.
- **No leaked names.** Companions that belong to an enclave are named with the enclave's **opaque numeric ID**, never with its name (Section 4.4). Companions for public stores may use short readable tags.

#### 1.12.2 Companion headers

Every companion begins with its own header, which contains:

- a magic number;
- the companion type ID;
- the database's unique ID;
- the sequence number of the archive it was unsealed from;
- a checksum.

A companion whose database ID or archive sequence doesn't match is **set aside and reported, never applied**. This prevents changes from another database, or from a stale session, from being applied.

#### 1.12.3 The companion type registry

Each companion type has a numeric type ID and one of the three compatibility classes (1.2). The companion slots in the archive header record which types are provisioned and which are in use, together with their classes. A reader that doesn't recognize a type acts according to its class:

| Class | What a reader that doesn't recognize the type does |
|---|---|
| compatible | leaves the companion untouched (never deletes it) and continues |
| read-only compatible | opens the database read-only |
| incompatible | refuses to open the database |

#### 1.12.4 `danger_mode`

`danger_mode` is a recovery option. The operator turns it on as a flag in the configuration. It allows the library to open a file that is **incompatible** (because of an unknown incompatible feature, page type or companion type) and attempt to reconcile it so that it can be updated by hand.

This is **very dangerous**: corruption is likely. The operator must explicitly accept every risk involved.

Safeguards:

- **Work on a copy.** By default `danger_mode` operates on a copy of the file, never on the original. The operator can explicitly force it to work on the original.
- **Audit record.** Every `danger_mode` session is recorded in the system audit series (Section 10.7).
- **Permanent marker.** Any file that `danger_mode` has written to carries a permanent "opened in danger_mode" marker in its header, so every later reader knows the file's history.

#### 1.12.5 Missing companions

Suppose the header records a companion as in use but the companion is missing.

- If its breadcrumbs show it was **fully synced** (its watermark equals its last sequence number), nothing has been lost. The companion is recreated empty, and opening continues.
- Otherwise, unsynced deltas have been lost. The database reports the loss and stops (Section 3.5, tier 3). The operator may explicitly accept the loss and open the database anyway, and that decision is recorded in the system audit series (Section 10.7).

## Open questions (do not specify until answered)

None at present. Open questions about concurrency are tracked in [Section 5](05-concurrency.md).
