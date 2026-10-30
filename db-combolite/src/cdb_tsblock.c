#include "cdb_tsblock.h"

#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_endian.h"
#include "cdb_varint.h"

/* Column kinds. */
enum { K_INT, K_F64, K_F32, K_BOOL, K_STR, K_PLAIN, K_BAD };

static int kind_of(uint8_t type)
{
    switch (type) {
    case CDB_TYPE_INT: return K_INT;
    case CDB_TYPE_F64: return K_F64;
    case CDB_TYPE_F32: return K_F32;
    case CDB_TYPE_BOOL: return K_BOOL;
    case CDB_TYPE_STRING: return K_STR;
    case CDB_TYPE_NULL:
    case CDB_TYPE_TIMESTAMP:
    case CDB_TYPE_REF:
    case CDB_TYPE_BINARY:
    case CDB_TYPE_DECIMAL:
    case CDB_TYPE_ARRAY:
    case CDB_TYPE_OBJECT:
    case CDB_TYPE_VECTOR: return K_PLAIN;
    default: return K_BAD;
    }
}

/* Two's-complement bit pattern to int64, without the implementation-defined
 * unsigned-to-signed conversion. */
static int64_t u2i(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(~u) - 1;
}

static size_t nbytes(uint64_t bits)
{
    return (size_t)((bits + 7u) / 8u);
}

static int utf8_valid(const uint8_t *s, size_t len)
{
    return cdb_value_utf8_valid(s, len); /* the value encoding's own check (14.6) */
}

/* ---- output cursor ------------------------------------------------------- */

typedef struct obuf {
    uint8_t *p;
    size_t cap, len;
    cdb_byte_order o;
} obuf;

static cdb_status o_need(const obuf *b, size_t n)
{
    return n <= b->cap - b->len ? CDB_OK : CDB_E_OVERFLOW;
}

static cdb_status o_bytes(obuf *b, const void *src, size_t n)
{
    if (o_need(b, n) != CDB_OK)
        return CDB_E_OVERFLOW;
    if (n)
        memcpy(b->p + b->len, src, n);
    b->len += n;
    return CDB_OK;
}

static cdb_status o_u8(obuf *b, uint8_t v)
{
    return o_bytes(b, &v, 1);
}

static cdb_status o_u32(obuf *b, uint32_t v)
{
    uint8_t t[4];
    cdb_store_u32(t, v, b->o);
    return o_bytes(b, t, 4);
}

static cdb_status o_u64(obuf *b, uint64_t v)
{
    uint8_t t[8];
    cdb_store_u64(t, v, b->o);
    return o_bytes(b, t, 8);
}

/* A bit column: a u32 bit count, then the stream. begin reserves the count
 * and continues a stream of `bits` bits already at the cursor; end writes
 * the count and moves the cursor past the stream. */
static cdb_status o_bits_begin(obuf *b, size_t *cntpos, cdb_bitw *w, size_t bits)
{
    if (o_need(b, 4u + nbytes(bits)) != CDB_OK)
        return CDB_E_OVERFLOW;
    *cntpos = b->len;
    return cdb_bitw_resume(w, b->p + b->len + 4u, b->cap - b->len - 4u, bits);
}

static cdb_status o_bits_end(obuf *b, size_t cntpos, const cdb_bitw *w)
{
    if (w->bits > UINT32_MAX)
        return CDB_E_OVERFLOW;
    cdb_store_u32(b->p + cntpos, (uint32_t)w->bits, b->o);
    b->len = cntpos + 4u + cdb_bitw_bytes(w);
    return CDB_OK;
}

static cdb_status enc_err(cdb_status st)
{
    return st == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : st;
}

/* ---- input cursor -------------------------------------------------------- */

typedef struct rd {
    const uint8_t *p;
    size_t len, pos;
    cdb_byte_order o;
} rd;

static cdb_status r_take(rd *r, size_t n, const uint8_t **q)
{
    if (n > r->len - r->pos)
        return CDB_E_MALFORMED;
    *q = r->p + r->pos;
    r->pos += n;
    return CDB_OK;
}

static cdb_status r_u8(rd *r, uint8_t *v)
{
    const uint8_t *q;
    cdb_status st = r_take(r, 1, &q);
    if (st == CDB_OK)
        *v = q[0];
    return st;
}

static cdb_status r_u32(rd *r, uint32_t *v)
{
    const uint8_t *q;
    cdb_status st = r_take(r, 4, &q);
    if (st == CDB_OK)
        *v = cdb_load_u32(q, r->o);
    return st;
}

static cdb_status r_u64(rd *r, uint64_t *v)
{
    const uint8_t *q;
    cdb_status st = r_take(r, 8, &q);
    if (st == CDB_OK)
        *v = cdb_load_u64(q, r->o);
    return st;
}

/* Reads a bit column's count and stream into a bit reader. */
static cdb_status r_bits(rd *r, cdb_bitr *br, uint32_t *bits)
{
    const uint8_t *q;
    cdb_status st = r_u32(r, bits);

    if (st == CDB_OK)
        st = r_take(r, nbytes(*bits), &q);
    if (st == CDB_OK)
        cdb_bitr_init(br, q, nbytes(*bits));
    return st;
}

/* Checks a bit column was consumed exactly, with zero padding. */
static cdb_status r_bits_end(const cdb_bitr *br, uint32_t bits)
{
    if (br->pos != bits)
        return CDB_E_MALFORMED;
    return cdb_bitr_check_end(br);
}

static cdb_status dec_err(cdb_status st)
{
    return (st == CDB_E_TRUNCATED || st == CDB_E_BUFFER_TOO_SMALL) ? CDB_E_MALFORMED : st;
}

/* ---- string dictionary --------------------------------------------------- */

/* Index of s among the entries of dict[0..dl), or the entry count when it is
 * absent (*count gets the entry count either way). */
static cdb_status dict_find(const uint8_t *d, size_t dl, const uint8_t *s, size_t sl, uint32_t *idx,
                            uint32_t *count)
{
    size_t pos = 0, n;
    uint64_t el;
    uint32_t i = 0, found = UINT32_MAX;

    while (pos < dl) {
        if (cdb_varint_decode(d + pos, dl - pos, &el, &n) != CDB_OK || el > dl - pos - n)
            return CDB_E_MALFORMED;
        pos += n;
        if (found == UINT32_MAX && el == sl && (sl == 0 || memcmp(d + pos, s, sl) == 0))
            found = i;
        pos += (size_t)el;
        i++;
    }
    *count = i;
    *idx = found == UINT32_MAX ? i : found;
    return CDB_OK;
}

/* Entry i of a dictionary already checked to be well formed. */
static void dict_entry(const uint8_t *d, size_t dl, uint32_t i, const uint8_t **s, size_t *sl)
{
    size_t pos = 0, n = 0;
    uint64_t el = 0;

    for (;;) {
        (void)cdb_varint_decode(d + pos, dl - pos, &el, &n);
        pos += n;
        if (i == 0)
            break;
        pos += (size_t)el;
        i--;
    }
    *s = d + pos;
    *sl = (size_t)el;
}

/* ---- argument checks ----------------------------------------------------- */

static cdb_status check_layout(const cdb_ts_layout *L)
{
    unsigned f;

    if (L->nfields > CDB_TS_MAX_FIELDS || L->precision > CDB_TS_NANOS ||
        (L->flags & ~(CDB_TS_BLOCK_COV | CDB_TS_BLOCK_LATE)) != 0 || L->window_start >= L->window_end)
        return CDB_E_INVALID_ARG;
    for (f = 0; f < L->nfields; f++) {
        if (kind_of(L->fields[f].type) == K_BAD)
            return CDB_E_INVALID_ARG;
        if (f > 0 && L->fields[f].id <= L->fields[f - 1].id)
            return CDB_E_INVALID_ARG;
    }
    return CDB_OK;
}

/* A plain value: exactly one canonical value of the field's type. */
static cdb_status check_plain(const uint8_t *s, size_t len, uint8_t type, cdb_byte_order o)
{
    cdb_value v;
    cdb_status st = cdb_value_validate(s, len, o, CDB_DECIMAL_UNBOUNDED, 0);

    if (st != CDB_OK)
        return st;
    st = cdb_value_read(s, len, o, &v);
    if (st != CDB_OK)
        return st;
    if (v.encoded_len != len || (unsigned)v.type != (unsigned)type)
        return CDB_E_MALFORMED;
    return CDB_OK;
}

static cdb_status check_val(uint8_t type, const cdb_ts_val *v, cdb_byte_order o)
{
    if (!v->present)
        return CDB_OK;
    switch (kind_of(type)) {
    case K_F32:
        return (v->bits >> 32) ? CDB_E_INVALID_ARG : CDB_OK;
    case K_BOOL:
        return v->bits > 1u ? CDB_E_INVALID_ARG : CDB_OK;
    case K_STR:
        if (v->len > UINT32_MAX || (v->len && v->bytes == NULL) || !utf8_valid(v->bytes, v->len))
            return CDB_E_INVALID_ARG;
        return CDB_OK;
    case K_PLAIN:
        if (v->bytes == NULL || v->len == 0 || check_plain(v->bytes, v->len, type, o) != CDB_OK)
            return CDB_E_INVALID_ARG;
        return CDB_OK;
    default:
        return CDB_OK;
    }
}

static void state_init(cdb_tsblock_state *s, int64_t first)
{
    unsigned f;

    memset(s, 0, sizeof *s);
    cdb_tsb_time_init(&s->t, first);
    for (f = 0; f < CDB_TS_MAX_FIELDS; f++) {
        cdb_tsb_int_init(&s->ints[f]);
        cdb_tsb_float_init(&s->floats[f]);
    }
}

/* Encodes one present value of a bit-coded column (not strings). */
static cdb_status enc_val(cdb_bitw *w, int kind, cdb_tsblock_state *s, unsigned f, const cdb_ts_val *v)
{
    switch (kind) {
    case K_INT: return cdb_tsb_int_encode(w, &s->ints[f], u2i(v->bits));
    case K_F64: return cdb_tsb_fbits_encode(w, &s->floats[f], v->bits, 64);
    case K_F32: return cdb_tsb_fbits_encode(w, &s->floats[f], v->bits, 32);
    default: return cdb_tsb_bool_encode(w, v->bits != 0);
    }
}

/* ---- build --------------------------------------------------------------- */

static cdb_status build_string(obuf *b, const cdb_ts_point *pts, size_t n, unsigned f, cdb_tsblock_state *s)
{
    size_t cntpos = b->len, dstart, i, cp;
    uint32_t idx, dn, used = 0;
    uint8_t vb[CDB_VARINT_MAX_LEN];
    cdb_bitw w;
    cdb_status st;

    /* Dictionary, in order of first use. */
    if (o_need(b, 8) != CDB_OK)
        return CDB_E_OVERFLOW;
    b->len += 8;
    dstart = b->len;
    for (i = 0; i < n; i++) {
        const cdb_ts_val *v = &pts[i].v[f];
        if (!v->present)
            continue;
        st = dict_find(b->p + dstart, b->len - dstart, v->bytes, v->len, &idx, &dn);
        if (st != CDB_OK)
            return st;
        if (idx == dn) {
            st = o_bytes(b, vb, cdb_varint_encode((uint64_t)v->len, vb));
            if (st == CDB_OK)
                st = o_bytes(b, v->bytes, v->len);
            if (st != CDB_OK)
                return st;
        }
    }
    if (b->len - dstart > UINT32_MAX)
        return CDB_E_OVERFLOW;
    st = dict_find(b->p + dstart, b->len - dstart, NULL, 0, &idx, &dn);
    if (st != CDB_OK)
        return st;
    cdb_store_u32(b->p + cntpos, dn, b->o);
    cdb_store_u32(b->p + cntpos + 4u, (uint32_t)(b->len - dstart), b->o);

    /* Indexes: n = the entries before this value, index n = new entry. */
    st = o_bits_begin(b, &cp, &w, 0);
    if (st != CDB_OK)
        return enc_err(st);
    for (i = 0; i < n; i++) {
        const cdb_ts_val *v = &pts[i].v[f];
        if (!v->present)
            continue;
        st = dict_find(b->p + dstart, (size_t)cdb_load_u32(b->p + cntpos + 4u, b->o), v->bytes, v->len, &idx,
                       &dn);
        if (st != CDB_OK)
            return st;
        if (idx > used)
            return CDB_E_MALFORMED; /* cannot happen: entries are in first-use order */
        st = cdb_tsb_dict_encode(&w, used, idx);
        if (st != CDB_OK)
            return enc_err(st);
        if (idx == used)
            used++;
    }
    s->dict_n[f] = used;
    return o_bits_end(b, cp, &w);
}

cdb_status cdb_tsblock_build(const cdb_ts_layout *L, const cdb_ts_point *pts, size_t n, cdb_byte_order o,
                             uint8_t *out, size_t cap, size_t *len, cdb_tsblock_state *st)
{
    obuf b;
    cdb_tsblock_state s;
    cdb_bitw w;
    size_t i, cp;
    unsigned f;
    cdb_status rc;
    uint8_t hdr[CDB_TS_BLOCK_HEADER];

    if (L == NULL || pts == NULL || out == NULL || len == NULL || n == 0 || n > CDB_TS_BLOCK_MAX_POINTS ||
        (o != CDB_ORDER_LITTLE && o != CDB_ORDER_BIG))
        return CDB_E_INVALID_ARG;
    rc = check_layout(L);
    if (rc != CDB_OK)
        return rc;
    for (i = 0; i < n; i++) {
        if (pts[i].ts < L->window_start || pts[i].ts >= L->window_end || (i > 0 && pts[i].ts <= pts[i - 1].ts))
            return CDB_E_INVALID_ARG;
        for (f = 0; f < L->nfields; f++)
            if (check_val(L->fields[f].type, &pts[i].v[f], o) != CDB_OK)
                return CDB_E_INVALID_ARG;
    }

    b.p = out;
    b.cap = cap;
    b.len = 0;
    b.o = o;
    state_init(&s, pts[0].ts);
    s.count = (uint32_t)n;

    /* Header. */
    cdb_store_u64(hdr, L->series, o);
    cdb_store_u64(hdr + 8, (uint64_t)L->window_start, o);
    cdb_store_u64(hdr + 16, (uint64_t)L->window_end, o);
    cdb_store_u32(hdr + 24, (uint32_t)n, o);
    cdb_store_u16(hdr + 28, L->nfields, o);
    hdr[30] = L->precision;
    hdr[31] = L->flags;
    rc = o_bytes(&b, hdr, sizeof hdr);
    if (rc != CDB_OK)
        return rc;

    /* Timestamps. */
    rc = o_u64(&b, (uint64_t)pts[0].ts);
    if (rc == CDB_OK)
        rc = o_bits_begin(&b, &cp, &w, 0);
    for (i = 1; i < n && rc == CDB_OK; i++)
        rc = cdb_tsb_time_encode(&w, &s.t, pts[i].ts);
    if (rc == CDB_OK)
        rc = o_bits_end(&b, cp, &w);
    if (rc != CDB_OK)
        return enc_err(rc);

    for (f = 0; f < L->nfields; f++) {
        uint8_t type = L->fields[f].type;
        int kind = kind_of(type);
        size_t bm = nbytes(n);

        rc = o_u64(&b, L->fields[f].id);
        if (rc == CDB_OK)
            rc = o_u8(&b, type);
        if (rc == CDB_OK)
            rc = o_need(&b, bm);
        if (rc != CDB_OK)
            return rc;
        memset(b.p + b.len, 0, bm);
        for (i = 0; i < n; i++)
            if (pts[i].v[f].present)
                b.p[b.len + (i >> 3)] = (uint8_t)(b.p[b.len + (i >> 3)] | (0x80u >> (i & 7u)));
        b.len += bm;

        if (kind == K_STR) {
            rc = build_string(&b, pts, n, f, &s);
        } else if (kind == K_PLAIN) {
            size_t lp = b.len;
            rc = o_u32(&b, 0);
            for (i = 0; i < n && rc == CDB_OK; i++)
                if (pts[i].v[f].present)
                    rc = o_bytes(&b, pts[i].v[f].bytes, pts[i].v[f].len);
            if (rc == CDB_OK && b.len - lp - 4u > UINT32_MAX)
                rc = CDB_E_OVERFLOW;
            if (rc == CDB_OK)
                cdb_store_u32(b.p + lp, (uint32_t)(b.len - lp - 4u), o);
        } else {
            rc = o_bits_begin(&b, &cp, &w, 0);
            for (i = 0; i < n && rc == CDB_OK; i++)
                if (pts[i].v[f].present)
                    rc = enc_val(&w, kind, &s, f, &pts[i].v[f]);
            if (rc == CDB_OK)
                rc = o_bits_end(&b, cp, &w);
        }
        if (rc != CDB_OK)
            return enc_err(rc);
    }

    *len = b.len;
    if (st != NULL)
        *st = s;
    return CDB_OK;
}

/* ---- decode -------------------------------------------------------------- */

/* Walks and fully validates a block. out (may be NULL) gets the points. */
static cdb_status walk(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_ts_layout *L, cdb_ts_point *out,
                       size_t cap, uint32_t *count_out, int64_t *first_out, int64_t *last_out,
                       cdb_tsblock_state *st_out)
{
    rd r;
    cdb_ts_layout lay;
    cdb_tsblock_state s;
    cdb_bitr br;
    const uint8_t *q;
    uint32_t count, bits, i;
    uint64_t u;
    int64_t t, prev;
    unsigned f;
    cdb_status rc;

    if (pl == NULL || (o != CDB_ORDER_LITTLE && o != CDB_ORDER_BIG))
        return CDB_E_INVALID_ARG;
    r.p = pl;
    r.len = len;
    r.pos = 0;
    r.o = o;
    memset(&lay, 0, sizeof lay);

    /* Header. */
    if (r_take(&r, CDB_TS_BLOCK_HEADER, &q) != CDB_OK)
        return CDB_E_MALFORMED;
    lay.series = cdb_load_u64(q, o);
    lay.window_start = u2i(cdb_load_u64(q + 8, o));
    lay.window_end = u2i(cdb_load_u64(q + 16, o));
    count = cdb_load_u32(q + 24, o);
    lay.nfields = cdb_load_u16(q + 28, o);
    lay.precision = q[30];
    lay.flags = q[31];
    if (count == 0 || count > CDB_TS_BLOCK_MAX_POINTS || lay.nfields > CDB_TS_MAX_FIELDS ||
        lay.precision > CDB_TS_NANOS || lay.window_start >= lay.window_end)
        return CDB_E_MALFORMED;
    if ((lay.flags & ~(CDB_TS_BLOCK_COV | CDB_TS_BLOCK_LATE)) != 0)
        return CDB_E_MALFORMED;
    if (out != NULL) {
        if (cap < count)
            return CDB_E_BUFFER_TOO_SMALL;
        memset(out, 0, (size_t)count * sizeof *out);
    }

    /* Timestamps. */
    rc = r_u64(&r, &u);
    if (rc != CDB_OK)
        return rc;
    t = u2i(u);
    if (t < lay.window_start || t >= lay.window_end)
        return CDB_E_MALFORMED;
    state_init(&s, t);
    s.count = count;
    if (out != NULL)
        out[0].ts = t;
    if (first_out != NULL)
        *first_out = t;
    rc = r_bits(&r, &br, &bits);
    if (rc != CDB_OK)
        return rc;
    for (i = 1; i < count; i++) {
        prev = t;
        rc = cdb_tsb_time_decode(&br, &s.t, &t);
        if (rc != CDB_OK)
            return dec_err(rc);
        if (t <= prev || t >= lay.window_end)
            return CDB_E_MALFORMED;
        if (out != NULL)
            out[i].ts = t;
    }
    rc = r_bits_end(&br, bits);
    if (rc != CDB_OK)
        return rc;

    for (f = 0; f < lay.nfields; f++) {
        const uint8_t *bm;
        uint8_t type;
        int kind;

        rc = r_u64(&r, &lay.fields[f].id);
        if (rc == CDB_OK)
            rc = r_u8(&r, &type);
        if (rc != CDB_OK)
            return rc;
        lay.fields[f].type = type;
        kind = kind_of(type);
        if (kind == K_BAD || (f > 0 && lay.fields[f].id <= lay.fields[f - 1].id))
            return CDB_E_MALFORMED;
        rc = r_take(&r, nbytes(count), &bm);
        if (rc != CDB_OK)
            return rc;
        if ((count & 7u) && (bm[count >> 3] & (0xFFu >> (count & 7u))) != 0)
            return CDB_E_NOT_CANONICAL;

#define PRESENT(k) (((unsigned)bm[(k) >> 3] >> (7u - ((k) & 7u))) & 1u)

        if (kind == K_STR) {
            uint32_t dn, dl, e, used = 0;
            const uint8_t *d;
            size_t pos = 0, n;

            rc = r_u32(&r, &dn);
            if (rc == CDB_OK)
                rc = r_u32(&r, &dl);
            if (rc == CDB_OK)
                rc = r_take(&r, dl, &d);
            if (rc != CDB_OK)
                return rc;
            /* Entries: well formed, valid UTF-8 and distinct. */
            for (e = 0; pos < dl; e++) {
                uint64_t el;
                uint32_t idx, k;
                if (e >= dn)
                    return CDB_E_MALFORMED;
                rc = cdb_varint_decode(d + pos, dl - pos, &el, &n);
                if (rc != CDB_OK)
                    return dec_err(rc);
                if (el > dl - pos - n)
                    return CDB_E_MALFORMED;
                if (!utf8_valid(d + pos + n, (size_t)el))
                    return CDB_E_MALFORMED;
                rc = dict_find(d, pos, d + pos + n, (size_t)el, &idx, &k);
                if (rc != CDB_OK)
                    return rc;
                if (idx != k)
                    return CDB_E_NOT_CANONICAL; /* duplicate entry */
                pos += n + (size_t)el;
            }
            if (e != dn)
                return CDB_E_MALFORMED;
            rc = r_bits(&r, &br, &bits);
            if (rc != CDB_OK)
                return rc;
            for (i = 0; i < count; i++) {
                uint64_t idx;
                if (!PRESENT(i))
                    continue;
                rc = cdb_tsb_dict_decode(&br, used, &idx);
                if (rc != CDB_OK)
                    return dec_err(rc);
                if (idx == used) {
                    if (used == dn)
                        return CDB_E_MALFORMED; /* new entry past the dictionary */
                    used++;
                }
                if (out != NULL) {
                    out[i].v[f].present = 1;
                    dict_entry(d, dl, (uint32_t)idx, &out[i].v[f].bytes, &out[i].v[f].len);
                }
            }
            rc = r_bits_end(&br, bits);
            if (rc != CDB_OK)
                return rc;
            if (used != dn)
                return CDB_E_NOT_CANONICAL; /* an entry is never used */
            s.dict_n[f] = dn;
        } else if (kind == K_PLAIN) {
            uint32_t pl_len;
            const uint8_t *vals;
            size_t pos = 0;

            rc = r_u32(&r, &pl_len);
            if (rc == CDB_OK)
                rc = r_take(&r, pl_len, &vals);
            if (rc != CDB_OK)
                return rc;
            for (i = 0; i < count; i++) {
                cdb_value v;
                if (!PRESENT(i))
                    continue;
                rc = cdb_value_read(vals + pos, pl_len - pos, o, &v);
                if (rc != CDB_OK)
                    return dec_err(rc);
                rc = check_plain(vals + pos, v.encoded_len, type, o);
                if (rc != CDB_OK)
                    return dec_err(rc);
                if (out != NULL) {
                    out[i].v[f].present = 1;
                    out[i].v[f].bytes = vals + pos;
                    out[i].v[f].len = v.encoded_len;
                }
                pos += v.encoded_len;
            }
            if (pos != pl_len)
                return CDB_E_MALFORMED;
        } else {
            rc = r_bits(&r, &br, &bits);
            if (rc != CDB_OK)
                return rc;
            for (i = 0; i < count; i++) {
                uint64_t vb = 0;
                int bv;
                int64_t iv;
                if (!PRESENT(i))
                    continue;
                switch (kind) {
                case K_INT:
                    rc = cdb_tsb_int_decode(&br, &s.ints[f], &iv);
                    vb = (uint64_t)iv;
                    break;
                case K_F64: rc = cdb_tsb_fbits_decode(&br, &s.floats[f], 64, &vb); break;
                case K_F32: rc = cdb_tsb_fbits_decode(&br, &s.floats[f], 32, &vb); break;
                default:
                    rc = cdb_tsb_bool_decode(&br, &bv);
                    vb = bv ? 1u : 0u;
                    break;
                }
                if (rc != CDB_OK)
                    return dec_err(rc);
                if (out != NULL) {
                    out[i].v[f].present = 1;
                    out[i].v[f].bits = vb;
                }
            }
            rc = r_bits_end(&br, bits);
            if (rc != CDB_OK)
                return rc;
        }
#undef PRESENT
    }
    if (r.pos != len)
        return CDB_E_MALFORMED; /* trailing bytes */

    if (L != NULL)
        *L = lay;
    if (count_out != NULL)
        *count_out = count;
    if (last_out != NULL)
        *last_out = t;
    if (st_out != NULL)
        *st_out = s;
    return CDB_OK;
}

cdb_status cdb_tsblock_parse(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_ts_layout *L, uint32_t *count,
                             int64_t *first_ts, int64_t *last_ts)
{
    int64_t first;
    cdb_status rc;

    if (L == NULL)
        return CDB_E_INVALID_ARG;
    rc = walk(pl, len, o, L, NULL, 0, count, &first, last_ts, NULL);
    if (rc == CDB_OK && first_ts != NULL)
        *first_ts = first;
    return rc;
}

cdb_status cdb_tsblock_decode(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_ts_layout *L,
                              cdb_ts_point *out, size_t cap, size_t *n, cdb_tsblock_state *st)
{
    uint32_t count;
    cdb_status rc;

    if (L == NULL || out == NULL || n == NULL)
        return CDB_E_INVALID_ARG;
    rc = walk(pl, len, o, L, out, cap, &count, NULL, NULL, st);
    if (rc == CDB_OK)
        *n = count;
    return rc;
}

/* ---- append -------------------------------------------------------------- */

/* Copies a bit column from r to b and opens it for appending. */
static cdb_status app_bits(rd *r, obuf *b, size_t *cp, cdb_bitw *w)
{
    uint32_t bits;
    const uint8_t *q;
    cdb_status rc = r_u32(r, &bits);

    if (rc == CDB_OK)
        rc = r_take(r, nbytes(bits), &q);
    if (rc != CDB_OK)
        return rc;
    rc = o_bits_begin(b, cp, w, bits);
    if (rc != CDB_OK)
        return enc_err(rc);
    memcpy(b->p + *cp + 4u, q, nbytes(bits));
    return CDB_OK;
}

cdb_status cdb_tsblock_append(const uint8_t *pl, size_t len, cdb_byte_order o, cdb_tsblock_state *st,
                              const cdb_ts_point *p, uint8_t *out, size_t cap, size_t *outlen)
{
    rd r;
    obuf b;
    cdb_tsblock_state s;
    cdb_bitw w;
    const uint8_t *q;
    uint32_t count;
    uint16_t nfields;
    int64_t wstart, wend;
    size_t cp;
    unsigned f;
    cdb_status rc;

    if (pl == NULL || st == NULL || p == NULL || out == NULL || outlen == NULL || out == pl ||
        (o != CDB_ORDER_LITTLE && o != CDB_ORDER_BIG))
        return CDB_E_INVALID_ARG;
    if (len < CDB_TS_BLOCK_HEADER)
        return CDB_E_MALFORMED;
    count = cdb_load_u32(pl + 24, o);
    nfields = cdb_load_u16(pl + 28, o);
    wstart = u2i(cdb_load_u64(pl + 8, o));
    wend = u2i(cdb_load_u64(pl + 16, o));
    if (count != st->count || count == 0 || nfields > CDB_TS_MAX_FIELDS)
        return CDB_E_INVALID_ARG;
    if (count >= CDB_TS_BLOCK_MAX_POINTS)
        return CDB_E_OVERFLOW;
    if (p->ts <= st->t.prev_t || p->ts < wstart || p->ts >= wend)
        return CDB_E_INVALID_ARG;

    r.p = pl;
    r.len = len;
    r.pos = 0;
    r.o = o;
    b.p = out;
    b.cap = cap;
    b.len = 0;
    b.o = o;
    s = *st;

    /* Header, with the new count. */
    (void)r_take(&r, CDB_TS_BLOCK_HEADER, &q);
    rc = o_bytes(&b, q, CDB_TS_BLOCK_HEADER);
    if (rc != CDB_OK)
        return rc;
    cdb_store_u32(out + 24, count + 1u, o);

    /* Timestamps. */
    rc = r_take(&r, 8, &q);
    if (rc == CDB_OK)
        rc = o_bytes(&b, q, 8);
    if (rc == CDB_OK)
        rc = app_bits(&r, &b, &cp, &w);
    if (rc == CDB_OK)
        rc = enc_err(cdb_tsb_time_encode(&w, &s.t, p->ts));
    if (rc == CDB_OK)
        rc = o_bits_end(&b, cp, &w);
    if (rc != CDB_OK)
        return rc;

    for (f = 0; f < nfields; f++) {
        const cdb_ts_val *v = &p->v[f];
        size_t ob = nbytes(count), nb = nbytes((uint64_t)count + 1u);
        uint8_t type;
        int kind;

        rc = r_take(&r, 9, &q);
        if (rc != CDB_OK)
            return rc;
        type = q[8];
        kind = kind_of(type);
        if (kind == K_BAD)
            return CDB_E_MALFORMED;
        if (check_val(type, v, o) != CDB_OK)
            return CDB_E_INVALID_ARG;
        rc = o_bytes(&b, q, 9);
        if (rc != CDB_OK)
            return rc;

        /* Presence bitmap, one point longer. */
        rc = r_take(&r, ob, &q);
        if (rc == CDB_OK)
            rc = o_need(&b, nb);
        if (rc != CDB_OK)
            return rc;
        memcpy(b.p + b.len, q, ob);
        if (nb > ob)
            b.p[b.len + ob] = 0;
        if (v->present)
            b.p[b.len + (count >> 3)] = (uint8_t)(b.p[b.len + (count >> 3)] | (0x80u >> (count & 7u)));
        b.len += nb;

        if (kind == K_STR) {
            uint32_t dn, dl, idx, k;
            const uint8_t *d;
            uint8_t vb[CDB_VARINT_MAX_LEN];
            size_t vl = 0;
            int add = 0;

            rc = r_u32(&r, &dn);
            if (rc == CDB_OK)
                rc = r_u32(&r, &dl);
            if (rc == CDB_OK)
                rc = r_take(&r, dl, &d);
            if (rc != CDB_OK)
                return rc;
            if (dn != st->dict_n[f])
                return CDB_E_INVALID_ARG;
            idx = dn;
            if (v->present) {
                rc = dict_find(d, dl, v->bytes, v->len, &idx, &k);
                if (rc != CDB_OK)
                    return rc;
                if (k != dn)
                    return CDB_E_MALFORMED;
                if (idx == dn) {
                    add = 1;
                    vl = cdb_varint_encode((uint64_t)v->len, vb);
                    if ((uint64_t)dl + vl + v->len > UINT32_MAX)
                        return CDB_E_OVERFLOW;
                }
            }
            rc = o_u32(&b, dn + (add ? 1u : 0u));
            if (rc == CDB_OK)
                rc = o_u32(&b, (uint32_t)(dl + (add ? vl + v->len : 0u)));
            if (rc == CDB_OK)
                rc = o_bytes(&b, d, dl);
            if (rc == CDB_OK && add)
                rc = o_bytes(&b, vb, vl);
            if (rc == CDB_OK && add)
                rc = o_bytes(&b, v->bytes, v->len);
            if (rc == CDB_OK)
                rc = app_bits(&r, &b, &cp, &w);
            if (rc == CDB_OK && v->present)
                rc = enc_err(cdb_tsb_dict_encode(&w, dn, idx));
            if (rc == CDB_OK)
                rc = o_bits_end(&b, cp, &w);
            if (rc != CDB_OK)
                return rc;
            if (add)
                s.dict_n[f] = dn + 1u;
        } else if (kind == K_PLAIN) {
            uint32_t pl_len;

            rc = r_u32(&r, &pl_len);
            if (rc == CDB_OK)
                rc = r_take(&r, pl_len, &q);
            if (rc != CDB_OK)
                return rc;
            if (v->present && (uint64_t)pl_len + v->len > UINT32_MAX)
                return CDB_E_OVERFLOW;
            rc = o_u32(&b, (uint32_t)(pl_len + (v->present ? v->len : 0u)));
            if (rc == CDB_OK)
                rc = o_bytes(&b, q, pl_len);
            if (rc == CDB_OK && v->present)
                rc = o_bytes(&b, v->bytes, v->len);
            if (rc != CDB_OK)
                return rc;
        } else {
            rc = app_bits(&r, &b, &cp, &w);
            if (rc == CDB_OK && v->present)
                rc = enc_err(enc_val(&w, kind, &s, f, v));
            if (rc == CDB_OK)
                rc = o_bits_end(&b, cp, &w);
            if (rc != CDB_OK)
                return rc;
        }
    }
    if (r.pos != len)
        return CDB_E_MALFORMED;

    s.count = count + 1u;
    *st = s;
    *outlen = b.len;
    return CDB_OK;
}
