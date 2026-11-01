/* Nibblets (spec 16): small records share pages, large ones keep their own,
 * freed space is reused, merging moves records and keeps their owners
 * pointing at them, and all of it survives crashes. */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_page.h"
#include "cdb_ts.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define SERIES 24u

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
static uint8_t page[PAGE];
static cdb_db_config cfg;
static cdb_db db;

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
    cfg.durability = CDB_DURABILITY_FULL;
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x4E, sizeof h.db_id);
    return cdb_db_open(&db, &env, "n", &cfg, &h);
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }

/* Live pages of the given type, by reading every logical page ever handed out. */
static unsigned pages_of(uint16_t type)
{
    uint64_t lid;
    unsigned n = 0;
    for (lid = 1; lid < db.pager.next_lid; lid++) {
        cdb_page_header h;
        if (cdb_pager_read(&db.pager, lid, page, &h) == CDB_OK && h.type == type)
            n++;
    }
    return n;
}

/* A document { 1: "doc <i>", 2: i } or, with big, a 400-byte string. */
typedef struct doc {
    uint8_t b[512];
    size_t n;
} doc;

static doc make_doc(unsigned i, int big)
{
    doc d;
    char s[420];
    cdb_value_writer w;
    if (big) {
        memset(s, 'a' + (int)(i % 26u), 400);
        s[400] = 0;
    } else {
        snprintf(s, sizeof s, "doc %u", i);
    }
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, s, strlen(s));
    cdb_value_field(&w, 2);
    cdb_value_write_int(&w, (int64_t)i);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static void expect_doc(uint64_t pool, uint64_t entry, unsigned i, int big)
{
    uint8_t out[PAGE];
    size_t len;
    doc want = make_doc(i, big);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(want.n, len);
    TEST_ASSERT_EQUAL_MEMORY(want.b, out, len);
}

/* Small documents share nibblet pages; a large one gets a page of its own.
 * Both read back, before and after reopening. */
static void test_small_records_share_pages(void)
{
    uint64_t pool, e[60], big;
    unsigned i, nib;
    doc d;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "p", 1, CDB_MODEL_DOCUMENT, &pool));
    begin();
    for (i = 0; i < 60; i++) {
        d = make_doc(i, 0);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, d.b, d.n, &e[i]));
    }
    d = make_doc(99, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, d.b, d.n, &big));
    commit();
    nib = pages_of(CDB_PAGE_TYPE_NIBBLET);
    TEST_ASSERT_TRUE(nib >= 1 && nib <= 8); /* 60 revisions of ~70 bytes */
    TEST_ASSERT_EQUAL_UINT(1, pages_of(CDB_PAGE_TYPE_DOCUMENT)); /* the large one */
    for (i = 0; i < 60; i++)
        expect_doc(pool, e[i], i, 0);
    expect_doc(pool, big, 99, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    for (i = 0; i < 60; i++)
        expect_doc(pool, e[i], i, 0);
    expect_doc(pool, big, 99, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    /* A stale nibblet record whose owner now points elsewhere is reported as
     * CDB_E_NOT_FOUND by *_nib_moved, leaving the current address intact. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    {
        uint64_t pool2, entry;
        uint8_t okey[16];
        d = make_doc(7, 0);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "st", 2, CDB_MODEL_DOCUMENT, &pool2));
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool2, NULL, 0, d.b, d.n, &entry));
        commit();
        memset(okey, 0, sizeof okey);
        okey[7] = (uint8_t)entry;
        okey[15] = 1u; /* revision 1 */
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND,
                              cdb_doc_nib_moved(&db, pool2, okey, sizeof okey, (UINT64_C(1) << 63) | 9999u,
                                                (UINT64_C(1) << 63) | 8888u));
        expect_doc(pool2, entry, 7, 0);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- time series */

static cdb_ts_def def1(void)
{
    cdb_ts_def d;
    memset(&d, 0, sizeof d);
    d.precision = CDB_TS_MILLIS;
    d.window = 60000;
    d.nfields = 1;
    d.fields[0].id = 1;
    d.fields[0].type = CDB_TYPE_INT;
    return d;
}

static cdb_status append(uint64_t store, uint64_t series, int64_t ts, int64_t v)
{
    uint8_t enc[64]; /* room for the writer's staged directory entry too */
    cdb_value_writer w;
    cdb_ts_in in;
    cdb_value_writer_init(&w, enc, sizeof enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, v);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    in.ts = ts;
    in.use_clock = 0;
    in.values = enc;
    in.len = cdb_value_writer_len(&w);
    return cdb_ts_append(&db, store, series, &in, 1);
}

/* How many points the series has (all of them read without error). */
static size_t points(uint64_t store, uint64_t series)
{
    uint8_t buf[PAGE];
    cdb_ts_row rows[8];
    size_t n;
    int64_t next;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, series, INT64_MIN, INT64_MAX, buf, sizeof buf, rows, 8,
                                               &n, &next));
    return n;
}

static uint64_t ts_store, series_ids[SERIES];

/* SERIES sparse series of two points each: every block is a few dozen
 * bytes, so they all share nibblet pages. Returns the first failure, so
 * the crash workload can use it. */
static cdb_status fill_sparse(void)
{
    cdb_ts_def d = def1();
    unsigned i;
    int created;
    cdb_status s;
    if ((s = cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &ts_store)) != CDB_OK
        || (s = cdb_tx_begin(&db, CDB_DURABILITY_FULL)) != CDB_OK)
        return s;
    for (i = 0; i < SERIES; i++) {
        char name[8];
        snprintf(name, sizeof name, "s%u", i);
        if ((s = cdb_ts_series(&db, ts_store, name, strlen(name), NULL, 0, &d, &series_ids[i], &created)) != CDB_OK
            || (s = append(ts_store, series_ids[i], 1000, (int64_t)i)) != CDB_OK
            || (s = append(ts_store, series_ids[i], 120000, (int64_t)i + 1)) != CDB_OK)
            return s;
    }
    return cdb_tx_commit(&db);
}

/* Dropping most series' blocks leaves nibblet pages under half full;
 * reconciliation merges them, and every remaining block still reads. */
static void test_freed_space_is_merged(void)
{
    unsigned before, after, i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, fill_sparse());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    before = pages_of(CDB_PAGE_TYPE_NIBBLET);
    TEST_ASSERT_TRUE(before >= 2);
    TEST_ASSERT_EQUAL_UINT(0, pages_of(CDB_PAGE_TYPE_TS_BLOCK));
    begin();
    for (i = 0; i < SERIES; i++)
        if (i % 4 != 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_drop_before(&db, ts_store, series_ids[i], INT64_MAX));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    after = pages_of(CDB_PAGE_TYPE_NIBBLET);
    TEST_ASSERT_TRUE(after < before);
    for (i = 0; i < SERIES; i++)
        TEST_ASSERT_EQUAL_size_t(i % 4 == 0 ? 2u : 0u, points(ts_store, series_ids[i]));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    for (i = 0; i < SERIES; i++)
        TEST_ASSERT_EQUAL_size_t(i % 4 == 0 ? 2u : 0u, points(ts_store, series_ids[i]));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A block that grows past a quarter page leaves its nibblet for a page of
 * its own. */
static void test_block_graduates(void)
{
    cdb_ts_def d = def1();
    uint64_t store, s;
    int created;
    unsigned i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "g", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "s", 1, NULL, 0, &d, &s, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, append(store, s, 1000, 1));
    commit();
    TEST_ASSERT_EQUAL_UINT(0, pages_of(CDB_PAGE_TYPE_TS_BLOCK));
    begin();
    for (i = 1; i < 200; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, append(store, s, 1000 + (int64_t)i * 37, (int64_t)i * 1000003));
    commit();
    TEST_ASSERT_TRUE(pages_of(CDB_PAGE_TYPE_TS_BLOCK) >= 1);
    {
        uint8_t buf[8u * PAGE];
        cdb_ts_row rows[256];
        size_t n;
        int64_t next;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, s, INT64_MIN, INT64_MAX, buf, sizeof buf, rows, 256,
                                                   &n, &next));
        TEST_ASSERT_EQUAL_size_t(200, n);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* Fill, reconcile, drop three quarters, reconcile (which merges). After a
 * crash anywhere, every series reads without error, and holds its points
 * unless its drop was acknowledged. */
static cdb_status workload(int *acked)
{
    unsigned i;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(fill_sparse());
    TRY(cdb_db_reconcile(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < SERIES; i++)
        if (i % 4 != 0)
            TRY(cdb_ts_drop_before(&db, ts_store, series_ids[i], INT64_MAX));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
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
    for (m = 0; m < 4; m++)
        for (op = 1; op <= total; op++) {
            unsigned i;
            init_env();
            cdb_faultenv_arm(&fe, op, modes[m], 1, 300);
            (void)workload(&acked);
            cdb_faultenv_reboot(&fe);
            if (open_db() != CDB_OK) {
                TEST_ASSERT_EQUAL_INT(0, acked); /* nothing acknowledged: nothing promised */
                continue;
            }
            if (acked >= 1) {
                for (i = 0; i < SERIES; i++) {
                    size_t n = points(ts_store, series_ids[i]);
                    if (acked == 2 && i % 4 != 0)
                        TEST_ASSERT_EQUAL_size_t(0, n);
                    else if (i % 4 == 0)
                        TEST_ASSERT_EQUAL_size_t(2, n);
                    else
                        TEST_ASSERT_TRUE(n == 0 || n == 2);
                }
            }
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
        }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_small_records_share_pages);
    RUN_TEST(test_freed_space_is_merged);
    RUN_TEST(test_block_graduates);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
