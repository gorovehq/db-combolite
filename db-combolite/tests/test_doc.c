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
static uint8_t doc_buffers[6u * PAGE];
static uint8_t doc_stage[4096];
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
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
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
    /* Changing a deleted document is refused when the change is made. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_patch(&db, pool, e, p.b, p.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_replace(&db, pool, e, NULL, 0, v1.b, v1.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_delete(&db, pool, e));
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

/* ------------------------------------------------------- schemas */

/* Closed: { 1: string, required, <= 32 bytes;  2: string;
 *           3: object { 1: string, required } } -- the shape of make_doc. */
static doc adr_schema(void)
{
    doc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_SCHEMA_OPEN);
    cdb_value_write_bool(&w, 0);
    cdb_value_field(&w, CDB_SCHEMA_FIELDS);
    cdb_value_begin_array(&w);

    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_RULE_FIELD);
    cdb_value_write_int(&w, 1);
    cdb_value_field(&w, CDB_RULE_TYPES);
    cdb_value_write_int(&w, (int64_t)CDB_SCHEMA_TYPE(CDB_TYPE_STRING));
    cdb_value_field(&w, CDB_RULE_REQUIRED);
    cdb_value_write_bool(&w, 1);
    cdb_value_field(&w, CDB_RULE_MAX_LEN);
    cdb_value_write_int(&w, 32);
    cdb_value_end_object(&w);

    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_RULE_FIELD);
    cdb_value_write_int(&w, 2);
    cdb_value_field(&w, CDB_RULE_TYPES);
    cdb_value_write_int(&w, (int64_t)CDB_SCHEMA_TYPE(CDB_TYPE_STRING));
    cdb_value_end_object(&w);

    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_RULE_FIELD);
    cdb_value_write_int(&w, 3);
    cdb_value_field(&w, CDB_RULE_TYPES);
    cdb_value_write_int(&w, (int64_t)CDB_SCHEMA_TYPE(CDB_TYPE_OBJECT));
    cdb_value_field(&w, CDB_RULE_OBJECT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_SCHEMA_FIELDS);
    cdb_value_begin_array(&w);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, CDB_RULE_FIELD);
    cdb_value_write_int(&w, 1);
    cdb_value_field(&w, CDB_RULE_TYPES);
    cdb_value_write_int(&w, (int64_t)CDB_SCHEMA_TYPE(CDB_TYPE_STRING));
    cdb_value_field(&w, CDB_RULE_REQUIRED);
    cdb_value_write_bool(&w, 1);
    cdb_value_end_object(&w);
    cdb_value_end_array(&w);
    cdb_value_end_object(&w);
    cdb_value_end_object(&w);

    cdb_value_end_array(&w);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static uint32_t set_schema(uint64_t pool, const doc *schema)
{
    uint32_t version = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_set_schema(&db, pool, schema->b, schema->n, &version));
    return version;
}

static uint32_t schema_of(uint64_t pool, uint64_t entry, uint64_t rev)
{
    cdb_doc_info info;
    size_t len;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, entry, rev, out, sizeof out, &len, NULL, 0, NULL, &info));
    return info.schema_version;
}

static void test_schema_enforced(void)
{
    doc schema = adr_schema(), untitled = make_doc(NULL, "draft", NULL), good = make_doc("ADR-1", "draft", NULL);
    const uint64_t extra[1] = { 9 }, owner_field[1] = { 3 }, owner[2] = { 3, 1 }, status[1] = { 2 },
                   title[1] = { 1 };
    doc p_extra = make_patch(extra, 1, "x"), p_owner_str = make_patch(owner_field, 1, "casey"),
        p_owner = make_patch(owner, 2, "casey"), p_status = make_patch(status, 1, "accepted"),
        p_title = make_patch(title, 1, "ADR-0");
    uint64_t pool, old, e, e2;
    uint32_t version;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    old = insert(pool, NULL, &untitled); /* no schema yet: accepted */
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_doc_set_schema(&db, pool, schema.b, schema.n, &version));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_UINT32(1, set_schema(pool, &schema));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_insert(&db, pool, NULL, 0, untitled.b, untitled.n, &e));
    e = insert(pool, NULL, &good);
    TEST_ASSERT_EQUAL_UINT64(old + 1u, e); /* the refused insert used no ID */
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_replace(&db, pool, e, NULL, 0, untitled.b, untitled.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, e, p_extra.b, p_extra.n));     /* closed */
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, e, p_owner_str.b, p_owner_str.n)); /* type */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p_owner.b, p_owner.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        doc want = make_doc("ADR-1", "draft", "casey");
        expect_doc(pool, e, 0, &want); /* nothing refused reached the journal */
    }
    TEST_ASSERT_EQUAL_UINT32(1, schema_of(pool, e, 0));
    TEST_ASSERT_EQUAL_UINT32(0, schema_of(pool, old, 0));

    /* Old documents are not re-validated, but a new revision must conform. */
    TEST_ASSERT_TRUE(doc_exists(pool, old));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, old, p_status.b, p_status.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, old, p_title.b, p_title.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_UINT32(1, schema_of(pool, old, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* The schema survives reopening. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_insert(&db, pool, NULL, 0, untitled.b, untitled.n, &e2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        uint8_t got[256];
        size_t len;
        uint32_t current;
        uint8_t empty[64];
        size_t empty_len;
        {
            cdb_value_writer w;
            cdb_value_writer_init(&w, empty, sizeof empty, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
            cdb_value_begin_object(&w);
            cdb_value_end_object(&w);
            TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
            empty_len = cdb_value_writer_len(&w);
        }
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get_schema(&db, pool, 0, got, sizeof got, &len, &current));
        TEST_ASSERT_EQUAL_UINT32(1, current);
        TEST_ASSERT_EQUAL_size_t(schema.n, len);
        TEST_ASSERT_EQUAL_MEMORY(schema.b, got, len);

        /* {} accepts everything: the way a schema is cleared. History stays. */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_set_schema(&db, pool, empty, empty_len, &version));
        TEST_ASSERT_EQUAL_UINT32(2, version);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get_schema(&db, pool, 1, got, sizeof got, &len, &current));
        TEST_ASSERT_EQUAL_UINT32(2, current);
        TEST_ASSERT_EQUAL_MEMORY(schema.b, got, len);
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get_schema(&db, pool, 3, got, sizeof got, &len, NULL));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e2 = insert(pool, NULL, &untitled);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_UINT32(2, schema_of(pool, e2, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_set_schema_rejects_bad_schemas(void)
{
    const uint8_t not_object[] = { CDB_TAG_INT, 2 };
    uint8_t unknown[64]; /* { 9: true } */
    size_t unknown_len;
    uint64_t pool, rows;
    uint32_t version, current;
    size_t len;
    {
        cdb_value_writer w;
        cdb_value_writer_init(&w, unknown, sizeof unknown, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 9);
        cdb_value_write_bool(&w, 1);
        cdb_value_end_object(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
        unknown_len = cdb_value_writer_len(&w);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "rows", 4, CDB_MODEL_RELATIONAL, &rows));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_set_schema(&db, pool, not_object, sizeof not_object, &version));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_set_schema(&db, pool, NULL, 0, &version));
    {
        int trivial;
        TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_schema_check(unknown, unknown_len, CDB_ORDER_LITTLE, &trivial));
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_doc_set_schema(&db, pool, unknown, unknown_len, &version));
    {
        doc schema = adr_schema();
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_set_schema(&db, rows, schema.b, schema.n, &version));
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get_schema(&db, pool, 0, out, sizeof out, &len, &current));
    TEST_ASSERT_EQUAL_UINT32(0, current); /* nothing was added */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A transaction's later changes are validated against its earlier ones,
 * whether the stage holds them, has overflowed, or is absent. */
static void staging_case(size_t stage_size)
{
    doc schema = adr_schema(), t = make_doc("T", NULL, NULL), t2 = make_doc("T2", NULL, NULL);
    const uint64_t owner[2] = { 3, 1 }, status[1] = { 2 }, title[1] = { 1 };
    doc set_owner = make_patch(owner, 2, "sam"), drop_owner = make_patch(owner, 2, NULL),
        set_status = make_patch(status, 1, "s"), drop_title = make_patch(title, 1, NULL);
    uint64_t pool, e, n, many[40];
    int i;

    cfg.doc_stage = stage_size ? doc_stage : NULL;
    cfg.doc_stage_size = stage_size;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    set_schema(pool, &schema);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, NULL, &t);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Each patch alone is valid against the committed {1: "T"}; together the
     * second leaves the owner object without its required field. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 3; i++) { /* fill a small stage first */
        doc filler = make_doc("filler", NULL, NULL);
        (void)insert(pool, NULL, &filler);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, set_owner.b, set_owner.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, e, drop_owner.b, drop_owner.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        doc want = make_doc("T", NULL, "sam");
        expect_doc(pool, e, 0, &want);
    }

    /* Insert, patch and delete in one transaction; then it is gone. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    n = insert(pool, NULL, &t);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, n, set_status.b, set_status.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, n, drop_title.b, drop_title.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_patch(&db, pool, n, set_status.b, set_status.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_replace(&db, pool, n, NULL, 0, t.b, t.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_delete(&db, pool, n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_FALSE(doc_exists(pool, n));

    /* A replace, then a patch validated against the replacement. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, t2.b, t2.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, e, drop_title.b, drop_title.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, set_owner.b, set_owner.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        doc want = make_doc("T2", NULL, "sam");
        expect_doc(pool, e, 0, &want);
    }

    /* A large transaction is never refused for lack of stage. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 40; i++)
        many[i] = insert(pool, NULL, &t);
    for (i = 0; i < 40; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, many[i], set_status.b, set_status.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_doc_patch(&db, pool, many[0], drop_title.b, drop_title.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        doc want = make_doc("T", "s", NULL);
        for (i = 0; i < 40; i++)
            expect_doc(pool, many[i], 0, &want);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_staging_large(void) { staging_case(sizeof doc_stage); }
static void test_staging_overflows(void) { staging_case(48); }
static void test_staging_absent(void) { staging_case(0); }

/* ------------------------------------------------------- page limits
 *
 * Every revision, including a later checkpoint that carries the summary,
 * must fit in one page. A change that would break that is refused when it
 * is made: failing at commit would fail again on every replay. */

static doc string_patch(uint64_t field, size_t n)
{
    static char text[PAGE];
    doc d;
    cdb_value_writer w;
    memset(text, 'x', n);
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, CDB_PATCH_SET);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, (int64_t)field);
    cdb_value_end_array(&w);
    cdb_value_write_string(&w, text, n);
    cdb_value_end_array(&w);
    cdb_value_end_array(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static void test_growth_past_the_page_is_refused(void)
{
    static char sum[500];
    doc v = make_doc("t", NULL, NULL), grow = string_patch(2, 200), more = string_patch(4, 200);
    uint64_t pool, e, e2;
    int round;

    memset(sum, 's', sizeof sum);
    cfg.checkpoint_every = 2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    e = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, sum, sizeof sum, v.b, v.n, &e2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Each patch is small; the document with its 500-byte summary is not. */
    for (round = 0; round < 2; round++) {
        uint64_t target = round == 0 ? e2 : e;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, target, grow.b, grow.n));
        TEST_ASSERT_EQUAL_INT(round == 0 ? CDB_E_OVERFLOW : CDB_OK, cdb_doc_patch(&db, pool, target, more.b, more.n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    /* A replace without a summary keeps the old one, and must fit with it. */
    {
        static uint8_t big[PAGE];
        static char text[600];
        cdb_value_writer w;
        size_t n;
        memset(text, 'y', sizeof text);
        cdb_value_writer_init(&w, big, sizeof big, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, text, sizeof text);
        cdb_value_end_object(&w);
        n = cdb_value_writer_len(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
        TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_doc_replace(&db, pool, e2, NULL, 0, big, n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e2, "short", 5, big, n)); /* a new summary fits */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    /* Every commit applied and survives a crash; checkpoints were written. */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_TRUE(doc_exists(pool, e));
    TEST_ASSERT_TRUE(doc_exists(pool, e2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A replace is stored as a patch of the changed fields unless that patch
 * would not fit its page; then the whole document is written instead. */
static void test_replace_falls_back_to_full_revision(void)
{
    static uint8_t a[4u * PAGE], b[4u * PAGE];
    cdb_value_writer w;
    cdb_doc_info info;
    size_t an, bn, len;
    uint64_t pool, e;
    int i;

    cdb_value_writer_init(&w, a, sizeof a, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 1; i <= 120; i++) {
        cdb_value_field(&w, (uint64_t)i);
        cdb_value_write_int(&w, 0);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    an = cdb_value_writer_len(&w);
    cdb_value_writer_init(&w, b, sizeof b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 1; i <= 120; i++) {
        cdb_value_field(&w, (uint64_t)i);
        cdb_value_write_int(&w, 1);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    bn = cdb_value_writer_len(&w);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "document %u bytes", (unsigned)bn);
        TEST_MESSAGE(msg);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, a, an, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, b, bn));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &len, NULL, 0, NULL, &info));
    TEST_ASSERT_EQUAL_UINT8(CDB_REV_CHECKPOINT, info.kind);
    TEST_ASSERT_EQUAL_MEMORY(b, out, bn);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, e, 1, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(a, out, an); /* history intact */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A dropped pool reads as dropped at once, not only after reopening. */
static void test_dropped_pool_is_gone(void)
{
    doc v = make_doc("x", NULL, NULL);
    uint64_t pool, e, e2;
    size_t len;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool("adr");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(doc_exists(pool, e)); /* the pool is now open */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, pool));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_insert(&db, pool, NULL, 0, v.b, v.n, &e2));
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
    {
        doc schema = adr_schema();
        uint32_t version;
        TRY(cdb_doc_set_schema(&db, pool, schema.b, schema.n, &version));
    }
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
                if (pool == 0) {
                    pool = make_pool("adr");
                } else {
                    /* If the schema survived, it is enforced. */
                    doc bad = make_doc(NULL, "x", NULL);
                    uint32_t current;
                    size_t len;
                    cdb_status gs = cdb_doc_get_schema(&db, pool, 0, out, sizeof out, &len, &current);
                    TEST_ASSERT_TRUE(gs == CDB_OK || (gs == CDB_E_NOT_FOUND && current == 0));
                    TEST_ASSERT_TRUE_MESSAGE(k == 0 || current == 1, "a committed schema was lost");
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                    TEST_ASSERT_EQUAL_INT(current ? CDB_E_SCHEMA : CDB_OK,
                                          cdb_doc_insert(&db, pool, NULL, 0, bad.b, bad.n, &e));
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));
                }
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
    RUN_TEST(test_growth_past_the_page_is_refused);
    RUN_TEST(test_replace_falls_back_to_full_revision);
    RUN_TEST(test_dropped_pool_is_gone);
    RUN_TEST(test_schema_enforced);
    RUN_TEST(test_set_schema_rejects_bad_schemas);
    RUN_TEST(test_staging_large);
    RUN_TEST(test_staging_overflows);
    RUN_TEST(test_staging_absent);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
