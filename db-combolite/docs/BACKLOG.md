# Backlog

Features and follow-ups that were agreed and deferred. Each one becomes a milestone or a branch when it is picked up, and its design decisions are brought to the user then, as usual.

## Features

### Streaming API for large content

**What:** write and read content a piece at a time, for example "open, append chunk, …, close", so a whole book or file is never held in memory at once.

**Why deferred:** M14 sized memory with `doc_max`, the largest document allowed. The caller provides buffers that size, because the core never allocates. That's enough for documents up to `doc_max`. The Gutenberg ingest can store books as chapter and paragraph documents (spec 7.5). Streaming was the more costly option, at about 2–3× the M14 work.

**What it touches:**
- **Journal:** one change split across several records, replayed as a unit.
- **Documents:** patches, diffs and checkpoints that work without the whole value in memory.
- **Full-text search:** indexing piece by piece instead of comparing two whole versions.
- **Where it may live:** possibly as blob attachments (spec 8.4, 14.15), page extents with a content hash. That needs SHA-256 through mbedTLS, behind the engine's own crypto interface.

## Follow-ups

- **Full-text token memory.** The rule that a word is never cut needs 2 × (`doc_max` + page) tokens, about 32 MB at `doc_max` = 1 MB. A word with its separator takes at least 2 bytes, so the bound could be halved. Check the word splitter first.
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
- **Links: cost of checks in large transactions.** Checks read the open transaction's own LINK and UNLINK records back from the journal. That is linear in the transaction's size, so a bulk import should commit often. A staging cache, like the one documents use, would fix it.
- **Time-series limits chosen in M16, to confirm.** At most 8 tags and 16 value fields per series, 1,024 points per block, and one append batch per journal record (at most a page). The series key is limited by the keys index: about 119 bytes on 1 KB pages and about 500 on 4 KB pages. For Gutenberg metrics, use eBook numbers as tags rather than titles.
- **Shared test toolkit (pinned until more modules are built).** The test suites repeat the same setup: static buffers sized by hand, the database config, the fault environment, open, close and reopen helpers, and the cap and need_open handling. Changing the document buffer size meant editing eight files. Gather it into a common test library that suites call, once enough module code exists to see the full set of patterns.
- **Table limits chosen in M17, to confirm.** 64 columns, 8 indexes, 8 columns per index, and 1 KB of defaults per table. Decimal, array, object and vector columns cannot be indexed yet, because the key encoding has no form for them.
- **Table creation is several catalog commits.** A crash part-way leaves a table without columns: row calls refuse it with CDB_E_STATE, and it can be dropped. Creating a unique index that meets a duplicate also leaves the new index's pages unreferenced, which the reclaim pass would free.
- **Unique checks read the transaction's own rows.** This is linear in the number of rows the transaction changed in that table (the same cost as the link checks). A staging cache keyed by index would fix both.
- **Nibblet rewrites on time-series appends.** Appending a point to a small block frees its nibblet and places a new one, which updates the free-space index each time; `test_ts` went from 4.4 s to 12.4 s. The fix is to rewrite the record in place when it still fits its page.
- **Overflow-chain tails as nibblets (spec 16.5, kind 4).** The last, partly filled page of an overflow chain would be stored as a nibblet, saving about half a page per large value.
- **Reclaiming space of dropped stores and indexes.** Dropping a store or an index leaves its pages and nibblets in place. A reclaim pass would free them.
- **Time series still to come.** The audit series (10.7), which would replace the collision callback; retention and downsampling recipes (10.4); links to individual points (6.1).
- **Links to other models.** Link ends can be documents and vectors so far. Rows, time-series points (a different reference shape) and enclave pointers (spec 6.11) come with those milestones.
