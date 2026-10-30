/* Compression codecs (spec 15): the compressed form, round trips through
 * both kinds of memory, determinism, damage detection and the keep rule. */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "cdb_codec.h"

#define BIG 65536u

static uint8_t plain[BIG], packed[BIG + 1024u], back[BIG];

static void *t_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void t_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static cdb_env_ops ops;
static cdb_env env;

void setUp(void)
{
    memset(&ops, 0, sizeof ops);
    ops.alloc = t_alloc;
    ops.free = t_free;
    env.ops = &ops;
    env.ctx = NULL;
}

void tearDown(void) {}

static void test_available(void)
{
    TEST_ASSERT_TRUE(cdb_codec_available(CDB_CODEC_NONE));
#ifdef CDB_WITH_XZ
    TEST_ASSERT_TRUE(cdb_codec_available(CDB_CODEC_LZMA2));
#endif
    TEST_ASSERT_FALSE(cdb_codec_available(CDB_CODEC_ZSTD)); /* reserved, not built */
}

#ifdef CDB_WITH_XZ
/* Repetitive text: compresses well. */
static void fill_text(size_t n)
{
    static const char words[] = "the quick brown fox jumps over the lazy dog while the database packs pages ";
    size_t i;
    for (i = 0; i < n; i++)
        plain[i] = (uint8_t)words[(i * 7u + i / 13u) % (sizeof words - 1u)];
}

/* A fixed pseudo-random sequence: does not compress. */
static void fill_noise(size_t n)
{
    uint32_t x = 0x12345678u;
    size_t i;
    for (i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        plain[i] = (uint8_t)x;
    }
}

static size_t roundtrip(cdb_codec_mem *m, size_t n)
{
    size_t clen, blen;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_compress(m, CDB_CODEC_LZMA2, plain, n, packed, sizeof packed, &clen));
    TEST_ASSERT_TRUE(clen <= cdb_codec_bound(n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_decompress(m, CDB_CODEC_LZMA2, packed, clen, back, sizeof back, &blen));
    TEST_ASSERT_EQUAL_size_t(n, blen);
    if (n)
        TEST_ASSERT_EQUAL_MEMORY(plain, back, n);
    return clen;
}

static void test_roundtrip_with_allocator(void)
{
    cdb_codec_mem m;
    size_t clen;
    memset(&m, 0, sizeof m);
    m.env = &env;
    fill_text(BIG);
    clen = roundtrip(&m, BIG);
    TEST_ASSERT_TRUE(cdb_codec_keeps(BIG, clen));
    TEST_ASSERT_TRUE(clen < BIG / 4u);
    fill_noise(4096);
    clen = roundtrip(&m, 4096);
    TEST_ASSERT_FALSE(cdb_codec_keeps(4096, clen)); /* noise does not save 1/8 */
    roundtrip(&m, 0);
    roundtrip(&m, 1);
}

/* Without an allocator the codec works in caller memory, sized by
 * cdb_codec_work_size; too little is refused, not overrun. */
static void test_roundtrip_in_work_memory(void)
{
    static uint8_t work[16u * 1024u * 1024u];
    cdb_codec_mem m;
    size_t need = cdb_codec_work_size(CDB_CODEC_LZMA2, BIG), clen;
    TEST_ASSERT_TRUE(need > 0 && need <= sizeof work);
    memset(&m, 0, sizeof m);
    m.work = work;
    m.work_size = need;
    fill_text(BIG);
    roundtrip(&m, BIG);
    m.work_size = 1024;
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                          cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, BIG, packed, sizeof packed, &clen));
}

/* The same input always compresses to the same bytes. */
static void test_deterministic(void)
{
    static uint8_t first[BIG + 1024u];
    cdb_codec_mem m;
    size_t a, b;
    memset(&m, 0, sizeof m);
    m.env = &env;
    fill_text(20000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, 20000, first, sizeof first, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, 20000, packed, sizeof packed, &b));
    TEST_ASSERT_EQUAL_size_t(a, b);
    TEST_ASSERT_EQUAL_MEMORY(first, packed, a);
}

/* Damage that changes the length or the stream's end is CDB_E_CORRUPT.
 * (A flipped bit inside the stream may still decode; page and record
 * checksums catch those first, spec 15.6.) */
static void test_damage(void)
{
    cdb_codec_mem m;
    size_t clen, blen, plen;
    memset(&m, 0, sizeof m);
    m.env = &env;
    fill_text(10000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, 10000, packed, sizeof packed, &clen));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_codec_length(packed, clen, &plen));
    TEST_ASSERT_EQUAL_size_t(10000, plen);
    /* Truncated. */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT,
                          cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, clen - 1u, back, sizeof back, &blen));
    /* Trailing bytes. */
    packed[clen] = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT,
                          cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, clen + 1u, back, sizeof back, &blen));
    /* A different stated length: 10000 is a two-byte varint; make it 9999. */
    {
        uint8_t saved = packed[0];
        packed[0] = (uint8_t)(packed[0] - 1u);
        TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT,
                              cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, clen, back, sizeof back, &blen));
        packed[0] = saved;
    }
    /* A dictionary property out of range. */
    {
        uint8_t saved = packed[2];
        packed[2] = 41;
        TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT,
                              cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, clen, back, sizeof back, &blen));
        packed[2] = saved;
    }
    /* Too small an output: the stated length is reported for a retry. */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                          cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, clen, back, 100, &blen));
    TEST_ASSERT_EQUAL_size_t(10000, blen);
    /* Too small an output to compress into. */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                          cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, 10000, packed, 20, &clen));
    /* Codecs that are not built. */
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED,
                          cdb_codec_compress(&m, CDB_CODEC_ZSTD, plain, 10, packed, sizeof packed, &clen));
}

#else
/* A build without the codec refuses, so callers store data plain (15.7). */
static void test_without_codec(void)
{
    cdb_codec_mem m;
    size_t n;
    memset(&m, 0, sizeof m);
    m.env = &env;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED,
                          cdb_codec_compress(&m, CDB_CODEC_LZMA2, plain, 10, packed, sizeof packed, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED,
                          cdb_codec_decompress(&m, CDB_CODEC_LZMA2, packed, 10, back, sizeof back, &n));
    TEST_ASSERT_EQUAL_size_t(0, cdb_codec_work_size(CDB_CODEC_LZMA2, 1000));
}
#endif

static void test_thresholds(void)
{
    TEST_ASSERT_FALSE(cdb_codec_worth_trying(1024, 4096, 0));
    TEST_ASSERT_TRUE(cdb_codec_worth_trying(1025, 4096, 0));
    TEST_ASSERT_TRUE(cdb_codec_worth_trying(10, 4096, 1));
    TEST_ASSERT_TRUE(cdb_codec_keeps(800, 700));
    TEST_ASSERT_FALSE(cdb_codec_keeps(800, 701));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_available);
#ifdef CDB_WITH_XZ
    RUN_TEST(test_roundtrip_with_allocator);
    RUN_TEST(test_roundtrip_in_work_memory);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_damage);
#else
    RUN_TEST(test_without_codec);
#endif
    RUN_TEST(test_thresholds);
    return UNITY_END();
}
