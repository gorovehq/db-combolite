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
| 481 | 11 | Reserved | zero |
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

### 14.1.9 Header integrity

- The header **always** uses CRC32C, whatever the file's checksum flag says. A reader can therefore verify the header before relying on any field in it.
- In tamper-evident mode, the keyed tag at offset 492 is checked as well.

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
| 50 | 6 | Reserved, zero |

### 14.2.2 Full header, 32-bit file (40 bytes)

Bytes 0–23 are the same as in 14.2.1. The rest of the header is:

| Offset | Size | Field |
|---|---|---|
| 24 | 4 | Logical page ID |
| 28 | 4 | Owning store ID |
| 32 | 4 | Encryption domain ID |
| 36 | 2 | Extent position |
| 38 | 2 | Reserved, zero |

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
| 26 | 2 | Reserved, zero |

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
- **Delta records.** The track ID is the coordinator slot number (2 bytes) followed by the companion type (2 bytes), and the sequence number is the delta sequence number. Delta sequences are reserved durably in the slot (Section 12.1), so numbering continues correctly when a later process reuses the slot.
- **Associated data.** The page header, or the delta record header, is passed as AEAD associated data. It is authenticated, but not encrypted.

### 14.2.6 Identifiers and numbering

- **Checksum algorithm IDs** (file header offset 76): `1` = CRC32C. `0` is invalid. Further algorithms are assigned IDs when they are added.
- **Page type IDs** (page header offset 2):

  | ID | Page type |
  |---|---|
  | `0x0001` | anchor map |
  | `0x0002` | free space |
  | `0x7F00` | reserved for tests |

  Further page types are assigned IDs when they are built.
- **Zero means "none".** Logical page ID `0` and physical page `0` both mean "no page".
- **Physical page numbers** count from the start of the file, in units of the file's page size. The **data area** starts after the two header slots (1,024 bytes) and the location-0 area (1,024 bytes), rounded up to a page boundary. System pages that are addressed physically, such as anchor-map and free-space pages, carry logical ID `0`, store `0` and domain `0`.

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
| 48 | 8 | Enclave directory root | public list of opaque enclave IDs and key references only (Section 4.4) |
| 56 | 8 | Schema-history root | Sections 4.3, 4.11 |
| 64 | 8 | Public quick-query index root | derived, can be rebuilt (Section 4.2) |
| 72 | 8 | Public dedup index root | |
| 80 | 8 | Public audit series root | Section 10.7 |
| 88 | 8 | Next object ID | the store, enclave and procedure ID counter; IDs are never reused (Section 4.12) |
| 96 | 388 | Reserved | zero |
| 484 | 8 | End magic | `0A 54 41 43 42 44 43 89` (the start magic reversed) |
| 492 | 16 | Tamper-evident tag | |
| 508 | 4 | CRC32C | covers bytes 0–507 |

Each store's entry-ID counter is held in that store's catalog entry, not in the root.

## 14.4 Companion files, delta records and commit records

### 14.4.1 Companion file header (64 bytes)

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
| 48 | 8 | Encryption domain ID (0 = public) |
| 56 | 4 | Reserved, zero |
| 60 | 4 | CRC32C over bytes 0–59 |

### 14.4.2 Delta record

A delta record is a 48-byte header, followed by the payload, followed by the trailer.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Record marker: `C0 DE` |
| 2 | 1 | Record kind: 0 = data change · 1 = commit record · 2 = abort · 3 = neuron-weight update (Section 6.10) |
| 3 | 1 | Flags: encrypted, compressed |
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
| 8 | Global commit number (14.1.8) |
| 8 | Commit timestamp from the primary coordinator's clock |
| 8 | Principal identity ID (Section 5.1) |
| 4 | Number of deltas in the transaction |
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

The exact encoding of each operation, per data model, is defined with the data-model layouts.

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
4. **Journal rotation.** Reconciliation writes the new, empty journal as `main.<slot>.cdb.new`, makes it durable, then renames it over the old one and syncs the directory. A crash always leaves exactly one journal.
5. **Commit numbers** start at 1. They are reserved in blocks of 1024, in the same way as page sequence numbers.
6. **Durability.** The default is *normal*, with a barrier every 32 commits (configurable). *Off* has barriers only at reconciliation and close.
7. **Visibility.** A transaction's changes are applied, and become visible, when it commits. A transaction cannot read its own changes before it commits.

## 14.5 Coordinator slot

### 14.5.1 Placement

- **Host files:** each coordinator slot is the file `coord/<slot>`.
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
| 13 | 1 | State flags: alive · unfinished baton session pending |
| 14 | 2 | Priority value (Section 5.5) |
| 16 | 16 | Database ID |
| 32 | 16 | Session identity: a random 128-bit value (Section 5.1) |
| 48 | 8 | ID of the principal identity the session authenticated as |
| 56 | 8 | Heartbeat counter |
| 64 | 8 | Heartbeat timestamp, from the primary's clock (µs) |
| 72 | 8 | Delta-sequence high-water mark (Section 12.1) |
| 80 | 8 | Transaction-counter high-water mark (14.4.3) |
| 88 | 1 | Current request kind: 0 = none · 1 = baton · 2 = claim primary · 3 = priority value |
| 89 | 1 | Reserved, zero |
| 90 | 2 | Heat ID: which baton is meant when several heats exist (Section 5.3) |
| 92 | 4 | Reserved, zero |
| 96 | 8 | Request timestamp |
| 104 | 8 | *x-total*: the declared size of the work |
| 112 | 8 | ID (CTS) of the baton held, or of the last baton held |
| 120 | 8 | Progress *x* |
| 128 | 8 | Time the baton was issued |
| 136 | 8 | Published clock. Written only by the primary. |
| 144 | 8 | The last global commit number this session has seen reconciled |
| 152 | 356 | Reserved, zero |
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
  2. the new free-space chains, then the changed anchor-map nodes from the leaves up, with the root last;
  3. barrier;
  4. the header commit.
- **Sequence reservation.** Page sequence numbers are reserved in blocks of 1024 (Section 12.1).

## 14.8 Ordered index

### 14.8.1 Structure

The ordered index is a **copy-on-write B+tree** (Section 9.5):

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

---

## 14.10 Table pages

### 14.10.1 Row-oriented tables

A row-oriented table is a B+tree clustered by **entry ID**, with the same structure as 14.8. Each leaf value is a record envelope (14.9) that has no summary. Primary keys and other indexes are separate ordered indexes that point to the entry ID.

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

---

## 14.11 Time-series blocks

- **Series lookup.** An ordered index on the **series key** returns the series entry ID. A second ordered index on *(series entry ID, window start)* returns the block for that window.
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

### 14.12.2 Neighbour-graph block (Sections 6.4, 7.4)

One record per entry:

| Size | Field |
|---|---|
| 8 | Entry ID |
| 2 | Neighbour count (≤ K) |
| 1 | Flags: cross-collection neighbours present |
| 1 | Version of the base measure |
| per neighbour | Neighbour reference (store ID only when cross-collection, then entry ID) and distance (f32) |

**Entry points.** Approximate search begins from a small set of **entry points** chosen to cover the collection, for example the medoid and a few well-spread entries. They are stored in the collection's catalog entry. A plain nearest-neighbour graph doesn't always lead the search everywhere, and starting from points that cover the collection solves this.

---

## 14.13 Links and neuron weights (Section 6)

Links, foreign keys and neuron mapping all use this layout.

| Size | Field |
|---|---|
| var | Source reference (store ID, entry ID), or an opaque pointer in a joined store |
| var | Target reference |
| 4 | Link-type ID. The link type, its cardinality and its delete behaviour are defined in the catalog (Sections 6.7 and 6.9). |
| 4 | Weight (f32). Used by neuron mapping; 1.0 for plain links. |
| 8 | Time the link was last reinforced (Section 6.10) |
| 1 | Flags: tombstone |

Every link is stored in **two** ordered indexes: one keyed by *(source, link type, target)* and one keyed by *(target, link type, source)*. Links can then be followed efficiently in both directions.

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

---

## 14.15 Blobs and overflow pages

- **Overflow chain.** An overflow page holds the logical ID of the next page in the chain (or 0 at the end), followed by a payload. A value that doesn't fit in its page continues along this chain.
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
- a **reference count**.

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

## Open questions (do not specify until answered)

None at present.
