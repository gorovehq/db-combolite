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

## Open questions (do not specify until answered)

None at present.
