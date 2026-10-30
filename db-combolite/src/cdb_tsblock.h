/* Time-series blocks: the payload that holds one series' points for one
 * time window, stored column by column.
 *
 * Fixed-width fields are in the file's byte order (cdb_endian.h).
 *
 *   Header, 32 bytes:
 *     series u64, window_start i64, window_end i64, count u32, nfields u16,
 *     precision u8, flags u8.
 *   Timestamp column:
 *     first ts i64, bit count u32, ceil(bits/8) bytes of the timestamp codec
 *     (cdb_tsb_time_*) for points 2..count.
 *   For each field, in layout order:
 *     field id u64, type u8 (a cdb_value_type), presence bitmap of
 *     ceil(count/8) bytes (bit i, most significant bit first, is 1 when point
 *     i has the field; padding bits zero), then over the present values only:
 *       INT          bit count u32 + int codec bytes
 *       F64, F32     bit count u32 + float codec bytes (fbits, width 64 or 32)
 *       BOOL         bit count u32 + 1 bit per value
 *       STRING       dict entry count u32, dict byte length u32, dict bytes
 *                    (varint length + UTF-8 bytes per entry; distinct, in order
 *                    of first use), bit count u32 + dict index codec bytes,
 *                    where n = the entries before this value and n = new entry
 *       other types  byte length u32 + the concatenated canonical encodings
 *
 * Every bitstream ends with zero padding to a whole byte. Writers produce the
 * one canonical form, and readers reject anything else. */
#ifndef CDB_TSBLOCK_H
#define CDB_TSBLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"
#include "cdb_tsbits.h"

#define CDB_TS_MAX_FIELDS 16u
#define CDB_TS_BLOCK_MAX_POINTS 1024u
#define CDB_TS_BLOCK_COV 0x01u  /* flag: change-of-value storage in effect */
#define CDB_TS_BLOCK_LATE 0x02u /* flag: holds late points that were folded in */

#define CDB_TS_BLOCK_HEADER 32u

typedef struct cdb_ts_field {
    uint64_t id;
    uint8_t type; /* a cdb_value_type */
} cdb_ts_field;

typedef struct cdb_ts_layout {
    uint64_t series;
    int64_t window_start, window_end; /* the block's window [start, end) */
    uint8_t precision, flags;
    uint16_t nfields;
    cdb_ts_field fields[CDB_TS_MAX_FIELDS];
} cdb_ts_layout;

/* One field of one point. bits: int64 (two's complement), f64/f32 bit
 * pattern, bool 0/1. bytes/len: a string's UTF-8 bytes, or for any other
 * type the value's canonical encoding (14.6). */
typedef struct cdb_ts_val {
    uint8_t present;
    uint64_t bits;
    const uint8_t *bytes;
    size_t len;
} cdb_ts_val;

typedef struct cdb_ts_point {
    int64_t ts;
    cdb_ts_val v[CDB_TS_MAX_FIELDS];
} cdb_ts_point;

/* The encoder state after a block's last point, so an append needs no decode. */
typedef struct cdb_tsblock_state {
    uint32_t count;
    cdb_tsb_time t;
    cdb_tsb_int ints[CDB_TS_MAX_FIELDS];
    cdb_tsb_float floats[CDB_TS_MAX_FIELDS];
    uint32_t dict_n[CDB_TS_MAX_FIELDS];
} cdb_tsblock_state;

/* Builds the canonical block payload for n points (1 <= n <= MAX_POINTS,
 * timestamps strictly increasing, all within the window). st (may be NULL)
 * gets the state after the last point. CDB_E_OVERFLOW when it doesn't fit cap.
 * A bad layout or point is CDB_E_INVALID_ARG. */
cdb_status cdb_tsblock_build(const cdb_ts_layout *L, const cdb_ts_point *pts, size_t n, cdb_byte_order o,
                             uint8_t *out, size_t cap, size_t *len, cdb_tsblock_state *st);

/* Reads and checks a block's header (and that its columns are well formed).
 * count, first_ts and last_ts may be NULL. */
cdb_status cdb_tsblock_parse(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_ts_layout *L, uint32_t *count,
                             int64_t *first_ts, int64_t *last_ts);

/* Decodes every point, fully validating canonical form. Strings and plain
 * values point into pl. st (may be NULL) gets the state after the last point.
 * CDB_E_BUFFER_TOO_SMALL when cap is less than the block's count. */
cdb_status cdb_tsblock_decode(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_ts_layout *L,
                              cdb_ts_point *out, size_t cap, size_t *n, cdb_tsblock_state *st);

/* Appends point p (p->ts > the block's last timestamp, within the window) to
 * block pl, writing the new block to out (never in place). st is the state
 * from build/decode/append, and is updated. The result is byte-identical to
 * building the block from all its points at once. CDB_E_OVERFLOW when it
 * doesn't fit cap, or the block has MAX_POINTS. On failure st is unchanged. */
cdb_status cdb_tsblock_append(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_tsblock_state *st,
                              const cdb_ts_point *p, uint8_t *out, size_t cap, size_t *outlen);

#endif
