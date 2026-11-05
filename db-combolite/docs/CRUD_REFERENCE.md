# Combolite CRUD Reference: C99 API & CMBQL

This document provides a comprehensive reference of all operations available in Combolite across the **C99 Engine API**, **Declarative SQL CMBQL**, and **Functional / Piped Flux CMBQL**, guaranteeing complete command parity across the entire storage engine.

---

## Table of Contents

- [1. Transaction Lifecycle](#1-transaction-lifecycle)
- [2. Complete Operation Parity Matrix](#2-complete-operation-parity-matrix)
  - [2.1 Secure Enclaves & Key Rotation](#21-secure-enclaves--key-rotation)
  - [2.2 Identities & RBAC Access Control](#22-identities--rbac-access-control)
  - [2.3 Catalog DDL & Schemas](#23-catalog-ddl--schemas)
  - [2.4 Event Triggers](#24-event-triggers)
  - [2.5 Document Store Model](#25-document-store-model)
  - [2.6 Relational Tables Model](#26-relational-tables-model)
  - [2.7 Cross-Model Graph Links](#27-cross-model-graph-links)
  - [2.8 Time Series Model](#28-time-series-model)
  - [2.9 Vector Collections](#29-vector-collections)
  - [2.10 Streaming Blobs](#210-streaming-blobs)
  - [2.11 Engine Maintenance & Optimization](#211-engine-maintenance--optimization)
  - [2.12 CRDT Multi-Master Replication](#212-crdt-multi-master-replication)
- [3. Streaming Query Operators & Piped Flux Sinks](#3-streaming-query-operators--piped-flux-sinks)
- [4. Stored Procedures & Control Flow (CMBP)](#4-stored-procedures--control-flow-cmbp)

---

## 1. Transaction Lifecycle

All mutating operations in Combolite require an active transaction. The engine writes changes to an append-only write-ahead journal; upon commit, changes are deterministically reconciled into the page store.

### C99 API
```c
#include "cdb_db.h"

/* Begin transaction */
cdb_status s = cdb_tx_begin(db, CDB_DURABILITY_FULL);

/* Perform CRUD operations... */

/* Commit or Abort */
if (success) {
    s = cdb_tx_commit(db);
} else {
    cdb_tx_abort(db);
}
```

### CMBQL Declarative SQL & Flux
```sql
BEGIN TRANSACTION;
/* ... mutations ... */
COMMIT;

/* Or rollback */
ROLLBACK;
```

---

## 2. Complete Operation Parity Matrix

### 2.1 Secure Enclaves & Key Rotation

Combolite provides confidential storage domains protected with hardware/AES-256-GCM authenticated encryption and zero-downtime key rotation.

> **Not yet effective.** A store (document pool, table, vector collection, time series or links) can't be placed in an enclave yet, so its data is written **unencrypted**, whatever enclaves exist. Enclaves are also not saved: after a reopen the database no longer knows them. Don't rely on enclaves to keep data confidential until the enclave work in spec sections 12.8–12.10 is built.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Create Enclave** | `cdb_db_create_enclave(db, id, uri)` | `CREATE ENCLAVE 1 KEY "vault://keys/1";` | `create_enclave(id: 1, key: "vault://keys/1");` |
| **Open Enclave** | `cdb_db_open_enclave(db, id, uri)` | `OPEN ENCLAVE 1 KEY "vault://keys/1";` | `open_enclave(id: 1, key: "vault://keys/1");` |
| **Rotate Master Key** | `cdb_db_rotate_master_key(db, uri)` | `ROTATE MASTER KEY "vault://keys/mk2";` | `rotate(target: "master", key: "vault://keys/mk2");` |
| **Rotate Enclave Key** | `cdb_db_rotate_enclave_key(db, id, uri)` | `ROTATE ENCLAVE 1 KEY "vault://keys/e2";` | `rotate(target: "enclave", id: 1, key: "vault://keys/e2");` |
| **Check Rotation** | `cdb_db_check_key_rotation(db)` | `CHECK ROTATION;` | `check_rotation();` |

---

### 2.2 Identities & RBAC Access Control

Cryptographic principal authentication (secp256k1 public keys) and hierarchical permission checks.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Register Principal** | `cdb_db_register_principal(db, pid, tid, pubkey)` | `REGISTER PRINCIPAL 101 TENANT 1 PUBKEY "0279be...";` | `register_principal(id: 101, tenant: 1, pubkey: "0279be...");` |
| **Grant Role** | `cdb_db_grant_role(db, pid, rid, scope, perms)` | `GRANT ROLE 1 TO 101 SCOPE 500 PERMS 15;` | `grant_role(role: 1, to: 101, scope: 500, perms: 15);` |
| **Check Permission** | `cdb_db_check_perm(db, pid, scope, perms)` | `CHECK PERM 101 SCOPE 500 PERMS 1;` | `check_perm(principal: 101, scope: 500, perms: 1);` |

---

### 2.3 Catalog DDL & Schemas

Creating, dropping, and defining schemas for multi-model stores.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Create Store** | `cdb_db_create_store(db, name, len, model, &id)` | `CREATE STORE "docs" MODEL document;` | `create_store(name: "docs", model: "document");` |
| **Drop Store** | `cdb_db_drop_store(db, store_id)` | `DROP STORE "temp";` | `drop_store("temp");` |
| **Add Schema** | `cdb_db_add_schema(db, store, sch, len, &v)` | `ADD SCHEMA "docs" "{\"type\":\"object\"}";` | `add_schema(store: "docs", schema: "{\"type\":\"object\"}");` |

*Supported models:* `document`, `table` (or `relational`), `vector`, `timeseries`, `link`.

---

### 2.4 Event Triggers

Post-commit event triggers with recursive depth limits and cascading safety checks.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Create Trigger** | `cdb_db_trigger_register(db, name, len, sid, ev, proc, plen, depth)` | `CREATE TRIGGER trg ON 100 AFTER INSERT EXECUTE "audit" MAX DEPTH 8;` | `create_trigger(name: "trg", on: 100, events: 1, execute: "audit", max_depth: 8);` |
| **Drop Trigger** | `cdb_db_trigger_unregister(db, name, len)` | `DROP TRIGGER trg;` | `drop_trigger("trg");` |

---

### 2.5 Document Store Model

Documents store schema-flexible objects with revision history, full-text summaries, and RFC 7396 delta patches.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Insert Document** | `cdb_doc_insert(db, store, sum, slen, body, blen, &id)` | `INSERT INTO "docs" ("{\"title\":\"Dune\"}", "Dune summary");` | `insert(store: "docs", data: "{\"title\":\"Dune\"}", summary: "Dune summary");` |
| **Patch Document** | `cdb_doc_patch(db, store, entry, patch, plen)` | `PATCH "docs" 1 WITH "{\"rating\":5}";` | `patch(store: "docs", entry: 1, patch: "{\"rating\":5}");` |
| **Replace Document**| `cdb_doc_replace(db, store, entry, sum, slen, body, blen)` | `REPLACE "docs" 1 WITH "{\"title\":\"Messiah\"}";` | `replace(store: "docs", entry: 1, data: "{\"title\":\"Messiah\"}");` |
| **Delete Document** | `cdb_doc_delete(db, store, entry)` | `DELETE FROM "docs" WHERE id = 1;` | `delete(store: "docs", entry: 1);` |

---

### 2.6 Relational Tables Model

Strongly-typed tables with composite secondary indexes, unique constraints, and row/columnar layouts.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Insert Row** | `cdb_table_insert(db, store, row, len, &entry)` | `INSERT INTO TABLE "users" ("{\"name\":\"Alice\"}");` | `table_insert(store: "users", data: "{\"name\":\"Alice\"}");` |
| **Update Row** | `cdb_table_update(db, store, entry, ch, len)` | `UPDATE TABLE "users" SET "{\"name\":\"Bob\"}" WHERE id = 10;` | `update(store: "users", entry: 10, changes: "{\"name\":\"Bob\"}");` |
| **Delete Row** | `cdb_table_delete(db, store, entry)` | `DELETE FROM TABLE "users" WHERE id = 10;` | `delete(store: "users", entry: 10);` |

---

### 2.7 Cross-Model Graph Links

Directed, weighted, temporal edges linking entities across heterogeneous models.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Define Link Type** | `cdb_link_define_type(db, name, len, card, enf, inv, &tid)` | `CREATE LINK TYPE "authored" CARDINALITY 1:N ENFORCED;` | `create_link_type(name: "authored", cardinality: "1:N", enforced: true);` |
| **Add Link** | `cdb_link_add(db, type, src, dst, weight, ts)` | `LINK 1 FROM 10:100 TO 20:200 WEIGHT 2.5 TIME 5000;` | `link(type: 1, from: [10, 100], to: [20, 200], weight: 2.5, time: 5000);` |
| **Reinforce Link** | `cdb_link_reinforce(db, type, src, dst, weight, ts)` | `REINFORCE LINK 1 FROM 10:100 TO 20:200 WEIGHT 1.5;` | `reinforce(type: 1, from: [10, 100], to: [20, 200], weight: 1.5);` |
| **Remove Link** | `cdb_link_remove(db, type, src, dst)` | `UNLINK 1 FROM 10:100 TO 20:200;` | `unlink(type: 1, from: [10, 100], to: [20, 200]);` |

---

### 2.8 Time Series Model

High-throughput metrics and events with delta-encoded nibblet pages, retention enforcement, downsampling, and pruning.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Create Series** | `cdb_ts_series_create(db, store, name, len, tags, tlen, &id)` | `CREATE SERIES "metrics" "cpu";` | `series(store: "metrics", name: "cpu");` |
| **Append Point** | `cdb_ts_append(db, store, series, ts, val)` | `APPEND INTO TIMESERIES "metrics" SERIES 1 TIME 1000 VALUE 42.5;` | `append(store: "metrics", series: 1, time: 1000, value: 42.5);` |
| **Prune Points** | `cdb_ts_prune(db, store, series, before_ts, &freed)` | `PRUNE TIMESERIES "metrics" SERIES 1 BEFORE 500;` | `prune(store: "metrics", series: 1, before: 500);` |
| **Set Retention** | `cdb_ts_retention_set(db, store, series, max_age_ns)` | `SET RETENTION TIMESERIES "metrics" SERIES 1 AGE 3600;` | `retention(store: "metrics", series: 1, age: 3600);` |
| **Apply Retention**| `cdb_ts_retention_apply(db, store, series, now_ts, &p)` | `APPLY RETENTION TIMESERIES "metrics" SERIES 1 TIME 5000;` | `apply_retention(store: "metrics", series: 1, time: 5000);` |
| **Downsample** | `cdb_ts_downsample_run(db, spec, from, to, &pts)` | `DOWNSAMPLE TIMESERIES SOURCE 1:1 INTO 1:2 STEP 60 FROM 0 TO 1000;` | `downsample(source: [1, 1], into: [1, 2], step: 60, from: 0, to: 1000);` |

---

### 2.9 Vector Collections

HNSW graph vector indexes with float vector embeddings.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Insert Vector** | `cdb_vec_insert(db, store, vec, len, s_st, s_ent, &id)` | `INSERT INTO VECTORS "emb" ("\x00\x01\x02");` | `vec_insert(store: "emb", data: "\x00\x01\x02");` |
| **Delete Vector** | `cdb_vec_delete(db, store, entry)` | `DELETE FROM VECTORS "emb" WHERE id = 1;` | `delete(store: "emb", entry: 1);` |

---

### 2.10 Streaming Blobs

Large binary object streaming with content-addressed chunking and Blake3/SHA-256 verification.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Free Blob** | `cdb_db_blob_free(db, store, &ref, page_buf)` | `FREE BLOB 1 PAGE 5 COUNT 1 LEN 1024;` | `free_blob(store: 1, page: 5, count: 1, len: 1024);` |

---

### 2.11 Engine Maintenance & Optimization

Online compaction, dead-space physical page reclaim, and asynchronous reconciliation.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Online Compact** | `cdb_db_compact(db, max_moves, &moved, &reclaimed)` | `COMPACT MAX 100;` | `compact(max: 100);` |
| **Reclaim Pages** | `cdb_db_reclaim(db, &pages_freed, &nibs_freed)` | `RECLAIM;` | `reclaim();` |
| **Reconcile State**| `cdb_db_reconcile(db)` | `RECONCILE;` | `reconcile();` |

---

### 2.12 CRDT Multi-Master Replication

Peer identification, vector clocks, and slice synchronization for peer-to-peer replication.

| Operation | C99 Function | Declarative SQL | Functional Flux |
| :--- | :--- | :--- | :--- |
| **Set Peer ID** | `cdb_db_set_peer_id(db, peer_id)` | `SET PEER 2;` | `peer(id: 2);` |
| **Import CRDT Slice**| `cdb_db_crdt_import_slice(db, slice, len, &applied)`| `SYNC IMPORT "slice_data";` | `sync(slice: "slice_data");` |

---

## 3. Streaming Query Operators & Piped Flux Sinks

CMBQL supports functional piping where query stages or generator functions stream items directly into sink stores using zero allocations:

```sql
/* Yield items and pipe directly into target store */
yield("{\"title\":\"Streamed Document\"}") | into("docs");

/* Filter and transform stream before storage */
Scan("telemetry") 
  | filter(temp > 85.0) 
  | project(device_id, temp, ts) 
  | into("alerts");
```

### Multi-Modal Operators in Pipelines
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

## 4. Stored Procedures & Control Flow (CMBP)

Stored procedures compile down to sandboxed bytecode executed within deterministic instruction budgets and register windows:

```sql
PROCEDURE audit_and_transfer($src_id, $dst_id, $amount)
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
    
    /* Enclave-protected audit entry */
    insert(store: "secure_audit", data: "{\"action\":\"transfer\"}");
    
    COMMIT;
    RETURN 0;
END;
```
