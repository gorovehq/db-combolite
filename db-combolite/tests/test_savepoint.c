/* Savepoints and rollback records (spec 11.12.3, 14.4.9): what a rollback
 * drops stays dropped in the transaction's own view, at commit, after a
 * crash, and in a procedure. */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_table.h"
#include "cdb_proc.h"
#include "cdb_ts.h"
#include "cdb_faultenv.h"

#define FCAP (768u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
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
static uint8_t doc_stage[4096];
static uint8_t idx_stage[4096];
static uint8_t table_buffers[4u * PAGE];
static uint8_t ts_buffers[3u * PAGE];
static uint8_t ts_work[(CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point)];
static cdb_db_config cfg;
static cdb_db db;

static uint8_t out[PAGE];

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
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
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.idx_stage = idx_stage;
    cfg.idx_stage_size = sizeof idx_stage;
    cfg.table_buffers = table_buffers;
    cfg.ts_buffers = ts_buffers;
    cfg.ts_work = ts_work;
    cfg.ts_work_size = sizeof ts_work;
    cfg.durability = CDB_DURABILITY_FULL;
    TEST_ASSERT_EQUAL_size_t(cdb_table_buffers_size(PAGE, 0), sizeof table_buffers);
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5B, sizeof h.db_id);
    return cdb_db_open(&db, &env, "sp", &cfg, &h);
}

/* ------------------------------------------------------------ values */

typedef struct enc {
    uint8_t b[128];
    size_t n;
} enc;

/* { 1: text } */
static enc make_doc(const char *text)
{
    enc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, text, strlen(text));
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static enc make_int(int64_t v)
{
    enc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_int(&w, v);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static enc make_str(const char *s)
{
    enc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_string(&w, s, strlen(s));
    d.n = cdb_value_writer_len(&w);
    return d;
}

static uint64_t make_pool(const char *name)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, name, strlen(name), CDB_MODEL_DOCUMENT, &id));
    return id;
}

static uint64_t insert(uint64_t pool, const char *text)
{
    enc d = make_doc(text);
    uint64_t e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, d.b, d.n, &e));
    return e;
}

/* The committed document is { 1: text }, or absent when text is NULL. */
static int committed_is(uint64_t pool, uint64_t entry, const char *text)
{
    size_t len;
    cdb_status s = cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, NULL, 0, NULL, NULL);
    if (text == NULL)
        return s == CDB_E_NOT_FOUND;
    {
        enc d = make_doc(text);
        return s == CDB_OK && len == d.n && memcmp(out, d.b, len) == 0;
    }
}

/* The same, as the open transaction sees it. */
static int tx_sees(uint64_t pool, uint64_t entry, const char *text)
{
    size_t len = 0;
    int live = 0;
    uint32_t schema;
    db.via_table = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get_in_tx(&db, pool, entry, CDB_STAGE_OFF_NONE, out, sizeof out, &len,
                                                    &live, &schema));
    if (text == NULL)
        return !live;
    {
        enc d = make_doc(text);
        return live && len == d.n && memcmp(out, d.b, len) == 0;
    }
}

/* ------------------------------------------------- transaction savepoints */

static void check_rollback_view(int small_stage)
{
    enc a2 = make_doc("a2");
    cdb_tx_mark m;
    uint64_t pool, a, b, c;
    if (small_stage)
        cfg.doc_stage_size = 64; /* nothing fits: every read goes to the journal */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    a = insert(pool, "a1");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
    b = insert(pool, "b");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, a, NULL, 0, a2.b, a2.n));
    TEST_ASSERT_TRUE(tx_sees(pool, a, "a2"));
    TEST_ASSERT_TRUE(tx_sees(pool, b, "b"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
    TEST_ASSERT_TRUE(tx_sees(pool, a, "a1"));
    TEST_ASSERT_TRUE(tx_sees(pool, b, NULL));
    c = insert(pool, "c");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(committed_is(pool, a, "a1"));
    TEST_ASSERT_TRUE(committed_is(pool, b, NULL));
    TEST_ASSERT_TRUE(committed_is(pool, c, "c"));

    /* Never reconciled: replay applies the journal with the same skips. */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_TRUE(committed_is(pool, a, "a1"));
    TEST_ASSERT_TRUE(committed_is(pool, b, NULL));
    TEST_ASSERT_TRUE(committed_is(pool, c, "c"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rollback_drops_changes(void)
{
    check_rollback_view(0);
}

static void test_rollback_drops_changes_overflowed_stage(void)
{
    check_rollback_view(1);
}

static void test_all_changes_rolled_back(void)
{
    cdb_tx_mark m;
    uint64_t pool, a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
    a = insert(pool, "a");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
    /* The mark stays usable, and a rollback that drops nothing is fine. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(committed_is(pool, a, NULL));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_TRUE(committed_is(pool, a, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_nested_rollbacks(void)
{
    cdb_tx_mark m1, m2;
    uint64_t pool, x, y, z, w;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    w = insert(pool, "w");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m1));
    x = insert(pool, "x");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m2));
    y = insert(pool, "y");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m2));
    z = insert(pool, "z");
    TEST_ASSERT_TRUE(tx_sees(pool, x, "x"));
    TEST_ASSERT_TRUE(tx_sees(pool, y, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m1));
    TEST_ASSERT_TRUE(tx_sees(pool, x, NULL));
    TEST_ASSERT_TRUE(tx_sees(pool, z, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(committed_is(pool, w, "w"));
    TEST_ASSERT_TRUE(committed_is(pool, x, NULL));
    TEST_ASSERT_TRUE(committed_is(pool, y, NULL));
    TEST_ASSERT_TRUE(committed_is(pool, z, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* The batch pattern: keep one, drop one, CDB_TX_MAX_DROPS times, which makes
 * that many separate ranges. One more is refused and changes nothing; a
 * rollback to the first mark then covers them all. */
static void test_drop_range_limit(void)
{
    cdb_tx_mark first, m;
    uint64_t pool, keep[CDB_TX_MAX_DROPS], drop[CDB_TX_MAX_DROPS], extra;
    unsigned i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &first));
    for (i = 0; i < CDB_TX_MAX_DROPS; i++) {
        keep[i] = insert(pool, "keep");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
        drop[i] = insert(pool, "drop");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
    }
    insert(pool, "keep");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
    extra = insert(pool, "extra");
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, cdb_tx_rollback_to(&db, &m));
    TEST_ASSERT_TRUE(tx_sees(pool, extra, "extra"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    for (i = 0; i < CDB_TX_MAX_DROPS; i++) {
        TEST_ASSERT_TRUE(committed_is(pool, keep[i], "keep"));
        TEST_ASSERT_TRUE(committed_is(pool, drop[i], NULL));
    }
    TEST_ASSERT_TRUE(committed_is(pool, extra, "extra"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &first));
    for (i = 0; i < CDB_TX_MAX_DROPS; i++) {
        keep[i] = insert(pool, "keep");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
        drop[i] = insert(pool, "drop");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &first));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    for (i = 0; i < CDB_TX_MAX_DROPS; i++)
        TEST_ASSERT_TRUE(committed_is(pool, keep[i], NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_mark_of_another_transaction(void)
{
    cdb_tx_mark m;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_pool("p");
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_tx_savepoint(&db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_tx_rollback_to(&db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A unique key a rolled-back row held is free again (the index stage). */
static void test_rollback_frees_unique_key(void)
{
    cdb_table_column col;
    cdb_table_def def;
    unsigned pk = 0;
    uint64_t t, fid, e1, e2, got;
    cdb_tx_mark m;
    enc k;
    int found;
    {
        /* The primary key [1]. */
        cdb_value_writer w;
        cdb_value_writer_init(&w, k.b, sizeof k.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_array(&w);
        cdb_value_write_int(&w, 1);
        cdb_value_end_array(&w);
        k.n = cdb_value_writer_len(&w);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    memset(&col, 0, sizeof col);
    col.name = "id";
    col.name_len = 2;
    col.type = CDB_TYPE_INT;
    memset(&def, 0, sizeof def);
    def.columns = &col;
    def.ncolumns = 1;
    def.primary_key = &pk;
    def.npk = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "t", 1, &def, &t));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, t, "id", 2, &fid));
    {
        enc row;
        cdb_value_writer w;
        cdb_value_writer_init(&w, row.b, sizeof row.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, fid);
        cdb_value_write_int(&w, 1);
        cdb_value_end_object(&w);
        row.n = cdb_value_writer_len(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&db, &m));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, t, row.b, row.n, &e1));
        TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_insert(&db, t, row.b, row.n, &e2));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&db, &m));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, t, row.b, row.n, &e2));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, t, k.b, k.n, &got, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(e2, got);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, t, e1, out, sizeof out, &(size_t){ 0 }));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ procedures */

static cdb_proc_vm vm;

/* R0 = store, R1 = "keep", R2 = "drop"; R3 = insert keep; SAVEPOINT 1;
 * R4 = insert drop; ROLLBACK_TO 1; RETURN R4. */
static void test_proc_rollback_drops_writes(void)
{
    uint8_t bc[256], args3[2] = { 0, 1 }, args4[2] = { 0, 2 };
    cdb_proc_writer pw;
    cdb_value ret;
    uint64_t pool;
    enc st, k = make_str("keep"), d = make_str("drop");
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    st = make_int((int64_t)pool);
    cdb_proc_writer_init(&pw, bc, sizeof bc, 8, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 0, st.b, st.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 1, k.b, k.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 2, d.b, d.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_call_mutation_ex(&pw, 3, CDB_MUT_DOC_INSERT, 2, args3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_savepoint(&pw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_call_mutation_ex(&pw, 4, CDB_MUT_DOC_INSERT, 2, args4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_rollback_to(&pw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_return(&pw, 4));

    /* Outside a transaction there is nothing a savepoint could undo. */
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* Registers are not restored: R4 still holds the dropped entry's ID. */
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, ret.type);
    TEST_ASSERT_TRUE(ret.as.i64 > 0);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, (uint64_t)ret.as.i64, 0, out, sizeof out,
                                                       &(size_t){ 0 }, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, (uint64_t)ret.as.i64 - 1u, 0, out, sizeof out,
                                              &(size_t){ 0 }, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A logical failure leaves its status in the register and the procedure
 * goes on; the transaction commits what succeeded. */
static void test_proc_logical_failure_continues(void)
{
    uint8_t bc[256], args3[2] = { 0, 1 }, args4[2] = { 2, 1 };
    cdb_proc_writer pw;
    cdb_value ret;
    uint64_t pool;
    enc st, bad = make_int(99999), k = make_str("keep");
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("p");
    st = make_int((int64_t)pool);
    cdb_proc_writer_init(&pw, bc, sizeof bc, 8, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 0, st.b, st.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 1, k.b, k.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, 2, bad.b, bad.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_call_mutation_ex(&pw, 3, CDB_MUT_DOC_INSERT, 2, args3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_call_mutation_ex(&pw, 4, CDB_MUT_DOC_INSERT, 2, args4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_return(&pw, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, ret.type);
    TEST_ASSERT_TRUE(ret.as.i64 < 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, 1, 0, out, sizeof out, &(size_t){ 0 }, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A point { 1: a } or, with b >= 0, { 1: a, 2: b }. */
static enc make_point(int64_t a, int64_t b)
{
    enc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, a);
    if (b >= 0) {
        cdb_value_field(&w, 2);
        cdb_value_write_int(&w, b);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

/* A mutation that journals and then fails logically (spec 11.12.3):
 * downsampling appends one summary point per bucket, and the second bucket
 * fails (an average written into a bool field is CDB_E_INVALID_ARG) after
 * the first was journaled. The mutation's own savepoint drops the first, so
 * the procedure goes on and the commit holds no summary at all. */
static void test_proc_failed_mutation_leaves_nothing(void)
{
    uint8_t bc[512], args[7] = { 0, 1, 2, 3, 4, 5, 6 };
    cdb_proc_writer pw;
    cdb_value ret;
    cdb_ts_def src, dst;
    cdb_ts_in in[3];
    cdb_ts_row rows[4];
    enc p1 = make_point(1, -1), p2 = make_point(2, -1), p3 = make_point(3, 9);
    enc r[7];
    uint64_t store, s_src, s_dst;
    int created;
    size_t n = 9;
    int64_t next;
    unsigned i;

    memset(&src, 0, sizeof src);
    src.precision = CDB_TS_MILLIS;
    src.nfields = 2;
    src.fields[0].id = 1;
    src.fields[0].type = CDB_TYPE_INT;
    src.fields[1].id = 2;
    src.fields[1].type = CDB_TYPE_INT;
    dst = src;
    dst.fields[1].type = CDB_TYPE_BOOL;
    memset(in, 0, sizeof in);
    in[0].ts = 100;
    in[0].values = p1.b;
    in[0].len = p1.n;
    in[1].ts = 200;
    in[1].values = p2.b;
    in[1].len = p2.n;
    in[2].ts = 1100; /* the second bucket: field 2 appears */
    in[2].values = p3.b;
    in[2].len = p3.n;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "t", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "src", 3, NULL, 0, &src, &s_src, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "dst", 3, NULL, 0, &dst, &s_dst, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, s_src, in, 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    r[0] = make_int((int64_t)store);
    r[1] = make_int((int64_t)s_src);
    r[2] = make_int((int64_t)store);
    r[3] = make_int((int64_t)s_dst);
    r[4] = make_int(1000); /* bucket step */
    r[5] = make_int(0);
    r[6] = make_int(4000);
    cdb_proc_writer_init(&pw, bc, sizeof bc, 8, 0);
    for (i = 0; i < 7; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_load_const(&pw, (uint8_t)i, r[i].b, r[i].n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_call_mutation_ex(&pw, 7, CDB_MUT_TS_DOWNSAMPLE, 7, args));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_return(&pw, 7));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, ret.type);
    TEST_ASSERT_EQUAL_INT64(CDB_E_INVALID_ARG, ret.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, s_dst, 0, 4000, out, sizeof out, rows, 4, &n, &next));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_proc_savepoint_limit(void)
{
    uint8_t bc[256];
    cdb_proc_writer pw;
    cdb_value ret;
    uint8_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    cdb_proc_writer_init(&pw, bc, sizeof bc, 2, 0);
    for (i = 0; i <= CDB_PROC_MAX_SAVEPOINTS; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_savepoint(&pw, i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_halt(&pw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));

    /* Setting the same ID again moves it instead of taking a new place. */
    cdb_proc_writer_init(&pw, bc, sizeof bc, 2, 0);
    for (i = 0; i <= CDB_PROC_MAX_SAVEPOINTS; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_savepoint(&pw, 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_rollback_to(&pw, 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_release_sp(&pw, 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_halt(&pw));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));

    /* A released savepoint is gone. */
    cdb_proc_writer_init(&pw, bc, sizeof bc, 2, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_savepoint(&pw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_release_sp(&pw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_rollback_to(&pw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_proc_write_halt(&pw));
    cdb_proc_vm_init(&vm, &db, CDB_ORDER_LITTLE, NULL, 0, 100);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_proc_exec(&vm, bc, cdb_proc_writer_len(&pw), &ret));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: A = a1, C = c (B and A = a2 were rolled back). S2: the
 * same, after a second transaction whose only change was rolled back. */
static cdb_status workload(int *acked)
{
    enc a1 = make_doc("a1"), a2 = make_doc("a2"), b = make_doc("b"), c = make_doc("c"), x = make_doc("x");
    cdb_tx_mark m;
    uint64_t pool, ea, eb, ec, ex;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_db_create_store(&db, "p", 1, CDB_MODEL_DOCUMENT, &pool));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, a1.b, a1.n, &ea));
    TRY(cdb_tx_savepoint(&db, &m));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, b.b, b.n, &eb));
    TRY(cdb_doc_replace(&db, pool, ea, NULL, 0, a2.b, a2.n));
    TRY(cdb_tx_rollback_to(&db, &m));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, c.b, c.n, &ec));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_tx_savepoint(&db, &m));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, x.b, x.n, &ex));
    TRY(cdb_tx_rollback_to(&db, &m));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

/* Entries: A = 1, B = 2, C = 3, X = 4. */
static int observed_state(void)
{
    uint64_t pool;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "p", 1, &pool, &found));
    if (!found)
        return 0;
    if (committed_is(pool, 2, NULL) == 0 || committed_is(pool, 4, NULL) == 0)
        return -1; /* a dropped change survived */
    if (committed_is(pool, 1, NULL) && committed_is(pool, 3, NULL))
        return 0;
    if (committed_is(pool, 1, "a1") && committed_is(pool, 3, "c"))
        return 1;
    return -1;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[2] = { 0, 60 };
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
                k = observed_state();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "a rolled-back change was applied, or a torn state");
                TEST_ASSERT_TRUE_MESSAGE(k >= (acked > 0 ? 1 : 0), "an acknowledged commit was lost");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rollback_drops_changes);
    RUN_TEST(test_rollback_drops_changes_overflowed_stage);
    RUN_TEST(test_all_changes_rolled_back);
    RUN_TEST(test_nested_rollbacks);
    RUN_TEST(test_drop_range_limit);
    RUN_TEST(test_mark_of_another_transaction);
    RUN_TEST(test_rollback_frees_unique_key);
    RUN_TEST(test_proc_rollback_drops_writes);
    RUN_TEST(test_proc_logical_failure_continues);
    RUN_TEST(test_proc_failed_mutation_leaves_nothing);
    RUN_TEST(test_proc_savepoint_limit);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
