# Section 4: The catalog

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

The catalog is the database's description of itself. It records the following for every store:

- its data model (relational, document, vector or time series);
- its schema;
- its encryption domain;
- its page size;
- its compression mode;
- its indexes;
- the companion types and key references it uses;
- where its deduplication index and quick-query index are located. These used to be listed in the file header (review G1).

The catalog also records stored procedures and the data each procedure depends on. Without the catalog, no other data in the database can be interpreted.

## Confirmed decisions

### 4.1 The catalog is a store inside the database

The catalog is not a structure with its own special format. It is an ordinary store in the database, and it describes itself. It therefore gets the same treatment as every other store:

- the same pages (Section 2);
- the same crash safety and commit sequence (Section 3);
- a redundant second copy (Section 3.5);
- the same scrubbing (Section 3.5).

### 4.2 The quick-query index is derived from the catalog

The catalog is the **single source of truth**. The quick-query index (Section 1.10) is a **derived** jump-to-address lookup, built from the catalog. It indexes where each data type lives, and it indexes stored procedures together with the data they operate on.

If the quick-query index is damaged or out of date, it is rebuilt from the catalog. Because it can always be rebuilt, it needs no redundant copy of its own, and it can't drift out of step with the catalog.

### 4.3 Schemas are versioned

When a schema changes, the catalog keeps a **version history** of the schema.

- Every record is read using the schema version it was written under.
- A schema change is therefore cheap, because existing records aren't rewritten when the change is made.
- Old records are migrated to the current version lazily, or during maintenance windows.

### 4.4 Catalog entries for an enclave stay inside the enclave

Catalog entries that describe an encrypted enclave are kept **inside that enclave, encrypted under its key**. These entries include the enclave's store names, schemas, indexes and procedures.

For each enclave, the public catalog records only:

- an opaque ID;
- the enclave's encryption key reference.

Someone who doesn't hold an enclave's key can't learn the enclave's names or schemas, or what it contains.

### 4.5 Stored procedures keep source text and compiled form

For every stored procedure, the catalog stores:

- the **source text**;
- the **compiled form**;
- the procedure's **dependencies**: the stores and fields it reads and writes. The quick-query index (4.2) is built partly from these dependencies.

The procedure language is defined in the query-layer section.

### 4.6 Stable numeric IDs

Every store, every enclave and every stored procedure has a **permanent numeric ID** in addition to its name. References inside the database always use the ID, never the name, so renaming an object never breaks a reference to it.

### 4.7 The catalog is always at location 0

The catalog has the reserved ID **0** and is always found at **location 0**. A reader never has to search for it or infer where it is from the file's structure. This keeps post-mortem analysis and data recovery straightforward even when other structures are damaged.

**Location 0 is a fixed physical position**: the first available page after the header copies.

**Header epochs.** The header format is versioned in **epochs**, and each epoch has a fixed header length. Location 0 for a file is therefore always known from the file's epoch. If the headers can't be read, a recovery tool can try the fixed header length of each known epoch in turn, and look for the catalog at each candidate location 0.

**Catalog integrity markers.** The catalog at location 0 starts with a **magic number** and ends with a magic number, and it carries a **checksum**. These let the catalog be found and verified even when the headers are damaged or unreadable.

**Updating the root without moving it.** The rest of the file is append-only, but the catalog root at location 0 changes over time and must stay where it is. It is updated like this:

- Location 0 holds a **pair of root slots, A and B**. Updates alternate between the two slots, in the same way as the header copies (Section 1.9).
- Each slot carries the magic numbers, a checksum and a sequence number. When the file is opened, the valid slot with the higher sequence number is used.
- The root points to the body of the catalog, which grows by appending like every other store.
- The A/B pair provides redundancy for the **root**. Every page of the catalog **body** is written twice, as mirrored appends (Section 3.5, review R5).
- On raw flash, a ring of N slots replaces the A/B pair (Section 1.11.3).
- When the database is opened, the newest root slot whose sequence is not higher than the committed header sequence is used (Section 3.2).

### 4.8 Each enclave has its own quick-query index

Each encrypted enclave keeps its own derived quick-query index (4.2), stored inside the enclave and encrypted under the enclave's key. The public quick-query index covers public stores only.

### 4.9 Where stored procedures live

- **A procedure that touches only one enclave** is stored in that enclave.
- **A compound procedure that touches more than one enclave** is stored in a **joined store**. This covers procedures that mix encrypted and unencrypted stores, and procedures that span only encrypted enclaves. Both cases use a joined store, for simplicity and consistency.
- **The joined store is encrypted under a key of its own.** Inside it, every reference to an encrypted enclave is an **opaque pointer** to the encrypted fields. It reveals neither the names nor the structures of the enclaves it points to.

### 4.10 Compiled procedures record their compiler version

The compiled form of a procedure records the compiler version that produced it. If a newer library can't run that version, it recompiles the procedure from the stored source text.

### 4.11 Schema history is kept as a matter of record

Earlier schema versions are kept by default, because in governance work the structures that came before are part of the record. Operators can **export** the history whenever they like. History is discarded only when **both** of these hold:

- the configuration allows discarding it, and
- the operator explicitly asks for it to be discarded.

### 4.12 ID rules

- An ID is **never reused**, even after the object it belonged to has been deleted.
- An ID has the same width as the file's address width, 32-bit or 64-bit (Section 2.4). The exception is files that use the compact header: there, IDs are 16-bit (Section 13.4).

## Open questions (do not specify until answered)

None at present.
