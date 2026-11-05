/* An open transaction reads its own records back from the journal (spec
 * 14.4.9 rule 6); when a store's records are encrypted for an enclave (spec
 * 12), those reads must decrypt them. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "combolite/cdb_crypto.h"
#include "cdb_doc.h"
#include "cdb_link.h"
#include "cdb_ts.h"
#include "cdb_vec.h"
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
static uint8_t doc_stage[64]; /* nothing fits: every read goes to the journal */
static uint8_t idx_stage[4096];
static uint8_t vec_buffers[3u * PAGE];
static uint8_t ts_buffers[3u * PAGE];
static uint8_t ts_work[(CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point)];
static cdb_db_config cfg;
static cdb_db db;

static uint8_t out[PAGE];

/* Longer than the stage, so the stage overflows. */
#define SECRET "secret secret secret secret secret secret secret secret secret secret secret secret"

void setUp(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));

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
    cfg.vec_buffers = vec_buffers;
    cfg.ts_buffers = ts_buffers;
    cfg.ts_work = ts_work;
    cfg.ts_work_size = sizeof ts_work;
    cfg.durability = CDB_DURABILITY_FULL;
    cfg.master_key_uri = "env://CDB_TEST_MK";
    cfg.aead_alg = CDB_AEAD_AES_256_GCM;
    setenv("CDB_TEST_MK", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
    setenv("CDB_TEST_ENC", "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff", 1);
}

void tearDown(void) {}

static void open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5C, sizeof h.db_id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "enc", &cfg, &h));
}

/* { 1: text } */
static size_t make_doc(const char *text, uint8_t *b, size_t cap)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, b, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, text, strlen(text));
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    return cdb_value_writer_len(&w);
}

static cdb_status count_entries(void *ctx, uint64_t entry, size_t staged_off)
{
    (void)entry;
    (void)staged_off;
    ++*(unsigned *)ctx;
    return CDB_OK;
}

/* A document pool whose store ID is an enclave's: its records are journaled
 * encrypted, and the stage is too small to hold them, so the transaction's
 * own reads (replay_tx, cdb_doc_tx_entries) decrypt them from the journal. */
static void test_doc_reads_decrypt(void)
{
    uint8_t b[256];
    size_t n = make_doc(SECRET, b, sizeof b), len = 0;
    uint64_t pool, e;
    int live = 0;
    uint32_t schema;
    unsigned entries = 0;

    open_db();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "p", 1, CDB_MODEL_DOCUMENT, &pool));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, pool, "env://CDB_TEST_ENC"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, b, n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get_in_tx(&db, pool, e, CDB_STAGE_OFF_NONE, out, sizeof out, &len, &live,
                                                    &schema));
    TEST_ASSERT_TRUE(live);
    TEST_ASSERT_EQUAL_size_t(n, len);
    TEST_ASSERT_EQUAL_MEMORY(b, out, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_tx_entries(&db, pool, count_entries, &entries));
    TEST_ASSERT_EQUAL_UINT(1u, entries);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(b, out, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A series created in the transaction is found again by key from the
 * journal (tx_next in cdb_ts.c). */
static void test_ts_reads_decrypt(void)
{
    cdb_ts_def d;
    uint64_t store, s1, s2;
    int created = 0, live = 0;

    memset(&d, 0, sizeof d);
    d.precision = CDB_TS_MILLIS;
    d.nfields = 1;
    d.fields[0].id = 1;
    d.fields[0].type = CDB_TYPE_INT;
    open_db();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "t", 1, CDB_TS_LATE_ACCEPT, 0, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, store, "env://CDB_TEST_ENC"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "s", 1, NULL, 0, &d, &s1, &created));
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "s", 1, NULL, 0, NULL, &s2, &created));
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_UINT64(s1, s2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_live_in_tx(&db, store, s1, &live));
    TEST_ASSERT_TRUE(live);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A vector inserted in the transaction is live from the journal
 * (live_in_tx in cdb_vec.c). */
static void test_vec_reads_decrypt(void)
{
    static const float f[4] = { 1.0f, 0.5f, 0.25f, 0.125f };
    uint8_t b[64];
    cdb_value_writer w;
    cdb_vec_config c;
    uint64_t store, e;
    int live = 0;

    cdb_value_writer_init(&w, b, sizeof b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_vector(&w, CDB_VEC_F32, 4, f);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    memset(&c, 0, sizeof c);
    c.elem = CDB_VEC_F32;
    c.dims = 4;
    c.measure = CDB_VEC_EUCLIDEAN;
    c.model = "test-embedder";
    c.model_len = 13;
    c.model_version = "1.0";
    c.model_version_len = 3;
    open_db();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "v", 1, &c, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, store, "env://CDB_TEST_ENC"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, store, b, cdb_value_writer_len(&w), 0, 0, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_live_in_tx(&db, store, e, &live));
    TEST_ASSERT_TRUE(live);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* With the index stage too small to hold them, link checks read the
 * transaction's LINK records from the journal (tx_next in cdb_link.c). The
 * link store's ID is the enclave's, so those records are encrypted. */
static void test_link_reads_decrypt(void)
{
    uint8_t b[256];
    size_t n = make_doc("end", b, sizeof b);
    uint64_t pool, type, x, y;

    cfg.idx_stage_size = 64;
    open_db();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "p", 1, CDB_MODEL_DOCUMENT, &pool));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_define_type(&db, "cites", 5, CDB_CARD_N_M, 0, 0, &type));
    TEST_ASSERT_TRUE(cdb_link_is_store(&db, db.links.store_id));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_enclave(&db, db.links.store_id, "env://CDB_TEST_ENC"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, b, n, &x));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, b, n, &y));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_add(&db, type, cdb_ref_entry(pool, x), cdb_ref_entry(pool, y), 1.0f, 0));
    TEST_ASSERT_TRUE(db.stage_idx_overflow);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS,
                          cdb_link_add(&db, type, cdb_ref_entry(pool, x), cdb_ref_entry(pool, y), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_doc_reads_decrypt);
    RUN_TEST(test_ts_reads_decrypt);
    RUN_TEST(test_vec_reads_decrypt);
    RUN_TEST(test_link_reads_decrypt);
    return UNITY_END();
}
