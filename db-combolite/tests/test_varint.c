#include "unity.h"
#include "cdb_varint.h"

void setUp(void) {}
void tearDown(void) {}

static void roundtrip(uint64_t v, size_t expected_len)
{
    uint8_t buf[CDB_VARINT_MAX_LEN];
    uint64_t out = 0;
    size_t n = cdb_varint_encode(v, buf);
    size_t used = 0;

    TEST_ASSERT_EQUAL_size_t(expected_len, n);
    TEST_ASSERT_EQUAL_size_t(n, cdb_varint_size(v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_varint_decode(buf, n, &out, &used));
    TEST_ASSERT_EQUAL_UINT64(v, out);
    TEST_ASSERT_EQUAL_size_t(n, used);
}

static void test_roundtrip_boundaries(void)
{
    roundtrip(0, 1);
    roundtrip(1, 1);
    roundtrip(127, 1);
    roundtrip(128, 2);
    roundtrip(16383, 2);
    roundtrip(16384, 3);
    roundtrip(UINT64_C(0xFFFFFFFF), 5);
    roundtrip(UINT64_MAX, 10);
}

static void test_known_encoding(void)
{
    uint8_t buf[CDB_VARINT_MAX_LEN];
    size_t n = cdb_varint_encode(300, buf);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_HEX8(0xAC, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[1]);
}

static void test_rejects_non_shortest(void)
{
    const uint8_t padded_zero[] = { 0x80, 0x00 };
    const uint8_t padded_one[] = { 0x81, 0x80, 0x00 };
    uint64_t v;
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_varint_decode(padded_zero, 2, &v, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_varint_decode(padded_one, 3, &v, &n));
}

static void test_rejects_truncated_and_overflow(void)
{
    const uint8_t truncated[] = { 0x80, 0x80 };
    const uint8_t too_big[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 };
    const uint8_t too_long[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x81, 0x01 };
    uint64_t v;
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_varint_decode(truncated, 2, &v, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_varint_decode(too_big, 10, &v, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_varint_decode(too_long, 11, &v, &n));
}

static void test_zigzag(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, cdb_zigzag_encode(0));
    TEST_ASSERT_EQUAL_UINT64(1, cdb_zigzag_encode(-1));
    TEST_ASSERT_EQUAL_UINT64(2, cdb_zigzag_encode(1));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, cdb_zigzag_encode(INT64_MIN));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX - 1, cdb_zigzag_encode(INT64_MAX));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, cdb_zigzag_decode(UINT64_MAX));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, cdb_zigzag_decode(UINT64_MAX - 1));
    TEST_ASSERT_EQUAL_INT64(-1, cdb_zigzag_decode(1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_boundaries);
    RUN_TEST(test_known_encoding);
    RUN_TEST(test_rejects_non_shortest);
    RUN_TEST(test_rejects_truncated_and_overflow);
    RUN_TEST(test_zigzag);
    return UNITY_END();
}
