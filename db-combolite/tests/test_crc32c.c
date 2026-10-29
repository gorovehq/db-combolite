#include <string.h>

#include "unity.h"
#include "cdb_crc32c.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value(void)
{
    /* The standard CRC-32C check value for "123456789". */
    TEST_ASSERT_EQUAL_HEX32(0xE3069283u, cdb_crc32c("123456789", 9));
}

static void test_empty_is_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0u, cdb_crc32c("", 0));
}

static void test_known_vectors(void)
{
    /* Test vectors from RFC 3720, appendix B.4. */
    uint8_t buf[32];
    unsigned i;

    memset(buf, 0x00, sizeof buf);
    TEST_ASSERT_EQUAL_HEX32(0x8A9136AAu, cdb_crc32c(buf, sizeof buf));
    memset(buf, 0xFF, sizeof buf);
    TEST_ASSERT_EQUAL_HEX32(0x62A8AB43u, cdb_crc32c(buf, sizeof buf));
    for (i = 0; i < 32; i++)
        buf[i] = (uint8_t)i;
    TEST_ASSERT_EQUAL_HEX32(0x46DD794Eu, cdb_crc32c(buf, sizeof buf));
}

static void test_incremental_matches_one_shot(void)
{
    const char *msg = "db-combolite incremental crc";
    size_t len = strlen(msg);
    uint32_t crc = cdb_crc32c_update(0, msg, 10);
    crc = cdb_crc32c_update(crc, msg + 10, len - 10);
    TEST_ASSERT_EQUAL_HEX32(cdb_crc32c(msg, len), crc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_is_zero);
    RUN_TEST(test_known_vectors);
    RUN_TEST(test_incremental_matches_one_shot);
    return UNITY_END();
}
