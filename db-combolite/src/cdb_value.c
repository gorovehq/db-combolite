/* Shared value encoding (spec 14.6): writer, reader and canonical validator.
 *
 * Three parts. The writer builds one value into a caller buffer, checking
 * room before every write so a failure never leaves a partial item; its
 * error is sticky. The reader decodes one value's header and gives views
 * into the buffer, checking only that value's bounds. The validator walks a
 * whole value recursively and rejects anything that is not the single
 * canonical encoding (spec 14.6.5), which content hashing depends on. */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_endian.h"
#include "cdb_varint.h"

#define F64_CANONICAL_NAN UINT64_C(0x7FF8000000000000)
#define F64_NEG_ZERO UINT64_C(0x8000000000000000)
#define F32_CANONICAL_NAN UINT32_C(0x7FC00000)
#define F32_NEG_ZERO UINT32_C(0x80000000)
#define F16_CANONICAL_NAN 0x7E00u
#define BF16_CANONICAL_NAN 0x7FC0u
#define HALF_NEG_ZERO 0x8000u

/* Each staged object directory entry: field ID then value offset. */
#define SCRATCH_ENTRY_SIZE 16u

/* 10^38, big-endian: compact decimals must stay below this (spec 14.6.4). */
static const uint8_t ten_pow_38[16] = {
    0x4B, 0x3B, 0x4C, 0xA8, 0x5A, 0x86, 0xC4, 0x7A,
    0x09, 0x8A, 0x22, 0x40, 0x00, 0x00, 0x00, 0x00
};

/* ------------------------------------------------------------ helpers */

static int f64_is_nan(uint64_t bits)
{
    return (bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)
        && (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0;
}

static int f32_is_nan(uint32_t bits)
{
    return (bits & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000)
        && (bits & UINT32_C(0x007FFFFF)) != 0;
}

/* Canonical float bit patterns: every NaN becomes the one quiet NaN, and
 * -0 becomes +0 (spec 14.6.5). */
static uint64_t f64_canonical(uint64_t bits)
{
    if (f64_is_nan(bits))
        return F64_CANONICAL_NAN;
    if (bits == F64_NEG_ZERO)
        return 0;
    return bits;
}

static uint32_t f32_canonical(uint32_t bits)
{
    if (f32_is_nan(bits))
        return F32_CANONICAL_NAN;
    if (bits == F32_NEG_ZERO)
        return 0;
    return bits;
}

/* The same for 16-bit vector elements; f16 and bf16 differ in where the
 * exponent and mantissa sit. */
static uint16_t half_canonical(uint16_t bits, cdb_vector_elem elem)
{
    if (elem == CDB_VEC_F16) {
        if ((bits & 0x7C00u) == 0x7C00u && (bits & 0x03FFu) != 0)
            return (uint16_t)F16_CANONICAL_NAN;
    } else {
        if ((bits & 0x7F80u) == 0x7F80u && (bits & 0x007Fu) != 0)
            return (uint16_t)BF16_CANONICAL_NAN;
    }
    if (bits == HALF_NEG_ZERO)
        return 0;
    return bits;
}

/* Strict UTF-8: sequences of 1 to 4 bytes with correct continuation bytes,
 * no overlong forms (each length has a minimum code point), nothing above
 * U+10FFFF, and no surrogates (U+D800..U+DFFF). Returns 1 if valid. */
int cdb_value_utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;

    while (i < len) {
        uint8_t c = s[i];
        size_t need;
        uint32_t cp;
        uint32_t min;
        size_t k;

        if (c < 0x80u) {
            i++;
            continue;
        } else if ((c & 0xE0u) == 0xC0u) {
            need = 1; cp = c & 0x1Fu; min = 0x80u;
        } else if ((c & 0xF0u) == 0xE0u) {
            need = 2; cp = c & 0x0Fu; min = 0x800u;
        } else if ((c & 0xF8u) == 0xF0u) {
            need = 3; cp = c & 0x07u; min = 0x10000u;
        } else {
            return 0;
        }
        if (len - i <= need)
            return 0;
        for (k = 1; k <= need; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0u) != 0x80u)
                return 0;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return 0;
        i += need + 1;
    }
    return 1;
}

/* Shortest big-endian two's complement: at least one byte, and no leading
 * byte that only repeats the sign of the next one. */
static int decimal_is_canonical(const uint8_t *b, size_t len)
{
    if (len == 0)
        return 0;
    if (len >= 2) {
        if (b[0] == 0x00 && !(b[1] & 0x80u))
            return 0;
        if (b[0] == 0xFF && (b[1] & 0x80u))
            return 0;
    }
    return 1;
}

/* Compact decimals: |unscaled| < 10^38 (spec 14.6.4). b is canonical. */
static int decimal_fits_compact(const uint8_t *b, size_t len)
{
    uint8_t mag[16];
    size_t pad;
    size_t i;
    int negative;

    if (len > 16)
        return 0;
    negative = (b[0] & 0x80u) != 0;
    pad = 16 - len;
    memset(mag, negative ? 0xFF : 0x00, pad);
    memcpy(mag + pad, b, len);
    if (negative) {
        /* Two's complement negate in place: invert, then add one. */
        unsigned carry = 1;
        for (i = 16; i-- > 0;) {
            unsigned v = (unsigned)(uint8_t)~mag[i] + carry;
            mag[i] = (uint8_t)v;
            carry = v >> 8;
        }
    }
    return memcmp(mag, ten_pow_38, sizeof mag) < 0;
}

/* Bits per vector element; 0 for an unknown element type. */
static size_t vector_elem_size_bits(cdb_vector_elem elem)
{
    switch (elem) {
    case CDB_VEC_F32: return 32;
    case CDB_VEC_F16:
    case CDB_VEC_BF16: return 16;
    case CDB_VEC_I8: return 8;
    case CDB_VEC_BINARY: return 1;
    }
    return 0;
}

/* Bytes needed for dims elements, or 0 on overflow or unknown type. */
static size_t vector_data_len(cdb_vector_elem elem, uint64_t dims)
{
    size_t bits = vector_elem_size_bits(elem);

    if (bits == 0 || dims == 0)
        return 0;
    if (bits == 1)
        return dims > (uint64_t)SIZE_MAX - 7u ? 0 : (size_t)((dims + 7u) / 8u);
    if (dims > (uint64_t)(SIZE_MAX / (bits / 8u)))
        return 0;
    return (size_t)dims * (bits / 8u);
}

/* Fixed-width directory entries (spec 14.6.3). Width codes 0..3 = 1, 2, 4, 8 bytes. */
static unsigned width_for(uint64_t v)
{
    if (v <= UINT8_MAX)
        return 1;
    if (v <= UINT16_MAX)
        return 2;
    if (v <= UINT32_MAX)
        return 4;
    return 8;
}

/* Width in bytes to its 2-bit code, and fixed-width loads and stores in
 * the file's byte order. */
static uint8_t width_code(unsigned width)
{
    return (uint8_t)(width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3);
}

static void store_width(uint8_t *p, uint64_t v, unsigned width, cdb_byte_order order)
{
    switch (width) {
    case 1: p[0] = (uint8_t)v; break;
    case 2: cdb_store_u16(p, (uint16_t)v, order); break;
    case 4: cdb_store_u32(p, (uint32_t)v, order); break;
    default: cdb_store_u64(p, v, order); break;
    }
}

static uint64_t load_width(const uint8_t *p, unsigned width, cdb_byte_order order)
{
    switch (width) {
    case 1: return p[0];
    case 2: return cdb_load_u16(p, order);
    case 4: return cdb_load_u32(p, order);
    default: return cdb_load_u64(p, order);
    }
}

/* ------------------------------------------------------------- writer
 *
 * Buffer layout while writing: encoded bytes grow up from the start
 * (w->len); open objects' directory entries are staged at the end, growing
 * down from w->cap (w->scratch_top). The free space is between the two, so
 * w_room measures against scratch_top, not cap. */

/* Records the first error (later ones are not recorded) and returns it. */
static cdb_status w_fail(cdb_value_writer *w, cdb_status status)
{
    if (w->error == CDB_OK)
        w->error = status;
    return status;
}

static cdb_status w_room(cdb_value_writer *w, size_t n)
{
    if (n > w->scratch_top - w->len)
        return w_fail(w, CDB_E_BUFFER_TOO_SMALL);
    return CDB_OK;
}

/* Whether a value may be written now: no earlier error; at the top level
 * only one root value; inside an object, only right after a field ID. */
static cdb_status w_begin_value(cdb_value_writer *w)
{
    if (w->error != CDB_OK)
        return w->error;
    if (w->depth == 0) {
        if (w->root_done)
            return w_fail(w, CDB_E_STATE);
    } else {
        const cdb_value_frame *f = &w->frames[w->depth - 1];
        if (f->kind == CDB_TAG_OBJECT && !f->expect_value)
            return w_fail(w, CDB_E_STATE);
    }
    return CDB_OK;
}

/* Counts a finished value in its container (or marks the root done). */
static void w_end_value(cdb_value_writer *w)
{
    if (w->depth == 0) {
        w->root_done = 1;
    } else {
        cdb_value_frame *f = &w->frames[w->depth - 1];
        f->count++;
        f->expect_value = 0;
    }
}

static void w_put_byte(cdb_value_writer *w, uint8_t b)
{
    w->buf[w->len++] = b;
}

static void w_put(cdb_value_writer *w, const void *data, size_t n)
{
    if (n)
        memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void w_put_varint(cdb_value_writer *w, uint64_t v)
{
    w->len += cdb_varint_encode(v, w->buf + w->len);
}

void cdb_value_writer_init(cdb_value_writer *w, uint8_t *buf, size_t cap,
                           cdb_byte_order order, cdb_decimal_mode decimal_mode)
{
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    w->scratch_top = cap;
    w->order = order;
    w->decimal_mode = decimal_mode;
    w->error = (buf == NULL && cap != 0) ? CDB_E_INVALID_ARG : CDB_OK;
}

size_t cdb_value_writer_len(const cdb_value_writer *w)
{
    return w->len;
}

static cdb_status w_simple(cdb_value_writer *w, uint8_t tag)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((s = w_room(w, 1)) != CDB_OK)
        return s;
    w_put_byte(w, tag);
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_null(cdb_value_writer *w)
{
    return w_simple(w, CDB_TAG_NULL);
}

cdb_status cdb_value_write_bool(cdb_value_writer *w, int value)
{
    return w_simple(w, value ? CDB_TAG_TRUE : CDB_TAG_FALSE);
}

cdb_status cdb_value_write_int(cdb_value_writer *w, int64_t value)
{
    uint64_t z = cdb_zigzag_encode(value);
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((s = w_room(w, 1 + cdb_varint_size(z))) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_INT);
    w_put_varint(w, z);
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_f64(cdb_value_writer *w, double value)
{
    uint64_t bits;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((s = w_room(w, 9)) != CDB_OK)
        return s;
    memcpy(&bits, &value, sizeof bits);
    w_put_byte(w, CDB_TAG_F64);
    cdb_store_u64(w->buf + w->len, f64_canonical(bits), w->order);
    w->len += 8;
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_f32(cdb_value_writer *w, float value)
{
    uint32_t bits;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((s = w_room(w, 5)) != CDB_OK)
        return s;
    memcpy(&bits, &value, sizeof bits);
    w_put_byte(w, CDB_TAG_F32);
    cdb_store_u32(w->buf + w->len, f32_canonical(bits), w->order);
    w->len += 4;
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_timestamp(cdb_value_writer *w, cdb_ts_precision precision, int64_t ticks)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((unsigned)precision > (unsigned)CDB_TS_NANOS)
        return w_fail(w, CDB_E_INVALID_ARG);
    if ((s = w_room(w, 10)) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_TIMESTAMP);
    w_put_byte(w, (uint8_t)precision);
    cdb_store_u64(w->buf + w->len, (uint64_t)ticks, w->order);
    w->len += 8;
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_ref(cdb_value_writer *w, uint64_t store_id, uint64_t entry_id)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if ((s = w_room(w, 1 + cdb_varint_size(store_id) + cdb_varint_size(entry_id))) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_REF);
    w_put_varint(w, store_id);
    w_put_varint(w, entry_id);
    w_end_value(w);
    return CDB_OK;
}

/* Tag, varint length, bytes: strings and binary. */
static cdb_status w_bytes(cdb_value_writer *w, uint8_t tag, const void *data, size_t len)
{
    cdb_status s;
    if (data == NULL && len != 0)
        return w_fail(w, CDB_E_INVALID_ARG);
    if (len > SIZE_MAX - CDB_VARINT_MAX_LEN - 1)
        return w_fail(w, CDB_E_OVERFLOW);
    if ((s = w_room(w, 1 + cdb_varint_size(len) + len)) != CDB_OK)
        return s;
    w_put_byte(w, tag);
    w_put_varint(w, len);
    w_put(w, data, len);
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_string(cdb_value_writer *w, const char *utf8, size_t len)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if (utf8 == NULL && len != 0)
        return w_fail(w, CDB_E_INVALID_ARG);
    if (len != 0 && !cdb_value_utf8_valid((const uint8_t *)utf8, len))
        return w_fail(w, CDB_E_INVALID_ARG);
    return w_bytes(w, CDB_TAG_STRING, utf8, len);
}

cdb_status cdb_value_write_binary(cdb_value_writer *w, const void *data, size_t len)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    return w_bytes(w, CDB_TAG_BINARY, data, len);
}

cdb_status cdb_value_write_decimal(cdb_value_writer *w, uint64_t scale,
                                   const uint8_t *unscaled_be, size_t unscaled_len)
{
    size_t payload;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if (unscaled_be == NULL || !decimal_is_canonical(unscaled_be, unscaled_len))
        return w_fail(w, CDB_E_INVALID_ARG);
    if (w->decimal_mode == CDB_DECIMAL_COMPACT && !decimal_fits_compact(unscaled_be, unscaled_len))
        return w_fail(w, CDB_E_OVERFLOW);
    if (unscaled_len > SIZE_MAX - CDB_VARINT_MAX_LEN * 2 - 1)
        return w_fail(w, CDB_E_OVERFLOW);
    payload = cdb_varint_size(scale) + unscaled_len;
    if ((s = w_room(w, 1 + cdb_varint_size(payload) + payload)) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_DECIMAL);
    w_put_varint(w, payload);
    w_put_varint(w, scale);
    w_put(w, unscaled_be, unscaled_len);
    w_end_value(w);
    return CDB_OK;
}

/* Writes the int64 as 8 big-endian bytes, then drops leading bytes that
 * only repeat the sign until the form is the shortest. */
cdb_status cdb_value_write_decimal_i64(cdb_value_writer *w, uint64_t scale, int64_t unscaled)
{
    uint8_t be[8];
    size_t start = 0;

    cdb_store_u64(be, (uint64_t)unscaled, CDB_ORDER_BIG);
    while (start < 7 && !decimal_is_canonical(be + start, 8 - start))
        start++;
    return cdb_value_write_decimal(w, scale, be + start, 8 - start);
}

cdb_status cdb_value_write_vector_raw(cdb_value_writer *w, cdb_vector_elem elem, uint64_t dims,
                                      const void *data, size_t len)
{
    size_t payload;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if (data == NULL || len == 0 || vector_data_len(elem, dims) != len)
        return w_fail(w, CDB_E_INVALID_ARG);
    payload = 1 + cdb_varint_size(dims) + len;
    if ((s = w_room(w, 1 + cdb_varint_size(payload) + payload)) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_VECTOR);
    w_put_varint(w, payload);
    w_put_byte(w, (uint8_t)elem);
    w_put_varint(w, dims);
    w_put(w, data, len);
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_vector(cdb_value_writer *w, cdb_vector_elem elem,
                                  uint64_t dims, const void *elems)
{
    size_t data_len;
    size_t payload;
    uint8_t *out;
    uint64_t i;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    data_len = vector_data_len(elem, dims);
    if (elems == NULL || data_len == 0)
        return w_fail(w, CDB_E_INVALID_ARG);
    if (data_len > SIZE_MAX - CDB_VARINT_MAX_LEN * 2 - 2)
        return w_fail(w, CDB_E_OVERFLOW);
    payload = 1 + cdb_varint_size(dims) + data_len;
    if ((s = w_room(w, 1 + cdb_varint_size(payload) + payload)) != CDB_OK)
        return s;
    w_put_byte(w, CDB_TAG_VECTOR);
    w_put_varint(w, payload);
    w_put_byte(w, (uint8_t)elem);
    w_put_varint(w, dims);
    out = w->buf + w->len;
    switch (elem) {
    case CDB_VEC_F32: {
        const float *src = (const float *)elems;
        for (i = 0; i < dims; i++) {
            uint32_t bits;
            memcpy(&bits, &src[i], sizeof bits);
            cdb_store_u32(out + i * 4u, f32_canonical(bits), w->order);
        }
        break;
    }
    case CDB_VEC_F16:
    case CDB_VEC_BF16: {
        const uint16_t *src = (const uint16_t *)elems;
        for (i = 0; i < dims; i++)
            cdb_store_u16(out + i * 2u, half_canonical(src[i], elem), w->order);
        break;
    }
    case CDB_VEC_I8:
        memcpy(out, elems, data_len);
        break;
    case CDB_VEC_BINARY:
        /* Unused bits of the last byte are cleared (canonical form). */
        memcpy(out, elems, data_len);
        if (dims % 8u)
            out[data_len - 1] &= (uint8_t)((1u << (unsigned)(dims % 8u)) - 1u);
        break;
    }
    w->len += data_len;
    w_end_value(w);
    return CDB_OK;
}

cdb_status cdb_value_write_encoded(cdb_value_writer *w, const void *value, size_t len)
{
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if (value == NULL || len == 0)
        return w_fail(w, CDB_E_INVALID_ARG);
    if ((s = cdb_value_validate((const uint8_t *)value, len, w->order, w->decimal_mode, 0)) != CDB_OK)
        return w_fail(w, s);
    if ((s = w_room(w, len)) != CDB_OK)
        return s;
    w_put(w, value, len);
    w_end_value(w);
    return CDB_OK;
}

/* Opens an array or object: writes only its tag for now. Its length and
 * count are not known until it ends, so the end functions slide the body
 * up to make room for them. */
static cdb_status w_begin_container(cdb_value_writer *w, uint8_t tag)
{
    cdb_value_frame *f;
    cdb_status s = w_begin_value(w);
    if (s != CDB_OK)
        return s;
    if (w->depth >= CDB_VALUE_MAX_DEPTH)
        return w_fail(w, CDB_E_DEPTH);
    if ((s = w_room(w, 1)) != CDB_OK)
        return s;
    f = &w->frames[w->depth++];
    memset(f, 0, sizeof *f);
    f->kind = tag;
    f->start = w->len;
    f->scratch_base = w->scratch_top;
    w_put_byte(w, tag);
    return CDB_OK;
}

cdb_status cdb_value_begin_array(cdb_value_writer *w)
{
    return w_begin_container(w, CDB_TAG_ARRAY);
}

cdb_status cdb_value_begin_object(cdb_value_writer *w)
{
    return w_begin_container(w, CDB_TAG_OBJECT);
}

static cdb_value_frame *w_top(cdb_value_writer *w, uint8_t kind)
{
    if (w->depth == 0 || w->frames[w->depth - 1].kind != kind)
        return NULL;
    return &w->frames[w->depth - 1];
}

/* Closes the innermost array: the body (the elements, already written
 * after the tag) moves up to make room for the byte length and element
 * count varints, which are written in the gap. */
cdb_status cdb_value_end_array(cdb_value_writer *w)
{
    cdb_value_frame *f;
    size_t body, header, total;
    if (w->error != CDB_OK)
        return w->error;
    if ((f = w_top(w, CDB_TAG_ARRAY)) == NULL)
        return w_fail(w, CDB_E_STATE);
    body = w->len - (f->start + 1);
    total = cdb_varint_size(f->count) + body;
    header = cdb_varint_size(total) + cdb_varint_size(f->count);
    if (w_room(w, header) != CDB_OK)
        return w->error;
    memmove(w->buf + f->start + 1 + header, w->buf + f->start + 1, body);
    w->len = f->start + 1;
    w_put_varint(w, total);
    w_put_varint(w, f->count);
    w->len += body;
    w->depth--;
    w_end_value(w);
    return CDB_OK;
}

/* Staged directory entry i of an open object: 16 bytes at the top of the
 * buffer (field ID, value offset, both in host order: scratch only). Entry
 * 0 is nearest the object's scratch_base; later entries lie below it. */
static void scratch_entry(const cdb_value_writer *w, const cdb_value_frame *f, uint64_t i,
                          uint64_t *field_id, uint64_t *offset)
{
    const uint8_t *e = w->buf + f->scratch_base - SCRATCH_ENTRY_SIZE * (size_t)(i + 1);
    memcpy(field_id, e, 8);
    memcpy(offset, e + 8, 8);
}

/* Starts a field: stages (field ID, offset of its value within the
 * object's values area) for the directory. */
cdb_status cdb_value_field(cdb_value_writer *w, uint64_t field_id)
{
    cdb_value_frame *f;
    uint64_t offset;
    if (w->error != CDB_OK)
        return w->error;
    if ((f = w_top(w, CDB_TAG_OBJECT)) == NULL || f->expect_value)
        return w_fail(w, CDB_E_STATE);
    if (f->count > 0 && field_id <= f->last_field)
        return w_fail(w, CDB_E_NOT_CANONICAL);
    if (w_room(w, SCRATCH_ENTRY_SIZE) != CDB_OK)
        return w->error;
    offset = (uint64_t)(w->len - (f->start + 1));
    w->scratch_top -= SCRATCH_ENTRY_SIZE;
    memcpy(w->buf + w->scratch_top, &field_id, 8);
    memcpy(w->buf + w->scratch_top + 8, &offset, 8);
    f->expect_value = 1;
    f->last_field = field_id;
    return CDB_OK;
}

/* Closes the innermost object: picks the smallest directory widths, slides
 * the values up past the room for byte length, field count, width byte and
 * directory, fills those in from the staged entries, and releases the
 * staging space. */
cdb_status cdb_value_end_object(cdb_value_writer *w)
{
    cdb_value_frame *f;
    size_t body, dir, header, total;
    uint64_t i, max_id = 0, max_off = 0;
    unsigned id_w, off_w;
    uint8_t *out;
    if (w->error != CDB_OK)
        return w->error;
    if ((f = w_top(w, CDB_TAG_OBJECT)) == NULL || f->expect_value)
        return w_fail(w, CDB_E_STATE);
    /* Entries are staged in increasing field and offset order, so the last
     * one holds both maxima. */
    if (f->count > 0)
        scratch_entry(w, f, f->count - 1, &max_id, &max_off);
    id_w = width_for(max_id);
    off_w = width_for(max_off);
    if (f->count > (uint64_t)(SIZE_MAX / 16u))
        return w_fail(w, CDB_E_OVERFLOW);
    dir = (size_t)f->count * (id_w + off_w);
    body = w->len - (f->start + 1);
    total = cdb_varint_size(f->count) + 1 + dir + body;
    header = cdb_varint_size(total) + cdb_varint_size(f->count) + 1 + dir;
    if (w_room(w, header) != CDB_OK)
        return w->error;
    memmove(w->buf + f->start + 1 + header, w->buf + f->start + 1, body);
    w->len = f->start + 1;
    w_put_varint(w, total);
    w_put_varint(w, f->count);
    w_put_byte(w, (uint8_t)(width_code(id_w) | (unsigned)width_code(off_w) << 4));
    out = w->buf + w->len;
    for (i = 0; i < f->count; i++) {
        uint64_t id, off;
        scratch_entry(w, f, i, &id, &off);
        store_width(out, id, id_w, w->order);
        store_width(out + id_w, off, off_w, w->order);
        out += id_w + off_w;
    }
    w->len += dir + body;
    w->scratch_top = f->scratch_base;
    w->depth--;
    w_end_value(w);
    return CDB_OK;
}

/* ------------------------------------------------------------- reader */

/* Reads a varint at *p (bounded by end) and advances *p past it. */
static cdb_status read_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    size_t n;
    cdb_status s = cdb_varint_decode(*p, (size_t)(end - *p), v, &n);
    if (s == CDB_OK)
        *p += n;
    return s;
}

cdb_status cdb_value_read(const uint8_t *buf, size_t len, cdb_byte_order order, cdb_value *out)
{
    const uint8_t *p = buf + 1;
    const uint8_t *end = buf + len;
    const uint8_t *pend;
    uint64_t v;
    cdb_status s;

    if (buf == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    if (len < 1)
        return CDB_E_TRUNCATED;
    memset(out, 0, sizeof *out);
    out->tag = buf[0];
    out->start = buf;

    switch (buf[0]) {
    case CDB_TAG_NULL:
        out->type = CDB_TYPE_NULL;
        out->encoded_len = 1;
        return CDB_OK;
    case CDB_TAG_FALSE:
    case CDB_TAG_TRUE:
        out->type = CDB_TYPE_BOOL;
        out->as.boolean = buf[0] == CDB_TAG_TRUE;
        out->encoded_len = 1;
        return CDB_OK;
    case CDB_TAG_INT:
        if ((s = read_varint(&p, end, &v)) != CDB_OK)
            return s;
        out->type = CDB_TYPE_INT;
        out->as.i64 = cdb_zigzag_decode(v);
        out->encoded_len = (size_t)(p - buf);
        return CDB_OK;
    case CDB_TAG_F64: {
        uint64_t bits;
        if (len < 9)
            return CDB_E_TRUNCATED;
        bits = cdb_load_u64(p, order);
        memcpy(&out->as.f64, &bits, sizeof bits);
        out->type = CDB_TYPE_F64;
        out->encoded_len = 9;
        return CDB_OK;
    }
    case CDB_TAG_F32: {
        uint32_t bits;
        if (len < 5)
            return CDB_E_TRUNCATED;
        bits = cdb_load_u32(p, order);
        memcpy(&out->as.f32, &bits, sizeof bits);
        out->type = CDB_TYPE_F32;
        out->encoded_len = 5;
        return CDB_OK;
    }
    case CDB_TAG_TIMESTAMP:
        if (len < 10)
            return CDB_E_TRUNCATED;
        if (p[0] > (uint8_t)CDB_TS_NANOS)
            return CDB_E_MALFORMED;
        out->type = CDB_TYPE_TIMESTAMP;
        out->as.ts.precision = (cdb_ts_precision)p[0];
        out->as.ts.ticks = (int64_t)cdb_load_u64(p + 1, order);
        out->encoded_len = 10;
        return CDB_OK;
    case CDB_TAG_REF:
        if ((s = read_varint(&p, end, &out->as.ref.store_id)) != CDB_OK)
            return s;
        if ((s = read_varint(&p, end, &out->as.ref.entry_id)) != CDB_OK)
            return s;
        out->type = CDB_TYPE_REF;
        out->encoded_len = (size_t)(p - buf);
        return CDB_OK;
    default:
        break;
    }

    if (buf[0] < CDB_TAG_FIRST_VARIABLE)
        return CDB_E_MALFORMED; /* unassigned fixed-shape tag: cannot be skipped */

    /* Variable-size types (0x40 and up) all start with a byte length, so
     * the extent of the value is known before its type is examined; an
     * unknown tag is reported as CDB_TYPE_UNKNOWN and can be skipped. */
    if ((s = read_varint(&p, end, &v)) != CDB_OK)
        return s;
    if (v > (uint64_t)(end - p))
        return CDB_E_TRUNCATED;
    out->payload = p;
    out->payload_len = (size_t)v;
    out->encoded_len = (size_t)(p - buf) + (size_t)v;
    pend = p + v;

    switch (buf[0]) {
    case CDB_TAG_STRING:
        out->type = CDB_TYPE_STRING;
        return CDB_OK;
    case CDB_TAG_BINARY:
        out->type = CDB_TYPE_BINARY;
        return CDB_OK;
    case CDB_TAG_DECIMAL:
        if ((s = read_varint(&p, pend, &out->as.decimal.scale)) != CDB_OK)
            return s == CDB_E_TRUNCATED ? CDB_E_MALFORMED : s;
        if (p == pend)
            return CDB_E_MALFORMED;
        out->type = CDB_TYPE_DECIMAL;
        out->as.decimal.unscaled = p;
        out->as.decimal.unscaled_len = (size_t)(pend - p);
        return CDB_OK;
    case CDB_TAG_ARRAY:
        if ((s = read_varint(&p, pend, &out->as.array.count)) != CDB_OK)
            return s == CDB_E_TRUNCATED ? CDB_E_MALFORMED : s;
        out->type = CDB_TYPE_ARRAY;
        out->as.array.items = p;
        out->as.array.items_len = (size_t)(pend - p);
        return CDB_OK;
    case CDB_TAG_OBJECT: {
        uint8_t widths;
        unsigned entry;
        uint64_t count;
        if ((s = read_varint(&p, pend, &count)) != CDB_OK)
            return s == CDB_E_TRUNCATED ? CDB_E_MALFORMED : s;
        if (p == pend)
            return CDB_E_MALFORMED;
        widths = *p++;
        if (widths & 0xCCu)
            return CDB_E_MALFORMED;
        out->as.object.id_width = (uint8_t)(1u << (widths & 0x03u));
        out->as.object.off_width = (uint8_t)(1u << ((widths >> 4) & 0x03u));
        entry = (unsigned)out->as.object.id_width + out->as.object.off_width;
        if (count > (uint64_t)(pend - p) / entry)
            return CDB_E_MALFORMED;
        out->type = CDB_TYPE_OBJECT;
        out->as.object.count = count;
        out->as.object.directory = p;
        out->as.object.directory_len = (size_t)count * entry;
        out->as.object.values = p + out->as.object.directory_len;
        out->as.object.values_len = (size_t)(pend - out->as.object.values);
        return CDB_OK;
    }
    case CDB_TAG_VECTOR: {
        cdb_vector_elem elem;
        if (p == pend)
            return CDB_E_MALFORMED;
        elem = (cdb_vector_elem)*p++;
        if (vector_elem_size_bits(elem) == 0)
            return CDB_E_MALFORMED;
        if ((s = read_varint(&p, pend, &out->as.vector.dims)) != CDB_OK)
            return s == CDB_E_TRUNCATED ? CDB_E_MALFORMED : s;
        if (vector_data_len(elem, out->as.vector.dims) != (size_t)(pend - p))
            return CDB_E_MALFORMED;
        out->type = CDB_TYPE_VECTOR;
        out->as.vector.elem = elem;
        out->as.vector.data = p;
        out->as.vector.data_len = (size_t)(pend - p);
        return CDB_OK;
    }
    default:
        out->type = CDB_TYPE_UNKNOWN;
        return CDB_OK;
    }
}

cdb_status cdb_value_array_iter(const cdb_value *array, cdb_byte_order order, cdb_value_iter *it)
{
    if (array == NULL || it == NULL || array->type != CDB_TYPE_ARRAY)
        return CDB_E_INVALID_ARG;
    memset(it, 0, sizeof *it);
    it->pos = array->as.array.items;
    it->end = array->as.array.items + array->as.array.items_len;
    it->order = order;
    return CDB_OK;
}

cdb_status cdb_value_array_next(cdb_value_iter *it, cdb_value *item, int *done)
{
    cdb_status s;
    if (it->pos == it->end) {
        *done = 1;
        return CDB_OK;
    }
    *done = 0;
    if ((s = cdb_value_read(it->pos, (size_t)(it->end - it->pos), it->order, item)) != CDB_OK)
        return s;
    it->pos += item->encoded_len;
    return CDB_OK;
}

cdb_status cdb_value_object_iter(const cdb_value *object, cdb_byte_order order, cdb_value_iter *it)
{
    if (object == NULL || it == NULL || object->type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    memset(it, 0, sizeof *it);
    it->dir_pos = object->as.object.directory;
    it->dir_end = object->as.object.directory + object->as.object.directory_len;
    it->id_width = object->as.object.id_width;
    it->off_width = object->as.object.off_width;
    it->pos = object->as.object.values;
    it->end = object->as.object.values + object->as.object.values_len;
    it->order = order;
    return CDB_OK;
}

/* Reads the value at a directory offset; an offset outside the values
 * area is CDB_E_MALFORMED. */
static cdb_status object_value_at(const cdb_value_iter *it, uint64_t offset, cdb_value *value)
{
    if (offset >= (uint64_t)(it->end - it->pos))
        return CDB_E_MALFORMED;
    return cdb_value_read(it->pos + offset, (size_t)(it->end - it->pos) - (size_t)offset,
                          it->order, value);
}

cdb_status cdb_value_object_next(cdb_value_iter *it, uint64_t *field_id, cdb_value *value, int *done)
{
    uint64_t offset;
    if (it->dir_pos == it->dir_end) {
        *done = 1;
        return CDB_OK;
    }
    *done = 0;
    *field_id = load_width(it->dir_pos, it->id_width, it->order);
    offset = load_width(it->dir_pos + it->id_width, it->off_width, it->order);
    it->dir_pos += it->id_width + it->off_width;
    return object_value_at(it, offset, value);
}

/* Binary search over the fixed-width directory (spec 14.6.3). */
cdb_status cdb_value_object_get(const cdb_value *object, cdb_byte_order order,
                                uint64_t field_id, cdb_value *value, int *found)
{
    cdb_value_iter it;
    uint64_t lo = 0, hi;
    unsigned entry;
    cdb_status s;

    *found = 0;
    if ((s = cdb_value_object_iter(object, order, &it)) != CDB_OK)
        return s;
    entry = (unsigned)it.id_width + it.off_width;
    hi = object->as.object.count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = it.dir_pos + (size_t)mid * entry;
        uint64_t id = load_width(e, it.id_width, order);
        if (id == field_id) {
            *found = 1;
            return object_value_at(&it, load_width(e + it.id_width, it.off_width, order), value);
        }
        if (id < field_id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return CDB_OK;
}

cdb_status cdb_value_vector_f32(const cdb_value *vec, cdb_byte_order order, uint64_t index, float *out)
{
    uint32_t bits;
    if (vec == NULL || out == NULL || vec->type != CDB_TYPE_VECTOR || vec->as.vector.elem != CDB_VEC_F32)
        return CDB_E_INVALID_ARG;
    if (index >= vec->as.vector.dims)
        return CDB_E_INVALID_ARG;
    bits = cdb_load_u32(vec->as.vector.data + index * 4u, order);
    memcpy(out, &bits, sizeof bits);
    return CDB_OK;
}

/* ---------------------------------------------------------- validator
 *
 * validate_at reads one value, checks the canonical rules that apply to
 * its type, and recurses into containers. *consumed reports its encoded
 * length so a container can walk to the next element. */

typedef struct validate_ctx {
    cdb_byte_order order;
    cdb_decimal_mode decimal_mode;
    unsigned flags;
} validate_ctx;

static cdb_status validate_at(const validate_ctx *c, const uint8_t *buf, size_t len,
                              unsigned depth, size_t *consumed);

/* Float elements must already be canonical; binary vectors must have zero
 * unused bits. (Length was checked by the reader.) */
static cdb_status validate_vector(const validate_ctx *c, const cdb_value *v)
{
    const uint8_t *d = v->as.vector.data;
    uint64_t i;

    switch (v->as.vector.elem) {
    case CDB_VEC_F32:
        for (i = 0; i < v->as.vector.dims; i++) {
            uint32_t bits = cdb_load_u32(d + i * 4u, c->order);
            if (bits != f32_canonical(bits))
                return CDB_E_NOT_CANONICAL;
        }
        break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        for (i = 0; i < v->as.vector.dims; i++) {
            uint16_t bits = cdb_load_u16(d + i * 2u, c->order);
            if (bits != half_canonical(bits, v->as.vector.elem))
                return CDB_E_NOT_CANONICAL;
        }
        break;
    case CDB_VEC_I8:
        break;
    case CDB_VEC_BINARY:
        if (v->as.vector.dims % 8u) {
            uint8_t unused = (uint8_t)~((1u << (unsigned)(v->as.vector.dims % 8u)) - 1u);
            if (d[v->as.vector.data_len - 1] & unused)
                return CDB_E_NOT_CANONICAL;
        }
        break;
    }
    return CDB_OK;
}

/* The elements must fill the body exactly and number exactly `count`. */
static cdb_status validate_array(const validate_ctx *c, const cdb_value *v, unsigned depth)
{
    const uint8_t *p = v->as.array.items;
    size_t left = v->as.array.items_len;
    uint64_t n = 0;

    while (left > 0) {
        size_t used;
        cdb_status s = validate_at(c, p, left, depth + 1, &used);
        if (s != CDB_OK)
            return s;
        p += used;
        left -= used;
        n++;
    }
    return n == v->as.array.count ? CDB_OK : CDB_E_MALFORMED;
}

static cdb_status validate_object(const validate_ctx *c, const cdb_value *v, unsigned depth)
{
    const uint8_t *dp = v->as.object.directory;
    unsigned id_w = v->as.object.id_width;
    unsigned off_w = v->as.object.off_width;
    size_t expected = 0;
    uint64_t prev = 0, last_off = 0;
    uint64_t i;

    for (i = 0; i < v->as.object.count; i++) {
        uint64_t id = load_width(dp, id_w, c->order);
        uint64_t off = load_width(dp + id_w, off_w, c->order);
        size_t used;
        cdb_status s;
        dp += id_w + off_w;
        if (i > 0 && id <= prev)
            return CDB_E_NOT_CANONICAL; /* unsorted or duplicate field */
        prev = id;
        /* Values are contiguous and in directory order: no gaps, no overlap. */
        if (off != (uint64_t)expected)
            return CDB_E_NOT_CANONICAL;
        last_off = off;
        if (expected >= v->as.object.values_len)
            return CDB_E_MALFORMED;
        s = validate_at(c, v->as.object.values + expected, v->as.object.values_len - expected,
                        depth + 1, &used);
        if (s != CDB_OK)
            return s;
        expected += used;
    }
    /* Widths must be the smallest that fit (an empty object uses 1 and 1). */
    if (id_w != width_for(prev) || off_w != width_for(last_off))
        return CDB_E_NOT_CANONICAL;
    return expected == v->as.object.values_len ? CDB_OK : CDB_E_MALFORMED;
}

static cdb_status validate_at(const validate_ctx *c, const uint8_t *buf, size_t len,
                              unsigned depth, size_t *consumed)
{
    cdb_value v;
    cdb_status s;

    if (depth > CDB_VALUE_MAX_DEPTH)
        return CDB_E_DEPTH;
    if ((s = cdb_value_read(buf, len, c->order, &v)) != CDB_OK)
        return s;

    switch (v.type) {
    case CDB_TYPE_F64: {
        uint64_t bits = cdb_load_u64(buf + 1, c->order);
        if (bits != f64_canonical(bits))
            return CDB_E_NOT_CANONICAL;
        break;
    }
    case CDB_TYPE_F32: {
        uint32_t bits = cdb_load_u32(buf + 1, c->order);
        if (bits != f32_canonical(bits))
            return CDB_E_NOT_CANONICAL;
        break;
    }
    case CDB_TYPE_STRING:
        if (!cdb_value_utf8_valid(v.payload, v.payload_len))
            return CDB_E_MALFORMED;
        break;
    case CDB_TYPE_DECIMAL:
        if (!decimal_is_canonical(v.as.decimal.unscaled, v.as.decimal.unscaled_len))
            return CDB_E_NOT_CANONICAL;
        if (c->decimal_mode == CDB_DECIMAL_COMPACT
            && !decimal_fits_compact(v.as.decimal.unscaled, v.as.decimal.unscaled_len))
            return CDB_E_OVERFLOW;
        break;
    case CDB_TYPE_ARRAY:
        if (depth + 1 > CDB_VALUE_MAX_DEPTH)
            return CDB_E_DEPTH;
        if ((s = validate_array(c, &v, depth)) != CDB_OK)
            return s;
        break;
    case CDB_TYPE_OBJECT:
        if (depth + 1 > CDB_VALUE_MAX_DEPTH)
            return CDB_E_DEPTH;
        if ((s = validate_object(c, &v, depth)) != CDB_OK)
            return s;
        break;
    case CDB_TYPE_VECTOR:
        if ((s = validate_vector(c, &v)) != CDB_OK)
            return s;
        break;
    case CDB_TYPE_UNKNOWN:
        if (!(c->flags & CDB_VALIDATE_ALLOW_UNKNOWN))
            return CDB_E_UNSUPPORTED;
        break;
    default:
        break;
    }
    *consumed = v.encoded_len;
    return CDB_OK;
}

/* The whole buffer must be exactly one value: trailing bytes are
 * CDB_E_MALFORMED. Unknown variable-size types are CDB_E_UNSUPPORTED
 * unless CDB_VALIDATE_ALLOW_UNKNOWN is given. */
cdb_status cdb_value_validate(const uint8_t *buf, size_t len, cdb_byte_order order,
                              cdb_decimal_mode decimal_mode, unsigned flags)
{
    validate_ctx c;
    size_t used;
    cdb_status s;

    if (buf == NULL)
        return CDB_E_INVALID_ARG;
    c.order = order;
    c.decimal_mode = decimal_mode;
    c.flags = flags;
    if ((s = validate_at(&c, buf, len, 0, &used)) != CDB_OK)
        return s;
    return used == len ? CDB_OK : CDB_E_MALFORMED;
}
