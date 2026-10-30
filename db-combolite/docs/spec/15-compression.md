# Section 15: Compression

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Compression trades CPU for space and I/O. Several parts of db-combolite use it, and before this section each part settled a piece on its own:

- **2.5:** the two strategies, in-page and compressed extents.
- **2.10:** defaults per data type, with an operator override and a warning.
- **9.12:** column chunks.
- **14.2.1:** the codec byte in the page header.
- **14.3.3:** the catalog's compression field.
- **14.4.2:** the compressed flag on journal records.

This section brings those together. It defines:

- which codecs exist;
- the exact form of compressed data;
- what is compressed, and when;
- the defaults per data type;
- how compression interacts with checksums and encryption;
- what a build without the codec does when it meets compressed data.

## Confirmed decisions

### 15.1 Codec registry

A codec ID takes 7 bits (0–127). It is stored in the page header's compression byte (bits 0–6, with bit 7 marking extent mode; 14.2.1) and wherever else compressed data is described.

IDs name compression **algorithms**, not file containers. db-combolite stores raw compressed streams inside its own format, so a container's headers are never stored.

| ID | Algorithm | Familiar as | Status |
|---|---|---|---|
| 0 | none | | stored uncompressed |
| 1 | LZMA2 | xz, 7z | **shipped** (liblzma, vendored and pinned) |
| 2 | DEFLATE | gzip, zip, zlib | reserved |
| 3 | Zstandard | zstd | reserved |
| 4 | LZ4 (block format) | lz4 | reserved |
| 5 | bzip2 | bz2 | reserved |
| 6 | LZMA (classic) | older 7z, .lzma | reserved |
| 7 | Brotli | br | reserved |
| 8 | Snappy | | reserved |
| 9–127 | | | unassigned |

- A reserved ID only fixes the number. Nothing writes it until the codec ships, and each codec is added through the dependency approval process (CLAUDE.md, constraint 5).
- A reader that meets a codec it does not have treats that data as unreadable, and never guesses (15.7).

### 15.2 Compressed form

Compressed data is stored as a small header followed by the codec's raw stream:

| Field | Encoding | Meaning |
|---|---|---|
| Uncompressed length | varint | exact length after decompression |
| Codec parameters | codec-defined | for LZMA2: one byte, the LZMA2 dictionary-size property |
| Stream | raw | the codec's raw stream, with no container |

- The `.xz` container is not used. It adds about 60 bytes of headers and a checksum of its own, while the page and record checksums already cover the data (15.6).
- A decompressor must produce exactly the stated length. A stream that ends early, runs long or fails to decode is `CDB_E_CORRUPT`.

### 15.3 What is compressed

| Data | Strategy (2.5) | Built in |
|---|---|---|
| **Column chunks** (14.10.2) | compressed after the column encoding (9.12) | M18 |
| **Large document bodies** (14.9.2): bodies that spill into an overflow chain (14.15) | compressed extent: the compressed body is stored in the revision page and its chain | M18 |
| **Journal records** (14.4.2): large change payloads | the payload is compressed, and the record's compressed flag is set | M18 |
| **Whole pages** (in-page) | the payload of a page is compressed, so a fixed-size page holds more entries (15.5) | M18 |

### 15.4 When compressed output is kept

- **Size threshold.** Compression is attempted only on items larger than **a quarter of the page size**. Smaller items are always stored plain, because LZMA2's fixed overhead would cancel the gain. Column chunks and pages are always attempted.
- **Keep rule.** The compressed form is kept only if it saves **at least 1/8** of the size. Otherwise the item is stored plain with codec 0, so nothing ever pays for decompression that gives little back. The rule is the same everywhere.
- **Codec settings.** LZMA2 runs at a **low preset** with a dictionary no larger than the item. That costs about 1–3 MB to compress and little to decompress.

### 15.5 In-page compression

- **Logical size.** A compressed page holds up to **4× the page size** of entries. Its logical content is at most four pages, compressed into one physical page.
- **Memory.** Readers decompress into caller-supplied buffers of four pages each. A device class may set the factor to 2 to save memory. The factor is fixed when a store is created and recorded with it.
- **Writing.** A page is written compressed only when the keep rule (15.4) is met. Otherwise it is written plain and holds one page of entries as usual.
- **Flags.** The page header's flag bit 0 (14.2.6) marks a compressed page, and its codec byte names the codec.

### 15.6 Order with checksums and encryption

- **Compress first, then encrypt** (Section 12). Encrypted data does not compress, so the plaintext is compressed first.
- **Checksums cover the stored bytes**, which are compressed and, when enabled, encrypted. A damaged page or record is therefore caught before decompression is attempted. Decompression checks the stated length as well (15.2).

### 15.7 Feature flag and builds without the codec

- **Feature flag.** Compression is an **incompatible** feature flag (1.2). It is set **when the first compressed item is written**, not when the database is created. A file that never compressed anything stays openable by builds without the codec.
- **Build option.** The `CDB_WITH_XZ` CMake option compiles in LZMA2 and is on by default. A build without it:
  - has its compression defaults off and writes everything plain;
  - refuses a file that carries the compression flag, rather than misreading it.
- **Microcontroller builds** may leave the codec out. A decoder-only build may come later (XZ Embedded, public domain) if microcontrollers need to read compressed files.

### 15.8 Defaults per data type, overrides and the warning

- **Defaults.** All four uses in 15.3 are **on by default** (2.10). A database-wide default is set at creation, and each store may override it in its catalog record's compression field (14.3.3).
- **Warning.** When an operator overrides a default, the library calls an optional **warning callback** in the database configuration with the store and the setting overridden, as 2.10 requires. When the system audit series exists (10.7), the warning will be written there as well.

### 15.9 Hot-path compression is measured before its default is final

LZMA2 packs tightly, but it is slow next to speed-oriented codecs such as zstd and LZ4. Compressing pages and journal records puts it on the hot path: every commit, every page write and every read of a compressed page waits on it. M18 therefore **benchmarks page and journal compression** against the same workloads uncompressed, using the Release build and a bench tool like `bench_graph`. If either one misses the agreed budget, its default changes before M18 is merged: it is either switched off by default or moved to a fast codec. Zstandard and LZ4 have reserved IDs (15.1) for that purpose. Column chunks and large document bodies are not on the hot path and stay as decided.

## Open questions (do not specify until answered)

- **The performance budget for 15.9:** how much slower commits, page writes and page reads may be with compression than without it. It will be set with the project owner once M18's first measurements exist.
