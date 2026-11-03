#ifndef CDB_CRDT_INTERNAL_H
#define CDB_CRDT_INTERNAL_H

#include "combolite/cdb_crdt.h"
#include "cdb_db.h"

/* Time-series commutative append point deduplication */
typedef struct cdb_crdt_ts_point {
    uint64_t series_id;
    int64_t ts;
    uint32_t peer_id;
    const uint8_t *val;
    size_t len;
} cdb_crdt_ts_point;

/* Checks if point with (series_id, ts, peer_id) matches existing point */
int cdb_crdt_ts_same_point(cdb_crdt_ts_point a, cdb_crdt_ts_point b);

#endif
