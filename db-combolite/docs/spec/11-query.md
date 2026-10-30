# Section 11: Query layer and stored procedures

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

This section covers how one query can reach across relational, document, vector and time-series data. It also covers what stored procedures and recipes are written in, how transactions span several stores, and how results stay within memory limits.

## Confirmed decisions

### 11.1 A structured core with our own text language on top

- **Core: a structured query format.** Every query has a canonical structured representation. Agents and MCP tools build queries in this form directly.
- **Text language: new, not SQL.** db-combolite defines its own human-readable query language. It is designed to handle links, similarity, documents and time series as first-class features, not as additions bolted onto SQL. Text queries compile into the structured core.
- **The text language is an optional module.** A build can include it or leave it out.

### 11.2 Stored procedures

- Procedures are written in the query language, extended with control flow: variables, conditionals and loops.
- They compile to a **small bytecode of db-combolite's own design**. This bytecode is the "compiled form" stored in the catalog (Section 4.5).
- They are **sandboxed**:
  - no file access and no network access;
  - limits on instructions, time and memory, so that a faulty procedure can't stall the database.

### 11.3 Transactions across stores; conflicts

- **Atomic across stores.** A single transaction can write to several stores at once, all or nothing. For example, it can add a document, a row and a link between them. Every delta carries the transaction ID. The transaction becomes durable when its commit record is written, and replay applies only transactions that have a commit record (Sections 1.3 and 3.3). The transaction's deltas are folded into the archive together at reconciliation (Section 3.2).
- **Read your own writes.** A writer sees its own changes before they are reconciled. Every other process sees the last reconciled state (Section 5.1).
- **Write conflicts.** The policy is **configurable**. By default, if two identities edit the same entry before reconciliation, the later write is rejected and must be retried.
- **Collision notice.** A writer whose write is rejected receives a **collision notification**, much like failing to acquire a semaphore lock, so it can wait, retry or give up.

### 11.4 Combining rankings in hybrid queries

A hybrid query can combine several sources of ranking: keyword relevance (Section 8.10), vector similarity (Section 7.7), neuron weights (Section 6.6) and filters. The rankings are merged by **reciprocal rank fusion** by default. The weight given to each source can be adjusted per query, or set by the operator.

### 11.5 Streaming results and budgets

- Results are always returned **incrementally through a cursor**. A result set is never built up in full before it is returned.
- Every query runs under a **memory budget and a time budget**.

### 11.6 Triggers and schedules

A procedure can run in two ways:

- **On events (triggers)**, for example when a row is written to a particular store;
- **On a schedule, or in maintenance windows.** Recipes (Section 10.4) and maintenance work run this way.

### 11.7 Running on microcontrollers

- **Binary wire form.** The structured core has a compact **binary** encoding, so a device never needs RAM to parse JSON. JSON stays available on larger hosts.
- **Compile off the device.** Microcontroller builds leave out the text parser. Queries and procedures are compiled on a host machine and loaded onto the device already compiled.
- **Prepared queries.** Queries that are used often are stored in the catalog in compiled form and called by ID.
- **Device-class presets.** The budgets in 11.5 have default values for each device class in the table in Section 2.8.

### 11.8 What a collision notice reveals

- **Same encryption domain.** The notice names the identity that holds the conflicting write.
- **Different tenant or enclave.** The notice is opaque and names no one, so no information leaks across the boundary.

### 11.9 Trigger safety

- **Depth limit.** Each trigger has a limit on how deeply the triggers it sets off can nest. The operator configures this limit **per trigger**.
- **Triggers run after commit.** A trigger runs after the transaction that set it off has committed. Triggered work is scheduled according to the resources available.
- **Cascade warning.** If an action would set off more triggered changes than an operator-defined threshold *x*, the library warns before running it. The warning says that operational availability may be reduced until all the triggers have caught up.

### 11.10 The structured query core

- **Shape: an operator tree.** A query is a tree of typed nodes (a logical plan), not a flat pipeline or a clause object. Each node takes other nodes as input, so a query composes the way an expression does: `Sort(Limit(Filter(Scan(...), cond), n), key)`. Cross-model joins are an ordinary `Join` node whose two inputs are scans of different store kinds.
- **Read-only.** The tree only reads and ranks. Writes (insert, update, delete, link) are not tree nodes; a stored procedure's bytecode (11.2) calls the engine's existing per-model mutation functions directly, the same functions the library itself already exposes. The tree never has to define its own write ordering, conflict handling or transaction boundaries, because it never writes.
- **Scans are named by catalog name.** `Scan("orders")` names a store the way the user created it; the planner resolves the name to a store ID when the query is prepared. A compiled/prepared query (11.7) still names its store this way, so it survives the store being dropped and recreated.
- **Expressions are a tree too.** A condition, join key or sort key is itself a small expression tree: field references, literals (the shared value encoding, Section 14.6), parameters, and operators (comparison, boolean, arithmetic, and model-specific functions such as text and vector search predicates). One expression grammar is reused in every node that needs one.
- **One `Field(name)` form across every model.** `Field("price")` resolves to whatever "price" means on that scan's model: a table column, a document field (absent is null, as documents already treat a missing field), a time-series value field, or a vector collection's metadata field. A vector itself is a reserved pseudo-field, e.g. `Field("$vector")`.
- **Nested field paths use a reserved separator byte, not a literal dot.** In the structured core, a path to a nested document field is its segments joined by the ASCII Unit Separator (0x1F), not by `.`. A field name may freely contain a literal dot; nothing needs escaping, because the delimiter is never the dot character. The field dictionary (14.6) instead disallows 0x1F in a field name, which costs far less than disallowing the common case. The human-readable text language (Section 11.1) still writes a nested path as `address.city`; its compiler is responsible for turning that into 0x1F-joined segments (and for however it lets a user write a field name containing a literal dot — an open question for the text-language design, not the structured core).
- **Parameters.** `Param(name)` or `Param(index)` appears anywhere a literal could. The caller supplies bindings when the query runs. This is what lets one compiled, prepared query (11.7) run again with different values without recompiling.
- **Joins: inner and left-outer.** A `Join` node supports those two kinds. Right-outer is a left-outer with its inputs swapped; full-outer is left for later if it turns out to be needed, since it is just another join-kind tag and does not require a format change. The unmatched side of a left-outer join yields **null for every one of its fields**, the same null a document's missing field already produces (Section 8.1) — no second kind of absence.
- **No correlated sub-trees.** Every node's inputs are fully independent: a sub-tree (a `HybridRank` branch, a `Join`'s right input) never references a field from an enclosing node. What a `Join` or `Traverse` node already covers is enough for this database's scope (Hard constraint 4); a correlated-subquery execution and scoping model is not needed.
- **Cursor resume is an opaque token.** A cursor (11.5) hands back an opaque byte blob after each batch; the caller passes it back unexamined to continue. Its internal structure is a storage-engine detail that can change across versions without a compatibility concern, since nothing outside the engine parses it.
- **Link traversal: a `Traverse` node.** `Traverse(input, linkType, direction, hops)` walks from each entry in its input across a named link type (Section 6), forward or reverse, for one hop or a bounded range of hops. A bounded hop range covers both an ordinary one-hop join across a link and a multi-hop neuron-graph walk (Section 6.8) with the same node. Traverse's output is typically fed into a `Join` or `Scan` to pull in the linked entries' own data.
- **Hybrid ranking: a `HybridRank` node.** `HybridRank(branches, weights, fuse)` takes a list of ranked sub-trees (for example a `TextSearch` branch and a `VectorSearch` branch over the same entries) and fuses them, by reciprocal rank fusion by default (11.4). Each branch is an ordinary sub-tree, so any ranked source can be mixed in, not just a fixed pair.
- **Aggregation: a dedicated node.** `Aggregate(input, groupBy, outputs)` takes explicit grouping-key expressions and explicit aggregate output expressions (`Sum`, `Count`, and so on), rather than inferring grouping from what is left ungrouped in a projection. This keeps the tree's grouping semantics unambiguous, which matters for a clean-room format with no inherited behaviour to fall back on.
- **Wire tag scheme.** The binary form (11.7) tags every node, operator and expression kind with a single leading byte, the same style as the value encoding's type tag (14.6): one byte per kind, with room held in reserve for kinds added later without a format version bump.
- **`Project`: an explicit output list, with an optional wildcard.** `Project(input, outputs: [{as, expr}], passthrough?)` names every output field and the expression that produces it; `passthrough` additionally carries through every field already present on the input, unchanged, alongside the listed ones.
- **`Limit` carries its own offset.** `Limit(input, count, offset)` skips `offset` rows (default 0) and then yields up to `count`, covering ordinary pagination with one node.
- **`Union` never deduplicates.** `Union(branches)` concatenates every branch's stream as-is (SQL's `UNION ALL`, not `UNION`). This matches the streaming-cursor model (11.5): removing duplicates would mean buffering and comparing rows across the whole result, which a query is never required to build up in full. A caller that wants distinct rows composes that explicitly.
- **`Sort` null placement is configurable per key.** `Sort(input, keys: [{expr, dir, nulls}])`, where `nulls` is `first` or `last` per key.
- **`TextSearch` and `VectorSearch` both yield a ranked, scored stream.** `TextSearch(input, fields, query)` searches the named fields of the input's entries with the existing BM25 engine (8.10) and yields matches each carrying a relevance score. `VectorSearch(input, field, near, k, metric?)` searches a named vector field (usually `$vector`) for the `k` nearest neighbours of `near` (a literal or a `Param`), optionally overriding the collection's configured distance metric (7.7) for this one query, and yields matches each carrying a similarity score. Both shapes match, which is what lets `HybridRank` treat any ranked branch — text, vector, or otherwise — the same way.

## Open questions (do not specify until answered)

- How the text language lets a user write a field name containing a literal dot, and how it compiles a dotted path into 0x1F-joined segments.
- The exact binary layout of each node kind (field order, lengths), to be written into Section 14 once implementation (M20) begins.
