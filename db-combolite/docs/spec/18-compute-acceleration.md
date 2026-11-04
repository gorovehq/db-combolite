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

- **Shortlist.** The accelerator returns more candidates than were asked for (18.10).
- **Rescoring.** The engine reads each candidate's vector from the vector blocks, computes its distance on the CPU exactly as the CPU path does, and chooses the final K.
- **Certifying.** The engine proves that no vector left out of the shortlist could have been among the K nearest, and widens the shortlist until it can (18.10).

So stored neighbour lists and their distances, and search results, are always the same as a CPU-only build would produce. The coverage checks of 14.12.4 run on the CPU as before.

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
- **Changes another process reconciles.** These appear only when the archive moves under the process and it rebases (5.9.1). To tell which collections they touched, each vector collection records a **change stamp**: the committed sequence of the reconciliation that last changed it. The stamp is the newest sequence in the collection's change log (14.12.5).
  - A mirror records the stamp of the state it reflects.
  - At a rebase, the engine compares each mirrored collection's stamp with the archive's. A mirror whose collection changed catches up (18.12), and the others stay as they are.
  - A process that reconciled the changes itself applied them through the hook, so it records the new stamps without dropping anything.
  - A collection without a change log has no stamp: at every rebase its mirror is stale.
- **A stale or empty mirror.** A mirror that cannot catch up (18.12) is stale. A request that finds its collection's mirror stale, or not filled yet, takes the CPU path, so its results and latency are those of a build without an accelerator. The mirror is filled again from the vector blocks on the worker pool (5.9.6), for later requests. Results are the same either way (18.3).

### 18.7 The accelerator's calls

- **Placement.** The environment (`cdb_env`) carries an optional accelerator next to its operations: a table of calls, `cdb_accel_ops`, and the accelerator's own state, passed back on every call. The accelerator library keeps its state apart from the environment's, so it combines with any environment, for example the host environment with CUDA, or a test environment with a fake accelerator.
- **Calls.** Every buffer belongs to the caller, and every fallible call returns `cdb_status`:
  - `mirror_open` creates a mirror for one collection, given its store, element type, dimensions and measure, and how many vectors to expect.
  - `mirror_put` inserts or replaces a batch of vectors, given their entry IDs. `mirror_del` removes a batch of entry IDs.
  - `mirror_close` drops a mirror.
  - `nearest` is given a batch of query vectors and a number of candidates wanted. For each query it returns up to that many entry IDs, nearest first, with the accelerator's distances and a bound on their error. The engine uses these only to certify the shortlist (18.10), never stores or returns them: the CPU rescores (18.3).
- **Synchronous.** `nearest` returns when the shortlists are ready; the engine then rescores them. A refresh batch or a bulk search sends many queries in one call, which keeps the device busy.
- **Updates in batches.** During a commit, a rebase or a reconciliation, the engine lists the entry IDs of each mirrored collection that the run changes. When the run ends it sends one `mirror_put` batch, with the vectors read back from the blocks, and one `mirror_del` batch. If the list outgrows its memory, the mirror is marked stale instead and refilled (18.6).
- **Declining.** An accelerator may decline any call. A declined `mirror_open` leaves that collection on the CPU path. A failed `nearest` sends that request to the CPU path. A failed `mirror_put` or `mirror_del` marks the mirror stale. No accelerator failure ever reaches the caller of the engine.
- **Refilling and threads.** A stale mirror is refilled as a new mirror on the worker pool while the old one, if any, keeps serving requests; then the engine swaps them and closes the old one. So calls on different mirrors may overlap, and the accelerator must allow that. The engine never calls one mirror from two threads at once.

### 18.8 Device memory

The device may be shared with the application's own work, such as computing embeddings, so the engine's mirrors have a budget.

- **The budget.** The database configuration gets a field, `accel_mem`: the most device memory the engine's mirrors may use together. 0 leaves it to the accelerator's judgment. The engine counts a mirror as the bytes of its vectors. Within the budget, the accelerator may still decline `mirror_open` or `mirror_put` when the device is short (18.7).
- **A collection that does not fit.** All or nothing: a collection whose vectors exceed the budget, or whose mirror the accelerator declines, has no mirror and uses the CPU path. There are no partial mirrors.
- **Several collections.** A collection that needs a mirror evicts the least recently used mirrors until it fits, as the open-store slots close the least recently used store. A mirror that a request is using is never evicted.

### 18.9 Element types and measures

- **The contract.** An accelerator may support any element type and measure (7), and declines the others at `mirror_open` (18.7). Supporting a new one is a change to an accelerator, not to this section.
- **`combolite_accel`'s first version** supports all five element types, f32, f16, bf16, i8 and binary, on both CUDA and Vulkan:
  - **f32, f16, bf16 and i8** with cosine, dot product and Euclidean distance. All three are the matrix product of 18.5 with the vectors' norms.
  - **Binary** with Hamming distance, through a bit-count kernel of its own.
- **Device support.** A device that cannot compute a type, such as a Vulkan device without 16-bit floats, declines it, and those collections use the CPU path. The others are unaffected.
- **Precision.** The device may compute in its own precision, for example accumulating f16 products in f32. Rescoring on the CPU makes the stored and returned distances those of the CPU path (18.3); the error bound and certification (18.10) catch any difference in ranking.

### 18.10 Certified shortlists

- **The error bound.** With each shortlist, `nearest` returns a bound *e* on how far any distance it computed, for that query, can be from the CPU's distance for the same pair. The accelerator derives it from the element type's precision, the dimensions and the vectors' norms. A bound that is too small breaks the guarantee below, so an accelerator that cannot bound its error declines (18.7).
- **The check.** Let *g* be the accelerator's distance for the last candidate in the shortlist, and *d* the CPU's distance for the K-th nearest after rescoring. Every vector left out had an accelerator distance of at least *g*, so a CPU distance of at least *g* − *e*. If *g* − *e* > *d*, none of them could be among the K nearest: the result is the CPU path's. Ties at *d* go to the lower entry ID, as on the CPU path, so equality does not certify.
- **Widening.** If the check fails, the engine asks again for a shortlist twice as long, and repeats until the check passes or the shortlist holds the whole collection, when every vector has been rescored and the result is exact by construction.
- **The first size.** A shortlist starts at 2K candidates, at least K + 8. This is provisional: measuring on real data, such as GutenFree's passages, may give the lower-precision types (f16, bf16, i8, binary) a larger factor.

### 18.11 Testing

The accelerator path is tested in two ways that cover different things.

- **A fake accelerator, everywhere.** The test library (like `cdb_faultdev`, never the production library) has a CPU fake accelerator. It computes distances in a different order from the CPU path and adds noise up to the error bound it reports, so near-ties and widening (18.10) happen on purpose. It can be scripted to decline or fail any call, so every fallback in 18.7 is exercised. It runs on every machine and under the sanitizers.
  - ctest reruns the vector and graph suites with the fake installed, as the `_cap1` suites rerun with one open store. They must give exactly the CPU path's results.
  - The neighbour-graph crash sweep also runs with the fake installed: after a crash and reboot, mirrors start empty, are refilled, and results still match.
- **Real devices, where present.** `test_accel_cuda` and `test_accel_vulkan` are built only when their backend is enabled, and are registered with ctest. They compare the device's results with the CPU path's on the same data, and check that the device's distances stay within the bound it reports, which only a real device can show. On a machine without a usable device they report skipped, not passed.

### 18.12 Incremental catch-up

A mirror that falls behind fetches only what changed, not the whole collection, so the cost of catching up grows with the changes and not with the collection.

- **The change log.** Every vector collection created from now on keeps a change log (14.12.5): which entries each archive commit inserted, replaced or deleted. A call, `cdb_vec_track_changes`, adds one to a collection created before. The log is part of the format, not of the accelerator, so it is kept whether or not any accelerator exists.
- **Catching up.** At a rebase, before the process publishes the committed sequence its readers use (5.9.7), the engine reads each mirrored collection's log after the mirror's stamp. It sends the live entries' vectors in one `mirror_put` batch and the deleted entries in one `mirror_del` batch (18.7), and records the new stamp.
- **When it cannot.** A mirror whose stamp is older than the oldest sequence the log still covers, or whose collection has no log, is stale and is refilled whole (18.6). So is one whose catch-up the accelerator declines.
- **Named cursors.** An application or agent that wants the changes since it last looked, across sessions, registers a **named change cursor** on the collection. A cursor records the sequence its owner has read up to, starts at the collection's change stamp when it is opened, and keeps the log from being trimmed past it until it is advanced or dropped. Opening, advancing and dropping a cursor are each a small catalog change, committed at once (14.12.5). An abandoned cursor holds the log until it is dropped.
- **Trimming.** At reconciliation, the baton holder drops log rows at or below the lower of two sequences. One is the **readers' floor**: the lowest committed sequence any live coordinator publishes (5.9.7), the same floor that guards page reuse. Every live process catches its mirrors up before it publishes, so no row a live mirror still needs is dropped. The other is the lowest named cursor's sequence.
- **The changes call.** `cdb_vec_changes` lists the entries of a collection that changed after a given committed sequence, each with whether it is live now, in pages of the caller's size. It is public, so an application with a cursor reads its changes with it, and the accelerator's catch-up uses the same call. It reports when the log no longer reaches back that far.

## Open questions

- **Writers that predate the change log (14.12.5).** A build from before the log, writing to a collection that has one, would not keep it, and catch-up would then miss changes. The format is not released yet, so no such writer exists, but how a file says that its writers must keep the log (a feature flag, or the epoch) is to be decided.

- **First shortlist sizes by element type (18.10).** The factor for each type, measured by how often a first shortlist fails its check.
- **Mirrors and growth.** A collection keeps growing after its mirror is made. Whether a `mirror_put` that would pass the budget evicts other mirrors first, or marks this mirror stale so the next fill decides again (18.8).
- **Refilling without a worker pool.** On a host whose environment has no thread hooks, the worker pool runs tasks at once (5.9.6), so an asynchronous refill would block the request after all. Whether such a host refills at the next reconciliation or `cdb_db_tick` instead.
- **Vulkan shaders.** How the compute shaders are built and checked in, for example SPIR-V generated by a tool in `tools/`, as the CRC tables are.
- **Profiling first.** Measured, a refresh slice of GutenFree took 22 s, while the arithmetic is estimated at under a second. Most of the time is probably in coverage walks and index work, which stay on the CPU. Profile a refresh before implementation, to show what acceleration gains and what else needs fixing.
