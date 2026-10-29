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

## Open questions (do not specify until answered)

None at present.
