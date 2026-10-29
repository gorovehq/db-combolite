# Section 7: Vector

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

This section covers how embeddings are stored and how similarity search over them works: element types, dimensions, similarity measures, indexing, and how the vector store shrinks for microcontrollers. The stored rankings (Section 6.4) and neuron mapping (Section 6.6) are built on what this section defines.

## Confirmed decisions

### 7.1 Element types are chosen per collection

Every collection chooses one element type:

| Type | Bytes per element | Notes |
|---|---|---|
| f32 | 4 | full precision |
| f16 / bf16 | 2 | half the size of f32, with a small loss of precision |
| int8 | 1 | quantized; a good fit for microcontrollers |
| binary | 1/8 (1 bit) | coarse; useful as a fast first-pass filter |

Each device class in the device-type table (Section 2.8) has a default element type.

### 7.2 Dimensions are fixed per collection

Every vector in a collection has the same number of dimensions, recorded in the catalog. Changing the dimension creates a new schema version (Section 4.3), which in practice means re-embedding the data.

### 7.3 Embedding provenance is recorded

For each collection, the catalog records the **embedding model and its version**. Two collections share an embedding space only when both match. Cross-collection rankings (Section 6.8) can only be built between collections that share an embedding space, so embeddings that aren't comparable are never mixed silently.

### 7.4 Exact search is always available; the approximate index reuses the neighbour graph

- **Exact search** compares the query against every vector. It is always available, and it is the only search method on small microcontrollers.
- **Approximate search** is optional and enabled by a feature flag. It **reuses the stored neighbour graph** from Section 6.4/6.8, where each entry already links to its K nearest neighbours.

Because of that reuse, one structure serves both the stored rankings and approximate search. It is maintained once and implemented once. The design is clean-room (constraint 5).

### 7.5 One vector per entry

Each entry has exactly one vector. Chunks and variants of a source, such as a title embedding, a body embedding or the chunks of a long document, are **separate entries linked to their parent** (Section 6).

A vector can record its **source**, the entry it was computed from. Search reports whether each hit's source document is unchanged, has been edited since the vector was written, or is gone, so stale embeddings are never returned silently (14.12.3).

Because each chunk is an entry of its own, it gets its own neighbour list. At runtime, a chunk of document A can therefore become a neighbour of a chunk of document B. This reveals connections that weren't known before, without going through either parent.

### 7.6 Encrypted enclaves must be unlocked to be searched

Similarity search needs vector values in the clear. To search an enclave, the enclave is unlocked first. Its vectors are then decrypted into memory or into the encrypted decrypt cache (Section 1.5), and searched there. The enclave's index is encrypted along with the enclave.

There is no support for searching encrypted vectors without decrypting them.

### 7.7 Similarity in three layers

1. **Base measure.** Each collection has one default measure (cosine, dot product, Euclidean or Hamming). The neighbour graph and the stored rankings are built for that measure.
2. **Learned similarity.** The neuron-mapping weights (Section 6.6) are stored, and they adjust rankings based on how the data has actually been used.
3. **A measure chosen per query.** A query may ask for a measure other than the base one. It can be answered in one of two ways:
   - **Approximate path:** the neighbour graph supplies candidates, and those candidates are re-ranked exactly using the requested measure.
   - **Exact path:** a full exact search using the requested measure.

**Choosing the path.** By default an automatic rule picks between the two paths. For example, the exact path is used when the collection is small or when the requested measure diverges from the base measure; otherwise the approximate path is used. The operator can override the rule in configuration, and a query can override it for itself. When both overrides are set, the query's choice wins over the operator's.

## Open questions (do not specify until answered)

1. **The automatic path rule (7.7).** The size threshold, and how to tell when a requested measure diverges from the base measure. *These will be set by benchmarking during development.*
