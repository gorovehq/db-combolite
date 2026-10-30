#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_link.h"
#include "cdb_vec.h"

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
static uint8_t vec_buffers[3u * PAGE];
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
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.vec_buffers = vec_buffers;
    cfg.durability = CDB_DURABILITY_FULL;
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
    memset(h.db_id, 0x1C, sizeof h.db_id);
    return cdb_db_open(&db, &env, "l", &cfg, &h);
}

/* ------------------------------------------------------------ helpers */

static uint64_t pool;

static uint64_t make_pool(const char *name)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, name, strlen(name), CDB_MODEL_DOCUMENT, &id));
    return id;
}

static uint64_t new_doc(void)
{
    uint8_t b[32];
    cdb_value_writer w;
    uint64_t e;
    cdb_value_writer_init(&w, b, sizeof b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, "x", 1);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, b, cdb_value_writer_len(&w), &e));
    return e;
}

static cdb_ref ref(uint64_t store, uint64_t entry)
{
    cdb_ref r;
    r.store = store;
    r.entry = entry;
    return r;
}

static cdb_ref D(uint64_t entry) { return ref(pool, entry); }

static uint64_t define(const char *name, unsigned card, int enforced, unsigned on_delete)
{
    uint64_t t;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_define_type(&db, name, strlen(name), card, enforced, on_delete, &t));
    return t;
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }

static void add(uint64_t type, cdb_ref s, cdb_ref t)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_add(&db, type, s, t, 1.0f, 100));
}

/* The link's flags, or -1 when there is none. */
static int flags_of(uint64_t type, cdb_ref s, cdb_ref t)
{
    cdb_link l;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_get(&db, type, s, t, &l, &found));
    return found ? (int)l.flags : -1;
}

static size_t count(cdb_ref e, int dir, uint64_t type)
{
    cdb_link out[16];
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_list(&db, e, dir, type, NULL, out, 16, &n));
    return n;
}

/* ------------------------------------------------------------ tests */

static void test_types(void)
{
    uint64_t t, again, store;
    int found;
    cdb_store_info info;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    t = define("cites", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_find_type(&db, "cites", 5, &again, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(t, again);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_link_define_type(&db, "cites", 5, CDB_CARD_1_N, 1, 0, &again));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_link_define_type(&db, "bad", 3, 4, 1, 0, &again));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_link_define_type(&db, "bad", 3, CDB_CARD_1_1, 1, 3, &again));
    /* A link type is not a store, and the link store's name is reserved. */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_catalog_get_store(&db.catalog, t, &info));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_db_create_store(&db, "$mine", 5, CDB_MODEL_DOCUMENT, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, CDB_LINK_STORE_NAME, 6, &store, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_db_drop_store(&db, store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_find_type(&db, "cites", 5, &again, &found));
    TEST_ASSERT_TRUE(found && again == t);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_add_follow_remove(void)
{
    const int64_t t0 = INT64_C(1761768000000000000);
    uint64_t t, a, b, c;
    cdb_link l, page[2];
    size_t n;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("notes");
    /* No link types yet: nothing to add, nothing found. */
    begin();
    a = new_doc();
    b = new_doc();
    c = new_doc();
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, 99, D(a), D(b), 1.0f, 0));
    commit();
    TEST_ASSERT_EQUAL_INT(-1, flags_of(99, D(a), D(b)));
    t = define("cites", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    begin();
    add(t, D(a), D(b));
    add(t, D(a), D(c));
    add(t, D(c), D(b));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_get(&db, t, D(a), D(b), &l, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, l.weight);
    TEST_ASSERT_EQUAL_INT64(100, l.time); /* no clock: the caller's time */
    TEST_ASSERT_EQUAL_UINT8(0, l.flags);
    TEST_ASSERT_EQUAL_size_t(2, count(D(a), CDB_LINK_OUT, 0));
    TEST_ASSERT_EQUAL_size_t(2, count(D(b), CDB_LINK_IN, t));
    TEST_ASSERT_EQUAL_size_t(0, count(D(b), CDB_LINK_OUT, 0));
    /* Listing in pages, in key order. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_list(&db, D(b), CDB_LINK_IN, 0, NULL, page, 1, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(a, page[0].source.entry);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_list(&db, D(b), CDB_LINK_IN, 0, &page[0], &page[1], 1, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(c, page[1].source.entry);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_list(&db, D(b), CDB_LINK_IN, 0, &page[1], page, 1, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    /* Reinforce with the clock on: the environment's time wins. */
    cdb_faultenv_set_time(&fe, 1, t0);
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_reinforce(&db, t, D(a), D(b), 2.5f, 7));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_get(&db, t, D(a), D(b), &l, &found));
    TEST_ASSERT_EQUAL_FLOAT(2.5f, l.weight);
    TEST_ASSERT_EQUAL_INT64(t0, l.time);
    /* Removing tombstones; a removed link can be added again (revived). */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, t, D(a), D(c)));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE, flags_of(t, D(a), D(c)));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_reinforce(&db, t, D(a), D(c), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_remove(&db, t, D(a), D(c)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_add(&db, t, D(a), D(c), 0.5f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_link_add(&db, t, D(a), D(c), 1.0f, 0));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_get(&db, t, D(a), D(c), &l, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT8(0, l.flags);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, l.weight);
    TEST_ASSERT_EQUAL_UINT32(1, l.removed);
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, t, D(a), D(c)));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_link_add(&db, t, D(b), D(c), 1.0f, 0)); /* not in a transaction */
    /* It all survives reopening. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(0, flags_of(t, D(a), D(b)));
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE, flags_of(t, D(a), D(c)));
    TEST_ASSERT_EQUAL_size_t(2, count(D(b), CDB_LINK_IN, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Each change is checked against what the open transaction sees. */
static void test_transaction_view(void)
{
    uint64_t t, a, b, d;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("notes");
    t = define("cites", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    begin();
    a = new_doc();
    b = new_doc();
    add(t, D(a), D(b)); /* both made in this transaction */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_link_add(&db, t, D(a), D(b), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, t, D(a), D(b)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_add(&db, t, D(a), D(b), 1.0f, 0)); /* revived within the transaction */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, t, D(a), D(b)));
    d = new_doc();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, d));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t, D(a), D(d), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t, D(a), D(999), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t, D(a), ref(12345, 1), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t, D(d), D(a), 1.0f, 0)); /* the source end too */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t, ref(12345, 1), D(a), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_link_add(&db, t + 1000, D(a), D(b), 1.0f, 0));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE, flags_of(t, D(a), D(b)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_cardinality(void)
{
    uint64_t parent, pair, loose, a, b, x, y, p, q;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("notes");
    parent = define("parent", CDB_CARD_1_N, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    pair = define("pair", CDB_CARD_1_1, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    loose = define("loose", CDB_CARD_1_N, 0, CDB_LINK_ON_DELETE_TOMBSTONE);
    begin();
    a = new_doc();
    b = new_doc();
    x = new_doc();
    y = new_doc();
    p = new_doc();
    q = new_doc();
    commit();
    begin();
    add(parent, D(a), D(x));
    add(parent, D(a), D(y)); /* a source may have many targets */
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_link_add(&db, parent, D(b), D(x), 1.0f, 0)); /* x has its source */
    add(pair, D(a), D(p));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_link_add(&db, pair, D(a), D(q), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_link_add(&db, pair, D(b), D(p), 1.0f, 0));
    add(loose, D(a), D(x));
    add(loose, D(b), D(x)); /* recorded only: not enforced */
    commit();
    /* Committed state counts too; a tombstoned link frees the slot. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_link_add(&db, parent, D(b), D(x), 1.0f, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, parent, D(a), D(x)));
    add(parent, D(b), D(x));
    commit();
    TEST_ASSERT_EQUAL_INT(0, flags_of(parent, D(b), D(x)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Deleting an entry: tombstone (both directions), cascade, refuse. Vectors
 * are ends too. */
static void test_delete_rules(void)
{
    static const float v4[4] = { 1, 0, 0, 0 };
    uint8_t enc[64];
    cdb_value_writer w;
    cdb_vec_config vc;
    uint64_t tomb, casc, refuse, a, b, c, d, coll, ve;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("notes");
    memset(&vc, 0, sizeof vc);
    vc.elem = CDB_VEC_F32;
    vc.dims = 4;
    vc.measure = CDB_VEC_COSINE;
    vc.model = "m";
    vc.model_len = 1;
    vc.model_version = "1";
    vc.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "emb", 3, &vc, &coll));
    tomb = define("cites", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_TOMBSTONE);
    casc = define("part", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_CASCADE);
    refuse = define("needs", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_REFUSE);
    cdb_value_writer_init(&w, enc, sizeof enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_vector(&w, CDB_VEC_F32, 4, v4);
    begin();
    a = new_doc();
    b = new_doc();
    c = new_doc();
    d = new_doc();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, enc, cdb_value_writer_len(&w), pool, a, &ve));
    add(tomb, D(a), D(b));
    add(tomb, D(c), D(a));
    add(casc, D(a), D(d));
    add(refuse, D(b), D(c));
    add(tomb, D(b), ref(coll, ve));
    commit();
    /* a goes: its tombstone links in both directions, its cascade link erased. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, a));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_SOURCE_GONE, flags_of(tomb, D(a), D(b)));
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE, flags_of(tomb, D(c), D(a)));
    TEST_ASSERT_EQUAL_INT(-1, flags_of(casc, D(a), D(d)));
    TEST_ASSERT_EQUAL_size_t(0, count(D(d), CDB_LINK_IN, 0));
    /* c is needed by b: refused while that link lives, also as the
     * transaction sees it. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_doc_delete(&db, pool, c));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_remove(&db, refuse, D(b), D(c)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, c));
    commit();
    {
        size_t len;
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, c, 0, enc, sizeof enc, &len, NULL, 0, NULL, NULL));
    }
    begin();
    d = new_doc();
    add(refuse, D(b), D(d));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_doc_delete(&db, pool, d)); /* the link is from this transaction */
    commit();
    /* A vector end. */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, ve));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE, flags_of(tomb, D(b), ref(coll, ve)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_drop_store(void)
{
    uint64_t t, notes, other, a, b;
    need_open(2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    notes = make_pool("notes");
    other = make_pool("other");
    t = define("cites", CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_CASCADE); /* a drop tombstones even these */
    begin();
    pool = notes;
    a = new_doc();
    pool = other;
    b = new_doc();
    pool = notes;
    commit();
    begin();
    add(t, D(a), ref(other, b));
    add(t, ref(other, b), D(a));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, other));
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE, flags_of(t, D(a), ref(other, b)));
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_SOURCE_GONE, flags_of(t, ref(other, b), D(a)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE, flags_of(t, D(a), ref(other, b)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: pool with a, b, c; types; a->b, b->c (tombstone type),
 * c->a (cascade). S2: a->c added, a->b reinforced. S3: c deleted (b->c and
 * a->c tombstoned, c->a erased). S4: a->b removed. */
static uint64_t sw_t, sw_casc;

static cdb_status workload(int *acked)
{
    uint64_t a, b, c;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_db_create_store(&db, "notes", 5, CDB_MODEL_DOCUMENT, &pool));
    TRY(cdb_link_define_type(&db, "cites", 5, CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_TOMBSTONE, &sw_t));
    TRY(cdb_link_define_type(&db, "part", 4, CDB_CARD_N_M, 1, CDB_LINK_ON_DELETE_CASCADE, &sw_casc));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    {
        uint8_t b1[16] = { 0 };
        cdb_value_writer w;
        cdb_value_writer_init(&w, b1, sizeof b1, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_end_object(&w);
        TRY(cdb_doc_insert(&db, pool, NULL, 0, b1, cdb_value_writer_len(&w), &a));
        TRY(cdb_doc_insert(&db, pool, NULL, 0, b1, cdb_value_writer_len(&w), &b));
        TRY(cdb_doc_insert(&db, pool, NULL, 0, b1, cdb_value_writer_len(&w), &c));
    }
    TRY(cdb_link_add(&db, sw_t, D(a), D(b), 1.0f, 10));
    TRY(cdb_link_add(&db, sw_t, D(b), D(c), 1.0f, 10));
    TRY(cdb_link_add(&db, sw_casc, D(c), D(a), 1.0f, 10));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_link_add(&db, sw_t, D(a), D(c), 1.0f, 20));
    TRY(cdb_link_reinforce(&db, sw_t, D(a), D(b), 3.0f, 20));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_delete(&db, pool, c));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_link_remove(&db, sw_t, D(a), D(b)));
    TRY(cdb_tx_commit(&db));
    *acked = 4;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static int weight_is(uint64_t type, cdb_ref s, cdb_ref t, float w)
{
    cdb_link l;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_get(&db, type, s, t, &l, &found));
    return found && l.weight == w;
}

static int observed_state(void)
{
    uint64_t id, ta, tc;
    int found, fa, fc;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "notes", 5, &id, &found));
    if (!found)
        return 0;
    pool = id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_find_type(&db, "cites", 5, &ta, &fa));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_link_find_type(&db, "part", 4, &tc, &fc));
    if (!fa || !fc || flags_of(ta, D(1), D(2)) == -1)
        return 0; /* pool and types, but no committed transaction */
    if (flags_of(ta, D(1), D(2)) == 0 && weight_is(ta, D(1), D(2), 1.0f) && flags_of(ta, D(2), D(3)) == 0
        && flags_of(tc, D(3), D(1)) == 0 && flags_of(ta, D(1), D(3)) == -1)
        return 1;
    if (!weight_is(ta, D(1), D(2), 3.0f))
        return -1;
    if (flags_of(ta, D(2), D(3)) == 0 && flags_of(tc, D(3), D(1)) == 0 && flags_of(ta, D(1), D(3)) == 0)
        return 2;
    if (flags_of(ta, D(2), D(3)) != (CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE) || flags_of(tc, D(3), D(1)) != -1)
        return -1;
    if (flags_of(ta, D(1), D(3)) != (CDB_LINK_TOMBSTONE | CDB_LINK_TARGET_GONE))
        return -1;
    if (flags_of(ta, D(1), D(2)) == 0)
        return 3;
    if (flags_of(ta, D(1), D(2)) == CDB_LINK_TOMBSTONE)
        return 4;
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
                k = observed_state();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "links in a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_types);
    RUN_TEST(test_add_follow_remove);
    RUN_TEST(test_transaction_view);
    RUN_TEST(test_cardinality);
    RUN_TEST(test_delete_rules);
    RUN_TEST(test_drop_store);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
