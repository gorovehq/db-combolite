# Section 6: Keys and links

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

This section covers two things:

- **Keys:** how every entry in every store is identified. An entry can be a row, a document, a vector, a time-series point, a procedure and so on.
- **Links:** how entries connect to each other, including across data models.

Queries that span more than one data model are built on these two mechanisms.

## Confirmed decisions

### 6.1 Every entry has a universal ID

Every entry in every store is addressed in the same way: **(store ID, entry ID)**. Both parts are permanent numeric IDs that are never reused (Section 4.12). Any entry can therefore reference any other entry, whatever data model each one belongs to.

**Time-series points (review G3).** A time-series **series** is an entry and has its own entry ID. An individual point does not get an ID of its own. It is addressed as *(series entry ID, timestamp)*, and links can target a point by that pair.

### 6.2 Active and passive keys

- An **active key** is a key that the user or the application assigns explicitly, for example `"ADR-0042"` or `"user_42"`.
- A **passive key** is a key that the database derives itself, for example an auto-increment number, a hash of the content or a key derived from an embedding.

### 6.3 Content-hash keys are also the deduplication keys

A passive key derived from a hash of an entry's content is also that entry's deduplication key (Section 1.10). The same hash serves both purposes.

Encryption is kept as tight as possible:

- **In an encrypted enclave**, content hashes are **keyed** with a key derived from that enclave's key. Without the enclave key, nobody can check whether the enclave contains a known piece of content.
- **In public stores**, content hashes are unkeyed.

Hashes are only ever compared inside one encryption domain (Section 1.6).

**Granularity (review G2).** Deduplication works on **whole entries and whole attachments**. Splitting large blobs into content-defined chunks and deduplicating the chunks may be added later, as a new page type.

### 6.4 Cardinal → Vector → Ordinal mapping

- **Cardinal:** an entry's identity, meaning which entry it is (6.1).
- **Vector:** the entry's embedding.
- **Ordinal:** the entry's ranked position by similarity, relative to other entries.

The mapping is a **stored structure** that is kept up to date. Rankings are computed ahead of time, so a question such as "what are entry X's nearest neighbours?" is answered by a direct lookup, not by a search at query time.

### 6.5 Relationship cardinality

*Cardinality*, as in "matrix ordinality, cardinality", means **relationship cardinality**: one-to-one, one-to-many and many-to-many.

### 6.6 Neuron mapping

Neuron mapping is a **weighted graph of links between entries**, designed for agent memory.

- **Learned weights.** A link's weight grows stronger when the two entries it joins are used together, and fades when they go unused.
- **Crossing boundaries.** A link can join entries in different collections, data models and enclaves. Wherever a link touches an encrypted enclave, it refers to the enclave's data through **opaque pointers**, in the same way as joined stores (Section 4.9).

### 6.7 Deleting an entry that links point to

- **By default**, links that point to a deleted entry are kept and marked as **tombstones** (dangling). History stays intact, which governance records require.
- **The behaviour can be configured per link type.** A link type can instead specify cascade (the links are deleted along with the entry) or refuse (the entry can't be deleted while links to it exist).

### 6.8 How the stored rankings are kept up to date

- **K neighbours per entry.** Each entry stores rankings for K neighbours. K can be configured and has a sensible default.
- **Crossing collections is optional.** Rankings can span more than one collection that shares an embedding space. This is a configuration option, so each user can keep rankings inside a single collection when their sensitivity requirements call for it.
- **Refresh.** Rankings are refreshed both during reconciliation and in maintenance windows. Which one does the work at any given moment depends on the resources available.
  - A commit only queues the entries whose rankings changed. Each reconciliation refreshes up to a budget of them (default 256, set by the operator), and a maintenance call can refresh any number (14.12.4).
  - A lookup reports whether the entry's rankings are current or still queued.

### 6.9 Enforcing relationship cardinality is configurable

The operator decides how strictly each link type's declared cardinality (6.5) is applied: either it is **enforced** on every write, or it is only **recorded** in the catalog for queries and validation.

### 6.10 Learning and batching neuron weights

- **What teaches a weight.** Several signals count as entries being used together: being read in the same query, being used in the same session, and an explicit **reinforce** call from the application. The best mix of these signals, and the rules for how weights fade over time, will be settled by experiment during development.
- **Batched weight updates.** A change to a weight is first written to a delta track (Section 1.3). The changes are then applied in batches during reconciliation, not on every read.

### 6.11 Links that cross an encryption boundary live in a joined store

A link that connects entries in different encryption domains, for example a public entry and an entry inside an enclave, is stored in a **joined store** (Section 4.9). The joined store refers to the enclave side only through opaque pointers.

## Open questions (do not specify until answered)

1. **How neuron weights are learned (6.10).** The mix of signals, the rate at which weights fade, and the floor below which a link is pruned or turned into a tombstone. *To be decided by experiment during development.*
