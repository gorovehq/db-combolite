#include "cdb_varint.h"

size_t cdb_varint_size(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t cdb_varint_encode(uint64_t v, uint8_t *out)
{
    size_t n = 0;
    while (v >= 0x80u) {
        out[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

cdb_status cdb_varint_decode(const uint8_t *in, size_t len, uint64_t *v, size_t *n)
{
    uint64_t result = 0;
    size_t i;

    for (i = 0; i < CDB_VARINT_MAX_LEN; i++) {
        uint8_t byte;
        unsigned shift = (unsigned)i * 7u;

        if (i >= len)
            return CDB_E_TRUNCATED;
        byte = in[i];
        /* The tenth byte may only carry the single remaining bit. */
        if (i == CDB_VARINT_MAX_LEN - 1 && byte > 1u)
            return CDB_E_OVERFLOW;
        result |= (uint64_t)(byte & 0x7Fu) << shift;
        if (!(byte & 0x80u)) {
            /* A zero final byte after a continuation is a padded, non-shortest form. */
            if (i > 0 && byte == 0)
                return CDB_E_NOT_CANONICAL;
            *v = result;
            *n = i + 1;
            return CDB_OK;
        }
    }
    return CDB_E_OVERFLOW;
}
