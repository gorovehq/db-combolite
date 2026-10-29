#include <math.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"

static uint8_t buf[512];
static cdb_value_writer w;

void setUp(void)
{
    memset(buf, 0xAA, sizeof buf);
    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED);
}

void tearDown(void) {}

static void assert_valid(cdb_byte_order order)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(buf, cdb_value_writer_len(&w), order,
                                                     CDB_DECIMAL_UNBOUNDED, 0));
}

static cdb_value read_root(cdb_byte_order order)
{
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf, cdb_value_writer_len(&w), order, &v));
    TEST_ASSERT_EQUAL_size_t(cdb_value_writer_len(&w), v.encoded_len);
    return v;
}

/* ------------------------------------------------------------ scalars */

static void test_null_and_bools(void)
{
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_array(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_null(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_bool(&w, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_bool(&w, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_array(&w));
    assert_valid(CDB_ORDER_LITTLE);

    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_ARRAY, v.type);
    TEST_ASSERT_EQUAL_UINT64(3, v.as.array.count);
    TEST_ASSERT_EQUAL_HEX8(CDB_TAG_NULL, v.as.array.items[0]);
    TEST_ASSERT_EQUAL_HEX8(CDB_TAG_TRUE, v.as.array.items[1]);
    TEST_ASSERT_EQUAL_HEX8(CDB_TAG_FALSE, v.as.array.items[2]);
}

static void test_int_roundtrip_extremes(void)
{
    const int64_t values[] = { 0, 1, -1, 63, -64, 64, INT64_MAX, INT64_MIN };
    size_t i;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
        cdb_value v;
        setUp();
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, values[i]));
        assert_valid(CDB_ORDER_LITTLE);
        v = read_root(CDB_ORDER_LITTLE);
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, v.type);
        TEST_ASSERT_EQUAL_INT64(values[i], v.as.i64);
    }
}

static void test_small_int_is_two_bytes(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, -3));
    TEST_ASSERT_EQUAL_size_t(2, cdb_value_writer_len(&w));
    TEST_ASSERT_EQUAL_HEX8(CDB_TAG_INT, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(5, buf[1]); /* zigzag(-3) = 5 */
}

static void test_floats_in_both_byte_orders(void)
{
    cdb_byte_order orders[2] = { CDB_ORDER_LITTLE, CDB_ORDER_BIG };
    int i;
    for (i = 0; i < 2; i++) {
        cdb_value v;
        cdb_value_writer_init(&w, buf, sizeof buf, orders[i], CDB_DECIMAL_UNBOUNDED);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f64(&w, 3.14159265358979));
        assert_valid(orders[i]);
        v = read_root(orders[i]);
        TEST_ASSERT_EQUAL_DOUBLE(3.14159265358979, v.as.f64);

        cdb_value_writer_init(&w, buf, sizeof buf, orders[i], CDB_DECIMAL_UNBOUNDED);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f32(&w, -2.5f));
        assert_valid(orders[i]);
        v = read_root(orders[i]);
        TEST_ASSERT_EQUAL_FLOAT(-2.5f, v.as.f32);
    }
    /* The same value encodes to different bytes in each order. */
    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_BIG, CDB_DECIMAL_UNBOUNDED);
    cdb_value_write_f32(&w, 1.0f);
    TEST_ASSERT_EQUAL_HEX8(0x3F, buf[1]);
}

static void test_float_canonicalisation(void)
{
    uint64_t nan_bits = UINT64_C(0x7FF0000000000001); /* a signalling NaN */
    uint64_t bits;
    double nan_value;
    memcpy(&nan_value, &nan_bits, sizeof nan_value);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f64(&w, nan_value));
    memcpy(&bits, buf + 1, 8);
    TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x7FF8000000000000), bits);
    assert_valid(CDB_ORDER_LITTLE);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f64(&w, -0.0));
    memcpy(&bits, buf + 1, 8);
    TEST_ASSERT_EQUAL_HEX64(0, bits);
}

static void test_rejects_non_canonical_floats(void)
{
    const uint8_t neg_zero_f64[] = { CDB_TAG_F64, 0, 0, 0, 0, 0, 0, 0, 0x80 };
    const uint8_t other_nan_f32[] = { CDB_TAG_F32, 0x01, 0x00, 0x80, 0x7F };
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(neg_zero_f64, sizeof neg_zero_f64,
                                                                  CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(other_nan_f32, sizeof other_nan_f32,
                                                                  CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED, 0));
}

static void test_timestamp_and_ref(void)
{
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_timestamp(&w, CDB_TS_MICROS, -1234567));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_TIMESTAMP, v.type);
    TEST_ASSERT_EQUAL_INT(CDB_TS_MICROS, v.as.ts.precision);
    TEST_ASSERT_EQUAL_INT64(-1234567, v.as.ts.ticks);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_ref(&w, 7, UINT64_C(900000000000)));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_REF, v.type);
    TEST_ASSERT_EQUAL_UINT64(7, v.as.ref.store_id);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(900000000000), v.as.ref.entry_id);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_value_write_timestamp(&w, (cdb_ts_precision)9, 0));
}

/* ------------------------------------------------------ strings, bytes */

static void test_string_and_binary(void)
{
    const char *text = "ADR-0042 \xC3\xA9t\xC3\xA9"; /* contains "été" */
    const uint8_t blob[] = { 0, 1, 2, 0xFF };
    cdb_value v;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&w, text, strlen(text)));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, v.type);
    TEST_ASSERT_EQUAL_size_t(strlen(text), v.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(text, v.payload, v.payload_len);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_binary(&w, blob, sizeof blob));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_BINARY, v.type);
    TEST_ASSERT_EQUAL_MEMORY(blob, v.payload, sizeof blob);
}

static void test_rejects_bad_utf8(void)
{
    const char overlong[] = { (char)0xC0, (char)0x80 };
    const char surrogate[] = { (char)0xED, (char)0xA0, (char)0x80 };
    const char truncated[] = { (char)0xE2, (char)0x82 };
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_value_write_string(&w, overlong, sizeof overlong));
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_value_write_string(&w, surrogate, sizeof surrogate));
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_value_write_string(&w, truncated, sizeof truncated));
}

static void test_validator_rejects_bad_utf8(void)
{
    const uint8_t encoded[] = { CDB_TAG_STRING, 2, 0xC0, 0x80 };
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_value_validate(encoded, sizeof encoded, CDB_ORDER_LITTLE,
                                                              CDB_DECIMAL_UNBOUNDED, 0));
}

/* ----------------------------------------------------------- decimals */

static void test_decimal_i64_is_shortest(void)
{
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_decimal_i64(&w, 2, 12345)); /* 123.45 */
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_DECIMAL, v.type);
    TEST_ASSERT_EQUAL_UINT64(2, v.as.decimal.scale);
    TEST_ASSERT_EQUAL_size_t(2, v.as.decimal.unscaled_len);
    TEST_ASSERT_EQUAL_HEX8(0x30, v.as.decimal.unscaled[0]);
    TEST_ASSERT_EQUAL_HEX8(0x39, v.as.decimal.unscaled[1]);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_decimal_i64(&w, 0, -1));
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_size_t(1, v.as.decimal.unscaled_len);
    TEST_ASSERT_EQUAL_HEX8(0xFF, v.as.decimal.unscaled[0]);

    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_decimal_i64(&w, 0, 128));
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_size_t(2, v.as.decimal.unscaled_len); /* 0x00 0x80 keeps it positive */
}

static void test_decimal_rejects_padding(void)
{
    const uint8_t padded[] = { 0x00, 0x05 };
    const uint8_t encoded[] = { CDB_TAG_DECIMAL, 3, 0x00, 0xFF, 0x80 };
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_value_write_decimal(&w, 0, padded, sizeof padded));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(encoded, sizeof encoded, CDB_ORDER_LITTLE,
                                                                  CDB_DECIMAL_UNBOUNDED, 0));
}

static void test_decimal_compact_limit(void)
{
    /* 10^38 - 1 is the largest compact magnitude; 10^38 itself overflows. */
    const uint8_t max_ok[] = { 0x4B, 0x3B, 0x4C, 0xA8, 0x5A, 0x86, 0xC4, 0x7A,
                               0x09, 0x8A, 0x22, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t too_big[] = { 0x4B, 0x3B, 0x4C, 0xA8, 0x5A, 0x86, 0xC4, 0x7A,
                                0x09, 0x8A, 0x22, 0x40, 0x00, 0x00, 0x00, 0x00 };
    const uint8_t neg_too_big[] = { 0xB4, 0xC4, 0xB3, 0x57, 0xA5, 0x79, 0x3B, 0x85,
                                    0xF6, 0x75, 0xDD, 0xC0, 0x00, 0x00, 0x00, 0x00 }; /* -10^38 */

    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_decimal(&w, 0, max_ok, sizeof max_ok));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(buf, cdb_value_writer_len(&w), CDB_ORDER_LITTLE,
                                                     CDB_DECIMAL_COMPACT, 0));

    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_value_write_decimal(&w, 0, too_big, sizeof too_big));
    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_value_write_decimal(&w, 0, neg_too_big, sizeof neg_too_big));

    /* Unbounded mode accepts it, but a compact reader rejects the result. */
    cdb_value_writer_init(&w, buf, sizeof buf, CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_decimal(&w, 0, too_big, sizeof too_big));
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_value_validate(buf, cdb_value_writer_len(&w), CDB_ORDER_LITTLE,
                                                             CDB_DECIMAL_COMPACT, 0));
}

/* ------------------------------------------------------------ vectors */

static void test_vector_f32(void)
{
    const float elems[3] = { 1.0f, -0.0f, 0.5f };
    cdb_value v;
    float out;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_vector(&w, CDB_VEC_F32, 3, elems));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_VECTOR, v.type);
    TEST_ASSERT_EQUAL_UINT64(3, v.as.vector.dims);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_vector_f32(&v, CDB_ORDER_LITTLE, 2, &out));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, out);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_vector_f32(&v, CDB_ORDER_LITTLE, 1, &out));
    TEST_ASSERT_FALSE(signbit(out)); /* -0.0 was normalised */
}

static void test_vector_binary_padding(void)
{
    const uint8_t bits[2] = { 0xFF, 0xFF }; /* 10 dims: 6 unused high bits in byte 1 */
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_vector(&w, CDB_VEC_BINARY, 10, bits));
    assert_valid(CDB_ORDER_LITTLE);
    v = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_size_t(2, v.as.vector.data_len);
    TEST_ASSERT_EQUAL_HEX8(0x03, v.as.vector.data[1]);

    /* Hand-built vector with a stray padding bit is not canonical. */
    {
        const uint8_t bad[] = { CDB_TAG_VECTOR, 4, CDB_VEC_BINARY, 10, 0xFF, 0x07 };
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(bad, sizeof bad, CDB_ORDER_LITTLE,
                                                                      CDB_DECIMAL_UNBOUNDED, 0));
    }
}

static void test_vector_length_mismatch_is_malformed(void)
{
    const uint8_t bad[] = { CDB_TAG_VECTOR, 4, CDB_VEC_I8, 3, 1, 2 }; /* 3 dims, 2 bytes */
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_value_read(bad, sizeof bad, CDB_ORDER_LITTLE, &v));
}

/* --------------------------------------------------------- containers */

static void write_sample_object(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&w, "accepted", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_array(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_bool(&w, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_array(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 300));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_ref(&w, 2, 99));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_object(&w));
}

static void test_nested_object_roundtrip(void)
{
    cdb_value root, field, item;
    cdb_value_iter it;
    int found, done;

    write_sample_object();
    assert_valid(CDB_ORDER_LITTLE);
    root = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_OBJECT, root.type);
    TEST_ASSERT_EQUAL_UINT64(3, root.as.object.count);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&root, CDB_ORDER_LITTLE, 300, &field, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_REF, field.type);
    TEST_ASSERT_EQUAL_UINT64(99, field.as.ref.entry_id);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&root, CDB_ORDER_LITTLE, 3, &field, &found));
    TEST_ASSERT_FALSE(found);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&root, CDB_ORDER_LITTLE, 4, &field, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_ARRAY, field.type);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_iter(&field, CDB_ORDER_LITTLE, &it));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_next(&it, &item, &done));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL_INT64(10, item.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_next(&it, &item, &done));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_OBJECT, item.type);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_next(&it, &item, &done));
    TEST_ASSERT_TRUE(done);
}

static void test_object_iteration_order(void)
{
    cdb_value root, value;
    cdb_value_iter it;
    uint64_t id;
    uint64_t seen[3];
    int done, n = 0;

    write_sample_object();
    root = read_root(CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_iter(&root, CDB_ORDER_LITTLE, &it));
    for (;;) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_next(&it, &id, &value, &done));
        if (done)
            break;
        seen[n++] = id;
    }
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_UINT64(1, seen[0]);
    TEST_ASSERT_EQUAL_UINT64(4, seen[1]);
    TEST_ASSERT_EQUAL_UINT64(300, seen[2]);
}

static void test_identical_values_encode_identically(void)
{
    uint8_t first[512];
    size_t first_len;

    write_sample_object();
    first_len = cdb_value_writer_len(&w);
    memcpy(first, buf, first_len);
    setUp();
    write_sample_object();
    TEST_ASSERT_EQUAL_size_t(first_len, cdb_value_writer_len(&w));
    TEST_ASSERT_EQUAL_MEMORY(first, buf, first_len);
}

static void test_field_order_enforced(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&w, 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_null(&w));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_field(&w, 5));
    /* The error is sticky. */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_end_object(&w));
}

static void test_validator_rejects_unsorted_directory(void)
{
    /* object, 2 fields: dir {5:0, 2:1}, values null, null */
    const uint8_t unsorted[] = { CDB_TAG_OBJECT, 7, 2, 5, 0, 2, 1, CDB_TAG_NULL, CDB_TAG_NULL };
    const uint8_t gap[] = { CDB_TAG_OBJECT, 8, 2, 1, 0, 2, 2, CDB_TAG_NULL, CDB_TAG_NULL, CDB_TAG_NULL };
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(unsorted, sizeof unsorted, CDB_ORDER_LITTLE,
                                                                  CDB_DECIMAL_UNBOUNDED, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_value_validate(gap, sizeof gap, CDB_ORDER_LITTLE,
                                                                  CDB_DECIMAL_UNBOUNDED, 0));
}

static void test_writer_state_errors(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_value_end_array(&w));
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_value_write_int(&w, 1)); /* value without a field ID */
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_value_write_int(&w, 2)); /* second root value */
}

static void test_depth_limit(void)
{
    int i;
    for (i = 0; i < CDB_VALUE_MAX_DEPTH; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_array(&w));
    TEST_ASSERT_EQUAL_INT(CDB_E_DEPTH, cdb_value_begin_array(&w));
}

static void test_buffer_too_small(void)
{
    uint8_t tiny[4];
    cdb_value_writer_init(&w, tiny, sizeof tiny, CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_value_write_string(&w, "too long", 8));

    /* An object needs room for its staged directory too. */
    cdb_value_writer_init(&w, buf, 16, CDB_ORDER_LITTLE, CDB_DECIMAL_UNBOUNDED);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&w));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_value_field(&w, 1));
}

static void test_truncated_input(void)
{
    size_t len;
    cdb_value v;
    write_sample_object();
    len = cdb_value_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_value_read(buf, len - 1, CDB_ORDER_LITTLE, &v));
    TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, cdb_value_validate(buf, len - 1, CDB_ORDER_LITTLE,
                                                         CDB_DECIMAL_UNBOUNDED, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_value_validate(buf, len + 1, CDB_ORDER_LITTLE,
                                                              CDB_DECIMAL_UNBOUNDED, 0)); /* trailing byte */
}

/* --------------------------------------------------- forward compatibility */

static void test_unknown_variable_tag_is_skippable(void)
{
    /* array [ <unknown tag 0x90, 3 bytes>, int 1 ] */
    const uint8_t encoded[] = { CDB_TAG_ARRAY, 8, 2, 0x90, 3, 0xDE, 0xAD, 0xBE, CDB_TAG_INT, 2 };
    cdb_value root, item;
    cdb_value_iter it;
    int done;

    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_value_validate(encoded, sizeof encoded, CDB_ORDER_LITTLE,
                                                                CDB_DECIMAL_UNBOUNDED, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(encoded, sizeof encoded, CDB_ORDER_LITTLE,
                                                     CDB_DECIMAL_UNBOUNDED, CDB_VALIDATE_ALLOW_UNKNOWN));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(encoded, sizeof encoded, CDB_ORDER_LITTLE, &root));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_iter(&root, CDB_ORDER_LITTLE, &it));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_next(&it, &item, &done));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_UNKNOWN, item.type);
    TEST_ASSERT_EQUAL_size_t(5, item.encoded_len);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_array_next(&it, &item, &done));
    TEST_ASSERT_EQUAL_INT64(1, item.as.i64);
}

static void test_unknown_fixed_tag_is_malformed(void)
{
    const uint8_t encoded[] = { 0x20 };
    cdb_value v;
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_value_read(encoded, 1, CDB_ORDER_LITTLE, &v));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_and_bools);
    RUN_TEST(test_int_roundtrip_extremes);
    RUN_TEST(test_small_int_is_two_bytes);
    RUN_TEST(test_floats_in_both_byte_orders);
    RUN_TEST(test_float_canonicalisation);
    RUN_TEST(test_rejects_non_canonical_floats);
    RUN_TEST(test_timestamp_and_ref);
    RUN_TEST(test_string_and_binary);
    RUN_TEST(test_rejects_bad_utf8);
    RUN_TEST(test_validator_rejects_bad_utf8);
    RUN_TEST(test_decimal_i64_is_shortest);
    RUN_TEST(test_decimal_rejects_padding);
    RUN_TEST(test_decimal_compact_limit);
    RUN_TEST(test_vector_f32);
    RUN_TEST(test_vector_binary_padding);
    RUN_TEST(test_vector_length_mismatch_is_malformed);
    RUN_TEST(test_nested_object_roundtrip);
    RUN_TEST(test_object_iteration_order);
    RUN_TEST(test_identical_values_encode_identically);
    RUN_TEST(test_field_order_enforced);
    RUN_TEST(test_validator_rejects_unsorted_directory);
    RUN_TEST(test_writer_state_errors);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_buffer_too_small);
    RUN_TEST(test_truncated_input);
    RUN_TEST(test_unknown_variable_tag_is_skippable);
    RUN_TEST(test_unknown_fixed_tag_is_malformed);
    return UNITY_END();
}
