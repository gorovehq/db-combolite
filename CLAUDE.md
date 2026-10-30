# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**The spec is complete. Implementation in C99 has started.**

- **Spec.** The spec is in `db-combolite/docs/spec/`. Start with `README.md` there, which lists every section, what it depends on, and what is still open. Every spec section records its confirmed decisions; code follows the spec, and spec section numbers are cited in code comments.
- **Still open.** These items need measuring, not deciding: the device-class table values, the vector search-path threshold (provisionally 10,000 vectors; spec 7.7), and how neuron weights are learned. Some crash-matrix rows still have to be written (spec 3.6).
- **Implementation.** Work follows the milestone plan below. The user reviews each milestone before the next one starts.

| Milestone | Scope | Status |
|---|---|---|
| M0 | Build skeleton, warnings, sanitizers, test runner | done |
| M1 | CRC32C, varints, byte-order helpers, value encoding (spec 14.6) | done |
| M2 | Storage backend interface, host-file backend, fault-injecting backend (spec 1.11.1) | done |
| M3 | File header (spec 1.9, 14.1) | done |
| M4 | Pages, anchor map, free-space list (spec 13, 14.2, 14.7) | done |
| M5 | Delta tracks, transactions, reconciliation, recovery; the crash-point matrix becomes the main test suite (spec 3, 14.4) | done (single process) |
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

## Commands

All commands run from `db-combolite/`:

```sh
cmake -S . -B build                          # configure (Debug by default: ASan and UBSan on)
cmake --build build -j                       # build the library and the tests
ctest --test-dir build --output-on-failure   # run every test
ctest --test-dir build -R test_value         # run one test executable
./build/tests/test_value                     # run one executable directly (Unity output)
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release   # optimised build, sanitizers off
```

## Git workflow

- **Never commit directly to `main`.** Every piece of work goes on its own branch, named with a type prefix:

  | Prefix | Use |
  |---|---|
  | `feat_` | new functionality, such as a milestone (`feat_m4-pages-anchor-map`) |
  | `fix_` | bug fixes |
  | `test_` | test-only changes |
  | `docs_` | spec and documentation changes |
  | `merge_` | integration branches that bring other branches together |
  | `refactor_` | restructuring that does not change behaviour |
  | `build_` | build system, CMake, CI and toolchain changes |

- **Branch names** take the form `<prefix><short-kebab-description>`.
- **Commit and merge only when the user asks.** Merging into `main` is the user's decision.
- **Merges into `main` use `--no-ff`,** so every branch shows up in the history as a single unit.
- **Keep merged branches.** Don't delete them; they are the audit trail of how development happened.
- **No AI attribution in commits.** Don't add `Co-Authored-By: Claude` trailers or similar lines to commit messages or PR descriptions. Attribution lives only in `ATTRIBUTIONS.md` at the repo root. Add each new third-party dependency to the table there.

## Code rules

These were agreed with the user:

- **C99 only.** `CMAKE_C_EXTENSIONS` is off.
- **Warnings are errors.** The flags include `-Wconversion`, `-Wsign-conversion` and `-Wvla`, so write explicit casts and never use VLAs.
- **No heap allocation in the core.** Memory comes from the caller, through buffers now and an allocator interface later, so microcontroller builds can use static pools. Readers return views into caller-owned buffers.
- **Fallible functions return `cdb_status`**, defined in `include/combolite/cdb_common.h`. The value writer's errors are sticky.
- **Explicit byte order.** Every multi-byte on-disk field goes through `src/cdb_endian.h`, using the file's declared byte order. Never cast raw bytes to integers.
- **Canonical encoding.** Writers produce the one canonical form, and validators reject anything else (spec 14.6.5).
- **Tests use Unity**, vendored in `third-party/unity/` (v2.6.1, MIT). Add a test executable with `cdb_add_test(<name>)` in `tests/CMakeLists.txt`.

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
  - `cdb_schema`: checks schemas and validates documents against them. Validation happens in `cdb_doc` when a change is made, against the document as the open transaction sees it: from the staging cache (`doc_stage`), or, after the cache overflows, from the transaction's own journal records.
  - `cdb_journal` and `cdb_db`: transactions, reconciliation and recovery. Changes reach pages only through the `cdb_apply_fn` hook, and the data models will plug in there.
- `src/cdb_faultenv.*`: several fault devices on one shared crash clock, plus directory entries that are lost on crash unless their directory was synced. Test-only.
- `src/cdb_faultdev.*`: an in-memory device that simulates power loss: unsynced writes are dropped, kept in part or torn. It is built only into the `combolite_testing` library, never into the production library.
  - **Crash modes:** drop every unsynced write, keep an in-order prefix of them, tear a write partway, or reorder (only the latest write survives, the way a real disk can land writes out of order).
  - **Crash sweeps.** A test counts a workload's mutating operations, then crashes at each one in every mode, calls `cdb_faultdev_reboot()`, and checks what recovery sees. Every new write sequence needs a sweep, and the reorder mode is what catches a missing barrier.
- `tests/`: one Unity executable per module. Scratch files go in `CDB_TEST_TMPDIR`, which is the build's `tests/` directory.

## What db-combolite is

A lightweight, portable, embeddable database (SQLite-class footprint) that supports these data models natively in one store:

- **Relational** tables and queries
- **Document pools** (schemaless documents)
- **Vector** storage and similarity retrieval
- **Matrix ordinality / cardinality**: ordered and counted relationships between entries
- **Stored procedures** and **cross-model queries** (one query can join relational, document and vector data)
- **Active and passive keying** of entries
- **Cardinal → Vector → Ordinal mapping** and **Neuron Mapping**

The confirmed meanings of these terms are in `docs/spec/06-keys-links.md`:

- **Active keys** are assigned explicitly by the user or application. **Passive keys** are derived by the database (content hash, auto-id or embedding).
- **Cardinal→Vector→Ordinal** maps an entry's identity to its embedding and then to its similarity rank. It is a stored structure that is kept up to date.
- **Cardinality** means relationship cardinality (1:1, 1:N, N:M).
- **Neuron mapping** is a graph of links between entries whose weights are learned.

### Hard constraints (these drive every design decision)

1. **Serverless, portable and embeddable.** It ships as a library, and at rest the store is a single file. There is never a daemon, broker or server process. Sessions, CRUD, multi-tenancy, reconciliation and coordination between processes are all done by the library code running inside each application. Processes coordinate only through the files themselves. Each library instance runs an in-process **coordinator** that plays the role a server would. The first coordinator to start is primary, and later ones are subordinate (see spec 1.8.0). Scope is a **single host**. There is no clustering, and no multi-machine access to one database.
2. **Microcontroller support is a long-term goal, not a current target.** The eventual aim is to package the library for PlatformIO, ESP-IDF and Arduino. The current focus is defining the **storage file format and its mechanics** as a standard. Don't add MCU build targets or a HAL yet. Do avoid format decisions that would block a constrained implementation later, such as unbounded page sizes or a format that can only be read by mapping the whole file into memory.
3. **Scales up to production.** It needs crash safety (journal or WAL), integrity checks and concurrent readers.
4. **Not a competitor to Postgres, MariaDB, Oracle, MySQL or MSSQL.** Turn down any feature that only makes sense for a server-class RDBMS.
5. **Clean-room spec and implementation.** The file format, storage engine and query layer are new designs optimized for this project's use case. Ideas from existing systems (SQLite's pager, LMDB, HNSW and so on) can be studied and cited as background in design docs. Don't vendor, port, wrap or copy their code, and don't make the format compatible with theirs. `third-party/` is not for storage or index engines. Approved dependencies:
   - **Unity**, for tests.
   - **mbedTLS**, for cryptography, behind a crypto interface of our own. Never hand-roll cryptography.
   - **ICU**, for language-aware collation on hosts that are not microcontrollers (spec 14.8.6).

### Role in the larger effort

db-combolite is the storage component of a future **portable governance framework for agentic development**, so a new project can start with governance and agent memory already in place. It must also stay **distributable on its own**. Keep governance-specific logic out of the core engine. Governance is a *consumer* (a schema or package on top of the engine).

The first consumer is a project-governance store with these categories: **Architecture, Development, Security, Deployment, Contribution, CI/CD**. It also stores agentic memory: facts, links, retrieval by embedding.

## How spec work proceeds

Write the spec one section at a time, and stop before each section. First explain what the section covers and which decisions it involves, along with their trade-offs. Then ask the user for direction. Don't write design choices into the spec that the user hasn't confirmed. Use placeholders when the user is still undecided.

## Directory intent (`db-combolite/`)

- `include/`, `src/`, `tests/`: see the code layout above.
- `docs/spec/`: the format specification.
- `tools/`: generators for checked-in source, such as the CRC-32C tables.
- `third-party/`: vendored dependencies. Anything that reaches the core must stay compatible with microcontrollers.
- `governance/`: governance-category definitions and schemas, for the first consumer. Still empty.
- `datastore/`, `workspace/`, `lib/`, `release/`, `package/`: empty placeholders for sample databases, scratch work and distribution.
- `build/`: CMake output. Git ignores it.
