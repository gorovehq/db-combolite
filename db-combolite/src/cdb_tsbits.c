#include "cdb_tsbits.h"

#include <string.h>

/* ---- unsigned helpers ---------------------------------------------------- */

/* Zigzag over the two's-complement bit pattern, without signed shifts. */
static uint64_t zz(uint64_t u)
{
    return (u << 1) ^ (0u - (u >> 63));
}

static uint64_t unzz(uint64_t z)
{
    return (z >> 1) ^ (0u - (z & 1u));
}

/* Converts a two's-complement bit pattern to int64 without relying on the
 * implementation-defined unsigned-to-signed conversion. */
static int64_t u2i(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(~u) - 1;
}

static int has_room(const cdb_bitw *w, size_t nbits)
{
    return nbits <= w->cap_bits - w->bits;
}

/* ---- bit writer and reader ----------------------------------------------- */

void cdb_bitw_init(cdb_bitw *w, uint8_t *buf, size_t cap_bytes)
{
    w->buf = buf;
    w->cap_bits = cap_bytes * 8u;
    w->bits = 0;
}

cdb_status cdb_bitw_resume(cdb_bitw *w, uint8_t *buf, size_t cap_bytes, size_t bits)
{
    if (bits > cap_bytes * 8u)
        return CDB_E_INVALID_ARG;
    w->buf = buf;
    w->cap_bits = cap_bytes * 8u;
    w->bits = bits;
    return CDB_OK;
}

size_t cdb_bitw_bytes(const cdb_bitw *w)
{
    return (w->bits + 7u) / 8u;
}

cdb_status cdb_bitw_put(cdb_bitw *w, uint64_t value, unsigned nbits)
{
    size_t pos = w->bits;

    if (nbits > 64u || (nbits < 64u && (value >> nbits) != 0))
        return CDB_E_INVALID_ARG;
    if (!has_room(w, nbits))
        return CDB_E_BUFFER_TOO_SMALL;
    while (nbits > 0) {
        size_t byte = pos >> 3;
        unsigned off = (unsigned)(pos & 7u);
        unsigned room = 8u - off;
        unsigned take = nbits < room ? nbits : room;
        unsigned chunk = (unsigned)((value >> (nbits - take)) & ((1u << take) - 1u));
        /* Keep the bits already written in this byte and clear the rest, so
         * the padding after the new last bit is zero. */
        unsigned keep = off ? ((unsigned)w->buf[byte] & (0xFFu << room)) : 0u;

        w->buf[byte] = (uint8_t)((keep | (chunk << (room - take))) & 0xFFu);
        pos += take;
        nbits -= take;
    }
    w->bits = pos;
    return CDB_OK;
}

void cdb_bitr_init(cdb_bitr *r, const uint8_t *buf, size_t len_bytes)
{
    r->buf = buf;
    r->len_bits = len_bytes * 8u;
    r->pos = 0;
}

cdb_status cdb_bitr_get(cdb_bitr *r, uint64_t *value, unsigned nbits)
{
    size_t pos = r->pos;
    uint64_t v = 0;

    if (nbits > 64u)
        return CDB_E_INVALID_ARG;
    if (nbits > r->len_bits - r->pos)
        return CDB_E_TRUNCATED;
    while (nbits > 0) {
        size_t byte = pos >> 3;
        unsigned off = (unsigned)(pos & 7u);
        unsigned room = 8u - off;
        unsigned take = nbits < room ? nbits : room;
        unsigned chunk = ((unsigned)r->buf[byte] >> (room - take)) & ((1u << take) - 1u);

        v = (v << take) | chunk;
        pos += take;
        nbits -= take;
    }
    r->pos = pos;
    *value = v;
    return CDB_OK;
}

cdb_status cdb_bitr_check_end(const cdb_bitr *r)
{
    size_t left = r->len_bits - r->pos;
    unsigned tail;

    if (left >= 8u)
        return CDB_E_NOT_CANONICAL;
    if (left == 0)
        return CDB_OK;
    tail = (unsigned)r->buf[r->pos >> 3] & ((1u << left) - 1u);
    return tail ? CDB_E_NOT_CANONICAL : CDB_OK;
}

/* ---- bucket code --------------------------------------------------------- */

#define B7 (UINT64_C(1) << 7)
#define B15 (UINT64_C(1) << 15)
#define B31 (UINT64_C(1) << 31)

size_t cdb_tsb_bucket_bits(uint64_t z)
{
    if (z == 0)
        return 1;
    if (z < B7)
        return 2 + 7;
    if (z < B15)
        return 3 + 15;
    if (z < B31)
        return 4 + 31;
    return 4 + 64;
}

cdb_status cdb_tsb_bucket_encode(cdb_bitw *w, uint64_t z)
{
    cdb_status st;

    if (!has_room(w, cdb_tsb_bucket_bits(z)))
        return CDB_E_BUFFER_TOO_SMALL;
    if (z == 0)
        return cdb_bitw_put(w, 0, 1);
    if (z < B7)
        st = cdb_bitw_put(w, 2, 2);
    else if (z < B15)
        st = cdb_bitw_put(w, 6, 3);
    else if (z < B31)
        st = cdb_bitw_put(w, 14, 4);
    else
        st = cdb_bitw_put(w, 15, 4);
    if (st != CDB_OK)
        return st;
    if (z < B7)
        return cdb_bitw_put(w, z, 7);
    if (z < B15)
        return cdb_bitw_put(w, z, 15);
    if (z < B31)
        return cdb_bitw_put(w, z, 31);
    return cdb_bitw_put(w, z, 64);
}

cdb_status cdb_tsb_bucket_decode(cdb_bitr *r, uint64_t *z)
{
    static const unsigned width[5] = { 0, 7, 15, 31, 64 };
    static const uint64_t min[5] = { 0, 1, B7, B15, B31 };
    size_t start = r->pos;
    unsigned ones = 0;
    uint64_t bit, v = 0;
    cdb_status st;

    while (ones < 4u) {
        st = cdb_bitr_get(r, &bit, 1);
        if (st != CDB_OK)
            goto fail;
        if (!bit)
            break;
        ones++;
    }
    if (ones > 0) {
        st = cdb_bitr_get(r, &v, width[ones]);
        if (st != CDB_OK)
            goto fail;
        if (v < min[ones]) {
            st = CDB_E_NOT_CANONICAL;
            goto fail;
        }
    }
    *z = v;
    return CDB_OK;
fail:
    r->pos = start;
    return st;
}

/* ---- timestamp column ---------------------------------------------------- */

void cdb_tsb_time_init(cdb_tsb_time *s, int64_t first_t)
{
    s->prev_t = first_t;
    s->prev_delta = 0;
}

static uint64_t time_delta(const cdb_tsb_time *s, int64_t t)
{
    return (uint64_t)t - (uint64_t)s->prev_t;
}

size_t cdb_tsb_time_bits(const cdb_tsb_time *s, int64_t t)
{
    return cdb_tsb_bucket_bits(zz(time_delta(s, t) - (uint64_t)s->prev_delta));
}

cdb_status cdb_tsb_time_encode(cdb_bitw *w, cdb_tsb_time *s, int64_t t)
{
    uint64_t delta = time_delta(s, t);
    cdb_status st = cdb_tsb_bucket_encode(w, zz(delta - (uint64_t)s->prev_delta));

    if (st != CDB_OK)
        return st;
    s->prev_t = t;
    s->prev_delta = u2i(delta);
    return CDB_OK;
}

cdb_status cdb_tsb_time_decode(cdb_bitr *r, cdb_tsb_time *s, int64_t *t)
{
    uint64_t z, delta;
    cdb_status st = cdb_tsb_bucket_decode(r, &z);

    if (st != CDB_OK)
        return st;
    delta = (uint64_t)s->prev_delta + unzz(z);
    s->prev_t = u2i((uint64_t)s->prev_t + delta);
    s->prev_delta = u2i(delta);
    *t = s->prev_t;
    return CDB_OK;
}

/* ---- integer column ------------------------------------------------------ */

void cdb_tsb_int_init(cdb_tsb_int *s)
{
    s->prev = 0;
}

size_t cdb_tsb_int_bits(const cdb_tsb_int *s, int64_t v)
{
    return cdb_tsb_bucket_bits(zz((uint64_t)v - (uint64_t)s->prev));
}

cdb_status cdb_tsb_int_encode(cdb_bitw *w, cdb_tsb_int *s, int64_t v)
{
    cdb_status st = cdb_tsb_bucket_encode(w, zz((uint64_t)v - (uint64_t)s->prev));

    if (st != CDB_OK)
        return st;
    s->prev = v;
    return CDB_OK;
}

cdb_status cdb_tsb_int_decode(cdb_bitr *r, cdb_tsb_int *s, int64_t *v)
{
    uint64_t z;
    cdb_status st = cdb_tsb_bucket_decode(r, &z);

    if (st != CDB_OK)
        return st;
    s->prev = u2i((uint64_t)s->prev + unzz(z));
    *v = s->prev;
    return CDB_OK;
}

/* ---- float columns ------------------------------------------------------- */

/* Leading zeros of a non-zero x within a word of `width` bits. */
static unsigned lead_zeros(uint64_t x, unsigned width)
{
    unsigned n = 0;
    while (!((x >> (width - 1u - n)) & 1u))
        n++;
    return n;
}

static unsigned trail_zeros(uint64_t x)
{
    unsigned n = 0;
    while (!((x >> n) & 1u))
        n++;
    return n;
}

/* lbits is the width of the L and (M - 1) fields: 6 for f64, 5 for f32. */
static size_t xor_bits(const cdb_tsb_float *s, uint64_t bits, unsigned width, unsigned lbits)
{
    uint64_t x;

    if (!s->started)
        return width;
    x = bits ^ s->prev_bits;
    if (x == 0)
        return 1;
    return 1u + 2u * lbits + (width - lead_zeros(x, width) - trail_zeros(x));
}

static cdb_status xor_encode(cdb_bitw *w, cdb_tsb_float *s, uint64_t bits, unsigned width,
                             unsigned lbits)
{
    uint64_t x = bits ^ s->prev_bits;
    unsigned l, t, m;
    cdb_status st;

    if (!has_room(w, xor_bits(s, bits, width, lbits)))
        return CDB_E_BUFFER_TOO_SMALL;
    if (!s->started) {
        st = cdb_bitw_put(w, bits, width);
    } else if (x == 0) {
        st = cdb_bitw_put(w, 0, 1);
    } else {
        l = lead_zeros(x, width);
        t = trail_zeros(x);
        m = width - l - t;
        st = cdb_bitw_put(w, 1, 1);
        if (st == CDB_OK)
            st = cdb_bitw_put(w, l, lbits);
        if (st == CDB_OK)
            st = cdb_bitw_put(w, m - 1u, lbits);
        if (st == CDB_OK)
            st = cdb_bitw_put(w, x >> t, m);
    }
    if (st != CDB_OK)
        return st;
    s->prev_bits = bits;
    s->started = 1;
    return CDB_OK;
}

static cdb_status xor_decode(cdb_bitr *r, cdb_tsb_float *s, uint64_t *bits, unsigned width,
                             unsigned lbits)
{
    size_t start = r->pos;
    uint64_t flag, l, m1, v, out;
    unsigned m;
    cdb_status st;

    if (!s->started) {
        st = cdb_bitr_get(r, &out, width);
        if (st != CDB_OK)
            goto fail;
    } else {
        st = cdb_bitr_get(r, &flag, 1);
        if (st != CDB_OK)
            goto fail;
        if (!flag) {
            out = s->prev_bits;
        } else {
            st = cdb_bitr_get(r, &l, lbits);
            if (st == CDB_OK)
                st = cdb_bitr_get(r, &m1, lbits);
            if (st != CDB_OK)
                goto fail;
            m = (unsigned)m1 + 1u;
            if (l + m > width) {
                st = CDB_E_NOT_CANONICAL;
                goto fail;
            }
            st = cdb_bitr_get(r, &v, m);
            if (st != CDB_OK)
                goto fail;
            /* L and M are canonical only if the payload's first and last
             * bits are both set. */
            if (!((v >> (m - 1u)) & 1u) || !(v & 1u)) {
                st = CDB_E_NOT_CANONICAL;
                goto fail;
            }
            out = s->prev_bits ^ (v << (width - (unsigned)l - m));
        }
    }
    s->prev_bits = out;
    s->started = 1;
    *bits = out;
    return CDB_OK;
fail:
    r->pos = start;
    return st;
}

void cdb_tsb_float_init(cdb_tsb_float *s)
{
    s->prev_bits = 0;
    s->started = 0;
}

static uint64_t f64_bits(double v)
{
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static uint64_t f32_bits(float v)
{
    uint32_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

/* Width of the L and (M - 1) fields for a float width; 0 for a bad width. */
static unsigned fbits_lbits(unsigned width)
{
    if (width == 64u)
        return 6;
    if (width == 32u)
        return 5;
    return 0;
}

size_t cdb_tsb_fbits_bits(const cdb_tsb_float *s, uint64_t bits, unsigned width)
{
    unsigned lbits = fbits_lbits(width);

    if (lbits == 0 || (width < 64u && (bits >> width) != 0))
        return 0;
    return xor_bits(s, bits, width, lbits);
}

cdb_status cdb_tsb_fbits_encode(cdb_bitw *w, cdb_tsb_float *s, uint64_t bits, unsigned width)
{
    unsigned lbits = fbits_lbits(width);

    if (lbits == 0 || (width < 64u && (bits >> width) != 0))
        return CDB_E_INVALID_ARG;
    return xor_encode(w, s, bits, width, lbits);
}

cdb_status cdb_tsb_fbits_decode(cdb_bitr *r, cdb_tsb_float *s, unsigned width, uint64_t *bits)
{
    unsigned lbits = fbits_lbits(width);

    if (lbits == 0)
        return CDB_E_INVALID_ARG;
    return xor_decode(r, s, bits, width, lbits);
}

size_t cdb_tsb_f64_bits(const cdb_tsb_float *s, double v)
{
    return cdb_tsb_fbits_bits(s, f64_bits(v), 64);
}

cdb_status cdb_tsb_f64_encode(cdb_bitw *w, cdb_tsb_float *s, double v)
{
    return cdb_tsb_fbits_encode(w, s, f64_bits(v), 64);
}

cdb_status cdb_tsb_f64_decode(cdb_bitr *r, cdb_tsb_float *s, double *v)
{
    uint64_t b;
    cdb_status st = cdb_tsb_fbits_decode(r, s, 64, &b);

    if (st == CDB_OK)
        memcpy(v, &b, sizeof *v);
    return st;
}

size_t cdb_tsb_f32_bits(const cdb_tsb_float *s, float v)
{
    return cdb_tsb_fbits_bits(s, f32_bits(v), 32);
}

cdb_status cdb_tsb_f32_encode(cdb_bitw *w, cdb_tsb_float *s, float v)
{
    return cdb_tsb_fbits_encode(w, s, f32_bits(v), 32);
}

cdb_status cdb_tsb_f32_decode(cdb_bitr *r, cdb_tsb_float *s, float *v)
{
    uint64_t b;
    uint32_t b32;
    cdb_status st = cdb_tsb_fbits_decode(r, s, 32, &b);

    if (st == CDB_OK) {
        b32 = (uint32_t)b;
        memcpy(v, &b32, sizeof *v);
    }
    return st;
}

/* ---- bool column --------------------------------------------------------- */

cdb_status cdb_tsb_bool_encode(cdb_bitw *w, int v)
{
    return cdb_bitw_put(w, v ? 1u : 0u, 1);
}

cdb_status cdb_tsb_bool_decode(cdb_bitr *r, int *v)
{
    uint64_t b;
    cdb_status st = cdb_bitr_get(r, &b, 1);

    if (st == CDB_OK)
        *v = (int)b;
    return st;
}

/* ---- dictionary index ---------------------------------------------------- */

unsigned cdb_tsb_dict_width(uint64_t n)
{
    unsigned w = 0;
    while (n) {
        w++;
        n >>= 1;
    }
    return w;
}

cdb_status cdb_tsb_dict_encode(cdb_bitw *w, uint64_t n, uint64_t i)
{
    if (i > n)
        return CDB_E_INVALID_ARG;
    return cdb_bitw_put(w, i, cdb_tsb_dict_width(n));
}

cdb_status cdb_tsb_dict_decode(cdb_bitr *r, uint64_t n, uint64_t *i)
{
    size_t start = r->pos;
    uint64_t v;
    cdb_status st = cdb_bitr_get(r, &v, cdb_tsb_dict_width(n));

    if (st != CDB_OK)
        return st;
    if (v > n) {
        r->pos = start;
        return CDB_E_MALFORMED;
    }
    *i = v;
    return CDB_OK;
}
