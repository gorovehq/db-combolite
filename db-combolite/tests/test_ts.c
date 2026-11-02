#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_ts.h"

#define FCAP (1536u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define MAXP 4000u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static uint8_t record_buf[2u * PAGE];
static uint8_t cat_work[24u * 1024u];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t ts_buffers[3u * PAGE];
static uint8_t ts_work[(CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point)];
static cdb_db_config cfg;
static cdb_db db;

static unsigned collisions;
static int64_t last_collision;

static void on_collision(void *ctx, uint64_t store, uint64_t series, int64_t ts)
{
    (void)ctx;
    (void)store;
    (void)series;
    collisions++;
    last_collision = ts;
}

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 8192;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

void setUp(void)
{
    init_env();
    memset(&cfg, 0, sizeof cfg);
    {
        const char *cap = getenv("CDB_TEST_MAX_OPEN"); /* the cap-1 runs (tests/CMakeLists.txt) */
        cfg.max_open = cap != NULL ? (unsigned)atoi(cap) : 0;
    }
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof arena;
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof free_list / sizeof free_list[0];
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof record_buf;
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof cat_work;
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 512;
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_buffers;
    cfg.ts_buffers = ts_buffers;
    cfg.ts_work = ts_work;
    cfg.ts_work_size = sizeof ts_work;
    cfg.ts_collision = on_collision;
    cfg.durability = CDB_DURABILITY_FULL;
    collisions = 0;
    TEST_ASSERT_EQUAL_size_t(cdb_ts_work_size(), sizeof ts_work);
}

void tearDown(void) {}

/* The test uses n stores of one model in a single transaction: raise a
 * smaller cap (the cap-1 runs) to n, which that transaction needs. */
static void need_open(unsigned n)
{
    if (cfg.max_open != 0 && cfg.max_open < n)
        cfg.max_open = n;
}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x7E, sizeof h.db_id);
    return cdb_db_open(&db, &env, "t", &cfg, &h);
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }

/* ------------------------------------------------------------ points */

/* Fields: 1 int, 2 f64, 3 string, 4 bool. A point carries field 1 always;
 * 2 when has_f; 3 when s is not NULL; 4 when b >= 0. */
typedef struct pt {
    int64_t ts;
    uint8_t enc[192];
    size_t n;
} pt;

static pt make(int64_t ts, int64_t i, int has_f, double f, const char *s, int b)
{
    pt p;
    cdb_value_writer w;
    p.ts = ts;
    cdb_value_writer_init(&w, p.enc, sizeof p.enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, i);
    if (has_f) {
        cdb_value_field(&w, 2);
        cdb_value_write_f64(&w, f);
    }
    if (s) {
        cdb_value_field(&w, 3);
        cdb_value_write_string(&w, s, strlen(s));
    }
    if (b >= 0) {
        cdb_value_field(&w, 4);
        cdb_value_write_bool(&w, b);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    p.n = cdb_value_writer_len(&w);
    return p;
}

/* The point an index stands for, in the long tests. */
static pt nth(int64_t ts, unsigned k)
{
    static const char *names[4] = { "alpha", "beta", "gamma", "delta" };
    return make(ts, (int64_t)k * 7 - 300, k % 3 != 0, (double)k * 0.25, k % 5 ? names[k % 4] : NULL, (int)(k % 2));
}

static cdb_ts_def def4(int cov)
{
    cdb_ts_def d;
    memset(&d, 0, sizeof d);
    d.precision = CDB_TS_MILLIS;
    d.window = 60000; /* a minute */
    d.change_of_value = cov;
    d.nfields = 4;
    d.fields[0].id = 1;
    d.fields[0].type = CDB_TYPE_INT;
    d.fields[1].id = 2;
    d.fields[1].type = CDB_TYPE_F64;
    d.fields[2].id = 3;
    d.fields[2].type = CDB_TYPE_STRING;
    d.fields[3].id = 4;
    d.fields[3].type = CDB_TYPE_BOOL;
    return d;
}

static uint64_t series_of(uint64_t store, const char *name, int cov)
{
    cdb_ts_def d = def4(cov);
    uint64_t s;
    int created;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, name, strlen(name), NULL, 0, &d, &s, &created));
    return s;
}

static cdb_status append1(uint64_t store, uint64_t series, const pt *p)
{
    cdb_ts_in in;
    in.ts = p->ts;
    in.use_clock = 0;
    in.values = p->enc;
    in.len = p->n;
    return cdb_ts_append(&db, store, series, &in, 1);
}

/* The model: what a range read must return. */
static pt model[MAXP];
static size_t model_n;

static void model_put(const pt *p)
{
    size_t i;
    for (i = 0; i < model_n && model[i].ts < p->ts; i++)
        ;
    if (i < model_n && model[i].ts == p->ts) {
        model[i] = *p;
        return;
    }
    memmove(&model[i + 1], &model[i], (model_n - i) * sizeof model[0]);
    model[i] = *p;
    model_n++;
}

/* Reads [from, to) in pages of `page` rows and checks it against the model. */
static void expect_range(uint64_t store, uint64_t series, int64_t from, int64_t to, size_t page)
{
    static uint8_t buf[64u * 1024u];
    static cdb_ts_row rows[MAXP];
    size_t got = 0, i = 0, n;
    int64_t at = from, next;
    while (i < model_n && model[i].ts < from)
        i++;
    for (;;) {
        size_t k;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, series, at, to, buf, sizeof buf, rows, page, &n, &next));
        for (k = 0; k < n; k++, i++, got++) {
            char msg[64];
            snprintf(msg, sizeof msg, "point %u", (unsigned)got);
            TEST_ASSERT_TRUE_MESSAGE(i < model_n && model[i].ts < to, msg);
            TEST_ASSERT_EQUAL_INT64_MESSAGE(model[i].ts, rows[k].ts, msg);
            TEST_ASSERT_EQUAL_size_t_MESSAGE(model[i].n, rows[k].len, msg);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(model[i].enc, buf + rows[k].off, rows[k].len, msg);
        }
        if (next == to)
            break;
        TEST_ASSERT_TRUE(n > 0);
        at = next;
    }
    TEST_ASSERT_TRUE_MESSAGE(i == model_n || model[i].ts >= to, "points missing from the range");
}

/* ------------------------------------------------------------ tests */

static void test_series(void)
{
    cdb_ts_tag a[2] = { { "repo", 4, "combolite", 9 }, { "branch", 6, "main", 4 } };
    cdb_ts_tag b[2] = { { "branch", 6, "main", 4 }, { "repo", 4, "combolite", 9 } };
    cdb_ts_tag dup[2] = { { "repo", 4, "x", 1 }, { "repo", 4, "y", 1 } };
    cdb_ts_def d = def4(0), got;
    uint64_t store, s1, s2, s3;
    int created;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "metrics", 7, CDB_TS_LATE_ACCEPT, 0, &store));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_ts_series(&db, store, "build", 5, a, 2, NULL, &s1, &created));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "build", 5, a, 2, &d, &s1, &created));
    TEST_ASSERT_TRUE(created);
    /* Tag order does not matter, as the transaction already sees. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "build", 5, b, 2, &d, &s2, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT64(s1, s2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "build", 5, a, 1, &d, &s3, &created));
    TEST_ASSERT_TRUE(created && s3 != s1);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_ts_series(&db, store, "build", 5, dup, 2, &d, &s3, &created));
    d.fields[1].id = 1; /* not increasing */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_ts_series(&db, store, "other", 5, NULL, 0, &d, &s3, &created));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "build", 5, b, 2, NULL, &s2, &created));
    TEST_ASSERT_EQUAL_UINT64(s1, s2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_describe(&db, store, s1, &got));
    TEST_ASSERT_EQUAL_UINT8(CDB_TS_MILLIS, got.precision);
    TEST_ASSERT_EQUAL_INT64(60000, got.window);
    TEST_ASSERT_EQUAL_UINT16(4, got.nfields);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "build", 5, a, 2, NULL, &s2, &created));
    TEST_ASSERT_EQUAL_UINT64(s1, s2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Thousands of points: many blocks and windows, read back in pages, also
 * after reopening. Bad points are refused. */
static void test_append_and_range(void)
{
    uint64_t store, s;
    unsigned k;
    pt bad;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    begin();
    s = series_of(store, "cpu", 0);
    commit();
    model_n = 0;
    for (k = 0; k < 3000; k++) {
        pt p = nth(INT64_C(1700000000000) + (int64_t)k * 250 + (k % 7 == 0 ? 3 : 0), k);
        if (k % 100 == 0)
            begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(store, s, &p));
        model_put(&p);
        if (k % 100 == 99)
            commit();
    }
    expect_range(store, s, INT64_MIN, INT64_MAX, 3000);
    expect_range(store, s, INT64_MIN, INT64_MAX, 37);
    expect_range(store, s, model[500].ts, model[2500].ts, 64);
    expect_range(store, s, model[0].ts + 1, model[0].ts + 2, 10); /* nothing there */
    /* A wrong type, an unknown field: refused, nothing journaled. */
    {
        cdb_value_writer w;
        bad.ts = model[model_n - 1].ts + 1;
        cdb_value_writer_init(&w, bad.enc, sizeof bad.enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, "x", 1);
        cdb_value_end_object(&w);
        bad.n = cdb_value_writer_len(&w);
    }
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, append1(store, s, &bad));
    {
        cdb_value_writer w;
        cdb_value_writer_init(&w, bad.enc, sizeof bad.enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 9);
        cdb_value_write_int(&w, 1);
        cdb_value_end_object(&w);
        bad.n = cdb_value_writer_len(&w);
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, append1(store, s, &bad));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    expect_range(store, s, INT64_MIN, INT64_MAX, 500);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Late points in each mode; duplicates and collisions. */
static void test_late_points(void)
{
    uint64_t acc, within, rej, held, sa, sw, sr, sh;
    unsigned k;
    need_open(4);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "acc", 3, CDB_TS_LATE_ACCEPT, 0, &acc));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "win", 3, CDB_TS_LATE_WITHIN, INT64_C(5000000000), &within));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "rej", 3, CDB_TS_LATE_REJECT, 0, &rej));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "hold", 4, CDB_TS_LATE_HOLD, 0, &held));
    begin();
    sa = series_of(acc, "s", 0);
    sw = series_of(within, "s", 0);
    sr = series_of(rej, "s", 0);
    sh = series_of(held, "s", 0);
    for (k = 0; k < 40; k++) {
        pt p = nth(100000 + (int64_t)k * 1000, k);
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(acc, sa, &p));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(within, sw, &p));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(rej, sr, &p));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(held, sh, &p));
    }
    commit();
    model_n = 0;
    for (k = 0; k < 40; k++) {
        pt p = nth(100000 + (int64_t)k * 1000, k);
        model_put(&p);
    }
    begin();
    {
        pt late = nth(100500, 999), old = nth(10000, 998), dup = nth(100000 + 5000, 5);
        pt clash = make(100000 + 6000, 4242, 0, 0, NULL, -1);
        /* accept: anything; within 5 s: 100500 ms is 38.5 s late, refused. */
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(acc, sa, &late));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(acc, sa, &old));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(acc, sa, &dup));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(acc, sa, &clash));
        TEST_ASSERT_EQUAL_INT(CDB_E_STATE, append1(within, sw, &late));
        {
            pt near = nth(100000 + 36500, 997); /* 2.5 s before the last point, a new timestamp */
            TEST_ASSERT_EQUAL_INT(CDB_OK, append1(within, sw, &near));
        }
        TEST_ASSERT_EQUAL_INT(CDB_E_STATE, append1(rej, sr, &late));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(held, sh, &late));
        commit();
        expect_range(rej, sr, INT64_MIN, INT64_MAX, 100);
        /* Held: invisible until reconciliation folds it in. */
        expect_range(held, sh, INT64_MIN, INT64_MAX, 100);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
        model_put(&late);
        expect_range(held, sh, INT64_MIN, INT64_MAX, 100);
        /* Accepted: merged; the duplicate changed nothing; the collision
         * replaced field 1 and kept the others. */
        model_put(&old);
        {
            /* nth(6): no f64 (6 % 3 == 0), string "gamma", bool 0; the clash replaced field 1 only. */
            pt want = make(100000 + 6000, 4242, 0, 0, "gamma", 0);
            model_put(&want);
        }
        TEST_ASSERT_EQUAL_UINT(1, collisions);
        TEST_ASSERT_EQUAL_INT64(106000, last_collision);
        expect_range(acc, sa, INT64_MIN, INT64_MAX, 100);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* With room for one open store, a store holding a late point stays open
 * until reconciliation folds the point in; then stores take turns. */
static void test_held_store_stays_open(void)
{
    uint64_t h, a, sh, sa, s;
    pt p1 = nth(1000, 1), p2 = nth(2000, 2), late = nth(1500, 3);
    cdb_ts_def d = def4(0);
    int created;
    cfg.max_open = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "h", 1, CDB_TS_LATE_HOLD, 0, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "a", 1, CDB_TS_LATE_ACCEPT, 0, &a));
    begin();
    sh = series_of(h, "s", 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, sh, &p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, sh, &p2));
    commit();
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, sh, &late)); /* held */
    commit();
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_ts_series(&db, a, "s", 1, NULL, 0, &d, &s, &created));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db)); /* folds it in */
    begin();
    sa = series_of(a, "s", 0); /* now h may be closed */
    TEST_ASSERT_EQUAL_INT(CDB_OK, append1(a, sa, &p1));
    commit();
    model_n = 0;
    model_put(&p1);
    model_put(&late);
    model_put(&p2);
    expect_range(h, sh, INT64_MIN, INT64_MAX, 100); /* reopens h, closing a */
    TEST_ASSERT_NULL(cdb_slots_find(&db.ts_slots, a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_change_of_value_and_drop(void)
{
    uint64_t store, s, h, hs;
    unsigned k;
    need_open(2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "h", 1, CDB_TS_LATE_HOLD, 0, &h));
    begin();
    s = series_of(store, "temp", 1);
    hs = series_of(h, "temp", 0);
    model_n = 0;
    for (k = 0; k < 200; k++) {
        pt p = make(1000 + (int64_t)k * 10, (int64_t)(k / 10), 0, 0, NULL, -1); /* changes every 10 points */
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(store, s, &p));
        if (k % 10 == 0)
            model_put(&p);
    }
    commit();
    expect_range(store, s, INT64_MIN, INT64_MAX, 1000);
    /* Dropping before a point inside a block keeps the rest. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_drop_before(&db, store, s, 1000 + 75 * 10));
    commit();
    while (model_n && model[0].ts < 1750) {
        memmove(model, model + 1, (model_n - 1) * sizeof model[0]);
        model_n--;
    }
    expect_range(store, s, INT64_MIN, INT64_MAX, 1000);
    /* Dropping everything, then appending again. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_drop_before(&db, store, s, INT64_MAX));
    commit();
    model_n = 0;
    expect_range(store, s, INT64_MIN, INT64_MAX, 1000);
    {
        pt p = make(5, 1, 0, 0, NULL, -1);
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(store, s, &p));
        commit();
        model_put(&p);
        expect_range(store, s, INT64_MIN, INT64_MAX, 1000);
    }
    /* Held points older than a drop go with it. */
    {
        pt a = make(100, 1, 0, 0, NULL, -1), b = make(200, 2, 0, 0, NULL, -1), late = make(50, 3, 0, 0, NULL, -1);
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, hs, &a));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, hs, &b));
        commit();
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(h, hs, &late));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_drop_before(&db, h, hs, 150));
        commit();
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
        model_n = 0;
        model_put(&b);
        expect_range(h, hs, INT64_MIN, INT64_MAX, 10);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_clock(void)
{
    uint64_t store, s;
    cdb_ts_in in;
    pt p = make(0, 1, 0, 0, NULL, -1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    begin();
    s = series_of(store, "x", 0);
    in.ts = 0;
    in.use_clock = 1;
    in.values = p.enc;
    in.len = p.n;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_ts_append(&db, store, s, &in, 1)); /* no clock */
    cdb_faultenv_set_time(&fe, 1, INT64_C(1761768000123456789));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, s, &in, 1));
    commit();
    model_n = 0;
    p.ts = INT64_C(1761768000123); /* milliseconds */
    model_put(&p);
    expect_range(store, s, INT64_MIN, INT64_MAX, 10);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_nibblet_inplace_rewrite(void)
{
    uint64_t store = 0, s;
    pt p;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "inplace", 7, CDB_TS_LATE_ACCEPT, 0, &store));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    s = series_of(store, "s1", 0);
    commit();

    model_n = 0;
    /* Append 10 points one-by-one in separate commits */
    for (i = 0; i < 10; i++) {
        p = nth((int64_t)(1000 + i * 100), i);
        model_put(&p);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, append1(store, s, &p));
        commit();
    }

    expect_range(store, s, INT64_MIN, INT64_MAX, 10);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: series with points 0..59 (two windows). S2: a late point
 * merged and a collision. S3: points before 30 dropped. */
static uint64_t sw_store, sw_series;

static cdb_status workload(int *acked)
{
    cdb_ts_def d = def4(0);
    cdb_ts_in in[60];
    pt p[60], late, clash;
    int created;
    unsigned k;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &sw_store));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_ts_series(&db, sw_store, "s", 1, NULL, 0, &d, &sw_series, &created));
    for (k = 0; k < 60; k++) {
        p[k] = nth(30000 + (int64_t)k * 1000, k);
        in[k].ts = p[k].ts;
        in[k].use_clock = 0;
        in[k].values = p[k].enc;
        in[k].len = p[k].n;
    }
    for (k = 0; k < 60; k += 10) /* a batch is one journal record: at most a page */
        TRY(cdb_ts_append(&db, sw_store, sw_series, in + k, 10));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    late = nth(30500, 500);
    clash = make(31000, 77, 0, 0, NULL, -1);
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(append1(sw_store, sw_series, &late));
    TRY(append1(sw_store, sw_series, &clash));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_ts_drop_before(&db, sw_store, sw_series, 30000 + 30 * 1000));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static void sweep_model(int k)
{
    unsigned i;
    model_n = 0;
    if (k == 0)
        return;
    for (i = 0; i < 60; i++) {
        pt p = nth(30000 + (int64_t)i * 1000, i);
        if (k < 3 || i >= 30)
            model_put(&p);
    }
    if (k == 2) {
        pt late = nth(30500, 500), want = make(31000, 77, 1, 0.25, "beta", 1);
        model_put(&late);
        model_put(&want);
    }
}

/* Which state the database is in: -1 when none matches. */
static int observed(void)
{
    static uint8_t buf[16384];
    static cdb_ts_row rows[128];
    uint64_t store, series;
    size_t n;
    int64_t next;
    int found, k;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "m", 1, &store, &found));
    if (!found || cdb_ts_series(&db, store, "s", 1, NULL, 0, NULL, &series, &found) == CDB_E_NOT_FOUND)
        return 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, series, INT64_MIN, INT64_MAX, buf, sizeof buf, rows, 128, &n,
                                               &next));
    for (k = 0; k <= 3; k++) {
        size_t i;
        int ok;
        sweep_model(k);
        ok = n == model_n;
        for (i = 0; ok && i < n; i++)
            ok = rows[i].ts == model[i].ts && rows[i].len == model[i].n
                 && memcmp(buf + rows[i].off, model[i].enc, model[i].n) == 0;
        if (ok)
            return k;
    }
    return -1;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[2] = { 0, 300 };
    uint64_t total, op;
    int acked, m;

    init_env();
    TEST_ASSERT_EQUAL_INT(CDB_OK, workload(&acked));
    total = fe.clock.ops;
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }
    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 2 : 1;
            for (t = 0; t < nt; t++) {
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultenv_reboot(&fe);
                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(), "recovery open failed");
                k = observed();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "points in a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_series);
    RUN_TEST(test_append_and_range);
    RUN_TEST(test_late_points);
    RUN_TEST(test_held_store_stays_open);
    RUN_TEST(test_change_of_value_and_drop);
    RUN_TEST(test_clock);
    RUN_TEST(test_nibblet_inplace_rewrite);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
