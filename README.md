# db-combolite

db-combolite is a lightweight, embeddable database library in C99, in the same class as SQLite. It stores several data models natively in one file:

- **Relational tables**, row-oriented, with constraints, secondary indexes and schema changes.
- **Document pools**, with revision history, patches, schema validation and full-text search (BM25).
- **Vector collections**, with exact search, an optional neighbour graph for approximate search, and binary (1-bit) vectors.
- **Links** between entries, with cardinality and delete rules.
- **Time series**, with bit-packed column blocks and late-arrival modes.

It is serverless: a library, a single file at rest, and no daemon. It is built for crash safety. The core never allocates on the heap; all memory comes from the caller, so that microcontroller builds stay possible later. The file format, storage engine and query layer are a clean-room design.

> **Status: early development.** The specification is complete. The implementation is following a milestone plan, and the file format is not yet stable. Not built yet: multi-process access (one process per database for now), the query core and query language, column-oriented tables, and cross-store transactions. Don't use it for data you care about.

## Layout

| Path | What |
|---|---|
| `db-combolite/docs/spec/` | The format and engine specification. Start with its `README.md`. |
| `db-combolite/include/`, `db-combolite/src/` | The library. |
| `db-combolite/tests/` | One Unity test executable per module, including crash-point sweeps on a fault-injecting device. |
| `db-combolite/demo/gutenfree/` | **GutenFree Diet**, an ncurses e-reader on Project Gutenberg data that exercises every model. See its [README](db-combolite/demo/gutenfree/README.md). |
| `db-combolite/docs/BACKLOG.md` | Deferred work and known issues, including the ones the demo found. |

## Build and test

From `db-combolite/`:

```sh
cmake -S . -B build                          # Debug: AddressSanitizer and UBSan on
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release   # optimised, sanitizers off
```

C99 only, warnings are errors, and the only third-party code in the core build is the Unity test framework. The demo is a separate target: see its README.

## Licence

MIT: see [`db-combolite/LICENSE`](db-combolite/LICENSE). Third-party components and data sources are listed in [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md).
