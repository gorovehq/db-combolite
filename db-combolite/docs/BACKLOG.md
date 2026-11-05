# Backlog

Features and follow-ups that were agreed and deferred. Each one becomes a milestone or a branch when it is picked up, and its design decisions are brought to the user then, as usual.

## Features

### Streaming API for large content

**What:** write and read content a piece at a time, for example "open, append chunk, …, close", so a whole book or file is never held in memory at once.

*Implemented on branch `feat_streaming-blobs`:*
- **Streaming crypto primitives:** incremental SHA-256 and HMAC-SHA-256 (`cdb_crypto_hash_*`) using Mbed TLS with zero heap allocations.
- **Blob storage engine:** `cdb_blob_write_begin`, `cdb_blob_write_chunk`, `cdb_blob_write_end`, `cdb_blob_write_abort`, `cdb_blob_read_begin`, `cdb_blob_read_chunk`, `cdb_blob_read_end`, `cdb_blob_free`. Pages are allocated across standard overflow page extents (`0x0007`).
- **Value encoding:** Tag `CDB_TAG_BLOB = 0x46` (spec 14.15) with `cdb_value_write_blob_ref` and `CDB_TYPE_BLOB_REF` decoding in `cdb_value_read`.
- **Multipart journal framing:** `CDB_RECORD_FLAG_CONTINUED = 0x04u` for multipart streaming frame logging.
- **High-level DB integration:** `cdb_db_blob_*` with automatic enclave HMAC content addressing and store isolation.
- **Unit test suite:** `test_blob` verifying NIST vectors, chunked roundtrip, tamper detection, page reclamation on abort, value encoding, multipart journaling, and enclave integration.

### Conflict-Free Replicated Data Types (CRDTs) and Multi-Master P2P Replication

*Implemented:*
- **State & Clock Tracking:** Hybrid Logical Clocks (`cdb_hlc`) with strict total ordering and sub-tick counters; bounded peer version vectors (`cdb_crdt_vector`) with O(1)/O(P) dominance and concurrency detection (`CDB_CRDT_CMP_DOMINATES`, `DOMINATED`, `CONCURRENT`, `EQUAL`).
- **CRDT Primitives:**
  - *Documents & Tables:* Field-level Last-Write-Wins (`cdb_crdt_lww_merge`) with tombstone tracking.
  - *Distributed Counters:* Positive-Negative counters (`cdb_pn_counter`) with associative, commutative, idempotent multi-peer merges.
  - *Graph Link Store:* Add-Wins Observed-Remove Set (`cdb_crdt_link_resolve`) for link tokens.
  - *Time Series:* Commutative point append deduplication (`cdb_crdt_ts_same_point`).
- **Delta-State Replication Protocol:**
  - Transport-agnostic binary wire framing: two-way handshake (`cdb_crdt_handshake_encode`/`decode`) and delta slices (`cdb_crdt_slice_encode`/`decode`).
  - High-level zero-allocation database export & import APIs (`cdb_db_crdt_export_slice`, `cdb_db_crdt_import_slice`, `cdb_db_crdt_import_deltas`).
  - Replication echo suppression via `CDB_RECORD_FLAG_REPLICATED = 0x08u`.
- **Unit test suite:** `test_crdt` verifying HLC causality, vector dominance/concurrency, PN-counter convergence, LWW document field resolution, graph link AWORSet, wire protocol codecs, and multi-peer bidirectional sync.

## Follow-ups

- **Two processes creating the same new database at once.** Creating the archive is not claimed through the coordinators (they live in the working directory, which needs the archive's database ID). The environment has no exclusive create. Rare, but possible.
- **A process can create only 21,504 entries between reconciliations (found by the demo, 2026-11-04).** Entry IDs come in claimed blocks of `CDB_COORD_ID_BLOCK` (1,024), and a coordinator slot holds at most `CDB_COORD_MAX_BLOCKS` (21) of them, across all stores. A used-up block is released only by `prune_held_blocks`, once reconciliation has recorded its store's `next_entry_id` in the catalog, and nothing reconciles by volume. So a long ingest in one process stops after 21 × 1,024 new entries, and every insert then fails with `CDB_E_OVERFLOW`, which reads like a too-large row. Rebuilding GutenFree's catalog hit it at row 21,504 of 79,875 (the old database had been filled before M19 or over several sessions). A partly used block was never released at all, so one entry in each of 21 stores stopped a session too. *Fixed on `fix_coord-id-blocks`:* `cdb_tx_commit` reconciles once more than half the blocks are held and gives back the unused rest of each block the catalog now covers (`release_held_blocks`); a single transaction that needs more than 21 blocks fails with `CDB_E_OUT_OF_BUDGET`. `tests/test_idblocks.c` covers both. *Still open:* one transaction is still limited to 21 blocks (21,504 new entries across its stores); recording held IDs as ranges (store, first, end) would lift that, but changes the coordinator slot format (spec 14.5).
- **A graph refresh at minimum `vec_work` checks every entry (found by the demo, 2026-11-04).** With `vec_work_size` at `cdb_vec_work_size()`, a refresh batch holds a handful of vectors, the choice of entry points keeps only `MIN_EP` (8), and the list of touched entries to check (an eighth of `vec_work`) overflows. Spec 14.12.4 then checks every entry with a list and every entry point, a walk each. That cost is paid by every refresh call, so slicing a refresh, or a reconciliation's budgeted refresh, repeats it. GutenFree's passages (16,033 vectors, K 16) took 767 s in one call, and about 170 s per 256-entry slice until the graph filled out, then up to 90 minutes per slice as the reach points grew. 13,313 of 15,265 entries were refreshed in 7.7 hours. The demo now gives `vec_work` 64 MB. *Decided (spec 17.3–17.5, 2026-11-04):* on host, graph maintenance grows past `vec_work` through the environment's allocator, up to a new `vec_mem` budget, and falls back to 14.12.4's slower paths at the budget instead of failing; graphs may differ in entry points by profile. *Implemented on `feat_vec-mem`:* `cdb_db_config.vec_mem` (default `CDB_VEC_MEM_DEFAULT`, 64 MB, provisional); each refresh sizes its memory for its pending entries and frees it after; `tests/test_graph.c` checks that sliced refreshes at the minimum `vec_work` never fall back on host, do on mcu, and do on host when `vec_mem` is too small. The misaligned check list at that minimum size was fixed with it. *Still open:* the `vec_mem` default and keeping memory between refreshes (spec 17 open questions), and sizing `vec_work` for mcu in `cdb_vec.h`. Keeping touched entries across budgeted refreshes (a 14.12.4 change) is held unless this is not enough.
- **Vector change log: what `feat_vec-change-log` leaves open (spec 14.12.5, 18.12, 2026-11-04).** The log, its trimming, named cursors, `cdb_vec_changes` and the catch-up (18.12) are implemented and tested (`tests/test_vec.c` with a crash sweep; trimming by another live process in `test_catch_up_across_processes`, `tests/test_accel.c`). Not yet:
  - **Collections closed at reconciliation** are not trimmed until they are next open, so an idle collection's log can lag behind its floor.
  - **The stamp** is found by bisecting sequences (`change_stamp`): about 64 index seeks at most. Fine for now; a stored stamp would make it one read.
- **Accelerator, what M25 leaves open (spec 18, 2026-11-05).** Layers 1–4 are in main: the `cdb_env` hook, mirrors, certified exact search, the CUDA and Vulkan backends, bulk search and the graph refresh on the device.
  - **The coverage checks' walks now limit a graph refresh** (CPU, index-bound). With the refresh on the device, all 16,033 GutenFree passages take CPU 137 s, CUDA 79 s, Vulkan on the RTX 4060 82 s, Vulkan on Intel UHD 110 s. gprof, 8,000 passages on CUDA: about 70% of the sampled time is in `check_entry` → `walk_run`.
    - `ep_next` seeks the entry points index at every step: 2.9 million seeks in 7,910 walks.
    - `reach` makes 3.5 million entry lookups.
    - Index writes (`save_table`) take most of the rest.
    - Reading the entry points once per check, and the entries' locations in batches, is the likely fix. It needs care, since a check can add reach points while it runs. This is an engine change, not the accelerator's.
  - **Approximate search's pending pass is not accelerated** (18.4).
  - **A mirror's size is an estimate** between fills: a change-log put is counted as new even when it replaced a vector, so the budget is used conservatively until the next fill.
  - **A collection too big for `accel_mem` is counted again** at each reconciliation that tries to fill it (one entries-index scan each time).
- **The accelerator's error bounds are far from the errors seen (M25 layer 2, 2026-11-04).** The bound (`cax_bound` in `accel/common/cax.c`, shared by the CUDA and Vulkan backends) is a worst case: twice float's unit roundoff per add, over every element, at the largest norm the mirror ever held. In `test_accel_cuda`, with Gaussian data at 384 dimensions, the largest error seen is about 0.15% of the bound for cosine and dot, 0.001% for Euclidean (whose bound is a square root of the squared distance's error), and 17% for i8 cosine. On clustered data at 100,000 vectors, f32 cosine (`bench_accel`), every first shortlist still certified. Data whose K-th and 2K-th distances are closer than the bound will widen or go to the CPU. That is still correct, but it gives up speed. A tighter bound must still be proven, never only observed.
  - **Measured on GutenFree's passages** (`bench_accel --file`, a copy of `gutenfree-v2`, 16,033 passages, 200 queries each). Every first shortlist certified, with none widened and none sent to the CPU:
    - `passage_vec` (f32, cosine, 384 dims), k=10: 51× faster than the CPU. The mean gap between hits 6 and 10 is 0.012.
    - `passage_vec`, k=100: 16× faster.
    - `passage_bits` (Hamming), k=10: 32× faster.
  - **Cosine has room to spare.** Its bound at 384 dims is about 5e-5, some 200 times smaller than that gap.
  - **The risk is Euclidean.** Its bound at unit norms is about 0.014, close to such gaps. Tighten it first if a Euclidean collection widens in practice: bound the squared distance's error by the vectors' difference rather than by their norms.
- **Vulkan's speed depends on the driver's power states (M25 layer 3, 2026-11-04).** `bench_accel`: f32 cosine, 384 dims, k=10.

  | Vectors | CPU | CUDA (RTX 4060) | Vulkan, RTX 4060 | Vulkan, Intel UHD |
  |---|---|---|---|---|
  | 10,000 | 13.5 ms | 0.22 ms | 0.31 ms | 2.4 ms |
  | 100,000 | 141 ms | 0.84 ms | 8.8 ms | 7.6 ms |

  - **The step on the RTX 4060 comes when the mirror outgrows its 32 MB L2.** Between bursts of work, the NVIDIA driver drops to P8, with the memory clock at 405 MHz. A short Vulkan burst then reads VRAM at about 8 GB/s. With 1,000 queries in a row, the clocks reach P0 and 30,000 vectors average 0.80 ms. CUDA contexts hold a higher power state, so CUDA does not see this.
  - **On NVIDIA, `cdb_accel_open` already prefers CUDA.** For Vulkan elsewhere, measure under real request patterns before tuning. A keep-alive that holds the clocks up would cost power, which matters on laptops and phones.
  - **Per-search overhead** is about 0.25 ms (a submit, about 20 barriers and a fence wait). The bulk search of layer 4 spreads it over many queries.
- **Mobile: Android and iOS (question, 2026-11-04).** Can the accelerator run inside a mobile app? A phone is a host, not a micro-controller, so it builds the host profile (spec 17), and spec 18 applies.
  - **Android: probably, through the Vulkan backend as it is.** Vulkan is Android's own GPU interface (`libvulkan.so` in the NDK), and the SPIR-V is checked in, so a device needs no shader compiler.
    - The backend needs Vulkan 1.2 with buffer device addresses. Recent phones have that, and a device without it declines, so its searches run on the CPU.
    - Still to do: a cross-compiled build of `combolite_accel` with the NDK. Then `test_accel_vulkan` on real phones (Adreno, Mali, Xclipse), to check their rounding against the bound and their limits (workgroup size 256, 16 KB of shared memory).
    - Power and thermals weigh more than on a desktop. Phones share memory between CPU and GPU, so `accel_mem` comes out of the app's RAM. For small collections the CPU path may be the better choice. Measure first.
  - **iOS: not as it is.** Apple has no Vulkan driver, only Metal. Two ways in:
    - **MoltenVK**, which runs Vulkan on Metal. The instance must enable portability enumeration (`VK_KHR_portability_enumeration`), which the backend does not do yet. Buffer device addresses must be available on the target GPUs. And Metal's fast math must be off, because it gives up the correctly rounded adds and multiplies that the float bound relies on. Verify all three on devices.
    - **A Metal backend of our own**: kernels in Metal Shading Language, behind the same `cdb_env` hook. It would be a fourth backend in `combolite_accel`, and would need the user's decision as a new dependency and layer.
- **Coordinator memory.** `cdb_coord` tracks up to 64 peers inside `cdb_db`, about 5 KB, even on hosts configured for one or two coordinators. Size it from `max_coords` in caller memory when micro-controller builds start.
- **Full-text token memory.** The rule that a word is never cut needs 2 × (`doc_max` + page) tokens, about 32 MB at `doc_max` = 1 MB. A word with its separator takes at least 2 bytes, so the bound could be halved. Check the word splitter first. The GutenFree Diet demo shows the cost: at `doc_max` = 8 MB the tokens are about 260 MB of the demo's 330 MB of engine buffers (about 41 bytes per byte of `doc_max`, 32 of them tokens).
- **Large transactions and pager memory.** Within one commit, the pager holds dirty page-map nodes in the memory the caller gives it. A whole book in one save fits easily (tested: 1 MB, 30,000 distinct words, 16 nodes, 4 KB pages). A bulk import of many large documents should commit every few documents.
- **ARM CRC-32C path.** It is only compiled when targeting little-endian ARMv8 with CRC, and it has never been built or run. Test it on ARM hardware or under an emulator.
- **Mutation checks on M13's reachability check.** The other M13 mutation checks ran; this new code hasn't had any yet.
- **Search-path threshold (spec 7.7).** The provisional 10,000 should be revisited with real embeddings and larger collections.
- **Host benchmarking tool.** A tool that measures a host, run on the host itself or from a workstation against a remote host, and recommends thresholds for a balance of memory and speed. Values to tune:
  - the open-store cap (`cdb_db_config.max_open`), the chunk size of 4, and how long idle stores stay open;
  - the vector search-path threshold;
  - pager nodes and buffer sizes;
  - the device-class table (spec 2.8).

  Until it exists, the per-class caps in `src/cdb_db.h` are estimates: 4 for a micro-controller (in caller memory), 8 for a single-board computer, 16 for a budget PC, and no cap for a workstation or server.
- **Open-store classes vs the device-class table.** The open-store advice uses five classes: micro-controller, single-board computer, budget PC, workstation, server. Spec 2.8's table has small micro-controllers, micro-controllers with PSRAM, single-board computers, desktops and workstations, and servers. Align the two when the table gets its values.
- **Time-series limits chosen in M16, to confirm.** At most 8 tags and 16 value fields per series, 1,024 points per block, and one append batch per journal record (at most a page). The series key is limited by the keys index: about 119 bytes on 1 KB pages and about 500 on 4 KB pages. For Gutenberg metrics, use eBook numbers as tags rather than titles.
- **Shared test toolkit (pinned until more modules are built).** The test suites repeat the same setup: static buffers sized by hand, the database config, the fault environment, open, close and reopen helpers, and the cap and need_open handling. Changing the document buffer size meant editing eight files. Gather it into a common test library that suites call, once enough module code exists to see the full set of patterns.
- **Table limits chosen in M17, to confirm.** 64 columns, 8 indexes, 8 columns per index, and 1 KB of defaults per table. Decimal, array, object and vector columns cannot be indexed yet, because the key encoding has no form for them.
- **Table creation is several catalog commits.** A crash part-way leaves a table without columns: row calls refuse it with CDB_E_STATE, and it can be dropped. Creating a unique index that meets a duplicate also leaves the new index's pages unreferenced, which the reclaim pass would free.
- **What M21–M23 left short of spec 11 (audited against the code, 2026-11-05).** The spec-review list of query and procedure gaps was M21–M23's scope. Done: tri-state NULLs (`NOT`, `AND`, `OR`, comparisons in `src/cdb_eval.c`), numeric coercion in comparisons, `Unnest`, the bytecode ISA with mutation calls, and the memory bound on `Sort` and `Aggregate` (`CDB_E_OUT_OF_BUDGET`; see the executor entry below for the 256-row cap). Still short:
  - **`Window` only tags rows.** It adds `$window_start` and `$window_end` for a tumbling window of `size_ns`; there are no sliding windows and no per-window aggregation.
  - **`AsofJoin` ignores its match condition** (`match_cond_wire` is decoded but never evaluated) and rescans the right input for every left row.
  - **No rollup pushdown:** nothing reads time-series rollup chunks for a query.
  - **`Traverse` does not walk links** (see the executor entry below), so cycle detection and the path schema are not there either.
  - **No literal dot in a CMBQL field name:** the compiler turns every dot of an unresolved path into 0x1F, and the lexer has no quoted-identifier form. The path is also cut silently at 255 bytes.
  - **Cursor isolation during writes** (a procedure cursor that sees its own transaction's later writes, or not): nothing in `cdb_exec` or `cdb_proc` addresses it, and no test covers it.
- **The query executor's live operators were stubs (found through the studio console, 2026-11-03).** Every `cdb_exec` test ran on mock rows with no database, so the live paths never ran. *Fixed on `fix_exec-live-scan`:* Scan reads table rows (`cdb_table_scan` in entry order, `cdb_table_get`) and documents (`cdb_doc_next_live`, `cdb_doc_get`) into a record buffer of `CDB_EXEC_SCAN_RECORD_PAGES` pages, and skips deleted entries; field lookups that a row's output fields do not answer fall through to the record through the field dictionary; Sort and Aggregate copy what they buffer into their own pool (`pin_row`, `pin_value`), because a scan's record buffer and the evaluator's scratch are overwritten by the next row; a plain-field group key keeps its field's name. `tests/test_exec_live.c` runs these on a real database. *Still open:*
  - **TextSearch, VectorSearch and Traverse do not search.** They pass their input rows through with a made-up score (1.0, 0.95) or depth (`min_hops`); HybridRank fuses those. They need to call `cdb_doc_search`, `cdb_vec_search_mode` and the link indexes, each with a live-database test.
  - **Vector, time-series and link-store scans** still count entry IDs up to the catalog's `next_entry_id`, which lags an open store; vectors yield no record (no `$vector`), time series and links yield bare IDs.
  - **Sort holds at most `CDB_EXEC_DEFAULT_BUFFERED_ROWS` (256) rows,** so `ORDER BY` over a large store fails with `CDB_E_OUT_OF_BUDGET`. Sort followed by Limit could keep only the top N.
  - **The text compiler has no aggregates:** `count(*)` gives "Unknown function call" and `| aggregate(...)` "Unknown pipeline stage", though spec 11's grammar has GROUP BY and AGGREGATE.
  - **A row holds at most `CDB_EXEC_MAX_FIELDS` (16) output fields,** fewer than some tables have columns (GutenFree's catalog has 17).
- **Active keys are only in the spec (found while reorganizing GutenFree, 2026-11-03).** Spec 6.2 defines keys the application assigns, such as `1342/0005`, but nothing implements them: no uniqueness, no lookup by key. The demo stores passage keys as an ordinary field, so finding one means a scan. Tables come close (a primary key is unique and found directly), but a table cannot carry a full-text index, so passages stay a document pool.
- **A table cannot have a full-text index.** `cdb_doc_create_full_text` and `cdb_doc_search` refuse a table's pool (`doc_pool` checks `is_table`).
- **Savepoints: what `fix_proc-savepoints` leaves open (spec 11.12.3, 14.4.9, 2026-11-05).**
  - **Rotation's copy of a transaction with rollbacks is untested.** The rebase copy (14.4.7 rule 2) leaves out dropped records and clears the commit flag, but only a second live process reaches that path, and `tests/test_savepoint.c` runs one.
  - **A mutation that fails after journaling** is rolled back to an internal savepoint, so it makes all its changes or none. No mutation was found that journals and then fails logically, so no test reaches that rollback.
  - **The replication export** (`cdb_db_crdt_export_slice`) now walks committed transactions as replay does, which also stops it exporting aborted transactions. `test_crdt` passes; no test exports a transaction with a rollback.
  - **Another process's collision scan** does not look for rollback records, so a dropped change can cause a needless collision (by design, 14.4.9 rule 7).
- **Readers of an open transaction's journal ignore encryption (found 2026-11-05).** The document, link, time-series and vector readers of the transaction's own records (`replay_tx`, `cdb_doc_tx_entries`, `tx_next` in `cdb_link.c` and `cdb_ts.c`, `live_in_tx` in `cdb_vec.c`) call `cdb_journal_read`, not `cdb_db.c`'s `read_record`, which finds an enclave's key. A record journaled encrypted for an enclave store should then fail with `CDB_E_SECURITY` once a reader gets to it, for example after the document stage overflows. Not reproduced yet.
- **Reserved words cannot name fields in CMBQL.** The lexer reserves 132 words, among them `type`, `key`, `role`, `time`, `data`, `weight`, `source`, `summary`, `model` and `age`, so `where type = "Text"` fails on a column named `type`. The compiler should take a keyword as a field name where an expression expects one.

## Done

Entries closed with nothing left open, one line each. Git history keeps their full text.

- **A second process could open a database already in use** (demo, 2026-10-30): M19 joins the coordinators at open and lets several processes share the database (spec 5.9).
- **An idle process was taken for dead** (M19 layer 1): M20's background heartbeat thread, when the environment has thread hooks; `cdb_db_tick` without them.
- **Unique checks were quadratic per insert** (demo, 2026-10-30): staged rows are found through a hash on (store, entry) (`db->stage_buckets`) and decoded at their offset (`staged_off`).
- **B+tree descent re-read, re-verified and fully decoded every page** (demo profile, 2026-10-30): the caller-memory data page cache (`page_cache` in `cdb_pager_config`) and in-place node search in `src/cdb_index.c`.
- **Link checks and unique checks read the transaction's own records back** (linear per check): hashed lookups through `idx_stage` in `src/cdb_db.c`.
- **Nibblet rewrites on time-series appends** slowed `test_ts` from 4.4 s to 12.4 s: `cdb_nib_rewrite` rewrites in place.
- **Overflow-chain tails as nibblets** (spec 16.5, kind 4): `cdb_overflow_*_db`, `tests/test_overflow_nib.c`.
- **Space of dropped stores and indexes was never reclaimed:** `cdb_index_drop`, `cdb_nib_sweep_page`, `cdb_db_reclaim`, called from `cdb_db_compact`.
- **Time series still to come** (audit series 10.7 and 12.7, replaced-value records 10.6, retention, downsampling 10.4, links to series and points 6.1): `src/cdb_audit.c`, `src/cdb_ts.c`, `src/cdb_link.c`.
- **Links to other models** (spec 6.1, 6.11, 14.13): the 25-byte `cdb_ref`, `tests/test_link.c`.
- **Savepoint rollback kept the writes, and a failed mutation ended the procedure** (audit, 2026-11-05): `ROLLBACK TO` now journals a rollback record (14.4.9) and rewinds the staging; logical failures leave their status in the register (11.12.3). `fix_proc-savepoints`, `tests/test_savepoint.c` with a crash sweep.
- **Accelerator, M25 layers 1–4** (spec 18): refills on the worker pool (`accel_fill_task`), the reader pin held for catch-up (`publish_pin`), the CUDA and Vulkan backends, bulk search with `accel_min_bytes`, the faster f32 loop, and the graph refresh on the device, whose shortlists are rescored and certified on the CPU.
