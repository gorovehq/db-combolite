#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_key.h"
#include "cdb_endian.h"

#define FCAP (1024u * 1024u)
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
static uint8_t fts_buffers[3u * PAGE];
static cdb_fts_token fts_tokens[2u * PAGE];
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
    memset(h.db_id, 0x7E, sizeof h.db_id);
    return cdb_db_open(&db, &env, "f", &cfg, &h);
}

/* ------------------------------------------------------------ words */

static size_t split(const char *text, cdb_fts_token *t, size_t cap)
{
    size_t n = 0;
    (void)cdb_fts_split((const uint8_t *)text, strlen(text), t, cap, &n);
    return n;
}

static void expect_word(const cdb_fts_token *t, const char *want)
{
    char msg[128];
    snprintf(msg, sizeof msg, "want '%s', got '%.*s'", want, (int)t->len, (const char *)t->p);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(want), t->len, msg);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, t->p, t->len, msg);
}

static void test_split_words(void)
{
    cdb_fts_token t[16];
    size_t n = split("Hello, WORLD! it's 2026\xE2\x80\x94ok", t, 16); /* em dash separates */
    TEST_ASSERT_EQUAL_size_t(6, n);
    expect_word(&t[0], "Hello"); /* folded only when compared and keyed */
    expect_word(&t[1], "WORLD");
    expect_word(&t[2], "it");
    expect_word(&t[3], "s");
    expect_word(&t[4], "2026");
    expect_word(&t[5], "ok");

    n = split("na\xC3\xAFve caf\xC3\xA9\xC2\xA0" "bar", t, 16); /* letters kept, no-break space splits */
    TEST_ASSERT_EQUAL_size_t(3, n);
    expect_word(&t[0], "na\xC3\xAFve");
    expect_word(&t[1], "caf\xC3\xA9");
    expect_word(&t[2], "bar");

    n = split("  \t\n...  ", t, 16);
    TEST_ASSERT_EQUAL_size_t(0, n);

    {
        /* Words are cut to 64 bytes, never inside a character. */
        char longw[80], cut[80];
        memset(longw, 'a', 70);
        longw[70] = 0;
        n = split(longw, t, 16);
        TEST_ASSERT_EQUAL_size_t(1, n);
        TEST_ASSERT_EQUAL_UINT32(64, t[0].len);
        memset(cut, 'b', 63);
        memcpy(cut + 63, "\xC3\xA9x", 4); /* the 2-byte character would end at byte 65 */
        n = split(cut, t, 16);
        TEST_ASSERT_EQUAL_UINT32(63, t[0].len);
    }
    {
        /* Malformed UTF-8 still splits deterministically. */
        const char bad[] = "ab\xFF" "cd ef\xC3";
        n = split(bad, t, 16);
        TEST_ASSERT_EQUAL_size_t(2, n);
        TEST_ASSERT_EQUAL_UINT32(5, t[0].len);
        TEST_ASSERT_EQUAL_UINT32(3, t[1].len);
    }
    {
        /* Words that do not fit are counted, not stored. */
        size_t found;
        n = 0;
        found = cdb_fts_split((const uint8_t *)"a b c d", 7, t, 2, &n);
        TEST_ASSERT_EQUAL_size_t(2, n);
        TEST_ASSERT_EQUAL_size_t(4, found);
    }
}

static void test_word_compare_folds_ascii(void)
{
    cdb_fts_token a[4];
    split("Storage STORAGE storage storages", a, 4);
    TEST_ASSERT_EQUAL_INT(0, cdb_fts_word_cmp(&a[0], &a[1]));
    TEST_ASSERT_EQUAL_INT(0, cdb_fts_word_cmp(&a[1], &a[2]));
    TEST_ASSERT_TRUE(cdb_fts_word_cmp(&a[2], &a[3]) < 0);
}

/* ------------------------------------------------------------ documents */

typedef struct doc {
    uint8_t b[512];
    size_t n;
} doc;

/* { 1: title, 2: text, 3: { 1: owner }, 4: [tags...] }; NULL parts left out. */
static doc make_doc(const char *title, const char *text, const char *owner, const char *tag1, const char *tag2)
{
    doc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    if (title) {
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, title, strlen(title));
    }
    if (text) {
        cdb_value_field(&w, 2);
        cdb_value_write_string(&w, text, strlen(text));
    }
    if (owner) {
        cdb_value_field(&w, 3);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_string(&w, owner, strlen(owner));
        cdb_value_end_object(&w);
    }
    if (tag1) {
        cdb_value_field(&w, 4);
        cdb_value_begin_array(&w);
        cdb_value_write_string(&w, tag1, strlen(tag1));
        if (tag2)
            cdb_value_write_string(&w, tag2, strlen(tag2));
        cdb_value_write_int(&w, 7); /* not text: ignored */
        cdb_value_end_array(&w);
    }
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

static doc make_patch(uint64_t field, const char *value)
{
    doc d;
    cdb_value_writer w;
    cdb_value_writer_init(&w, d.b, sizeof d.b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, value ? CDB_PATCH_SET : CDB_PATCH_REMOVE);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, (int64_t)field);
    cdb_value_end_array(&w);
    if (value)
        cdb_value_write_string(&w, value, strlen(value));
    cdb_value_end_array(&w);
    cdb_value_end_array(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    d.n = cdb_value_writer_len(&w);
    return d;
}

/* Indexes title, text and tags, and the summary; not the owner. */
static cdb_fts_config fts_config(void)
{
    cdb_fts_config c;
    memset(&c, 0, sizeof c);
    c.paths[0].ids[0] = 1;
    c.paths[0].depth = 1;
    c.paths[1].ids[0] = 2;
    c.paths[1].depth = 1;
    c.paths[2].ids[0] = 4;
    c.paths[2].depth = 1;
    c.npaths = 3;
    c.summary = 1;
    return c;
}

static uint64_t make_pool(void)
{
    uint64_t id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "notes", 5, CDB_MODEL_DOCUMENT, &id));
    return id;
}

static void create_index(uint64_t pool)
{
    cdb_fts_config c = fts_config();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_create_full_text(&db, pool, &c));
}

static uint64_t insert(uint64_t pool, const char *summary, const doc *d)
{
    uint64_t e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, summary, summary ? strlen(summary) : 0, d->b, d->n, &e));
    return e;
}

static size_t search(uint64_t pool, const char *q, int mode, cdb_fts_hit *hits, size_t max)
{
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_search(&db, pool, q, strlen(q), mode, hits, max, &n));
    return n;
}

/* ------------------------------------------------------------ oracle
 *
 * Rebuilds what the index must hold from the live documents and compares
 * it with the three stored indexes, entry by entry and in total. */

static cdb_fts_token oracle_words[PAGE];

static struct cdb_doc_pool *pool_of(uint64_t store)
{
    struct cdb_doc_pool *p = (struct cdb_doc_pool *)cdb_slots_find(&db.pool_slots, store);
    if (p != NULL)
        return p;
    TEST_FAIL_MESSAGE("pool not open");
    return NULL;
}

static size_t key_of(uint8_t *k, const cdb_fts_token *t, int with_entry, uint64_t entry)
{
    char w[CDB_FTS_MAX_WORD];
    cdb_key_builder kb;
    uint32_t i;
    for (i = 0; i < t->len; i++)
        w[i] = (char)((t->p[i] >= 'A' && t->p[i] <= 'Z') ? t->p[i] + 32 : t->p[i]);
    cdb_key_init(&kb, k, 96);
    cdb_key_string(&kb, w, t->len);
    if (with_entry)
        cdb_key_u64(&kb, entry);
    return kb.len;
}

static uint32_t get_u32(cdb_index *ix, const uint8_t *k, size_t klen, int *found)
{
    uint8_t v[16];
    size_t vlen;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(ix, k, klen, v, sizeof v, &vlen, found));
    if (!*found)
        return 0;
    TEST_ASSERT_EQUAL_size_t(4, vlen);
    return cdb_load_u32(v, CDB_ORDER_BIG);
}

static void verify_index(uint64_t store)
{
    static uint8_t page[PAGE];
    static uint8_t body[PAGE];
    struct cdb_doc_pool *p = pool_of(store);
    cdb_fts_config c = p->fts;
    uint64_t e, live = 0, words = 0, postings = 0, scanned = 0, lengths_rows = 0;
    cdb_index_cursor cur;
    uint8_t k[96], pk[96], v[16];
    size_t klen, vlen, pklen = 0;
    uint32_t prev_count = 0;
    int done, found, have_prev = 0;

    TEST_ASSERT_TRUE(p->fts_on);
    for (e = 1; e < p->next_entry; e++) {
        cdb_fts_doc d;
        size_t blen, slen, unique, total, i;
        uint8_t lk[16];
        cdb_key_builder kb;
        cdb_status s = cdb_doc_get(&db, store, e, 0, body, sizeof body, &blen, summary_out, sizeof summary_out,
                                   &slen, NULL);
        cdb_key_init(&kb, lk, sizeof lk);
        cdb_key_u64(&kb, e);
        if (s == CDB_E_NOT_FOUND) {
            (void)get_u32(&p->fts_lengths, lk, kb.len, &found);
            TEST_ASSERT_FALSE_MESSAGE(found, "a gone document still has a length");
            continue;
        }
        TEST_ASSERT_EQUAL_INT(CDB_OK, s);
        live++;
        d.body = body;
        d.body_len = blen;
        d.summary = slen ? (const uint8_t *)summary_out : NULL;
        d.summary_len = slen;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_fts_collect(&c, &d, CDB_ORDER_LITTLE, oracle_words, PAGE, &unique, &total));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)total, get_u32(&p->fts_lengths, lk, kb.len, &found), "length");
        TEST_ASSERT_TRUE(found);
        words += total;
        postings += unique;
        for (i = 0; i < unique; i++) {
            uint32_t tf = get_u32(&p->fts_postings, k, key_of(k, &oracle_words[i], 1, e), &found);
            TEST_ASSERT_TRUE_MESSAGE(found, "a posting is missing");
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(oracle_words[i].tf, tf, "term frequency");
        }
    }

    /* No extra postings, and each term's frequency is its posting count. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&cur, &p->fts_postings, page, NULL, 0));
    for (;;) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done));
        if (done || !have_prev || klen - 8u != pklen || memcmp(k, pk, pklen) != 0) {
            if (have_prev) {
                uint32_t df = get_u32(&p->fts_terms, pk, pklen, &found);
                TEST_ASSERT_TRUE_MESSAGE(found, "a term without a dictionary entry");
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(prev_count, df, "document frequency");
            }
            if (done)
                break;
            pklen = klen - 8u;
            memcpy(pk, k, pklen);
            prev_count = 0;
            have_prev = 1;
        }
        prev_count++;
        scanned++;
    }
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(postings, scanned, "postings for words no document has");
    {
        uint64_t terms = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&cur, &p->fts_terms, page, NULL, 0));
        for (;;) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done));
            if (done)
                break;
            TEST_ASSERT_TRUE_MESSAGE(cdb_load_u32(v, CDB_ORDER_BIG) > 0, "a term with no documents");
            terms++;
        }
        (void)terms;
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&cur, &p->fts_lengths, page, NULL, 0));
    for (;;) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done));
        if (done)
            break;
        if (cdb_load_u64(k, CDB_ORDER_BIG) == 0) {
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(live, cdb_load_u64(v, CDB_ORDER_BIG), "live documents");
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(words, cdb_load_u64(v + 8, CDB_ORDER_BIG), "words in all");
        } else {
            lengths_rows++;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(live, lengths_rows);
}

/* ------------------------------------------------------------ search */

static void test_search_and_ranking(void)
{
    doc a = make_doc("Storage engine", "The pager gives copy on write storage", "casey", "storage", "core"),
        b = make_doc("Journal", "Crash safety comes from the journal", NULL, "durability", NULL),
        c = make_doc("Notes", "storage storage storage", NULL, NULL, NULL),
        d = make_doc("Owner only", "nothing here", "Storage", NULL, NULL);
    cdb_fts_hit hits[8];
    uint64_t pool, ea, eb, ec, ed;
    size_t n;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    ea = insert(pool, NULL, &a);
    eb = insert(pool, "Why the WAL exists", &b);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    create_index(pool); /* indexes the two documents already there */
    verify_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    ec = insert(pool, NULL, &c);
    ed = insert(pool, NULL, &d);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_index(pool);

    /* The document that says it most ranks first; the owner is not indexed. */
    n = search(pool, "STORAGE", CDB_FTS_ANY, hits, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(ec, hits[0].entry);
    TEST_ASSERT_EQUAL_UINT64(ea, hits[1].entry);
    TEST_ASSERT_TRUE(hits[0].score > hits[1].score);
    (void)ed;

    /* Summary words are found; rarer words weigh more. */
    n = search(pool, "wal", CDB_FTS_ANY, hits, 8);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(eb, hits[0].entry);

    /* any vs all */
    n = search(pool, "journal storage", CDB_FTS_ANY, hits, 8);
    TEST_ASSERT_EQUAL_size_t(3, n);
    n = search(pool, "pager storage", CDB_FTS_ALL, hits, 8);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(ea, hits[0].entry);
    n = search(pool, "pager journal", CDB_FTS_ALL, hits, 8);
    TEST_ASSERT_EQUAL_size_t(0, n);
    n = search(pool, "storage unheardof", CDB_FTS_ALL, hits, 8);
    TEST_ASSERT_EQUAL_size_t(0, n);
    n = search(pool, "storage unheardof", CDB_FTS_ANY, hits, 8);
    TEST_ASSERT_EQUAL_size_t(2, n);
    n = search(pool, "the", CDB_FTS_ANY, hits, 1); /* max limits the hits */
    TEST_ASSERT_EQUAL_size_t(1, n);
    n = search(pool, "core durability", CDB_FTS_ANY, hits, 8); /* tags */
    TEST_ASSERT_EQUAL_size_t(2, n);
    n = search(pool, " ,. ", CDB_FTS_ANY, hits, 8);
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_equal_scores_order_by_entry(void)
{
    doc d = make_doc("same words", NULL, NULL, NULL, NULL);
    cdb_fts_hit hits[4];
    uint64_t pool, e[3];
    int i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    create_index(pool); /* on an empty pool */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 3; i++)
        e[i] = insert(pool, NULL, &d);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_size_t(3, search(pool, "words", CDB_FTS_ANY, hits, 4));
    for (i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL_UINT64(e[i], hits[i].entry);
    TEST_ASSERT_EQUAL_size_t(2, search(pool, "words", CDB_FTS_ANY, hits, 2));
    TEST_ASSERT_EQUAL_UINT64(e[1], hits[1].entry);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Each case is set up so that dropping the factor would reverse the order
 * (equal scores fall back to entry order, which favours the wrong one). */
static void test_rare_and_short_rank_higher(void)
{
    doc common = make_doc("common filler", NULL, NULL, NULL, NULL), rare = make_doc("rare filler", NULL, NULL, NULL, NULL),
        more = make_doc("common", NULL, NULL, NULL, NULL),
        long_doc = make_doc("needle one two three four five six seven eight nine", NULL, NULL, NULL, NULL),
        short_doc = make_doc("needle one", NULL, NULL, NULL, NULL);
    cdb_fts_hit hits[8];
    uint64_t pool, ec, er, el, es;
    int i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    create_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    ec = insert(pool, NULL, &common);
    er = insert(pool, NULL, &rare);
    for (i = 0; i < 4; i++)
        (void)insert(pool, NULL, &more);
    el = insert(pool, NULL, &long_doc);
    es = insert(pool, NULL, &short_doc);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_size_t(6, search(pool, "rare common", CDB_FTS_ANY, hits, 8));
    TEST_ASSERT_EQUAL_UINT64(er, hits[0].entry); /* idf */
    TEST_ASSERT_TRUE(hits[0].score > hits[1].score);
    TEST_ASSERT_EQUAL_size_t(2, search(pool, "needle", CDB_FTS_ANY, hits, 8));
    TEST_ASSERT_EQUAL_UINT64(es, hits[0].entry); /* length normalisation */
    TEST_ASSERT_EQUAL_UINT64(el, hits[1].entry);
    TEST_ASSERT_TRUE(hits[0].score > hits[1].score);
    (void)ec;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------------ changes */

static void test_index_follows_changes(void)
{
    doc v1 = make_doc("alpha beta", "gamma", NULL, "delta", NULL), v2 = make_doc("alpha", "epsilon epsilon", NULL, NULL, NULL);
    doc p_text = make_patch(2, "zeta gamma"), p_drop = make_patch(1, NULL);
    cdb_fts_hit hits[4];
    uint64_t pool, e, f;
    int i;

    cfg.checkpoint_every = 4;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    create_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, "first summary", &v1);
    f = insert(pool, NULL, &v2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_index(pool);

    /* Patches, in one transaction with an insert-then-patch of the same document. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p_text.b, p_text.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p_drop.b, p_drop.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_index(pool);
    TEST_ASSERT_EQUAL_size_t(0, search(pool, "beta", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "zeta", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "alpha", CDB_FTS_ANY, hits, 4)); /* only f now */
    TEST_ASSERT_EQUAL_UINT64(f, hits[0].entry);

    /* A word that stays but appears more often. */
    {
        doc twice = make_patch(2, "zeta zeta gamma");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, twice.b, twice.n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
        verify_index(pool);
    }

    /* A replace with a new summary. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_replace(&db, pool, e, "second", 6, v2.b, v2.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_index(pool);
    TEST_ASSERT_EQUAL_size_t(0, search(pool, "first", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "second", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_size_t(2, search(pool, "epsilon", CDB_FTS_ANY, hits, 4));

    /* Through several checkpoints. */
    for (i = 0; i < 9; i++) {
        char w[16];
        doc p;
        snprintf(w, sizeof w, "word%d", i);
        p = make_patch(2, w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, e, p.b, p.n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
        verify_index(pool);
    }
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "word8", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_size_t(0, search(pool, "word7", CDB_FTS_ANY, hits, 4));

    /* A delete leaves the index; the deleted document cannot be changed. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, f));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_patch(&db, pool, f, p_text.b, p_text.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_index(pool);
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "alpha", CDB_FTS_ANY, hits, 4)); /* f had it too */
    TEST_ASSERT_EQUAL_UINT64(e, hits[0].entry);
    TEST_ASSERT_EQUAL_size_t(0, search(pool, "epsilon", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_persists_and_replays(void)
{
    doc v = make_doc("durable words", NULL, NULL, NULL, NULL), w = make_doc("replayed words", NULL, NULL, NULL, NULL);
    cdb_fts_hit hits[4];
    uint64_t pool;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    create_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    (void)insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "notes", 5, &pool, &found));
    TEST_ASSERT_EQUAL_size_t(1, search(pool, "durable", CDB_FTS_ANY, hits, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    (void)insert(pool, NULL, &w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0); /* never reconciled */
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "notes", 5, &pool, &found));
    TEST_ASSERT_EQUAL_size_t(2, search(pool, "words", CDB_FTS_ANY, hits, 4));
    verify_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rejects_bad_use(void)
{
    cdb_fts_config c = fts_config(), bad;
    doc v = make_doc("x", NULL, NULL, NULL, NULL);
    cdb_fts_hit hits[2];
    uint64_t pool, plain, e;
    size_t n;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    pool = make_pool();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "plain", 5, CDB_MODEL_DOCUMENT, &plain));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_doc_search(&db, plain, "x", 1, CDB_FTS_ANY, hits, 2, &n));
    memset(&bad, 0, sizeof bad);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_create_full_text(&db, pool, &bad)); /* nothing to index */
    bad = c;
    bad.paths[1].depth = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_create_full_text(&db, pool, &bad));
    cfg.fts_tokens_cap = 2u * PAGE - 1u; /* too small to hold every word of a page */
    db.cfg.fts_tokens_cap = cfg.fts_tokens_cap;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_doc_create_full_text(&db, pool, &c));
    cfg.fts_tokens_cap = db.cfg.fts_tokens_cap = 2u * PAGE;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_doc_create_full_text(&db, pool, &c));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    create_index(pool);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_doc_create_full_text(&db, pool, &c));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_doc_search(&db, pool, "a b c d e f g h i j k l m n o p q", 33, CDB_FTS_ANY, hits, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_search(&db, pool, "a", 1, 7, hits, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    e = insert(pool, NULL, &v);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Without its buffers the pool cannot be used: its index would fall behind. */
    cfg.fts_tokens = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "notes", 5, &pool, &found));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_doc_get(&db, pool, e, 0, out, sizeof out, &n, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: A, B inserted (no index). S2: index created. S3: A patched.
 * S4: B replaced. S5: A deleted. */
static cdb_status workload(int *acked)
{
    doc a = make_doc("alpha", "first text", NULL, "tag", NULL), b = make_doc("beta", "second text", NULL, NULL, NULL),
        b2 = make_doc("beta", "rewritten", NULL, NULL, NULL), p = make_patch(2, "patched text");
    cdb_fts_config c = fts_config();
    uint64_t pool, ea, eb;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_db_create_store(&db, "notes", 5, CDB_MODEL_DOCUMENT, &pool));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_insert(&db, pool, "sum", 3, a.b, a.n, &ea));
    TRY(cdb_doc_insert(&db, pool, NULL, 0, b.b, b.n, &eb));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_doc_create_full_text(&db, pool, &c));
    *acked = 2;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_patch(&db, pool, ea, p.b, p.n));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_replace(&db, pool, eb, NULL, 0, b2.b, b2.n));
    TRY(cdb_tx_commit(&db));
    *acked = 4;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_doc_delete(&db, pool, ea));
    TRY(cdb_tx_commit(&db));
    *acked = 5;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

/* The state recovery shows, from the documents and whether an index exists. */
static int observed(uint64_t *pool_out)
{
    cdb_fts_hit hits[4];
    uint64_t pool;
    size_t n, len;
    int found, a_live, a_patched = 0, b_rewritten = 0, has_index;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "notes", 5, &pool, &found));
    *pool_out = found ? pool : 0;
    if (!found)
        return 0;
    if (cdb_doc_get(&db, pool, 1, 0, out, sizeof out, &len, NULL, 0, NULL, NULL) == CDB_E_NOT_FOUND
        && cdb_doc_get(&db, pool, 2, 0, out, sizeof out, &len, NULL, 0, NULL, NULL) == CDB_E_NOT_FOUND)
        return 0;
    has_index = cdb_doc_search(&db, pool, "text", 4, CDB_FTS_ANY, hits, 4, &n) == CDB_OK;
    a_live = cdb_doc_get(&db, pool, 1, 0, out, sizeof out, &len, NULL, 0, NULL, NULL) == CDB_OK;
    if (a_live) {
        doc a2 = make_doc("alpha", "patched text", NULL, "tag", NULL);
        a_patched = len == a2.n && memcmp(out, a2.b, len) == 0;
    }
    {
        doc b2 = make_doc("beta", "rewritten", NULL, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, pool, 2, 0, out, sizeof out, &len, NULL, 0, NULL, NULL));
        b_rewritten = len == b2.n && memcmp(out, b2.b, len) == 0;
    }
    if (!has_index)
        return (a_live && !a_patched && !b_rewritten) ? 1 : -1;
    verify_index(pool);
    if (!a_live)
        return b_rewritten ? 5 : -1;
    if (b_rewritten)
        return a_patched ? 4 : -1;
    return a_patched ? 3 : 2;
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
                uint64_t pool;
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultenv_reboot(&fe);
                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(), "recovery open failed");
                k = observed(&pool);
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                if (k >= 2) {
                    /* The recovered index keeps following changes. */
                    doc v = make_doc("after", "more text", NULL, NULL, NULL);
                    uint64_t e;
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, v.b, v.n, &e));
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
                    verify_index(pool);
                }
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_split_words);
    RUN_TEST(test_word_compare_folds_ascii);
    RUN_TEST(test_search_and_ranking);
    RUN_TEST(test_equal_scores_order_by_entry);
    RUN_TEST(test_rare_and_short_rank_higher);
    RUN_TEST(test_index_follows_changes);
    RUN_TEST(test_persists_and_replays);
    RUN_TEST(test_rejects_bad_use);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
