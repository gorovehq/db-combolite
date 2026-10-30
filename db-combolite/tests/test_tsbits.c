#include <string.h>

#include "unity.h"
#include "cdb_tsbits.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t buf[65536];
static uint64_t rng_state;

static uint64_t rnd(void)
{
    rng_state = rng_state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return rng_state ^ (rng_state >> 29);
}

/* Two's-complement bit pattern to int64, without implementation-defined casts. */
static int64_t as_i64(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(~u) - 1;
}

static double d_from(uint64_t b)
{
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static uint64_t d_bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

static float f_from(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static uint32_t f_bits(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static void reader_for(cdb_bitr *r, const cdb_bitw *w)
{
    cdb_bitr_init(r, w->buf, cdb_bitw_bytes(w));
}

/* ---- bit writer and reader ----------------------------------------------- */

static void test_bit_order_msb_first(void)
{
    cdb_bitw w;
    cdb_bitr r;
    uint64_t v;

    memset(buf, 0xFF, 16);
    cdb_bitw_init(&w, buf, 4);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 1, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 5, 4));
    TEST_ASSERT_EQUAL_HEX8(0xA8, buf[0]); /* 1 0101 000: padding cleared */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0x3FF, 10));
    TEST_ASSERT_EQUAL_size_t(15, w.bits);
    TEST_ASSERT_EQUAL_size_t(2, cdb_bitw_bytes(&w));
    TEST_ASSERT_EQUAL_HEX8(0xAF, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFE, buf[1]);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_bitw_put(&w, 2, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_bitw_put(&w, 0, 65));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0, 0));
    TEST_ASSERT_EQUAL_size_t(15, w.bits);

    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    TEST_ASSERT_EQUAL_UINT64(1, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 4));
    TEST_ASSERT_EQUAL_UINT64(5, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 10));
    TEST_ASSERT_EQUAL_UINT64(0x3FF, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_bitr_get(&r, &v, 2));
    TEST_ASSERT_EQUAL_size_t(15, r.pos);

    /* A full 64-bit word at an odd offset (131 bits). */
    memset(buf, 0, 17);
    cdb_bitw_init(&w, buf, 17);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 5, 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, UINT64_C(0x0123456789ABCDEF), 64));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, UINT64_MAX, 64));
    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 3));
    TEST_ASSERT_EQUAL_UINT64(5, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 64));
    TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0123456789ABCDEF), v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 64));
    TEST_ASSERT_EQUAL_HEX64(UINT64_MAX, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

static void test_writer_all_or_nothing(void)
{
    uint8_t small[8], snap[8];
    cdb_bitw w;
    cdb_tsb_time ts;
    cdb_tsb_int is;
    cdb_tsb_float fs;
    cdb_tsb_time ts_snap;

    memset(small, 0, sizeof small);
    cdb_bitw_init(&w, small, 3);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0xABCDE, 20));
    memcpy(snap, small, sizeof small);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_bitw_put(&w, 0x1F, 5));
    TEST_ASSERT_EQUAL_size_t(20, w.bits);
    TEST_ASSERT_EQUAL_MEMORY(snap, small, sizeof small);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0xF, 4));
    TEST_ASSERT_EQUAL_size_t(24, w.bits);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_bitw_put(&w, 0, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_bool_encode(&w, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_dict_encode(&w, 1, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_dict_encode(&w, 0, 0)); /* zero bits */

    /* A multi-field code that does not fit writes none of its fields. */
    memset(small, 0, sizeof small);
    cdb_bitw_init(&w, small, 2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0, 4));
    memcpy(snap, small, sizeof small);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_bucket_encode(&w, 200));
    TEST_ASSERT_EQUAL_size_t(4, w.bits);
    TEST_ASSERT_EQUAL_MEMORY(snap, small, sizeof small);

    cdb_tsb_time_init(&ts, 1000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &ts, 1010)); /* 9 bits */
    ts_snap = ts;
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_time_encode(&w, &ts, 5000));
    TEST_ASSERT_EQUAL_INT64(ts_snap.prev_t, ts.prev_t);
    TEST_ASSERT_EQUAL_INT64(ts_snap.prev_delta, ts.prev_delta);
    TEST_ASSERT_EQUAL_size_t(13, w.bits);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &ts, 1020)); /* 1 bit */
    TEST_ASSERT_EQUAL_size_t(14, w.bits);

    cdb_tsb_int_init(&is);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_int_encode(&w, &is, 5));
    TEST_ASSERT_EQUAL_INT64(0, is.prev);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &is, 0));
    TEST_ASSERT_EQUAL_size_t(15, w.bits);

    /* Floats: the raw first value needs 64 bits. */
    memset(small, 0, sizeof small);
    cdb_bitw_init(&w, small, 7);
    cdb_tsb_float_init(&fs);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_f64_encode(&w, &fs, 1.5));
    TEST_ASSERT_EQUAL_INT(0, fs.started);
    TEST_ASSERT_EQUAL_size_t(0, w.bits);
    cdb_bitw_init(&w, small, 8);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_encode(&w, &fs, 1.5));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_f64_encode(&w, &fs, 1.5));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_tsb_f64_encode(&w, &fs, 2.5));
    TEST_ASSERT_EQUAL_HEX64(d_bits(1.5), fs.prev_bits);
    TEST_ASSERT_EQUAL_size_t(64, w.bits);
}

static void test_writer_resume_appends(void)
{
    uint8_t one[32], two[32]; /* the values below take 217 bits */
    cdb_bitw w;
    cdb_tsb_int s1, s2;
    size_t i, mid;
    static const int64_t vals[] = { 3, 3, 900, -5, 70000, INT64_MIN, 0 };

    memset(one, 0, sizeof one);
    memset(two, 0xEE, sizeof two);
    cdb_bitw_init(&w, one, sizeof one);
    cdb_tsb_int_init(&s1);
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s1, vals[i]));

    /* Write the first three, then continue from a resumed writer. */
    cdb_bitw_init(&w, two, sizeof two);
    cdb_tsb_int_init(&s2);
    for (i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s2, vals[i]));
    mid = w.bits;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_bitw_resume(&w, two, 1, 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_resume(&w, two, sizeof two, mid));
    for (; i < sizeof vals / sizeof vals[0]; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s2, vals[i]));
    TEST_ASSERT_EQUAL_MEMORY(one, two, cdb_bitw_bytes(&w));
}

/* ---- bucket code --------------------------------------------------------- */

static void test_bucket_boundaries(void)
{
    static const struct {
        uint64_t z;
        size_t bits;
    } cases[] = {
        { 0, 1 },
        { 1, 9 },
        { 127, 9 },
        { 128, 18 },
        { 32767, 18 },
        { 32768, 35 },
        { UINT64_C(0x7FFFFFFF), 35 },
        { UINT64_C(0x80000000), 68 },
        { UINT64_MAX, 68 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cdb_bitw w;
        cdb_bitr r;
        uint64_t z = 1;

        memset(buf, 0, 16);
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, cdb_tsb_bucket_bits(cases[i].z));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bucket_encode(&w, cases[i].z));
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, w.bits);
        reader_for(&r, &w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bucket_decode(&r, &z));
        TEST_ASSERT_EQUAL_UINT64(cases[i].z, z);
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, r.pos);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
    }

    /* Exact bit patterns. */
    memset(buf, 0, 16);
    {
        cdb_bitw w;
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bucket_encode(&w, 127)); /* 10 1111111 */
        TEST_ASSERT_EQUAL_HEX8(0xBF, buf[0]);
        TEST_ASSERT_EQUAL_HEX8(0x80, buf[1]);
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bucket_encode(&w, 128)); /* 110 000000010000000 */
        TEST_ASSERT_EQUAL_HEX8(0xC0, buf[0]);
        TEST_ASSERT_EQUAL_HEX8(0x20, buf[1]);
        TEST_ASSERT_EQUAL_HEX8(0x00, buf[2]);
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bucket_encode(&w, 0)); /* 0 */
        TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);
    }
}

static void test_bucket_signed_boundaries(void)
{
    /* zigzag: 63 -> 126, -64 -> 127, 64 -> 128, -16384 -> 32767, 16384 -> 32768. */
    static const struct {
        int64_t d;
        size_t bits;
    } cases[] = {
        { 0, 1 },          { -1, 9 },         { 63, 9 },          { -64, 9 },
        { 64, 18 },        { -16384, 18 },    { 16384, 35 },      { -1073741824, 35 },
        { 1073741824, 68 }, { INT64_MAX, 68 }, { INT64_MIN, 68 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cdb_tsb_int s;
        cdb_tsb_time ts;
        cdb_bitw w;
        cdb_bitr r;
        int64_t v;

        cdb_tsb_int_init(&s);
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, cdb_tsb_int_bits(&s, cases[i].d));
        cdb_tsb_time_init(&ts, 0);
        /* With prev_delta 0 the first dod equals the delta. */
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, cdb_tsb_time_bits(&ts, cases[i].d));

        memset(buf, 0, 16);
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s, cases[i].d));
        TEST_ASSERT_EQUAL_size_t(cases[i].bits, w.bits);
        reader_for(&r, &w);
        cdb_tsb_int_init(&s);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_decode(&r, &s, &v));
        TEST_ASSERT_EQUAL_INT64(cases[i].d, v);
    }
}

static void test_bucket_rejects_oversized(void)
{
    static const struct {
        uint64_t prefix;
        unsigned plen;
        uint64_t payload;
        unsigned width;
        cdb_status expect;
    } cases[] = {
        { 2, 2, 0, 7, CDB_E_NOT_CANONICAL },
        { 2, 2, 1, 7, CDB_OK },
        { 6, 3, 0, 15, CDB_E_NOT_CANONICAL },
        { 6, 3, 127, 15, CDB_E_NOT_CANONICAL },
        { 6, 3, 128, 15, CDB_OK },
        { 14, 4, 32767, 31, CDB_E_NOT_CANONICAL },
        { 14, 4, 32768, 31, CDB_OK },
        { 15, 4, 0, 64, CDB_E_NOT_CANONICAL },
        { 15, 4, UINT64_C(0x7FFFFFFF), 64, CDB_E_NOT_CANONICAL },
        { 15, 4, UINT64_C(0x80000000), 64, CDB_OK },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cdb_bitw w;
        cdb_bitr r;
        cdb_tsb_int s;
        cdb_tsb_time ts;
        uint64_t z = 0;
        int64_t v;

        memset(buf, 0, 16);
        cdb_bitw_init(&w, buf, 16);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, cases[i].prefix, cases[i].plen));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, cases[i].payload, cases[i].width));
        reader_for(&r, &w);
        TEST_ASSERT_EQUAL_INT(cases[i].expect, cdb_tsb_bucket_decode(&r, &z));
        if (cases[i].expect == CDB_OK) {
            TEST_ASSERT_EQUAL_UINT64(cases[i].payload, z);
            continue;
        }
        TEST_ASSERT_EQUAL_size_t(0, r.pos);

        /* The column decoders pass the rejection on and keep their state. */
        cdb_tsb_int_init(&s);
        s.prev = 42;
        TEST_ASSERT_EQUAL_INT(cases[i].expect, cdb_tsb_int_decode(&r, &s, &v));
        TEST_ASSERT_EQUAL_INT64(42, s.prev);
        cdb_tsb_time_init(&ts, 7);
        TEST_ASSERT_EQUAL_INT(cases[i].expect, cdb_tsb_time_decode(&r, &ts, &v));
        TEST_ASSERT_EQUAL_INT64(7, ts.prev_t);
        TEST_ASSERT_EQUAL_size_t(0, r.pos);
    }
}

/* ---- timestamp column ---------------------------------------------------- */

static void time_roundtrip(const int64_t *t, size_t n)
{
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_time s;
    size_t i;

    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_time_init(&s, t[0]);
    for (i = 1; i < n; i++) {
        size_t before = w.bits;
        size_t predicted = cdb_tsb_time_bits(&s, t[i]);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &s, t[i]));
        TEST_ASSERT_EQUAL_size_t(predicted, w.bits - before);
    }
    reader_for(&r, &w);
    cdb_tsb_time_init(&s, t[0]);
    for (i = 1; i < n; i++) {
        int64_t got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_decode(&r, &s, &got));
        TEST_ASSERT_EQUAL_INT64(t[i], got);
    }
    TEST_ASSERT_EQUAL_size_t(w.bits, r.pos);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

static void test_time_regular(void)
{
    static int64_t t[10000];
    cdb_bitw w;
    cdb_tsb_time s;
    size_t i, n = sizeof t / sizeof t[0];

    for (i = 0; i < n; i++)
        t[i] = INT64_C(1700000000000) + (int64_t)i * 1000;
    time_roundtrip(t, n);

    /* The second point costs the first delta (zigzag 2000: 18 bits); every
     * later point at the same interval costs exactly 1 bit. */
    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_time_init(&s, t[0]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &s, t[1]));
    TEST_ASSERT_EQUAL_size_t(18, w.bits);
    for (i = 2; i < n; i++) {
        size_t before = w.bits;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &s, t[i]));
        TEST_ASSERT_EQUAL_size_t(1, w.bits - before);
    }
    TEST_ASSERT_EQUAL_size_t(18 + (n - 2), w.bits);

    /* A regular decreasing series: 9 bits (zigzag 13), then 1 bit each. */
    for (i = 0; i < 500; i++)
        t[i] = -(int64_t)i * 7;
    time_roundtrip(t, 500);
    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_time_init(&s, t[0]);
    for (i = 1; i < 500; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &s, t[i]));
    TEST_ASSERT_EQUAL_size_t(9 + 498, w.bits);
}

static void test_time_irregular(void)
{
    static int64_t t[4000];
    static const int64_t edges[] = {
        0, INT64_MIN, INT64_MAX, INT64_MIN, -1, 1, INT64_MAX, INT64_MAX, 0, INT64_MIN, INT64_MIN,
        INT64_MAX - 1, INT64_MIN + 1, 0,
    };
    size_t i, n = sizeof t / sizeof t[0], ne = sizeof edges / sizeof edges[0];

    rng_state = 1;
    for (i = 0; i < ne; i++)
        t[i] = edges[i];
    for (; i < n; i++) {
        uint64_t prev = (uint64_t)t[i - 1];
        uint64_t kind = rnd() % 6;
        if (kind == 0)
            t[i] = as_i64(prev + 1000 + rnd() % 50);            /* jittered interval */
        else if (kind == 1)
            t[i] = as_i64(prev - rnd() % 100000);               /* decreasing */
        else if (kind == 2)
            t[i] = as_i64(rnd());                               /* huge jump */
        else if (kind == 3)
            t[i] = t[i - 1];                                    /* repeat */
        else if (kind == 4)
            t[i] = (rnd() & 1) ? INT64_MIN : INT64_MAX;         /* extremes */
        else
            t[i] = as_i64(prev + (rnd() % (UINT64_C(1) << 40))); /* medium jump */
    }
    time_roundtrip(t, n);
}

/* ---- integer column ------------------------------------------------------ */

static void test_int_roundtrip(void)
{
    static int64_t v[5000];
    static const int64_t edges[] = {
        0, INT64_MIN, INT64_MAX, -1, 1, INT64_MIN, INT64_MIN, INT64_MAX, 0, INT64_MAX, INT64_MIN,
        63, -1, 63, 127, -32768,
    };
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_int s;
    size_t i, n = sizeof v / sizeof v[0], ne = sizeof edges / sizeof edges[0];

    rng_state = 2;
    for (i = 0; i < ne; i++)
        v[i] = edges[i];
    for (; i < n; i++) {
        uint64_t kind = rnd() % 4;
        if (kind == 0)
            v[i] = as_i64(rnd());
        else if (kind == 1)
            v[i] = as_i64((uint64_t)v[i - 1] + rnd() % 200 - 100);
        else if (kind == 2)
            v[i] = v[i - 1];
        else
            v[i] = (rnd() & 1) ? INT64_MIN : INT64_MAX;
    }

    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_int_init(&s);
    for (i = 0; i < n; i++) {
        size_t before = w.bits;
        size_t predicted = cdb_tsb_int_bits(&s, v[i]);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s, v[i]));
        TEST_ASSERT_EQUAL_size_t(predicted, w.bits - before);
        if (i > 0 && v[i] == v[i - 1])
            TEST_ASSERT_EQUAL_size_t(1, w.bits - before);
    }
    reader_for(&r, &w);
    cdb_tsb_int_init(&s);
    for (i = 0; i < n; i++) {
        int64_t got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_decode(&r, &s, &got));
        TEST_ASSERT_EQUAL_INT64(v[i], got);
    }
    TEST_ASSERT_EQUAL_size_t(w.bits, r.pos);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

/* ---- float columns ------------------------------------------------------- */

static void f64_roundtrip(const uint64_t *b, size_t n)
{
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_float s;
    size_t i;

    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_float_init(&s);
    for (i = 0; i < n; i++) {
        size_t before = w.bits;
        size_t predicted = cdb_tsb_f64_bits(&s, d_from(b[i]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_encode(&w, &s, d_from(b[i])));
        TEST_ASSERT_EQUAL_size_t(predicted, w.bits - before);
        if (i == 0)
            TEST_ASSERT_EQUAL_size_t(64, w.bits);
        else if (b[i] == b[i - 1])
            TEST_ASSERT_EQUAL_size_t(1, w.bits - before);
    }
    reader_for(&r, &w);
    cdb_tsb_float_init(&s);
    for (i = 0; i < n; i++) {
        double got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_decode(&r, &s, &got));
        TEST_ASSERT_EQUAL_HEX64(b[i], d_bits(got));
    }
    TEST_ASSERT_EQUAL_size_t(w.bits, r.pos);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

static void test_f64_roundtrip(void)
{
    static uint64_t b[3000];
    static const uint64_t special[] = {
        UINT64_C(0x3FF0000000000000), UINT64_C(0x3FF0000000000000), /* 1.0 repeated */
        UINT64_C(0x3FF0000000000000), UINT64_C(0x7FF8000000000000), /* quiet NaN */
        UINT64_C(0x7FF0000000000001),                               /* signalling NaN */
        UINT64_C(0xFFF8000000000123),                               /* negative NaN payload */
        UINT64_C(0x7FFFFFFFFFFFFFFF), UINT64_C(0x7FFFFFFFFFFFFFFF),
        UINT64_C(0x8000000000000000),                               /* -0.0 */
        UINT64_C(0x0000000000000000),                               /* +0.0 */
        UINT64_C(0x8000000000000000), UINT64_C(0x7FF0000000000000), /* +inf */
        UINT64_C(0xFFF0000000000000),                               /* -inf */
        UINT64_C(0x0000000000000001),                               /* smallest subnormal */
        UINT64_C(0x000FFFFFFFFFFFFF),                               /* largest subnormal */
        UINT64_C(0x8000000000000001), UINT64_C(0x7FEFFFFFFFFFFFFF), /* DBL_MAX */
        UINT64_C(0x0000000000000000), UINT64_C(0xFFFFFFFFFFFFFFFF), /* M = 64 */
    };
    size_t i, n = sizeof b / sizeof b[0], ns = sizeof special / sizeof special[0];

    rng_state = 3;
    for (i = 0; i < ns; i++)
        b[i] = special[i];
    for (; i < n; i++) {
        uint64_t kind = rnd() % 4;
        if (kind == 0)
            b[i] = rnd();
        else if (kind == 1)
            b[i] = b[i - 1];
        else if (kind == 2)
            b[i] = d_bits(20.0 + (double)(i % 64) * 0.25);
        else
            b[i] = special[rnd() % ns];
    }
    f64_roundtrip(b, n);
}

static void test_f64_exact_bits(void)
{
    static const uint64_t b[] = {
        0, UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
        UINT64_C(0x7FFFFFFFFFFFFFFE),
    };
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_float s;
    uint64_t v;
    size_t i, before;
    static const size_t cost[] = { 64, 14, 14, 77 };

    memset(buf, 0, 64);
    cdb_bitw_init(&w, buf, 64);
    cdb_tsb_float_init(&s);
    for (i = 0; i < 4; i++) {
        before = w.bits;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_encode(&w, &s, d_from(b[i])));
        TEST_ASSERT_EQUAL_size_t(cost[i], w.bits - before);
    }
    /* x = top bit: '1', L = 0, M - 1 = 0, payload '1'. */
    reader_for(&r, &w);
    r.pos = 64;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    TEST_ASSERT_EQUAL_UINT64(1, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(0, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(0, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    TEST_ASSERT_EQUAL_UINT64(1, v);
    /* x = 1: L = 63, M - 1 = 0. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(63, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(0, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    /* x = all ones: L = 0, M - 1 = 63. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(0, v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &v, 6));
    TEST_ASSERT_EQUAL_UINT64(63, v);
}

static void test_f32_roundtrip(void)
{
    static uint32_t b[3000];
    static const uint32_t special[] = {
        0x3F800000u, 0x3F800000u, 0x3F800000u, /* 1.0f repeated */
        0x7FC00000u,                           /* quiet NaN */
        0x7F800001u,                           /* signalling NaN */
        0xFFC00123u,                           /* negative NaN payload */
        0x80000000u, 0x00000000u, 0x80000000u, /* -0.0f, +0.0f */
        0x7F800000u, 0xFF800000u,              /* infinities */
        0x00000001u, 0x007FFFFFu, 0x80000001u, /* subnormals */
        0x7F7FFFFFu,                           /* FLT_MAX */
        0x00000000u, 0xFFFFFFFFu,              /* M = 32 */
    };
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_float s;
    size_t i, n = sizeof b / sizeof b[0], ns = sizeof special / sizeof special[0];

    rng_state = 4;
    for (i = 0; i < ns; i++)
        b[i] = special[i];
    for (; i < n; i++) {
        uint64_t kind = rnd() % 4;
        if (kind == 0)
            b[i] = (uint32_t)rnd();
        else if (kind == 1)
            b[i] = b[i - 1];
        else if (kind == 2)
            b[i] = f_bits(20.0f + (float)(i % 64) * 0.25f);
        else
            b[i] = special[rnd() % ns];
    }

    cdb_bitw_init(&w, buf, sizeof buf);
    cdb_tsb_float_init(&s);
    for (i = 0; i < n; i++) {
        size_t before = w.bits;
        size_t predicted = cdb_tsb_f32_bits(&s, f_from(b[i]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f32_encode(&w, &s, f_from(b[i])));
        TEST_ASSERT_EQUAL_size_t(predicted, w.bits - before);
        if (i == 0)
            TEST_ASSERT_EQUAL_size_t(32, w.bits);
        else if (b[i] == b[i - 1])
            TEST_ASSERT_EQUAL_size_t(1, w.bits - before);
        else if (b[i - 1] == 0 && b[i] == 0xFFFFFFFFu)
            TEST_ASSERT_EQUAL_size_t(1 + 5 + 5 + 32, w.bits - before);
    }
    reader_for(&r, &w);
    cdb_tsb_float_init(&s);
    for (i = 0; i < n; i++) {
        float got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f32_decode(&r, &s, &got));
        TEST_ASSERT_EQUAL_HEX32(b[i], f_bits(got));
    }
    TEST_ASSERT_EQUAL_size_t(w.bits, r.pos);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

static void test_float_rejects_bad_lm(void)
{
    /* After a raw first value of 0: '1', L, M - 1, payload. */
    static const struct {
        unsigned width, lbits;
        uint64_t l, m;
        uint64_t payload;
        cdb_status expect;
        uint64_t value;
    } cases[] = {
        { 64, 6, 0, 2, 1, CDB_E_NOT_CANONICAL, 0 },  /* payload top bit clear: L too small */
        { 64, 6, 0, 2, 2, CDB_E_NOT_CANONICAL, 0 },  /* payload low bit clear: M too big */
        { 64, 6, 63, 2, 3, CDB_E_NOT_CANONICAL, 0 }, /* L + M > 64 */
        { 64, 6, 1, 1, 1, CDB_OK, UINT64_C(0x4000000000000000) },
        { 64, 6, 0, 2, 3, CDB_OK, UINT64_C(0xC000000000000000) },
        { 32, 5, 0, 2, 1, CDB_E_NOT_CANONICAL, 0 },
        { 32, 5, 0, 2, 2, CDB_E_NOT_CANONICAL, 0 },
        { 32, 5, 31, 2, 3, CDB_E_NOT_CANONICAL, 0 }, /* L + M > 32 */
        { 32, 5, 0, 32, UINT64_C(0xFFFFFFFE), CDB_E_NOT_CANONICAL, 0 },
        { 32, 5, 0, 2, 3, CDB_OK, 0xC0000000u },
        { 32, 5, 31, 1, 1, CDB_OK, 1 },
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        cdb_bitw w;
        cdb_bitr r;
        cdb_tsb_float s;
        cdb_status st;
        uint64_t got = 0;

        memset(buf, 0, 32);
        cdb_bitw_init(&w, buf, 32);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0, cases[i].width));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 1, 1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, cases[i].l, cases[i].lbits));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, cases[i].m - 1, cases[i].lbits));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, cases[i].payload, (unsigned)cases[i].m));
        reader_for(&r, &w);
        cdb_tsb_float_init(&s);
        if (cases[i].width == 64) {
            double d;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_decode(&r, &s, &d));
            st = cdb_tsb_f64_decode(&r, &s, &d);
            if (st == CDB_OK)
                got = d_bits(d);
        } else {
            float f;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f32_decode(&r, &s, &f));
            st = cdb_tsb_f32_decode(&r, &s, &f);
            if (st == CDB_OK)
                got = f_bits(f);
        }
        TEST_ASSERT_EQUAL_INT(cases[i].expect, st);
        if (st == CDB_OK) {
            TEST_ASSERT_EQUAL_HEX64(cases[i].value, got);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
        } else {
            TEST_ASSERT_EQUAL_size_t(cases[i].width, r.pos);
            TEST_ASSERT_EQUAL_HEX64(0, s.prev_bits);
        }
    }
}

/* ---- bool and dictionary ------------------------------------------------- */

static void test_bool_roundtrip(void)
{
    static int v[1001];
    cdb_bitw w;
    cdb_bitr r;
    size_t i, n = sizeof v / sizeof v[0];

    rng_state = 5;
    for (i = 0; i < n; i++)
        v[i] = (int)(rnd() & 1u);
    cdb_bitw_init(&w, buf, sizeof buf);
    for (i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bool_encode(&w, v[i] ? 7 : 0));
    TEST_ASSERT_EQUAL_size_t(n, w.bits);
    reader_for(&r, &w);
    for (i = 0; i < n; i++) {
        int got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bool_decode(&r, &got));
        TEST_ASSERT_EQUAL_INT(v[i], got);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

static void test_dict_roundtrip(void)
{
    static uint64_t ns[700], is[700];
    static const struct {
        uint64_t n;
        unsigned w;
    } widths[] = {
        { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 2 }, { 4, 3 }, { 7, 3 }, { 8, 4 },
        { 255, 8 }, { 256, 9 }, { 300, 9 }, { UINT64_MAX, 64 },
    };
    cdb_bitw w;
    cdb_bitr r;
    size_t i, count = 0, total = 0;
    uint64_t n = 0, got;

    for (i = 0; i < sizeof widths / sizeof widths[0]; i++)
        TEST_ASSERT_EQUAL_UINT(widths[i].w, cdb_tsb_dict_width(widths[i].n));

    /* For every n in 0..300: one existing index (when there is one), then a
     * new entry, which grows the dictionary. */
    rng_state = 6;
    cdb_bitw_init(&w, buf, sizeof buf);
    while (n <= 300) {
        if (n > 0) {
            ns[count] = n;
            is[count] = rnd() % n;
            count++;
        }
        ns[count] = n;
        is[count] = n;
        count++;
        n++;
    }
    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_dict_encode(&w, ns[i], is[i]));
        total += cdb_tsb_dict_width(ns[i]);
    }
    TEST_ASSERT_EQUAL_size_t(total, w.bits);
    reader_for(&r, &w);
    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_dict_decode(&r, ns[i], &got));
        TEST_ASSERT_EQUAL_UINT64(is[i], got);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));

    /* Out-of-range indexes. */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsb_dict_encode(&w, 5, 6));
    memset(buf, 0, 4);
    cdb_bitw_init(&w, buf, 4);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 7, 3));
    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_tsb_dict_decode(&r, 5, &got));
    TEST_ASSERT_EQUAL_size_t(0, r.pos);
    /* n = 0 takes no bits, even from an empty stream. */
    cdb_bitr_init(&r, buf, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_dict_decode(&r, 0, &got));
    TEST_ASSERT_EQUAL_UINT64(0, got);
}

/* ---- padding ------------------------------------------------------------- */

static void test_padding_rejected(void)
{
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_int s;
    int64_t v;
    uint64_t raw;
    unsigned bit;

    memset(buf, 0, 8);
    cdb_bitw_init(&w, buf, 8);
    cdb_tsb_int_init(&s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &s, 5)); /* 9 bits: 7 padding */
    TEST_ASSERT_EQUAL_size_t(2, cdb_bitw_bytes(&w));

    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_bitr_check_end(&r)); /* unread data */
    cdb_tsb_int_init(&s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_decode(&r, &s, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));

    for (bit = 0; bit < 7; bit++) {
        buf[1] = (uint8_t)(1u << bit);
        reader_for(&r, &w);
        cdb_tsb_int_init(&s);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_decode(&r, &s, &v));
        TEST_ASSERT_EQUAL_INT64(5, v);
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_bitr_check_end(&r));
    }
    buf[1] = 0;

    /* A trailing zero byte is not the shortest form either. */
    cdb_bitr_init(&r, buf, 3);
    cdb_tsb_int_init(&s);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_decode(&r, &s, &v));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, cdb_bitr_check_end(&r));

    /* An exactly byte-aligned stream has no padding. */
    cdb_bitw_init(&w, buf, 8);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitw_put(&w, 0xFF, 8));
    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_get(&r, &raw, 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
}

/* ---- truncation ---------------------------------------------------------- */

#define MIX_N 60

static int64_t mix_t[MIX_N], mix_i[MIX_N];
static uint64_t mix_d[MIX_N], mix_dn[MIX_N], mix_di[MIX_N];
static uint32_t mix_f[MIX_N];
static int mix_b[MIX_N];

/* Decodes the mixed stream; returns the first failure, or CDB_OK. Every value
 * decoded before the failure must match, and a failing call must leave the
 * reader where it was. */
static cdb_status decode_mixed(cdb_bitr *r)
{
    cdb_tsb_time ts;
    cdb_tsb_int is;
    cdb_tsb_float ds, fs;
    size_t k, before;
    cdb_status st;

    cdb_tsb_time_init(&ts, mix_t[0]);
    cdb_tsb_int_init(&is);
    cdb_tsb_float_init(&ds);
    cdb_tsb_float_init(&fs);
    for (k = 0; k < MIX_N; k++) {
        int64_t t, iv;
        double d;
        float f;
        int b;
        uint64_t di;

#define STEP(call, check)              \
    before = r->pos;                   \
    st = (call);                       \
    if (st != CDB_OK) {                \
        TEST_ASSERT_EQUAL_size_t(before, r->pos); \
        return st;                     \
    }                                  \
    check;

        if (k > 0) {
            STEP(cdb_tsb_time_decode(r, &ts, &t), TEST_ASSERT_EQUAL_INT64(mix_t[k], t))
        }
        STEP(cdb_tsb_dict_decode(r, mix_dn[k], &di), TEST_ASSERT_EQUAL_UINT64(mix_di[k], di))
        STEP(cdb_tsb_int_decode(r, &is, &iv), TEST_ASSERT_EQUAL_INT64(mix_i[k], iv))
        STEP(cdb_tsb_f64_decode(r, &ds, &d), TEST_ASSERT_EQUAL_HEX64(mix_d[k], d_bits(d)))
        STEP(cdb_tsb_f32_decode(r, &fs, &f), TEST_ASSERT_EQUAL_HEX32(mix_f[k], f_bits(f)))
        STEP(cdb_tsb_bool_decode(r, &b), TEST_ASSERT_EQUAL_INT(mix_b[k], b))
#undef STEP
    }
    return CDB_OK;
}

static void test_truncation_every_cut(void)
{
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_time ts;
    cdb_tsb_int is;
    cdb_tsb_float ds, fs;
    size_t k, cut;
    uint64_t dict_n = 0;

    rng_state = 7;
    mix_t[0] = INT64_C(1000000);
    for (k = 0; k < MIX_N; k++) {
        if (k > 0)
            mix_t[k] = as_i64((uint64_t)mix_t[k - 1] + 10 + ((k % 7 == 0) ? rnd() % 100000 : 0));
        mix_i[k] = (k % 5 == 0) ? as_i64(rnd()) : (int64_t)(rnd() % 300);
        mix_d[k] = (k % 3 == 0 && k > 0) ? mix_d[k - 1] : rnd();
        mix_f[k] = (k % 3 == 1) ? 0x3F800000u : (uint32_t)rnd();
        mix_b[k] = (int)(rnd() & 1u);
        mix_dn[k] = dict_n;
        mix_di[k] = (k % 3 == 0) ? dict_n : rnd() % (dict_n + 1);
        if (mix_di[k] == dict_n)
            dict_n++;
    }

    memset(buf, 0, 4096);
    cdb_bitw_init(&w, buf, 4096);
    cdb_tsb_time_init(&ts, mix_t[0]);
    cdb_tsb_int_init(&is);
    cdb_tsb_float_init(&ds);
    cdb_tsb_float_init(&fs);
    for (k = 0; k < MIX_N; k++) {
        if (k > 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &ts, mix_t[k]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_dict_encode(&w, mix_dn[k], mix_di[k]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_int_encode(&w, &is, mix_i[k]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_encode(&w, &ds, d_from(mix_d[k])));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f32_encode(&w, &fs, f_from(mix_f[k])));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_bool_encode(&w, mix_b[k]));
    }

    reader_for(&r, &w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, decode_mixed(&r));
    TEST_ASSERT_EQUAL_size_t(w.bits, r.pos);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));

    /* Every bit-level cut (which includes every byte-level cut). */
    for (cut = 0; cut < w.bits; cut++) {
        cdb_bitr_init(&r, buf, (cut + 7) / 8);
        r.len_bits = cut;
        TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, decode_mixed(&r));
        TEST_ASSERT_TRUE(r.pos <= cut);
    }
    /* Byte-level cuts as a caller would see them. */
    for (cut = 0; cut < cdb_bitw_bytes(&w); cut++) {
        cdb_bitr_init(&r, buf, cut);
        TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, decode_mixed(&r));
    }
}

/* ---- bit-pattern float API ----------------------------------------------- */

/* Signalling NaNs (quiet bit clear, payload non-zero) round-trip exactly,
 * in sequences that hit every XOR case, for both widths. */
static void fbits_roundtrip(unsigned width, const uint64_t *v, size_t n)
{
    cdb_bitw w;
    cdb_bitr r;
    cdb_tsb_float es, ds;
    size_t i, before;
    uint64_t got;

    memset(buf, 0xA5, 1024);
    cdb_bitw_init(&w, buf, 1024);
    cdb_tsb_float_init(&es);
    for (i = 0; i < n; i++) {
        before = w.bits;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_fbits_encode(&w, &es, v[i], width));
        if (i > 0) {
            /* bits() predicted what was written (state before this value). */
            cdb_tsb_float tmp;
            tmp.started = 1;
            tmp.prev_bits = v[i - 1];
            TEST_ASSERT_EQUAL_size_t(w.bits - before, cdb_tsb_fbits_bits(&tmp, v[i], width));
        } else {
            TEST_ASSERT_EQUAL_size_t(width, w.bits);
        }
    }
    reader_for(&r, &w);
    cdb_tsb_float_init(&ds);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_fbits_decode(&r, &ds, width, &got));
        TEST_ASSERT_EQUAL_HEX64(v[i], got);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_bitr_check_end(&r));
    TEST_ASSERT_EQUAL_HEX64(es.prev_bits, ds.prev_bits);
}

static void test_fbits_snan_exact(void)
{
    static const uint64_t s64[] = {
        UINT64_C(0x7FF0000000000001), UINT64_C(0x7FF0000000000001), /* repeat */
        UINT64_C(0xFFF4000000000000), UINT64_C(0x7FF7FFFFFFFFFFFF),
        UINT64_C(0x8000000000000000), UINT64_C(0x7FF0000000000001),
        UINT64_C(0xFFF0000000000002), UINT64_C(0x0000000000000000)
    };
    static const uint64_t s32[] = {
        UINT64_C(0x7F800001), UINT64_C(0x7F800001), UINT64_C(0xFFA00000),
        UINT64_C(0x7FBFFFFF), UINT64_C(0x80000000), UINT64_C(0xFF800002),
        UINT64_C(0x00000000), UINT64_C(0x7F800001)
    };
    cdb_bitw w;
    cdb_tsb_float s;

    fbits_roundtrip(64, s64, sizeof s64 / sizeof s64[0]);
    fbits_roundtrip(32, s32, sizeof s32 / sizeof s32[0]);

    /* Bad widths and a 32-bit pattern with high bits set are refused. */
    cdb_bitw_init(&w, buf, 64);
    cdb_tsb_float_init(&s);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsb_fbits_encode(&w, &s, 1, 16));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_tsb_fbits_encode(&w, &s, UINT64_C(0x100000000), 32));
    TEST_ASSERT_EQUAL_size_t(0, w.bits);
    TEST_ASSERT_EQUAL_INT(0, s.started);

    /* The f64/f32 wrappers give the same stream as the bit-pattern API. */
    {
        cdb_bitw a, b;
        cdb_tsb_float sa, sb;
        static uint8_t ba[64], bb[64];
        const double dv[3] = { 1.5, -2.25, 1.5 };
        size_t i;
        cdb_bitw_init(&a, ba, sizeof ba);
        cdb_bitw_init(&b, bb, sizeof bb);
        cdb_tsb_float_init(&sa);
        cdb_tsb_float_init(&sb);
        for (i = 0; i < 3; i++) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_f64_encode(&a, &sa, dv[i]));
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_fbits_encode(&b, &sb, d_bits(dv[i]), 64));
        }
        TEST_ASSERT_EQUAL_size_t(a.bits, b.bits);
        TEST_ASSERT_EQUAL_MEMORY(ba, bb, cdb_bitw_bytes(&a));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bit_order_msb_first);
    RUN_TEST(test_writer_all_or_nothing);
    RUN_TEST(test_writer_resume_appends);
    RUN_TEST(test_bucket_boundaries);
    RUN_TEST(test_bucket_signed_boundaries);
    RUN_TEST(test_bucket_rejects_oversized);
    RUN_TEST(test_time_regular);
    RUN_TEST(test_time_irregular);
    RUN_TEST(test_int_roundtrip);
    RUN_TEST(test_f64_roundtrip);
    RUN_TEST(test_f64_exact_bits);
    RUN_TEST(test_f32_roundtrip);
    RUN_TEST(test_float_rejects_bad_lm);
    RUN_TEST(test_bool_roundtrip);
    RUN_TEST(test_dict_roundtrip);
    RUN_TEST(test_padding_rejected);
    RUN_TEST(test_truncation_every_cut);
    RUN_TEST(test_fbits_snan_exact);
    return UNITY_END();
}
