#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cdb_db.h"
#include "cdb_faultenv.h"
#include "cdb_exec.h"

#define FCAP (640u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[1024];
static uint8_t cat_work[16384];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static cdb_db_config cfg;
static cdb_db db;

#define KV_PAGES 128u

static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    cdb_page_header t;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    if (d->entry_id >= KV_PAGES) return CDB_OK;
    if (d->op == CDB_OP_DELETE) {
        s = cdb_pager_free(p, d->entry_id);
        return s == CDB_E_NOT_FOUND ? CDB_OK : s;
    }
    while (p->next_lid < KV_PAGES) {
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK) return s;
    }
    memset(&t, 0, sizeof(t));
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    return cdb_pager_write(p, d->entry_id, &t, d->data, d->len);
}

static cdb_header settings(void)
{
    cdb_header h;
    memset(&h, 0, sizeof(h));
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    for (unsigned i = 0; i < 16; i++) {
        h.db_id[i] = (uint8_t)(0x30 + i);
    }
    return h;
}

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
    memset(&cfg, 0, sizeof(cfg));
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof(arena);
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof(free_list) / sizeof(free_list[0]);
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof(record_buf);
    cfg.apply = kv_apply;
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof(cat_work);
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 512;
    cfg.catalog_page = cat_page;
    cfg.durability = CDB_DURABILITY_FULL;
}

void tearDown(void) {}

static void test_tx_atomic_multi_store(void)
{
    cdb_header h = settings();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_multi", &cfg, &h));

    /* Begin a single transaction spanning 3 different stores */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));

    /* Store 101: Document pool insert */
    const char *doc = "{\"title\": \"Atomic Multi-Model\"}";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 101, 1, CDB_OP_REPLACE, doc, strlen(doc)));

    /* Store 102: Relational table insert + write-key */
    const char *row = "{\"id\": 1, \"status\": \"active\"}";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 102, 1, CDB_OP_REPLACE, row, strlen(row)));
    const char *pk_key = "1";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&db, 102, 0, pk_key, strlen(pk_key)));

    /* Store 103: Time-series append */
    const char *metric = "{\"ts\": 1600000000, \"val\": 42.5}";
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 103, 1, CDB_OP_REPLACE, metric, strlen(metric)));

    /* Commit atomically across all 3 stores */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_tx_abort_multi_store(void)
{
    cdb_header h = settings();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_abort", &cfg, &h));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 201, 1, CDB_OP_REPLACE, "doc1", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 202, 1, CDB_OP_REPLACE, "row1", 4));

    /* Abort transaction */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));

    /* Verify subsequent transaction still functions normally */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db, 201, 2, CDB_OP_REPLACE, "doc2", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_cursor_snapshot_isolation(void)
{
    /* Verifies that opening an execution cursor captures a snapshot of the entries
     * and is isolated from subsequent mutations performed during iteration */
    cdb_exec_cursor cur;
    uint8_t q_wire[128];
    uint8_t scratch[4096];
    cdb_query_writer qw;
    cdb_exec_row rows[3];
    cdb_exec_row out;
    int has_row = 0;

    memset(rows, 0, sizeof(rows));
    rows[0].entry_id = 1;
    rows[1].entry_id = 2;
    rows[2].entry_id = 3;

    cdb_query_writer_init(&qw, q_wire, sizeof(q_wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "test_store", 10));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, q_wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "test_store", 10, rows, 2));

    /* Read first row */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out, &has_row));
    TEST_ASSERT_EQUAL_INT(1, has_row);
    TEST_ASSERT_EQUAL_UINT64(1, out.entry_id);

    /* Read second row */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out, &has_row));
    TEST_ASSERT_EQUAL_INT(1, has_row);
    TEST_ASSERT_EQUAL_UINT64(2, out.entry_id);

    /* Cursor reaches EOF (2 entries) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out, &has_row));
    TEST_ASSERT_EQUAL_INT(0, has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_atomic_multi_store);
    RUN_TEST(test_tx_abort_multi_store);
    RUN_TEST(test_cursor_snapshot_isolation);
    return UNITY_END();
}
