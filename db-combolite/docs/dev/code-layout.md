# Code layout

Moved from `CLAUDE.md`.

## Code layout

- `include/combolite/`: the public API (`cdb_common.h`, `cdb_value.h`, `cdb_storage.h`, `cdb_env.h`).
- `src/`: implementation and internal headers (`cdb_crc32c`, `cdb_varint`, `cdb_endian.h`, `cdb_value`). Tests include `src/` directly so they can test internal modules.
- **Engine layers, bottom up:**
  - `cdb_storage` and `cdb_env`: files and directories.
  - `cdb_header`, `cdb_page` and `cdb_pager`: the archive. The pager provides copy-on-write logical pages.
  - `cdb_overflow`: chains of overflow pages (type `0x0007`) for values longer than the page that refers to them. The owner keeps the length and first page.
  - `cdb_key` and `cdb_index`: order-preserving keys and the B+tree. Index nodes are logical pages, so the pager's copy-on-write sits underneath, and an index's root logical ID never changes. A value longer than `cdb_index_max_value()` goes to an overflow chain; keys are always inline.
  - `cdb_catroot` and `cdb_catalog`: the catalog. Root slots sit at location 0, and the body is one mirrored B+tree with prefixed keys. Catalog changes commit immediately.
  - `cdb_doc`: document pools. It provides insert, replace, patch and delete inside transactions, revision history, and the built-in apply hook. **The apply hook must never fail for logical reasons**, because it runs again during recovery. With `cdb_db_config.doc_max` set, a body may be that large and continues past its revision page in an overflow chain. The caller sizes buffers with `cdb_doc_buffers_size()`, `cdb_fts_buffers_size()` and `cdb_fts_tokens_needed()`, and `record_buf` must hold `doc_max` plus two pages. `tests/test_bigdoc.c` covers this; `test_doc.c` covers `doc_max` 0.
  - `cdb_fts`: full-text search. Splits words, maintains the terms, postings and lengths indexes from each document's state before and after a change (called from the document apply), and ranks searches with BM25.
  - `cdb_vec`: vector collections. Vectors are packed into vector blocks, with entries, blocks and sources indexes. The built-in apply (`cdb_doc_apply`) dispatches vector stores here. Hits report whether their source document has changed since the vector was written.
    - **Neighbour graph** (optional, `cdb_vec_create_graph`): each entry's exact K nearest neighbours, in graph, reverse and pending indexes (spec 14.12.4). Commits only queue work; reconciliation refreshes a budget of pending entries, and `cdb_vec_refresh` does the rest.
    - **Entry points:** spread points (about √N) plus reach points, which are added wherever a walk can't reach an entry, because an exact graph falls apart on clustered data.
    - **Search:** exact below `vec_exact_below` (default 10,000), otherwise a best-first walk from the entry points plus a direct pass over pending vectors.
    - `tests/bench_graph` (not run by ctest) measures build time, exact and approximate query time, and recall. Run it from the Release build.
  - `cdb_link`: links between entries (spec 6, 14.13.1). Link types are catalog objects of kind 2. Every link lives in the one link store (`$links`, model 5) in forward and reverse indexes. Changes are LINK and UNLINK records, checked as the open transaction sees things. Document and vector deletes call `cdb_link_check_delete` before journaling and `cdb_link_on_delete` after applying, and `cdb_db_drop_store` calls `cdb_link_on_drop`. The environment's optional `now` clock gives link times and commit timestamps.
  - Time series (spec 10, 14.11.1), in three layers:
    - `cdb_tsbits`: the bit codecs (buckets, delta-of-delta, XOR floats, dictionary indexes).
    - `cdb_tsblock`: a block payload. Its tests check that appending point by point gives exactly the bytes of building the block at once.
    - `cdb_ts`: the store, with keys, series, blocks and held indexes. It handles series by key, batched appends, late modes (hold parks points in the held index, and `cdb_ts_flush` folds them in at reconciliation), collisions, `drop_before` and range reads.
  - `cdb_nib`: nibblets (spec 16, 14.18), small records packed into shared slotted pages (type `0x0009`).
    - **Which records:** document and table revisions, and time-series blocks, up to a quarter page.
    - **Address:** (page, slot), packed in memory into one `uint64_t` with the top bit set. On disk an address is 8 or 10 bytes, and the length tells which.
    - **Back-reference:** each record carries its owner's store, its kind and the owner's index key.
    - **Free space:** a free-space index whose root is in the catalog root slot (offset 96) gives best-fit placement.
    - **Merging:** `cdb_doc_flush_pools` merges under-half-full pages at each reconciliation and tells each owner its record's new address (`cdb_doc_nib_moved`, `cdb_ts_nib_moved`). A record whose store was dropped is discarded then.
    - **Always on:** the two working pages are the last two pages of `doc_buffers`.
  - `cdb_table`: relational tables (spec 9.7–9.11, 14.10.1).
    - **Built on document pools:** a table is a document pool whose rows are encoded objects keyed by column field ID. The document calls refuse a table unless `db->via_table` is set, which the table layer does around its own calls.
    - **Columns** live in the schema history. A row is read through the current columns using the schema version it was written under: dropped columns are hidden, added columns read as their default, widened types are converted. Rows are never rewritten.
    - **Checks** for not-null, unique and primary keys happen when the change is made, against the indexes plus the open transaction's own rows (`cdb_doc_get_in_tx`, `cdb_doc_tx_entries`).
    - **Index upkeep:** `cdb_doc_apply` calls `cdb_table_apply_before` and `cdb_table_apply_after` around each change.
    - **History off:** `keep` N makes the document engine write only full revisions and free the older ones, and a delete removes the row.
    - **Tests:** `tests/test_table.c` and `tests/test_nib.c` cover this.
  - `cdb_schema`: checks schemas and validates documents against them. Validation happens in `cdb_doc` when a change is made, against the document as the open transaction sees it: from the staging cache (`doc_stage`), or, after the cache overflows, from the transaction's own journal records.
  - `cdb_journal` and `cdb_db`: transactions, reconciliation and recovery. Changes reach pages only through the `cdb_apply_fn` hook, and the data models will plug in there.
    - **Background heartbeat thread (M20):** autonomous thread in `cdb_db` keeping idle processes alive indefinitely; synchronizes database calls through re-entrant mutexes.
  - `cdb_worker`: fixed ring-buffer worker thread pool and task queue (spec 5.9.6, M20). Offloads heavy asynchronous maintenance tasks (`cdb_vec_refresh_async`) with zero dynamic heap allocation in the core engine; falls back to synchronous execution when thread hooks are NULL or worker threads are disabled.
  - `cdb_slots`: the open stores of each model (document pools, vector collections, time-series stores).
    - **Growth:** slots grow in chunks of 4, from `cfg.slot_mem` and then the environment's allocator, up to `cfg.max_open` (0: no cap).
    - **At the cap:** the least recently used store is closed. Its state is staged to the catalog first. A store the open transaction has used is never closed, nor a time-series store with held points.
    - **Shrinking:** reconciliation frees chunks idle since the previous reconciliation.
    - **Pointer lifetime:** a slot pointer is valid until the next open of the same model outside a transaction or operation scope. Code that holds one store while opening another of the same model (a join) must wrap the work in `cdb_db_op_begin`/`cdb_db_op_end`. Scopes nest. Each model's slot struct starts with `cdb_slot`, so its store is `p->slot.store_id`.
    - **Tests:**
      - The fault environment's allocator frees everything at reboot, as a crash would. `cdb_faultenv_live_allocs()` counts what is live.
      - ctest runs the multi-store suites a second time as `<suite>_cap1`, with `CDB_TEST_MAX_OPEN=1`. A test that needs several stores of one model in one transaction calls `need_open(n)`.
- `src/cdb_faultenv.*`: several fault devices on one shared crash clock, plus directory entries that are lost on crash unless their directory was synced. Test-only.
- `accel/`: `combolite_accel`, the optional accelerator library (spec 18), never part of the core. An application links it and installs its hook in `cdb_env`. Host profile only.
  - **Opening:** `cdb_accel_open` (`cdb_accel.h`) takes whichever backend this build has: CUDA first, then Vulkan's best device, ranked discrete, integrated, virtual. A device that runs on the CPU (llvmpipe) is used only with the `CDB_ACCEL_SOFTWARE` open flag.
  - **Shared code** (`accel/common/cax.c`): the device row shape, stored vectors as rows with the CPU path's norms, the entry ID to row map, and the error bound. The bound has a double epilogue (CUDA) and a float one (Vulkan).
  - **CUDA** (`accel/cuda/`, `-DCDB_ACCEL_CUDA=ON`). The host code (`cdb_accel_cuda.c`) is C99 on the CUDA runtime's and cuBLAS's C interfaces. The kernels (`cak_kernels.cu`) are a separate CUDA C++ static library, so the core's flags and sanitizers never reach nvcc.
    - **How it computes:** cuBLAS dot products in pedantic math, so no TF32. Our own top-K: unique 64-bit keys, a radix select and a bitonic sort. A worst-case error bound per query.
  - **Vulkan** (`accel/vulkan/`, `-DCDB_ACCEL_VULKAN=ON`). It needs Vulkan 1.2 with buffer device addresses, and nothing more.
    - **Shaders:** our own compute shaders in `accel/vulkan/shaders/`. Their SPIR-V is checked in as `cav_spirv.c` and `.h`, which `tools/gen_spirv.py` writes; never edit them by hand.
    - **Check:** ctest's `check_spirv` fails when the sources changed since they were generated.
  - **Tests:** `tests/accel_device.h` holds the cases every device must pass.
    - `test_accel_cuda` and `test_accel_vulkan` run them; the Vulkan one runs on every device, llvmpipe included through the flag. Each exits 77 (skipped) without a hardware device.
    - ASan needs `protect_shadow_gap=0` for CUDA, and `tests/lsan_drivers.supp` hides the drivers' libdbus allocations; ctest sets both.
    - `bench_accel` (not in ctest) times exact search on each path, also on a copy of an existing database (`--file`), and a graph refresh (`--refresh`; `bench_graph --file` times it on the CPU).
  - **Bulk search and the threshold:** `cdb_vec_search_bulk` searches many queries, 64 per device request and per hold of the lock. `accel_min_bytes` (default 1 MB) keeps requests that read less on the CPU. Graph refreshes use the device too, with results identical to the CPU's (`test_graph`, `accel_device.h`).
- `src/cdb_fakeaccel.*`: a fake accelerator (spec 18.11) for the `cdb_env` accelerator hook: the CPU's distances plus noise up to the bound it reports, and calls scriptable to decline or fail. Test-only, like the fault devices. ctest reruns the vector and graph suites with it as `<suite>_accel` (`CDB_TEST_ACCEL=1`); they must give exactly the CPU's results.
- **Locking.** Every public data call holds the database's re-entrant lock (`cdb_db_lock`) for its whole run, so worker-pool tasks (graph refresh, mirror refills) never overlap an application call. A new public call takes it too: wrap an unchanged static body. The lock is a no-op without thread hooks.
- `src/cdb_faultdev.*`: an in-memory device that simulates power loss: unsynced writes are dropped, kept in part or torn. It is built only into the `combolite_testing` library, never into the production library.
  - **Crash modes:** drop every unsynced write, keep an in-order prefix of them, tear a write partway, or reorder (only the latest write survives, the way a real disk can land writes out of order).
  - **Crash sweeps.** A test counts a workload's mutating operations, then crashes at each one in every mode, calls `cdb_faultdev_reboot()`, and checks what recovery sees. Every new write sequence needs a sweep, and the reorder mode is what catches a missing barrier.
- `tests/`: one Unity executable per module. Scratch files go in `CDB_TEST_TMPDIR`, which is the build's `tests/` directory.
- `demo/gutenfree/`: **GutenFree Diet**, an ncurses e-reader on Project Gutenberg data that exercises every model and stresses the engine. It is built only with `-DCDB_BUILD_DEMO=ON` and runs from its own folder, with its data in `data/` (git-ignored).
  - **Dependencies:** llama.cpp (embeddings) is vendored in `demo/third-party/llama.cpp`; see `VENDORED.md`.
  - **API:** it uses the internal headers in `src/`, as the tests do.
  - **Findings:** engine problems it turns up go in `docs/BACKLOG.md`.

## Directory intent (`db-combolite/`)

- `include/`, `src/`, `tests/`: see the code layout above.
- `docs/spec/`: the format specification.
- `tools/`: generators for checked-in source, such as the CRC-32C tables.
- `third-party/`: vendored dependencies. Anything that reaches the core must stay compatible with microcontrollers.
- `governance/`: governance-category definitions and schemas, for the first consumer. Still empty.
- `datastore/`, `workspace/`, `lib/`, `release/`, `package/`: empty placeholders for sample databases, scratch work and distribution.
- `build/`: CMake output. Git ignores it.
