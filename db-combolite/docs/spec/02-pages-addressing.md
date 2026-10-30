# Section 2: Pages and addressing

Status: **Draft. The decisions below are confirmed; the open questions are not.**
Byte layouts, field sizes and algorithm identifiers are not defined yet.

## Purpose

A **page** is the fixed-size block in which every structure in a db-combolite file is stored: header copies, the anchor list, deltas, rows, documents, vectors and so on. Four things work at the granularity of a page:

- **I/O.** Data is read and written a page at a time.
- **Integrity.** Each page carries its own checksum or authentication tag.
- **Encryption.** Each page is encrypted under a single key.
- **Allocation.** Space is handed out and reclaimed a page at a time.

**Addressing** is how a reference finds a page. A reference holds a logical address, and that address is resolved through the anchor list (Section 1.7). New data is always appended, and pages never move in place.

## Confirmed decisions

### 2.1 Page size is chosen by the operator

Every library build must support both of these modes. The operator picks one for each database, according to the use case.

- **Uniform:** a single page size for the whole file, fixed when the file is created. This is meant for casual use and simple projects.
- **Per store:** each store has its own page size, for example large pages for vectors and small pages for relational rows. This is meant for robust projects that have to work within memory limits or balance resources between stores. The operator accepts the extra allocator complexity in return for the ability to fine-tune.

### 2.2 Each page has exactly one owner

Every page belongs to exactly one store **and** exactly one encryption domain. As a result:

- each page is encrypted under exactly one enclave key, or under none;
- deduplication stays naturally inside its domain (Section 1.6);
- the smallest amount of space a store or an enclave can occupy is one page.

### 2.3 Integrity and encryption

- **Unencrypted pages** carry a checksum. In tamper-evident mode, public pages carry a keyed integrity tag instead (Section 12.7).
- **Encrypted pages** use authenticated encryption, which provides an authentication tag. The tag replaces the checksum, so these pages carry no separate checksum.

### 2.4 Logical address width can be configured

- At creation the library **probes the host's native word size** and uses it as the default: 32-bit on microcontrollers, 64-bit on workstations and servers.
- The operator can **override** the default and pick either width. This allows for configurations that already exist.

### 2.5 Compression is a hybrid, chosen to fit each module and data type

Two compression strategies are available:

- **In-page compression.** The page size stays fixed, and the page holds more records.
- **Compressed extents.** Compressed data spans a run of pages, and an anchor points to the run.

Each module and data type uses whichever strategy fits it best.

### 2.6 Page types: extensible, handled like feature flags

Page types are registered in the same way as feature flags (Section 1.2). Each page type falls into one of three classes, and that class decides what a reader does when it meets a page type it doesn't recognize:

- **compatible:** the reader ignores the page;
- **read-only compatible:** the reader opens the file read-only;
- **incompatible:** the reader refuses to open the file.

Page types identified so far:

| Page type | Holds |
|---|---|
| Header | a header slot: A/B, or a ring on raw flash (Sections 1.9, 1.11.3) |
| Catalog root slot | a root slot at location 0 (Section 4.7) |
| Anchor map | the mapping from logical to physical addresses (the anchor list) |
| Free space | the tracking of space that has been freed but not yet compacted |
| Index node | the interior and leaf nodes of index structures |
| Overflow | values that are too large for the page that references them |
| Document | records from document pools |
| Vector block | vector data |
| Time-series block | time-series data |
| Delta | records from the delta tracks |
| Dedup index | the deduplication index for one encryption domain |
| Neighbour-graph block | stored rankings and the approximate-search graph (Sections 6.4, 7.4) |
| Full-text index node | postings for keyword search (Section 8.10) |
| Blob | attachments and large binary values (Section 8.4) |
| Validation map | which pages are known to be valid, persisted at reseal (Section 13.5); **compatible** class |
| Coordinator slot | a single-writer coordinator slot, used in raw-flash regions (Section 5.7) |

How each page type is laid out internally is defined in the section for the data model that uses it.

### 2.7 Address width is recorded in the file

The host's native word size only sets the *default* (2.4). The address width actually used is recorded in the header. **Every reader must support both 32-bit and 64-bit files.** A 32-bit host does 64-bit arithmetic in software. It can fail only when the file contains more pages than the host can number.

### 2.8 Page sizes are powers of two within bounds

In both modes, every page size is a power of two between a minimum and a maximum.

The bounds can be configured:

- **Built-in defaults.** The spec provides a **lookup table of sensible limits for each device type**, so a new database works well with no tuning.
- **Power users** can adjust the bounds to fit their use case.

The table covers these device classes:

- small microcontrollers
- microcontrollers with external PSRAM (for example ESP32)
- single-board computers (for example Raspberry Pi)
- desktops and workstations
- servers

For hosts that are not microcontrollers, the limits are given in RAM tiers from **1 GB to 16 GB**. Hosts with more than 16 GB use the 16 GB tier as their default, and any value the operator sets is accepted without further guidance. **Physical addresses count in units of the smallest page size in the file.** This works because every page size is a power of two.

### 2.9 Checksum algorithm is a header flag

The algorithm used for page checksums is chosen per file and recorded in a header flag, so it can be optimized for the target host. Candidates include CRC32C and a 64-bit hash.

### 2.10 Compression: defaults, with an override and a warning

The spec sets a sensible default compression strategy for each data type (2.5). Operators may override the default. If they do, the library warns them about the possible consequences. Section 15 gives the codecs, the defaults and the warning in detail.

### 2.11 The common page header

The common page header is defined in [Section 13](13-page-header.md).

## Open questions (do not specify until answered)

1. **Values in the device-type table (2.8).** *Pending benchmarking.* The values will be measured on representative hardware for each class.
