# Section 8: Document

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Document pools store schemaless records, such as decision records, policies, notes and agent memories. This section defines:

- how documents are encoded on disk;
- how much structure is enforced on them;
- how large content is handled;
- how documents are searched;
- how changes are kept over time;
- how documents connect to the vector store, given that chunks are separate linked entries (Section 7.5).

## Confirmed decisions

### 8.1 A purpose-built binary encoding

Documents are stored in a binary encoding designed for db-combolite. Every field is typed and length-prefixed, so a reader can jump straight to the field it needs without parsing the whole document. JSON is used for import and export.

### 8.2 Every document has a summary field

Every document has space set aside for a short **summary**. Searches that use the summary for context can then run alongside vector embedding and retrieval.

### 8.3 Optional schemas per pool

A pool can have a schema in the catalog. The schema is versioned (Section 4.3), and documents written to the pool are validated against it. A pool without a schema accepts any document.

- **What is checked.** Every new revision is checked against the pool's **current** schema and records that version (14.9.1). Existing documents are not re-checked when the schema changes, but a change to an older document must produce a document that is valid under the current schema.
- **When.** The check happens when the change is made, inside the transaction. An invalid change is refused and nothing is journaled; applying a committed change never fails for schema reasons.
- **Within one transaction.** A change is checked against the document as the transaction sees it, including the transaction's own earlier changes. The schema format is in 14.9.4.

### 8.4 Attachments and size limits are configured by the operator

- **Attachments** (PDFs, images and so on) are stored in one of two ways, chosen by the operator:
  - **inside the database**, as binary objects, deduplicated (Section 6.3) and encrypted along with their enclave; or
  - **as external files** that the database references.
- **Maximum document size** is also set by the operator, so it can match the operator's preferences, the host's storage capacity and the memory available.
- A document too large for its page spills into overflow pages (Section 2.6).

### 8.5 Indexing is configurable

Field indexing has the following modes, chosen through configuration and possibly controlled by a feature flag:

- **Declared:** only the fields the operator names are indexed.
- **Full:** every field is indexed.
- **Incremental since the last maintenance window:** fields written since the last maintenance window are indexed.
- **Incremental since the last reconciliation:** fields written since the last reconciliation are indexed.

### 8.6 Revisions follow a governance model, similar to git

- The **original** document is kept intact.
- Each change is recorded as a **delta**. Deltas are tracked and kept as a shadow chain of the document's history.
- The **current version** is the most recent delta for each section that has changed, combined with references to the sections of the original that haven't changed.
- The document can be **rolled back** through its history to any earlier head.

### 8.7 The application chunks; the library calls the application's embedding hook

- **Chunking.** When the client starts up, the database tells the application the **chunk limits**. The application splits documents into chunks within those limits.
- **Embeddings.** The application registers its embedding function with the library as a **hook**. The library calls the hook whenever a document or a chunk needs an embedding. The database contains no model code, because there are too many models to support. It provides the framework guidance, which matters most for the less capable models that can run on microcontrollers.

### 8.8 Field types

The encoding supports the JSON types plus the following:

- **timestamps**;
- **exact decimals**;
- **raw binary**;
- **entry references**: universal IDs (Section 6.1), so a document can point directly to any other entry.

### 8.9 Summaries come from a summarizer hook

The application registers a **summarizer hook** in the same way as the embedding hook (8.7). The model behind the hook produces the summary, and the framework defines the summary's form and limits, which the hook must follow.

### 8.10 Full-text search is built in

Keyword search with relevance ranking is part of db-combolite. Because a built-in tool is known to exist, other tools can rely on it; for example, MCP tools can be built on top of it. An application can still run its own search, but that is usually slower.

A pool has at most one full-text index, over the fields the operator names and optionally the summary (the *declared* mode of 8.5). It is kept up to date at every commit and ranks results with BM25. The layout is in 14.14.1.

Grouping a document's words uses the C library's `qsort`, which may allocate memory internally on some hosts. This is accepted as an exception to the rule that the core does not allocate: the engine's own code still takes its memory only from the caller, either as buffers or through the environment's optional allocator.

### 8.11 External attachments are verified and encrypted

- The catalog records the **content hash** of every external attachment, so any tampering is detected.
- An external attachment that belongs to an enclave is **encrypted under that enclave's key**.

### 8.12 Revision chains are checkpointed and attributed

- **Checkpoints.** Periodically, the chain of deltas is merged into a **checkpoint version**: a full copy of the current state. The history behind it is kept, and rollback through that history still works (8.6). The interval is configurable; the default is every 16 revisions (14.9.2).
- **Attribution.** Every revision records the **principal identity** (Sections 5.1 and 12.5) that made it, which gives the document a full audit trail.

### 8.13 When the embedding hook runs is configurable

The operator chooses one of two modes:

- **during the write:** simple, but a write takes as long as the model does;
- **deferred:** the hook runs at reconciliation or in the background. A document can be found by vector search only once its embedding exists.

### 8.14 Linear history with parent IDs, ready for branching

- Revision history is **linear**.
- Every revision records its **parent revision ID**. Branching and merging can therefore be added later without changing the format.
- **Conflicting edits** by different identities are handled by the conflict policy in Section 5.
- **Drafts and proposed amendments** are separate documents linked to the original (Section 6). When one is accepted, it is applied to the original as a new revision.

## Open questions (do not specify until answered)

None at present.
