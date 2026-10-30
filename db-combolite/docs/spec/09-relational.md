# Section 9: Relational

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Relational stores hold tables of typed rows. In governance, tables carry the structured part of the record: categories, statuses, owners, dates, and which records relate to which. This section defines how rows are typed, laid out, keyed, constrained, indexed and versioned. Wherever possible it reuses what earlier sections already define.

## Confirmed decisions

### 9.1 One type system across all data models

Table columns use the same types as documents (Section 8.8): the JSON types, plus timestamps, exact decimals, raw binary and entry references. A value means the same thing in every data model, so a query that spans several models never has to convert between type systems.

### 9.2 Row or column layout, chosen per table

Each table is laid out in one of two ways:

- **Row-oriented.** All the values of a row are stored together. This suits record-keeping work, where whole records are read and updated.
- **Column-oriented.** All the values of a column are stored together. This suits analysis, such as "count the records in each status", and it compresses well with the column compression in Section 2.5.

The default layout is a **configuration setting**. Some operators prefer the space saved by compression, and others prefer clean record-keeping. Any individual table can override the default.

### 9.3 Primary keys are active keys

- Every row has a universal entry ID (Section 6.1). The database uses it internally for all references.
- A declared primary key, for example `adr_number`, is an **active key** (Section 6.2). It is implemented as a unique index over the columns it names.
- A primary key may be **composite**, made up of more than one column.

### 9.4 Constraints; foreign keys are links

- The standard constraints are supported: **not-null**, **unique** and **check** rules.
- **A foreign key is a Section 6 link.** It is not a separate mechanism. This gives foreign keys the following properties:
  - each one has a relationship cardinality (Section 6.5);
  - enforcement can be configured by the operator (Section 6.9);
  - what happens on delete is tombstone by default, or cascade or refuse if configured (Section 6.7);
  - the referenced entry can be in any data model: a row, a document, a vector entry and so on.

### 9.5 Ordered indexes ship first

The core includes a single index type: a clean-room **ordered index**. It supports exact lookups, range scans and sorting. Other index types, such as a hash index, can be added later as new page types through the extension mechanism in Section 2.6. Shipping only one index type keeps the microcontroller build small.

### 9.6 Row history can be configured per table

Each table decides whether it keeps a revision history:

- **On:** rows keep the same git-like history as documents (Sections 8.6, 8.12 and 8.14). Revisions are attributed to the principal identity that made them, and history is periodically checkpointed.
- **Off:** suited to high-volume tables such as logs and counters.

The default comes from a **configuration setting**, in the same way as the layout default in 9.2. Out of the box that setting is **on**, in keeping with the governance-first stance. Any table can override it.

With history off, a table keeps its **last N revisions** of each row (9.11).

### 9.7 Tables are built on document pools

- A row-oriented table is a document pool with a **table schema**. Each row is an encoded object (14.6) whose field IDs are the table's column IDs.
- Revisions, patches, checkpoints, transactions and crash safety are those of documents (Section 8).
- **The API** works on rows as encoded objects:
  - insert, replace, patch and delete;
  - get a row by entry ID, or by primary key;
  - a range scan over any index, and a full scan.

  Column-oriented tables take the same calls (9.12).

### 9.8 Columns and schema changes

- **Where columns are defined.** A table's columns are defined in the catalog's schema history (Section 4.11), as versions of the table schema. Each column has a name (in the field dictionary), a type (9.1), a not-null flag and an optional **constant default**. A default is used when an insert leaves the column out.
- **Rows are never rewritten by a schema change.** Each row records the schema version it was written under, and an older row is read through the current version.
- **Supported changes:**
  - **Add a column.** It is nullable or has a default, and older rows read it as that default or as null. A not-null column added later must have a default.
  - **Drop a column.** It is hidden from reads at once, and its stored values disappear as rows are rewritten. A column used by an index or the primary key can be dropped only after that index is.
  - **Rename a column.** Only its name in the field dictionary changes.
  - **Change a column's type.** Only widening that never loses data is allowed; the permitted widenings are listed with the layout (14.10.1). Anything else is refused.

### 9.9 Constraints are checked when the change is made

- **Which constraints.** Not-null, unique and the primary key.
- **When.** They are checked when the insert or update is made, against the table and its indexes as the open transaction sees them. Applying a committed change never refuses one, so recovery cannot trip on a constraint.
- **Nulls in unique indexes never collide.** Any number of rows may hold null in a unique column; uniqueness applies to the non-null values. Primary-key columns are implicitly not-null.
- **Check rules (9.4) wait for the query layer,** whose expressions they need.

### 9.10 Secondary indexes

- An index covers one or more columns, and composite indexes are allowed.
- Creating an index on a table that already has rows builds it before the call returns.
- From then on, applying each change keeps the index current.

### 9.11 Tables without history keep their last N revisions

- With history off (9.6), each row keeps only its **last N revisions**. N can be configured per table and defaults to **1**, which means the current row only.
- When older revisions are pruned, the oldest revision kept becomes a full revision, so the patch chain still works.
- A delete removes the row. Links to it are still tombstoned (Section 6.7).

### 9.12 Column-oriented tables

- **Changes.** Committed changes go to a per-table **pending rows** index. Reconciliation folds them in, rewriting each affected row group once (14.10.2), and scans read the row groups together with the pending rows.
- **Row groups.** They hold **1024 rows** by default. This value is provisional, can be configured per table, and will be revisited with the host benchmarking tool.
- **Memory bound.** A group also closes early when any column's values would pass a per-table **byte cap** (64 KiB by default, provisional like the row count). So the memory one group needs is fixed by the row count and the cap, and works from caller memory alone. Groups therefore cover entry-ID ranges of varying width. Reconciliation splits a group that grows past the cap, so a write is never refused because a group is full. Underfull groups are not merged yet.
- **Row size.** A column-oriented table's rows are bounded by its cap, not by the document limit: a row larger than the cap is refused when it is written. This guarantees that a group of one row always fits.
- **Encodings.** The encoding of each column chunk is chosen **automatically**: the chunk is encoded every way that applies, and the smallest result is kept.
- **Compression.** After encoding, a chunk is compressed with **xz** (LZMA2, through the vendored liblzma) at a low preset, with a dictionary no larger than the chunk. The compressed form is kept only if it saves at least 1/8 of the size; codec 0 means uncompressed. Section 15 covers compression in detail.

## Open questions (do not specify until answered)

None at present.
