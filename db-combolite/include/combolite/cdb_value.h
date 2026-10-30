/* Shared value encoding (spec 14.6).
 *
 * Every value is a 1-byte type tag followed by its payload. Tags 0x00-0x3F
 * are fixed-shape types every reader knows; tags 0x40-0xFF always carry a
 * varint length, so readers can skip variable types they do not recognise.
 *
 * Nothing here allocates. The writer encodes into a caller-supplied buffer;
 * the reader returns views that point into the caller's buffer. */
#ifndef COMBOLITE_CDB_VALUE_H
#define COMBOLITE_CDB_VALUE_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

#define CDB_VALUE_MAX_DEPTH 32 /* deepest nesting of arrays and objects */

/* On-disk type tags (spec 14.6.1). */
enum {
    CDB_TAG_NULL = 0x00,
    CDB_TAG_FALSE = 0x01,
    CDB_TAG_TRUE = 0x02,
    CDB_TAG_INT = 0x03,
    CDB_TAG_F64 = 0x04,
    CDB_TAG_F32 = 0x05,
    CDB_TAG_TIMESTAMP = 0x06,
    CDB_TAG_REF = 0x07,
    CDB_TAG_STRING = 0x40,
    CDB_TAG_BINARY = 0x41,
    CDB_TAG_DECIMAL = 0x42,
    CDB_TAG_ARRAY = 0x43,
    CDB_TAG_OBJECT = 0x44,
    CDB_TAG_VECTOR = 0x45,
    CDB_TAG_FIRST_VARIABLE = 0x40
};

/* Decoded types, as reported by the reader (both boolean tags map to
 * CDB_TYPE_BOOL). */
typedef enum cdb_value_type {
    CDB_TYPE_NULL,
    CDB_TYPE_BOOL,
    CDB_TYPE_INT,
    CDB_TYPE_F64,
    CDB_TYPE_F32,
    CDB_TYPE_TIMESTAMP,
    CDB_TYPE_REF,
    CDB_TYPE_STRING,
    CDB_TYPE_BINARY,
    CDB_TYPE_DECIMAL,
    CDB_TYPE_ARRAY,
    CDB_TYPE_OBJECT,
    CDB_TYPE_VECTOR,
    CDB_TYPE_UNKNOWN /* a variable-size tag this build does not know; skippable */
} cdb_value_type;

typedef enum cdb_ts_precision {
    CDB_TS_SECONDS = 0,
    CDB_TS_MILLIS = 1,
    CDB_TS_MICROS = 2,
    CDB_TS_NANOS = 3
} cdb_ts_precision;

typedef enum cdb_vector_elem {
    CDB_VEC_F32 = 0,
    CDB_VEC_F16 = 1,
    CDB_VEC_BF16 = 2,
    CDB_VEC_I8 = 3,
    CDB_VEC_BINARY = 4
} cdb_vector_elem;

/* Decimal mode for a file (spec 14.6.4). */
typedef enum cdb_decimal_mode {
    CDB_DECIMAL_UNBOUNDED = 0,
    CDB_DECIMAL_COMPACT = 1 /* at most 38 digits; fits in 128 bits */
} cdb_decimal_mode;

/* ---------------------------------------------------------------- writer
 *
 * Usage: init the writer over a buffer, write exactly one root value (which
 * may be an array or object containing others), then read the length with
 * cdb_value_writer_len. Inside an object, call cdb_value_field before each
 * value. Every call returns the writer's error, which is sticky, so a
 * sequence of calls can be checked once at the end (w.error):
 *   CDB_E_BUFFER_TOO_SMALL  the buffer is full
 *   CDB_E_STATE             a call out of order (a value with no field in an
 *                           object, a second root value, an unmatched end)
 *   CDB_E_NOT_CANONICAL     field IDs not strictly increasing
 *   CDB_E_DEPTH             nesting deeper than CDB_VALUE_MAX_DEPTH
 *   CDB_E_OVERFLOW          a number or size beyond what the encoding allows
 *                           (or a decimal too large for compact mode)
 *   CDB_E_INVALID_ARG       bad input: a string that is not UTF-8, a NULL
 *                           pointer with a length, a bad precision or shape
 * cdb_value_write_encoded also passes on the validator's errors.
 * The buffer belongs to the caller; the writer only keeps a pointer to it. */

/* One open array or object (internal to the writer). */
typedef struct cdb_value_frame {
    uint8_t kind;          /* CDB_TAG_ARRAY or CDB_TAG_OBJECT */
    uint8_t expect_value;  /* object: a field ID was written, its value is due */
    size_t start;          /* offset of the container's tag byte */
    uint64_t count;
    uint64_t last_field;
    size_t scratch_base;   /* scratch_top when the container began */
} cdb_value_frame;

/* Containers are built in place. While an object is open, its field
 * directory is staged at the top of the buffer (growing downwards), so the
 * buffer needs room for the finished value plus 16 bytes per open field. */
typedef struct cdb_value_writer {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t scratch_top;
    cdb_byte_order order;
    cdb_decimal_mode decimal_mode;
    unsigned depth;
    int root_done;
    cdb_status error; /* sticky: once set, every call returns it */
    cdb_value_frame frames[CDB_VALUE_MAX_DEPTH];
} cdb_value_writer;

/* Starts a writer over buf (cap bytes). order is the byte order of the file
 * the value is for; decimal_mode limits which decimals are accepted. */
void cdb_value_writer_init(cdb_value_writer *w, uint8_t *buf, size_t cap,
                           cdb_byte_order order, cdb_decimal_mode decimal_mode);

/* Number of encoded bytes once the root value is complete. */
size_t cdb_value_writer_len(const cdb_value_writer *w);

cdb_status cdb_value_write_null(cdb_value_writer *w);
cdb_status cdb_value_write_bool(cdb_value_writer *w, int value);
cdb_status cdb_value_write_int(cdb_value_writer *w, int64_t value);
/* NaN is stored as the canonical NaN and -0.0 as 0.0 (spec 14.6.5). */
cdb_status cdb_value_write_f64(cdb_value_writer *w, double value);
cdb_status cdb_value_write_f32(cdb_value_writer *w, float value);
cdb_status cdb_value_write_timestamp(cdb_value_writer *w, cdb_ts_precision precision, int64_t ticks);
cdb_status cdb_value_write_ref(cdb_value_writer *w, uint64_t store_id, uint64_t entry_id);
/* Rejects text that is not well-formed UTF-8. */
cdb_status cdb_value_write_string(cdb_value_writer *w, const char *utf8, size_t len);
cdb_status cdb_value_write_binary(cdb_value_writer *w, const void *data, size_t len);
/* unscaled_be: the unscaled integer in shortest big-endian two's complement. */
cdb_status cdb_value_write_decimal(cdb_value_writer *w, uint64_t scale,
                                   const uint8_t *unscaled_be, size_t unscaled_len);
cdb_status cdb_value_write_decimal_i64(cdb_value_writer *w, uint64_t scale, int64_t unscaled);
/* elems points to host-order elements: float for F32, uint16_t bit patterns
 * for F16/BF16, int8_t for I8, and packed bits (LSB first) for BINARY. */
cdb_status cdb_value_write_vector(cdb_value_writer *w, cdb_vector_elem elem,
                                  uint64_t dims, const void *elems);

/* data: the elements already in the writer's byte order and canonical form,
 * as found in an encoded vector (cdb_value.as.vector.data). */
cdb_status cdb_value_write_vector_raw(cdb_value_writer *w, cdb_vector_elem elem, uint64_t dims,
                                      const void *data, size_t len);

/* Copies one value that is already encoded (for example, read from another
 * document). It is validated against the writer's decimal mode first. */
cdb_status cdb_value_write_encoded(cdb_value_writer *w, const void *value, size_t len);

cdb_status cdb_value_begin_array(cdb_value_writer *w);
cdb_status cdb_value_end_array(cdb_value_writer *w);
cdb_status cdb_value_begin_object(cdb_value_writer *w);
/* Field IDs must be written in strictly increasing order. */
cdb_status cdb_value_field(cdb_value_writer *w, uint64_t field_id);
cdb_status cdb_value_end_object(cdb_value_writer *w);

/* ---------------------------------------------------------------- reader
 *
 * The reader never copies: every pointer in a cdb_value points into the
 * buffer it was read from, which must stay unchanged while the value is in
 * use. Malformed input gives CDB_E_MALFORMED or CDB_E_TRUNCATED. */

/* One decoded value. Which member of `as` is valid depends on type. */
typedef struct cdb_value {
    cdb_value_type type;
    uint8_t tag;
    const uint8_t *start;   /* the value's first byte (its tag) */
    size_t encoded_len;     /* bytes taken by the whole value, tag included */
    const uint8_t *payload; /* variable types: bytes after the length prefix */
    size_t payload_len;
    union {
        int boolean;
        int64_t i64;
        double f64;
        float f32;
        struct { cdb_ts_precision precision; int64_t ticks; } ts;
        struct { uint64_t store_id; uint64_t entry_id; } ref;
        struct { uint64_t scale; const uint8_t *unscaled; size_t unscaled_len; } decimal;
        struct { uint64_t count; const uint8_t *items; size_t items_len; } array;
        struct {
            uint64_t count;
            uint8_t id_width;  /* bytes per directory field ID: 1, 2, 4 or 8 */
            uint8_t off_width; /* bytes per directory value offset */
            const uint8_t *directory; size_t directory_len;
            const uint8_t *values; size_t values_len;
        } object;
        struct { cdb_vector_elem elem; uint64_t dims; const uint8_t *data; size_t data_len; } vector;
    } as;
} cdb_value;

/* Decodes the value at the start of buf. Checks bounds and the shape of
 * this value only; use cdb_value_validate for a full canonical check. */
cdb_status cdb_value_read(const uint8_t *buf, size_t len, cdb_byte_order order, cdb_value *out);

/* Iterator over an array's items or an object's fields, in stored order
 * (objects: ascending field ID). */
typedef struct cdb_value_iter {
    const uint8_t *pos;
    const uint8_t *end;
    cdb_byte_order order;
    /* objects only */
    const uint8_t *dir_pos;
    const uint8_t *dir_end;
    uint8_t id_width;
    uint8_t off_width;
} cdb_value_iter;

cdb_status cdb_value_array_iter(const cdb_value *array, cdb_byte_order order, cdb_value_iter *it);
/* Returns CDB_OK with *done = 1 once the array is exhausted. */
cdb_status cdb_value_array_next(cdb_value_iter *it, cdb_value *item, int *done);

cdb_status cdb_value_object_iter(const cdb_value *object, cdb_byte_order order, cdb_value_iter *it);
cdb_status cdb_value_object_next(cdb_value_iter *it, uint64_t *field_id, cdb_value *value, int *done);
/* Binary-searches the fixed-width directory; never decodes other values.
 * *found is 0 when the field is absent. */
cdb_status cdb_value_object_get(const cdb_value *object, cdb_byte_order order,
                                uint64_t field_id, cdb_value *value, int *found);

/* Element bytes of a vector, converted to host order where needed.
 * Only F32 vectors are supported; another element type, or an index past
 * the last element, is CDB_E_INVALID_ARG. */
cdb_status cdb_value_vector_f32(const cdb_value *vec, cdb_byte_order order, uint64_t index, float *out);

#define CDB_VALIDATE_ALLOW_UNKNOWN 0x1u /* accept, and skip, unknown variable-size tags */

/* Checks that buf[0..len) holds exactly one value in canonical form,
 * recursing into containers up to CDB_VALUE_MAX_DEPTH. */
cdb_status cdb_value_validate(const uint8_t *buf, size_t len, cdb_byte_order order,
                              cdb_decimal_mode decimal_mode, unsigned flags);

/* Whether s[0..len) is well-formed UTF-8 (no overlongs, surrogates or code
 * points past U+10FFFF): the check strings get (14.6). */
int cdb_value_utf8_valid(const uint8_t *s, size_t len);

/* The per-type canonical checks cdb_value_validate applies to a decimal's
 * unscaled integer and to a vector's float elements (spec 14.6.5), exposed
 * for callers that hold a value's pieces without its tag byte (such as a
 * column chunk's decoder, cdb_colenc.c) and so cannot call
 * cdb_value_validate itself.
 *
 * unscaled_be: the shortest big-endian two's complement form, as passed to
 * cdb_value_write_decimal. cdb_value_decimal_fits_compact assumes
 * unscaled_be is already canonical, same as the writer. */
int cdb_value_decimal_is_canonical(const uint8_t *unscaled_be, size_t len);
int cdb_value_decimal_fits_compact(const uint8_t *unscaled_be, size_t len);

/* The canonical form of a float bit pattern: every NaN becomes the one
 * quiet NaN, and -0 becomes +0. A pattern is canonical exactly when it
 * equals what this returns. cdb_value_half_canonical covers both F16 and
 * BF16 vector elements (elem says which). */
uint32_t cdb_value_f32_canonical(uint32_t bits);
uint16_t cdb_value_half_canonical(uint16_t bits, cdb_vector_elem elem);

#endif
