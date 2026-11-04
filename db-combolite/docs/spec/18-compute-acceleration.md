# Section 18: Compute acceleration

Status: **Draft. The decisions below are confirmed; the open questions are not.**

Depends on: 5.9 (processes and reconciliation), 6.8 and 14.12.4 (the neighbour graph), 7 (vector collections), 17 (build profiles).

## Purpose

Some of the engine's vector work is plain arithmetic over many vectors at once: finding each pending entry's K nearest neighbours when the neighbour graph is refreshed, exact search, and the direct pass over pending vectors in approximate search. On a host with a GPU, that arithmetic can run there.

This section defines an optional **accelerator**: what the engine may hand to it, how results stay the same as on the CPU, and how it stays out of the portable core. It changes nothing in the file format.

Embedding, which turns text into vectors, is not engine work. An application computes its embeddings and may use its own GPU for that (GutenFree Diet uses llama.cpp's backends).

## Confirmed decisions

### 18.1 Host only, optional, outside the core

- **Profiles.** The accelerator exists only in the host profile (Section 17). An mcu build has no accelerator hook.
- **Optional.** An engine without an accelerator, or whose accelerator declines a request, uses the CPU path. Neither case is ever an error. This matches the exception for neighbour-graph maintenance in 17.3.
- **Outside the core.** The core gains only an optional hook in the environment (`cdb_env`), as it did for threads (M20). The GPU code lives in a separate optional library, `combolite_accel`, never in `src/`. An application links it and passes its hook in the environment. The core never calls a GPU API itself, and the rule against heap allocation in the core is unchanged.
- **Build.** The accelerator library is built for **CUDA**, **Vulkan**, both or neither, chosen at build time. With both, the hook uses whichever device it finds at run time.

### 18.2 A resident mirror and top-K requests

- **Mirror.** For a collection it accelerates, the accelerator keeps a copy of the collection's vectors in device memory, keyed by entry ID. The mirror is a cache. The vector blocks (14.12) are always the authority, and the mirror is never written to the file.
  - It is filled from the vector blocks.
  - It is told of every insert, replace and delete the engine applies to the collection, so it changes in step with the blocks.
  - Whenever the engine cannot be sure the mirror is current, it drops the mirror and fills it again, or uses the CPU path.
- **Requests.** The engine asks: for each of these query vectors, return the nearest candidates in the collection under this measure. One request replaces a refresh batch's pass over every vector block (14.12.4), or an exact search's pass.

### 18.3 The GPU shortlists, the CPU decides

GPU arithmetic adds in a different order from the CPU, so its distances can differ in the last bits.

- **Shortlist.** The accelerator returns more candidates than were asked for: K plus a margin.
- **Rescoring.** The engine reads each candidate's vector from the vector blocks, computes its distance on the CPU exactly as the CPU path does, and chooses the final K.

So stored neighbour lists and their distances, and search results, are the same as a CPU-only build would produce. The one exception is when a true neighbour falls outside the shortlist. The margin is sized to make that rare (18.6), and the coverage checks of 14.12.4 run on the CPU as before.

### 18.4 What may be accelerated

- **Neighbour-graph refresh batches.** The pass that finds each pending entry's K nearest (14.12.4). Coverage checks and their walks stay on the CPU: they follow lists through index pages, which is not arithmetic.
- **Exact vector search.** Searches below `vec_exact_below` (7.7), and the direct pass over pending vectors in approximate search (7.4).
- **Bulk search.** A new batched call that searches many query vectors in one request, for work such as building `similar_to`-style links, or an agent retrieving memories for several prompts. It is defined for both paths: without an accelerator it runs the same searches on the CPU.

### 18.5 Kernels and dependencies

- **CUDA.** Distances are computed as a matrix product, ‖q‖² + ‖v‖² − 2 q·v, with **cuBLAS**, NVIDIA's math library. The top-K selection is our own kernel.
- **Vulkan.** Vulkan has no standard math library, so the distance and top-K kernels are our own compute shaders.
- **Clean room.** cuBLAS and the Vulkan loader are math and device libraries, not storage or index engines, so they are allowed. Each is added to `ATTRIBUTIONS.md` when it is first used.

### 18.6 Keeping a mirror current across processes

A search runs outside any transaction, so it sees the archive at a committed sequence plus the process's own applied commits (5.9.1). A mirror must reflect the same state.

- **Changes this process applies.** Every change reaches the process's pages through the apply hook: its own commits, their re-application at a rebase, and, when the process holds the baton, every slot's transactions replayed at reconciliation (5.9.1). The engine tells the mirror of each one as it is applied, so these never leave a mirror stale.
- **Changes another process reconciles.** These appear only when the archive moves under the process and it rebases (5.9.1). To tell which collections they touched, each vector collection records a **change stamp**: the committed sequence of the reconciliation that last changed it. This is an addition to the format (14.12).
  - A mirror records the stamp of the state it reflects.
  - At a rebase, the engine compares each mirrored collection's stamp with the archive's. A mirror whose collection changed is stale, and the others stay.
  - A process that reconciled the changes itself applied them through the hook, so it records the new stamps without dropping anything.
- **A stale or empty mirror.** A request that finds its collection's mirror stale, or not filled yet, takes the CPU path, so its results and latency are those of a build without an accelerator. The mirror is filled again from the vector blocks on the worker pool (5.9.6), for later requests. Results are the same either way (18.3).

### 18.7 The accelerator's calls

- **Placement.** The environment (`cdb_env`) carries an optional accelerator next to its operations: a table of calls, `cdb_accel_ops`, and the accelerator's own state, passed back on every call. The accelerator library keeps its state apart from the environment's, so it combines with any environment, for example the host environment with CUDA, or a test environment with a fake accelerator.
- **Calls.** Every buffer belongs to the caller, and every fallible call returns `cdb_status`:
  - `mirror_open` creates a mirror for one collection, given its store, element type, dimensions and measure, and how many vectors to expect.
  - `mirror_put` inserts or replaces a batch of vectors, given their entry IDs. `mirror_del` removes a batch of entry IDs.
  - `mirror_close` drops a mirror.
  - `nearest` is given a batch of query vectors and a number of candidates wanted. For each query it returns up to that many entry IDs, nearest first, with the accelerator's distances. The distances are used only to measure the shortlist margin, never stored or returned: the CPU rescores (18.3).
- **Synchronous.** `nearest` returns when the shortlists are ready; the engine then rescores them. A refresh batch or a bulk search sends many queries in one call, which keeps the device busy.
- **Updates in batches.** During a commit, a rebase or a reconciliation, the engine lists the entry IDs of each mirrored collection that the run changes. When the run ends it sends one `mirror_put` batch, with the vectors read back from the blocks, and one `mirror_del` batch. If the list outgrows its memory, the mirror is marked stale instead and refilled (18.6).
- **Declining.** An accelerator may decline any call. A declined `mirror_open` leaves that collection on the CPU path. A failed `nearest` sends that request to the CPU path. A failed `mirror_put` or `mirror_del` marks the mirror stale. No accelerator failure ever reaches the caller of the engine.
- **Refilling and threads.** A stale mirror is refilled as a new mirror on the worker pool while the old one, if any, keeps serving requests; then the engine swaps them and closes the old one. So calls on different mirrors may overlap, and the accelerator must allow that. The engine never calls one mirror from two threads at once.

### 18.8 Device memory

The device may be shared with the application's own work, such as computing embeddings, so the engine's mirrors have a budget.

- **The budget.** The database configuration gets a field, `accel_mem`: the most device memory the engine's mirrors may use together. 0 leaves it to the accelerator's judgment. The engine counts a mirror as the bytes of its vectors. Within the budget, the accelerator may still decline `mirror_open` or `mirror_put` when the device is short (18.7).
- **A collection that does not fit.** All or nothing: a collection whose vectors exceed the budget, or whose mirror the accelerator declines, has no mirror and uses the CPU path. There are no partial mirrors.
- **Several collections.** A collection that needs a mirror evicts the least recently used mirrors until it fits, as the open-store slots close the least recently used store. A mirror that a request is using is never evicted.

## Open questions

- **The shortlist margin.** How many candidates beyond K, fixed or as a factor of K, to be set by measuring how often a true neighbour falls outside it on real data, such as GutenFree's passages.
- **Mirrors and growth.** A collection keeps growing after its mirror is made. Whether a `mirror_put` that would pass the budget evicts other mirrors first, or marks this mirror stale so the next fill decides again (18.8).
- **Where the change stamp lives (18.6).** Its place in a vector collection's on-disk state (14.12), and how a reader without the accelerator treats it (it can ignore it).
- **Refilling without a worker pool.** On a host whose environment has no thread hooks, the worker pool runs tasks at once (5.9.6), so an asynchronous refill would block the request after all. Whether such a host refills at the next reconciliation or `cdb_db_tick` instead.
- **Element types and measures.** Whether the first version covers only f32 vectors with the Euclidean, cosine and dot-product measures, leaving binary (Hamming) and other element types to the CPU.
- **Vulkan shaders.** How the compute shaders are built and checked in, for example SPIR-V generated by a tool in `tools/`, as the CRC tables are.
- **Testing.**
  - A CPU "fake accelerator" that implements the hook with deliberately different arithmetic, to prove rescoring makes results identical.
  - Device tests that run only where a GPU is present.
- **Profiling first.** Measured, a refresh slice of GutenFree took 22 s, while the arithmetic is estimated at under a second. Most of the time is probably in coverage walks and index work, which stay on the CPU. Profile a refresh before implementation, to show what acceleration gains and what else needs fixing.
