/* System audit series and hash-chaining (spec 10.7, 10.6, 12.7).
 *
 * Provides a tamper-evident, SHA-256 hash-chained series recording database
 * governance events:
 * - Timestamp collisions in time-series (recording the replaced values)
 * - Document & table revisions with principal identities
 * - Schema creation and mutations
 * - Baton handoffs, recovery, and crash events
 * - Key rotations and enclave operations
 *
 * Retention and downsampling never apply to this series. */
#ifndef CDB_AUDIT_H
#define CDB_AUDIT_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

/* Audit event types (spec 10.7, 10.6, 12.7). */
#define CDB_AUDIT_COLLISION 1u  /* Time-series collision (store, series, ts, old value) */
#define CDB_AUDIT_REVISION  2u  /* Document/table revision + principal identity */
#define CDB_AUDIT_SCHEMA    3u  /* Schema creation / migration */
#define CDB_AUDIT_BATON     4u  /* Baton handoff, crash recovery */
#define CDB_AUDIT_KEY_OP    5u  /* Master/enclave key rotation */
#define CDB_AUDIT_CUSTOM    6u  /* Application governance / health event */

/* Reserved store ID for the built-in public audit series. */
#define CDB_AUDIT_STORE_ID UINT64_C(0xFFFFFFFFFFFFFFF0)

/* An audit event record. */
typedef struct cdb_audit_event {
    int64_t ts;
    uint64_t seq;
    uint16_t event_type;
    uint32_t principal_id;
    uint64_t store_id;
    uint64_t entity_id;
    uint8_t prev_hash[32];
    uint8_t hash[32];
    uint32_t payload_len;
    const uint8_t *payload;
} cdb_audit_event;

struct cdb_db;

/* Appends a new event to the audit series with SHA-256 hash chaining. */
cdb_status cdb_audit_log(struct cdb_db *db, uint16_t event_type, uint32_t principal_id,
                         uint64_t store_id, uint64_t entity_id,
                         const void *payload, size_t payload_len);

/* Verifies the integrity of the SHA-256 hash chain over the range [from_ts, to_ts).
 * Sets *valid to 1 if valid, 0 if tampering or corruption is detected. */
cdb_status cdb_audit_verify(struct cdb_db *db, int64_t from_ts, int64_t to_ts, int *valid);

/* Queries events in [from_ts, to_ts). If payload_buf is provided, payloads are copied
 * into payload_buf sequentially. */
cdb_status cdb_audit_query(struct cdb_db *db, int64_t from_ts, int64_t to_ts,
                           uint8_t *payload_buf, size_t payload_cap,
                           cdb_audit_event *events, size_t max_events, size_t *count);

#endif
