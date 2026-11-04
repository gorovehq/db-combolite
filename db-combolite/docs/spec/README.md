# db-combolite format specification

This is a clean-room specification for a portable, serverless, multi-model embedded database. It is written one section at a time. Each section lists its **confirmed decisions** and its **open questions**, and nothing goes into a section until the project owner has confirmed it.

## Sections

| # | Section | Builds on | Open items |
|---|---|---|---|
| 1 | [File layout and header](01-file-layout.md) | — | — |
| 2 | [Pages and addressing](02-pages-addressing.md) | 1 | values for the device-type table (needs benchmarking) |
| 3 | [Crash safety and recovery](03-crash-safety.md) | 1, 2, 5 | — |
| 4 | [Catalog](04-catalog.md) | 1, 2, 3 | — |
| 5 | [Concurrency and coordination](05-concurrency.md) | 1, 3 | — |
| 6 | [Keys and links](06-keys-links.md) | 1, 4 | how neuron weights are learned (decide by experiment during development) |
| 7 | [Vector](07-vector.md) | 2, 4, 6 | automatic search-path rule: provisional threshold of 10,000 vectors, to revisit with real embeddings |
| 8 | [Document](08-document.md) | 2, 4, 6, 7 | — |
| 9 | [Relational](09-relational.md) | 2, 4, 6, 8 | — |
| 10 | [Time series](10-time-series.md) | 2, 4, 5, 9 | — |
| 11 | [Query layer and stored procedures](11-query.md) | 3–10 | — |
| 12 | [Security](12-security.md) | 1, 2, 4, 5, 6 | — |
| 13 | [Common page header](13-page-header.md) | 1, 2, 12 | — |
| 14 | [Byte layouts](14-byte-layouts.md) | all | — |
| 15 | [Compression](15-compression.md) | 1, 2, 9, 12, 13, 14 | the performance budget for page and journal compression (set after M18's benchmarks) |
| 16 | [Small records (nibblets)](16-nibblets.md) | 2, 3, 8, 9, 12, 14 | — |
| 17 | [Build profiles](17-build-profiles.md) | 4, 5, 11, 14 | the host query memory budget's default; host limit values (set by benchmark) |
| 18 | [Compute acceleration](18-compute-acceleration.md) | 5, 6, 7, 14, 17 | the shortlist margin; mirrors that outgrow the device budget; where the change stamp lives; refilling without a worker pool; element types; shader build; testing |

## Reviews

- [Review 1](REVIEW-1.md): cross-section consistency. Found 6 critical issues, 10 gaps and 7 editorial fixes. All have been applied.
