#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "cdb_db.h"
#include "cdb_faultenv.h"

#define FCAP (640u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define STORE 1u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[1024];
static uint8_t page[PAGE];
static cdb_db_config cfg;
static cdb_db db;

/* A tiny key-value store: entry N lives in logical page N. */
static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    cdb_page_header t;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    if (d->op == CDB_OP_DELETE) {
        s = cdb_pager_free(p, d->entry_id);
        return s == CDB_E_NOT_FOUND ? CDB_OK : s;
    }
    while (p->next_lid <= d->entry_id)
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
            return s;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    return cdb_pager_write(p, d->entry_id, &t, d->data, d->len);
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
        h.db_id[i] = (uint8_t)(0x30 + i);
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
    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof arena;
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof free_list / sizeof free_list[0];
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof record_buf;
    cfg.apply = kv_apply;
    cfg.durability = CDB_DURABILITY_FULL;
}

void tearDown(void) {}

static cdb_status open_db(const cdb_env *e, const char *base)
{
    cdb_header h = settings();
    return cdb_db_open(&db, e, base, &cfg, &h);
}

static cdb_status put(uint64_t entry, const char *text)
{
    return cdb_tx_put(&db, STORE, entry, CDB_OP_REPLACE, text, strlen(text));
}

static cdb_status del(uint64_t entry)
{
    return cdb_tx_put(&db, STORE, entry, CDB_OP_DELETE, NULL, 0);
}

/* Returns 1 and copies the value if the entry exists. */
static int get(uint64_t entry, char *out, size_t cap)
{
    cdb_page_header h;
    cdb_status s = cdb_pager_read(&db.pager, entry, page, &h);
    if (s == CDB_E_NOT_FOUND)
        return 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    TEST_ASSERT_TRUE(h.payload_len < cap);
    memcpy(out, page + cdb_page_header_size(db.pager.fmt), h.payload_len);
    out[h.payload_len] = '\0';
    return 1;
}

static void expect(uint64_t entry, const char *text)
{
    char v[64];
    if (text == NULL) {
        TEST_ASSERT_FALSE(get(entry, v, sizeof v));
    } else {
        TEST_ASSERT_TRUE(get(entry, v, sizeof v));
        TEST_ASSERT_EQUAL_STRING(text, v);
    }
}

static int path_exists(const char *path)
{
    int e = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->exists(env.ctx, path, &e));
    return e;
}

/* ------------------------------------------------------------ basics */

static void test_roundtrip_and_close_leaves_one_file(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_TRUE(path_exists("t.cdbwork/main.0.cdb"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "one"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(2, "two"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    expect(1, "one");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_TRUE(path_exists("t.cdbarch"));
    TEST_ASSERT_FALSE(path_exists("t.cdbwork")); /* one file at rest (spec 1.3) */

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, "one");
    expect(2, "two");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_open_missing_without_create(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_db_open(&db, &env, "nope", &cfg, NULL));
}

static void test_abort_and_empty_transactions(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "never"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db));
    expect(1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, put(1, "outside a transaction"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_full_commit_replayed_after_crash(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "durable"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* Not reconciled: the archive does not have it yet, only the journal. */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, "durable");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_uncommitted_tail_discarded(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "kept"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "no commit record"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(2, "no commit record"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&db.journal_st)); /* durable, yet uncommitted */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, "kept");
    expect(2, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_reconcile_then_reopen(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_OFF));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "reconciled"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT64(CDB_JOURNAL_HEADER_SIZE, db.journal.end); /* a fresh journal */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, "reconciled");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_missing_journal_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "only in the journal"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->remove(env.ctx, "t.cdbwork/main.0.cdb"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "t.cdbwork"));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, open_db(&env, "t")); /* spec 1.12.5 */
    cfg.flags = CDB_DB_ACCEPT_LOSS;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_damaged_journal_is_set_aside(void)
{
    cdb_faultdev *j;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "x"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    j = cdb_faultenv_device(&fe, "t.cdbwork/main.0.cdb");
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(j, 20, 0)); /* inside the database ID */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, open_db(&env, "t"));
    TEST_ASSERT_TRUE(path_exists("t.cdbwork/main.0.cdb.stray")); /* set aside, never applied */
    cfg.flags = CDB_DB_ACCEPT_LOSS;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_compaction_through_db(void)
{
    cdb_faultdev *arch;
    uint64_t moved, reclaimed, before, after;
    char text[32];
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 1; i <= 120; i++) {
        snprintf(text, sizeof text, "row-%u", i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, put(i, text));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 1; i <= 120; i++)
        if (i % 6 != 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, del(i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db)); /* nothing new: no-op */

    arch = cdb_faultenv_device(&fe, "t.cdbarch");
    TEST_ASSERT_NOT_NULL(arch);
    before = arch->current_size;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_compact(&db, 1000, &moved, &reclaimed));
    after = arch->current_size;
    TEST_ASSERT_TRUE_MESSAGE(after < before, "the archive did not shrink");
    for (i = 1; i <= 120; i++) {
        snprintf(text, sizeof text, "row-%u", i);
        expect(i, i % 6 == 0 ? text : NULL);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
    expect(6, "row-6");
    expect(7, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* Committed states, in order. Entries 1..3; NULL = absent. */
static const char *const states[5][3] = {
    { NULL, NULL, NULL },   /* S0: empty */
    { "a1", "a2", NULL },   /* S1: T1 */
    { "b1", "a2", NULL },   /* S2: T2 */
    { "b1", NULL, "c3" },   /* S3: T4 */
    { "d1", NULL, "c3" },   /* S4: T5 (durability off) */
};

static cdb_status workload(int *must_have)
{
    cdb_status s;
    *must_have = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db(&env, "t"));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(put(1, "a1"));
    TRY(put(2, "a2"));
    TRY(cdb_tx_commit(&db));
    *must_have = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(put(1, "b1"));
    TRY(cdb_tx_commit(&db));
    *must_have = 2;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(put(3, "aborted"));
    TRY(cdb_tx_abort(&db));
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(put(3, "c3"));
    TRY(del(2));
    TRY(cdb_tx_commit(&db));
    *must_have = 3;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_OFF));
    TRY(put(1, "d1"));
    TRY(cdb_tx_commit(&db)); /* acknowledged, but not durable until close */
    TRY(cdb_db_close(&db));
    *must_have = 4;
#undef TRY
    return CDB_OK;
}

static int matching_state(void)
{
    int k, e;
    char v[64];
    for (k = 0; k < 5; k++) {
        int match = 1;
        for (e = 0; e < 3 && match; e++) {
            int present = get((uint64_t)e + 1, v, sizeof v);
            if (states[k][e] == NULL)
                match = !present;
            else
                match = present && strcmp(states[k][e], v) == 0;
        }
        if (match)
            return k;
    }
    return -1;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 30, 300 };
    uint64_t total, op;
    unsigned runs = 0;
    int must, m;

    init_env();
    TEST_ASSERT_EQUAL_INT(CDB_OK, workload(&must));
    total = fe.clock.ops;
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }

    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 3 : 1;
            for (t = 0; t < nt; t++) {
                char v[64];
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&must));
                cdb_faultenv_reboot(&fe);

                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(&env, "t"), "recovery open failed");
                k = matching_state();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "recovered a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= must || (must == 4 && k >= 3),
                                         "an acknowledged durable commit was lost");
                /* Still usable: write, close, reopen. */
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "after"));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
                TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&env, "t"));
                TEST_ASSERT_TRUE(get(1, v, sizeof v));
                TEST_ASSERT_EQUAL_STRING("after", v);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
                runs++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)(total * 6u), runs);
}

/* ------------------------------------------------------- real files */

static void test_host_filesystem(void)
{
    static cdb_hostenv he;
    cdb_env host;
    char base[256], path[300];
    int exists;

    snprintf(base, sizeof base, "%s/db_%ld", CDB_TEST_TMPDIR, (long)getpid());
    snprintf(path, sizeof path, "%s.cdbarch", base);
    unlink(path);
    cdb_hostenv_init(&he, &host);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&host, base));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, put(1, "on disk"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    snprintf(path, sizeof path, "%s.cdbwork", base);
    TEST_ASSERT_EQUAL_INT(CDB_OK, host.ops->exists(host.ctx, path, &exists));
    TEST_ASSERT_FALSE(exists);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db(&host, base));
    expect(1, "on disk");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    snprintf(path, sizeof path, "%s.cdbarch", base);
    unlink(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_and_close_leaves_one_file);
    RUN_TEST(test_open_missing_without_create);
    RUN_TEST(test_abort_and_empty_transactions);
    RUN_TEST(test_full_commit_replayed_after_crash);
    RUN_TEST(test_uncommitted_tail_discarded);
    RUN_TEST(test_reconcile_then_reopen);
    RUN_TEST(test_missing_journal_is_reported);
    RUN_TEST(test_damaged_journal_is_set_aside);
    RUN_TEST(test_compaction_through_db);
    RUN_TEST(test_crash_sweep);
    RUN_TEST(test_host_filesystem);
    return UNITY_END();
}
