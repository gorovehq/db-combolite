/* Bit-level codecs for time-series columns.
 *
 * Bitstreams are written most significant bit first within each byte. The
 * bits after the last used bit of the final byte are zero, and a reader
 * rejects non-zero padding. Every encoder only appends bits; it never
 * rewrites earlier ones. Every encode and decode is all or nothing: on
 * failure the writer or reader position and the column state are unchanged.
 *
 * Bucket code for an unsigned value z (the smallest bucket that fits):
 *   z == 0     '0'
 *   z < 2^7    '10'   + 7 bits
 *   z < 2^15   '110'  + 15 bits
 *   z < 2^31   '1110' + 31 bits
 *   otherwise  '1111' + 64 bits
 * Signed differences go through zigzag first ((v << 1) ^ (v >> 63), computed
 * in unsigned arithmetic; the same mapping as cdb_zigzag_encode). */
#ifndef CDB_TSBITS_H
#define CDB_TSBITS_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

/* Bit writer over a caller buffer. */
typedef struct cdb_bitw {
    uint8_t *buf;
    size_t cap_bits;
    size_t bits; /* bits written so far */
} cdb_bitw;

/* Bit reader over a caller buffer. */
typedef struct cdb_bitr {
    const uint8_t *buf;
    size_t len_bits;
    size_t pos; /* bits consumed so far */
} cdb_bitr;

/* Starts an empty stream in buf[0..cap_bytes). */
void cdb_bitw_init(cdb_bitw *w, uint8_t *buf, size_t cap_bytes);

/* Continues an existing stream of `bits` bits in buf[0..cap_bytes), so a
 * column can be appended to later. Fails if bits exceeds the capacity. */
cdb_status cdb_bitw_resume(cdb_bitw *w, uint8_t *buf, size_t cap_bytes, size_t bits);

/* Bytes the stream occupies so far: ceil(bits / 8). */
size_t cdb_bitw_bytes(const cdb_bitw *w);

/* Appends the low nbits (0..64) of value. value must fit in nbits
 * (CDB_E_INVALID_ARG otherwise). CDB_E_BUFFER_TOO_SMALL leaves w unchanged. */
cdb_status cdb_bitw_put(cdb_bitw *w, uint64_t value, unsigned nbits);

void cdb_bitr_init(cdb_bitr *r, const uint8_t *buf, size_t len_bytes);

/* Reads nbits (0..64) into *value. CDB_E_TRUNCATED at the end, with r unchanged. */
cdb_status cdb_bitr_get(cdb_bitr *r, uint64_t *value, unsigned nbits);

/* Checks the stream ends here: fewer than 8 bits remain and they are all
 * zero. Otherwise CDB_E_NOT_CANONICAL (non-zero padding or trailing bytes). */
cdb_status cdb_bitr_check_end(const cdb_bitr *r);

/* Bucket code of an unsigned value. */
size_t cdb_tsb_bucket_bits(uint64_t z);
cdb_status cdb_tsb_bucket_encode(cdb_bitw *w, uint64_t z);
/* Rejects a value in a bigger bucket than needed with CDB_E_NOT_CANONICAL. */
cdb_status cdb_tsb_bucket_decode(cdb_bitr *r, uint64_t *z);

/* Timestamp column: delta-of-delta. The first timestamp is not in the
 * bitstream; the caller stores it and passes it to init. */
typedef struct cdb_tsb_time {
    int64_t prev_t;
    int64_t prev_delta;
} cdb_tsb_time;

void cdb_tsb_time_init(cdb_tsb_time *s, int64_t first_t);
/* Bits that encoding t next would append. */
size_t cdb_tsb_time_bits(const cdb_tsb_time *s, int64_t t);
cdb_status cdb_tsb_time_encode(cdb_bitw *w, cdb_tsb_time *s, int64_t t);
cdb_status cdb_tsb_time_decode(cdb_bitr *r, cdb_tsb_time *s, int64_t *t);

/* Integer column: delta from the previous value, which starts at 0. */
typedef struct cdb_tsb_int {
    int64_t prev;
} cdb_tsb_int;

void cdb_tsb_int_init(cdb_tsb_int *s);
size_t cdb_tsb_int_bits(const cdb_tsb_int *s, int64_t v);
cdb_status cdb_tsb_int_encode(cdb_bitw *w, cdb_tsb_int *s, int64_t v);
cdb_status cdb_tsb_int_decode(cdb_bitr *r, cdb_tsb_int *s, int64_t *v);

/* Float columns: XOR with the previous bit pattern. The first value is raw.
 * Values round-trip bit for bit (NaN payloads, -0.0). */
typedef struct cdb_tsb_float {
    uint64_t prev_bits;
    int started;
} cdb_tsb_float;

void cdb_tsb_float_init(cdb_tsb_float *s);

/* Bit-pattern entry points: width is 64 or 32 (anything else is
 * CDB_E_INVALID_ARG, or 0 bits). For width 32, bits holds the float's
 * pattern in its low 32 bits; encode rejects set high bits. The pattern
 * never passes through a floating-point register, so signalling NaNs keep
 * their exact bits. The f64 and f32 functions below wrap these. */
size_t cdb_tsb_fbits_bits(const cdb_tsb_float *s, uint64_t bits, unsigned width);
cdb_status cdb_tsb_fbits_encode(cdb_bitw *w, cdb_tsb_float *s, uint64_t bits, unsigned width);
cdb_status cdb_tsb_fbits_decode(cdb_bitr *r, cdb_tsb_float *s, unsigned width, uint64_t *bits);

size_t cdb_tsb_f64_bits(const cdb_tsb_float *s, double v);
cdb_status cdb_tsb_f64_encode(cdb_bitw *w, cdb_tsb_float *s, double v);
/* Rejects an L or M that does not match the decoded XOR (CDB_E_NOT_CANONICAL). */
cdb_status cdb_tsb_f64_decode(cdb_bitr *r, cdb_tsb_float *s, double *v);

size_t cdb_tsb_f32_bits(const cdb_tsb_float *s, float v);
cdb_status cdb_tsb_f32_encode(cdb_bitw *w, cdb_tsb_float *s, float v);
cdb_status cdb_tsb_f32_decode(cdb_bitr *r, cdb_tsb_float *s, float *v);

/* Bool column: one bit per value (1 = true). Any non-zero v is true. */
cdb_status cdb_tsb_bool_encode(cdb_bitw *w, int v);
cdb_status cdb_tsb_bool_decode(cdb_bitr *r, int *v);

/* Dictionary index: with n entries before this point, the index i in [0, n]
 * is written in cdb_tsb_dict_width(n) bits; i == n means a new entry.
 * encode: i > n is CDB_E_INVALID_ARG. decode: i > n is CDB_E_MALFORMED. */
unsigned cdb_tsb_dict_width(uint64_t n);
cdb_status cdb_tsb_dict_encode(cdb_bitw *w, uint64_t n, uint64_t i);
cdb_status cdb_tsb_dict_decode(cdb_bitr *r, uint64_t n, uint64_t *i);

#endif
