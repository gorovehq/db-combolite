#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
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
static uint8_t doc_buffers[5u * PAGE];
static cdb_db_config cfg;
static cdb_db db;

static uint8_t out[PAGE];
static char summary_out[PAGE];

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
    cfg.durability = CDB_DURABILITY_FULL;
    TEST_ASSERT_TRUE(cdb_index_work_size(PAGE) <= sizeof cat_work);
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5D, sizeof h.db_id);
    return cdb_db_open(&db, &env, "g", &cfg, &h);
}

static uint64_t make_pool(const char *name)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, name, strlen(name), CDB_MODEL_DOCUMENT, &id));
    return id;
}

/* ------------------------------------------------------------ documents */

typedef struct doc {
    uint8_t b[256];
    size_t n;
} doc;

/* { 1: title, 2: status, 3: { 1: owner } } with any part omitted when NULL. */
static doc make_doc(const char *title, const char *status, const char *owner)
{
    doc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    if (title) {
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, title, strlen(title));
    }
    if (status) {
        cdb_value_field(&w, 2);
        cdb_value_write_string(&w, status, strlen(status));
    }
    if (owner) {
        cdb_value_field(&w, 3);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, owner, strlen(owner));
        cdb_value_end_object(&w);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

/* A patch with one operation: set (value non-NULL, a string) or remove. */
static doc make_patch(const uint64_t *path, unsigned depth, const char *value)
{
    doc d;
    cdb_value_writer w;
    unsigned i;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, value ? CDB_PATCH_SET : CDB_PATCH_REMOVE);
    cdb_value_begin_array(&w);
    for (i = 0; i < depth; i++)
        cdb_value_write_int(&w, (int64_t)path[i]);
    cdb_value_end_array(&w);
    if (value)
        cdb_value_write_string(&w, value, strlen(value));
    cdb_value_end_array(&w);
    cdb_value_end_array(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static void expect_doc(uint64_t pool, uint64_t entry, uint64_t rev, const doc *want)
{
    size_t len;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, rev, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_size_t(want->n, len);
    TEST_ASSERT_EQUAL_MEMORY(want->b, out, len);
}

static int doc_exists(uint64_t pool, uint64_t entry)
{
    size_t len;
    cdb_status s = cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, NULL, 0, NULL, NULL);
    if (s == CDB_E_NOT_FOUND)
        return 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    return 1;
}

static uint64_t insert(uint64_t pool, const char *summary, const doc *d)
{
    uint64_t entry;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, summary, summary ? strlen(summary) : 0,
                                                 d->b, d->n, &entry));
    return entry;
}

/* ------------------------------------------------------------ basics */

static void test_insert_and_get(void)
{
    doc a = make_doc("ADR-0001", "accepted", "casey"), b = make_doc("ADR-0002", "proposed", NULL);
    cdb_doc_info info;
    size_t len, slen;
    uint64_t pool, ea, eb;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    ea = insert(pool, "Use C99 for the core", &a);
    eb = insert(pool, NULL, &b);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_UINT64(1, ea);
    TEST_ASSERT_EQUAL_UINT64(2, eb);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, ea, 0, out, sizeof out, &len,
                                              summary_out, sizeof summary_out, &slen, &info));
    TEST_ASSERT_EQUAL_MEMORY(a.b, out, a.n);
    TEST_ASSERT_EQUAL_MEMORY("Use C99 for the core", summary_out, slen);
    TEST_ASSERT_EQUAL_UINT64(1, info.revision);
    TEST_ASSERT_EQUAL_UINT8(CDB_REV_ORIGINAL, info.kind);
    TEST_ASSERT_TRUE(info.commit_number >= 1);
    expect_doc(pool, eb, 0, &b);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, 3, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_replace_keeps_history_and_summary(void)
{
    doc v1 = make_doc("Title", "proposed", "casey"), v2 = make_doc("Title", "accepted", NULL);
    cdb_doc_info info;
    size_t len, slen;
    uint64_t pool, e;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, "keep me", &v1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, v2.b, v2.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &len,
                                              summary_out, sizeof summary_out, &slen, &info));
    TEST_ASSERT_EQUAL_MEMORY(v2.b, out, v2.n);
    TEST_ASSERT_EQUAL_UINT8(CDB_REV_DELTA, info.kind);
    TEST_ASSERT_EQUAL_UINT64(2, info.revision);
    TEST_ASSERT_EQUAL_MEMORY("keep me", summary_out, slen); /* no new summary given */
    expect_doc(pool, e, 1, &v1);                         /* history (spec 8.6) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_patch_nested_set_and_remove(void)
{
    doc v1 = make_doc("Title", "proposed", "casey");
    const uint64_t owner[2] = { 3, 1 }, status[1] = { 2 }, fresh[2] = { 7, 4 };
    doc p1 = make_patch(owner, 2, "sam"), p2 = make_patch(status, 1, NULL), p3 = make_patch(fresh, 2, "x");
    doc expect1 = make_doc("Title", "proposed", "sam"), expect2 = make_doc("Title", NULL, "sam");
    uint64_t pool, e;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, NULL, &v1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p1.b, p1.n)); /* same transaction */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    expect_doc(pool, e, 0, &expect1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p2.b, p2.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    expect_doc(pool, e, 0, &expect2);
    expect_doc(pool, e, 2, &expect1);
    expect_doc(pool, e, 1, &v1);

    /* A set through a path that does not exist builds the objects on the way. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p3.b, p3.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        cdb_value root, f7, f4;
        size_t len;
        int found;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_validate(out, len, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT, 0));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, len, CDB_ORDER_LITTLE, &root));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&root, CDB_ORDER_LITTLE, 7, &f7, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&f7, CDB_ORDER_LITTLE, 4, &f4, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_MEMORY("x", f4.payload, 1);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_delete_writes_tombstone(void)
{
    doc v1 = make_doc("Gone", "x", NULL);
    const uint64_t path[1] = { 1 };
    doc p = make_patch(path, 1, "ignored");
    cdb_doc_info info;
    size_t len;
    uint64_t pool, e;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, NULL, &v1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_FALSE(doc_exists(pool, e));
    expect_doc(pool, e, 1, &v1); /* history survives deletion */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, e, 2, out, sizeof out, &len, NULL, 0, NULL, &info));
    /* Changing a deleted document is a no-op, not a failure (it must replay). */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p.b, p.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_FALSE(doc_exists(pool, e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_checkpoints(void)
{
    const uint64_t path[1] = { 2 };
    char status[16];
    cdb_doc_info info;
    size_t len;
    uint64_t pool, e;
    int i;

    cfg.checkpoint_every = 4;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    {
        doc v1 = make_doc("T", "s0", NULL);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        e = insert(pool, "sum", &v1);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    for (i = 1; i <= 10; i++) {
        doc p;
        snprintf(status, sizeof status, "s%d", i);
        p = make_patch(path, 1, status);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p.b, p.n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    /* Every revision materializes to its own state, whatever its kind. */
    for (i = 0; i <= 10; i++) {
        doc want;
        snprintf(status, sizeof status, "s%d", i);
        want = make_doc("T", status, NULL);
        expect_doc(pool, e, (uint64_t)i + 1u, &want);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, (uint64_t)i + 1u, out, sizeof out, &len,
                                                  summary_out, sizeof summary_out, &len, &info));
        TEST_ASSERT_EQUAL_MEMORY("sum", summary_out, 3); /* carried through checkpoints */
        TEST_ASSERT_EQUAL_UINT8(i == 0 ? CDB_REV_ORIGINAL : (i % 4 == 0 ? CDB_REV_CHECKPOINT : CDB_REV_DELTA),
                                info.kind);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_reopen_and_ids_continue(void)
{
    doc v = make_doc("A", "B", "C");
    uint64_t pool, e1, e2;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e1 = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "adr", 3, &pool, &found));
    expect_doc(pool, e1, 0, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e2 = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(e2 > e1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_replay_after_crash_and_ids_not_reused(void)
{
    doc v = make_doc("durable", NULL, NULL);
    uint64_t pool, e1, e2;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    e1 = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0); /* never reconciled */
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "adr", 3, &pool, &found));
    expect_doc(pool, e1, 0, &v); /* replayed from the journal */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    e2 = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(e2 > e1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rejects_bad_input(void)
{
    doc v = make_doc("x", NULL, NULL);
    const uint8_t not_object[] = { CDB_TAG_INT, 2 };
    const uint8_t bad_patch[] = { CDB_TAG_INT, 2 };
    static uint8_t huge[PAGE];
    uint64_t pool, e, other;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    other = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "rows", 4, CDB_MODEL_RELATIONAL, &other));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_doc_insert(&db, pool, NULL, 0, v.b, v.n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_insert(&db, pool, NULL, 0, not_object, sizeof not_object, &e));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_insert(&db, other, NULL, 0, v.b, v.n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_replace(&db, pool, 9, NULL, 0, v.b, v.n));
    e = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_patch(&db, pool, e, bad_patch, sizeof bad_patch));
    {
        /* An object too large for one page. */
        cdb_value_writer w;
        static char big[PAGE];
        memset(big, 'a', sizeof big - 100);
        cdb_value_writer_init(&w, huge, sizeof huge, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, big, sizeof big - 100);
        cdb_value_end_object(&w);
        TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_doc_insert(&db, pool, NULL, 0, huge, cdb_value_writer_len(&w), &e));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* Pool "adr". S0: nothing (or empty pool). S1: A = a1, B = b1. S2: A = a2 (patched).
 * S3: B = b2 (replaced). S4: A deleted. */
static cdb_status workload(int *acked)
{
    doc a1 = make_doc("A", "draft", "casey"), b1 = make_doc("B", "draft", NULL), b2 = make_doc("B", "final", "sam");
    const uint64_t path[1] = { 2 };
    doc p = make_patch(path, 1, "review");
    uint64_t pool, ea, eb;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_db_create_store(&db, "adr", 3, CDB_MODEL_DOCUMENT, &pool));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_insert(&db, pool, "first", 5, a1.b, a1.n, &ea));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, b1.b, b1.n, &eb));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_patch(&db, pool, ea, p.b, p.n));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_replace(&db, pool, eb, NULL, 0, b2.b, b2.n));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_delete(&db, pool, ea));
    TRY(cdb_tx_commit(&db));
    *acked = 4;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static int matches(uint64_t pool, uint64_t entry, const doc *want)
{
    size_t len;
    cdb_status s = cdb_doc_get(&db, pool, entry, 0, out, sizeof out, &len, NULL, 0, NULL, NULL);
    if (want == NULL)
        return s == CDB_E_NOT_FOUND;
    return s == CDB_OK && len == want->n && memcmp(out, want->b, len) == 0;
}

static int observed_state(uint64_t *pool_out)
{
    doc a1 = make_doc("A", "draft", "casey"), a2 = make_doc("A", "review", "casey"),
        b1 = make_doc("B", "draft", NULL), b2 = make_doc("B", "final", "sam");
    uint64_t pool;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "adr", 3, &pool, &found));
    *pool_out = found ? pool : 0;
    if (!found || (matches(pool, 1, NULL) && matches(pool, 2, NULL)))
        return 0;
    if (matches(pool, 1, &a1) && matches(pool, 2, &b1))
        return 1;
    if (matches(pool, 1, &a2) && matches(pool, 2, &b1))
        return 2;
    if (matches(pool, 1, &a2) && matches(pool, 2, &b2))
        return 3;
    if (matches(pool, 1, NULL) && matches(pool, 2, &b2))
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
                doc v = make_doc("after", NULL, NULL);
                uint64_t pool, e;
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultenv_reboot(&fe);
                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(), "recovery open failed");
                k = observed_state(&pool);
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "documents in a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                if (pool == 0)
                    pool = make_pool("adr");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, v.b, v.n, &e));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
                TEST_ASSERT_TRUE_MESSAGE(k == 0 || e >= 3, "an entry ID was reused");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_insert_and_get);
    RUN_TEST(test_replace_keeps_history_and_summary);
    RUN_TEST(test_patch_nested_set_and_remove);
    RUN_TEST(test_delete_writes_tombstone);
    RUN_TEST(test_checkpoints);
    RUN_TEST(test_reopen_and_ids_continue);
    RUN_TEST(test_replay_after_crash_and_ids_not_reused);
    RUN_TEST(test_rejects_bad_input);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
