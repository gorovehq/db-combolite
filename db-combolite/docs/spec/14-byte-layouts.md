# Section 14: Byte layouts

Status: **Draft. The decisions below are confirmed; the open questions are not.**
This section gives the exact offsets and sizes of every on-disk structure. Each structure is added once the owner confirms it. Unless a structure says otherwise, every integer is stored in the byte order the file declares (14.1.2).

## 14.1 File header (epoch 1)

### 14.1.1 Placement

- **Slot size.** A header slot is **512 bytes**. That is the smallest sector size on SD cards and disks, so writing one slot never spans two sectors.
- **Host files.** Slot A is at byte 0 and slot B is at byte 512. Location 0, which holds the catalog root slots (Section 4.7), starts at the first page boundary at or after byte 1024.
- **Raw flash.** The header uses a ring of N slots of 512 bytes each, in dedicated erase blocks (Section 1.11.3).

### 14.1.2 Byte order

- The **byte-order flag** is a single byte: `'L'` (0x4C) or `'B'` (0x42). A single byte has no byte order, so the flag reads the same on every host (Section 1.1).
- The **byte-order check** is the constant `0x01020304`, written in the declared order. It confirms the declared order.

### 14.1.3 Magic number

The magic number is `89 43 44 42 0D 0A 1A 0A` (`\x89CDB\r\n\x1A\n`). The `\r\n` and `\x1A` bytes are there to catch files damaged by text-mode transfers or line-ending conversion.

### 14.1.4 Field layout

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | Magic | 14.1.3 |
| 8 | 1 | Byte-order flag | 14.1.2 |
| 9 | 1 | Epoch | `1` |
| 10 | 1 | Slot role | 0 = working, 1 = committed (Section 1.9) |
| 11 | 1 | Layout flags | bit 0: 64-bit addresses · bit 1: compact page header · bit 2: written by `danger_mode` · bit 3: governance mode · bits 4–7: reserved, zero |
| 12 | 4 | Byte-order check | `0x01020304` |
| 16 | 16 | Database ID | random 128-bit value; companion headers must match it (Section 1.12.2) |
| 32 | 8 | Committed sequence | Section 1.9 |
| 40 | 8 | Header write counter | incremented on every header write, checkpoints included; on flash it identifies the newest slot in the ring |
| 48 | 8 | Page-sequence high-water mark | Section 12.1 |
| 56 | 8 | Anchor-map root | physical location, in units of the smallest page size |
| 64 | 8 | Append point | end of the used archive area, in units of the smallest page size |
| 72 | 1 | Minimum page size | log₂ |
| 73 | 1 | Maximum page size | log₂ |
| 74 | 1 | Page-size mode | 0 = uniform, 1 = per store (Section 2.1) |
| 75 | 1 | Default page size | log₂ |
| 76 | 1 | Checksum algorithm ID | Section 2.9 |
| 77 | 1 | AEAD algorithm ID | Section 12.1 |
| 78 | 2 | Header ring size N | 2 on host files |
| 80 | 8 | Compatible feature flags | 64 bits |
| 88 | 8 | Read-only-compatible feature flags | 64 bits |
| 96 | 8 | Incompatible feature flags | 64 bits |
| 104 | 4 | Application ID | lets a package mark the file, for example as a governance store |
| 108 | 4 | Creating library version | |
| 112 | 4 | Last-writing library version | |
| 116 | 4 | Reserved | zero |
| 120 | 8 | Creation time | microseconds since the Unix epoch |
| 128 | 128 | Master-key reference | UTF-8 URI, up to 128 bytes, padded with zero bytes (no terminator when full) |
| 256 | 144 | Companion slots | 12 slots × 12 bytes (14.1.5) |
| 400 | 64 | Reserved-region anchors | 4 × 16 bytes (14.1.6) |
| 464 | 8 | Commit-number high-water mark | global commit numbers are reserved durably in blocks, in the same way as page sequences (Section 12.1, 14.1.8) |
| 472 | 8 | Anchor-map mirror root | the mirrored copy of the anchor map (Section 3.5, 14.7) |
| 480 | 1 | Content-hash algorithm ID | Section 14.15: SHA-256 by default |
| 481 | 3 | Reserved | zero |
| 484 | 8 | Reconciliation-marks root | logical ID of the per-slot marks index (14.4.8); 0 until a second slot's journal is first reconciled (5.9) |
| 492 | 16 | Tamper-evident tag | keyed tag in tamper-evident mode (Section 12.7), otherwise zero |
| 508 | 4 | CRC32C | covers bytes 0–507 |

### 14.1.5 Companion slot (12 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Companion type ID |
| 2 | 1 | Compatibility class |
| 3 | 1 | State bits: bit 0 provisioned, bit 1 in use |
| 4 | 8 | Watermark: reconciled through this global commit number (14.1.8) |

### 14.1.6 Reserved-region anchor (16 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Feature-flag index |
| 1 | 3 | Reserved, zero |
| 4 | 4 | Size hint, in pages |
| 8 | 8 | Anchor logical ID |

### 14.1.7 Capacity of epoch 1

Epoch 1 provides:

- 12 companion slots;
- 4 reserved-region anchors;
- 64 feature flags in each class.

Raising any of these limits requires a new epoch.

### 14.1.8 Watermarks count global commit numbers

Delta tracks belong to individual processes (Section 1.3), so the header can't hold a watermark for every track of every process. Instead, every transaction receives a **global commit number** when it commits. The number is assigned through the primary coordinator (Section 5.2). Commit numbers are reserved durably in blocks. The high-water mark at offset 464 is persisted before any number in a block is used, so no commit number is ever issued twice, even across crashes. Each companion slot's watermark then means "reconciled through commit number X". After a crash, replay applies every committed transaction whose number is above the watermark, taking it from whichever process's tracks hold it, in commit-number order.

**More than one process (5.9.2).** With more than one process, the reconciling baton holder assigns commit numbers, not the primary. It assigns them contiguously, in merge order, as it merges, so the watermark keeps the meaning above. Whether a journaled transaction is already in the archive is decided by its slot's mark in the reconciliation-marks index (14.4.8), not by its commit number, which the writer leaves at 0.

### 14.1.9 Header integrity

- The header **always** uses CRC32C, whatever the file's checksum flag says. A reader can therefore verify the header before relying on any field in it.
- In tamper-evident mode, the keyed tag at offset 492 is checked as well.

### 14.1.10 Assigned feature flags

The bits of the three feature-flag fields (offsets 80, 88 and 96) that are assigned, with their class (1.2). Every other bit is unassigned and must be zero.

| Class | Bit | Feature | Set when |
|---|---|---|---|
| read-only compatible | 0 | vector change logs (14.12.5) | the first change log in the file is created |
| incompatible | 0 | store ID slot (14.2.7) | always, when the file is created; a library refuses a file without it |

## 14.2 Page header and trailer

The fields are defined in Section 13. 8-byte fields are aligned to 8-byte boundaries.

### 14.2.1 Full header, 64-bit file (56 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Page marker: `C0 DB` |
| 2 | 2 | Page type ID |
| 4 | 1 | Layout version |
| 5 | 1 | Flags |
| 6 | 1 | Compression: bits 0–6 hold the codec; bit 7 is set for extent mode |
| 7 | 1 | Reserved, zero |
| 8 | 4 | Key version |
| 12 | 4 | Payload length |
| 16 | 8 | Write sequence number |
| 24 | 8 | Logical page ID |
| 32 | 8 | Owning store ID |
| 40 | 8 | Encryption domain ID |
| 48 | 2 | Extent position |
| 50 | 2 | Session salt index (14.2.8); 0 on a page that is not encrypted |
| 52 | 4 | Reserved, zero |

### 14.2.2 Full header, 32-bit file (40 bytes)

Bytes 0–23 are the same as in 14.2.1. The rest of the header is:

| Offset | Size | Field |
|---|---|---|
| 24 | 4 | Logical page ID |
| 28 | 4 | Owning store ID |
| 32 | 4 | Encryption domain ID |
| 36 | 2 | Extent position |
| 38 | 2 | Session salt index (14.2.8) |

### 14.2.3 Compact header (28 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Page marker |
| 2 | 2 | Page type ID |
| 4 | 1 | Layout version |
| 5 | 1 | Flags |
| 6 | 1 | Compression |
| 7 | 1 | Extent position (an extent holds at most 255 pages) |
| 8 | 8 | Write sequence number. This keeps its full width so that nonces stay unique. |
| 16 | 2 | Key version (at most 65,536 rotations) |
| 18 | 2 | Payload length |
| 20 | 2 | Logical page ID |
| 22 | 2 | Owning store ID |
| 24 | 2 | Encryption domain ID |
| 26 | 2 | Session salt index (14.2.8) |

### 14.2.4 Trailer

- The trailer occupies the **last 16 bytes** of the page.
- **Compact pages with a checksum only** use an 8-byte trailer instead (Section 13.4).
- A checksum that is shorter than the trailer, such as the 4-byte CRC32C, is padded with zeros.

The trailer holds one of three things, depending on how the page is protected:

- the AEAD tag;
- the keyed integrity tag, in tamper-evident mode;
- the checksum.

### 14.2.5 Nonce construction

AES-GCM and ChaCha20-Poly1305 both take a 12-byte nonce:

**nonce = track ID (4 bytes) ‖ sequence number (8 bytes)**

- **Archive pages.** The track ID is the reserved value `0xFFFFFFFF`, and the sequence number is the page's write sequence number. Page sequence numbers are unique across the whole file (Section 12.1).
- **Reserved track prefixes.** Coordinator slot numbers stop at `0xFFFD`, so a delta record's track ID (slot first) can never start with `0xFFFE` or `0xFFFF`, which the two page tracks use.
- **Overlay pages** (5.9.1). The track ID is `0xFFFE` followed by the process's slot number (2 bytes), and the sequence number comes from a counter of the session's own, which starts at 1 and never goes back within the session, whatever rebases empty the overlay. The overlay's subkey is the session's (14.2.8), so its nonces can't meet another session's, and the track ID keeps them apart from the archive pages the same session writes once it holds the baton.
- **Delta records.** The track ID is the coordinator slot number (2 bytes) followed by the companion type (2 bytes), and the sequence number is the delta sequence number. Delta sequences are reserved durably in the slot (Section 12.1), so numbering continues correctly when a later process reuses the slot.
- **Associated data.** The page header, or the delta record header, is passed as AEAD associated data. It is authenticated, but not encrypted.

### 14.2.6 Identifiers and numbering

- **Checksum algorithm IDs** (file header offset 76): `1` = CRC32C. `0` is invalid. Further algorithms are assigned IDs when they are added.
- **Page type IDs** (page header offset 2):

  | ID | Page type |
  |---|---|
  | `0x0001` | anchor map |
  | `0x0002` | free space |
  | `0x0003` | index node (14.8.7) |
  | `0x0004` | catalog record (14.3) |
  | `0x0005` | document revision (14.9.2) |
  | `0x0006` | vector block (14.12.3) |
  | `0x0007` | overflow (14.15) |
  | `0x0008` | time-series block (14.11.1) |
  | `0x0009` | nibblet page: small records of any store (14.18) |
  | `0x000A` | enclave root: the roots of an enclave's or joined store's own catalog (14.3.5) |
  | `0x000B` | held log: a domain's committed changes waiting for its key (14.4.10) |
  | `0x7F00` | reserved for tests |

  Further page types are assigned IDs when they are built.
- **Page header flags** (offset 5) take bits in the order Section 13 lists them: bit 0 compressed, bit 1 member of an extent, bit 2 overflow continuation, bit 3 tamper-evident, bit 4 written in `danger_mode`; bits 5–7 are reserved, zero. Only bit 2 is set so far.
- **Zero means "none".** Logical page ID `0` and physical page `0` both mean "no page".
- **Physical page numbers** count from the start of the file, in units of the file's page size. The **data area** starts after the two header slots (1,024 bytes) and the location-0 area (1,024 bytes), rounded up to a page boundary. System pages that are addressed physically, such as anchor-map and free-space pages, carry logical ID `0`, store `0` and domain `0`.

### 14.2.7 The store ID slot, and hidden IDs on encrypted pages

Every page's payload starts with a **store ID slot**, in every domain and for every page type, system pages (anchor map, free space) included, so that a page type has the same room whether its page is public or encrypted, and a store can move between domains page by page (12.8).

- **The slot** holds the owning store ID, at the file's ID width (2, 4 or 8 bytes) and in the file's byte order. It is 0 for a page that no single store owns, such as a nibblet page or an enclave root page. The page type's own layout follows it.
- **Public pages.** The slot repeats the header's owning store ID; a page whose two disagree is corrupt.
- **Encrypted pages** (an enclave or a joined store) hide their store (12.8): the header's owning store ID field holds 0, and the encryption domain ID field names the domain, so the reader finds the key without the catalog. The slot, inside the encryption, is the only record of the store.
- **Lengths.** The header's payload length counts the slot. Page types size themselves from the payload room after it, which is the same in every domain. **Offsets in every page layout of this section count from the end of the slot, and every capacity or fan-out figure is computed from the room after it.**
- **Order of operations.** The slot is written outside compression: the plaintext is the slot followed by the page type's payload, compressed when the page is compressed (Section 15), and on an encrypted page the whole plaintext is then encrypted.
- **Checks.** A reader that expects a page of a known store checks the slot, after decryption on an encrypted page.
- **Files written before the slot** are not supported and are not migrated: the format is not yet stable (pre-1.0), and such a database is rebuilt from its source data. Files with the slot carry incompatible feature flag 0 (14.1.10): a library that knows the slot refuses a file without the flag, and an older library refuses a file with it, each with a clear status instead of misreading pages.

### 14.2.8 Session salts (12.1)

- **Drawing.** Opening a database that has a master key for writing draws a 16-byte salt from the environment's random hook (`cdb_env.random`), once, before its journal is created. Without the hook, such a database opens read-only. A database without a master key draws none, and its companion headers hold zero.
- **Journal and overlay files** carry the salt in their companion header (14.4.1), and every record or overlay page in a file uses that file's salt. A process that takes over a dead slot reconciles that slot's journal and then starts a journal of its own, under its own salt (14.4.7 rule 9). Records and overlay pages use the header's salt; a delta record names no salt, and an overlay page's salt index is `0xFFFF`, meaning "the salt in this file's header".
- **Held records** keep the salt they were written under in their held entry (14.4.10).
- **Archive pages** name their salt in the salt index field of their header (14.2.1–14.2.3), 1–65,534; the field is part of the header, so it is authenticated as associated data. Index 0 means no salt and is used only by pages that are not encrypted.
- **Salt table.** An ordered index (14.8) whose root is at catalog root slot offset 104 (14.3.2): key, the salt index (2 bytes, big-endian); value, the salt (16 bytes) and the last commit that used it (8 bytes, below). Only reconciliation writes archive pages, so only the baton holder writes the table: in the archive commit that writes pages under a salt, it adds the salt at the lowest free index if it isn't there yet, and sets the salt's last-commit sequence to that commit. There is no crash window and no race between processes.
- **Derivation.** The page subkey (12.2) is derived with HKDF-SHA-256 from the domain key, with the session salt as the HKDF salt and `cdb page` as the info. The other subkeys of 12.2 never produce a nonce from a counter and are derived without a session salt, so every copy and session computes the same values.
- **Pruning.** A salt-table value is the salt (16 bytes) followed by the commit sequence of the last archive commit that wrote a page under it (8 bytes). A salt leaves the table only when a complete scrub pass (3.5) that **started after** that commit finds no archive page using it; a page written under the salt later moves the commit sequence on, so a salt still in use is never removed. When every index is taken, reconciliation re-encrypts pages under an older salt with the current session's (as for a key version, 12.4) until a scrub pass frees an index.

## 14.3 Catalog root slot (location 0)

### 14.3.1 Placement

- Each root slot is **512 bytes**, the same size as a header slot. A recovery tool can therefore find root slots without knowing the file's page size.
- **Host files.** Slots A and B are placed back to back at location 0 (Section 4.7). The area is then padded up to the next page boundary, and the catalog body starts after the padding.
- **Raw flash.** A ring of N slots is used instead (Section 1.11.3).
- **Choosing a slot on open.** The newest slot whose commit sequence is ≤ the header's committed sequence is used (Section 3.2).

### 14.3.2 Field layout

Logical IDs in this structure are always 8 bytes, even in 32-bit files, so the structure is identical in every file.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | Start magic | `89 43 44 42 43 41 54 0A` (`\x89CDBCAT\n`) |
| 8 | 1 | Epoch | |
| 9 | 1 | Slot index | |
| 10 | 2 | Reserved | zero |
| 12 | 4 | Catalog format version | |
| 16 | 8 | Commit sequence | |
| 24 | 8 | Root write counter | identifies the newest slot in a flash ring |
| 32 | 8 | Catalog body root | logical page ID of the primary copy |
| 40 | 8 | Catalog mirror root | logical page ID of the mirrored copy (Section 3.5) |
| 48 | 8 | Reserved | zero (the enclave directory is in the catalog body, 14.3.4) |
| 56 | 8 | Schema-history root | Sections 4.3, 4.11 |
| 64 | 8 | Public quick-query index root | derived, can be rebuilt (Section 4.2) |
| 72 | 8 | Public dedup index root | |
| 80 | 8 | Public audit series root | Section 10.7 |
| 88 | 8 | Next object ID | the store, enclave and procedure ID counter; IDs are never reused (Section 4.12) |
| 96 | 8 | Public nibblet free-space root | the free-space index of public nibblet pages (14.18.3); 0 until the first nibblet |
| 104 | 8 | Session salt table root | 14.2.8; 0 until the first encrypted archive page |
| 112 | 8 | Replication group record | object ID of the copy's group record (14.21.1); 0 when the database does not replicate |
| 120 | 364 | Reserved | zero |
| 484 | 8 | End magic | `0A 54 41 43 42 44 43 89` (the start magic reversed) |
| 492 | 16 | Tamper-evident tag | |
| 508 | 4 | CRC32C | covers bytes 0–507 |

Each store's entry-ID counter is held in that store's catalog entry, not in the root.

### 14.3.3 Catalog body and records

- **One tree, mirrored.** The catalog body is a single B+tree (14.8), written twice: primary and mirror (Section 3.5). Every key starts with a kind byte:

  | Prefix | Key | Value |
  |---|---|---|
  | `01` | object ID (8 bytes, big-endian) | record reference |
  | `02` | object kind (key int) + name (key string) | object ID |
  | `03` | store ID + schema version | record reference |
  | `04` | store ID + field name | field ID |
  | `05` | store ID + field ID | field name |
  | `06` | none | next object ID: a copy of the root slot's counter |
  | `07` | joined store ID (8 bytes, big-endian) | empty: a joined store that links into this catalog's domain (12.9) |
  | `08` | procedure ID + version (4 bytes, big-endian) | record reference: every version of a procedure (14.3.6) |
  | `09` | store ID + procedure ID | empty: a procedure in this catalog that depends on the store (14.3.6) |
  | `0A` | principal ID + role ID | empty: a grant of a role in this catalog to a principal (14.3.7) |

- **Record pages.** Records live in pages of type `0x0004`. Each record is written twice, and a **record reference** is the two logical page IDs, 16 bytes.
- **Store records** use the shared value encoding (14.6), with these field IDs:

  | ID | Field | ID | Field |
  |---|---|---|---|
  | 1 | kind | 7 | compression |
  | 2 | ID | 8 | schema version |
  | 3 | name | 9 | next field ID |
  | 4 | data model | 10 | creating commit |
  | 5 | encryption domain | 11 | flags; bit 0 = dropped |
  | 6 | page-size code (log₂) | 12 | document pools: head index root; link store: forward index root (optional) |
  | 13 | document pools: revisions index root; link store: reverse index root (optional) | 14 | document pools: next entry ID (optional) |
  | 15 | document pools: full-text settings, an object (14.14.1; optional) | 16 | settings, an object: vector collections (14.12.3), time-series stores (14.11.1) |
  | 22 | moving (optional): an object `{1: the domain being moved out of, 2: walk cursor}`; absent when no move is under way (12.8) | | |
  | 23 | replicated store (optional): clock index root, conflict list root, next conflict number (14.21.2) | | |

  Optional fields that are absent read as zero. Field IDs belong to an object kind: a kind that reuses a store record's ID for the same meaning says so, and any other field ID means only what its kind defines.

- **Moving a store (12.8).** While field 22 is present, field 5 names the domain the store is moving into, and the record lives in that domain's catalog. The walk cursor is a binary value whose meaning is the data model's: the structure and key the walk stopped at. Empty means start from the beginning; restarting is always correct, because a page already in the new domain is passed over by its header alone. The finish call, or the walk reaching its end, removes field 22 in the archive commit that re-encrypts the last old page.

- **Object kinds** (field 1, and the kind in key `02`): 1 store · 2 link type (14.13.1) · 3 enclave · 4 joined store (14.3.4) · 5 procedure (14.3.6) · 6 principal · 7 role (both 14.3.7) · 8 replication group record (14.21.1). Reading an object as the wrong kind finds nothing. **Data models** (field 4): 1 relational · 2 document · 3 vector · 4 time series · 5 the link store.

- **Writing the root slots.** The first catalog commit writes both root slots. Later commits alternate between them.
- **Choosing a slot on open.**
  - If both slots are damaged, the catalog is lost: **corrupt**.
  - If one slot is damaged and the other was never written, a first catalog commit was cut short, so the file has no catalog yet.
- **The ID counter.** The next object ID is the larger of the root slot's value and the body's `06` value. A fallback to an older slot therefore never reuses an ID.
- **Catalog changes commit immediately.** At the database level they reconcile first, and they are refused while a transaction is open.
- **Names.** Names are 1–64 bytes of valid UTF-8. A name's key must fit within the index key limit. Field names must also fit within the index value limit.

### 14.3.4 Enclave directory

The enclave directory is the public half of every enclave and joined store (12.8, 12.9). It holds what is needed to find a domain's key and its encrypted catalog, and nothing that names or describes what is inside (Section 4.4).

- **Where.** Each enclave or joined store is a catalog object in the public catalog body (14.3.3), of kind 3 (enclave) or 4 (joined store). Its record is stored like a store record, under key `01` + its object ID, mirrored and committed with the catalog. It takes its ID from the shared object ID counter (4.12).
- **No name.** A directory record has no name and no key `02` entry. An enclave is found by its ID; names exist only inside its own catalog.
- **Record fields**, in the shared value encoding (14.6). Fields 1, 2, 10 and 11 mean what they mean in a store record:

  | ID | Field |
  |---|---|
  | 1 | kind: 3 enclave, 4 joined store |
  | 2 | ID |
  | 10 | creating commit |
  | 11 | flags; bit 0 = dropped |
  | 17 | key versions: an array of objects `{1: version, 2: key reference}`, one for each version still in use (below), in increasing version order |
  | 18 | current key version: the version new writes use; it is in field 17 |
  | 19 | enclave root: an array of two logical page IDs, the primary and the mirror enclave root page (below) |
  | 20 | rotation: an object `{1: last rotated, 2: interval, 3: overdue response}` |
  | 21 | held log: the logical page ID of the held log's head page (14.4.10) |

- **Key versions.** Each key reference (12.3) addresses that version's key, wrapped under the master key, in the external keystore. Rotating the key appends a version and makes it current. A version is removed from field 17 only once nothing uses it: no archive or overlay page, no journal record of any process, no held record (14.4.10), no keyed hash or dedup entry (14.16), and no tag or sealed reference in a joined store (14.13.2). A page or record whose key version is not in field 17 is corrupt.
- **Enclave root pages.** The two pages named in field 19 are in the domain's own encryption domain, encrypted under its current page subkey, and hold the roots of the domain's own catalog (14.3.5). The primary and the mirror are written alike, as the public catalog's body is (3.5); their logical IDs never change.
- **Opening.** Opening an enclave or joined store fetches and unwraps the current key version and reads the primary root page, or the mirror when the primary is damaged. A key that does not authenticate either page is the wrong key: the open fails with `CDB_E_SECURITY` and nothing is cached.
- **Rotation state** is public, so that it can be checked before any key is fetched:
  - *last rotated*: a timestamp in nanoseconds since the Unix epoch, from the environment's clock, as commit timestamps are (14.4.4); the creation time until the first rotation;
  - *interval*: nanoseconds; 0 means the database configuration's default (12.4);
  - *overdue response*: 0 the configuration's default, 1 warn, 2 refuse writes to the domain, 3 refuse to open it.

  An outsider can learn when a domain's keys were rotated and how its rotation is configured, but nothing about its contents.

### 14.3.5 Enclave root pages and an enclave's catalog

An enclave or joined store keeps its own catalog (12.8, Section 4.4). It has the same shape as the public catalog, inside the domain: everything in it is written in the domain and encrypted under the domain's page subkey.

- **Enclave root pages.** The two pages named in the directory record's field 19 (14.3.4) are logical pages of type `0x000A` in the domain. Their header's store ID is 0, like every encrypted page's (12.8). Both are written alike at every change to the domain's catalog. Payload, in the file's byte order:

  | Offset | Size | Field | Notes |
  |---|---|---|---|
  | 0 | 8 | Magic | `89 43 44 42 45 4E 43 0A` (`\x89CDBENC\n`) |
  | 8 | 4 | Format version | 1 |
  | 12 | 4 | Reserved | zero |
  | 16 | 8 | Domain ID | must equal the directory record's ID |
  | 24 | 8 | Commit sequence | the archive commit that wrote this version |
  | 32 | 8 | Catalog body root | logical page ID of the primary copy |
  | 40 | 8 | Catalog mirror root | logical page ID of the mirrored copy (3.5) |
  | 48 | 8 | Schema-history root | |
  | 56 | 8 | Quick-query index root | derived, can be rebuilt (4.8) |
  | 64 | 8 | Dedup index root | the domain's own (Section 1.6) |
  | 72 | 8 | Audit series root | the domain's own, hash-chained (12.7) |
  | 80 | 8 | Nibblet free-space root | the domain's own (14.18.3); 0 until its first nibblet |
  | 88 | 40 | Reserved | zero |

  A root that is 0 does not exist yet. A page whose magic, version or domain ID is wrong is corrupt; the open falls back to the other page, and fails with `CDB_E_CORRUPT` when both are.
- **The body** is one mirrored B+tree with the public body's key prefixes (14.3.3): objects (`01`), names (`02`), schema versions (`03`, `04`, `05`). Its records use the same field IDs and kinds. Prefix `06` is not used: object IDs come from the **public** counter (14.3.2 offset 88), so an ID is unique across the whole database and survives a move between domains (12.8). An outsider can see that an object was created, and when, but not in which domain or of what kind.
- **What it holds:** the records of the domain's stores (including its own link store, model 5), link types, schema history, field dictionaries, procedures (12.10) and roles (12.5).
- **Changes commit as public catalog changes do** (14.3.3): they reconcile first, are refused while a transaction is open, and commit immediately. A change that touches both catalogs, such as creating a store in an enclave (which advances the public counter) or moving a store between domains, writes both in one archive commit.

### 14.3.6 Procedure records (12.10)

A stored procedure is a catalog object of kind 5, named in key `02` like a store, in the catalog its placement chooses (12.10). Key `01` + its ID leads to its current version's record; key `08` + its ID + a version leads to each version's record, the current one included. Every version is a separate record, and none is rewritten.

- **Record fields**, in the shared value encoding (14.6). Fields 1, 2, 3, 10 and 11 mean what they mean in a store record; field 10 is the commit that created this version.

  | ID | Field |
  |---|---|
  | 1 | kind: 5 |
  | 2 | ID |
  | 3 | name |
  | 10 | creating commit |
  | 11 | flags; bit 0 = dropped |
  | 23 | version, from 1 |
  | 24 | compiler version (4.10) |
  | 25 | source text: a body (below) |
  | 26 | compiled form: a body holding the bytecode (14.20) |
  | 27 | dependencies: an array of objects `{1: store ID, 2: field ID, 0 for the whole store, 3: access, bit 0 read, bit 1 write}` |
  | 28 | calls: an array of the procedure IDs it calls |
  | 29 | joined store (compound procedures): the joined store named at creation, kept when the procedure becomes single-domain |

- **Bodies.** A body is an object: `{1: bytes}` inline, when the bytes are at most a quarter of the page size; otherwise `{2: total length, 3: first logical page ID}` of an overflow chain (14.15) in the procedure's domain. A body stored the other way is rejected as not canonical.
- **Dependents.** For each store in a version's dependencies, the catalog that holds the procedure has key `09` + store ID + procedure ID, for the current version only. A store move (12.8) finds the procedures to re-place through key `09` in the public catalog, the store's old domain's catalog and the catalogs of the joined stores linking into it, all of which are open during a move.
- **Re-placing** a procedure moves its records (every version), its key `02` and its keys `08` and `09` to the catalog its placement now chooses, in the store move's archive commit. Its ID does not change. Overflow chains move with it, re-encrypted in the new domain.
- **Dropping** a procedure sets bit 0 of field 11 on its current record; its history stays (4.11).

### 14.3.7 Principals, roles and grants (12.5)

- **Principals** are catalog objects of kind 6 in the public catalog, named in key `02`. Fields 1, 2, 3, 10 and 11 mean what they mean in a store record; bit 0 of field 11 marks a retired principal, which keeps its record (signatures it made stay checkable) but holds no roles.

  | ID | Field |
  |---|---|
  | 30 | tenant ID |
  | 31 | signature algorithm: 1 ECDSA over P-256 with SHA-256 |
  | 32 | public key: for algorithm 1, the 33-byte compressed point |

- **Roles** are catalog objects of kind 7, named in key `02`, in the catalog of the domain their scopes lie in (12.5). Fields 1, 2, 3, 10 and 11 as above; bit 0 of field 11 marks a dropped role, whose grants are removed in the same commit.

  | ID | Field |
  |---|---|
  | 33 | permissions: an array of objects `{1: scope kind, 2: scope ID, 3: permissions}` |

  - **Scope kinds:** 0 the whole database (scope ID 0; public roles only) · 1 a store · 2 a domain as a whole (an enclave or joined store: scope ID is its ID; only in that domain's own catalog) · 3 a procedure.
  - **Permissions:** bit 0 read · bit 1 write · bit 2 execute · bit 3 admin; other bits zero.
  - Every store and procedure a role names must be in the role's own catalog. A role naming anything else is malformed.
- **Grants** are keys `0A` + principal ID + role ID, with an empty value, in the role's catalog. Granting a role that is already granted, or revoking one that is not, changes nothing and is not an audit event.
- **Checking a permission** for a principal on a store or procedure takes the union of the permissions that every granted role in the public catalog and in the object's own domain gives on the object itself, on its domain as a whole, and on the whole database.
- **Audit events** (12.5) name the principal, role and scope IDs, never key material.

## 14.4 Companion files, delta records and commit records

### 14.4.1 Companion file header (80 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic: `89 43 44 42 43 4D 50 0A` (`\x89CDBCMP\n`) |
| 8 | 1 | Byte-order flag. Repeated here so that the companion can be examined on its own. |
| 9 | 1 | Epoch |
| 10 | 2 | Companion type ID |
| 12 | 2 | Slot number of the owning coordinator |
| 14 | 2 | Reserved, zero |
| 16 | 16 | Database ID. Must match the archive's (Section 1.12.2). |
| 32 | 8 | Committed sequence of the archive this companion was unsealed from |
| 40 | 8 | First delta sequence number in this file |
| 48 | 8 | Encryption domain ID: the domain of a companion that belongs to one (named by its opaque ID, 1.12.1); 0 for a file that holds several domains, such as a main journal or an overlay, whose records and pages name their own domain (14.2, 14.4.2) |
| 56 | 16 | Session salt (12.1, 14.2.8) of the session that writes this file; zero when the database has no master key |
| 72 | 4 | Reserved, zero |
| 76 | 4 | CRC32C over bytes 0–75 |

### 14.4.2 Delta record

A delta record is a 48-byte header, followed by the payload, followed by the trailer.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Record marker: `C0 DE` |
| 2 | 1 | Record kind: 0 = data change · 1 = commit record · 2 = abort · 3 = neuron-weight update (Section 6.10) · 4 = write-set key · 5 = prepare (both 5.9.4) · 6 = rollback (14.4.9) · 7 = replication (14.21.1) |
| 3 | 1 | Flags: `0x01` encrypted · `0x02` compressed · `0x04` continued · `0x08` replicated · `0x10` on a commit record only: the transaction holds a rollback record (14.4.9) |
| 4 | 4 | Payload length |
| 8 | 8 | Delta sequence number, which is the nonce source (14.2.5) |
| 16 | 8 | Transaction ID (14.4.3) |
| 24 | 8 | Target store ID |
| 32 | 8 | Target entry ID, or 0 if there is none |
| 40 | 4 | Key version |
| 44 | 4 | Reserved, zero |
| 48 | n | Payload |
| 48+n | 16 | Trailer: the AEAD tag, or a checksum padded with zeros |

The header is the AEAD associated data: it is authenticated but not encrypted.

**Hidden IDs.** A record that targets a store in an enclave or a joined store (12.8) is encrypted, and hides its target:

- the target store field (offset 24) holds the record's **encryption domain ID**, and the target entry field (offset 32) holds 0;
- the encrypted plaintext starts with the real **target store ID** and **target entry ID**, as two unsigned varints (14.6, shortest form only), followed by the payload. The payload length counts them;
- they are written outside compression: the varints, then the payload (compressed when flag `0x02` is set), all encrypted together.

This applies to every record kind that names a store: data changes, write-set keys (kind 4) and neuron-weight updates. Commit, abort, prepare, rollback and replication (kind 7) records name no store; they are never encrypted and are unchanged. A record that is not encrypted keeps its IDs in the header.

### 14.4.3 Transaction IDs

A transaction ID is 8 bytes:

- **2 bytes:** the slot number of the coordinator that started the transaction;
- **6 bytes:** a counter kept by that slot.

Each slot reserves its counter values durably, in blocks, with the block's high-water mark stored in the slot (Section 12.1). Transaction IDs are therefore unique across processes, and no coordination is needed for each new transaction.

### 14.4.4 Commit record

A transaction's single **commit record** is written to the owning process's **main journal**, `main.<slot>.cdb`. At replay, a transaction counts only if this commit record is present (Section 3.3).

Commit record payload:

| Size | Field |
|---|---|
| 8 | Global commit number (14.1.8). Written as 0 (5.9.2); the number is assigned at reconciliation |
| 8 | Commit timestamp from the primary coordinator's clock: ns since the Unix epoch. Until the coordinator exists, the environment's clock; 0 when the platform has none. When a prepare record was written, the same stamp as that record. This is the stamp used for merge order (5.9.4) |
| 8 | Principal identity ID (Section 5.1) |
| 4 | Number of deltas in the transaction, not counting data records a rollback dropped (14.4.9) |
| 4 | Bitmask of the companion types the transaction touched |
| 2 | Signature length; 0 if the record is unsigned |
| n | Signature, present when the signing policy requires one (Section 12.5) |

### 14.4.5 Data-change payload

A data-change payload is an **operation code** followed by a value in the shared binary encoding (Section 8.1).

| Code | Operation |
|---|---|
| insert | create an entry |
| replace | replace an entry in full |
| patch | change some fields of an entry |
| delete | remove an entry |
| link | create a link (Section 6) |
| unlink | remove a link |

The exact encoding of each operation, per data model, is defined with the data-model layouts. Document payloads are defined in 14.9.3.

### 14.4.6 Session, journal and recovery rules (single process)

1. **One main journal.** It holds both the data records and the commit records. Typed tracks per data model are added together with the data models.
2. **Companion slot 0 records the journal.**
   - The slot holds: type `1` (main journal), class *incompatible*, state *provisioned*, and the reconciled-through watermark (14.1.8).
   - **Setting "in use".** The slot is marked *in use* only after the journal and its directory entry are durable.
   - **Clearing "in use".** At close, *in use* is cleared before the journal is removed.
   - **Consequence.** A journal that is marked *in use* but is missing therefore always means possible loss. Opening is refused unless the operator accepts the loss (Section 1.12.5).
3. **Which journal may be replayed.** A journal is replayed only onto the archive state it was unsealed from: its unsealed sequence must equal the archive's committed sequence.
   - **Header sequence.** A journal created when a session opens records the sequence produced by the commit that marks it *in use*.
   - **Damaged or foreign journals.** A journal that is unreadable, belongs to another database, or would need replaying onto a different state is renamed `main.<slot>.cdb.stray` and is never applied.
4. **Journal rotation.** Every commit of the archive starts a new, empty journal: reconciliation, and also commits made outside it (catalog changes, compaction), which reconcile first. Otherwise later transactions would be journaled against a state the archive has moved past, and rule 3 would set them aside after a crash. The new journal is written as `main.<slot>.cdb.new`, made durable, then renamed over the old one, and the directory is synced. A crash always leaves exactly one journal.
5. **Commit numbers** start at 1. *Since M19 (5.9.2):* they are assigned at reconciliation, in the same archive commit as the watermark, so they need no reservation. The high-water mark at header offset 464 is kept as written and no longer advanced. Transaction counters are reserved instead, in blocks of 1024, in the coordinator slot (14.4.3).
6. **Durability.** The default is *normal*, with a barrier every 32 commits (configurable). *Off* has barriers only at reconciliation and close.
7. **Visibility.** A transaction's changes are applied, and become visible, when it commits. A transaction cannot read its own changes before it commits.

### 14.4.7 Multi-process rules (5.9)

1. **One journal per slot.** Slot *s* writes `main.<s>.cdb`. Rules 2–4 of 14.4.6 apply per slot. The slot's coordinator file (14.5) records *journal in use*, because the header's companion slot can't record one flag per process.
2. **Rotation.** A process rotates its own journal after it sees a new archive. The new journal keeps the transactions above its slot's mark, copied in their original order. A transaction with rollback records is copied without them and without the data records they dropped, and its commit record loses flag `0x10` (14.4.9). The baton holder never rotates another slot's journal.
3. **Write-set key record (kind 4).** Target store: the table. Target entry: 0. Payload: index number (4 bytes), then the CRC32C of the encoded key (4 bytes). A hash collision can only cause a needless collision, never a missed one.
4. **Prepare record (kind 5).** Payload: the stamp (8 bytes), from the primary's clock in ns.
5. **Replay after a crash.** For every slot's journal, the committed transactions above the slot's mark are merged in (stamp, slot, counter) order and numbered on from the watermark. Rule 3 of 14.4.6 becomes: a journal with a transaction to replay must have been started from the archive's committed state or an earlier one; one started from a later state is set aside.
6. **The marks index borrows the catalog's work memory,** so every database is opened with `catalog_work` and `catalog_page` configured.
7. **Journal in use, two places.** The slot's flag (14.5.2, bit 2) is what counts for each process: it is set once the slot's journal and its directory entry are durable, and cleared before the journal is removed. The header's companion slot 0 says that some session may hold changes: the first session to open sets it, and only a coordinator that closes while no other is alive clears it. A slot whose flag is set but whose journal is missing means possible loss. Recovery then refuses to open, and a baton holder's reconciliation returns `CDB_E_CORRUPT`, unless the operator accepts the loss (1.12.5).
8. **Overlay file (5.9.1).** `pages.<slot>.cdb` holds whole pages in the archive's page format, at physical page numbers of its own. Nothing ever refers to it from the archive, it is never replayed, and recovery removes it.
9. **Recovery only when alone.** Opening replays journals (rule 5) only when no other coordinator is alive. Otherwise the baton holder merges the dead slots' journals at its next reconciliation. A process that takes over a dead slot whose journal still holds transactions reconciles once before it starts its own journal.

### 14.4.8 Reconciliation-marks index (5.9.2)

A B+tree (14.8) whose root logical ID is at header offset 484.

| Part | Content |
|---|---|
| Key | Coordinator slot number, encoded as an unsigned integer key (14.8) |
| Value | 8 bytes: the transaction counter (the 6-byte counter of 14.4.3, widened) through which this slot's journal is reconciled |

### 14.4.9 Rollback record (savepoints, 11.12.3)

Rolling back to a savepoint drops the data records an open transaction has journaled since the savepoint. The journal stays append-only: nothing is rewritten, and no delta sequence number is used twice, so no nonce is reused (14.2.5).

1. **Ordinals.** A transaction's data records (kind 0) are numbered 0, 1, 2, … in journal order. Other record kinds are not numbered. Rotation can give records new sequence numbers but keeps their order, so ordinals stay valid.
2. **Rollback record (kind 6).** Target store and entry: 0. Payload, 8 bytes in the archive's byte order:

   | Size | Field |
   |---|---|
   | 4 | First dropped ordinal *p* |
   | 4 | Number *n* of the transaction's data records that precede this record |

   It drops the data records with ordinals *p* to *n* − 1. A record with *p* > *n*, or whose *n* does not match the data records before it, is malformed. A writer never writes one that drops nothing (*p* = *n*).
3. **Several rollbacks.** A transaction's dropped ordinals are the union of what each of its rollback records drops. A later rollback to an earlier savepoint covers the earlier ones' ranges.
4. **Limit.** The dropped ordinals of one transaction form at most **32** disjoint ranges. This is a format limit (17.2): a writer refuses a rollback that would make a 33rd range with `CDB_E_OUT_OF_BUDGET`, before writing anything, and a reader treats a transaction with more as malformed.
5. **Commit.** A transaction with at least one rollback record sets flag `0x10` on its commit record. A reader that finds the flag reads the transaction's records once to collect the dropped ranges before it applies any. A reader that does not find it does not look for rollback records. The commit's delta count leaves dropped records out. A transaction whose data records were all dropped ends with an abort record, not a commit record.
6. **Readers.** Everything that takes a committed transaction's changes skips dropped records: the apply at commit, replay after a crash, rotation (14.4.7 rule 2) and replication export. An open transaction's own reads of its journal (its view of its own changes) skip them too.
7. **Collisions.** Write-set key records (kind 4) are never dropped, and another process's collision scan (5.9.4) does not look for rollback records. Dropped changes can therefore cause a needless collision, never a missed one.

### 14.4.10 Held log (12.8)

A domain's held log keeps the committed changes of that domain that a process could not apply because it lacked the key. It lives in the archive, so the database stays one file at rest, and it is written without the key: it holds the records' ciphertext unchanged.

1. **Pages.** The log is a chain of logical pages of type `0x000B`. They are public pages (domain ID 0, store ID 0) holding another domain's ciphertext. The head page is allocated when the domain is created; its logical ID is in the directory record's field 21 (14.3.4) and never changes. Payload, in the file's byte order:

   | Size | Field |
   |---|---|
   | address width | Next page's logical ID, 0 for the last page |
   | 4 | Bytes used in this page after this field |
   | n | Log bytes |

   The log is the concatenation of the pages' log bytes. An entry can continue onto the next page.
2. **Entry.** One per held part of a transaction, in merge order (14.4.7 rule 5):

   | Size | Field |
   |---|---|
   | 4 | Length of the rest of the entry |
   | 8 | Transaction ID (14.4.3) |
   | 8 | Commit stamp (14.4.4), the merge order key |
   | 8 | Global commit number the transaction was given at reconciliation (14.1.8) |
   | 4 | Track ID of the records: slot (2 bytes) and companion type (2 bytes), as in the nonce (14.2.5) |
   | 16 | Session salt the records were written under, from their journal's header (14.2.8); zero for records that are not encrypted |
   | 4 | Record count |
   | n | The records, each a complete delta record (14.4.2) copied byte for byte |

   Only the domain's records that the transaction keeps are copied: its data changes and neuron-weight updates, in journal order. Write-set keys, rollback records and the data records a rollback dropped (14.4.9) are left out.
3. **Nonces.** Delta sequence numbers are never reused within a slot (12.1). A held record keeps its header, sequence number and ciphertext, so it is decrypted with the nonce built from the entry's track ID and the record's own sequence number, under the page subkey derived with the entry's session salt. Copying it changes nothing that the nonce or the associated data covers, and delta sequence numbers are reserved durably (12.1), so no other record ever takes that nonce.
4. **Holding** happens in the archive commit that applies the transaction's other domains and advances its slot's mark (5.9.2). The entry is appended to the log, and new pages are linked, in that same commit, so a crash leaves either both or neither.
5. **Draining** happens in a reconciliation by a process that holds the domain's key. It applies the entries in log order, frees every page but the head, and empties the head, all in one archive commit; then it applies the domain's new changes.
6. **Reading.** A process with the domain open reads the log's entries in order, after the archive and before the journals, when it builds its view of the domain.
7. **Key versions.** A held record names the key version that encrypted it. That version stays in the directory record's field 17 until the record is drained (14.3.4).
8. **Collisions.** Held entries keep no write-set keys, and need none: holding happens in an archive commit, which rejects every transaction open across it (5.9.4), and a transaction begun later reads the held log as part of its view (rule 6), so it sees the held changes it would clash with.
9. **Replication form.** An entry that holds sealed replication changes (14.21.5) has transaction ID 0, track ID 0, commit number 0 and a zero session salt, and its commit stamp is the newest change's clock physical time; its records are the changes, each exactly as received. Draining merges them (19.5) instead of applying them, in log order.

## 14.5 Coordinator slot

### 14.5.1 Placement

- **Host files:** each coordinator slot is the file `coord/<slot>`, the slot number in decimal.
- **Lobby files (5.9.5):** `coord/join.<session>`, the session identity as 32 lowercase hex digits. One 512-byte half in the layout below: request kind 4, the wanted slot at offset 10 and the wanted priority at offset 14. Times are µs of the primary's clock, as in the slot.
- **Raw flash:** each slot is a region.
- **Two halves.** Every slot has two 512-byte halves, A and B. Writes alternate between them. The valid half with the higher heartbeat counter wins. A torn heartbeat write therefore never makes a live coordinator look dead.
- **Maximum number of coordinators.** It can be configured. The default comes from the device-class table (Section 2.8): about 1–2 on microcontrollers and 64 on desktop and server hosts. On raw flash, the coordinator region is sized for this maximum.

### 14.5.2 Field layout (each half)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic: `89 43 44 42 43 52 44 0A` (`\x89CDBCRD\n`) |
| 8 | 1 | Byte-order flag |
| 9 | 1 | Epoch |
| 10 | 2 | Slot number |
| 12 | 1 | Role: 0 = free · 1 = subordinate · 2 = primary |
| 13 | 1 | State flags: bit 0 alive · bit 1 unfinished baton session pending · bit 2 journal in use (14.4.7) |
| 14 | 2 | Priority value (Section 5.5) |
| 16 | 16 | Database ID |
| 32 | 16 | Session identity: a random 128-bit value (Section 5.1) |
| 48 | 8 | ID of the principal identity the session authenticated as |
| 56 | 8 | Heartbeat counter |
| 64 | 8 | Heartbeat timestamp, from the primary's clock (µs) |
| 72 | 8 | Delta-sequence high-water mark (Section 12.1) |
| 80 | 8 | Transaction-counter high-water mark (14.4.3) |
| 88 | 1 | Current request kind: 0 = none · 1 = baton · 2 = claim primary · 3 = priority value · 4 = join, only in a lobby file (5.9.5) · 5 = entry-ID block (5.9.3) |
| 89 | 1 | Reserved, zero |
| 90 | 2 | Heat ID: which baton is meant when several heats exist (Section 5.3) |
| 92 | 4 | Reserved, zero |
| 96 | 8 | Request timestamp |
| 104 | 8 | *x-total*: the declared size of the work |
| 112 | 8 | ID (CTS) of the baton held, or of the last baton held |
| 120 | 8 | Progress *x* |
| 128 | 8 | Time the baton was issued |
| 136 | 8 | Published clock. Written only by the primary. |
| 144 | 8 | The last global commit number this session has seen reconciled. With M19 (5.9.7): the archive's committed sequence that this session's readers use; freed pages are reused only below it |
| 152 | 1 | Number of held entry-ID blocks, at most 21 (5.9.3) |
| 153 | 7 | Reserved, zero |
| 160 | 336 | Held entry-ID blocks (5.9.3): 21 × 16 bytes: store ID (8), first ID of the 1024-ID block (8) |
| 496 | 12 | Reserved, zero |
| 508 | 4 | CRC32C over bytes 0–507 |

### 14.5.3 How the primary's clock is shared

The primary writes its current clock value to offset 136 with every heartbeat. A subordinate computes the primary's time as its own monotonic clock plus the offset it measured the last time it read the primary's slot.

## 14.6 Shared value encoding

This is the single binary encoding for every value in every data model: document fields, table columns, time-series values and delta payloads (Sections 8.1, 8.8 and 9.1). Every value is a 1-byte **type tag** followed by its payload.

### 14.6.1 Type tags

| Tag | Type | Payload |
|---|---|---|
| `0x00` | null | none |
| `0x01` | false | none |
| `0x02` | true | none |
| `0x03` | integer | signed 64-bit, zigzag varint |
| `0x04` | float64 | 8 bytes, IEEE 754 |
| `0x05` | float32 | 4 bytes, IEEE 754 |
| `0x06` | timestamp | 1 precision byte (s, ms, µs, ns), then a fixed 8-byte signed count since the Unix epoch |
| `0x07` | entry reference | store ID varint, entry ID varint (Section 6.1) |
| `0x40` | string | varint length, then UTF-8 bytes |
| `0x41` | binary | varint length, then bytes |
| `0x42` | exact decimal | varint length, then the scale (varint) and the unscaled integer (two's complement, big-endian) |
| `0x43` | array | varint byte length, varint element count, then the elements |
| `0x44` | object | varint byte length, varint field count, width byte, then the fixed-width field directory (14.6.3), then the values |
| `0x45` | vector | varint byte length, element type, dimension count, then the raw elements (Section 7.1) |

- **Integers, lengths and IDs** use LEB128 varints.
- **Timestamps** are a fixed 8 bytes, not a varint, to keep columnar time-series blocks simple (Section 10.3).

### 14.6.2 Skipping unknown tags

Readers can skip types they don't recognize, because every tag falls into one of two ranges:

- **`0x00`–`0x3F`** are fixed-size types. Every reader knows all of them.
- **`0x40`–`0xFF`** are variable-size types, and they **always** begin with a varint length.

A reader that meets an unknown variable-size tag reads the length and skips that many bytes. New types are added only in the variable-size range. Candidates for later include unsigned 64-bit integers, dates without a time, and intervals.

### 14.6.3 Objects, field IDs and the field directory

- **Field dictionaries.** Every document pool and every table keeps a **field dictionary** in the catalog, mapping field names to small numeric field IDs. For an enclave, the dictionary is stored inside the enclave (Section 4.4).
- **Field IDs, not names.** Objects store field IDs. Field names are never repeated inside records.
- **The field directory.** An object's field directory lists *(field ID, value offset)* pairs. It is sorted strictly by field ID.
- **Fixed-width entries.** Directory entries are fixed width, so a reader can binary-search the directory and jump straight to the value it wants, without parsing any other field (Section 8.1).
  - **The width byte.** A width byte comes before the directory:
    - bits 0–1 hold the field-ID width code;
    - bits 4–5 hold the offset width code;
    - all other bits are zero.
  - **Width codes:** 0 = 1 byte, 1 = 2 bytes, 2 = 4 bytes, 3 = 8 bytes.
  - **Byte order.** Entries are stored in the file's byte order.
- **Value offsets.** Each offset is measured from the start of the values area. Values are stored contiguously and in directory order, with no gaps.
- **Canonical form:**
  - each width is the **smallest** that fits the largest field ID, or the largest offset, in that object;
  - an empty object has the width byte `0x00`.
- **Nesting limit.** Arrays and objects nest at most **32** deep, the top-level value counting as one. This is a format limit (17.2): a writer never produces a deeper value, and a reader rejects one.

### 14.6.4 Exact decimals: unbounded or compact

The operator chooses one of two decimal modes for a file. The choice is recorded as a file feature flag.

- **Unbounded.** Decimals have no limit on precision.
- **Compact.** Decimals are limited to 38 digits, so the unscaled value always fits in 128 bits. This mode is optimized for microcontrollers.

A build that supports only compact decimals refuses to open a file in unbounded mode. The default mode comes from the device-class table (Section 2.8).

### 14.6.5 Canonical encoding is mandatory

Content-hash deduplication (Section 6.3) needs every value to have exactly one valid encoding. Writers must produce the canonical form, and readers reject anything that isn't canonical. The canonical form requires:

- varints in their shortest form;
- object directories sorted strictly by field ID, with no duplicate fields;
- NaN encoded as a single canonical bit pattern;
- −0.0 normalized to 0.0;
- strings stored exactly as supplied.

Strings are **not** Unicode-normalized, because that would require shipping Unicode tables, which is too heavy for microcontrollers. As a consequence, a precomposed "é" and an "e" followed by a combining accent are different values.

### 14.6.6 Encoding details

| Item | Rule |
|---|---|
| Timestamp precision codes | 0 = seconds · 1 = milliseconds · 2 = microseconds · 3 = nanoseconds |
| Vector element codes | 0 = f32 · 1 = f16 · 2 = bf16 · 3 = int8 · 4 = binary |
| Binary vectors | Bit *i* is bit *(i mod 8)* of byte *i / 8*, least significant bit first. Unused bits in the last byte must be zero. |
| Vector dimensions | A vector has at least one dimension. Its data length must equal the length implied by its element type and dimension count. |
| Floats in vectors | f32, f16 and bf16 elements follow the same canonical rules as scalar floats: one canonical NaN, and −0 stored as +0. |
| Byte order of fixed fields | float64, float32, timestamp ticks, vector elements and object directory entries all use the file's byte order. Decimals are always big-endian (14.6.1). |
| Decimal zero | Stored as a single byte, `0x00`. |
| Decimal scale | Not normalized. `1.0` (scale 1) and `1.00` (scale 2) are different values, because the scale records the precision the writer supplied. |
| Unassigned fixed-shape tags | Tags `0x08`–`0x3F` that have not been assigned are **malformed**. They have no length prefix, so a reader cannot skip them. |

## 14.7 Anchor map and free-space list

### 14.7.1 The anchor map is a copy-on-write page table

Logical page IDs are allocated densely from a counter. The anchor map is therefore a **multi-level table** indexed directly by logical ID:

- **Leaf pages** hold an array of entries. The entry for a logical ID is found by that ID's position in the array.
- **Interior pages** hold the physical locations of their child pages.
- **Updates are copy-on-write.** A changed leaf, and every page on its path to the root, is written as a new appended page. Existing pages are never modified (Section 1.7). Reconciliation batches these updates.
- **Data pages between commits.** The first change to a logical page after a commit always goes to a new physical page, so the committed version stays intact. A further change before the next commit overwrites that new page in place. No committed state refers to it, which the pager recognises from its sealed sequence number: at or above the pager's sequence at the last commit or open, since sequences are never reused. A page changed many times between commits therefore takes one physical page, not one per change. Pages that compaction moves are always written to new pages.
- **Roots.** The root's location is at header offset 56. The root of the mirror copy is at header offset 472.
- **Entries per page** follow from the page size. For example, a 4 KiB page in a 64-bit file holds about 335 leaf entries.

### 14.7.2 Leaf entry

The entry is 12 bytes in a 64-bit file and 8 bytes in a 32-bit file.

| Size | Field |
|---|---|
| 8 / 4 | Physical location, in units of the smallest page size |
| 1 | Page size (log₂). Used in per-store page-size mode. |
| 1 | State: 0 = unused · 1 = live · 2 = reserved for thin provisioning, with no physical page yet · 3 = released |
| 2 | Extent length in pages, or 0 if the page is not part of an extent |

### 14.7.3 Interior entry

An interior entry is the physical location of a child page: 8 bytes in a 64-bit file, 4 bytes in a 32-bit file.

### 14.7.4 Free-space list

The free-space list is a sorted list of freed physical **extents** that are waiting for compaction (Sections 1.3, 3.4 and 3.7). Each entry is 24 bytes in a 64-bit file and 16 bytes in a 32-bit file:

- the start location;
- the length, in units of the smallest page size;
- the commit sequence at which the extent was freed.

An extent is reclaimed only after the lock state that covers it has been reconciled.

- **Ordering.** The list is kept sorted by start.
- **Merging.** Touching extents merge only if the same commit freed them.
- **The value 0.** A freed sequence of `0` means *reusable now*. Every extent whose freeing commit is older than the latest commit is normalized to 0, and touching extents marked 0 then merge.

### 14.7.5 Where the roots are recorded

- The anchor map's root is at header offset 56.
- The anchor map's mirror root is at header offset 472.
- The free-space list's root and the free-space list's mirror root are kept in a small fixed area at the start of the anchor map's root page.

### 14.7.6 Anchor-map page payload

Every anchor-map page payload starts with an 8-byte node header:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Level: 0 for a leaf; the root is at level depth − 1 |
| 1 | 1 | Flags: bit 0 is set on the root |
| 2 | 6 | Reserved, zero |

The **root** page has a 32-byte area directly after the node header:

| Offset | Size | Field |
|---|---|---|
| 8 | 8 | Next logical ID: the allocation counter; IDs are never reused (Section 4.12) |
| 16 | 8 | Free-space list: root of the primary chain |
| 24 | 8 | Free-space list: root of the mirror chain |
| 32 | 8 | Reserved, zero |

The entries follow: leaf entries (14.7.2) or interior entries (14.7.3).

- **Depth.** The tree's depth is the root's level plus one.
- **Fan-out.** The number of entries per page follows from the page capacity. The root holds fewer entries than other pages, because of its 32-byte area.
- **Adding a level.** When the next logical ID would exceed what the tree can address, a new root is added. The old root becomes child 0 of the new root.

### 14.7.7 Free-space page payload

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Number of extents on this page |
| 4 | 4 | Reserved, zero |
| 8 | 8 | Next page in the chain (physical); 0 at the end |
| 16 | … | Extents (14.7.4) |

At every commit, the whole list is written as a new chain. The pages of the previous chain are freed by that commit.

### 14.7.8 Mirroring and commit order

- **Mirror of the anchor map.** The mirror is a complete second tree.
  - Leaf pages in the mirror hold the same entries as the primary leaves.
  - Interior pages point into their own tree.
  - When one copy of a node fails verification, the other copy is used in its place (Section 3.5).
- **Mirror of the free-space list.** The list is mirrored as a second chain.
- **Commit order.** A commit follows Section 3.2:
  1. barrier;
  2. the changed anchor-map nodes below the root, from the leaves up;
  3. the root's two pages are reserved;
  4. the pages for both free-space chains are allocated, and the chains are written;
  5. the root is written;
  6. barrier;
  7. the header commit.

  Every commit rewrites the root, because the root records the roots of the free-space chains. All chain pages are allocated before any of them is written. Allocating can only shorten the list, so the chains then hold exactly the final list. Spare chain pages are written as empty links.
- **Chain length.** A free-space chain uses at most 128 pages. A longer list needs compaction.
- **Sequence reservation.** Page sequence numbers are reserved in blocks of 1024 (Section 12.1).

## 14.8 Ordered index

### 14.8.1 Structure

The ordered index is a **copy-on-write B+tree** (Section 9.5). A tree is at most **16** levels deep, the root counting as one; this is a format limit (17.2), and a reader treats a deeper tree as corrupt.

- **Interior pages** hold separator keys and child logical IDs.
- **Leaf pages** hold keys together with their values.
- **Updates** append a new copy of the changed leaf and of every page on its path to the root, in the same way as the anchor map (14.7).
- **No sibling links.** Leaves do not point to their neighbours, because with copy-on-write one changed leaf would force its neighbours to be rewritten in a cascade. A range scan instead moves from leaf to leaf through the parent pages.

### 14.8.2 Node header

The node header follows the common page header.

| Size | Field |
|---|---|
| 1 | Level (0 = leaf) |
| 1 | Flags: unique index · prefix compression enabled |
| 2 | Key count |
| 2 | Length of the shared prefix (when prefix compression is enabled) |
| 2 | Offset of the free space |

### 14.8.3 Entries

- **Slot directory.** Entries are reached through a slot directory: an array of 2-byte offsets, kept in key order.
- **Leaf entry.** A key (varint length, then the key bytes), followed by a value. The value is usually an entry reference (store ID, entry ID). It can also be a small inline payload.
- **Interior entry.** A separator key, followed by a child logical ID.
- **Non-unique indexes.** The entry ID is appended to every key, so each key in the tree is distinct.
- **Prefix compression.** Prefix compression can be enabled for each page with a flag. When it is enabled, the prefix shared by every key on the page is stored once.

### 14.8.4 Order-preserving key encoding

Index keys use their own encoding, separate from the value encoding in 14.6. With this encoding, a plain byte comparison (`memcmp`) gives the correct sort order:

- **Type rank.** Each key starts with one type-rank byte, so values of different types sort in a predictable order.
- **Integers** are stored big-endian with the sign bit flipped.
- **Floats** have their bits transformed so that they sort in numeric order.
- **Strings** end with a terminator byte. Any zero bytes inside a string are escaped.
- **Composite keys** are the concatenation of their encoded parts.

### 14.8.5 Collation

- **Binary collation** sorts strings in plain UTF-8 byte order. It is always available, and it is the only collation on microcontrollers.
- **Language-aware collation** is available on hosts that are not microcontrollers. The collation for an index is taken from the **host locale** by default, and the operator can override it in configuration.

### 14.8.7 Node page and entry layout

Index nodes are logical pages of type `0x0003`. A child pointer is a **logical page ID**, so rewriting a node never changes its parent: the pager provides the copy-on-write. The root keeps its logical ID for the whole life of the index.

**Node payload layout:**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Level (0 = leaf) |
| 1 | 1 | Flags: bit 0 unique, bit 1 prefix compression |
| 2 | 2 | Key count |
| 4 | 2 | Shared prefix length |
| 6 | 2 | Offset where the entry heap starts |
| 8 | prefix length | Shared prefix |
| … | 2 × key count | Slot directory: entry offsets from the payload start, in key order |
| … | … | Free space |
| heap … capacity | … | Entries |

**Entries:**

- **Leaf:** suffix length (varint), key suffix, value length (varint), value.
- **Interior:** suffix length (varint), key suffix, child logical ID (8 or 4 bytes, the file's address width). The first entry of every interior node has an empty key, meaning *no lower bound*.

**Rules:**

- **Prefix compression** applies to leaves only. The stored prefix is at most 32 bytes.
- **Splitting.** A node that overflows is split greedily into pieces about three quarters full, at most 16 pieces. The first key of each later piece becomes its separator.
- **Splitting the root.** When the root splits, its contents move to new children, and the root, keeping its logical ID, becomes their parent one level higher.
- **Deleting.** Underfull nodes are not merged. Empty nodes are removed, along with their parent entry. A root left with a single child absorbs that child.
- **Size limits.** A key, and an inline value, may each be at most (capacity − 8) / 8 − 4 bytes, so that any four entries fit in one node.
- **Overflow values.** A longer value (up to 2³² − 1 bytes) is kept in an overflow chain (14.15). Its leaf entry still records the value's full length; since that exceeds the inline limit, the bytes that follow are the chain's first logical ID, at the file's address width, instead of the value. Replacing or deleting the value releases its chain. Keys are always inline.

**Key type ranks (14.8.4):**

| Type | Rank | Encoding after the rank |
|---|---|---|
| null | `0x10` | none |
| false / true | `0x20` / `0x21` | none |
| integer | `0x30` | 8 bytes big-endian, sign bit flipped |
| float | `0x38` | 8 bytes; negative values are inverted, other values have the sign bit set; one NaN, sorted last; −0 = +0 |
| timestamp | `0x40` | 8 bytes big-endian, sign bit flipped |
| string | `0x50` | bytes with `00` escaped as `00 FF`, ended by `00 00` |
| binary | `0x58` | as string |
| reference | `0x60` | store ID and entry ID, 8 bytes each, big-endian |

Decimals have no key encoding yet.

### 14.8.6 Collation is fixed at creation and provided by ICU

- **Fixed at creation.** An index's collation is chosen **once, when the index is created**. The host locale supplies the default, and the operator can override it. The catalog records the collation together with its **collation version**. A host's current locale never changes the collation of an existing index.
- **Collation version changes.** If the collation data changes version, the index is flagged for rebuild and is rebuilt in a maintenance window. Until the rebuild, lookups that would use the index fall back to scans.
- **Microcontrollers.** A microcontroller treats an index with a language-aware collation as unusable and falls back to a scan. The data itself remains fully readable.
- **Source of collation.** Language-aware collation is provided by **ICU**, behind a collation interface. A collation library is not a storage or index engine, so constraint 5 allows it.

## 14.9 Record envelope (documents and table rows)

Documents (Section 8) and rows of row-oriented tables (Section 9) share one **record envelope**. The summary appears only on documents. Sharing the envelope means revisions, history and attribution are implemented once.

### 14.9.1 Envelope

| Size | Field |
|---|---|
| 8 | Entry ID |
| 8 | Revision ID |
| 8 | Parent revision ID (Section 8.14); 0 for the original |
| 8 | Global commit number of this revision |
| 8 | Principal identity ID (Section 8.12) |
| 4 | Schema version (Section 4.3) |
| 1 | Revision kind: 0 = original · 1 = delta · 2 = checkpoint · 3 = tombstone |
| 1 | Flags: has summary · body in overflow (14.15) · signed |
| 2 | Reserved |
| var | Summary: a string value, present only if the "has summary" flag is set (Section 8.2) |
| var | Body: an object value (original or checkpoint), or a patch (delta) |

### 14.9.2 Revisions

- **Delta revisions.** A delta revision stores a **patch**: a list of *(field path, operation, value)* entries, where the operation is set or remove. A field path is a sequence of field IDs, with an array index where needed. Fields that the patch doesn't touch are resolved from earlier revisions, back to the original or the nearest checkpoint (Section 8.6).
- **Finding revisions.**
  - An ordered index keyed by *(entry ID, revision ID)* locates every revision.
  - A second index, keyed by entry ID, points to each entry's **head** revision.
- **Pages.** Records are packed into document or row pages behind a slot directory, in the same way as index leaves (14.8.3).
  - *Current implementation:* each document revision occupies one logical page of type `0x0005`: the page header (13), then the envelope. Envelope flags: bit 0 has summary, bit 1 body in overflow. With bit 1 set, the summary (if any) is followed by the body's total length (8 bytes) and the first logical ID of an overflow chain (address width), then as much of the body as the page holds; the chain (14.15) holds the rest. The summary always stays in the revision page. Packing several revisions per page is deferred.
  - *Size:* the operator's `doc_max` bounds a document body; 0 means the whole revision fits one page. Callers supply buffers of that size, since the core does not allocate. Revisions are history and keep their chains.
- **Index values.**
  - **Head index** (key: entry ID): 21 bytes — head revision ID (8), revision page ID (8), chain length since the last original or checkpoint (4), revision kind (1).
  - **Revisions index** (key: entry ID, revision ID): the revision page ID (8).
  - Both roots and the pool's next entry ID are kept in the store record (14.3.3, fields 12–14). These fields are optional: a record written without them reads as zero.
- **Patch encoding.** A patch is an array value whose elements are arrays `[op, path, value]`:
  - `op` 1 = set (the value is required) · 2 = remove (no value).
  - `path` is an array of field IDs leading through nested objects. Array-index steps are reserved; arrays are currently replaced whole.
  - Setting through a missing field creates the object; setting through a field that is not an object replaces it. Removing a missing field does nothing.
- **Replace.** A replace is stored as a delta holding the top-level differences from the current state.
- **Checkpoints.** A checkpoint is written in place of a delta when the chain length would reach the configured interval (default 16).
- **Summaries.** A revision's summary changes only when the change supplies one. Checkpoints carry the current summary forward.
- **Tombstones.** A delete writes a tombstone revision. Earlier revisions stay readable; the current state reads as not found.

### 14.9.3 Document change payloads

The data-change payloads (14.4.5) for document pools, keyed by store and entry:

| Operation | Payload |
|---|---|
| insert, replace | flags (1 byte: bit 0 = has summary) · if set, summary length (varint) and summary bytes · body (an object value) |
| patch | a patch (14.9.2) |
| delete | empty |

**Applying never fails for logical reasons.** The built-in document apply runs again during recovery, so a change to a missing or deleted document, or to a dropped or non-document store, does nothing instead of failing. A failure there would recur on every reopen. Input is validated when the change is made, inside the transaction.

**Every revision is within the limits.** A change is checked against the document as the transaction sees it, including the transaction's own earlier changes. The resulting document, with the summary it will carry, must fit: envelope and summary in the revision page, and the body there too, or with `doc_max` set, within `doc_max` bytes. So a later checkpoint always fits. Otherwise the change is refused with *overflow* and nothing is journaled. A patch is a delta revision's body and is held to the same limit. A change to a document that is missing or deleted at that point is refused with *not found*. When a replace's patch form would not fit the work buffer, the apply writes the whole document as a checkpoint instead.

### 14.9.4 Document schemas (Section 8.3)

A schema is an object value (14.6), stored as a schema version in the catalog (14.3.3, key `03`).

| Field | Name | Value |
|---|---|---|
| 1 | open | bool: fields the schema does not list are allowed. Default true |
| 2 | fields | array of rules, in strictly ascending field-ID order |

Each rule is an object:

| Field | Name | Value |
|---|---|---|
| 1 | field | int: the field ID the rule constrains. Required |
| 2 | types | int: allowed types, one bit per type (below); 0 = any |
| 3 | required | bool. Default false |
| 4 | max length | int: bytes of a string or binary value, elements of an array |
| 5 | object schema | a schema, applied when the value is an object |
| 6 | element types | int: allowed types of array elements; 0 = any |
| 7 | element schema | a schema, applied to array elements that are objects |

- **Type bits:** 0 null · 1 bool · 2 int · 3 f64 · 4 f32 · 5 timestamp · 6 reference · 7 string · 8 binary · 9 decimal · 10 array · 11 object · 12 vector.
- **Unknown fields** in a schema or rule are refused. A build that does not know a rule never enforces less than the schema says: writes to that pool are refused as unsupported.
- **Clearing a schema** is done by adding the empty schema `{}`, which accepts every document. History is kept (Section 4.11).
- **Not yet defined:** enumerations, numeric ranges, patterns and cross-field rules. They will be new rule fields.
- **Transactions.** A change is validated against the document as the open transaction sees it. The library keeps those states in a working area the application supplies. When the area is full, it reads the transaction's own journal records back instead, so the area never limits a transaction's size.

---

## 14.10 Table pages

### 14.10.1 Row-oriented tables

A row-oriented table is a B+tree clustered by **entry ID**, with the same structure as 14.8. Each leaf value is a record envelope (14.9) that has no summary. Primary keys and other indexes are separate ordered indexes that point to the entry ID.

- **Tables are document pools** with a table schema (Section 9.7), so the head and revision indexes are those of 14.9.2.
- **Row index keys.** Index keys use the order-preserving encoding (14.8.4) of the indexed columns, in order.
  - A unique index stores the key and maps it to the entry ID.
  - A row with a null in any indexed column is still indexed, with the entry ID appended to its key so that nulls never collide (Section 9.9).
- **Permitted type widenings** (Section 9.8): int to decimal, and f32 to f64. More can be added; each must lose no information.
- **Rows** are encoded objects (14.6) holding only their non-null columns, keyed by column field ID. Each revision's envelope records the schema version it was written under, which tells a reader which columns were added after it (Section 9.8).
- **Table schema** (a schema version, 4.11), as an object:
  - field 1: the columns in field-ID order. Each column is an object of field 1 field ID, 2 type (the shared type tags of 9.1), 3 not-null (bool), 4 default (one encoded value, when there is one) and 5 the schema version that added it;
  - field 2: the primary key's field IDs, in key order.
- **Table settings** (the store's settings object, 14.3.3 field 16):
  - 1 keep (0 means every revision; N means the last N);
  - 2 layout (0 means row-oriented);
  - 3 the indexes, each an object of 1 number (the primary key's index is 0), 2 field IDs, 3 unique (bool) and 4 root page;
  - 4 the number the next index gets.
- **Index keys and values.**
  - A key is the indexed columns' values in the order-preserving encoding, a null as the null component.
  - For a non-unique index, or a key that contains a null, the entry ID follows as 8 big-endian bytes.
  - The value is the entry ID (8 bytes, big-endian).
  - Decimal, array, object and vector columns cannot be indexed yet (14.8.4 has no encoding for them).
- **Limits** (M17): 64 columns, 8 indexes, 8 columns per index or primary key, 1 KB of default values in all.

### 14.10.2 Column-oriented tables

- **Row groups.** Rows are stored in **row groups** of N rows. N can be configured, and its default comes from the device-class table.
- **Row-group header.** Each row group has a header page holding:
  - the row count;
  - the entry-ID range;
  - the location and statistics (minimum, maximum, null count) of each column chunk.
- **Column chunks.** One column chunk per column, each made of:
  - an encoding: plain · dictionary · run-length · bit-packed;
  - a compression codec (Section 2.5);
  - a null bitmap;
  - the values.
- **Changes.** Changes arrive through the delta tracks. At reconciliation, each affected row group is rewritten copy-on-write.
- **History.** Columnar tables that have history turned on (Section 9.6) keep that history in a **row-format side store**, using the same envelope as 14.9. Analytic scans stay fast, and the revision trail remains complete.

### 14.10.2.1 Column chunk encodings (current layout)

A column chunk's **values** part (14.10.2) holds the chunk's present (non-null) values, all of the column's type. The null bitmap, the encoding ID and the compression codec (Section 2.5, 14.10.2) sit outside this part and are known to the caller; so are the value count *n* (from the null bitmap) and the column's type tag, so neither is stored in the encoded bytes.

**Items.** Within a chunk, a value is stored as an **item**: the canonical encoding of 14.6 with the tag dropped, since every value in a chunk shares the column's tag.

| Column type | Item bytes |
|---|---|
| int | zigzag varint (14.6, no tag) |
| f64 / f32 | 8 / 4 bytes, file byte order |
| timestamp | precision byte, then 8 bytes, file byte order |
| ref | two varints |
| bool | one byte, 0 = false, 1 = true (the two bool tags share one column type) |
| tag ≥ 0x40 (string, binary, decimal, array, object, vector) | length varint, then that many payload bytes |

Item bytes are equal if and only if the values are equal, since values are canonical. A null value and the reserved fixed tags 0x08–0x3F never appear in a column.

**Encoding IDs** (stored by the caller in the chunk header):

| ID | Name | Bytes |
|---|---|---|
| 0 | plain | the *n* items, one after another |
| 1 | dictionary | varint *k* (distinct items, 1 ≤ *k* ≤ *n*), the *k* items in order of first use, then *n* indexes, each in `bitlen(k − 1)` bits (0 bits when *k* = 1), as one bitstream |
| 2 | run-length | pairs of (varint run length ≥ 1, item) until *n* values are covered; adjacent runs never hold the same item |
| 3 | bit-packed | integer-like columns only (int, timestamp, bool): for timestamp, a precision byte shared by every value; then a zigzag varint of the minimum *m*, a byte *w* (0–64), then the *n* values (*v* − *m*, computed in `uint64`) in *w* bits each. *w* is the bit length of the largest *v* − *m*, so some value equals *m* and some value needs all *w* bits. A timestamp column whose values do not share one precision cannot be bit-packed |

A bitstream is most-significant-bit first (Section 10.3's bit codecs); the bits after the last one are zero, and the stream ends exactly there. None of these forms has a fixed-width offset or length field — every count, run length and the dictionary's *k* is a varint, so a chunk's own byte form never caps it at 4 GiB.

**Choice** (Section 9.12): the chunk is encoded every way that applies, and the smallest result is kept; a tie goes to the lowest ID. An empty chunk (*n* = 0) is plain and zero bytes; any other encoding with *n* = 0 is rejected as not canonical.

**Canonical form** (14.6.5) of the encodings: every set of values has exactly one encoded form per encoding ID. Rejected as not canonical: dictionary entries repeated, out of first-use order, or unused; adjacent runs of the same item; a bit width wider than needed; a minimum no value reaches; non-zero padding bits; trailing bytes. Rejected as corrupt: a run length of 0, a run-length total that disagrees with *n*, an out-of-range index or count, a width over 64, bit-packing on a type that does not support it, a value that wraps below the minimum. An item is checked as deeply as a tag-less item allows: fixed-shape scalars and strings get the same canonical check as a tagged value; array and object elements are themselves full tagged values and are validated recursively, including the container's own canonical rules (ascending field IDs, contiguous offsets, smallest directory widths); binary has no canonical rule beyond its bytes; decimal and vector items get the same canonical bit-pattern checks as a tagged value (shortest two's-complement form and the compact-mode limit for decimal; canonical float bits for vector elements).

### 14.10.2.2 Row groups, headers and chunks (current layout)

**Table settings** (the store's settings object, 14.3.3 field 16) of a column-oriented table: fields 1–4 as in 14.10.1, with 2 layout = 1, and:

| Field | Value |
|---|---|
| 5 | groups index root page |
| 6 | pending rows index root page |
| 7 | most rows in a group (default 1024; at most 65,536) |
| 8 | the **cap**: most value bytes one column may hold in a group (default 64 KiB) |

- **Bounds** (Section 9.12). A group closes at field 7's row count, or earlier when any column's values would pass the cap. The cap must hold the entry-ID chunk: at least 11 bytes per row. A row larger than the cap is refused when it is written, so a group of one row always fits.
- **Roots of 0** in fields 5 and 6 mean table creation stopped part-way. The table then has no columns, and every row call refuses it.

**Groups index.** One entry per group. The key is the group's **last** entry ID (8 bytes, big-endian), so seeking an entry ID lands on the only group that can hold it. Groups cover disjoint, ascending ranges of entry IDs; a range may have gaps. The value is the group header, all big-endian:

| Size | Field |
|---|---|
| 8 | first entry ID |
| 4 | row count (1 to 65,536) |
| 4 | schema version the group was written under |
| 2 | column count |
| | chunk references: the entry-ID chunk first, then one per column in ascending field-ID order |

A **chunk reference**:

| Size | Field |
|---|---|
| 8 | field ID; 0 for the entry-ID chunk |
| 1 | the column's value tag (14.6); a bool column uses the false tag |
| 1 | address length, 8 or 10 |
| 8 or 10 | the chunk's address (14.18.2): a nibblet, or the first page of an overflow chain |
| 4 | the chunk's stored bytes |
| 4 | null count |
| 1 + *n* | minimum: length, then the smallest present value in the shared encoding |
| 1 + *n* | maximum: likewise, the largest |

- **Statistics.** Minimum and maximum are kept only when each is at most **32 bytes**, and only for types with an order (bool, int, f64, f32, timestamp, reference, string, binary). Otherwise both lengths are 0. Values compare as their index keys would (14.8.4); strings and binaries compare by bytes, the shorter first on a tie.
- **Checks on read.** The row count fits the entry-ID range; field IDs ascend; no null count passes the row count; statistics come in pairs, are canonical values of the column's type, and minimum ≤ maximum; the bytes end exactly at the last reference. Anything else is corrupt.

**Entry-ID chunk.** The group's entry IDs, strictly ascending, stored as an int column with no nulls. Position *p* in every column chunk is the row whose entry ID is at position *p* here. Its first and last values equal the header's first entry ID and the key.

**Chunk bytes:**

| Size | Field |
|---|---|
| 1 | encoding ID (14.10.2.1) |
| 1 | codec (Section 15.1): 0 none, 1 LZMA2 |
| ⌈rows / 8⌉ | null bitmap, present only when the null count is not 0: bit *p*, most significant bit first, set when row *p* is null; unused bits zero |
| rest | the present values in the encoding's form (14.10.2.1); with codec 1, that form compressed in the form of 15.2 |

- **Compression** (Section 15.4) is always tried on a chunk's values, and kept only when it saves at least 1/8.
- **Where a chunk lives** (Section 16.1). A chunk of at most a quarter page is a **nibblet of kind 3**. Its back-reference key is the group's last entry ID, then the field ID (8 + 8 bytes, big-endian), which is enough to find and update the reference when the nibblet moves. A larger chunk is an overflow chain of its own (14.15). A chunk stored the other way is rejected as not canonical.

---

## 14.11 Time-series blocks

- **Series lookup.** An ordered index on the **series key** returns the series entry ID. A second ordered index on *(series entry ID, first timestamp)* returns the blocks, so a busy window can span several blocks (14.11.1).
- **Block header:**

| Size | Field |
|---|---|
| 8 | Series entry ID |
| 8 | Window start |
| 8 | Window end |
| 4 | Point count |
| 2 | Number of value fields |
| 1 | Timestamp precision (Section 10.2) |
| 1 | Flags: change-of-value storage in effect · contains late points that were folded in |

- **Timestamp column.** The first timestamp is stored as 8 bytes, followed by a bitstream of delta-of-delta values (Section 10.3).
- **Value columns.** Each value field has its own column, made of:
  - a field ID and a value type;
  - an encoding, which by default depends on the type:

    | Value type | Default encoding |
    |---|---|
    | floats | XOR with the previous value |
    | integers | delta, then zigzag, then bit-packing |
    | strings | dictionary |
    | anything else | plain |

  - a **presence bitmap**, because a point doesn't have to carry every field.
- **Derived and downsampled series** (Section 10.4) are ordinary series.

### 14.11.1 Current layout

- **Store.** A store of model 4 holds any number of series. Its settings object is store record field 16 (14.3.3): 1 late mode (0 accept · 1 accept within the lateness · 2 reject · 3 hold) · 2 lateness (ns) · 3–6 root page IDs of the keys, series, blocks and held indexes. Series IDs are the store's entry IDs (field 14).
- **Indexes** (keys big-endian; a timestamp in a key has its sign bit flipped):

  | Index | Key | Value |
  |---|---|---|
  | keys | series key: the name, then each tag's key and value, tags sorted by key (14.8.4 strings) | series ID (8) |
  | series | series ID | series record |
  | blocks | series ID · first timestamp | block page (8) · point count (4) · last timestamp (8) |
  | held | series ID · timestamp | the point's values, an encoded object |

- **Series record**, big-endian: precision (1) · change-of-value (1) · field count (1) · has points (1) · window in ticks (8) · last timestamp (8) · newest block's key timestamp (8) · that block's encoder state: point count (4), previous timestamp (8), previous delta (8); then per field: ID (8) · type (1) · state (int: previous value, 8; f64/f32: previous bits, 8, and started, 1; string: dictionary size, 4; others: none).
- **Blocks.** One logical page of type `0x0008` per block, with at most 1,024 points of one window: the window is the multiple of the window size at or below a timestamp, and a block never crosses one. The payload:
  - Header, 32 bytes, in the file's byte order: series · window start · window end · point count · field count (2) · precision · flags (bit 0 change-of-value in effect, bit 1 holds late points folded in).
  - Timestamp column: the first timestamp (8), a bit count (4), then the bitstream of the later points.
  - Per field, in ID order: field ID (8) · type (1) · a presence bitmap of one bit per point · then, over the present values: int, f64, f32 and bool: bit count (4) and bitstream; string: dictionary entry count (4), dictionary byte length (4), the entries (varint length and UTF-8 bytes; distinct, in order of first use), bit count (4) and bitstream; any other type: byte length (4) and the values' canonical encodings.
- **Bitstreams** are written most significant bit first; the bits after the last one, to the end of its byte, are zero, and no whole byte is left over.
  - *Bucket code* for an unsigned z, the smallest that fits: `0` (z = 0) · `10` + 7 bits · `110` + 15 bits · `1110` + 31 bits · `1111` + 64 bits.
  - *Timestamps:* each later point codes zigzag(delta − previous delta), wrapping, with the previous delta 0 for the second point.
  - *Integers:* zigzag(value − previous value), wrapping, starting from 0.
  - *Floats:* the first value's bit pattern (64 or 32 bits); then, for x = bits XOR previous bits, `0` when x = 0, else `1`, the count of leading zeros (6 bits; 5 for f32), the significant length − 1 (6 bits; 5), and the significant bits. Patterns are stored exactly, NaNs included.
  - *Booleans:* one bit each. *Strings:* the dictionary index in w bits, where w is the bit length of n, the entries so far; index n adds a new entry.
  - Readers reject any other form: a bigger bucket than needed, wrong float lengths, non-zero padding, an unused or repeated dictionary entry.
- **Change payloads** (14.4.5), keyed by the store and series ID, big-endian. Insert (create a series): precision · change-of-value · window (8) · field count · (field ID (8), type) each · key length (2) · key. Patch (append): a point count (varint), then per point its timestamp (8) and its values (varint length, encoded object). Delete (drop before): a timestamp (8). A batch must fit one journal record and one page.
- **Applying a point.** Newer than the series' last: appended to the newest block when it is in the same window and fits, else a new block. Older or equal: merged into the block of its window that it belongs to, which is rewritten, split in halves as needed, and flagged as holding late points. A point equal to the stored one at its timestamp is stored once; otherwise its fields replace the stored ones field by field, and the collision callback is called. With change-of-value storage, a newer point equal to the last one is not stored.
- **Late points** are checked when the change is made, against the series as the open transaction sees it: reject refuses any point older than the last; accept within refuses one later than the lateness. In hold mode a late point is parked in the held index when applied, unseen by reads, and folded in at reconciliation; dropping removes held points too.

---

## 14.12 Vector blocks and the neighbour graph

### 14.12.1 Vector block

The element type and the number of dimensions are fixed for each collection and recorded in the catalog (Sections 7.1 and 7.2).

| Size | Field |
|---|---|
| 8 | Collection store ID |
| 4 | Vector count |
| 1 | Element type |
| 1 | Flags: cached norms present |
| 2 | Dimension count |
| 8 × count | Entry IDs |
| 4 × count | Cached norms (f32), present only when the flag is set |
| … | Vector data, packed contiguously and aligned to 16 bytes for SIMD |

An ordered index maps each entry ID to *(block logical ID, slot)*.

### 14.12.3 Current layout of a vector collection

- **Blocks.** Vector blocks are logical pages of type `0x0006`, written in the file's byte order. The vector count is the number of slots in the block. A slot whose entry ID is 0 is empty; a block never ends with an empty slot, and a block left with no vectors is released. The vector data starts at the first 16-byte boundary of the page after the entry IDs and norms. Norms are cached when the base measure is cosine.
- **Settings.** An object in store record field 16, written when the collection is created:

  | Field | Value |
  |---|---|
  | 1 | element type (14.6): 0 f32 · 1 f16 · 2 bf16 · 3 int8 · 4 binary |
  | 2 | dimensions (1–65535) |
  | 3 | base measure: 1 cosine · 2 dot product · 3 Euclidean · 4 Hamming (binary only, and the only measure for binary) |
  | 4, 5 | embedding model name and version, strings of at most 64 bytes (Section 7.3) |
  | 6, 7, 8 | root page IDs of the entries, blocks and sources indexes |
  | 9 | bool: blocks cache norms |
  | 10 | K, neighbours kept per entry (1–64), present once a neighbour graph exists (14.12.4) |
  | 11, 12, 13 | root page IDs of the graph, reverse and pending indexes (14.12.4) |
  | 14 | root page ID of the changes index (14.12.5), present once the collection has a change log |

  Fields 10–13 are added when the graph is created, and field 14 when a change log is added to an older collection; the rest never change.

- **Indexes.**

  | Index | Key (big-endian) | Value |
  |---|---|---|
  | entries | entry ID | block ID (8) · slot (4) · source store (8) · source entry (8) · source revision (8) |
  | blocks | block ID | slots in use (4) · slots (4) |
  | sources | source store · source entry · entry ID | empty |

- **Sources.** A vector may name the entry it was computed from. When the source is a document, its head revision at the vector's commit is recorded; a replace (re-embedding) records it again. A search reports each hit's source as *current*, *outdated* (the document has a newer revision), *deleted* (the document or its pool is gone), *unchecked* (the source is not a document) or *none*. Vectors are not removed with their source: references are kept by default (Section 6.7); cascading belongs with link types.
- **Change payloads** (14.4.5). Insert and replace: flags (1 byte; bit 0 = a source follows) · source store and entry (varints) · the vector, an encoded vector value of the collection's type and dimensions. A replace keeps the source. Delete: empty. Patch: advances a named change cursor, keyed by the cursor number, with the new sequence (8, big-endian) (14.12.5). Vectors keep no history.
- **Distances** (smaller is closer): cosine 1 − cos (a zero vector is at distance 1 from everything); dot product −dot; Euclidean the L2 distance; Hamming the number of differing bits. Ties are ordered by entry ID. f16 and bf16 are widened to f32 for arithmetic.
- **Size.** One vector must fit in a block of one page, so the page size bounds the dimensions until per-store page sizes (Section 2.1) exist.

### 14.12.2 Neighbour-graph block (Sections 6.4, 7.4)

One record per entry:

| Size | Field |
|---|---|
| 8 | Entry ID |
| 2 | Neighbour count (≤ K) |
| 1 | Flags: cross-collection neighbours present |
| 1 | Version of the base measure |
| per neighbour | Neighbour reference (store ID only when cross-collection, then entry ID) and distance (f32) |

**Entry points.** Approximate search begins from a set of **entry points** chosen to cover the collection. A graph of exact nearest neighbours doesn't always lead the search everywhere: on clustered data it falls apart into one piece per cluster, with no links between them. Starting from points that cover the collection solves this. They are stored with the graph (14.12.4).

### 14.12.4 Current layout of the neighbour graph

- **Creation.** A collection gets a graph by an explicit call that names K (default 16, at most 64; a record of K neighbours must fit one index value). Every vector already there is queued. Settings fields 10–13 (14.12.3) record it.
- **Indexes.**

  | Index | Key (big-endian) | Value |
  |---|---|---|
  | graph | entry ID | record: entry ID (8) · neighbour count (2) · flags (1, 0: no cross-collection neighbours yet) · measure version (1, currently 1) · per neighbour: entry ID (8) · distance (f32 bits, 4) |
  | graph | 0 | live vector count (8) |
  | reverse | neighbour ID · entry ID | empty: *entry* lists *neighbour* |
  | reverse | 0 · entry ID | kind (1): *entry* is an entry point, 1 spread or 2 reach |
  | pending | entry ID | empty: the entry's list must be worked out again |

  All values are big-endian. A list holds the entry's K nearest other vectors under the base measure, ordered by distance and then entry ID: it is exact once the entry is no longer pending. Entry ID 0 is never used, so the rows under 0 can't be mistaken for an entry's.
- **Commits only queue work.** An insert adds the entry to *pending*. A replace takes the entry out of the graph (its record, its reverse rows, and itself from every list that holds it) and queues it, and every entry that listed it. A delete does the same without queuing the entry itself, and drops its entry-point row. The live count follows inserts and deletes.
- **Refresh** (Section 6.8). Pending entries are refreshed in batches that fit the caller's working memory. A batch reads every vector once: it builds each batch entry's list exactly, and offers each batch vector to the lists of the other entries, which keep their K nearest. Reconciliation refreshes up to a budget of entries (default 256); a maintenance call refreshes any number. Refresh writes only pages, inside the reconciliation or a commit of its own.
- **Entry points** are of two kinds, and both are reviewed after every refresh. A query measures every entry point.
  - **Spread** (kind 1) are chosen again each time. There are about √live of them, at least 8, at most 1024 and no more than the working memory holds. One pass over the blocks keeps each vector farther than a radius r from those already chosen, under Euclidean distance (Hamming for binary vectors); when too many are chosen, r grows to the distance of the closest pair of those chosen, and they are thinned to points farther than r apart. Only spread rows are added or removed by this choice.
  - **Reach** (kind 2) cover the pieces the spread points miss: an exact neighbour graph can fall apart into more pieces than there are spread points. A refresh checks the entries it touched: each refreshed entry, each entry it dropped from a list or from a list it rewrote, and every reach entry point. A check walks from the other entry points with the entry's own vector and stops once a list leads to it. An entry the walk doesn't reach becomes a reach entry point; a reach entry point the walk does reach stops being one. Pending entries are not checked, since searches measure them directly. When the entries to check don't fit the working memory, every entry with a list and every entry point is checked.
- **Approximate search** (Section 7.4). A best-first walk under the base measure starts from the nearest entry points and keeps the *ef* best vectors found (default 64, at least the number asked for). Pending vectors are not in the graph, so each one is measured directly. What was found is ranked with the requested measure.
- **Neighbour lookup.** Returns an entry's stored list, and whether it is current (the entry is not pending).

### 14.12.5 Change log (Section 18.12)

- **Creation.** A collection created with this layout has a change log from the start. An explicit call adds one to a collection created before it. Settings field 14 (14.12.3) is the root page ID of the changes index, present once the collection has a log.
- **Feature flag.** Creating the first change log in a file sets read-only-compatible feature flag 0, *vector change logs* (14.1.10). A build that does not know the flag opens the file read-only (1.2), so no writer that would not keep a log ever writes to a file that has one. The flag is never cleared.
- **Index.**

  | Index | Key (big-endian) | Value |
  |---|---|---|
  | changes | sequence · entry ID | kind (1): 1 inserted or replaced, 2 deleted |
  | changes | 0 · 0 | trimmed-through sequence (8, big-endian): the log holds every change after it |
  | changes | 0 · cursor number | a named cursor: read-up-to sequence (8, big-endian) · name (1–64 bytes, UTF-8) |

  Entry ID 0 and sequence 0 are never used, so the rows under sequence 0 can't be mistaken for changes.
- **Named cursors** (Section 18.12). Cursor numbers start at 1; a new cursor takes one above the highest in use, and names are unique within a collection. Opening a cursor sets its sequence to the collection's change stamp. Opening and dropping one are catalog changes (4.7), so each commits at once.
- **Advancing a cursor** is a data change in a transaction (14.4.5): the *patch* operation on the collection, keyed by the cursor number as its entry ID, with the new sequence (8, big-endian) as its payload. It reaches the index when it is applied, and reconciliation applies every replayed change before it trims, so the log is never trimmed past an advance. A cursor's sequence never moves backwards: an advance to a lower sequence, or of a cursor that no longer exists, changes nothing.
- **The sequence.** A change is keyed by the committed sequence of the archive commit that makes it durable: the committed sequence of the archive the change is applied to, plus one. A process whose archive moves before then rebases and applies the change again (5.9.1), so its key follows. Two changes to one entry under one sequence share a row, and the later one's kind stands.
- **The change stamp** of a collection (18.6) is the sequence of its newest row, or the trimmed-through sequence when the log has no change rows.
- **Trimming.** At reconciliation, the baton holder deletes the change rows whose sequence is at or below the trim point, and raises the trimmed-through sequence to it. The trim point is the lower of the readers' floor (5.9.7) and the lowest named cursor's sequence.
- **Reading.** Changes after a sequence *s* are the rows whose key is above *s* · the largest entry ID. They are complete only when *s* is at or above the trimmed-through sequence.
- **Readers without the accelerator** need not read the log. Every writer keeps it up to date.

---

## 14.13 Links and neuron weights (Section 6)

Links, foreign keys and neuron mapping all use this layout.

| Size | Field |
|---|---|
| var | Source reference (store ID, entry ID), or an opaque pointer in a joined store |
| var | Target reference |
| 8 | Link-type ID, a catalog object ID like any other (Section 4.12). The link type, its cardinality and its delete behaviour are defined in the catalog (Sections 6.7 and 6.9). |
| 4 | Weight (f32). Used by neuron mapping; 1.0 for plain links. |
| 8 | Time the link was last reinforced (Section 6.10) |
| 1 | Flags: tombstone |

Every link is stored in **two** ordered indexes: one keyed by *(source, link type, target)* and one keyed by *(target, link type, source)*. Links can then be followed efficiently in both directions.

### 14.13.1 Current layout

- **Link types** are catalog objects of kind 2 (14.3.3), named in key `02` like stores, with a record of: 1 kind · 2 ID · 3 name · 4 cardinality (1 = 1:1, 2 = 1:N, 3 = N:M) · 5 enforced (1: checked on every write; 0: recorded only, Section 6.9) · 6 on delete (0 tombstone, 1 cascade, 2 refuse).
- **The link store.** Every link lives in one store of model 5, named `$links` (names starting with `$` are the engine's own). It is created with the first link type. Its record holds the forward index root in field 12 and the reverse index root in field 13.
- **Indexes.** A reference is store ID and entry ID, 8 bytes each; keys are big-endian.

  | Index | Key | Value |
  |---|---|---|
  | forward | source (16) · type (8) · target (16) | weight (f32 bits, 4) · time (8, ns since the Unix epoch) · flags (1) · removed count (4) |
  | reverse | target (16) · type (8) · source (16) | empty |

  Flags: bit 0 tombstone · bit 1 the source is gone · bit 2 the target is gone.
- **Change payloads** (14.4.5), keyed by the link store with entry 0, all big-endian. LINK: mode (1: 1 create, 2 reinforce) · type · source · target · weight (f32 bits) · time. UNLINK: type · source · target. The time is the environment's clock when there is one, else the caller's.
- **Checks when a change is made**, as the open transaction sees things (its own records included): both ends exist (documents and vectors so far); the pair is not linked under the type, and its link was not tombstoned with an end gone (a removed one may be revived); an enforced 1:N type allows each target one live source, and 1:1 also each source one live target. Reinforce and remove need a live link. A delete is refused while a live refuse-type link points at the entry.
- **Applying.** Create adds both rows when the pair has none; a link removed on purpose (tombstone flag, neither end-gone flag) is revived instead: flags cleared, weight and time set, removed count up by one. Reinforce sets the weight and time of a live link; remove (UNLINK) tombstones it. Applying never fails for logical reasons.
- **Deletes and drops.** After an entry's delete is applied, each of its links, in either direction, is tombstoned and flagged with the end that is gone; a cascade type erases the link instead, and a refuse-type link found then is tombstoned. Dropping a store tombstones every link with an end in it, whatever the type, in the drop's own commit.

### 14.13.2 Joined stores (12.9)

A joined store keeps the links between domains in a link store of its own (model 5, `$links`), recorded in its own catalog (14.3.5) and written in its domain like any of its pages and records.

- **Cross-domain link types** are kind-2 records in the joined store's catalog with two more fields: 7 the joined store's ID, 8 the member domains (an array of domain IDs; 0 is the public domain). Defining one adds key `07` + the joined store's ID to the catalog of each member domain that has no such key yet, in the same archive commit.
- **Ends.** Every end is 40 bytes, big-endian:

  | Size | Public end | Enclave end |
  |---|---|---|
  | 8 | domain ID, 0 | the enclave's domain ID |
  | 32 | store ID (8) · entry ID (8) · 16 zero bytes | tag: HMAC-SHA-256 under the enclave's index subkey of (joined store ID ‖ store ID ‖ entry ID), each 8 bytes big-endian |

- **Indexes.**

  | Index | Key | Value |
  |---|---|---|
  | forward | source end (40) · type (8) · target end (40) | weight (4) · time (8) · flags (1) · removed count (4), as in 14.13.1; then, for the source if it is an enclave end, the key version its tag and sealed reference were made under (4) and the sealed reference (32); then the same for the target |
  | reverse | target end (40) · type (8) · source end (40) | empty |

- **Sealed reference**, 32 bytes: the 16-byte reference (store ID · entry ID, big-endian) encrypted with the file's AEAD algorithm (12.1) under the enclave's seal subkey (12.2), followed by the 16-byte tag. The nonce is the first 12 bytes of HMAC-SHA-256, under the enclave's seal nonce subkey, of (joined store ID ‖ reference). The associated data is the joined store's ID (8 bytes, big-endian). Because the nonce covers both the associated data and the plaintext, two sealings share a nonce only when they are identical.
- **Change payloads** are those of 14.13.1, with 40-byte ends, each enclave end followed by its sealed reference (32). The records are the joined store's, so they are encrypted and hide their IDs (14.4.2).
- **Rotation (12.4).** A lookup from an enclave end computes its tag under every key version of that enclave still in use. Compaction re-makes ends and sealed references made under an older version: it deletes the forward and reverse rows and writes them again under the current version, in one commit.
- **Checks and applying** follow 14.13.1. An enclave end's existence is checked in its enclave before the change is journaled; at apply, the tag and sealed reference are taken from the change as they are.

---

## 14.14 Full-text index (Section 8.10)

- **Term dictionary.** An ordered index keyed by term bytes. Each entry holds the term's document frequency and the location of its postings list.
- **Postings.** Postings lists are stored in blocks. Each block holds:
  - entry references, delta-encoded as varints;
  - the term frequency for each entry;
  - optionally, the positions of the term within the entry.
- **Ranking.** Ranking is BM25 by default. Each entry's field length is stored, because BM25 needs it.
- **Tokenizing.**
  - On hosts that are not microcontrollers, text is split into words and case-folded using **ICU**.
  - On microcontrollers, text is split on UTF-8 whitespace and punctuation, and only ASCII is case-folded.
  - The core does **no stemming**. Stemming can be added later as an extension.
  - Storing positions, which phrase search needs, can be switched on per index.

### 14.14.1 Current layout

The structures above are built from three ordered indexes (14.8). Prefix-compressed leaves of the postings index serve as the postings blocks, and a term's postings are found by its key prefix rather than a stored location.

| Index | Key | Value |
|---|---|---|
| terms | term (key string, 14.8) | document frequency (4 bytes) |
| postings | term, then entry ID (8 bytes, big-endian) | term frequency (4 bytes) |
| lengths | entry ID | words indexed for the entry (4 bytes) |
| lengths | 0 | live documents (8) · words indexed in all (8) |

- **Settings.** One full-text index per pool. Its settings are an object in store record field 15 (14.3.3):

  | Field | Value |
  |---|---|
  | 1, 2, 3 | root page IDs of the terms, postings and lengths indexes |
  | 4 | splitter: 1 = simple (below). A build without the named splitter refuses the pool |
  | 5 | bool: the summary is indexed |
  | 6 | array of paths; each is an array of field IDs through nested objects (at most 8 paths, 8 deep) |

- **What is indexed.** A path that ends at a string indexes it; one that ends at an array indexes the strings directly inside it. Other values, and paths that do not exist, contribute nothing.
- **Simple splitter (1).** Words are broken on ASCII characters other than letters and digits, on U+0080–U+00BF (except ª, µ, º), ×, ÷, U+2000–U+206F, U+3000–U+303F and U+FEFF. ASCII letters fold to lower case. Words are cut to 64 bytes on a character boundary. Malformed UTF-8 bytes count as word characters. The ICU splitter will be a second splitter ID; an index changes splitter only by being rebuilt.
- **Updates.** The index changes in the document apply, from the document's state before and after the change, so it is always consistent with committed documents and is rebuilt by replay. A deleted document leaves the index. Every word is indexed: the working area for words must hold two pages' worth (one per document version), and a document with its summary always fits in one page (14.9.3), so it can never run out. A pool whose configured area is smaller is refused.
- **Creation.** Creating the index on a pool that has documents indexes them in the same commit.
- **Search.** A query is split by the same splitter, has at most 16 distinct words, and matches documents containing *any* or *all* of them. Scores are BM25 with k1 = 1.2 and b = 0.75 and idf = ln(1 + (N − df + 0.5) / (df + 0.5)). Equal scores are ordered by entry ID.
- **Not yet:** positions and phrase search, ICU, per-field weights, dropping an index.

---

## 14.15 Blobs and overflow pages

- **Overflow chain.** An overflow page holds the logical ID of the next page in the chain (or 0 at the end), followed by a payload. A value that doesn't fit in its page continues along this chain.
  - *Current implementation:* overflow pages have type `0x0007` and the overflow-continuation header flag (bit 2), and belong to the store of the value they hold. The next page's ID is written at the file's address width, in the file's byte order. Every page but the last is full; the owner records the value's total length and the chain's first page, so a reader checks each page's type, store and payload length against what the chain still needs. Chains are logical pages like any other, so an uncommitted chain disappears with its change. Used by index values (14.8.7) and document bodies (14.9.2).
- **Blobs.** Attachments (Section 8.4) are stored as **extents** of contiguous pages wherever possible. A blob record holds:
  - the content hash;
  - the total length;
  - the logical ID of the extent;
  - its length in pages.
- **External attachments.** The record holds:
  - a path relative to the archive;
  - the size;
  - the content hash (Section 8.11);
  - whether the file is encrypted.
- **Content-hash algorithm.**
  - The default is **SHA-256**. It is supported everywhere, and ESP32 has hardware for it.
  - **Inside enclaves**, hashes are computed as HMAC-SHA-256 under the enclave's hash subkey (Section 12.2).
  - The algorithm ID is recorded in the file header at offset 480 (14.1.4), so that a faster hash such as BLAKE3 can be added later.

---

## 14.16 Dedup index

There is one dedup index per encryption domain (Section 1.6). It is an ordered index keyed by content hash. Each entry holds:

- the location of the canonical copy;
- a **reference count**;
- in an enclave or joined store, the **key version** whose hash subkey made the keyed hash (12.4). After a rotation, a lookup hashes under every version still in use, and compaction re-keys entries made under older ones.

Reference counts are updated at reconciliation. When a count reaches zero, the space is passed to the free-space list (14.7.4).

---

## 14.17 Validation map (Section 13.5)

The validation map belongs to the **compatible** page class.

| Size | Field |
|---|---|
| 1 | Region size, as log₂ of the number of pages covered by each bit |
| 8 | Base location |
| 8 | "Verified through" watermark |
| 8 | Time the last full scrub finished |
| … | Bitmap: 1 = verified since the last change |

---

## 14.18 Nibblet pages (Section 16)

### 14.18.1 Page layout

A nibblet page is a logical page of type `0x0009`. Its header names store 0, because the page is shared (16.2). Payload, in the file's byte order:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Encryption domain ID (0 = public) |
| 8 | 2 | Slot count |
| 10 | 2 | Lowest record offset: records occupy this offset up to the payload's end |
| 12 | 2 | Free bytes, including the gaps left by removed records |
| 14 | 4 × slots | Slot directory: record offset (2) and record length (2); length 0 marks a free slot |

- **Records** are packed from the end of the payload toward the directory. A record holds, in order:
  - the owner's store ID (8 bytes);
  - the record kind (1 byte; Section 16.5);
  - the owner key's length (1 byte) and the owner key, the key under which the owner's index refers to the record;
  - the record itself, such as a revision's stored form (14.9.1) or a time-series block payload (14.11.1).

  The first three make the back-reference (16.2).
- **Compaction.** Gaps are closed by moving records and rewriting their offsets; slot numbers never change. A freed slot is reused by the next record placed in the page.

### 14.18.2 Record addresses

Every owner refers to a record by a page ID, and adds a 2-byte slot when the record is a nibblet. For time-series blocks (14.11.1), the blocks index value gains the slot after its page ID, and the value's length tells the two forms apart. For revisions:

- **Revisions index value** (14.9.2):
  - 8 bytes: a revision on its own page (the logical page ID);
  - 10 bytes: a nibblet (the page ID, then a 2-byte slot).
- **Head record:**
  - 21 bytes: the head revision is on its own page;
  - 23 bytes: the same fields, then the head's 2-byte slot.

  Older files hold only the 8-byte and 21-byte forms, and both forms stay valid.

### 14.18.3 Free-space index

- **Where it lives.** The public domain's free-space index is an ordered index whose root is recorded in the catalog root slot (14.3.2, offset 96), beside the public dedup and audit roots. It is created with the first nibblet, which can happen during an apply or a recovery replay, and its root reaches the archive with the next commit. Enclave and joined-store domains record theirs in their enclave root pages (14.3.4), beside their dedup roots.
- **Keys** are big-endian: (domain ID 8, free bytes 2, page ID 8). The value is empty.
- **Best fit** seeks to (domain, record size, 0) and takes the first key in that domain. Every placement, removal and merge updates the page's key.

## 14.19 Structured query wire format (CMBQL core)

A query is encoded as a compact binary tree of typed operator nodes and expressions (spec 11.7, 11.10). It is the canonical representation exchanged over the wire, prepared in the catalog, and executed by the streaming cursor engine.

### 14.19.1 Header

| Offset | Length | Description |
|---|---|---|
| 0 | 4 | Magic bytes: `0x43 0x4D 0x42 0x51` (`CMBQ`) |
| 4 | 1 | Format version (`0x01`) |
| 5 | 1 | Flags (reserved, `0x00`) |
| 6 | var | Root plan node |

### 14.19.2 Operator node tags (`0x01`–`0x1F`)

Every operator node begins with a 1-byte kind tag, followed by its node-specific fields. Sub-nodes are embedded recursively.

| Tag | Name | Fields |
|---|---|---|
| `0x01` | `Scan` | `store_name` (varint len + UTF-8 bytes) |
| `0x02` | `Filter` | `input` (node), `predicate` (expression) |
| `0x03` | `Project` | `passthrough` (uint8: 0 or 1), `count` (varint N), `input` (node), N outputs: `[alias` (varint len + UTF-8), `expr` (expression)`]` |
| `0x04` | `Join` | `kind` (uint8: 1 = Inner, 2 = LeftOuter), `left` (node), `right` (node), `condition` (expression) |
| `0x05` | `Sort` | `count` (varint N), `input` (node), N sort keys: `[expr` (expression), `dir` (uint8: 1 = ASC, 2 = DESC), `nulls` (uint8: 1 = First, 2 = Last)`]` |
| `0x06` | `Limit` | `count` (varint), `offset` (varint), `input` (node) |
| `0x07` | `Union` | `count` (varint N), N branches: `[node]` |
| `0x08` | `Aggregate` | `groups_count` (varint G), `aggs_count` (varint A), `input` (node), G group expressions: `[expr]`, A aggregates: `[alias` (varint len + UTF-8), `fn` (uint8: 1 = COUNT, 2 = SUM, 3 = AVG, 4 = MIN, 5 = MAX), `arg` (expression)`]` |
| `0x09` | `Traverse` | `link_type` (varint len + UTF-8), `dir` (uint8: 1 = Forward, 2 = Reverse, 3 = Both), `min_hops` (varint), `max_hops` (varint), `input` (node) |
| `0x0A` | `TextSearch` | `mode` (uint8: 1 = ANY, 2 = ALL), `limit` (varint), `query` (varint len + UTF-8), `fields_count` (varint F), `input` (node), F field names: `[varint len + UTF-8]` |
| `0x0B` | `VectorSearch` | `field` (varint len + UTF-8), `k` (varint), `metric` (uint8: 0 = Default, 1 = Euclidean, 2 = Cosine, 3 = Dot), `input` (node), `near` (expression) |
| `0x0C` | `HybridRank` | `fuse` (uint8: 1 = RRF), `count` (varint N), N branches: `[node, weight` (uint32 scaled by 1000)`]` |
| `0x0D` | `Window` | `type` (uint8: 1 = Tumbling, 2 = Sliding, 3 = Hopping), `time_field` (varint len + UTF-8), `size_ns` (varint uint64), `step_ns` (varint uint64), `input` (node) |
| `0x0E` | `AsofJoin` | `kind` (uint8: 1 = Backward, 2 = Forward, 3 = Nearest), `tolerance_ns` (varint uint64), `left` (node), `right` (node), `left_time` (expression), `right_time` (expression), `match_cond` (expression) |
| `0x0F` | `Unnest` | `item_alias` (varint len + UTF-8), `index_alias` (varint len + UTF-8, empty if omitted), `input` (node), `array_expr` (expression) |

### 14.19.3 Expression tags (`0x20`–`0x3F`)

Expressions are trees embedded within operator nodes.

| Tag | Name | Fields |
|---|---|---|
| `0x20` | `Literal` | `len` (varint), encoded `cdb_value` bytes (14.6) |
| `0x21` | `Field` | `path` (varint len + UTF-8; nested paths joined by `0x1F` Unit Separator) |
| `0x22` | `Param` | `kind` (uint8: 1 = Index, 2 = Named), `index` (varint uint32) or `name` (varint len + UTF-8) |
| `0x23` | `Unary` | `op` (uint8: 1 = NOT, 2 = NEG, 3 = IS_NULL, 4 = IS_NOT_NULL), `operand` (expression) |
| `0x24` | `Binary` | `op` (uint8: 1 = AND, 2 = OR, 3 = EQ, 4 = NE, 5 = LT, 6 = LE, 7 = GT, 8 = GE, 9 = ADD, 10 = SUB, 11 = MUL, 12 = DIV, 13 = MOD, 14 = LIKE, 15 = CONTAINS), `left` (expression), `right` (expression) |
| `0x25` | `Call` | `fn_id` (varint uint16), `args_count` (varint N), N arguments: `[expression]` |

## 14.20 Stored procedure bytecode ISA (CMBQL compiled form)

Stored procedures compile to a compact, register-based bytecode stored in the catalog (spec 4.5, 11.2, 11.7). The virtual machine is sandboxed, performs zero heap allocation, and enforces instruction execution budgets.

### 14.20.1 Header

| Offset | Length | Description |
|---|---|---|
| 0 | 4 | Magic bytes: `0x43 0x4D 0x42 0x50` (`CMBP`) |
| 4 | 1 | Format version (`0x01`) |
| 5 | 1 | Flags (reserved, `0x00`) |
| 6 | 1 | Register count $R$ (1–64; registers $R_0 \dots R_{R-1}$) |
| 7 | 1 | Parameter count $P$ ($P \le R$) |
| 8 | var | Bytecode length (varint bytes) |
| var | var | Bytecode instruction stream |

### 14.20.2 Instruction opcodes

Every instruction starts with a 1-byte opcode:

| Opcode | Name | Operands | Description |
|---|---|---|---|
| `0x00` | `NOP` | none | No operation |
| `0x01` | `LOAD_CONST` | `dst` (uint8), `len` (varint), `bytes` | Loads encoded `cdb_value` literal into $R_{dst}$ |
| `0x02` | `LOAD_PARAM` | `dst` (uint8), `idx` (uint8) | Loads procedure parameter $idx$ into $R_{dst}$ |
| `0x03` | `MOVE` | `dst` (uint8), `src` (uint8) | Copies $R_{src}$ into $R_{dst}$ |
| `0x04` | `JUMP` | `rel_off` (int16 big-endian) | Unconditional relative branch |
| `0x05` | `JUMP_IF_TRUE` | `cond` (uint8), `rel_off` (int16 big-endian) | Branch if $R_{cond}$ is truthy |
| `0x06` | `JUMP_IF_FALSE` | `cond` (uint8), `rel_off` (int16 big-endian) | Branch if $R_{cond}$ is falsy or NULL |
| `0x07` | `CALL_MATH` | `dst` (uint8), `fn_id` (uint16 big-endian), `argc` (uint8), `args` ($argc \times$ uint8) | Dispatches math/DSP function; result into $R_{dst}$ |
| `0x08` | `CALL_MUTATION` | `dst` (uint8), `op` (uint8), `store` (uint8), `entry` (uint8), `payload` (uint8) | Invokes engine mutation; returns its result or status into $R_{dst}$ (11.12.3) |
| `0x09` | `OPEN_CURSOR` | `slot` (uint8), `len` (varint), `wire` | Opens query plan cursor in $slot$ using wire bytes |
| `0x0A` | `CURSOR_NEXT` | `has_row` (uint8), `row_dst` (uint8), `slot` (uint8) | Fetches next row from cursor slot |
| `0x0B` | `CURSOR_CLOSE`| `slot` (uint8) | Closes cursor in slot |
| `0x0C` | `RETURN` | `src` (uint8) | Returns $R_{src}$ and halts |
| `0x0D` | `HALT` | none | Halts procedure execution |
| `0x0E` | `SAVEPOINT` | `id` (uint8) | Sets savepoint $id$ in the open transaction (11.12.3) |
| `0x0F` | `ROLLBACK_TO` | `id` (uint8) | Undoes the transaction's changes since savepoint $id$; the savepoint stays set |
| `0x10` | `RELEASE_SP` | `id` (uint8) | Forgets savepoint $id$ and every later one, keeping their changes |
| `0x11` | `CALL_MUTATION_EX` | `dst` (uint8), `op` (uint8), `argc` (uint8), `args` ($argc \times$ uint8, at most 16) | As `CALL_MUTATION`, with any number of argument registers |

## Open questions (do not specify until answered)

None at present.

## 14.21 Replication (Section 19)

**Withdrawn pending redesign.** Section 19 is being redesigned (its open questions); the layouts below record the last proposal and are not to be implemented.

Multi-byte fields in this section are big-endian, in every file and on the wire, so that copies of any byte order exchange the same bytes.

### 14.21.1 Group record

Each copy keeps one **group record**, a public catalog object of kind 8 (14.3.3) with no name, found through catalog root slot offset 112. It is the copy's own state and is never replicated. Fields 1, 2 and 10 as in a store record, then:

| ID | Field |
|---|---|
| 34 | copy number (0–65,535) |
| 35 | copy numbers this copy may still hand out: an object `{1: lowest, 2: highest}`, empty when none are left |
| 36 | group limit (19.1) |
| 37 | hybrid clock: an object `{1: physical, ns since the Unix epoch, 2: logical}` |
| 38 | version vector: an array of objects `{1: copy number, 2: epoch, 3: physical, 4: logical}`, one per origin branch (19.4), at most the group limit |
| 46 | epochs: an ordered index keyed by copy number (2) then epoch (8), valued the parent epoch (8): every epoch this copy has seen, for telling a chain from a fork |
| 47 | this copy's current epoch (8) and the epoch it continues from (8) |
| 39 | member vectors root: an ordered index keyed by member copy number (2) then origin copy number (2), valued physical (8) · logical (4): each member's vector as it last reported it, for purging tombstones (19.6). Key (member, 0xFFFF) holds the last message sequence received from that member (8) |
| 40 | retired copies: an array of copy numbers |
| 41 | principal: the ID of the principal (14.3.7) whose key signs this copy's slices |
| 42 | drift bound in ns (19.4); 0 means the default |
| 43 | group ID: the first copy's database ID (16 bytes); every copy of the group has it, while each copy has a database ID of its own (14.1) |
| 44 | message sequence: the last sequence number this copy put in a message (14.21.4) |
| 45 | signing key reference: where the application's keystore holds this copy's principal private key (12.3); the key itself is never in the file |

- **The first copy** is copy 0 and may hand out 1–65,535. A database that never replicated has allocated its IDs with 0 in their top 16 bits (19.2), so they are already copy 0's.
- **Creating a replica** hands the new copy the upper half of the creator's remaining range: the new copy takes the lowest number of that half as its own and may hand out the rest. The new copy's file is written from the creator's state, with a new database ID of its own and the group's ID in field 43.
- **Persistence through the journal.** The group record is written only by reconciliation, never inside a transaction. A transaction that writes replicated stores or imports changes journals one **replication record** (kind 7, 14.4.2) after its last data record and before its commit record. Its payload, big-endian: the copy's clock after the transaction (physical 8 · logical 4), then a 2-byte count and, per origin the version vector advances to, copy number 2 · physical 8 · logical 4. Reconciliation folds the replication records of the transactions it merges into the group record's fields 37 and 38, in the same archive commit; recovery replays them with their transactions. Rollbacks never drop a replication record, because it is written at commit, after them.
- **Creating a replica** writes the creator's group record first, with its range already halved, in an archive commit, and only then writes and syncs the new copy's file. A crash between the two loses copy numbers, which is safe; it never gives two copies the same numbers.
- **Reach.** Halving lets a copy create at most 16 replicas itself; larger groups grow by replicas creating replicas, up to the group limit.

### 14.21.2 Replicated stores

A replicated store's record has field 23 (14.3.3): an object `{1: clock index root, 2: conflict list root, 3: next conflict number}`. Marking a store replicated later writes a clock entry for every live path of every entry, at the clock time of the marking.

- **Clock index.** An ordered index. Key: entry ID (8), then the path, one field ID (8) per level; an empty path stands for the whole entry. Value: physical (8) · logical (4) · copy number (2) · epoch (8) · flags (1, bit 0 tombstone).
- **Link adds.** In a replicated store's links, the forward row's value is followed by a 2-byte count and the identities of the link's live adds (copy number 2 · epoch 8 · physical 8 · logical 4 · ordinal 2).
- **Conflict list** (19.5). An ordered index keyed by conflict number (8). Value, an object: `{1: index number, 2: the key's bytes, 3: entry that kept the key, 4: entry set aside, 5: winner's clock time, 6: set-aside entry's clock time}`. Resolving a conflict removes its key.

### 14.21.3 Replication-clock series

A time-series store of the engine's own, `$replclock`, holds one series per copy number. Each point, at the time of the handshake that brought it: `{1: estimated offset of that copy's clock from this copy's, ns (signed), 2: error bound, ns, 3: copy number that measured it}`. A copy measures a sender's offset as the sender's wall-clock reading minus its own clock at receipt; the error bound is the round trip when the transport reports one, else the configured drift bound.

### 14.21.4 Wire messages

Every message starts with:

| Size | Field |
|---|---|
| 4 | Magic: `43 44 42 52` (`CDBR`) |
| 1 | Wire version: 1 |
| 1 | Message type: 1 handshake · 2 slice |
| 16 | Group ID (14.21.1 field 43), shared by every copy |
| 2 | Sender's copy number |
| 8 | Sender's principal ID |
| 8 | Message sequence: one more than the sender's previous message |
| 8 | Sender's wall clock at sending, ns since the Unix epoch |

and ends with the sender's signature: a 2-byte length, then an ECDSA P-256 signature over SHA-256 of every byte before the length (ASN.1 DER). A receiver refuses a message whose principal is unknown or retired, whose signature does not verify, or whose group ID is not its own. A message whose sequence is not above the last one received from that sender is a replay: its clock reading and offsets are ignored, and its changes are filtered as any others are.

- **Handshake** body:
  - the sender's version vector: a 2-byte count, then per entry copy number 2 · epoch 8 · physical 8 · logical 4 (one entry per origin branch, 19.4);
  - the epochs it knows: a 2-byte count, then per epoch copy number 2 · epoch 8 · parent epoch 8 (0 for a copy's first);
  - the member reports it knows (19.6): a 2-byte count, then per member copy number 2 · reported at 8 · vector entry count 2 · entries as above;
  - the group's retirements: a 2-byte count, then per retirement retired copy number 2 · retiring copy number 2 · principal ID 8 · time 8 · signature length 2 · signature over the preceding 20 bytes and the group ID;
  - the clock offsets it knows: a 2-byte count, then per copy copy number 2 · offset 8 (signed) · error bound 8 · measured at 8;
  - the domains the sender carries (19.8): a 2-byte count, then the domain IDs (8 each).
- **Slice** body: a 4-byte change count, then the changes in each origin's clock order.

### 14.21.5 Changes

Each change:

| Size | Field |
|---|---|
| 2 | Origin copy number |
| 8 | Origin epoch (19.4) |
| 8 | Clock time: physical |
| 4 | Clock time: logical |
| 2 | Ordinal: the change's position among its origin's changes with the same clock time |
| 1 | Kind (below) |
| 8 | Domain ID: 0 public, else the enclave or joined store |
| 4 | Key version that sealed the body; 0 when the domain ID is 0 |
| 4 | Body length |
| n | Body: in plain bytes when the domain ID is 0, else sealed |

Change kinds and bodies (references are store ID 8 · entry ID 8; paths are a 1-byte level count, then one field ID (8) per level; values use the shared encoding, 14.6):

| Kind | Change | Body |
|---|---|---|
| 1 | set a path | reference · path · value |
| 2 | remove a path | reference · path |
| 3 | delete an entry | reference |
| 4 | add or reinforce a link | link type (8) · source end · target end · weight (4) · time (8); ends as in 14.13.1, or 14.13.2 in a joined store. The change's own identity (origin, epoch, clock) is the add's identity |
| 5 | remove a link | link type · source end · target end · a 2-byte count, then the identities of the adds it removes (copy number 2 · epoch 8 · physical 8 · logical 4 · ordinal 2) |
| 6 | a time-series point | store (8) · series (8) · timestamp (8) · values (an object) |
| 7 | replace a vector | reference · vector value |
| 8 | a catalog object a replicated store needs (19.3) | the object's record, as stored (14.3) |
| 9 | a schema version | store (8) · version (4) · the schema record |

- **Sealed bodies.** The body is encrypted with the file's AEAD algorithm under the domain's replication subkey, derived from the domain key without a session salt (14.2.8), so every copy that holds the domain key can open it. The associated data is the change's header (the bytes before the body). The nonce is the first 12 bytes of HMAC-SHA-256, under the domain's replication nonce subkey, of the header and the plaintext body: it repeats only for an identical change, even between forks that share a copy number. The sealed body is the ciphertext followed by the 16-byte tag.
- **Holding.** A copy without the domain key holds the change, sealed as received, in the domain's held log (14.4.10), in a held entry of the replication form (14.4.10 rule 9). Holding advances the version vector: the copy has the change and relays it, still sealed, in its own slices. It is merged when a key holder drains the log.
