/* Order-preserving key encoding (spec 14.8.4). */
#include <string.h>

#include "cdb_endian.h"
#include "cdb_key.h"

void cdb_key_init(cdb_key_builder *k, uint8_t *buf, size_t cap)
{
    k->buf = buf;
    k->cap = cap;
    k->len = 0;
    k->error = buf == NULL && cap ? CDB_E_INVALID_ARG : CDB_OK;
}

/* Checks n more bytes fit; sets the sticky error if not, so a key that did
 * not fit is never used half-built. */
static cdb_status room(cdb_key_builder *k, size_t n)
{
    if (k->error != CDB_OK)
        return k->error;
    if (n > k->cap - k->len) {
        k->error = CDB_E_BUFFER_TOO_SMALL;
        return k->error;
    }
    return CDB_OK;
}

/* Writes the type-rank byte after reserving room for it and the `more`
 * bytes the component will add, so a component is written whole or not
 * at all. */
static cdb_status put_rank(cdb_key_builder *k, unsigned rank, size_t more)
{
    cdb_status s = room(k, 1 + more);
    if (s == CDB_OK)
        k->buf[k->len++] = (uint8_t)rank;
    return s;
}

/* Writes 8 bytes big-endian. Room must already be reserved. Signed values
 * are passed with their sign bit flipped by the caller, which turns signed
 * order into unsigned (memcmp) order. */
static void put_u64(cdb_key_builder *k, uint64_t v)
{
    cdb_store_u64(k->buf + k->len, v, CDB_ORDER_BIG);
    k->len += 8;
}

cdb_status cdb_key_null(cdb_key_builder *k)
{
    return put_rank(k, CDB_KEY_RANK_NULL, 0);
}

cdb_status cdb_key_bool(cdb_key_builder *k, int v)
{
    return put_rank(k, v ? CDB_KEY_RANK_TRUE : CDB_KEY_RANK_FALSE, 0);
}

cdb_status cdb_key_int(cdb_key_builder *k, int64_t v)
{
    cdb_status s = put_rank(k, CDB_KEY_RANK_INT, 8);
    if (s == CDB_OK)
        put_u64(k, (uint64_t)v ^ UINT64_C(0x8000000000000000));
    return s;
}

cdb_status cdb_key_f64(cdb_key_builder *k, double v)
{
    uint64_t bits;
    cdb_status s = put_rank(k, CDB_KEY_RANK_FLOAT, 8);
    if (s != CDB_OK)
        return s;
    memcpy(&bits, &v, sizeof bits);
    if ((bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)
        && (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0)
        bits = UINT64_C(0x7FF8000000000000); /* one NaN */
    if (bits == UINT64_C(0x8000000000000000))
        bits = 0; /* -0 == +0 */
    /* Negative: invert everything (larger magnitude sorts lower).
     * Positive: set the sign bit (sorts above every negative). */
    bits = (bits & UINT64_C(0x8000000000000000)) ? ~bits : bits | UINT64_C(0x8000000000000000);
    put_u64(k, bits);
    return CDB_OK;
}

cdb_status cdb_key_timestamp(cdb_key_builder *k, int64_t ticks)
{
    cdb_status s = put_rank(k, CDB_KEY_RANK_TIMESTAMP, 8);
    if (s == CDB_OK)
        put_u64(k, (uint64_t)ticks ^ UINT64_C(0x8000000000000000));
    return s;
}

/* Strings and binary: each 00 byte becomes 00 FF, then 00 00 ends the
 * component. Since 00 00 sorts below 00 FF and below any other byte, a
 * shorter string sorts before a longer one it prefixes, and the end of the
 * component cannot be confused with its contents. Zeros are counted first
 * so the exact size can be reserved. */
static cdb_status put_escaped(cdb_key_builder *k, unsigned rank, const uint8_t *p, size_t len)
{
    size_t zeros = 0, i;
    cdb_status s;
    for (i = 0; i < len; i++)
        zeros += p[i] == 0;
    if (len > SIZE_MAX - zeros - 3)
        return k->error = CDB_E_OVERFLOW;
    if ((s = put_rank(k, rank, len + zeros + 2)) != CDB_OK)
        return s;
    for (i = 0; i < len; i++) {
        k->buf[k->len++] = p[i];
        if (p[i] == 0)
            k->buf[k->len++] = 0xFF;
    }
    k->buf[k->len++] = 0;
    k->buf[k->len++] = 0;
    return CDB_OK;
}

cdb_status cdb_key_string(cdb_key_builder *k, const char *s, size_t len)
{
    if (s == NULL && len)
        return k->error = CDB_E_INVALID_ARG;
    return put_escaped(k, CDB_KEY_RANK_STRING, (const uint8_t *)s, len);
}

cdb_status cdb_key_binary(cdb_key_builder *k, const void *data, size_t len)
{
    if (data == NULL && len)
        return k->error = CDB_E_INVALID_ARG;
    return put_escaped(k, CDB_KEY_RANK_BINARY, (const uint8_t *)data, len);
}

cdb_status cdb_key_ref(cdb_key_builder *k, uint64_t store_id, uint64_t entry_id)
{
    cdb_status s = put_rank(k, CDB_KEY_RANK_REF, 16);
    if (s == CDB_OK) {
        put_u64(k, store_id);
        put_u64(k, entry_id);
    }
    return s;
}

cdb_status cdb_key_u64(cdb_key_builder *k, uint64_t v)
{
    cdb_status s = room(k, 8);
    if (s == CDB_OK)
        put_u64(k, v);
    return s;
}

int cdb_key_compare(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}
