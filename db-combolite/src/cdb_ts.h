/* Time series (spec 10, 14.11).
 *
 * A time-series store (model CDB_MODEL_TIME_SERIES) holds many series. A
 * series is found by its key: a name and tags (sorted by tag key). Each has
 * a timestamp precision, a window size, the fields its points carry (an ID
 * and a value type each, at most CDB_TS_MAX_FIELDS) and whether change-of-
 * value storage is on. Points are stored in blocks (cdb_tsblock.h), one
 * logical page each (type 0x0008), keyed by (series, first timestamp); a
 * block never crosses a window. Indexes, all with big-endian keys:
 *   keys:   series key -> series ID
 *   series: series ID -> series record (definition, last point, encoder state)
 *   blocks: (series, first timestamp) -> block page, point count, last timestamp
 *   held:   (series, timestamp) -> a late point held until reconciliation
 *
 * Changes go through transactions and apply at commit; the apply never fails
 * for logical reasons. Late points (older than the series' last) follow the
 * store's late mode (10.5): accept, accept within a lateness, reject, or
 * hold (kept out of sight and folded in at reconciliation). A point at an
 * existing timestamp: equal values are stored once; different values replace
 * the stored ones field by field, and the collision callback is told (10.6).
 * With change-of-value storage, a point equal to the previous one is not
 * stored.
 *
 * A point's values are an encoded object (14.6) of field ID -> value, each
 * field of its declared type; a field may be absent.
 *
 * Buffers: cdb_db_config.ts_buffers (3 pages) and ts_work
 * (cdb_ts_work_size()). */
#ifndef CDB_TS_H
#define CDB_TS_H

#include "cdb_db.h"
#include "cdb_tsblock.h"

/* Timestamp precisions: cdb_ts_precision (cdb_value.h, spec 14.6). */

/* Late modes (spec 10.5). */
#define CDB_TS_LATE_ACCEPT 0u
#define CDB_TS_LATE_WITHIN 1u
#define CDB_TS_LATE_REJECT 2u
#define CDB_TS_LATE_HOLD 3u

#define CDB_TS_MAX_TAGS 8u

typedef struct cdb_ts_tag {
    const char *key;
    size_t key_len;
    const char *value;
    size_t value_len;
} cdb_ts_tag;

typedef struct cdb_ts_def {
    uint8_t precision;       /* CDB_TS_SECONDS .. CDB_TS_NANOS */
    int64_t window;          /* ticks of the precision; 0: one hour */
    int change_of_value;     /* fold a point equal to the previous one */
    uint16_t nfields;
    cdb_ts_field fields[CDB_TS_MAX_FIELDS]; /* IDs strictly increasing */
} cdb_ts_def;

/* A point to append. With use_clock, ts is taken from the environment's
 * clock (CDB_E_UNSUPPORTED without one). values: an encoded object. */
typedef struct cdb_ts_in {
    int64_t ts;
    int use_clock;
    const void *values;
    size_t len;
} cdb_ts_in;

/* A point read back: its timestamp and an encoded object in the caller's
 * buffer, at off, len bytes. */
typedef struct cdb_ts_row {
    int64_t ts;
    size_t off;
    size_t len;
} cdb_ts_row;

size_t cdb_ts_work_size(void);

/* Creates a store; commits immediately, like other catalog changes.
 * lateness is in nanoseconds, for CDB_TS_LATE_WITHIN. */
cdb_status cdb_ts_create(cdb_db *db, const char *name, size_t len, unsigned late_mode, int64_t lateness,
                         uint64_t *store);

/* Finds a series by key, as the open transaction (if any) sees it. With def,
 * inside a transaction, creates it when missing; an existing series is
 * returned as is, whatever def says. *created says which. CDB_E_NOT_FOUND
 * without def when it is missing. */
cdb_status cdb_ts_series(cdb_db *db, uint64_t store, const char *name, size_t len, const cdb_ts_tag *tags,
                         size_t ntags, const cdb_ts_def *def, uint64_t *series, int *created);
cdb_status cdb_ts_describe(cdb_db *db, uint64_t store, uint64_t series, cdb_ts_def *def);

/* Inside a transaction: appends points (any order). A point that is late
 * under the store's mode is refused with CDB_E_STATE; a value of the wrong
 * type or an unknown field is CDB_E_INVALID_ARG. Nothing is journaled when
 * any point is refused. */
cdb_status cdb_ts_append(cdb_db *db, uint64_t store, uint64_t series, const cdb_ts_in *points, size_t n);

/* Inside a transaction: removes every point older than ts (held ones too). */
cdb_status cdb_ts_drop_before(cdb_db *db, uint64_t store, uint64_t series, int64_t ts);

/* Committed points with from <= ts < to, oldest first: at most max rows,
 * values encoded into buf. *next is where to continue (to when done). */
cdb_status cdb_ts_range(cdb_db *db, uint64_t store, uint64_t series, int64_t from, int64_t to, uint8_t *buf,
                        size_t cap, cdb_ts_row *rows, size_t max, size_t *n, int64_t *next);

/* Engine hooks. */
int cdb_ts_is_open(cdb_db *db, uint64_t store);
cdb_status cdb_ts_apply(cdb_db *db, const cdb_delta *d, int *handled);
cdb_status cdb_ts_flush(cdb_db *db); /* at reconciliation: held points, entry counters */

#endif
