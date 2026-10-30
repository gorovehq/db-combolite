# GutenFree Diet

GutenFree Diet is an ncurses e-reader built on db-combolite, as a demonstration and a stress test. It uses every data model the engine has so far, on real data from [Project Gutenberg](https://www.gutenberg.org/):

| Feature | Engine model |
|---|---|
| Card catalog of about 80,000 ebooks (GUTINDEX plus `pg_catalog.csv`): filters, 7 sort orders | row-oriented table, 8 indexes |
| Book text as about 2 KB passages, a reader | document pool |
| Search across every stored book | full-text index (BM25) |
| Semantic search, "more like this" | vector collections (f32 and 1-bit), neighbour graph, links |
| Diagnostics: gauges for every operation, live probe load | time series |

The demo uses the engine's internal C API. There is no query language yet: that is milestones M20 and M21. **No database, book text or model is in this repository.** You build your own local database with the steps below.

## Requirements

- Linux or another POSIX system, CMake 3.16 or later, a C99 and a C++ compiler.
- ncurses with wide-character support. On Debian and Ubuntu: `sudo apt install libncurses-dev`.
- Python 3, for the download scripts (standard library only).
- About 1 GB of disk for a few dozen books with embeddings.

llama.cpp (MIT) is vendored in `demo/third-party/llama.cpp`, pinned to release b11701 (see `VENDORED.md`). It is used only to compute embeddings and is built only with the demo.

## Build

From `db-combolite/`:

```sh
cmake -S . -B build-demo-rel -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCDB_BUILD_DEMO=ON -DCDB_BUILD_TESTS=OFF
cmake --build build-demo-rel -j --target gutenfree
```

`-DCDB_BUILD_DEMO=ON` is required: the engine's default build never needs ncurses or llama.cpp. A Debug build (`-B build-demo`, without a build type) turns on AddressSanitizer and UBSan. That is good for finding bugs, but every timing it shows is about 9 times slower. The screens show which build is running.

## Get the data

The scripts fetch only from Gutenberg mirrors, never from www.gutenberg.org. Everything lands in `datastore/gutenberg/`, which git ignores.

```sh
# 1. The catalog index, GUTINDEX.ALL (about 9 MB).
python3 demo/gutenfree/tools/gf_fetch.py --mirror https://gutenberg.pglaf.org/ --gutindex
# 2. Bibliographic data: genres (bookshelves), subjects, dates, LoCC classes.
python3 demo/gutenfree/tools/gf_fetch.py --mirror https://gutenberg.pglaf.org/ --catalog
# 3. The embedding model: bge-small-en-v1.5, Q8_0 GGUF, about 37 MB, SHA-256 checked.
python3 demo/gutenfree/tools/gf_model.py
# 4. Optional: a seeded random sample of 25 English books.
#    You can also add books one by one, or in batches, from the catalog screen.
python3 demo/gutenfree/tools/gf_fetch.py --mirror https://gutenberg.pglaf.org/ --count 25
```

## Build a database and run

```sh
G=./build-demo-rel/demo/gutenfree/gutenfree
$G ingest-index     # catalog: GUTINDEX, then pg_catalog.csv merged in (about 30 s)
$G ingest-books     # store the fetched books as passages, with full-text indexing
$G embed            # vectors, similar_to links, neighbour graph
$G                  # the e-reader
```

The database is `datastore/gutenberg/gutenfree.cdbarch` by default. Set `GUTENFREE_DB=/some/path` (no extension) to use another one. To start over, delete `<path>.cdbarch` and `<path>.cdbwork`.

**Only one process may open a database at a time.** Multi-process access is milestone M19, and until then nothing stops a second process from opening the file and corrupting it. Quit the e-reader before running a command against the same database.

## Using the e-reader

| Key | Screen |
|---|---|
| `c` | Card catalog. `/` filter, `o` change the sort order, Space queue a book, `A` queue every match, `F` fetch the queue, `a` add one book, Enter details, `g` go to an ebook number |
| `r` | Shelf, the books with text, opening in the reader |
| `s` | Full-text search |
| `v` | Semantic search: exact, neighbour-graph and 1-bit results compared |
| `e` | Embed stored books that have no vectors yet |
| `d` | Diagnostics: gauges, `L` live probe load, Enter explains a measure |
| `i` | Re-ingest the catalog |

## Other commands

```sh
$G filter genre="science fiction" type=Text author=verne   # the catalog filter, from the shell
$G probe 15                                                # 15 s of probe load
$G metrics                                                 # what every gauge holds
$G --help
```

## Licences and sources

- The demo code is MIT, like the rest of db-combolite.
- llama.cpp: MIT. ncurses: X11-style.
- bge-small-en-v1.5 (BAAI) and its GGUF conversion (CompendiumLabs): MIT. The model is downloaded at setup, never committed.
- Book texts and catalog data come from Project Gutenberg through its mirrors and are never committed. Check Project Gutenberg's licence and trademark terms before you redistribute any text you fetch.
