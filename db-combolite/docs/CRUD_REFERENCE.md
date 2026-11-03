# Combolite CRUD Reference: C99 API & CMBQL

This document provides a comprehensive reference of all Create, Read, Update, and Delete (CRUD) operations available in Combolite via the **C99 Engine API** and the **CMBQL** (Combolite Query Language) text query interface.

---

## Table of Contents

- [1. Transaction Lifecycle](#1-transaction-lifecycle)
- [2. C99 Native Engine API by Model](#2-c99-native-engine-api-by-model)
  - [2.1 Document Pool Model (`cdb_doc.h`)](#21-document-pool-model-cdb_doch)
  - [2.2 Relational Tables (`cdb_table.h`)](#22-relational-tables-cdb_tableh)
  - [2.3 Cross-Model Graph Links (`cdb_link.h`)](#23-cross-model-graph-links-cdb_linkh)
  - [2.4 Vector Collections (`cdb_vec.h`)](#24-vector-collections-cdb_vech)
  - [2.5 Time Series Stores (`cdb_ts.h`)](#25-time-series-stores-cdb_tsh)
  - [2.6 Streaming Large Blobs (`cdb_blob.h` & `cdb_db.h`)](#26-streaming-large-blobs-cdb_blobh--cdb_dbh)
  - [2.7 Raw Engine Mutations & Catalog Management (`cdb_db.h`)](#27-raw-engine-mutations--catalog-management-cdb_dbh)
  - [2.8 CRDT & Multi-Master Replication (`cdb_crdt.h`)](#28-crdt--multi-master-replication-cdb_crdth)
- [3. CMBQL (Combolite Query Language)](#3-cmbql-combolite-query-language)
  - [3.1 Declarative SQL Syntax](#31-declarative-sql-syntax)
  - [3.2 Pipeline / Flux Stream Syntax](#32-pipeline--flux-stream-syntax)
  - [3.3 Graph Link Commands](#33-graph-link-commands)
  - [3.4 Stored Procedures & Transaction Control (CMBP)](#34-stored-procedures--transaction-control-cmbp)

---

## 1. Transaction Lifecycle

All mutating operations in Combolite require an active transaction. The engine writes changes to an append-only journal; upon commit, changes are deterministically applied to the page store.

```c
#include "cdb_db.h"

/* 1. Begin transaction */
cdb_status s = cdb_tx_begin(db, CDB_DURABILITY_FULL);
if (s != CDB_OK) { /* handle error */ }

/* 2. Perform CRUD operations (documents, tables, links, etc.) */
/* ... */

/* 3. Commit or Abort */
if (success) {
    s = cdb_tx_commit(db);
} else {
    s = cdb_tx_abort(db);
}
```

---

## 2. C99 Native Engine API by Model

### 2.1 Document Pool Model (`cdb_doc.h`)

Documents store arbitrary schema-flexible JSON/CBOR objects with automatic full-text indexing, revision history, and RFC 7396 binary patches.

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create** | `cdb_doc_insert(db, store, summary, sum_len, body, body_len, &id)` | Inserts a new document and returns allocated `entry` ID. |
| **Read (Historical)** | `cdb_doc_get(db, store, id, rev, body, cap, &len, &meta)` | Reads document at specific revision `rev` (or head if `rev == 0`). |
| **Read (In-Tx)** | `cdb_doc_get_in_tx(db, store, id, staged_off, body, cap, &len, &meta)` | Reads document reflecting uncommitted staging changes in open transaction. |
| **Update (Replace)** | `cdb_doc_replace(db, store, id, summary, sum_len, body, body_len)` | Completely replaces body and summary with a new revision. |
| **Update (Patch)** | `cdb_doc_patch(db, store, id, patch, patch_len)` | Applies RFC 7396 binary JSON/CBOR field delta patch. |
| **Delete** | `cdb_doc_delete(db, store, id)` | Marks entry as deleted (tombstone) and removes from indexes. |

```c
/* Example: Insert, Read in Transaction, and Patch */
uint64_t doc_id = 0;
const char *json = "{\"title\":\"Edge DB\",\"stars\":5}";
cdb_doc_insert(db, store_id, "Edge DB", 7, json, strlen(json), &doc_id);

const char *patch = "{\"stars\":6}";
cdb_doc_patch(db, store_id, doc_id, patch, strlen(patch));
```

---

### 2.2 Relational Tables (`cdb_table.h`)

Tables provide strongly-typed relational storage with primary keys, composite secondary indexes, and row-oriented or columnar layouts.

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create** | `cdb_table_insert(db, store, row_bytes, len, &entry)` | Inserts row, checks uniqueness, and updates indexes. |
| **Read (Point)** | `cdb_table_get(db, store, entry, out_buf, cap, &len)` | Point lookup of a row by primary entry ID. |
| **Read (Scan)** | `cdb_table_scan(db, store, index_num, from, from_len, to, to_len, cb, ctx)` | Scans range over index (`index_num = 0` for primary key). |
| **Update** | `cdb_table_update(db, store, entry, changes, len)` | Updates specified column values for an existing row. |
| **Delete** | `cdb_table_delete(db, store, entry)` | Deletes row and purges secondary index keys. |

---

### 2.3 Cross-Model Graph Links (`cdb_link.h`)

Links create lightweight, directed, weighted edges connecting entities across *different* stores and models (e.g., Document $\to$ Relational Row $\to$ Vector).

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create (Link)** | `cdb_link_add(db, type, source_ref, target_ref, weight, timestamp)` | Creates a directed edge between two entities. |
| **Read (Edge)** | `cdb_link_get(db, type, source_ref, target_ref, &out_link, &found)` | Retrieves edge weight, timestamp, and flags. |
| **Read (Traverse)**| `cdb_link_list(db, entry_ref, direction, type, after, out, max, &n)` | Traverses outward (`CDB_LINK_OUT`) or inward (`CDB_LINK_IN`) links. |
| **Update** | `cdb_link_reinforce(db, type, source_ref, target_ref, delta_weight, ts)` | Adjusts edge weight (Hebbian learning / decay). |
| **Delete (Unlink)**| `cdb_link_remove(db, type, source_ref, target_ref)` | Removes directed edge. |

```c
/* Example: Link a user document to an organization table entry */
cdb_ref user = { user_store_id, 101 };
cdb_ref org  = { org_store_id, 42 };
cdb_link_add(db, link_type_id, user, org, 1.0f, now_ns);
```

---

### 2.4 Vector Collections (`cdb_vec.h`)

Dense embeddings for similarity search, with HNSW-style proximity graphs.

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create** | `cdb_vec_insert(db, store, vec_bytes, len, src_store, src_entry, &entry)` | Stores vector with optional link to source entity. |
| **Read (Get)** | `cdb_vec_get(db, store, entry, out, cap, &len, &src_store, &src_entry)` | Retrieves raw vector embedding and source reference. |
| **Read (Search)**| `cdb_vec_search(db, store, query_vec, len, measure, k, out_ids, out_dist, &found)` | k-Nearest Neighbors search (`L2`, `Cosine`, `Dot`). |
| **Delete** | `cdb_vec_delete(db, store, entry)` | Deletes vector from block store and proximity graph. |

---

### 2.5 Time Series Stores (`cdb_ts.h`)

High-throughput metric and event logging with bit-packed Gorilla/XOR compression, downsampling, and TTL retention policies.

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create (Append)**| `cdb_ts_append(db, store, series, points_array, count)` | Appends batch of timestamped metric points. |
| **Read (Range)** | `cdb_ts_range(db, store, series, from_ts, to_ts, buf, cap, cb, ctx)` | Scans compressed blocks across a time interval. |
| **Update (TTL)** | `cdb_ts_set_retention(db, store, series, max_age_ticks)` | Configures automatic retention window for series. |
| **Delete (Prune)** | `cdb_ts_drop_before(db, store, series, timestamp)` | Truncates historical blocks older than timestamp. |

---

### 2.6 Streaming Large Blobs (`cdb_blob.h` & `cdb_db.h`)

Content-addressed, chunked binary storage (e.g., audio, images, ML weights) with zero-heap streaming and HMAC tamper verification.

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Create (Write)** | `cdb_db_blob_write_begin(db, store, scratch, &writer)`<br>`cdb_db_blob_write_chunk(&writer, chunk, len)`<br>`cdb_db_blob_write_end(&writer, &out_ref)` | Streams data chunks into pages, calculating digest and returning reference. |
| **Read (Stream)** | `cdb_db_blob_read_begin(db, store, &ref, page_buf, verify, &reader)`<br>`cdb_db_blob_read_chunk(&reader, dest, cap, &out_len)`<br>`cdb_db_blob_read_end(&reader)` | Reads blob chunk by chunk, verifying checksums. |
| **Delete** | `cdb_db_blob_free(db, store, &ref, page_buf)` | De-allocates and reclaims underlying blob pages. |

---

### 2.7 Raw Engine Mutations & Catalog Management (`cdb_db.h`)

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Raw Put** | `cdb_tx_put(db, store, entry, op, data, len)` | Writes low-level change (`CDB_OP_INSERT`, `REPLACE`, `PATCH`, `DELETE`). |
| **Create Store** | `cdb_db_create_store(db, name, len, model, &id)` | Allocates and initializes a new store. |
| **Find Store** | `cdb_db_find_store(db, name, len, &id, &found)` | Looks up store ID by human-readable name. |
| **Drop Store** | `cdb_db_drop_store(db, store_id)` | Drops store and unlinks all catalog entries. |

---

### 2.8 CRDT & Multi-Master Replication (`cdb_crdt.h`)

Deterministic multi-master replication without centralized locks:

| Operation | C99 Function | Description |
| :--- | :--- | :--- |
| **Export Deltas** | `cdb_db_crdt_export_slice(db, since_vector, out_buf, cap, &out_len)` | Serializes delta slice of mutations committed since peer's known vector. |
| **Import Deltas** | `cdb_db_crdt_import_slice(db, in_buf, in_len, &applied_count)` | Applies incoming delta slice with automatic HLC clock updates and echo suppression. |
| **Field LWW Merge**| `cdb_crdt_lww_merge(fields_a, count_a, fields_b, count_b, out, cap, &out_cnt)` | Merges document/table fields using Last-Write-Wins with tombstone tracking. |
| **PN-Counter** | `cdb_pn_counter_inc` / `dec` / `val` / `merge` | Distributed counter supporting commutative multi-peer merges. |
| **Link AWORSet** | `cdb_crdt_link_resolve(token_a, token_b)` | Resolves concurrent link addition vs removal (Add-Wins). |

---

## 3. CMBQL (Combolite Query Language)

CMBQL supports two complementary formats compiled to bytecode: **Declarative SQL** and **Pipeline / Flux Stream Syntax**.

### 3.1 Declarative SQL Syntax

#### Create (INSERT)
```sql
INSERT INTO users (id, name, email, score) 
VALUES (101, "Alice", "alice@example.com", 98.5);
```

#### Read (SELECT)
```sql
SELECT title, author, score 
FROM Scan("books") 
WHERE score >= 0.8 
SORT BY score DESC 
LIMIT 10 OFFSET 0;
```
*Advanced Clauses:*
- **Joins:** `FROM Scan("orders") JOIN Scan("users") ON orders.user_id == users.id`
- **Aggregations:** `GROUP BY category AGGREGATE count(), sum(price), avg(rating)`
- **Temporal Joins:** `ASOF_JOIN Scan("market_ticks") ON ts`
- **Array Unnesting:** `UNNEST tags AS tag`

#### Update (UPDATE)
```sql
UPDATE users 
SET score = score + 5.0, status = "active" 
WHERE id == 101;
```

#### Delete (DELETE)
```sql
DELETE FROM users 
WHERE id == 101 OR score < 10.0;
```

---

### 3.2 Pipeline / Flux Stream Syntax

Streaming pipelines execute with zero heap allocations through Volcano-style iterators:

```sql
Scan("telemetry") 
  | filter(temp > 85.0 AND voltage > 3.3) 
  | window(field: "ts", size: 60000) 
  | project(device_id, temp, voltage) 
  | sort(temp DESC) 
  | limit(25);
```

#### Multi-Modal Operators in Pipelines
- **Full-Text BM25 Search:**
  ```sql
  TextSearch("documents", "distributed consensus") | filter(score > 2.0) | limit(10);
  ```
- **Vector kNN Search:**
  ```sql
  VectorSearch("articles_vec", [0.12, 0.98, -0.45], k: 20);
  ```
- **Hybrid Search (Reciprocal Rank Fusion):**
  ```sql
  HybridRank(
    TextSearch("docs", "crdt offline sync"),
    VectorSearch("docs_vec", $query_emb, k: 50),
    alpha: 0.7
  ) | limit(10);
  ```
- **Graph Traversal:**
  ```sql
  Traverse(from: $user_ref, type: "follows", depth: 2, direction: "out");
  ```

---

### 3.3 Graph Link Commands

```sql
/* Create directed edge with metadata */
LINK ("users", 101) TO ("organizations", 42) TYPE "member_of" PROPERTIES {"role": "admin"};

/* Remove edge */
UNLINK ("users", 101) FROM ("organizations", 42) TYPE "member_of";
```

---

### 3.4 Stored Procedures & Transaction Control (CMBP)

Stored procedures compile down to compact sandboxed bytecode executed within memory limits:

```sql
PROCEDURE transfer_funds($src_id, $dst_id, $amount)
BEGIN
    BEGIN TRANSACTION;
    
    LET $src_balance = (SELECT balance FROM Scan("accounts") WHERE id == $src_id);
    IF $src_balance < $amount THEN
        ROLLBACK;
        RETURN -1;
    END;
    
    SAVEPOINT before_transfer;
    UPDATE accounts SET balance = balance - $amount WHERE id == $src_id;
    UPDATE accounts SET balance = balance + $amount WHERE id == $dst_id;
    
    COMMIT;
    RETURN 0;
END;
```
