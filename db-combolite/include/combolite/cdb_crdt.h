/* Conflict-Free Replicated Data Types (CRDTs) and Multi-Master P2P Replication (backlog).
 *
 * Provides deterministic, conflict-free multi-master convergence across distributed
 * nodes, edge devices, and microcontrollers without central locking.
 *
 * - Hybrid Logical Clock (HLC) and compact peer version vectors
 * - Field-level Last-Write-Wins (LWW) document & table merging
 * - Positive-Negative Counters (PN-Counter)
 * - Add-Wins Observed-Remove Sets (AWORSet) for graph links
 * - Commutative time-series append
 * - Two-way vector handshake and transport-agnostic delta replication
 */
#ifndef COMBOLITE_CDB_CRDT_H
#define COMBOLITE_CDB_CRDT_H

#include <stddef.h>
#include <stdint.h>
#include "combolite/cdb_common.h"

#define CDB_CRDT_MAX_PEERS 32u
#define CDB_CRDT_WIRE_MAGIC 0x43524454u /* 'CRDT' */

/* Hybrid Logical Clock (HLC) timestamp. Total ordering across nodes. */
typedef struct cdb_hlc {
    uint64_t physical;  /* epoch timestamp (e.g. microseconds or logical counter) */
    uint32_t logical;   /* sub-tick logical sequence counter */
    uint32_t peer_id;   /* unique node/peer identifier */
} cdb_hlc;

/* Compares two HLC timestamps. Returns <0 if a < b, >0 if a > b, 0 if a == b. */
int cdb_hlc_compare(cdb_hlc a, cdb_hlc b);

/* Generates a fresh local tick on clock given wall_clock_now. */
void cdb_hlc_tick(cdb_hlc *clock, uint64_t wall_clock_now);

/* Updates clock upon receiving a remote HLC timestamp. */
void cdb_hlc_update(cdb_hlc *clock, cdb_hlc remote, uint64_t wall_clock_now);

/* One entry in a peer version vector. */
typedef struct cdb_crdt_peer_entry {
    uint32_t peer_id;
    uint64_t counter;   /* mutation sequence counter */
    cdb_hlc hlc;        /* highest HLC seen from peer */
} cdb_crdt_peer_entry;

/* Compact bounded peer version vector. */
typedef struct cdb_crdt_vector {
    cdb_crdt_peer_entry peers[CDB_CRDT_MAX_PEERS];
    uint32_t count;
} cdb_crdt_vector;

/* Vector comparison results. */
#define CDB_CRDT_CMP_EQUAL 0
#define CDB_CRDT_CMP_DOMINATES 1    /* a is strictly ahead of b */
#define CDB_CRDT_CMP_DOMINATED -1  /* a is strictly behind b */
#define CDB_CRDT_CMP_CONCURRENT 2  /* a and b have divergent/concurrent edits */

int cdb_crdt_vector_compare(const cdb_crdt_vector *a, const cdb_crdt_vector *b);
void cdb_crdt_vector_set(cdb_crdt_vector *v, uint32_t peer_id, uint64_t counter, cdb_hlc hlc);
cdb_status cdb_crdt_vector_get(const cdb_crdt_vector *v, uint32_t peer_id, cdb_crdt_peer_entry *out);
void cdb_crdt_vector_merge(cdb_crdt_vector *dest, const cdb_crdt_vector *src);

/* Positive-Negative Counter (PN-Counter). */
typedef struct cdb_pn_counter {
    uint64_t p[CDB_CRDT_MAX_PEERS];
    uint64_t n[CDB_CRDT_MAX_PEERS];
} cdb_pn_counter;

void cdb_pn_counter_init(cdb_pn_counter *c);
void cdb_pn_counter_inc(cdb_pn_counter *c, uint32_t peer_idx, uint64_t amount);
void cdb_pn_counter_dec(cdb_pn_counter *c, uint32_t peer_idx, uint64_t amount);
int64_t cdb_pn_counter_val(const cdb_pn_counter *c);
void cdb_pn_counter_merge(cdb_pn_counter *dest, const cdb_pn_counter *a, const cdb_pn_counter *b);

/* Field-Level LWW Document Attribute. */
#define CDB_CRDT_FIELD_SET 1u
#define CDB_CRDT_FIELD_TOMBSTONE 2u

typedef struct cdb_crdt_field {
    uint64_t field_id;
    uint8_t op;         /* CDB_CRDT_FIELD_SET or CDB_CRDT_FIELD_TOMBSTONE */
    cdb_hlc hlc;        /* timestamp when field was updated */
    const uint8_t *val; /* encoded value bytes (cdb_value format) */
    size_t val_len;
} cdb_crdt_field;

/* Merges two field-level documents (arrays of cdb_crdt_field), producing a convergent
 * merged object with winning fields. */
cdb_status cdb_crdt_lww_merge(const cdb_crdt_field *fields_a, size_t count_a,
                              const cdb_crdt_field *fields_b, size_t count_b,
                              cdb_crdt_field *out_fields, size_t cap_out, size_t *count_out);

/* Graph Link AWORSet (Add-Wins Observed-Remove Set) token */
typedef struct cdb_crdt_link_token {
    uint64_t type;
    uint64_t source_store;
    uint64_t source_entry;
    uint64_t target_store;
    uint64_t target_entry;
    cdb_hlc hlc;
    uint8_t tombstoned; /* 1 if removed */
} cdb_crdt_link_token;

/* Resolves concurrent link addition vs removal (Add-Wins semantics).
 * Returns 1 if link is alive, 0 if tombstoned. */
int cdb_crdt_link_resolve(cdb_crdt_link_token a, cdb_crdt_link_token b);

/* Wire protocol message types. */
#define CDB_CRDT_MSG_HANDSHAKE 1u
#define CDB_CRDT_MSG_DELTA_SLICE 2u

/* Encodes a handshake message with the local version vector. */
cdb_status cdb_crdt_handshake_encode(uint32_t peer_id, const cdb_crdt_vector *v,
                                     uint8_t *buf, size_t cap, size_t *len);

/* Decodes a handshake message. */
cdb_status cdb_crdt_handshake_decode(const uint8_t *buf, size_t len,
                                     uint32_t *peer_id, cdb_crdt_vector *v);

/* CRDT replication delta record. */
typedef struct cdb_crdt_delta_rec {
    uint64_t store_id;
    uint64_t entry_id;
    uint8_t op;          /* CDB_OP_* */
    cdb_hlc hlc;
    const uint8_t *data;
    size_t len;
} cdb_crdt_delta_rec;

/* Encodes a delta slice containing delta records. */
cdb_status cdb_crdt_slice_encode(uint32_t sender_peer_id, const cdb_crdt_delta_rec *recs, size_t count,
                                 uint8_t *buf, size_t cap, size_t *len);

/* Decodes a delta slice into records. */
cdb_status cdb_crdt_slice_decode(const uint8_t *buf, size_t len,
                                 uint32_t *sender_peer_id,
                                 cdb_crdt_delta_rec *recs, size_t cap, size_t *count);

#endif
