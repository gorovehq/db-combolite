# Milestones

Moved from `CLAUDE.md`. The user reviews each milestone before the next one starts.

- **Still open.** These items need measuring, not deciding: the device-class table values, the vector search-path threshold (provisionally 10,000 vectors; spec 7.7), and how neuron weights are learned. Some crash-matrix rows still have to be written (spec 3.6).

| Milestone | Scope | Status |
|---|---|---|
| M0 | Build skeleton, warnings, sanitizers, test runner | done |
| M1 | CRC32C, varints, byte-order helpers, value encoding (spec 14.6) | done |
| M2 | Storage backend interface, host-file backend, fault-injecting backend (spec 1.11.1) | done |
| M3 | File header (spec 1.9, 14.1) | done |
| M4 | Pages, anchor map, free-space list (spec 13, 14.2, 14.7) | done |
| M5 | Delta tracks, transactions, reconciliation, recovery; the crash-point matrix becomes the main test suite (spec 3, 14.4) | done (multi-process completed in M19) |
| M6 | Space reuse and compaction (spec 1.3, 3.4, 3.7) | done |
| M7 | Ordered index: order-preserving keys and a B+tree over pager pages (spec 14.8) | done |
| M8 | Catalog: root slots, mirrored body, stores, schema history, field dictionary (spec 4, 14.3) | done |
| M9 | Documents: revision chains, patches, checkpoints, built-in document apply (spec 8, 14.9) | done |
| M10 | Document schemas: validation at write time, per-transaction staging (spec 8.3, 14.9.4) | done |
| M11 | Full-text search: term dictionary, postings, BM25 (spec 8.10, 14.14) | done |
| M12 | Vector collections: blocks, exact search, sources with staleness (spec 7, 14.12) | done |
| M13 | Neighbour graph: stored rankings and approximate search (spec 6.4, 6.8, 7.4) | done |
| M14 | Overflow pages: index values and document bodies larger than a page (spec 14.8.7, 14.9.2, 14.15) | done |
| M15 | Links: link types, the link store, delete and drop rules; the environment clock (spec 6, 14.4, 14.13) | done |
| M16 | Time series: series, bit-level column blocks, late modes incl. hold-and-forward, collisions (spec 10, 14.11) | done |
| M17 | Row-oriented tables on document pools, and nibblets (small records packed in shared pages) for document and table revisions and sparse time-series blocks: column schemas and schema changes, not-null/unique/primary keys, secondary indexes, history on or last-N (spec 9, 14.10.1, 16, 14.18) | done |
| M18 | Column-oriented tables and compression: row groups (small column chunks as nibblets), automatic column encodings, pending rows folded at reconciliation; xz (LZMA2) for column chunks, large document bodies, journal records and in-page compression (spec 9.12, 14.10.2, 15) | done |
| M19 | Coordinator: the in-process coordinator, primary/subordinate election, heartbeats, the append/settle/choose baton protocol for reconciliation and compaction, per-process delta tracks and single-writer coordinator slots, multi-process recovery; completes M5 beyond the single-process case (spec 5, 14.4, 14.5) | done |
| M20 | Parallel engine work (high priority): optional thread and mutex hooks in the environment (micro-controllers stay single-threaded), worker threads for reconciliation, neighbour-graph refresh, compression and other per-store work | done |
| M21 | Structured query core (CMBQL core): operator-tree execution (Scan/Filter/Project/Join/Sort/Limit/Union/Aggregate/Traverse/TextSearch/VectorSearch/HybridRank), pipeline breaker memory bounds, time-series Window/AsofJoin/rollup pushdown, Traverse cycle/pruning semantics, Unnest, cross-model type coercion/NULLs, binary wire form (spec 14), and stored-procedure bytecode ISA engine (spec 11.1, 11.2, 11.7, 11.10) | done |
| M22 | Text query language (CMBQL): db-combolite's own human-readable language (CMBQL), formal EBNF grammar and lexer, literal dot escaping vs 0x1F nested path resolution, procedural variable scoping and materialization, compiling into the M21 structured core (spec 11.1, 11.10) | done |
| M23 | Cross-store atomic transactions and collision handling, triggers and trigger safety, scheduled and maintenance-window procedure runs, intra-transaction cursor isolation, procedure error trapping and savepoints (spec 11.3, 11.6, 11.9) | done |
| M24 | Security, authenticated encryption (AES-256-GCM, ChaCha20-Poly1305), envelope key wrapping, subkey derivations, key rotation, tamper-evident governance mode, identities and RBAC authorization (spec 12, 14) | done |
| M25 | Compute acceleration (spec 18), in layers. **1:** the accelerator hook in `cdb_env`, mirrors (filled on the worker pool a chunk at a time, kept in step through batched apply notes, caught up from the vector change log at a rebase, evicted under `accel_mem`), certified exact search, the fake accelerator. **2:** CUDA backend (cuBLAS distances, our own top-K). **3:** Vulkan backend (our own shaders, checked-in SPIR-V). **4:** bulk search, graph refresh acceleration (after profiling), the demo | layers 1–4 done; approximate search's pending pass still on the CPU (BACKLOG) |
