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
- **Open-store limit, to decide before the relational milestone.** `cdb_db` holds at most 8 open document pools and 8 vector collections (`CDB_DB_MAX_POOLS`); a 9th gives "buffer too small". The link store has a fixed place of its own. Relational tables need more indexes each, and the Gutenberg layout uses many stores. Options: raise the fixed limit, or have callers supply the slots.
- **Links: cost of checks in large transactions.** Checks read the open transaction's own LINK and UNLINK records back from the journal. That is linear in the transaction's size, so a bulk import should commit often. A staging cache, like the one documents use, would fix it.
- **Links to other models.** Link ends can be documents and vectors so far. Rows, time-series points (a different reference shape) and enclave pointers (spec 6.11) come with those milestones.
