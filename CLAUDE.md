# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**The spec is complete. Implementation in C99 has started.**

- **Spec.** The spec is in `db-combolite/docs/spec/`. Start with `README.md` there, which lists every section, what it depends on, and what is still open. Every spec section records its confirmed decisions; code follows the spec, and spec section numbers are cited in code comments.
- **Still open.** These items need measuring, not deciding: the device-class table values, the vector search-path thresholds, and how neuron weights are learned. Some crash-matrix rows still have to be written (spec 3.6).
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
| M7 | Catalog (spec 4, 14.3) | next |

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
- `third-party/`: vendored dependencies. Anything that reaches the core must stay compatible with microcontrollers.
- `governance/`: governance-category definitions and schemas, for the first consumer. Still empty.
- `datastore/`, `workspace/`, `lib/`, `release/`, `package/`: empty placeholders for sample databases, scratch work and distribution.
- `build/`: CMake output. Git ignores it.
