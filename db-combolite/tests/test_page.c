#include <string.h>

#include "unity.h"
#include "cdb_page.h"

void setUp(void) {}
void tearDown(void) {}

static cdb_page_header sample(void)
{
    cdb_page_header h;
    memset(&h, 0, sizeof h);
    h.type = CDB_PAGE_TYPE_DATA_TEST;
    h.layout_version = 1;
    h.flags = 0x02;
    h.key_version = 3;
    h.payload_len = 5;
    h.write_seq = UINT64_C(0x0102030405060708);
    h.logical_id = 1234;
    h.store_id = 7;
    h.domain_id = 9;
    h.extent_pos = 2;
    return h;
}

static void roundtrip(cdb_page_format fmt, size_t page_size, cdb_byte_order order)
{
    static uint8_t page[4096];
    cdb_page_header h = sample(), back;
    memset(page, 0, page_size);
    memcpy(page + cdb_page_header_size(fmt), "hello", 5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_page_seal(page, page_size, fmt, order, CDB_CHECKSUM_CRC32C, &h));
    TEST_ASSERT_EQUAL_HEX8(0xC0, page[0]);
    TEST_ASSERT_EQUAL_HEX8(0xDB, page[1]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_page_open(page, page_size, fmt, order, CDB_CHECKSUM_CRC32C, &back));
    TEST_ASSERT_EQUAL_UINT16(h.type, back.type);
    TEST_ASSERT_EQUAL_UINT64(h.write_seq, back.write_seq);
    TEST_ASSERT_EQUAL_UINT64(h.logical_id, back.logical_id);
    TEST_ASSERT_EQUAL_UINT64(h.store_id, back.store_id);
    TEST_ASSERT_EQUAL_UINT64(h.domain_id, back.domain_id);
    TEST_ASSERT_EQUAL_UINT32(h.payload_len, back.payload_len);
    TEST_ASSERT_EQUAL_UINT16(h.extent_pos, back.extent_pos);
    TEST_ASSERT_EQUAL_MEMORY("hello", page + cdb_page_header_size(fmt), 5);
}

static void test_sizes(void)
{
    TEST_ASSERT_EQUAL_size_t(56, cdb_page_header_size(CDB_PAGE_FMT_FULL64));
    TEST_ASSERT_EQUAL_size_t(40, cdb_page_header_size(CDB_PAGE_FMT_FULL32));
    TEST_ASSERT_EQUAL_size_t(28, cdb_page_header_size(CDB_PAGE_FMT_COMPACT));
    TEST_ASSERT_EQUAL_size_t(4096 - 72, cdb_page_capacity(CDB_PAGE_FMT_FULL64, 4096));
    TEST_ASSERT_EQUAL_size_t(512 - 36, cdb_page_capacity(CDB_PAGE_FMT_COMPACT, 512));
    TEST_ASSERT_EQUAL_size_t(0, cdb_page_capacity(CDB_PAGE_FMT_COMPACT, 2048)); /* compact is for small pages */
}

static void test_roundtrips(void)
{
    roundtrip(CDB_PAGE_FMT_FULL64, 4096, CDB_ORDER_LITTLE);
    roundtrip(CDB_PAGE_FMT_FULL64, 512, CDB_ORDER_BIG);
    roundtrip(CDB_PAGE_FMT_FULL32, 1024, CDB_ORDER_LITTLE);
    roundtrip(CDB_PAGE_FMT_COMPACT, 512, CDB_ORDER_BIG);
}

static void test_detects_damage(void)
{
    static uint8_t page[1024];
    cdb_page_header h = sample(), back;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_page_seal(page, sizeof page, CDB_PAGE_FMT_FULL64, CDB_ORDER_LITTLE,
                                                CDB_CHECKSUM_CRC32C, &h));
    page[300] ^= 0x01; /* payload */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_page_open(page, sizeof page, CDB_PAGE_FMT_FULL64,
                                                       CDB_ORDER_LITTLE, CDB_CHECKSUM_CRC32C, &back));
    page[300] ^= 0x01;
    page[30] ^= 0x80; /* header */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_page_open(page, sizeof page, CDB_PAGE_FMT_FULL64,
                                                       CDB_ORDER_LITTLE, CDB_CHECKSUM_CRC32C, &back));
}

static void test_overflow_in_narrow_formats(void)
{
    static uint8_t page[512];
    cdb_page_header h = sample();
    h.logical_id = 70000; /* does not fit in 16 bits */
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_page_seal(page, sizeof page, CDB_PAGE_FMT_COMPACT,
                                                        CDB_ORDER_LITTLE, CDB_CHECKSUM_CRC32C, &h));
    h = sample();
    h.store_id = UINT64_C(1) << 32;
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_page_seal(page, sizeof page, CDB_PAGE_FMT_FULL32,
                                                        CDB_ORDER_LITTLE, CDB_CHECKSUM_CRC32C, &h));
    h = sample();
    h.payload_len = 1000;
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_page_seal(page, sizeof page, CDB_PAGE_FMT_FULL64,
                                                        CDB_ORDER_LITTLE, CDB_CHECKSUM_CRC32C, &h));
}

static void test_unknown_checksum(void)
{
    static uint8_t page[512];
    cdb_page_header h = sample();
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_page_seal(page, sizeof page, CDB_PAGE_FMT_FULL64,
                                                           CDB_ORDER_LITTLE, 9, &h));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sizes);
    RUN_TEST(test_roundtrips);
    RUN_TEST(test_detects_damage);
    RUN_TEST(test_overflow_in_narrow_formats);
    RUN_TEST(test_unknown_checksum);
    return UNITY_END();
}
