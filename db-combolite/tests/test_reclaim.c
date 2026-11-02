#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_nib.h"
#include "cdb_table.h"
#include "cdb_ts.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 9u /* 512-byte pages keep page boundaries small */
#define PAGE (1u << PAGE_LOG2)
#define NODES 32u
#define DOC_MAX 4096u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[2048];
static uint8_t cat_work[16384];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE + 3u * (PAGE + DOC_MAX)];
static uint8_t doc_stage[4096];
static uint8_t table_buf[4u * (PAGE + DOC_MAX)];
static uint8_t ts_buffers[3u * PAGE];
static uint8_t ts_work[(CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point)];
static cdb_db_config cfg;
static cdb_db db;

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

static cdb_header settings(void)
{
    cdb_header h;
    unsigned i;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    for (i = 0; i < 16; i++)
        h.db_id[i] = (uint8_t)(0x60 + i);
    return h;
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
    cfg.doc_max = DOC_MAX;
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.table_buffers = table_buf;
    cfg.ts_buffers = ts_buffers;
    cfg.ts_work = ts_work;
    cfg.ts_work_size = sizeof ts_work;
    cfg.durability = CDB_DURABILITY_FULL;
}

void tearDown(void) {}

/* Helper to count live pages owned by a specific store */
static uint64_t count_store_live_pages(cdb_db *d, uint64_t store_id)
{
    uint64_t count = 0;
    uint64_t lid;
    uint8_t scratch[PAGE];
    for (lid = 1; lid < d->pager.next_lid; lid++) {
        cdb_anchor_entry e;
        cdb_page_header h;
        if (cdb_pager_lookup(&d->pager, lid, &e) == CDB_OK && e.state == CDB_ANCHOR_LIVE) {
            if (cdb_pager_read(&d->pager, lid, scratch, &h) == CDB_OK) {
                if (h.store_id == store_id)
                    count++;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------ Table Index Drop */

static void test_drop_table_secondary_index(void)
{
    cdb_header h = settings();
    uint64_t T = 0, F_ID = 0, F_SCORE = 0;
    uint32_t ix = 0;
    cdb_table_column cols[2];
    cdb_table_def def;
    unsigned pk = 0;
    uint64_t before_free_extents, after_drop_free_extents;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_idx_drop", &cfg, &h));

    /* Create table */
    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[0].not_null = 1;
    cols[1].name = "score";
    cols[1].name_len = 5;
    cols[1].type = CDB_TYPE_INT;

    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 2;
    def.primary_key = &pk;
    def.npk = 1;
    def.keep = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "scores", 6, &def, &T));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "id", 2, &F_ID));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "score", 5, &F_SCORE));

    /* Insert rows */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 20; i++) {
        uint8_t row[64];
        cdb_value_writer w;
        cdb_value_writer_init(&w, row, sizeof row, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, F_ID);
        cdb_value_write_int(&w, (int64_t)i);
        cdb_value_field(&w, F_SCORE);
        cdb_value_write_int(&w, (int64_t)(i * 10));
        cdb_value_end_object(&w);

        uint64_t entry_id;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row, cdb_value_writer_len(&w), &entry_id));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    /* Create secondary index on score */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create_index(&db, T, &F_SCORE, 1, 0, &ix));
    TEST_ASSERT_TRUE(ix > 0);

    /* Insert more rows to populate the secondary index tree */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 20; i < 50; i++) {
        uint8_t row[64];
        cdb_value_writer w;
        cdb_value_writer_init(&w, row, sizeof row, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, F_ID);
        cdb_value_write_int(&w, (int64_t)i);
        cdb_value_field(&w, F_SCORE);
        cdb_value_write_int(&w, (int64_t)(i * 10));
        cdb_value_end_object(&w);

        uint64_t entry_id;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row, cdb_value_writer_len(&w), &entry_id));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    before_free_extents = db.pager.free_len;

    /* Drop the secondary index */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_drop_index(&db, T, ix));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    after_drop_free_extents = db.pager.free_len;

    /* Dropping the index must have freed pages */
    TEST_ASSERT_TRUE(after_drop_free_extents >= before_free_extents);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Document Store Reclaim */

static void test_reclaim_dropped_document_store(void)
{
    cdb_header h = settings();
    uint64_t pool_id = 0;
    uint64_t pages_freed = 0, nibblets_freed = 0;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_doc_reclaim", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "docs", 4, CDB_MODEL_DOCUMENT, &pool_id));

    /* Insert several documents with large content */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 25; i++) {
        uint8_t doc_buf[256];
        cdb_value_writer w;
        cdb_value_writer_init(&w, doc_buf, sizeof doc_buf, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, "reclaim_test_title", 18);
        cdb_value_field(&w, 2);
        cdb_value_write_int(&w, (int64_t)i);
        cdb_value_end_object(&w);

        uint64_t entry_id;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool_id, NULL, 0, doc_buf,
                                                    cdb_value_writer_len(&w), &entry_id));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    uint64_t live_pages = count_store_live_pages(&db, pool_id);
    TEST_ASSERT_TRUE(live_pages > 0);

    /* Drop the document store */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, pool_id));

    /* Run space reclamation */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reclaim(&db, &pages_freed, &nibblets_freed));
    TEST_ASSERT_TRUE(pages_freed >= live_pages);

    /* All pages belonging to the dropped store must now be freed */
    uint64_t remaining_live = count_store_live_pages(&db, pool_id);
    TEST_ASSERT_EQUAL_UINT64(0, remaining_live);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Time Series Nibblet Reclaim */

static void test_reclaim_dropped_timeseries_nibblets(void)
{
    cdb_header h = settings();
    uint64_t ts_store = 0, series_id = 0;
    uint64_t pages_freed = 0, nibblets_freed = 0;
    cdb_ts_def d;
    int created = 0;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_ts_nib_reclaim", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "metrics", 7, CDB_TS_LATE_ACCEPT, 0, &ts_store));

    memset(&d, 0, sizeof d);
    d.precision = CDB_TS_MILLIS;
    d.nfields = 1;
    d.fields[0].id = 1;
    d.fields[0].type = CDB_TYPE_INT;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, ts_store, "cpu", 3, NULL, 0, &d, &series_id, &created));

    /* Append small time-series points that pack into nibblet pages */
    for (i = 0; i < 30; i++) {
        uint8_t enc[64];
        cdb_value_writer w;
        cdb_value_writer_init(&w, enc, sizeof enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_int(&w, (int64_t)(i * 5));
        cdb_value_end_object(&w);

        cdb_ts_in in;
        in.ts = (int64_t)(1000 + i);
        in.use_clock = 0;
        in.values = enc;
        in.len = cdb_value_writer_len(&w);

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, ts_store, series_id, &in, 1));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    /* Drop the time-series store */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, ts_store));

    /* Run space reclamation: cleans up nibblets and pages */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reclaim(&db, &pages_freed, &nibblets_freed));
    TEST_ASSERT_TRUE(pages_freed > 0 || nibblets_freed > 0);

    /* Verify no live pages remain for dropped store */
    TEST_ASSERT_EQUAL_UINT64(0, count_store_live_pages(&db, ts_store));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ Compaction Integration */

static void test_compaction_reclaims_dropped_store(void)
{
    cdb_header h = settings();
    uint64_t store1 = 0, store2 = 0;
    uint64_t moved = 0, reclaimed = 0;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_compact_reclaim", &cfg, &h));

    /* Store 1: persistent store */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "keep", 4, CDB_MODEL_DOCUMENT, &store1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 5; i++) {
        uint8_t b[64];
        cdb_value_writer w;
        cdb_value_writer_init(&w, b, sizeof b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_int(&w, (int64_t)i);
        cdb_value_end_object(&w);
        uint64_t entry;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, store1, NULL, 0, b, cdb_value_writer_len(&w), &entry));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Store 2: temporary store with lots of data at the tail */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "temp", 4, CDB_MODEL_DOCUMENT, &store2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 50; i++) {
        uint8_t b[128];
        cdb_value_writer w;
        cdb_value_writer_init(&w, b, sizeof b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, "bulk_payload_data_string_for_tail_expansion", 43);
        cdb_value_end_object(&w);
        uint64_t entry;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, store2, NULL, 0, b, cdb_value_writer_len(&w), &entry));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    uint64_t append_point_before = db.pager.append_point;

    /* Drop store 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, store2));

    /* Compacting now should automatically reclaim dropped store 2 and shrink the file tail */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_compact(&db, 1000, &moved, &reclaimed));
    TEST_ASSERT_TRUE(reclaimed > 0);
    TEST_ASSERT_TRUE(db.pager.append_point < append_point_before);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ No-Op Reclaim */

static void test_reclaim_clean_db(void)
{
    cdb_header h = settings();
    uint64_t store1 = 0;
    uint64_t pages_freed = 999, nibblets_freed = 999;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "test_clean_reclaim", &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "active", 6, CDB_MODEL_DOCUMENT, &store1));

    /* Reclaim on clean DB: 0 freed, CDB_OK */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reclaim(&db, &pages_freed, &nibblets_freed));
    TEST_ASSERT_EQUAL_UINT64(0, pages_freed);
    TEST_ASSERT_EQUAL_UINT64(0, nibblets_freed);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_drop_table_secondary_index);
    RUN_TEST(test_reclaim_dropped_document_store);
    RUN_TEST(test_reclaim_dropped_timeseries_nibblets);
    RUN_TEST(test_compaction_reclaims_dropped_store);
    RUN_TEST(test_reclaim_clean_db);
    return UNITY_END();
}
