/* Column chunk encodings (spec 9.12, 14.10.2). The byte forms, the choice
 * rule and how deeply each type is checked are described at the top of
 * cdb_colenc.h.
 *
 * Encoding never builds a candidate and throws it away: each encoding is
 * first "run" into a sink with no buffer, which only counts the bytes it
 * would write, and the smallest is then run again into the real buffer.
 * The sink counts past the end of the buffer, so a too-small buffer is
 * reported together with the size that would have fit. */
#include "cdb_colenc.h"

#include <string.h>

#include "cdb_endian.h"
#include "cdb_varint.h"

/* ---- helpers -------------------------------------------------------------- */

#define IS_BOOL(tag) ((tag) == CDB_TAG_FALSE)

static unsigned bitlen(uint64_t v)
{
    unsigned n = 0;

    while (v != 0) {
        n++;
        v >>= 1;
    }
    return n;
}

/* Bytes of n values of w bits each, or 0 with *ok = 0 on overflow. */
static size_t packed_bytes(size_t n, unsigned w, int *ok)
{
    *ok = 1;
    if (n > SIZE_MAX / 64u - 1u) {
        *ok = 0;
        return 0;
    }
    return (n * (size_t)w + 7u) / 8u;
}

/* A bool column is named by either bool tag; internally it is the false tag. */
static int normalize_tag(uint8_t *tag)
{
    if (*tag == CDB_TAG_TRUE)
        *tag = CDB_TAG_FALSE;
    return !(*tag == CDB_TAG_NULL || (*tag > CDB_TAG_REF && *tag < CDB_TAG_FIRST_VARIABLE));
}

int cdb_colenc_can_bitpack(uint8_t tag)
{
    return tag == CDB_TAG_INT || tag == CDB_TAG_TIMESTAMP || tag == CDB_TAG_TRUE ||
           tag == CDB_TAG_FALSE;
}

/* Smallest power of two that is >= v (v > 0). */
static size_t next_pow2(size_t v)
{
    size_t p = 1;

    while (p < v)
        p <<= 1;
    return p;
}

/* Words for the dictionary's open-addressing hash table: a power of two at
 * least twice n, so the table stays at most half full. */
static size_t dict_table_words(size_t n)
{
    size_t want;

    if (n == 0)
        return 0;
    want = n > SIZE_MAX / 2u ? n : n * 2u; /* avoid overflow at absurd n */
    if (want < 8u)
        want = 8u;
    return next_pow2(want);
}

size_t cdb_colenc_scratch_words(size_t n)
{
    return 3u * n + 1u + dict_table_words(n);
}

size_t cdb_colenc_max_size(size_t values_len)
{
    return values_len;
}

/* ---- deep validation of array and object items (reader side) -------------
 *
 * See the note at the top of cdb_colenc.h: array and object elements are
 * themselves full tagged values physically present in the chunk, so once
 * the container's own header is parsed (spec 14.6.3, with only
 * cdb_varint_decode and cdb_endian.h) each element is checked with the
 * public cdb_value_validate, recursively. Vector and decimal have no such
 * public hook and stay shape-only here; see the header comment. */

static uint64_t load_w(const uint8_t *p, unsigned w, cdb_byte_order order)
{
    switch (w) {
    case 1:
        return p[0];
    case 2:
        return cdb_load_u16(p, order);
    case 4:
        return cdb_load_u32(p, order);
    default:
        return cdb_load_u64(p, order);
    }
}

/* The smallest of 1, 2, 4, 8 bytes that holds v (mirrors cdb_value.c's
 * directory width rule). */
static unsigned width_for(uint64_t v)
{
    if (v <= 0xFFu)
        return 1;
    if (v <= 0xFFFFu)
        return 2;
    if (v <= 0xFFFFFFFFu)
        return 4;
    return 8;
}

/* Bits per element, from the public element semantics in cdb_value.h
 * (float/f16/bf16/i8 elements, or packed bits for BINARY). 0 for an
 * unrecognised element type. */
static size_t vec_elem_bits(cdb_vector_elem elem)
{
    switch (elem) {
    case CDB_VEC_F32:
        return 32;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        return 16;
    case CDB_VEC_I8:
        return 8;
    case CDB_VEC_BINARY:
        return 1;
    default:
        return 0;
    }
}

static cdb_status validate_array_payload(cdb_byte_order order, cdb_decimal_mode dm,
                                         const uint8_t *payload, size_t payload_len)
{
    const uint8_t *p = payload;
    size_t left = payload_len, nn;
    uint64_t count, n = 0;
    cdb_status st;

    if ((st = cdb_varint_decode(p, left, &count, &nn)) != CDB_OK)
        return st == CDB_E_TRUNCATED ? CDB_E_MALFORMED : st;
    p += nn;
    left -= nn;
    while (left > 0) {
        cdb_value v;

        if ((st = cdb_value_read(p, left, order, &v)) != CDB_OK)
            return st;
        if ((st = cdb_value_validate(p, v.encoded_len, order, dm, 0)) != CDB_OK)
            return st;
        p += v.encoded_len;
        left -= v.encoded_len;
        n++;
    }
    return n == count ? CDB_OK : CDB_E_MALFORMED;
}

/* Mirrors cdb_value.c's validate_object: field IDs strictly increasing,
 * values contiguous from offset 0 with no gaps, and the smallest directory
 * widths that fit; each value is checked with cdb_value_validate. */
static cdb_status validate_object_payload(cdb_byte_order order, cdb_decimal_mode dm,
                                          const uint8_t *payload, size_t payload_len)
{
    const uint8_t *p = payload, *dir, *values;
    size_t left = payload_len, nn, dir_bytes, values_len, expected = 0;
    uint64_t count, prev_id = 0, last_off = 0, i;
    unsigned id_w, off_w, entry;
    uint8_t widths;
    cdb_status st;

    if ((st = cdb_varint_decode(p, left, &count, &nn)) != CDB_OK)
        return st == CDB_E_TRUNCATED ? CDB_E_MALFORMED : st;
    p += nn;
    left -= nn;
    if (left == 0)
        return CDB_E_MALFORMED;
    widths = *p++;
    left--;
    if (widths & 0xCCu)
        return CDB_E_MALFORMED;
    id_w = 1u << (widths & 0x03u);
    off_w = 1u << ((widths >> 4) & 0x03u);
    entry = id_w + off_w;
    if (count > (uint64_t)left / entry)
        return CDB_E_MALFORMED;
    dir_bytes = (size_t)count * entry;
    dir = p;
    values = p + dir_bytes;
    values_len = left - dir_bytes;

    for (i = 0; i < count; i++) {
        uint64_t id = load_w(dir, id_w, order);
        uint64_t off = load_w(dir + id_w, off_w, order);
        cdb_value v;

        dir += entry;
        if (i > 0 && id <= prev_id)
            return CDB_E_NOT_CANONICAL;
        prev_id = id;
        if (off != (uint64_t)expected)
            return CDB_E_NOT_CANONICAL;
        last_off = off;
        if (expected >= values_len)
            return CDB_E_MALFORMED;
        if ((st = cdb_value_read(values + expected, values_len - expected, order, &v)) != CDB_OK)
            return st;
        if ((st = cdb_value_validate(values + expected, v.encoded_len, order, dm, 0)) != CDB_OK)
            return st;
        expected += v.encoded_len;
    }
    if (id_w != width_for(prev_id) || off_w != width_for(last_off))
        return CDB_E_NOT_CANONICAL;
    return expected == values_len ? CDB_OK : CDB_E_MALFORMED;
}

/* The canonical rules for a vector's element bytes (spec 14.6.5): every
 * float element is already in its canonical bit pattern, and a BINARY
 * vector's unused tail bits are zero. Mirrors cdb_value.c's
 * validate_vector, but through the public cdb_value_f32_canonical and
 * cdb_value_half_canonical rather than duplicating their logic. */
static cdb_status validate_vector_data(cdb_byte_order order, cdb_vector_elem elem, uint64_t dims,
                                       const uint8_t *data, size_t data_len)
{
    uint64_t i;

    switch (elem) {
    case CDB_VEC_F32:
        for (i = 0; i < dims; i++) {
            uint32_t bits = cdb_load_u32(data + i * 4u, order);
            if (bits != cdb_value_f32_canonical(bits))
                return CDB_E_NOT_CANONICAL;
        }
        break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        for (i = 0; i < dims; i++) {
            uint16_t bits = cdb_load_u16(data + i * 2u, order);
            if (bits != cdb_value_half_canonical(bits, elem))
                return CDB_E_NOT_CANONICAL;
        }
        break;
    case CDB_VEC_I8:
        break;
    case CDB_VEC_BINARY:
        if (dims % 8u) {
            uint8_t unused = (uint8_t)~((1u << (unsigned)(dims % 8u)) - 1u);
            if (data[data_len - 1] & unused)
                return CDB_E_NOT_CANONICAL;
        }
        break;
    }
    return CDB_OK;
}

/* ---- items (reader side: no tag in front) ---------------------------------
 *
 * Checks one item at p[0..avail) of a column of type `tag` and gives its
 * length. A fixed-shape item is checked with cdb_value_validate on a copy
 * with its tag restored. Array and object payloads recurse as above;
 * decimal and vector are shape-only (see cdb_colenc.h). */
static cdb_status parse_item(uint8_t tag, cdb_byte_order order, cdb_decimal_mode dm,
                             const uint8_t *p, size_t avail, size_t *ilen)
{
    uint8_t tmp[24];
    uint64_t v;
    size_t n, n2;
    cdb_status st;

    if (IS_BOOL(tag)) {
        if (avail < 1)
            return CDB_E_TRUNCATED;
        if (p[0] > 1u)
            return CDB_E_MALFORMED;
        *ilen = 1;
        return CDB_OK;
    }
    if (tag >= CDB_TAG_FIRST_VARIABLE) {
        if ((st = cdb_varint_decode(p, avail, &v, &n)) != CDB_OK)
            return st;
        if (v > (uint64_t)(avail - n))
            return CDB_E_TRUNCATED;
        switch (tag) {
        case CDB_TAG_STRING:
            if (!cdb_value_utf8_valid(p + n, (size_t)v))
                return CDB_E_MALFORMED;
            break;
        case CDB_TAG_BINARY:
            break; /* any bytes are canonical */
        case CDB_TAG_ARRAY:
            if ((st = validate_array_payload(order, dm, p + n, (size_t)v)) != CDB_OK)
                return st;
            break;
        case CDB_TAG_OBJECT:
            if ((st = validate_object_payload(order, dm, p + n, (size_t)v)) != CDB_OK)
                return st;
            break;
        case CDB_TAG_DECIMAL: {
            /* Scale, then at least one unscaled byte (shape), checked for
             * the shortest big-endian form and, in compact mode, the
             * 128-bit limit, through the public wrappers over cdb_value.c's
             * own canonical rules (spec 14.6.4, 14.6.5). */
            uint64_t scale;
            size_t sn;
            const uint8_t *unscaled;
            size_t unscaled_len;

            if ((st = cdb_varint_decode(p + n, (size_t)v, &scale, &sn)) != CDB_OK)
                return st == CDB_E_TRUNCATED ? CDB_E_MALFORMED : st;
            if (sn == (size_t)v)
                return CDB_E_MALFORMED; /* no unscaled bytes */
            unscaled = p + n + sn;
            unscaled_len = (size_t)v - sn;
            if (!cdb_value_decimal_is_canonical(unscaled, unscaled_len))
                return CDB_E_NOT_CANONICAL;
            if (dm == CDB_DECIMAL_COMPACT && !cdb_value_decimal_fits_compact(unscaled, unscaled_len))
                return CDB_E_OVERFLOW;
            break;
        }
        case CDB_TAG_VECTOR: {
            /* Element type, dims, exactly enough data bytes (shape), then
             * each element's canonical bit pattern, through the public
             * wrappers over cdb_value.c's own canonical rules. */
            cdb_vector_elem elem;
            uint64_t dims;
            size_t bits, dn, want;

            if (v == 0)
                return CDB_E_MALFORMED;
            elem = (cdb_vector_elem)p[n];
            bits = vec_elem_bits(elem);
            if (bits == 0)
                return CDB_E_MALFORMED;
            if ((st = cdb_varint_decode(p + n + 1, (size_t)v - 1u, &dims, &dn)) != CDB_OK)
                return st == CDB_E_TRUNCATED ? CDB_E_MALFORMED : st;
            if (dims > (SIZE_MAX - 7u) / bits)
                return CDB_E_OVERFLOW;
            want = (size_t)((dims * bits + 7u) / 8u);
            if ((size_t)v - 1u - dn != want)
                return CDB_E_MALFORMED;
            if ((st = validate_vector_data(order, elem, dims, p + n + 1 + dn, want)) != CDB_OK)
                return st;
            break;
        }
        default: /* binary already handled above */
            break;
        }
        *ilen = n + (size_t)v;
        return CDB_OK;
    }
    switch (tag) {
    case CDB_TAG_INT:
        if ((st = cdb_varint_decode(p, avail, &v, &n)) != CDB_OK)
            return st;
        break;
    case CDB_TAG_REF:
        if ((st = cdb_varint_decode(p, avail, &v, &n)) != CDB_OK)
            return st;
        if ((st = cdb_varint_decode(p + n, avail - n, &v, &n2)) != CDB_OK)
            return st;
        n += n2;
        break;
    case CDB_TAG_F64:
        n = 8;
        break;
    case CDB_TAG_F32:
        n = 4;
        break;
    case CDB_TAG_TIMESTAMP:
        n = 9;
        break;
    default:
        return CDB_E_INVALID_ARG;
    }
    if (avail < n)
        return CDB_E_TRUNCATED;
    tmp[0] = tag;
    memcpy(tmp + 1, p, n);
    if (cdb_value_validate(tmp, n + 1, order, dm, 0) != CDB_OK)
        return CDB_E_MALFORMED;
    *ilen = n;
    return CDB_OK;
}

/* ---- encoder -------------------------------------------------------------- */

/* Output that counts what it is given and copies only what fits. */
typedef struct sink {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int over;
} sink;

static uint8_t *sink_take(sink *s, size_t x)
{
    uint8_t *p = NULL;

    if (!s->over && s->buf != NULL && x <= s->cap - s->len)
        p = s->buf + s->len;
    else if (x > 0)
        s->over = 1;
    s->len += x;
    return p;
}

static void sink_put(sink *s, const uint8_t *p, size_t x)
{
    uint8_t *q = sink_take(s, x);

    if (q != NULL && x > 0)
        memcpy(q, p, x);
}

static void sink_varint(sink *s, uint64_t v)
{
    uint8_t tmp[CDB_VARINT_MAX_LEN];
    size_t n = cdb_varint_encode(v, tmp);

    sink_put(s, tmp, n);
}

/* The input values, parsed once: vo[i] is where value i starts; vo[n] is the
 * end. idx, first and the hash table serve the dictionary. */
typedef struct enc_ctx {
    uint8_t tag;
    cdb_byte_order order;
    const uint8_t *in;
    size_t n;
    uint64_t *vo;
    uint64_t *idx;   /* dictionary index of each value */
    uint64_t *first; /* first value of each dictionary entry */
    uint64_t *table; /* open-addressing hash table: 0 = empty, else index + 1 */
    size_t table_mask;
    size_t k;        /* dictionary entries */
    size_t dict_items;
} enc_ctx;

static size_t item_size(const enc_ctx *c, size_t i)
{
    return IS_BOOL(c->tag) ? 1u : (size_t)(c->vo[i + 1] - c->vo[i]) - 1u;
}

static void put_item(sink *s, const enc_ctx *c, size_t i)
{
    if (IS_BOOL(c->tag)) {
        uint8_t b = (uint8_t)(c->in[c->vo[i]] == CDB_TAG_TRUE ? 1u : 0u);
        sink_put(s, &b, 1);
    } else {
        sink_put(s, c->in + c->vo[i] + 1, item_size(c, i));
    }
}

/* Values are canonical, so equal values have equal bytes. */
static int same_value(const enc_ctx *c, size_t i, size_t j)
{
    size_t li = (size_t)(c->vo[i + 1] - c->vo[i]);

    return li == (size_t)(c->vo[j + 1] - c->vo[j]) &&
           memcmp(c->in + c->vo[i], c->in + c->vo[j], li) == 0;
}

/* FNV-1a over an item's raw bytes. Only used to find a candidate match
 * faster; a wrong or collision-heavy hash cannot change the result, since a
 * candidate is always confirmed with same_value before it is accepted. */
static uint64_t hash_item(const enc_ctx *c, size_t i)
{
    uint64_t h = 1469598103934665603ULL;
    size_t off = IS_BOOL(c->tag) ? c->vo[i] : c->vo[i] + 1u;
    size_t len = item_size(c, i);
    size_t j;

    for (j = 0; j < len; j++) {
        h ^= c->in[off + j];
        h *= 1099511628211ULL;
    }
    return h;
}

/* The integer an int-like value stands for (and a timestamp's precision). */
static void get_int(const enc_ctx *c, size_t i, int64_t *v, uint8_t *prec)
{
    const uint8_t *p = c->in + c->vo[i];
    uint64_t u;
    size_t nn;

    switch (c->tag) {
    case CDB_TAG_INT:
        (void)cdb_varint_decode(p + 1, (size_t)(c->vo[i + 1] - c->vo[i]) - 1u, &u, &nn);
        *v = cdb_zigzag_decode(u);
        break;
    case CDB_TAG_TIMESTAMP:
        *prec = p[1];
        *v = (int64_t)cdb_load_u64(p + 2, c->order);
        break;
    default: /* bool */
        *v = p[0] == CDB_TAG_TRUE ? 1 : 0;
        break;
    }
}

/* Checks one input value with its real tag in front: the same canonical
 * check cdb_value.c applies anywhere else in the file, reaching every type
 * including decimal and vector in full (see cdb_colenc.h). */
static cdb_status validate_input_value(uint8_t tag, cdb_byte_order order, cdb_decimal_mode dm,
                                       const uint8_t *buf, size_t avail, size_t *vlen)
{
    cdb_value v;
    cdb_status st;

    if ((st = cdb_value_read(buf, avail, order, &v)) != CDB_OK)
        return st;
    if (IS_BOOL(tag) ? v.type != CDB_TYPE_BOOL : v.tag != tag)
        return CDB_E_INVALID_ARG;
    if ((st = cdb_value_validate(buf, v.encoded_len, order, dm, 0)) != CDB_OK)
        return st;
    *vlen = v.encoded_len;
    return CDB_OK;
}

/* Parses the input into vo; checks every value. */
static cdb_status parse_input(enc_ctx *c, cdb_decimal_mode dm, size_t values_len)
{
    size_t pos = 0, i, ilen;
    cdb_status st;

    for (i = 0; i < c->n; i++) {
        c->vo[i] = (uint64_t)pos;
        if (pos >= values_len)
            return CDB_E_TRUNCATED;
        if ((st = validate_input_value(c->tag, c->order, dm, c->in + pos, values_len - pos,
                                       &ilen)) != CDB_OK)
            return st;
        pos += ilen;
    }
    c->vo[c->n] = (uint64_t)pos;
    return pos == values_len ? CDB_OK : CDB_E_INVALID_ARG;
}

static size_t plain_size(const enc_ctx *c)
{
    size_t i, total = 0;

    for (i = 0; i < c->n; i++)
        total += item_size(c, i);
    return total;
}

/* Builds the dictionary with a hash table over the scratch array to find a
 * repeated value without rescanning every earlier entry. Returns 0 (and
 * stops early) once the entries alone reach `limit` bytes, because the
 * result could then not beat that size. */
static int build_dict(enc_ctx *c, size_t limit)
{
    size_t i;

    c->k = 0;
    c->dict_items = 0;
    memset(c->table, 0, (c->table_mask + 1u) * sizeof *c->table);
    for (i = 0; i < c->n; i++) {
        size_t slot = (size_t)(hash_item(c, i) & c->table_mask);
        size_t j = 0;
        int found = 0;

        for (;;) {
            uint64_t ent = c->table[slot];

            if (ent == 0)
                break; /* empty: no match */
            j = (size_t)(ent - 1u);
            if (same_value(c, c->first[j], i)) {
                found = 1;
                break;
            }
            slot = (slot + 1u) & c->table_mask;
        }
        if (found) {
            c->idx[i] = (uint64_t)j;
            continue;
        }
        j = c->k;
        c->first[j] = (uint64_t)i;
        c->table[slot] = (uint64_t)j + 1u;
        c->k++;
        c->idx[i] = (uint64_t)j;
        c->dict_items += item_size(c, i);
        if (c->dict_items >= limit)
            return 0;
    }
    return 1;
}

static cdb_status run_plain(const enc_ctx *c, sink *s)
{
    size_t i;

    for (i = 0; i < c->n; i++)
        put_item(s, c, i);
    return CDB_OK;
}

static cdb_status run_dict(const enc_ctx *c, sink *s)
{
    size_t j, i, nb;
    unsigned w = bitlen((uint64_t)c->k - 1u);
    int ok;
    uint8_t *region;

    sink_varint(s, (uint64_t)c->k);
    for (j = 0; j < c->k; j++)
        put_item(s, c, (size_t)c->first[j]);
    nb = packed_bytes(c->n, w, &ok);
    if (!ok)
        return CDB_E_OVERFLOW;
    region = sink_take(s, nb);
    if (region != NULL && nb > 0) {
        cdb_bitw bw;

        memset(region, 0, nb);
        cdb_bitw_init(&bw, region, nb);
        for (i = 0; i < c->n; i++)
            (void)cdb_bitw_put(&bw, c->idx[i], w);
    }
    return CDB_OK;
}

static cdb_status run_rle(const enc_ctx *c, sink *s)
{
    size_t i = 0, j;

    while (i < c->n) {
        j = i + 1;
        while (j < c->n && same_value(c, i, j))
            j++;
        sink_varint(s, (uint64_t)(j - i));
        put_item(s, c, i);
        i = j;
    }
    return CDB_OK;
}

static cdb_status run_bitpack(const enc_ctx *c, sink *s)
{
    int64_t v, min = 0, max = 0;
    uint8_t prec = 0, p2 = 0;
    uint64_t span;
    size_t i, nb;
    unsigned w;
    uint8_t wb;
    int ok;
    uint8_t *region;

    if (!cdb_colenc_can_bitpack(c->tag) || c->n == 0)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < c->n; i++) {
        get_int(c, i, &v, &p2);
        if (i == 0) {
            min = max = v;
            prec = p2;
        } else {
            if (c->tag == CDB_TAG_TIMESTAMP && p2 != prec)
                return CDB_E_INVALID_ARG; /* mixed precisions */
            if (v < min)
                min = v;
            if (v > max)
                max = v;
        }
    }
    span = (uint64_t)max - (uint64_t)min; /* uint64: never overflows */
    w = bitlen(span);
    if (c->tag == CDB_TAG_TIMESTAMP)
        sink_put(s, &prec, 1);
    sink_varint(s, cdb_zigzag_encode(min));
    wb = (uint8_t)w;
    sink_put(s, &wb, 1);
    nb = packed_bytes(c->n, w, &ok);
    if (!ok)
        return CDB_E_OVERFLOW;
    region = sink_take(s, nb);
    if (region != NULL && nb > 0) {
        cdb_bitw bw;

        memset(region, 0, nb);
        cdb_bitw_init(&bw, region, nb);
        for (i = 0; i < c->n; i++) {
            get_int(c, i, &v, &p2);
            (void)cdb_bitw_put(&bw, (uint64_t)v - (uint64_t)min, w);
        }
    }
    return CDB_OK;
}

static cdb_status run_enc(const enc_ctx *c, unsigned enc, sink *s)
{
    switch (enc) {
    case CDB_COLENC_PLAIN:
        return run_plain(c, s);
    case CDB_COLENC_DICT:
        return run_dict(c, s);
    case CDB_COLENC_RLE:
        return run_rle(c, s);
    default:
        return run_bitpack(c, s);
    }
}

#define AUTO 0xFFu

static cdb_status encode_impl(unsigned want, uint8_t tag, cdb_byte_order order,
                              cdb_decimal_mode dm, const uint8_t *values, size_t values_len,
                              size_t n, uint64_t *scratch, size_t scratch_words, uint8_t *out,
                              size_t out_cap, unsigned *enc, size_t *out_len)
{
    enc_ctx c;
    sink s;
    unsigned e, best = 0;
    size_t best_size = (size_t)-1, psize;
    cdb_status st;

    if (out_len == NULL || (n > 0 && (values == NULL || scratch == NULL)) ||
        (out == NULL && out_cap > 0) || !normalize_tag(&tag) ||
        (want != AUTO && want > CDB_COLENC_BITPACK))
        return CDB_E_INVALID_ARG;
    if (enc != NULL)
        *enc = CDB_COLENC_PLAIN;
    *out_len = 0;
    if (n == 0) { /* empty: plain, zero bytes */
        if (values_len != 0)
            return CDB_E_INVALID_ARG;
        if (want != AUTO && want != CDB_COLENC_PLAIN)
            return CDB_E_INVALID_ARG;
        return CDB_OK;
    }
    if (n > values_len)
        return CDB_E_INVALID_ARG;
    if (scratch_words < cdb_colenc_scratch_words(n))
        return CDB_E_BUFFER_TOO_SMALL;
    c.tag = tag;
    c.order = order;
    c.in = values;
    c.n = n;
    c.vo = scratch;
    c.idx = scratch + n + 1;
    c.first = scratch + 2u * n + 1u;
    c.table = scratch + 3u * n + 1u;
    c.table_mask = dict_table_words(n) - 1u;
    c.k = 0;
    c.dict_items = 0;
    if ((st = parse_input(&c, dm, values_len)) != CDB_OK)
        return st;
    psize = plain_size(&c);

    if (want != AUTO) {
        best = want;
        if (want == CDB_COLENC_DICT)
            (void)build_dict(&c, (size_t)-1);
    } else {
        int dict_ok = build_dict(&c, psize);

        for (e = 0; e <= CDB_COLENC_BITPACK; e++) {
            memset(&s, 0, sizeof s);
            if (e == CDB_COLENC_DICT && !dict_ok)
                continue;
            if (run_enc(&c, e, &s) != CDB_OK)
                continue; /* does not apply */
            if (s.len < best_size) {
                best_size = s.len;
                best = e;
            }
        }
    }
    /* Run the winner for real. */
    s.buf = out;
    s.cap = out_cap;
    s.len = 0;
    s.over = 0;
    if ((st = run_enc(&c, best, &s)) != CDB_OK)
        return st;
    if (enc != NULL)
        *enc = best;
    *out_len = s.len;
    return s.over ? CDB_E_BUFFER_TOO_SMALL : CDB_OK;
}

cdb_status cdb_colenc_encode(uint8_t tag, cdb_byte_order order, cdb_decimal_mode decimal_mode,
                             const uint8_t *values, size_t values_len, size_t n,
                             uint64_t *scratch, size_t scratch_words, uint8_t *out,
                             size_t out_cap, unsigned *enc, size_t *out_len)
{
    return encode_impl(AUTO, tag, order, decimal_mode, values, values_len, n, scratch,
                       scratch_words, out, out_cap, enc, out_len);
}

cdb_status cdb_colenc_encode_as(unsigned want, uint8_t tag, cdb_byte_order order,
                                cdb_decimal_mode decimal_mode, const uint8_t *values,
                                size_t values_len, size_t n, uint64_t *scratch,
                                size_t scratch_words, uint8_t *out, size_t out_cap,
                                size_t *out_len)
{
    return encode_impl(want, tag, order, decimal_mode, values, values_len, n, scratch,
                       scratch_words, out, out_cap, NULL, out_len);
}

/* ---- reader --------------------------------------------------------------- */

static cdb_status fail(cdb_colenc_reader *r, cdb_status st)
{
    r->err = st;
    return st;
}

/* Header of the dictionary form: k, the entries (checked, distinct), the
 * bit position of the indexes. */
static cdb_status init_dict(cdb_colenc_reader *r, uint64_t *scratch, size_t scratch_words)
{
    uint64_t k;
    size_t nn, pos, j, m, il, packed, left;
    int ok;
    cdb_status st;

    if ((st = cdb_varint_decode(r->data, r->len, &k, &nn)) != CDB_OK)
        return st;
    if (k == 0 || k > (uint64_t)r->n)
        return CDB_E_CORRUPT;
    if (scratch == NULL || (uint64_t)scratch_words < k + 1u)
        return CDB_E_BUFFER_TOO_SMALL;
    pos = nn;
    r->offs = scratch;
    r->k = k;
    for (j = 0; j < (size_t)k; j++) {
        r->offs[j] = (uint64_t)pos;
        if ((st = parse_item(r->tag, r->order, r->decimal_mode, r->data + pos, r->len - pos,
                             &il)) != CDB_OK)
            return st;
        pos += il;
        r->offs[j + 1] = (uint64_t)pos;
        for (m = 0; m < j; m++)
            if (r->offs[m + 1] - r->offs[m] == il &&
                memcmp(r->data + r->offs[m], r->data + r->offs[j], il) == 0)
                return CDB_E_NOT_CANONICAL; /* repeated entry */
    }
    r->width = bitlen(k - 1u);
    packed = packed_bytes(r->n, r->width, &ok);
    if (!ok)
        return CDB_E_OVERFLOW;
    left = r->len - pos;
    if (left < packed)
        return CDB_E_TRUNCATED;
    if (left > packed)
        return CDB_E_NOT_CANONICAL;
    cdb_bitr_init(&r->bits, r->data + pos, left);
    return CDB_OK;
}

static cdb_status init_bitpack(cdb_colenc_reader *r)
{
    size_t pos = 0, nn, packed, left;
    uint64_t z;
    int ok;
    cdb_status st;

    if (!cdb_colenc_can_bitpack(r->tag))
        return CDB_E_CORRUPT;
    if (r->tag == CDB_TAG_TIMESTAMP) {
        if (r->len < 1)
            return CDB_E_TRUNCATED;
        r->prec = r->data[0];
        if (r->prec > (uint8_t)CDB_TS_NANOS)
            return CDB_E_MALFORMED;
        pos = 1;
    }
    if ((st = cdb_varint_decode(r->data + pos, r->len - pos, &z, &nn)) != CDB_OK)
        return st;
    pos += nn;
    r->min = cdb_zigzag_decode(z);
    if (IS_BOOL(r->tag) && (r->min < 0 || r->min > 1))
        return CDB_E_CORRUPT;
    if (pos >= r->len)
        return CDB_E_TRUNCATED;
    if (r->data[pos] > 64u)
        return CDB_E_CORRUPT;
    r->width = r->data[pos];
    pos++;
    packed = packed_bytes(r->n, r->width, &ok);
    if (!ok)
        return CDB_E_OVERFLOW;
    left = r->len - pos;
    if (left < packed)
        return CDB_E_TRUNCATED;
    if (left > packed)
        return CDB_E_NOT_CANONICAL;
    cdb_bitr_init(&r->bits, r->data + pos, left);
    r->max_delta = 0;
    r->saw_min = 0;
    return CDB_OK;
}

cdb_status cdb_colenc_reader_init(cdb_colenc_reader *r, uint8_t tag, cdb_byte_order order,
                                  cdb_decimal_mode decimal_mode, unsigned enc,
                                  const uint8_t *data, size_t len, size_t n, uint64_t *scratch,
                                  size_t scratch_words)
{
    cdb_status st = CDB_OK;

    if (r == NULL || (data == NULL && len > 0) || !normalize_tag(&tag))
        return CDB_E_INVALID_ARG;
    memset(r, 0, sizeof *r);
    r->tag = tag;
    r->order = order;
    r->decimal_mode = decimal_mode;
    r->enc = (uint8_t)enc;
    r->data = data;
    r->len = len;
    r->n = n;
    if (enc > CDB_COLENC_BITPACK)
        return CDB_E_UNSUPPORTED;
    if (n == 0) /* the canonical empty chunk: plain, no bytes */
        return (enc == CDB_COLENC_PLAIN && len == 0) ? CDB_OK : CDB_E_NOT_CANONICAL;
    if (enc == CDB_COLENC_DICT)
        st = init_dict(r, scratch, scratch_words);
    else if (enc == CDB_COLENC_BITPACK)
        st = init_bitpack(r);
    if (st != CDB_OK)
        r->err = st;
    return st;
}

cdb_status cdb_colenc_next(cdb_colenc_reader *r, const uint8_t **item, size_t *item_len,
                           int *done)
{
    size_t il, nn;
    uint64_t v;
    cdb_status st;

    if (r->err != CDB_OK)
        return r->err;
    if (r->i >= r->n) {
        *done = 1;
        return CDB_OK;
    }
    *done = 0;
    switch (r->enc) {
    case CDB_COLENC_PLAIN:
        if ((st = parse_item(r->tag, r->order, r->decimal_mode, r->data + r->pos,
                             r->len - r->pos, &il)) != CDB_OK)
            return fail(r, st);
        *item = r->data + r->pos;
        *item_len = il;
        r->pos += il;
        break;
    case CDB_COLENC_RLE:
        if (r->run_left == 0) {
            const uint8_t *prev = r->run_item;
            size_t prev_len = r->run_item_len;

            if ((st = cdb_varint_decode(r->data + r->pos, r->len - r->pos, &v, &nn)) != CDB_OK)
                return fail(r, st);
            if (v == 0 || v > (uint64_t)(r->n - r->i))
                return fail(r, CDB_E_CORRUPT);
            r->pos += nn;
            if ((st = parse_item(r->tag, r->order, r->decimal_mode, r->data + r->pos,
                                 r->len - r->pos, &il)) != CDB_OK)
                return fail(r, st);
            if (r->have_prev && il == prev_len && memcmp(prev, r->data + r->pos, il) == 0)
                return fail(r, CDB_E_NOT_CANONICAL); /* adjacent runs of one item */
            r->run_left = v;
            r->run_item = r->data + r->pos;
            r->run_item_len = il;
            r->have_prev = 1;
            r->pos += il;
        }
        r->run_left--;
        *item = r->run_item;
        *item_len = r->run_item_len;
        break;
    case CDB_COLENC_DICT: {
        uint64_t idx = 0;

        if (r->width > 0 && (st = cdb_bitr_get(&r->bits, &idx, r->width)) != CDB_OK)
            return fail(r, st);
        if (idx >= r->k || idx > r->next_new)
            return fail(r, idx >= r->k ? CDB_E_CORRUPT : CDB_E_NOT_CANONICAL);
        if (idx == r->next_new)
            r->next_new++; /* first use must come in entry order */
        *item = r->data + r->offs[idx];
        *item_len = (size_t)(r->offs[idx + 1] - r->offs[idx]);
        break;
    }
    default: { /* bit-packed */
        uint64_t delta, u;
        int64_t sv;

        if ((st = cdb_bitr_get(&r->bits, &delta, r->width)) != CDB_OK)
            return fail(r, st);
        u = (uint64_t)r->min + delta;
        sv = (int64_t)u;
        if (sv < r->min || (IS_BOOL(r->tag) && sv > 1))
            return fail(r, CDB_E_CORRUPT); /* wrapped below the minimum */
        if (delta > r->max_delta)
            r->max_delta = delta;
        if (delta == 0)
            r->saw_min = 1;
        if (r->tag == CDB_TAG_INT) {
            il = cdb_varint_encode(cdb_zigzag_encode(sv), r->tmp);
        } else if (r->tag == CDB_TAG_TIMESTAMP) {
            r->tmp[0] = r->prec;
            cdb_store_u64(r->tmp + 1, (uint64_t)sv, r->order);
            il = 9;
        } else {
            r->tmp[0] = (uint8_t)sv;
            il = 1;
        }
        *item = r->tmp;
        *item_len = il;
        break;
    }
    }
    r->i++;
    return CDB_OK;
}

cdb_status cdb_colenc_reader_finish(const cdb_colenc_reader *r)
{
    if (r->err != CDB_OK)
        return r->err;
    if (r->i != r->n)
        return CDB_E_STATE;
    if (r->n == 0)
        return CDB_OK;
    switch (r->enc) {
    case CDB_COLENC_PLAIN:
    case CDB_COLENC_RLE:
        return r->pos == r->len ? CDB_OK : CDB_E_NOT_CANONICAL;
    case CDB_COLENC_DICT:
        if (r->next_new != r->k)
            return CDB_E_NOT_CANONICAL; /* an entry nothing uses */
        return cdb_bitr_check_end(&r->bits);
    default:
        if (r->width != bitlen(r->max_delta) || !r->saw_min)
            return CDB_E_NOT_CANONICAL; /* width or minimum larger than needed */
        return cdb_bitr_check_end(&r->bits);
    }
}

cdb_status cdb_colenc_validate(uint8_t tag, cdb_byte_order order, cdb_decimal_mode decimal_mode,
                               unsigned enc, const uint8_t *data, size_t len, size_t n,
                               uint64_t *scratch, size_t scratch_words)
{
    cdb_colenc_reader r;
    const uint8_t *item;
    size_t il;
    int done = 0;
    cdb_status st;

    if ((st = cdb_colenc_reader_init(&r, tag, order, decimal_mode, enc, data, len, n, scratch,
                                     scratch_words)) != CDB_OK)
        return st;
    for (;;) {
        if ((st = cdb_colenc_next(&r, &item, &il, &done)) != CDB_OK)
            return st;
        if (done)
            break;
    }
    return cdb_colenc_reader_finish(&r);
}
