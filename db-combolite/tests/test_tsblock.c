#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_endian.h"
#include "cdb_tsblock.h"

void setUp(void) {}
void tearDown(void) {}

#define BUFSZ (1u << 19)
#define NMAX CDB_TS_BLOCK_MAX_POINTS

static uint8_t blk_all[BUFSZ], blk_a[BUFSZ], blk_b[BUFSZ], blk_c[BUFSZ];
static cdb_ts_point pts[NMAX + 1], dec[NMAX + 1];
static uint64_t rng_state;

static const cdb_byte_order orders[2] = { CDB_ORDER_LITTLE, CDB_ORDER_BIG };

static uint64_t rnd(void)
{
    rng_state = rng_state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return rng_state ^ (rng_state >> 29);
}

static int64_t as_i64(uint64_t u)
{
    if (u <= (uint64_t)INT64_MAX)
        return (int64_t)u;
    return -(int64_t)(~u) - 1;
}

/* ---- value pools --------------------------------------------------------- */

#define NSTR 8
static const char *const strs[NSTR] = { "", "a", "b", "cpu0", "temp\xC3\xA9", "\xE2\x82\xAC", "a longer string", "z" };

#define NPLAIN 6
static uint8_t plain_buf[2][3][NPLAIN][64];
static size_t plain_len[2][3][NPLAIN];
static const uint8_t plain_types[3] = { CDB_TYPE_TIMESTAMP, CDB_TYPE_BINARY, CDB_TYPE_REF };

static void make_pools(void)
{
    int oi, k, j;
    for (oi = 0; oi < 2; oi++) {
        for (k = 0; k < 3; k++) {
            for (j = 0; j < NPLAIN; j++) {
                cdb_value_writer w;
                uint8_t bin[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
                cdb_value_writer_init(&w, plain_buf[oi][k][j], 64, orders[oi], CDB_DECIMAL_UNBOUNDED);
                if (k == 0)
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_timestamp(&w, CDB_TS_MILLIS, (int64_t)j * 1000 - 2500));
                else if (k == 1)
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_binary(&w, bin, (size_t)j));
                else
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_ref(&w, (uint64_t)j + 1, (uint64_t)j * 7777));
                plain_len[oi][k][j] = cdb_value_writer_len(&w);
            }
        }
    }
}

static int plain_kind(uint8_t type)
{
    int k;
    for (k = 0; k < 3; k++)
        if (plain_types[k] == type)
            return k;
    return -1;
}

static void set_str(cdb_ts_val *v, const char *s)
{
    v->present = 1;
    v->bytes = (const uint8_t *)s;
    v->len = strlen(s);
}

static void set_plain(cdb_ts_val *v, int oi, int k, int j)
{
    v->present = 1;
    v->bytes = plain_buf[oi][k][j];
    v->len = plain_len[oi][k][j];
}

/* ---- comparisons --------------------------------------------------------- */

static void assert_points(const cdb_ts_layout *L, const cdb_ts_point *a, const cdb_ts_point *b, size_t n)
{
    size_t i;
    unsigned f;
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT64(a[i].ts, b[i].ts);
        for (f = 0; f < L->nfields; f++) {
            const cdb_ts_val *x = &a[i].v[f], *y = &b[i].v[f];
            uint8_t t = L->fields[f].type;
            TEST_ASSERT_EQUAL_INT(x->present ? 1 : 0, y->present ? 1 : 0);
            if (!x->present)
                continue;
            if (t == CDB_TYPE_INT || t == CDB_TYPE_F64 || t == CDB_TYPE_F32 || t == CDB_TYPE_BOOL) {
                TEST_ASSERT_EQUAL_HEX64(x->bits, y->bits);
            } else {
                TEST_ASSERT_EQUAL_size_t(x->len, y->len);
                if (x->len)
                    TEST_ASSERT_EQUAL_MEMORY(x->bytes, y->bytes, x->len);
            }
        }
    }
}

static void assert_state(const cdb_tsblock_state *a, const cdb_tsblock_state *b)
{
    unsigned f;
    TEST_ASSERT_EQUAL_UINT32(a->count, b->count);
    TEST_ASSERT_EQUAL_INT64(a->t.prev_t, b->t.prev_t);
    TEST_ASSERT_EQUAL_INT64(a->t.prev_delta, b->t.prev_delta);
    for (f = 0; f < CDB_TS_MAX_FIELDS; f++) {
        TEST_ASSERT_EQUAL_INT64(a->ints[f].prev, b->ints[f].prev);
        TEST_ASSERT_EQUAL_HEX64(a->floats[f].prev_bits, b->floats[f].prev_bits);
        TEST_ASSERT_EQUAL_INT(a->floats[f].started, b->floats[f].started);
        TEST_ASSERT_EQUAL_UINT32(a->dict_n[f], b->dict_n[f]);
    }
}

static void assert_layout(const cdb_ts_layout *a, const cdb_ts_layout *b)
{
    unsigned f;
    TEST_ASSERT_EQUAL_HEX64(a->series, b->series);
    TEST_ASSERT_EQUAL_INT64(a->window_start, b->window_start);
    TEST_ASSERT_EQUAL_INT64(a->window_end, b->window_end);
    TEST_ASSERT_EQUAL_UINT8(a->precision, b->precision);
    TEST_ASSERT_EQUAL_UINT8(a->flags, b->flags);
    TEST_ASSERT_EQUAL_UINT16(a->nfields, b->nfields);
    for (f = 0; f < a->nfields; f++) {
        TEST_ASSERT_EQUAL_HEX64(a->fields[f].id, b->fields[f].id);
        TEST_ASSERT_EQUAL_UINT8(a->fields[f].type, b->fields[f].type);
    }
}

/* Decodes and parses blk, checks it against L/pts/n and (if given) state. */
static void check_block(const uint8_t *blk, size_t len, cdb_byte_order o, const cdb_ts_layout *L,
                        const cdb_ts_point *p, size_t n, const cdb_tsblock_state *st)
{
    cdb_ts_layout dl;
    cdb_tsblock_state ds;
    size_t dn = 0;
    uint32_t cnt = 0;
    int64_t first = 0, last = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_decode(blk, len, o, &dl, dec, NMAX, &dn, &ds));
    TEST_ASSERT_EQUAL_size_t(n, dn);
    assert_layout(L, &dl);
    assert_points(L, p, dec, n);
    if (st != NULL)
        assert_state(st, &ds);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_parse(blk, len, o, &dl, &cnt, &first, &last));
    TEST_ASSERT_EQUAL_UINT32(n, cnt);
    TEST_ASSERT_EQUAL_INT64(p[0].ts, first);
    TEST_ASSERT_EQUAL_INT64(p[n - 1].ts, last);
    assert_layout(L, &dl);
}

/* ---- fixed block with every type ----------------------------------------- */

static void all_types_layout(cdb_ts_layout *L)
{
    static const uint8_t types[8] = { CDB_TYPE_INT, CDB_TYPE_F64, CDB_TYPE_F32, CDB_TYPE_BOOL,
                                      CDB_TYPE_STRING, CDB_TYPE_TIMESTAMP, CDB_TYPE_BINARY, CDB_TYPE_REF };
    unsigned f;
    memset(L, 0, sizeof *L);
    L->series = UINT64_C(0x0102030405060708);
    L->window_start = -1000;
    L->window_end = 1000000;
    L->precision = CDB_TS_MILLIS;
    L->flags = CDB_TS_BLOCK_COV;
    L->nfields = 8;
    for (f = 0; f < 8; f++) {
        L->fields[f].id = 3u + 5u * f;
        L->fields[f].type = types[f];
    }
}

static const uint64_t f64_pats[6] = { UINT64_C(0x3FF8000000000000), UINT64_C(0x3FF8000000000000),
                                      UINT64_C(0x7FF0000000000001), UINT64_C(0x8000000000000000),
                                      UINT64_C(0xC002000000000000), UINT64_C(0x0000000000000001) };
static const uint64_t f32_pats[6] = { 0x3FC00000u, 0x7F800001u, 0x7F800001u, 0x80000000u, 0xFFA00000u, 0x1u };

static size_t all_types_points(int oi)
{
    size_t i, n = 21;
    memset(pts, 0, sizeof pts);
    for (i = 0; i < n; i++) {
        pts[i].ts = -500 + (int64_t)(i * 10) + (i > 10 ? (int64_t)(i * i) : 0);
        if (i % 3 != 1) {
            pts[i].v[0].present = 1;
            pts[i].v[0].bits = (uint64_t)((int64_t)i * 1000 - 9000);
        }
        if (i % 4 != 2) {
            pts[i].v[1].present = 1;
            pts[i].v[1].bits = f64_pats[i % 6];
        }
        pts[i].v[2].present = 1;
        pts[i].v[2].bits = f32_pats[i % 6];
        if (i % 2 == 0) {
            pts[i].v[3].present = 1;
            pts[i].v[3].bits = (i / 2) & 1u;
        }
        if (i % 5 != 4)
            set_str(&pts[i].v[4], strs[(i * 3) % NSTR]);
        if (i % 2 == 1)
            set_plain(&pts[i].v[5], oi, 0, (int)(i % NPLAIN));
        if (i % 7 != 0)
            set_plain(&pts[i].v[6], oi, 1, (int)(i % NPLAIN));
        if (i == 3 || i == 17)
            set_plain(&pts[i].v[7], oi, 2, (int)(i % NPLAIN));
    }
    return n;
}

static void test_build_decode_all_types(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        cdb_tsblock_state st;
        size_t n, len = 0;
        cdb_byte_order o = orders[oi];

        all_types_layout(&L);
        n = all_types_points(oi);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len, &st));
        TEST_ASSERT_EQUAL_UINT32(n, st.count);
        check_block(blk_all, len, o, &L, pts, n, &st);

        /* Header layout in the file's byte order. */
        TEST_ASSERT_EQUAL_HEX64(L.series, cdb_load_u64(blk_all, o));
        TEST_ASSERT_EQUAL_HEX8(o == CDB_ORDER_BIG ? 0x01 : 0x08, blk_all[0]);
        TEST_ASSERT_EQUAL_UINT32(n, cdb_load_u32(blk_all + 24, o));
        TEST_ASSERT_EQUAL_UINT16(8, cdb_load_u16(blk_all + 28, o));
        TEST_ASSERT_EQUAL_HEX8(CDB_TS_MILLIS, blk_all[30]);
        TEST_ASSERT_EQUAL_HEX8(CDB_TS_BLOCK_COV, blk_all[31]);
        {
            /* Read in the other byte order, the header no longer makes sense. */
            cdb_ts_layout x;
            TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, cdb_tsblock_parse(blk_all, len, orders[1 - oi], &x, NULL, NULL, NULL));
        }
    }
}

static void test_single_point_and_empty_fields(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        cdb_tsblock_state st;
        size_t len = 0;

        all_types_layout(&L);
        memset(pts, 0, sizeof pts);
        pts[0].ts = 999999; /* the last tick of the window */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 1, orders[oi], blk_all, BUFSZ, &len, &st));
        check_block(blk_all, len, orders[oi], &L, pts, 1, &st);

        /* No fields at all. */
        L.nfields = 0;
        pts[0].ts = -1000;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 1, orders[oi], blk_all, BUFSZ, &len, &st));
        TEST_ASSERT_EQUAL_size_t(CDB_TS_BLOCK_HEADER + 12u, len);
        check_block(blk_all, len, orders[oi], &L, pts, 1, &st);
    }
}

/* ---- the append oracle --------------------------------------------------- */

static const uint8_t rand_types[8] = { CDB_TYPE_INT, CDB_TYPE_F64, CDB_TYPE_F32, CDB_TYPE_BOOL,
                                       CDB_TYPE_STRING, CDB_TYPE_TIMESTAMP, CDB_TYPE_BINARY, CDB_TYPE_REF };

static void random_layout(cdb_ts_layout *L)
{
    unsigned f;
    uint64_t id = rnd() % 4;
    memset(L, 0, sizeof *L);
    L->series = rnd();
    L->window_start = -(int64_t)(rnd() % (UINT64_C(1) << 40));
    L->window_end = INT64_MAX;
    L->precision = (uint8_t)(rnd() % 4);
    L->flags = (uint8_t)(rnd() % 4);
    L->nfields = (uint16_t)(rnd() % (CDB_TS_MAX_FIELDS + 1));
    for (f = 0; f < L->nfields; f++) {
        id += 1 + rnd() % 1000;
        L->fields[f].id = id;
        L->fields[f].type = rand_types[rnd() % 8];
    }
}

static void random_points(const cdb_ts_layout *L, int oi, size_t n)
{
    size_t i;
    unsigned f;
    int64_t t = L->window_start + (int64_t)(rnd() % 1000);
    unsigned presence[CDB_TS_MAX_FIELDS];

    for (f = 0; f < L->nfields; f++)
        presence[f] = (unsigned)(rnd() % 5); /* 0: never; else present with chance f/4 */
    memset(pts, 0, n * sizeof pts[0]);
    for (i = 0; i < n; i++) {
        uint64_t r = rnd() % 16;
        if (i > 0)
            t += r < 10 ? 1000 : r < 14 ? (int64_t)(1 + rnd() % 5000) : (int64_t)(1 + rnd() % (UINT64_C(1) << 40));
        pts[i].ts = t;
        for (f = 0; f < L->nfields; f++) {
            cdb_ts_val *v = &pts[i].v[f];
            uint8_t ty = L->fields[f].type;
            if (presence[f] == 0 || rnd() % 4 >= presence[f])
                continue;
            v->present = 1;
            switch (ty) {
            case CDB_TYPE_INT:
                r = rnd() % 4;
                v->bits = r == 0 ? rnd() : r == 1 ? (uint64_t)(int64_t)(i * 3) : (uint64_t)(as_i64(rnd()) % 200);
                break;
            case CDB_TYPE_F64:
                r = rnd() % 4;
                v->bits = r == 0 ? rnd() : r == 1 ? f64_pats[rnd() % 6] : (i > 0 ? pts[i - 1].v[f].bits : 0);
                break;
            case CDB_TYPE_F32:
                r = rnd() % 4;
                v->bits = r == 0 ? (rnd() & 0xFFFFFFFFu) : r == 1 ? f32_pats[rnd() % 6] : (i > 0 ? pts[i - 1].v[f].bits : 0);
                break;
            case CDB_TYPE_BOOL:
                v->bits = rnd() & 1u;
                break;
            case CDB_TYPE_STRING:
                set_str(v, strs[rnd() % NSTR]);
                break;
            default:
                set_plain(v, oi, plain_kind(ty), (int)(rnd() % NPLAIN));
                break;
            }
        }
    }
}

static void run_oracle(int oi, size_t n, int check_each)
{
    cdb_ts_layout L;
    cdb_tsblock_state st_all, st_inc, st_chk;
    size_t len_all = 0, len_cur = 0, len_next = 0, k, len_chk = 0;
    uint8_t *cur = blk_a, *next = blk_b, *tmp;
    cdb_byte_order o = orders[oi];

    random_layout(&L);
    random_points(&L, oi, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len_all, &st_all));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 1, o, cur, BUFSZ, &len_cur, &st_inc));
    for (k = 1; k < n; k++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_append(cur, len_cur, o, &st_inc, &pts[k], next, BUFSZ, &len_next));
        tmp = cur;
        cur = next;
        next = tmp;
        len_cur = len_next;
        if (check_each) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, k + 1, o, blk_c, BUFSZ, &len_chk, &st_chk));
            TEST_ASSERT_EQUAL_size_t(len_chk, len_cur);
            TEST_ASSERT_EQUAL_MEMORY(blk_c, cur, len_cur);
            assert_state(&st_chk, &st_inc);
        }
    }
    TEST_ASSERT_EQUAL_size_t(len_all, len_cur);
    TEST_ASSERT_EQUAL_MEMORY(blk_all, cur, len_all);
    assert_state(&st_all, &st_inc);
    check_block(blk_all, len_all, o, &L, pts, n, &st_all);
}

static void test_append_matches_build(void)
{
    int oi, it;
    make_pools();
    rng_state = 12345;
    for (oi = 0; oi < 2; oi++) {
        for (it = 0; it < 150; it++)
            run_oracle(oi, 1 + (size_t)(rnd() % 40), 1);
        for (it = 0; it < 40; it++)
            run_oracle(oi, 1 + (size_t)(rnd() % 300), 0);
        run_oracle(oi, NMAX, 0);
    }
}

/* Appending to a block whose state came from decode works the same. */
static void test_append_after_decode(void)
{
    int oi;
    make_pools();
    rng_state = 777;
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L, dl;
        cdb_tsblock_state st_all, st;
        size_t n = 60, len_all, len_a, len_b, dn;
        cdb_byte_order o = orders[oi];

        random_layout(&L);
        random_points(&L, oi, n);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len_all, &st_all));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n - 1, o, blk_a, BUFSZ, &len_a, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_decode(blk_a, len_a, o, &dl, dec, NMAX, &dn, &st));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_append(blk_a, len_a, o, &st, &pts[n - 1], blk_b, BUFSZ, &len_b));
        TEST_ASSERT_EQUAL_size_t(len_all, len_b);
        TEST_ASSERT_EQUAL_MEMORY(blk_all, blk_b, len_b);
        assert_state(&st_all, &st);
    }
}

/* ---- overflow ------------------------------------------------------------ */

static void test_overflow(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        cdb_tsblock_state st, st0;
        size_t n, len = 0, len_a = 0, len_b = 0, cap;
        cdb_byte_order o = orders[oi];

        /* Page full: every capacity short of the block fails. */
        all_types_layout(&L);
        n = all_types_points(oi);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len, NULL));
        for (cap = 0; cap < len; cap++)
            TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_tsblock_build(&L, pts, n, o, blk_a, cap, &len_a, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_a, len, &len_a, NULL));
        TEST_ASSERT_EQUAL_size_t(len, len_a);

        /* Append into an out buffer that is too small leaves the state alone. */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n - 1, o, blk_a, BUFSZ, &len_a, &st0));
        for (cap = 0; cap < len; cap++) {
            st = st0;
            TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_tsblock_append(blk_a, len_a, o, &st, &pts[n - 1], blk_b, cap, &len_b));
            assert_state(&st0, &st);
        }
        st = st0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_append(blk_a, len_a, o, &st, &pts[n - 1], blk_b, len, &len_b));
        TEST_ASSERT_EQUAL_MEMORY(blk_all, blk_b, len);

        /* MAX_POINTS. */
        L.nfields = 1;
        memset(pts, 0, sizeof pts);
        for (n = 0; n <= NMAX; n++) {
            pts[n].ts = (int64_t)n;
            pts[n].v[0].present = 1;
            pts[n].v[0].bits = n;
        }
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, NMAX + 1, o, blk_a, BUFSZ, &len_a, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, NMAX, o, blk_a, BUFSZ, &len_a, &st0));
        st = st0;
        TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_tsblock_append(blk_a, len_a, o, &st, &pts[NMAX], blk_b, BUFSZ, &len_b));
        assert_state(&st0, &st);
    }
}

/* ---- argument checks ----------------------------------------------------- */

static void test_build_rejects_bad_input(void)
{
    cdb_ts_layout L;
    cdb_tsblock_state st;
    size_t n, len;
    cdb_byte_order o = CDB_ORDER_LITTLE;
    static const uint8_t bad_utf8[2] = { 0xC3, 0x28 };

    make_pools();
    all_types_layout(&L);
    n = all_types_points(0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, &st));

    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, 0, o, blk_a, BUFSZ, &len, NULL));

    pts[5].ts = pts[4].ts; /* not strictly increasing */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[n - 1].ts = L.window_end; /* outside the window */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[0].ts = L.window_start - 1;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));

    n = all_types_points(0);
    pts[2].v[4].bytes = bad_utf8;
    pts[2].v[4].len = 2;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[1].v[5].bytes = plain_buf[0][1][3]; /* binary in a timestamp field */
    pts[1].v[5].len = plain_len[0][1][3];
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[1].v[5].len--; /* a cut value */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[0].v[2].bits = UINT64_C(0x100000000);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    n = all_types_points(0);
    pts[0].v[3].bits = 2;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));

    n = all_types_points(0);
    L.fields[3].id = L.fields[2].id;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    all_types_layout(&L);
    L.flags = 0x04;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));
    all_types_layout(&L);
    L.fields[0].type = CDB_TYPE_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_build(&L, pts, n, o, blk_a, BUFSZ, &len, NULL));

    /* Append: a timestamp not after the last one, or a bad state. */
    all_types_layout(&L);
    n = all_types_points(0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n - 1, o, blk_a, BUFSZ, &len, &st));
    {
        cdb_ts_point p = pts[n - 1];
        cdb_tsblock_state s2 = st;
        size_t ol;
        p.ts = pts[n - 2].ts;
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_tsblock_append(blk_a, len, o, &st, &p, blk_b, BUFSZ, &ol));
        s2.count++;
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                              cdb_tsblock_append(blk_a, len, o, &s2, &pts[n - 1], blk_b, BUFSZ, &ol));
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                              cdb_tsblock_append(blk_a, len, o, &st, &pts[n - 1], blk_a, BUFSZ, &ol));
    }
}

/* ---- rejection of corrupt and non-canonical blocks ----------------------- */

static cdb_status decode_any(const uint8_t *b, size_t len, cdb_byte_order o)
{
    cdb_ts_layout L;
    size_t n;
    return cdb_tsblock_decode(b, len, o, &L, dec, NMAX, &n, NULL);
}

static cdb_status parse_any(const uint8_t *b, size_t len, cdb_byte_order o)
{
    cdb_ts_layout L;
    return cdb_tsblock_parse(b, len, o, &L, NULL, NULL, NULL);
}

static size_t find(const uint8_t *hay, size_t hl, const void *needle, size_t nl)
{
    size_t i;
    for (i = 0; i + nl <= hl; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return i;
    TEST_FAIL_MESSAGE("pattern not found");
    return 0;
}

static void test_reject_truncated_and_trailing(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        size_t n, len, cut;
        cdb_byte_order o = orders[oi];

        all_types_layout(&L);
        n = all_types_points(oi);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len, NULL));
        for (cut = 0; cut < len; cut++) {
            memcpy(blk_a, blk_all, cut);
            TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, decode_any(blk_a, cut, o));
            TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, parse_any(blk_a, cut, o));
        }
        memcpy(blk_a, blk_all, len);
        blk_a[len] = 0;
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len + 1, o));
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, parse_any(blk_a, len + 1, o));
    }
}

/* Two points 0 and 1: the time column is '10' + 7 bits = 9 bits, so its
 * second byte has 7 padding bits. With one bool field present on both,
 * offsets are: ts bits at 44..45, field id 46, type 54, bitmap 55, bool
 * bit count 56, bool bits 60. */
static void test_reject_padding(void)
{
    int oi;
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        size_t len;
        cdb_byte_order o = orders[oi];

        memset(&L, 0, sizeof L);
        L.window_end = 100;
        L.nfields = 1;
        L.fields[0].id = 9;
        L.fields[0].type = CDB_TYPE_BOOL;
        memset(pts, 0, sizeof pts);
        pts[1].ts = 1;
        pts[0].v[0].present = pts[1].v[0].present = 1;
        pts[0].v[0].bits = 1;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 2, o, blk_all, BUFSZ, &len, NULL));
        TEST_ASSERT_EQUAL_size_t(61, len);
        TEST_ASSERT_EQUAL_UINT32(9, cdb_load_u32(blk_all + 40, o));
        TEST_ASSERT_EQUAL_HEX8(0xC0, blk_all[55]);
        TEST_ASSERT_EQUAL_HEX8(0x80, blk_all[60]);
        TEST_ASSERT_EQUAL_INT(CDB_OK, decode_any(blk_all, len, o));

        memcpy(blk_a, blk_all, len);
        blk_a[45] ^= 0x01; /* time column padding */
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, decode_any(blk_a, len, o));
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, parse_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[55] ^= 0x01; /* bitmap padding */
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[60] ^= 0x10; /* bool column padding */
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        cdb_store_u32(blk_a + 56, 3, o); /* bit count claims a padding bit */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[54] = CDB_TYPE_UNKNOWN;
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[31] = 0x04; /* unknown flag */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        cdb_store_u32(blk_a + 24, 0, o); /* count 0 */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        cdb_store_u16(blk_a + 28, CDB_TS_MAX_FIELDS + 1, o);
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
    }
}

/* A hand-made block with no fields and two timestamps, the second encoded
 * with the codec whatever its order. */
static size_t craft_ts_block(cdb_byte_order o, int64_t ws, int64_t we, int64_t t0, int64_t t1, uint8_t *out)
{
    cdb_bitw w;
    cdb_tsb_time s;

    memset(out, 0, 64);
    cdb_store_u64(out + 8, (uint64_t)ws, o);
    cdb_store_u64(out + 16, (uint64_t)we, o);
    cdb_store_u32(out + 24, 2, o);
    cdb_store_u64(out + 32, (uint64_t)t0, o);
    cdb_bitw_init(&w, out + 44, 16);
    cdb_tsb_time_init(&s, t0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsb_time_encode(&w, &s, t1));
    cdb_store_u32(out + 40, (uint32_t)w.bits, o);
    return 44 + cdb_bitw_bytes(&w);
}

static void test_reject_timestamps(void)
{
    int oi;
    for (oi = 0; oi < 2; oi++) {
        cdb_byte_order o = orders[oi];
        size_t len;

        len = craft_ts_block(o, 0, 1000, 100, 200, blk_a);
        TEST_ASSERT_EQUAL_INT(CDB_OK, decode_any(blk_a, len, o));
        len = craft_ts_block(o, 0, 1000, 100, 50, blk_a); /* unsorted */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, parse_any(blk_a, len, o));
        len = craft_ts_block(o, 0, 1000, 100, 100, blk_a); /* duplicate */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        len = craft_ts_block(o, 0, 1000, 100, 1000, blk_a); /* past the window */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        len = craft_ts_block(o, 0, 1000, -1, 10, blk_a); /* before the window */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        len = craft_ts_block(o, 1000, 1000, 1000, 1001, blk_a); /* empty window */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
    }
}

static size_t string_block(cdb_byte_order o, const char *a, const char *b)
{
    cdb_ts_layout L;
    size_t len;

    memset(&L, 0, sizeof L);
    L.window_end = 100;
    L.nfields = 1;
    L.fields[0].id = 1;
    L.fields[0].type = CDB_TYPE_STRING;
    memset(pts, 0, sizeof pts);
    pts[1].ts = 1;
    set_str(&pts[0].v[0], a);
    set_str(&pts[1].v[0], b);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 2, o, blk_all, BUFSZ, &len, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, decode_any(blk_all, len, o));
    return len;
}

static void test_reject_dictionary(void)
{
    int oi;
    for (oi = 0; oi < 2; oi++) {
        cdb_byte_order o = orders[oi];
        size_t len, at, nl;

        /* Duplicate entry: "a","b" -> "a","a" with indexes still saying new, new. */
        len = string_block(o, "a", "b");
        at = find(blk_all, len, "\x01" "a" "\x01" "b", 4);
        TEST_ASSERT_EQUAL_UINT32(2, cdb_load_u32(blk_all + at - 8, o));
        memcpy(blk_a, blk_all, len);
        blk_a[at + 3] = 'a';
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, decode_any(blk_a, len, o));
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, parse_any(blk_a, len, o));

        /* Invalid UTF-8 in an entry. */
        memcpy(blk_a, blk_all, len);
        blk_a[at + 3] = 0xFF;
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));

        /* Entry count disagrees with the entries. */
        memcpy(blk_a, blk_all, len);
        cdb_store_u32(blk_a + at - 8, 3, o);
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));

        /* An entry that is never used: "a","a" with an extra entry "z". */
        len = string_block(o, "a", "a");
        at = find(blk_all, len, "\x01" "a", 2);
        TEST_ASSERT_EQUAL_UINT32(1, cdb_load_u32(blk_all + at - 8, o));
        memcpy(blk_a, blk_all, at);
        cdb_store_u32(blk_a + at - 8, 2, o);
        cdb_store_u32(blk_a + at - 4, 4, o);
        memcpy(blk_a + at, "\x01" "a" "\x01" "z", 4);
        memcpy(blk_a + at + 4, blk_all + at + 2, len - at - 2);
        nl = len + 2;
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_CANONICAL, decode_any(blk_a, nl, o));

        /* An index that says "new" past the end of the dictionary: the second
         * value of "a","a" is index 0 (1 bit); make it 1. */
        memcpy(blk_a, blk_all, len);
        blk_a[len - 1] = 0x80;
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
    }
}

static void test_reject_plain_and_ids(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L;
        size_t len, at;
        cdb_byte_order o = orders[oi];
        uint8_t key[9];

        /* Two fields: ids 4 (bool) then 6 (binary), one point. */
        memset(&L, 0, sizeof L);
        L.window_end = 100;
        L.nfields = 2;
        L.fields[0].id = 4;
        L.fields[0].type = CDB_TYPE_BOOL;
        L.fields[1].id = 6;
        L.fields[1].type = CDB_TYPE_BINARY;
        memset(pts, 0, sizeof pts);
        pts[0].v[0].present = 1;
        set_plain(&pts[0].v[1], oi, 1, 3);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, 1, o, blk_all, BUFSZ, &len, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, decode_any(blk_all, len, o));
        cdb_store_u64(key, 6, o);
        key[8] = CDB_TYPE_BINARY;
        at = find(blk_all, len, key, 9);

        memcpy(blk_a, blk_all, len);
        cdb_store_u64(blk_a + at, 4, o); /* ids not strictly increasing */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[at + 8] = CDB_TYPE_TIMESTAMP; /* value of the wrong type */
        TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, decode_any(blk_a, len, o));
        memcpy(blk_a, blk_all, len);
        blk_a[at + 9] = 0; /* presence says absent, but the bytes remain */
        TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, decode_any(blk_a, len, o));
    }
}

/* Every single-bit flip either fails to decode, or decodes to points that
 * build back to exactly the flipped bytes (one canonical form). */
static void test_bit_flips_canonical(void)
{
    int oi;
    make_pools();
    for (oi = 0; oi < 2; oi++) {
        cdb_ts_layout L, dl;
        size_t n, len, bit, dn, rl;
        cdb_byte_order o = orders[oi];
        static cdb_ts_point dp[NMAX];

        all_types_layout(&L);
        n = all_types_points(oi);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&L, pts, n, o, blk_all, BUFSZ, &len, NULL));
        for (bit = 0; bit < len * 8u; bit++) {
            memcpy(blk_a, blk_all, len);
            blk_a[bit >> 3] = (uint8_t)(blk_a[bit >> 3] ^ (0x80u >> (bit & 7u)));
            if (cdb_tsblock_decode(blk_a, len, o, &dl, dp, NMAX, &dn, NULL) != CDB_OK)
                continue;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tsblock_build(&dl, dp, dn, o, blk_b, BUFSZ, &rl, NULL));
            TEST_ASSERT_EQUAL_size_t(len, rl);
            TEST_ASSERT_EQUAL_MEMORY(blk_a, blk_b, len);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_decode_all_types);
    RUN_TEST(test_single_point_and_empty_fields);
    RUN_TEST(test_append_matches_build);
    RUN_TEST(test_append_after_decode);
    RUN_TEST(test_overflow);
    RUN_TEST(test_build_rejects_bad_input);
    RUN_TEST(test_reject_truncated_and_trailing);
    RUN_TEST(test_reject_padding);
    RUN_TEST(test_reject_timestamps);
    RUN_TEST(test_reject_dictionary);
    RUN_TEST(test_reject_plain_and_ids);
    RUN_TEST(test_bit_flips_canonical);
    return UNITY_END();
}
