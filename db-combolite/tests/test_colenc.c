/* Column chunk encodings (spec 9.12, 14.10.2): round trips of every encoding
 * over several types, the automatic choice, determinism, canonical-form
 * rejection and buffer limits. */
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "cdb_colenc.h"
#include "cdb_varint.h"
#include "combolite/cdb_value.h"

#define VCAP 65536u
#define SCR_WORDS 8192u
#define DM CDB_DECIMAL_UNBOUNDED

static uint8_t vals[VCAP]; /* the column's values, tag and payload each */
static size_t vlen, cnt;
static uint8_t out[VCAP];
static uint64_t scr[SCR_WORDS];
static cdb_byte_order ORDER = CDB_ORDER_BIG;
static uint32_t rng_state;

void setUp(void)
{
    vlen = 0;
    cnt = 0;
    rng_state = 12345u;
}

void tearDown(void) {}

static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* ---- building columns ----------------------------------------------------- */

#define W_BEGIN cdb_value_writer w; \
    cdb_value_writer_init(&w, vals + vlen, VCAP - vlen, ORDER, CDB_DECIMAL_UNBOUNDED)
#define W_END do { TEST_ASSERT_EQUAL_INT(CDB_OK, w.error); \
    vlen += cdb_value_writer_len(&w); cnt++; } while (0)

static void add_int(int64_t v)
{
    W_BEGIN;
    cdb_value_write_int(&w, v);
    W_END;
}

static void add_bool(int v)
{
    W_BEGIN;
    cdb_value_write_bool(&w, v);
    W_END;
}

static void add_f64(double v)
{
    W_BEGIN;
    cdb_value_write_f64(&w, v);
    W_END;
}

static void add_str(const char *s)
{
    W_BEGIN;
    cdb_value_write_string(&w, s, strlen(s));
    W_END;
}

static void add_bin(const uint8_t *p, size_t n)
{
    W_BEGIN;
    cdb_value_write_binary(&w, p, n);
    W_END;
}

static void add_ts(cdb_ts_precision pr, int64_t t)
{
    W_BEGIN;
    cdb_value_write_timestamp(&w, pr, t);
    W_END;
}

static void add_arr2(int64_t a, int64_t b)
{
    W_BEGIN;
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, a);
    cdb_value_write_int(&w, b);
    cdb_value_end_array(&w);
    W_END;
}

static void add_obj1(uint64_t field, int64_t v)
{
    W_BEGIN;
    cdb_value_begin_object(&w);
    cdb_value_field(&w, field);
    cdb_value_write_int(&w, v);
    cdb_value_end_object(&w);
    W_END;
}

static void add_decimal(uint64_t scale, int64_t unscaled)
{
    W_BEGIN;
    cdb_value_write_decimal_i64(&w, scale, unscaled);
    W_END;
}

static void add_vec3(float a, float b, float c)
{
    float e[3];
    e[0] = a;
    e[1] = b;
    e[2] = c;
    W_BEGIN;
    cdb_value_write_vector(&w, CDB_VEC_F32, 3, e);
    W_END;
}

/* ---- checking a round trip ------------------------------------------------ */

/* Encodes the global column (auto when forced < 0). Returns 1 on success,
 * 0 if the form does not apply (CDB_E_INVALID_ARG). Decodes with the reader
 * and compares item by item with the input. */
static int roundtrip(uint8_t tag, int forced, unsigned *enc_out, size_t *len_out)
{
    unsigned enc = 0;
    size_t olen = 0, pos = 0, i;
    cdb_status st;
    cdb_colenc_reader r;

    if (forced < 0)
        st = cdb_colenc_encode(tag, ORDER, DM, vals, vlen, cnt, scr, SCR_WORDS, out, sizeof out,
                               &enc, &olen);
    else {
        enc = (unsigned)forced;
        st = cdb_colenc_encode_as(enc, tag, ORDER, DM, vals, vlen, cnt, scr, SCR_WORDS, out,
                                  sizeof out, &olen);
    }
    if (st == CDB_E_INVALID_ARG)
        return 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, st);
    if (enc_out)
        *enc_out = enc;
    if (len_out)
        *len_out = olen;

    TEST_ASSERT_EQUAL_INT(CDB_OK,
                          cdb_colenc_reader_init(&r, tag, ORDER, DM, enc, out, olen, cnt, scr, SCR_WORDS));
    for (i = 0; i < cnt; i++) {
        cdb_value v;
        const uint8_t *item;
        size_t il;
        int done = 0;
        uint8_t boolbyte;

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(vals + pos, vlen - pos, ORDER, &v));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_next(&r, &item, &il, &done));
        TEST_ASSERT_FALSE(done);
        if (v.type == CDB_TYPE_BOOL) {
            boolbyte = (uint8_t)(v.as.boolean ? 1 : 0);
            TEST_ASSERT_EQUAL_UINT(1, il);
            TEST_ASSERT_EQUAL_UINT8(boolbyte, item[0]);
        } else {
            TEST_ASSERT_EQUAL_UINT(v.encoded_len - 1u, il);
            TEST_ASSERT_EQUAL_MEMORY(vals + pos + 1, item, il);
        }
        pos += v.encoded_len;
    }
    {
        const uint8_t *item;
        size_t il;
        int done = 0;

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_next(&r, &item, &il, &done));
        TEST_ASSERT_TRUE(done);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_reader_finish(&r));
    TEST_ASSERT_EQUAL_INT(CDB_OK,
                          cdb_colenc_validate(tag, ORDER, DM, enc, out, olen, cnt, scr, SCR_WORDS));
    return 1;
}

/* Every encoding, then the automatic choice, which must be the smallest of
 * the ones that apply and, on a tie, the lowest ID. */
static void check_column(uint8_t tag, int bitpack_expected)
{
    size_t sizes[4], best = (size_t)-1, olen;
    unsigned e, enc, best_enc = 0;

    for (e = 0; e < 4; e++) {
        int ok = roundtrip(tag, (int)e, NULL, &sizes[e]);

        if (e == CDB_COLENC_BITPACK)
            TEST_ASSERT_EQUAL_INT(cnt > 0 ? bitpack_expected : 0, ok);
        else if (cnt > 0 || e == CDB_COLENC_PLAIN)
            TEST_ASSERT_TRUE_MESSAGE(ok, "form refused");
        if (ok && sizes[e] < best) {
            best = sizes[e];
            best_enc = e;
        }
    }
    TEST_ASSERT_TRUE(roundtrip(tag, -1, &enc, &olen));
    TEST_ASSERT_EQUAL_UINT(best, olen);
    TEST_ASSERT_EQUAL_UINT(best_enc, enc);
}

static void fill_ints(size_t n, int64_t base, uint32_t range)
{
    size_t i;

    for (i = 0; i < n; i++)
        add_int(base + (int64_t)(rnd() % range));
}

/* ---- round trips ---------------------------------------------------------- */

static void run_roundtrips(void)
{
    static const char *words[] = { "red", "green", "blue", "", "a longer string value" };
    uint8_t blob[7];
    size_t i;

    /* ints, including the extremes */
    setUp();
    check_column(CDB_TAG_INT, 1); /* n = 0 */
    setUp();
    add_int(7);
    check_column(CDB_TAG_INT, 1); /* n = 1 */
    setUp();
    fill_ints(300, -50, 100);
    check_column(CDB_TAG_INT, 1);
    setUp();
    add_int(INT64_MIN);
    add_int(INT64_MAX);
    add_int(0);
    add_int(INT64_MIN);
    add_int(-1);
    check_column(CDB_TAG_INT, 1);
    setUp();
    add_int(INT64_MAX);
    add_int(INT64_MAX);
    check_column(CDB_TAG_INT, 1);

    /* bool, named by either tag */
    setUp();
    for (i = 0; i < 100; i++)
        add_bool(rnd() & 1u ? 1 : 0);
    check_column(CDB_TAG_TRUE, 1);
    check_column(CDB_TAG_FALSE, 1);
    setUp();
    add_bool(1);
    check_column(CDB_TAG_TRUE, 1);
    setUp();
    for (i = 0; i < 40; i++)
        add_bool(1);
    check_column(CDB_TAG_FALSE, 1);

    /* f64 (bit patterns copied as they are) */
    setUp();
    add_f64(0.0);
    add_f64(1.5);
    add_f64(-2.25e300);
    add_f64(1.5);
    add_f64(1.5);
    check_column(CDB_TAG_F64, 0);

    /* strings */
    setUp();
    for (i = 0; i < 200; i++)
        add_str(words[rnd() % 5u]);
    check_column(CDB_TAG_STRING, 0);
    setUp();
    add_str("only");
    check_column(CDB_TAG_STRING, 0);

    /* bytes */
    setUp();
    for (i = 0; i < 50; i++) {
        size_t j;
        for (j = 0; j < sizeof blob; j++)
            blob[j] = (uint8_t)(rnd() % 3u);
        add_bin(blob, 1u + rnd() % sizeof blob);
    }
    check_column(CDB_TAG_BINARY, 0);

    /* timestamps, one precision */
    setUp();
    for (i = 0; i < 100; i++)
        add_ts(CDB_TS_MILLIS, 1700000000000 + (int64_t)i * 1000 + (int64_t)(rnd() % 7u));
    check_column(CDB_TAG_TIMESTAMP, 1);
    setUp();
    add_ts(CDB_TS_NANOS, INT64_MIN);
    add_ts(CDB_TS_NANOS, INT64_MAX);
    check_column(CDB_TAG_TIMESTAMP, 1);
    /* mixed precisions cannot be bit-packed; the choice still round trips */
    setUp();
    add_ts(CDB_TS_SECONDS, 5);
    add_ts(CDB_TS_MILLIS, 5);
    add_ts(CDB_TS_SECONDS, 5);
    check_column(CDB_TAG_TIMESTAMP, 0);
}

static void test_roundtrips(void)
{
    ORDER = CDB_ORDER_BIG;
    run_roundtrips();
    ORDER = CDB_ORDER_LITTLE;
    run_roundtrips();
    ORDER = CDB_ORDER_BIG;
}

/* ---- the choice ----------------------------------------------------------- */

static unsigned choose(uint8_t tag)
{
    unsigned enc = 99;
    size_t olen;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(tag, ORDER, DM, vals, vlen, cnt, scr, SCR_WORDS,
                                                    out, sizeof out, &enc, &olen));
    return enc;
}

static void test_choice(void)
{
    static const char *words[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
                                   "golf", "hotel", "india", "juliet" };
    size_t i, j;

    /* few distinct strings in random order: dictionary */
    for (i = 0; i < 300; i++)
        add_str(words[rnd() % 3u]);
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_DICT, choose(CDB_TAG_STRING));

    /* long runs of long strings: run-length */
    setUp();
    for (i = 0; i < 10; i++)
        for (j = 0; j < 50; j++)
            add_str(words[i]);
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_RLE, choose(CDB_TAG_STRING));

    /* a small range of ints: bit-packed */
    setUp();
    fill_ints(500, 1000000, 100);
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_BITPACK, choose(CDB_TAG_INT));

    /* all-distinct strings: nothing beats plain */
    setUp();
    for (i = 0; i < 50; i++) {
        char s[16];
        s[0] = 's';
        s[1] = (char)('a' + (int)(i / 10u));
        s[2] = (char)('a' + (int)(i % 10u));
        s[3] = 0;
        add_str(s);
    }
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_PLAIN, choose(CDB_TAG_STRING));

    /* wide random ints: whatever is smallest (check_column compares them all) */
    setUp();
    for (i = 0; i < 100; i++) {
        uint64_t u = ((uint64_t)rnd() << 40) ^ ((uint64_t)rnd() << 20) ^ rnd();
        add_int((int64_t)u);
    }
    check_column(CDB_TAG_INT, 1);

    /* ties go to the lowest ID: two equal bools are 2 bytes in every form */
    setUp();
    add_bool(1);
    add_bool(1);
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_PLAIN, choose(CDB_TAG_TRUE));
    setUp();
    add_str("x");
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_PLAIN, choose(CDB_TAG_STRING));
    /* empty: plain and no bytes */
    setUp();
    TEST_ASSERT_EQUAL_UINT(CDB_COLENC_PLAIN, choose(CDB_TAG_INT));
}

static void test_determinism(void)
{
    uint8_t out2[VCAP];
    size_t i, l1, l2;
    unsigned e1, e2;

    for (i = 0; i < 200; i++)
        add_int((int64_t)(rnd() % 40u));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(CDB_TAG_INT, ORDER, DM, vals, vlen, cnt, scr,
                                                    SCR_WORDS, out, sizeof out, &e1, &l1));
    memset(scr, 0xAB, sizeof scr); /* scratch contents must not matter */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(CDB_TAG_INT, ORDER, DM, vals, vlen, cnt, scr,
                                                    SCR_WORDS, out2, sizeof out2, &e2, &l2));
    TEST_ASSERT_EQUAL_UINT(e1, e2);
    TEST_ASSERT_EQUAL_UINT(l1, l2);
    TEST_ASSERT_EQUAL_MEMORY(out, out2, l1);
}

/* The byte forms written in the header comment. */
static void test_exact_bytes(void)
{
    static const uint8_t dict[] = { 0x02, 0x02, 0x04, 0x40 };
    static const uint8_t rle[] = { 0x02, 0x02, 0x01, 0x04 };
    static const uint8_t bp[] = { 0x0A, 0x02, 0x18 };
    size_t olen;

    add_int(1);
    add_int(2);
    add_int(1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode_as(CDB_COLENC_DICT, CDB_TAG_INT, ORDER, DM, vals,
                                                       vlen, cnt, scr, SCR_WORDS, out, sizeof out,
                                                       &olen));
    TEST_ASSERT_EQUAL_UINT(sizeof dict, olen);
    TEST_ASSERT_EQUAL_MEMORY(dict, out, olen);

    setUp();
    add_int(1);
    add_int(1);
    add_int(2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode_as(CDB_COLENC_RLE, CDB_TAG_INT, ORDER, DM, vals,
                                                       vlen, cnt, scr, SCR_WORDS, out, sizeof out,
                                                       &olen));
    TEST_ASSERT_EQUAL_UINT(sizeof rle, olen);
    TEST_ASSERT_EQUAL_MEMORY(rle, out, olen);

    setUp();
    add_int(5);
    add_int(6);
    add_int(7);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode_as(CDB_COLENC_BITPACK, CDB_TAG_INT, ORDER,
                                                       DM, vals, vlen, cnt, scr, SCR_WORDS, out,
                                                       sizeof out, &olen));
    TEST_ASSERT_EQUAL_UINT(sizeof bp, olen);
    TEST_ASSERT_EQUAL_MEMORY(bp, out, olen);
}

/* ---- rejection ------------------------------------------------------------ */

#define BYTES(...) ((const uint8_t[]){ __VA_ARGS__ })
#define CHECK(tag, enc, n, expect, ...)                                                  \
    do {                                                                                 \
        const uint8_t b_[] = { __VA_ARGS__ };                                            \
        TEST_ASSERT_EQUAL_INT((expect), cdb_colenc_validate((tag), ORDER, DM, (enc), b_,     \
                                                            sizeof b_, (n), scr, SCR_WORDS)); \
    } while (0)

static void test_reject_plain(void)
{
    CHECK(CDB_TAG_INT, CDB_COLENC_PLAIN, 2, CDB_OK, 0x02, 0x04);
    CHECK(CDB_TAG_INT, CDB_COLENC_PLAIN, 2, CDB_E_NOT_CANONICAL, 0x02, 0x04, 0x00); /* trailing */
    CHECK(CDB_TAG_INT, CDB_COLENC_PLAIN, 3, CDB_E_TRUNCATED, 0x02, 0x04);
    CHECK(CDB_TAG_INT, CDB_COLENC_PLAIN, 1, CDB_E_NOT_CANONICAL, 0x82, 0x00); /* long varint */
    CHECK(CDB_TAG_STRING, CDB_COLENC_PLAIN, 1, CDB_OK, 0x02, 'h', 'i');
    CHECK(CDB_TAG_STRING, CDB_COLENC_PLAIN, 1, CDB_E_TRUNCATED, 0x03, 'h', 'i');
    CHECK(CDB_TAG_STRING, CDB_COLENC_PLAIN, 1, CDB_E_MALFORMED, 0x01, 0xFF); /* not UTF-8 */
    CHECK(CDB_TAG_FALSE, CDB_COLENC_PLAIN, 2, CDB_OK, 0x00, 0x01);
    CHECK(CDB_TAG_FALSE, CDB_COLENC_PLAIN, 1, CDB_E_MALFORMED, 0x02);
    CHECK(CDB_TAG_F32, CDB_COLENC_PLAIN, 1, CDB_E_TRUNCATED, 0x00, 0x00, 0x00);
    /* empty chunk */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_validate(CDB_TAG_INT, ORDER, DM, CDB_COLENC_PLAIN, out,
                                                      0, 0, scr, SCR_WORDS));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_colenc_validate(CDB_TAG_INT, ORDER,
                                                                   DM, CDB_COLENC_DICT, out, 0, 0, scr,
                                                                   SCR_WORDS));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_colenc_validate(CDB_TAG_INT, ORDER, DM, 4, out, 0, 0,
                                                                 scr, SCR_WORDS));
}

static void test_reject_dict(void)
{
    /* values 1, 2, 1: entries 02 04, width 1, indexes 0 1 0 */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_OK, 0x02, 0x02, 0x04, 0x40);
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x02, 0x02, 0x02, 0x40); /* repeat */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x02, 0x02, 0x04, 0xA0); /* order */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x03, 0x02, 0x04, 0x06, 0x10); /* unused */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x02, 0x02, 0x04, 0x41); /* padding */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x02, 0x02, 0x04, 0x40, 0x00);
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_CORRUPT, 0x00, 0x00);          /* no entries */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_CORRUPT, 0x04, 0x02, 0x04, 0x06, 0x08); /* k > n */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_TRUNCATED, 0x02, 0x02, 0x04);  /* no indexes */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_TRUNCATED, 0x02, 0x02);        /* cut entries */
    /* one entry: width 0, no index bytes at all */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_OK, 0x01, 0x02);
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_NOT_CANONICAL, 0x01, 0x02, 0x00);
    /* three entries, 2-bit indexes: an index of 3 is out of range */
    CHECK(CDB_TAG_INT, CDB_COLENC_DICT, 3, CDB_E_CORRUPT, 0x03, 0x02, 0x04, 0x06, 0x1C);
}

static void test_reject_rle(void)
{
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 3, CDB_OK, 0x02, 0x02, 0x01, 0x04);
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 3, CDB_E_CORRUPT, 0x00, 0x02, 0x03, 0x04); /* zero run */
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 3, CDB_E_NOT_CANONICAL, 0x01, 0x02, 0x02, 0x02); /* adjacent */
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 3, CDB_E_TRUNCATED, 0x02, 0x02);         /* count short */
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 2, CDB_E_CORRUPT, 0x03, 0x02);           /* run past n */
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 3, CDB_E_NOT_CANONICAL, 0x02, 0x02, 0x01, 0x04, 0x00);
    CHECK(CDB_TAG_INT, CDB_COLENC_RLE, 2, CDB_E_TRUNCATED, 0x02);               /* run, no item */
}

static void test_reject_bitpack(void)
{
    uint8_t big[16];
    size_t bl;

    /* 5 6 7: minimum 5 (zigzag 0x0A), width 2, deltas 0 1 2 */
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_OK, 0x0A, 0x02, 0x18);
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_NOT_CANONICAL, 0x0A, 0x03, 0x05, 0x00); /* wide */
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_NOT_CANONICAL, 0x08, 0x02, 0x6C); /* low min */
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_NOT_CANONICAL, 0x0A, 0x02, 0x19); /* padding */
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_NOT_CANONICAL, 0x0A, 0x02, 0x18, 0x00);
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_TRUNCATED, 0x0A, 0x02);
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_CORRUPT, 0x0A, 65, 0x00);
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_E_NOT_CANONICAL, 0x8A, 0x00, 0x02, 0x18); /* min */
    /* width 0: all values the minimum */
    CHECK(CDB_TAG_INT, CDB_COLENC_BITPACK, 3, CDB_OK, 0x0A, 0x00);
    /* a delta that wraps below the minimum (min = INT64_MAX, delta 1) */
    bl = cdb_varint_encode(cdb_zigzag_encode(INT64_MAX), big);
    big[bl++] = 1;
    big[bl++] = 0x80;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_colenc_validate(CDB_TAG_INT, ORDER, DM, CDB_COLENC_BITPACK,
                                                             big, bl, 1, scr, SCR_WORDS));
    /* timestamps: precision byte first, at most 3 */
    CHECK(CDB_TAG_TIMESTAMP, CDB_COLENC_BITPACK, 1, CDB_OK, 0x01, 0x00, 0x00);
    CHECK(CDB_TAG_TIMESTAMP, CDB_COLENC_BITPACK, 1, CDB_E_MALFORMED, 0x04, 0x00, 0x00);
    /* bools: minimum 0 or 1, values at most 1 */
    CHECK(CDB_TAG_FALSE, CDB_COLENC_BITPACK, 3, CDB_OK, 0x00, 0x01, 0x40);
    CHECK(CDB_TAG_FALSE, CDB_COLENC_BITPACK, 1, CDB_E_CORRUPT, 0x04, 0x00);
    /* a type that cannot be bit-packed */
    CHECK(CDB_TAG_STRING, CDB_COLENC_BITPACK, 1, CDB_E_CORRUPT, 0x00, 0x00);
}

/* A damaged valid chunk is never accepted as the same values without an
 * error... or, if it decodes, it must decode to a canonical chunk, so
 * re-encoding the same form gives the same bytes. Flip every bit. */
static void test_bit_flips(void)
{
    size_t i, olen, bit;
    unsigned e;

    for (i = 0; i < 60; i++)
        add_int((int64_t)(rnd() % 20u) + 100);
    for (e = 0; e < 4; e++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode_as(e, CDB_TAG_INT, ORDER, DM, vals, vlen, cnt,
                                                           scr, SCR_WORDS, out, sizeof out, &olen));
        for (bit = 0; bit < olen * 8u; bit++) {
            uint8_t save = out[bit / 8u];
            cdb_colenc_reader r;
            const uint8_t *item;
            size_t il, pos = 0, k = 0;
            int done = 0, same = 1;
            cdb_status st;

            out[bit / 8u] = (uint8_t)(save ^ (1u << (bit % 8u)));
            st = cdb_colenc_reader_init(&r, CDB_TAG_INT, ORDER, DM, e, out, olen, cnt, scr, SCR_WORDS);
            while (st == CDB_OK && k < cnt) {
                cdb_value v;

                st = cdb_colenc_next(&r, &item, &il, &done);
                if (st != CDB_OK)
                    break;
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(vals + pos, vlen - pos, ORDER, &v));
                if (il != v.encoded_len - 1u || memcmp(item, vals + pos + 1, il) != 0)
                    same = 0;
                pos += v.encoded_len;
                k++;
            }
            if (st == CDB_OK)
                st = cdb_colenc_reader_finish(&r);
            /* Either an error, or (a flip that changed a value) different
             * values; a flip never yields an unchanged, accepted chunk. */
            TEST_ASSERT_TRUE(st != CDB_OK ? 1 : !same);
            out[bit / 8u] = save;
        }
    }
}

/* ---- buffers and arguments ------------------------------------------------ */

static void test_buffers(void)
{
    size_t i, need, olen;
    unsigned enc;
    uint8_t small[256];
    cdb_colenc_reader r;

    for (i = 0; i < 40; i++)
        add_str(i % 2 ? "left" : "right");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(CDB_TAG_STRING, ORDER, DM, vals, vlen, cnt, scr,
                                                    SCR_WORDS, out, sizeof out, &enc, &need));
    TEST_ASSERT_TRUE(need <= cdb_colenc_max_size(vlen));
    for (i = 0; i < need; i++) {
        unsigned e2;

        memset(small, 0xEE, sizeof small);
        TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                              cdb_colenc_encode(CDB_TAG_STRING, ORDER, DM, vals, vlen, cnt, scr,
                                                SCR_WORDS, small, i, &e2, &olen));
        TEST_ASSERT_EQUAL_UINT(need, olen);
        TEST_ASSERT_EQUAL_UINT(enc, e2);
        /* nothing past the stated capacity was touched */
        {
            size_t j;
            for (j = i; j < sizeof small; j++)
                TEST_ASSERT_EQUAL_UINT8(0xEE, small[j]);
        }
    }
    memset(small, 0xEE, sizeof small);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(CDB_TAG_STRING, ORDER, DM, vals, vlen, cnt, scr,
                                                    SCR_WORDS, small, need, &enc, &olen));
    TEST_ASSERT_EQUAL_UINT8(0xEE, small[need]);

    /* scratch too small */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                          cdb_colenc_encode(CDB_TAG_STRING, ORDER, DM, vals, vlen, cnt, scr,
                                            cdb_colenc_scratch_words(cnt) - 1u, out, sizeof out,
                                            &enc, &olen));
    /* dictionary reader scratch too small: 2 entries need 3 words */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode_as(CDB_COLENC_DICT, CDB_TAG_STRING, ORDER,
                                                       DM, vals, vlen, cnt, scr, SCR_WORDS, out,
                                                       sizeof out, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                          cdb_colenc_reader_init(&r, CDB_TAG_STRING, ORDER, DM, CDB_COLENC_DICT, out,
                                                 olen, cnt, scr, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_reader_init(&r, CDB_TAG_STRING, ORDER,
                                                         DM, CDB_COLENC_DICT, out, olen, cnt, scr, 3));
}

static void test_bad_input(void)
{
    size_t olen;
    unsigned enc;

    add_int(1);
    add_int(2);
    /* a value of another type, a bad tag, bytes left over, a forced form that does not apply */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode(CDB_TAG_STRING, ORDER, DM, vals, vlen, cnt, scr, SCR_WORDS,
                                            out, sizeof out, &enc, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode(CDB_TAG_NULL, ORDER, DM, vals, vlen, cnt, scr, SCR_WORDS,
                                            out, sizeof out, &enc, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode(CDB_TAG_INT, ORDER, DM, vals, vlen, cnt - 1u, scr,
                                            SCR_WORDS, out, sizeof out, &enc, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED,
                          cdb_colenc_encode(CDB_TAG_INT, ORDER, DM, vals, vlen - 1u, cnt, scr,
                                            SCR_WORDS, out, sizeof out, &enc, &olen));
    setUp();
    add_str("x");
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode_as(CDB_COLENC_BITPACK, CDB_TAG_STRING, ORDER, DM, vals,
                                               vlen, cnt, scr, SCR_WORDS, out, sizeof out,
                                               &olen));
    /* a non-canonical value in the input */
    {
        static const uint8_t bad[] = { CDB_TAG_INT, 0x82, 0x00 };

        TEST_ASSERT_TRUE(CDB_OK != cdb_colenc_encode(CDB_TAG_INT, ORDER, DM, bad, sizeof bad, 1, scr,
                                                     SCR_WORDS, out, sizeof out, &enc, &olen));
    }
    /* n == 0 accepts only the plain encoding */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode_as(CDB_COLENC_DICT, CDB_TAG_INT, ORDER, DM, vals, 0, 0,
                                               scr, SCR_WORDS, out, sizeof out, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode_as(CDB_COLENC_RLE, CDB_TAG_INT, ORDER, DM, vals, 0, 0,
                                               scr, SCR_WORDS, out, sizeof out, &olen));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_colenc_encode_as(CDB_COLENC_BITPACK, CDB_TAG_INT, ORDER, DM, vals, 0,
                                               0, scr, SCR_WORDS, out, sizeof out, &olen));
}

/* Deep validation of container and leaf types that are not pure scalars:
 * array and object elements recurse through cdb_value_validate; decimal and
 * vector get a shape check (see cdb_colenc.h for what is, and is not,
 * checked on the read side). */
static void test_deep_validate(void)
{
    size_t olen;

    /* array: round trips, and a bad element or wrong count is caught */
    add_arr2(1, 2);
    add_arr2(-5, 100000);
    check_column(CDB_TAG_ARRAY, 0);
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_colenc_encode(CDB_TAG_ARRAY, ORDER, DM, vals, 0, 0, scr,
                                                    SCR_WORDS, out, sizeof out, NULL, &olen));
    {
        /* array item (tag already stripped): length 3, payload = count
         * varint 3, then one int element (tag 0x03, zigzag 1): the
         * declared count does not match the one element actually there. */
        static const uint8_t bad_count[] = { 0x03, 0x03, CDB_TAG_INT, 0x02 };

        TEST_ASSERT_EQUAL_INT(
            CDB_E_MALFORMED,
            cdb_colenc_validate(CDB_TAG_ARRAY, ORDER, DM, CDB_COLENC_PLAIN, bad_count,
                               sizeof bad_count, 1, scr, SCR_WORDS));
    }

    /* object: round trips, and a field out of order is caught */
    setUp();
    add_obj1(1, 7);
    add_obj1(2, -7);
    check_column(CDB_TAG_OBJECT, 0);

    /* decimal: round trips; zero unscaled bytes is rejected */
    setUp();
    add_decimal(2, 12345);
    add_decimal(0, -1);
    check_column(CDB_TAG_DECIMAL, 0);
    {
        /* decimal tag, len=1, scale varint 0x00, zero unscaled bytes */
        static const uint8_t no_unscaled[] = { 0x01, 0x00 };

        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED,
                              cdb_colenc_validate(CDB_TAG_DECIMAL, ORDER, DM, CDB_COLENC_PLAIN,
                                                 no_unscaled, sizeof no_unscaled, 1, scr,
                                                 SCR_WORDS));
    }
    {
        /* decimal tag, len=3: scale varint 0x00, unscaled {0x00, 0x05} -
         * a redundant leading byte (0x05's high bit is already 0), so not
         * the shortest big-endian two's complement form. */
        static const uint8_t non_shortest[] = { 0x03, 0x00, 0x00, 0x05 };

        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL,
                              cdb_colenc_validate(CDB_TAG_DECIMAL, ORDER, DM, CDB_COLENC_PLAIN,
                                                 non_shortest, sizeof non_shortest, 1, scr,
                                                 SCR_WORDS));
    }

    /* vector: round trips; a short data length is rejected */
    setUp();
    add_vec3(1.0f, -2.5f, 0.0f);
    check_column(CDB_TAG_VECTOR, 0);
    {
        /* vector tag, len=4: elem F32 (0), dims=1, only 2 data bytes (needs 4) */
        static const uint8_t short_data[] = { 0x04, 0x00, 0x01, 0x00, 0x00 };

        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED,
                              cdb_colenc_validate(CDB_TAG_VECTOR, ORDER, DM, CDB_COLENC_PLAIN,
                                                 short_data, sizeof short_data, 1, scr,
                                                 SCR_WORDS));
    }
    {
        /* vector tag, len=6: elem F32, dims=1, data = 0x7F800001 (big
         * endian): a signalling NaN, not the one canonical quiet NaN. */
        static const uint8_t bad_nan[] = { 0x06, 0x00, 0x01, 0x7F, 0x80, 0x00, 0x01 };

        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL,
                              cdb_colenc_validate(CDB_TAG_VECTOR, ORDER, DM, CDB_COLENC_PLAIN,
                                                 bad_nan, sizeof bad_nan, 1, scr, SCR_WORDS));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrips);
    RUN_TEST(test_choice);
    RUN_TEST(test_determinism);
    RUN_TEST(test_exact_bytes);
    RUN_TEST(test_reject_plain);
    RUN_TEST(test_reject_dict);
    RUN_TEST(test_reject_rle);
    RUN_TEST(test_reject_bitpack);
    RUN_TEST(test_bit_flips);
    RUN_TEST(test_buffers);
    RUN_TEST(test_bad_input);
    RUN_TEST(test_deep_validate);
    return UNITY_END();
}
