/* Fixed-width loads and stores in an explicit byte order (spec 14.1.2).
 * Byte-at-a-time, so they are safe on any alignment and any host order.
 * Any order other than CDB_ORDER_BIG is treated as little-endian; callers
 * validate the order where it comes from untrusted input. */
#ifndef CDB_ENDIAN_H
#define CDB_ENDIAN_H

#include <stdint.h>

#include "combolite/cdb_common.h"

static inline void cdb_store_u16(uint8_t *p, uint16_t v, cdb_byte_order order)
{
    if (order == CDB_ORDER_BIG) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
}

static inline uint16_t cdb_load_u16(const uint8_t *p, cdb_byte_order order)
{
    if (order == CDB_ORDER_BIG)
        return (uint16_t)((unsigned)p[0] << 8 | p[1]);
    return (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static inline void cdb_store_u32(uint8_t *p, uint32_t v, cdb_byte_order order)
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned shift = (unsigned)(order == CDB_ORDER_BIG ? 3 - i : i) * 8u;
        p[i] = (uint8_t)(v >> shift);
    }
}

static inline uint32_t cdb_load_u32(const uint8_t *p, cdb_byte_order order)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned shift = (unsigned)(order == CDB_ORDER_BIG ? 3 - i : i) * 8u;
        v |= (uint32_t)p[i] << shift;
    }
    return v;
}

static inline void cdb_store_u64(uint8_t *p, uint64_t v, cdb_byte_order order)
{
    int i;
    for (i = 0; i < 8; i++) {
        unsigned shift = (unsigned)(order == CDB_ORDER_BIG ? 7 - i : i) * 8u;
        p[i] = (uint8_t)(v >> shift);
    }
}

static inline uint64_t cdb_load_u64(const uint8_t *p, cdb_byte_order order)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        unsigned shift = (unsigned)(order == CDB_ORDER_BIG ? 7 - i : i) * 8u;
        v |= (uint64_t)p[i] << shift;
    }
    return v;
}

/* The CPU's own byte order: whether the low byte of 1 is stored first. */
static inline cdb_byte_order cdb_host_byte_order(void)
{
    const uint16_t probe = 1;
    return *(const uint8_t *)&probe ? CDB_ORDER_LITTLE : CDB_ORDER_BIG;
}

#endif
