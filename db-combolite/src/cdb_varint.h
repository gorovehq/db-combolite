/* LEB128 varints and zigzag encoding (spec 14.6.1). Decoding enforces the
 * shortest form, which the canonical encoding requires (spec 14.6.5). */
#ifndef CDB_VARINT_H
#define CDB_VARINT_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

#define CDB_VARINT_MAX_LEN 10 /* bytes in the longest varint (a full 64-bit value) */

/* Bytes the encoding of v takes (1 to 10). */
size_t cdb_varint_size(uint64_t v);

/* Writes v to out, which must have room for cdb_varint_size(v) bytes.
 * Returns the number of bytes written. */
size_t cdb_varint_encode(uint64_t v, uint8_t *out);

/* Reads one varint from in[0..len). On success stores the value in *v and
 * the bytes consumed in *n. Rejects non-shortest forms and values over 64 bits. */
cdb_status cdb_varint_decode(const uint8_t *in, size_t len, uint64_t *v, size_t *n);

/* Zigzag maps signed to unsigned so small magnitudes stay small:
 * 0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ... */
static inline uint64_t cdb_zigzag_encode(int64_t v)
{
    uint64_t u = (uint64_t)v;
    return (u << 1) ^ (v < 0 ? UINT64_MAX : 0);
}

static inline int64_t cdb_zigzag_decode(uint64_t u)
{
    uint64_t magnitude = u >> 1;
    if (u & 1u)
        return -(int64_t)magnitude - 1;
    return (int64_t)magnitude;
}

#endif
