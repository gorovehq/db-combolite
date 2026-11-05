# Section 17: Build profiles

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

db-combolite has to run in two very different places. On a host (a desktop, a server, a laptop running an agent), it should use the memory and cores it has and be as fast as it can. On a micro-controller (the long-term target in the project's constraints), it has to fit in a few hundred kilobytes of caller memory.

Until this section, every runtime limit was sized for the micro-controller case. A query could read at most 16 KB of a row, Sort could hold at most 256 rows, and a row could have at most 16 output fields. Those limits made ordinary host work fail, such as reading a whole book or ordering 80,000 catalog rows.

A **build profile** sizes these limits for where the library runs, without splitting the file format.

## Confirmed decisions

### 17.1 Two profiles, one file format

- **Profiles.** There are two profiles: **host** and **mcu**. A build selects one at compile time. The host profile is the default.
- **One format.** Both profiles read and write the same files. A file written under either profile opens under the other, as long as it stays within the format limits (17.2), which are the same in both.
- **One codebase.** There is one engine. The profile chooses its limits (17.3) and, where performance depends on it, its code paths (17.5). There is no separate micro-controller engine.

### 17.2 Format limits are the same in both profiles

A **format limit** decides what a file may contain, so every reader has to accept it. Format limits are fixed by the sections that define them, never by the profile. They include:

- names: at most 64 bytes (Section 4)
- tables: at most 64 columns, 8 indexes, 8 columns per index or primary key, and 1 KB of default values (14.10)
- full-text settings: at most 8 paths, 8 deep (14.14)
- the coordinator slot, including its 21 entry-ID blocks (14.5)
- value nesting (32 levels, 14.6.3) and index tree depth (16, 14.8.1)

Raising a format limit is a format change and is done in the section that defines it.

### 17.3 Profile limits

A **profile limit** bounds only what one running process does: its working memory, its queries, its threads. A file does not record it, so the two profiles may differ.

| Limit | mcu | host |
|---|---|---|
| A query's record buffer (one row or document read by a scan) | 4 pages | the largest document body (`doc_max`), grown as needed |
| Rows a pipeline breaker (Sort, Aggregate groups, HybridRank) holds | 256 | as many as the query's memory budget allows |
| Output fields per row | 16 | 256 |
| Procedure registers and cursors | 64 and 4 | 1,024 and 64 |
| Triggers and scheduled tasks | 16 and 16 | 1,024 and 1,024 |
| Worker threads | at most 4 | up to the machine's core count |
| Full-text query terms; time-series fields per series | 16; 16 | 256; 256 |
| Neighbour-graph maintenance: a refresh's batch of vectors, the entries it checks, and the entry points it chooses (6.8, 14.12.4) | the caller's `vec_work` | `vec_work`, grown as needed up to the maintenance budget (17.4) |

**When a limit is reached**, both profiles report it the same way, with `CDB_E_OUT_OF_BUDGET`. A limit is never silently cut short.

**Neighbour-graph maintenance is the one exception.** When a refresh has no more memory to grow into, it does not fail. It uses the fallbacks 14.12.4 already defines: smaller batches, fewer entry points (down to the minimum), and, when the entries to check don't fit, checking every entry with a list and every entry point. The graph stays correct; only the refresh gets slower. Reconciliation runs refreshes, and it never fails because a graph is large.

### 17.4 Memory

- **mcu.** All working memory comes from the caller, as before: buffers passed in, and the environment's optional `alloc`/`free` hooks only where the engine already uses them (the open-store slots).
- **host.** Working memory may also grow through the environment's `alloc`/`free` hooks, which the host environment backs with `malloc`. The core still never calls `malloc` itself. A query's growth is bounded by a **query memory budget** in the database configuration. Reaching the budget is `CDB_E_OUT_OF_BUDGET` (17.3).
- **host, neighbour-graph maintenance.** A refresh starts from the caller's `vec_work` and grows the same way, bounded by a separate **maintenance memory budget** (`vec_mem`) in the database configuration. It is separate from the query budget because refreshes run at reconciliation, not inside a query. With enough memory, a refresh handles its pending entries in one pass over the vectors, checks only the entries it touched, and keeps about √N entry points. Reaching this budget is not an error (17.3). The memory is sized for each refresh's own pending entries and freed when the refresh ends, so an idle database holds none of it.
- **mcu, sizing `vec_work`.** The mcu profile never grows, so `cdb_vec.h` provides a sizing function: the `vec_work` a refresh of a given number of entries, in a collection of a given size, needs in order to avoid checking every entry (14.12.4). It is the same estimate the host uses to grow, so a caller can size a static pool from it.

### 17.5 Code paths may differ; results may not

- **Code paths.** Where performance depends on it, the host profile may use a different algorithm. For example, Sort followed by Limit can keep only the top N rows, and buffers can grow instead of being fixed.
- **Results.** The same query on the same file gives the same rows in the same order under both profiles, unless one of them reaches a profile limit (17.3).
- **The neighbour graph's shape.** Which entry points a refresh chooses depends on the memory the refreshing process has (17.3), so the same data refreshed under the two profiles can be stored with different entry points. Searches read the stored graph, so a search on a given file gives the same results under either profile. Approximate search (7.4) on the same data can differ slightly, depending on which process refreshed the graph. Exact search and every other result are unaffected.

### 17.6 Build settings

- **host.** Optimised for speed: `-O3` and link-time optimisation. Builds for the local machine only may also use `-march=native`. Distributed builds never do.
- **mcu.** Optimised for size (`-Os`), with optional dependencies off, as in today's `noxz` build.

### 17.7 Testing

The whole test suite, including crash sweeps, runs under both profiles. Tests that depend on a limit check it under each profile's value.

## Open questions

These need measuring, not deciding. The values stand, marked provisional, until the host benchmarking tool (BACKLOG) settles them in one pass, with the open-store cap, the vector search-path threshold and the device-class table:

- **The host query memory budget's default.** The proposal is 256 MB.
- **The maintenance memory budget's default (`vec_mem`).** The proposal is 64 MB. GutenFree Diet's 16,033 passages (384 dimensions, K 16) refreshed in one pass in 64 MB, and a full build of their graph took 926 s in slices of 256 with no refresh checking every entry.
- **The exact host values in 17.3.** The values in the table are the starting point.
