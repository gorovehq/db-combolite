#include "unity.h"
#include "cdb_endian.h"

void setUp(void) {}
void tearDown(void) {}

static void test_u32_layouts(void)
{
    uint8_t b[4];
    cdb_store_u32(b, 0x01020304u, CDB_ORDER_BIG);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04, b[3]);
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, cdb_load_u32(b, CDB_ORDER_BIG));
    cdb_store_u32(b, 0x01020304u, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_HEX8(0x04, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[3]);
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, cdb_load_u32(b, CDB_ORDER_LITTLE));
}

static void test_u16_and_u64_roundtrip(void)
{
    uint8_t b[8];
    cdb_store_u16(b, 0xBEEFu, CDB_ORDER_BIG);
    TEST_ASSERT_EQUAL_HEX16(0xBEEFu, cdb_load_u16(b, CDB_ORDER_BIG));
    cdb_store_u16(b, 0xBEEFu, CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_HEX16(0xBEEFu, cdb_load_u16(b, CDB_ORDER_LITTLE));
    cdb_store_u64(b, UINT64_C(0x0102030405060708), CDB_ORDER_BIG);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[0]);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x0102030405060708), cdb_load_u64(b, CDB_ORDER_BIG));
    cdb_store_u64(b, UINT64_C(0x0102030405060708), CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_HEX8(0x08, b[0]);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x0102030405060708), cdb_load_u64(b, CDB_ORDER_LITTLE));
}

static void test_byte_order_flag_bytes(void)
{
    /* The flag is a single byte, so it reads the same on every host (spec 14.1.2). */
    TEST_ASSERT_EQUAL_HEX8('L', (uint8_t)CDB_ORDER_LITTLE);
    TEST_ASSERT_EQUAL_HEX8('B', (uint8_t)CDB_ORDER_BIG);
    TEST_ASSERT_TRUE(cdb_host_byte_order() == CDB_ORDER_LITTLE || cdb_host_byte_order() == CDB_ORDER_BIG);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_u32_layouts);
    RUN_TEST(test_u16_and_u64_roundtrip);
    RUN_TEST(test_byte_order_flag_bytes);
    return UNITY_END();
}
