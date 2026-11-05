# Section 13: Common page header

Status: **Draft. The decisions below are confirmed; the open questions are not.**
This section replaces the placeholder in Section 2.11. Exact byte offsets are deferred to the byte-layout pass.

## Purpose

Every page starts with the same small header and ends with an integrity trailer, whatever the page contains. With these alone, a reader can identify a page, verify it, decrypt it and work out where it belongs, without consulting any other structure. That holds both in normal operation and in a post-mortem where the catalog or the anchor map is damaged.

## Confirmed decisions

### 13.1 Header fields

| Field | Purpose | Size (32-bit / 64-bit file) |
|---|---|---|
| Page marker | Short magic value. Lets recovery tools find page boundaries in raw bytes. | 2 B |
| Page type ID | The page type. Its compatibility class comes from the registry (Section 2.6). | 2 B |
| Layout version | Version of the page type's internal layout. A page type can change its layout without needing a new type ID. | 1 B |
| Flags | Compressed, member of an extent, overflow continuation, tamper-evident, written in `danger_mode`. | 1 B |
| Owning store ID | The store that owns the page (Sections 2.2, 4.6). 0 on an encrypted page, whose store ID is inside the encrypted payload (Section 12.8). | 4 / 8 B |
| Encryption domain ID | The page's enclave or joined store; 0 means public. It is recorded apart from the store ID **on purpose**, so a page can be decrypted without the catalog. | 4 / 8 B |
| Key version | The key version that encrypted the page (Section 12.4). | 4 B |
| Logical page ID | The page's anchor address (Sections 1.7, 2.4). | 4 / 8 B |
| Write sequence number | Breadcrumb watermarks (Section 1.3) and nonce uniqueness (Section 12.1). | 8 B |
| Compression codec | The codec used, and whether compression is in-page or by extent (Sections 2.5, 2.10). | 1 B |
| Extent position | The page's position within its extent, if it belongs to one. | 2 B |
| Payload length | The number of bytes in use. | 2–4 B |

The header comes to about **36 B in a 32-bit file** and about **50 B in a 64-bit file**.

### 13.2 The integrity trailer is at the end of the page

The checksum (Section 2.9), the AEAD tag (Section 12.1) or the keyed integrity tag (Section 12.7) is stored at the **end** of the page, after everything it protects. A writer can therefore build the whole page and compute the trailer in a single pass. The trailer is 16 B.

### 13.3 The header is plaintext but authenticated

The header is never encrypted, because a reader needs its contents in order to decrypt the page.

- On **encrypted** pages, and on public pages in **tamper-evident** mode, the header is authenticated by the page's tag, so any change to it is detected.
- On public pages protected only by an unkeyed checksum, the header is covered by the checksum. That detects accidental damage but not deliberate tampering.

The only identifiers the header exposes are opaque numeric IDs (Section 4.4).

### 13.4 Compact header for small pages

Files whose pages are **1 KiB or smaller** can choose a **compact header variant**, recorded in a file header flag. The compact header uses smaller IDs and a shorter trailer. This keeps the overhead reasonable on microcontroller-sized pages, where the full header would take up about 10% of a 512 B page.

- **ID widths.** Store IDs, domain IDs and logical page IDs are 16-bit, so a compact file holds at most 65,536 pages. That is 32 MiB at 512 B per page.
- **Trailer length.** Only a **checksum** trailer is shortened, to 8 B. AEAD tags stay at 16 B on encrypted pages, because a truncated authentication tag weakens protection against forgery.

### 13.5 Validation state is kept outside the pages

Pages don't carry a "validated" flag. Validation state is kept in a **validation map**: a per-region bitmap or a "verified up to" watermark. The map is held in a companion file and in memory.

**At reseal, the map is persisted into the archive** as a validation-map page type in the **compatible** class (Section 2.6). It therefore survives while the database is at rest, and older readers can ignore it (review G7).

- **Updating the map.** The scrubber (Section 3.5) updates the map. So do ordinary reads, because every read already verifies the page it reads.
- **Startup validation** is configurable:
  - **off**;
  - **critical structures only**: the header copies, the catalog root and the anchor map;
  - **full**: every page, for operators who accept the startup time this takes.

At startup, the database knows which pages are known good without having written to any page.

## Open questions (do not specify until answered)

None at present.
