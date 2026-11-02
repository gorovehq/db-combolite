/* Test overflow-chain tails packed into shared nibblets (spec 16.5, kind 4). */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_nib.h"
#include "cdb_overflow.h"
#include "cdb_page.h"

#define FCAP (2048u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define DOC_MAX 12288u
#define DBUF (DOC_MAX + PAGE)

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static uint8_t record_buf[DOC_MAX + 2u * PAGE];
static uint8_t cat_work[24u * 1024u];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[3u * DBUF + 5u * PAGE];
static uint8_t doc_stage[64u * 1024u];
static uint8_t fts_buffers[DBUF + 2u * PAGE];
static cdb_fts_token fts_tokens[2u * (DOC_MAX + PAGE)];
static cdb_db_config cfg;
static cdb_db db;

static uint8_t out[DOC_MAX];
static char summary_out[PAGE];

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
    cfg.doc_max = DOC_MAX;
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.fts_buffers = fts_buffers;
    cfg.fts_tokens = fts_tokens;
    cfg.fts_tokens_cap = sizeof fts_tokens / sizeof fts_tokens[0];
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
    memset(h.db_id, 0x55, sizeof h.db_id);
    return cdb_db_open(&db, &env, "test_overflow_nib", &cfg, &h);
}

static uint64_t make_pool(const char *name)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, name, strlen(name), CDB_MODEL_DOCUMENT, &id));
    return id;
}

static size_t make_doc(uint8_t *buf, size_t cap, size_t target_len, uint8_t seed)
{
    static char text[DOC_MAX];
    size_t text_len = target_len > 32u ? target_len - 32u : 1u;
    size_t i;
    cdb_value_writer w;

    for (i = 0; i < text_len; i++)
        text[i] = (char)('a' + ((seed + i) % 26u));

    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, text, text_len);
    cdb_value_field(&w, 2);
    cdb_value_write_int(&w, (int64_t)seed);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    return cdb_value_writer_len(&w);
}

/* ------------------------------------------------------------ Tests */

static void test_overflow_tail_single_page(void)
{
    static uint8_t body[2048];
    size_t body_len;
    uint64_t pool, entry = 0;
    size_t len = 0, slen = 0;

    body_len = make_doc(body, sizeof body, 1060, 0xA1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("docs1");

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, body, body_len, &entry));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Verify content matches exactly */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));
    TEST_ASSERT_EQUAL_size_t(body_len, len);
    TEST_ASSERT_EQUAL_MEMORY(body, out, body_len);

    /* Close and reopen to verify persistence across restarts */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    memset(out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));
    TEST_ASSERT_EQUAL_size_t(body_len, len);
    TEST_ASSERT_EQUAL_MEMORY(body, out, body_len);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_overflow_tail_multi_page(void)
{
    /* 1024-byte page: inline ~950, page 1 holds 1000, remainder ~150 bytes in tail nibblet */
    static uint8_t body[4096];
    size_t body_len;
    uint64_t pool, entry = 0;
    size_t len = 0, slen = 0;

    body_len = make_doc(body, sizeof body, 2100, 0xB2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("docs2");

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, "multi", 5, body, body_len, &entry));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Verify content and summary match */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));
    TEST_ASSERT_EQUAL_size_t(body_len, len);
    TEST_ASSERT_EQUAL_MEMORY(body, out, body_len);
    TEST_ASSERT_EQUAL_size_t(5, slen);
    TEST_ASSERT_EQUAL_MEMORY("multi", summary_out, 5);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_overflow_tail_update_and_delete(void)
{
    static uint8_t body1[2048];
    static uint8_t body2[4096];
    size_t len1, len2;
    uint64_t pool, entry = 0;
    size_t len = 0, slen = 0;

    len1 = make_doc(body1, sizeof body1, 1060, 0xC3);
    len2 = make_doc(body2, sizeof body2, 2080, 0xD4);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("docs3");

    /* Insert body1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, body1, len1, &entry));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Replace with body2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, entry, NULL, 0, body2, len2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Verify body2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));
    TEST_ASSERT_EQUAL_size_t(len2, len);
    TEST_ASSERT_EQUAL_MEMORY(body2, out, len2);

    /* Delete entry */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, entry));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Entry should be gone */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_overflow_tail_reconciliation_merge(void)
{
    static uint8_t bodies[10][2048];
    size_t body_lens[10];
    uint64_t pool, entries[10];
    size_t i, len, slen;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("docs_merge");

    /* Insert 10 documents with overflow tails */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 10; i++) {
        body_lens[i] = make_doc(bodies[i], sizeof bodies[i], 1080, (uint8_t)(0x10 + i));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, bodies[i], body_lens[i], &entries[i]));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Delete several documents to leave nibblet pages under half full */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, entries[i]));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Reconcile triggers cdb_nib_merge, relocating under-half-full nibblets */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));

    /* Verify the surviving entries still read cleanly after migration */
    for (i = 6; i < 10; i++) {
        memset(out, 0, sizeof out);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entries[i], 0, out, sizeof out, &len, summary_out, sizeof summary_out, &slen, NULL));
        TEST_ASSERT_EQUAL_size_t(body_lens[i], len);
        TEST_ASSERT_EQUAL_MEMORY(bodies[i], out, body_lens[i]);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_overflow_tail_dropped_store_reclaim(void)
{
    static uint8_t body[2048];
    size_t body_len;
    uint64_t pool, entry;
    uint64_t pages_freed = 0, nibblets_freed = 0;

    body_len = make_doc(body, sizeof body, 1080, 0xEE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("docs_drop");

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, body, body_len, &entry));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Drop the document pool */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, pool));

    /* Reclaim should unplace the tail nibblet and free its page */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reclaim(&db, &pages_freed, &nibblets_freed));
    TEST_ASSERT_TRUE(nibblets_freed > 0);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_overflow_tail_single_page);
    RUN_TEST(test_overflow_tail_multi_page);
    RUN_TEST(test_overflow_tail_update_and_delete);
    RUN_TEST(test_overflow_tail_reconciliation_merge);
    RUN_TEST(test_overflow_tail_dropped_store_reclaim);
    return UNITY_END();
}
