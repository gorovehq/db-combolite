/* Documents larger than a page (spec 14.9, 14.15): bodies up to doc_max
 * continue in overflow chains. test_doc covers doc_max 0. */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"

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
static uint8_t doc_buffers[3u * DBUF + 3u * PAGE];
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
    TEST_ASSERT_EQUAL_size_t(cdb_doc_buffers_size(PAGE, DOC_MAX), sizeof doc_buffers);
    TEST_ASSERT_EQUAL_size_t(cdb_fts_buffers_size(PAGE, DOC_MAX), sizeof fts_buffers);
    TEST_ASSERT_EQUAL_size_t(cdb_fts_tokens_needed(PAGE, DOC_MAX), cfg.fts_tokens_cap);
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x6E, sizeof h.db_id);
    return cdb_db_open(&db, &env, "big", &cfg, &h);
}

static uint64_t make_pool(void)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "books", 5, CDB_MODEL_DOCUMENT, &id));
    return id;
}

/* ------------------------------------------------------------ documents */

typedef struct big {
    uint8_t b[DOC_MAX + 256u];
    size_t n;
} big;

/* Distinct words in a text; 0: every word differs. */
static unsigned vocab;

/* Text of exactly len bytes: words "s<seed>w<i>" separated by spaces, with
 * tail (if any) as the last word. */
static void make_text(char *t, size_t len, unsigned seed, const char *tail)
{
    size_t at = 0, tl = tail ? strlen(tail) : 0;
    unsigned i = 0;
    while (at < len) {
        char word[24];
        int n = snprintf(word, sizeof word, "s%uw%u ", seed, vocab ? i++ % vocab : i++);
        size_t k = (size_t)n;
        if (k > len - at)
            k = len - at;
        memcpy(t + at, word, k);
        at += k;
    }
    if (tl && tl < len) {
        t[len - tl - 1] = ' ';
        memcpy(t + len - tl, tail, tl);
    }
}

/* { 1: text, 2: seed, 3: extra text (if extra_len) }. */
static void make_big(big *d, unsigned seed, size_t text_len, const char *tail, size_t extra_len)
{
    static char text[DOC_MAX + 256u];
    cdb_value_writer w;
    cdb_value_writer_init(&w, d->b, sizeof d->b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    make_text(text, text_len, seed, tail);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, text, text_len);
    cdb_value_field(&w, 2);
    cdb_value_write_int(&w, (int64_t)seed);
    if (extra_len) {
        make_text(text, extra_len, seed + 1000u, NULL);
        cdb_value_field(&w, 3);
        cdb_value_write_string(&w, text, extra_len);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d->n = cdb_value_writer_len(&w);
}

/* A patch setting field to a text of len bytes. */
static void make_text_patch(big *d, uint64_t field, size_t len, unsigned seed)
{
    static char text[DOC_MAX];
    cdb_value_writer w;
    make_text(text, len, seed, NULL);
    cdb_value_writer_init(&w, d->b, sizeof d->b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, CDB_PATCH_SET);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, (int64_t)field);
    cdb_value_end_array(&w);
    cdb_value_write_string(&w, text, len);
    cdb_value_end_array(&w);
    cdb_value_end_array(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d->n = cdb_value_writer_len(&w);
}

static int matches(uint64_t pool, uint64_t entry, uint64_t rev, const big *want, const char *summary)
{
    size_t len, slen;
    cdb_status s = cdb_doc_get(&db, pool, entry, rev, out, sizeof out, &len, summary_out, sizeof summary_out, &slen,
                               NULL);
    if (want == NULL)
        return s == CDB_E_NOT_FOUND;
    if (s != CDB_OK || len != want->n || memcmp(out, want->b, len) != 0)
        return 0;
    if (summary)
        return slen == strlen(summary) && memcmp(summary_out, summary, slen) == 0;
    return 1;
}

static void expect(uint64_t pool, uint64_t entry, uint64_t rev, const big *want, const char *summary)
{
    TEST_ASSERT_TRUE_MESSAGE(matches(pool, entry, rev, want, summary), "document differs");
}

/* ------------------------------------------------------------ tests */

static void test_round_trip_sizes(void)
{
    static big d[8], too;
    static const size_t text[7] = { 10, 900, 960, 1000, 2000, 6000, 0 };
    uint64_t pool, e[8];
    size_t i, max_text = DOC_MAX;

    /* The largest text whose document is exactly within doc_max. */
    do
        make_big(&too, 7, max_text--, NULL, 0);
    while (too.n > DOC_MAX);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 7; i++) {
        make_big(&d[i], (unsigned)i, text[i] ? text[i] : max_text + 1u, NULL, 0);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, i % 2 ? "a summary" : NULL, i % 2 ? 9 : 0, d[i].b,
                                                     d[i].n, &e[i]));
    }
    TEST_ASSERT_EQUAL_size_t(DOC_MAX, d[6].n);
    /* One byte past doc_max is refused when the change is made. */
    make_big(&too, 7, max_text + 2u, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(DOC_MAX + 1u, too.n);
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_doc_insert(&db, pool, NULL, 0, too.b, too.n, &e[7]));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    for (i = 0; i < 7; i++)
        expect(pool, e[i], 0, &d[i], i % 2 ? "a summary" : NULL);
    /* A buffer too small reports the length. */
    {
        size_t len;
        TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL,
                              cdb_doc_get(&db, pool, e[6], 0, out, 100, &len, NULL, 0, NULL, NULL));
        TEST_ASSERT_EQUAL_size_t(DOC_MAX, len);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    for (i = 0; i < 7; i++)
        expect(pool, e[i], 0, &d[i], i % 2 ? "a summary" : NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Revisions of a large document: deltas and patches that themselves need
 * overflow chains, checkpoints, and every revision read back. */
static void test_history(void)
{
    static big rev[7], p;
    uint64_t pool, e;
    unsigned r;
    cfg.checkpoint_every = 3;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    make_big(&rev[1], 1, 6000, NULL, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, "first", 5, rev[1].b, rev[1].n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* rev 2: a replace whose patch is a whole new text (a delta past the page). */
    make_big(&rev[2], 2, 6500, NULL, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, rev[2].b, rev[2].n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* rev 3: a large patch adds field 3. */
    make_text_patch(&p, 3, 4000, 33);
    {
        /* rev[3] is rev[2] with field 3 set to the patch's text (seed 33). */
        static char text[DOC_MAX];
        cdb_value_writer w;
        make_text(text, 6500, 2, NULL);
        cdb_value_writer_init(&w, rev[3].b, sizeof rev[3].b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, text, 6500);
        cdb_value_field(&w, 2);
        cdb_value_write_int(&w, 2);
        make_text(text, 4000, 33, NULL);
        cdb_value_field(&w, 3);
        cdb_value_write_string(&w, text, 4000);
        cdb_value_end_object(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
        rev[3].n = cdb_value_writer_len(&w);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p.b, p.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* rev 4: small (the chain is full: a checkpoint), with a new summary. */
    make_big(&rev[4], 4, 30, NULL, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, "small", 5, rev[4].b, rev[4].n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    /* rev 5 and 6: large again, as deltas. */
    make_big(&rev[5], 5, 11000, NULL, 0);
    make_big(&rev[6], 6, 3000, NULL, 2000);
    for (r = 5; r <= 6; r++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, rev[r].b, rev[r].n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    for (r = 1; r <= 6; r++)
        expect(pool, e, r, &rev[r], r < 4 ? "first" : "small");
    expect(pool, e, 0, &rev[6], "small");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    for (r = 1; r <= 6; r++)
        expect(pool, e, r, &rev[r], r < 4 ? "first" : "small");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static cdb_fts_config fts_text(void)
{
    cdb_fts_config c;
    memset(&c, 0, sizeof c);
    c.paths[0].ids[0] = 1;
    c.paths[0].depth = 1;
    c.npaths = 1;
    c.summary = 1;
    return c;
}

static size_t hits_for(uint64_t pool, const char *word, uint64_t *first)
{
    cdb_fts_hit hits[4];
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_search(&db, pool, word, strlen(word), CDB_FTS_ANY, hits, 4, &n));
    if (n && first)
        *first = hits[0].entry;
    return n;
}

/* A word that only the overflow part of a body holds is indexed, and
 * leaves the index when it leaves the document. */
static void test_full_text_sees_the_whole_body(void)
{
    static big a, b;
    cdb_fts_config c = fts_text();
    uint64_t pool, e, got = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_create_full_text(&db, pool, &c));
    make_big(&a, 1, 9000, "zebrafinch", 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, a.b, a.n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_size_t(1, hits_for(pool, "zebrafinch", &got));
    TEST_ASSERT_EQUAL_UINT64(e, got);
    TEST_ASSERT_EQUAL_size_t(1, hits_for(pool, "s1w0", NULL));
    make_big(&b, 2, 9000, "kingfisher", 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, b.b, b.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_size_t(0, hits_for(pool, "zebrafinch", NULL));
    TEST_ASSERT_EQUAL_size_t(1, hits_for(pool, "kingfisher", NULL));
    /* Every word of the old text left: the start, the middle, the chain. */
    TEST_ASSERT_EQUAL_size_t(0, hits_for(pool, "s1w0", NULL));
    TEST_ASSERT_EQUAL_size_t(0, hits_for(pool, "s1w300", NULL));
    TEST_ASSERT_EQUAL_size_t(0, hits_for(pool, "s1w380", NULL));
    TEST_ASSERT_EQUAL_size_t(0, hits_for(pool, "s1w1000", NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Several large changes to one document in one transaction, read back from
 * the journal (staging overflows at once). */
static void test_transaction_view(void)
{
    static big a, b, p, want;
    uint64_t pool, e;
    cfg.doc_stage_size = 64;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    make_big(&a, 1, 5000, NULL, 0);
    make_big(&b, 2, 7000, NULL, 0);
    make_text_patch(&p, 3, 5000, 9);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, a.b, a.n, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, NULL, 0, b.b, b.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p.b, p.n));
    /* A second large field would pass doc_max: refused, as the transaction sees it. */
    {
        static big more;
        make_text_patch(&more, 4, 1000, 10);
        TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_doc_patch(&db, pool, e, more.b, more.n));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        static char text[DOC_MAX];
        cdb_value_writer w;
        cdb_value_writer_init(&w, want.b, sizeof want.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        make_text(text, 7000, 2, NULL);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, text, 7000);
        cdb_value_field(&w, 2);
        cdb_value_write_int(&w, 2);
        make_text(text, 5000, 9, NULL);
        cdb_value_field(&w, 3);
        cdb_value_write_string(&w, text, 5000);
        cdb_value_end_object(&w);
        want.n = cdb_value_writer_len(&w);
    }
    expect(pool, e, 0, &want, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: A = a1 ("a"), B = b1. S2: A = a2, B = b1 + field 3.
 * S3: A deleted. Full-text on, so recovery replays the index too. */
static big a1, a2, b1, b2, bp;

static void sweep_docs(void)
{
    static char text[DOC_MAX];
    cdb_value_writer w;
    /* Each spans a few pages: enough for chains, small enough to replay at
     * every crash point. A small vocabulary keeps the full-text index's
     * writes few. */
    vocab = 8;
    make_big(&a1, 1, 2500, NULL, 0);
    make_big(&a2, 2, 3500, NULL, 0);
    make_big(&b1, 3, 1500, NULL, 0);
    make_text_patch(&bp, 3, 1200, 4);
    cdb_value_writer_init(&w, b2.b, sizeof b2.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    make_text(text, 1500, 3, NULL);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, text, 1500);
    cdb_value_field(&w, 2);
    cdb_value_write_int(&w, 3);
    make_text(text, 1200, 4, NULL);
    cdb_value_field(&w, 3);
    cdb_value_write_string(&w, text, 1200);
    cdb_value_end_object(&w);
    b2.n = cdb_value_writer_len(&w);
}

static cdb_status workload(int *acked)
{
    cdb_fts_config c = fts_text();
    uint64_t pool, ea, eb;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_db_create_store(&db, "books", 5, CDB_MODEL_DOCUMENT, &pool));
    TRY(cdb_doc_create_full_text(&db, pool, &c));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_insert(&db, pool, "a", 1, a1.b, a1.n, &ea));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, b1.b, b1.n, &eb));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_replace(&db, pool, ea, NULL, 0, a2.b, a2.n));
    TRY(cdb_doc_patch(&db, pool, eb, bp.b, bp.n));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_delete(&db, pool, ea));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static int observed_state(uint64_t *pool_out)
{
    uint64_t pool;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "books", 5, &pool, &found));
    *pool_out = found ? pool : 0;
    if (!found || (matches(pool, 1, 0, NULL, NULL) && matches(pool, 2, 0, NULL, NULL)))
        return 0;
    if (matches(pool, 1, 0, &a1, "a") && matches(pool, 2, 0, &b1, NULL))
        return 1;
    if (matches(pool, 1, 0, &a2, "a") && matches(pool, 2, 0, &b2, NULL))
        return 2;
    if (matches(pool, 1, 0, NULL, NULL) && matches(pool, 2, 0, &b2, NULL) && matches(pool, 1, 1, &a1, "a"))
        return 3;
    return -1;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[2] = { 0, 300 };
    uint64_t total, op;
    int acked, m;

    sweep_docs();
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
                    pool = make_pool();
                /* Still takes a large document. */
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, a2.b, a2.n, &e));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
                TEST_ASSERT_TRUE(matches(pool, e, 0, &a2, NULL));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_sizes);
    RUN_TEST(test_history);
    RUN_TEST(test_full_text_sees_the_whole_body);
    RUN_TEST(test_transaction_view);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
