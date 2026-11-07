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

The binary goes into this folder, next to its scripts, as `./gutenfree`. `-DCDB_BUILD_DEMO=ON` is required: the engine's default build never needs ncurses or llama.cpp. A Debug build (`-B build-demo`, without a build type) turns on AddressSanitizer and UBSan. That is good for finding bugs, but every timing it shows is about 9 times slower. The screens show which build is running, and the build made last is the one in this folder.

### With a GPU

The engine's optional accelerator (spec 18) can run vector searches and neighbour-graph refreshes on a GPU. Add `-DCDB_ACCEL_CUDA=ON` (NVIDIA, needs the CUDA toolkit) and/or `-DCDB_ACCEL_VULKAN=ON` (any Vulkan 1.2 GPU):

```sh
cmake -S . -B build-demo-accel -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCDB_BUILD_DEMO=ON -DCDB_BUILD_TESTS=OFF \
      -DCDB_ACCEL_CUDA=ON -DCDB_ACCEL_VULKAN=ON -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-demo-accel -j --target gutenfree
```

- **Embeddings on the GPU too:** the engine's accelerator covers vector searches and graph refreshes only; the embedding model runs in llama.cpp, which uses the GPU only when built with `-DGGML_CUDA=ON` (or `-DGGML_VULKAN=ON`). Without it, `embed` runs on the CPU. Set `CMAKE_CUDA_ARCHITECTURES` to your GPU's (89 is an RTX 40-series); leaving it out builds llama.cpp's kernels for every architecture, which takes much longer. `nvcc` must be on `PATH` (`/usr/local/cuda/bin`).

- **Which device:** the demo uses CUDA when there is an NVIDIA GPU, otherwise the best Vulkan GPU. The main screen, the diagnostics screen and every command show which one, and the diagnostics screen counts the searches it took.
- **Results:** they are the same with or without it; only the time changes.
- **Turning it off:** `GUTENFREE_ACCEL=off` runs without it. `GUTENFREE_ACCEL=software` also allows a device that emulates a GPU on the CPU, such as llvmpipe, which is usually slower than having none.
- **Measured** on an RTX 4060 laptop, building the graph of 15,521 passages (`bench-graph`): 107 s with CUDA, 150 s without.

## Get the data and run

Everything runs from this folder, and everything it downloads or builds lands in `data/`, which git ignores. The scripts fetch only from a Gutenberg mirror, never from www.gutenberg.org. The default is Project Gutenberg's own pglaf.org mirror; `--mirror URL` picks another one from MIRRORS.ALL.

```sh
cd demo/gutenfree
python3 gf_fetch.py --gutindex    # the catalog index, GUTINDEX.ALL (about 9 MB)
python3 gf_fetch.py --catalog     # genres, subjects, dates, LoCC classes (about 21 MB)
python3 gf_model.py               # embedding model bge-small-en-v1.5, about 37 MB, SHA-256 checked
python3 gf_fetch.py --count 25    # optional: 25 random English books (or add them in the app)

./gutenfree ingest-index          # catalog: GUTINDEX, then pg_catalog.csv merged in (about 30 s)
./gutenfree ingest-books          # store the fetched books as passages, with full-text indexing
./gutenfree embed                 # vectors, similar_to links, neighbour graph
./gutenfree                       # the e-reader
```

The database is `data/gutenfree.cdbarch`. Set `GUTENFREE_DB=/some/path` (no extension) to use another one. To start over, delete `data/gutenfree.cdbarch` and `data/gutenfree.cdbwork`.

**Only one process may open a database at a time.** Multi-process access is milestone M19, and until then nothing stops a second process from opening the file and corrupting it. Quit the e-reader before running a command against the same database.

## Using the e-reader

| Key | Screen |
|---|---|
| `c` | Card catalog. `/` filter, `o` change the sort order, Space queue a book, `A` queue every match, `f` the download queue (runs in the background), Enter details, `g` go to an ebook number |
| `f` | Download queue: start it, then use the app while it runs; the status bar shows progress |
| F2 or Ctrl-D | Diagnostics, from any screen |
| `r` | Shelf, the books with text, opening in the reader |
| `s` | Full-text search |
| `v` | Semantic search: exact, neighbour-graph and 1-bit results compared |
| `e` | Embed stored books that have no vectors yet |
| `d` | Diagnostics: gauges, `L` live probe load, Enter explains a measure |
| `i` | Re-ingest the catalog |

## Other commands

```sh
./gutenfree filter genre="science fiction" type=Text author=verne   # the catalog filter, from the shell
./gutenfree probe 15                                                # 15 s of probe load: lookups, reads, scans, text and vector search
./gutenfree metrics                                                 # every point of each gauge: count, min, mean, max, last
./gutenfree metrics vec.search_graph.fresh fts.search               # an operation's p50, p99 and mean across runs
./gutenfree graph                                                   # refresh the passages' neighbour graph
./gutenfree link-similar                                            # link similar books (one bulk search)
./gutenfree bench-graph data/copy                                   # time a whole graph build, on a copy of the database
GUTENFREE_WORKERS=4 ./gutenfree pipeline 100                        # 100 new books: parallel downloads, 4 writer processes, GPU embedding
python3 gf_fetch.py --rename-existing                               # give books saved under <number>.txt their title names
./gutenfree --help
```

**The pipeline** is the stress test. It fetches books the database has never seen (8 downloads at once, no pause, backing off on 429 and 503) and hands each to one of `GUTENFREE_WORKERS` writer processes the moment it lands. Each writer has its own database handle, so the engine sees that many writers at once; each stores its book, embeds it on the GPU and commits the vectors. One book in ten is held out as unseen search queries. Engine refusals are retried three times and counted per status. Books are saved as `<Title>.txt` in characters no shell needs escaped; `manifest.tsv` maps numbers to files.

**Measurements.** Every command above, and the ingest and embed commands, ends with a report: each engine operation it made, timed one call at a time (count, min, p50, p95, p99, max, calls per second), the archive commits and file growth, and the accelerator's share of the searches. The summaries are stored, so `metrics` shows them later. `embed` ends with a search check: 200 stored passages and 50 fresh phrases (`gf_queries.c`, embedded as semantic search does) each searched exactly, through the neighbour graph and through the 1-bit vectors, with how many of the exact top 10 the other two find. Stored passages find their own precomputed neighbours, so the fresh queries are the honest score. Timings are only meaningful in a RelWithDebInfo or Release build; the report names the build.

## Licences and sources

- The demo code is MIT, like the rest of db-combolite.
- llama.cpp: MIT. ncurses: X11-style.
- bge-small-en-v1.5 (BAAI) and its GGUF conversion (CompendiumLabs): MIT. The model is downloaded at setup, never committed.
- Book texts and catalog data come from Project Gutenberg through its mirrors and are never committed. Check Project Gutenberg's licence and trademark terms before you redistribute any text you fetch.
