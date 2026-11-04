/* Entry-ID blocks (spec 5.9.3): a process holds at most CDB_COORD_MAX_BLOCKS
 * claimed blocks, so commits must give them back as they go, and one
 * transaction that needs more says so with CDB_E_OUT_OF_BUDGET. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_coord.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define POOLS (CDB_COORD_MAX_BLOCKS + 9u) /* more stores than blocks a slot holds */

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
static uint8_t doc_stage[4096];
static uint8_t idx_stage[16u * 1024u];
static cdb_db_config cfg;
static cdb_db db;
static uint64_t pool[POOLS];

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
    cdb_header h;
    unsigned i;

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
    cfg.durability = CDB_DURABILITY_FULL;

    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x1D, sizeof h.db_id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "ids", &cfg, &h));
    for (i = 0; i < POOLS; i++) {
        char name[16];
        int n = snprintf(name, sizeof name, "pool%u", i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, name, (size_t)n, CDB_MODEL_DOCUMENT, &pool[i]));
    }
}

void tearDown(void)
{
    cdb_db_close(&db);
}

static cdb_status insert_one(uint64_t store)
{
    uint8_t body[32];
    cdb_value_writer w;
    uint64_t entry;
    cdb_value_writer_init(&w, body, sizeof body, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, (int64_t)store);
    cdb_value_end_object(&w);
    return cdb_doc_insert(&db, store, NULL, 0, body, cdb_value_writer_len(&w), &entry);
}

/* One entry in each of more stores than a slot holds blocks, a commit each:
 * every store claims a block, so the commits must give them back. */
static void test_commits_give_blocks_back(void)
{
    unsigned i;
    for (i = 0; i < POOLS; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, insert_one(pool[i]), "insert");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
        TEST_ASSERT_TRUE(db.coord.me.nblocks <= CDB_COORD_MAX_BLOCKS / 2 + 1);
    }
    /* And each store takes more after its block was given back */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert_one(pool[0]));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
}

/* One transaction that needs more blocks than a slot holds is refused with
 * a status that names a limit, and the database goes on afterwards. */
static void test_one_transaction_over_the_limit(void)
{
    cdb_status s = CDB_OK;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < POOLS && s == CDB_OK; i++)
        s = insert_one(pool[i]);
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, s);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_MAX_BLOCKS + 1u, i);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert_one(pool[POOLS - 1]));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_commits_give_blocks_back);
    RUN_TEST(test_one_transaction_over_the_limit);
    return UNITY_END();
}
