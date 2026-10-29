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

/* The definition, one bit at a time: the reference for the fast paths. */
static uint32_t crc_bitwise(uint32_t crc, const uint8_t *p, size_t len)
{
    uint32_t c = ~crc;
    unsigned b;
    while (len--) {
        c ^= *p++;
        for (b = 0; b < 8; b++)
            c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    }
    return ~c;
}

static void test_paths_match_the_definition(void)
{
    static uint8_t buf[600];
    uint32_t x = 12345u;
    size_t off, len, cut;

    TEST_MESSAGE(cdb_crc32c_hw() ? "CPU CRC-32C instruction in use" : "portable path in use");
    for (off = 0; off < sizeof buf; off++) {
        x = x * 1103515245u + 12345u;
        buf[off] = (uint8_t)(x >> 16);
    }
    /* Every start alignment, lengths around the 8-byte steps. */
    for (off = 0; off < 8; off++)
        for (len = 0; len <= 300; len++) {
            uint32_t want = crc_bitwise(0, buf + off, len);
            TEST_ASSERT_EQUAL_HEX32(want, cdb_crc32c_update(0, buf + off, len));
            TEST_ASSERT_EQUAL_HEX32(want, cdb_crc32c_sw(0, buf + off, len));
        }
    /* Continuing from any split point. */
    for (cut = 0; cut <= 64; cut++) {
        uint32_t want = crc_bitwise(0, buf, 64);
        TEST_ASSERT_EQUAL_HEX32(want, cdb_crc32c_update(cdb_crc32c_update(0, buf, cut), buf + cut, 64 - cut));
        TEST_ASSERT_EQUAL_HEX32(want, cdb_crc32c_sw(cdb_crc32c_sw(0, buf, cut), buf + cut, 64 - cut));
    }
    /* A whole page, as the pager checks one. */
    TEST_ASSERT_EQUAL_HEX32(crc_bitwise(0, buf, sizeof buf), cdb_crc32c(buf, sizeof buf));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty_is_zero);
    RUN_TEST(test_known_vectors);
    RUN_TEST(test_incremental_matches_one_shot);
    RUN_TEST(test_paths_match_the_definition);
    return UNITY_END();
}
