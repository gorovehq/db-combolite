#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_faultenv.h"
#include "cdb_page.h"
#include "cdb_vec.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define DIMS 16u
#define MAXN 64u

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
    memset(h.db_id, 0x3C, sizeof h.db_id);
    return cdb_db_open(&db, &env, "v", &cfg, &h);
}

/* ------------------------------------------------------------ vectors
 *
 * Test vectors hold small multiples of 1/4, exact in every float type, so
 * the reference below computes with the same values the database stores. */

typedef struct vec {
    double x[DIMS];      /* the values (bits, for binary) */
    uint8_t enc[128];    /* encoded value */
    size_t n;
} vec;

static uint32_t rng = 12345u;

static int next_int(int lo, int hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 16) % (uint32_t)(hi - lo + 1));
}

static uint16_t f32_to_half(float f) /* exact for the values used here */
{
    uint32_t b;
    unsigned sign, e;
    memcpy(&b, &f, sizeof b);
    sign = (b >> 31) & 1u;
    if ((b & 0x7FFFFFFFu) == 0)
        return (uint16_t)(sign << 15);
    e = ((b >> 23) & 0xFFu) - 127u + 15u;
    return (uint16_t)((sign << 15) | (e << 10) | ((b >> 13) & 0x3FFu));
}

static vec make_vec(cdb_vector_elem elem, int zero)
{
    vec v;
    cdb_value_writer w;
    float f[DIMS];
    uint16_t h[DIMS];
    int8_t i8[DIMS];
    uint8_t bits[DIMS / 8u];
    unsigned i;
    memset(bits, 0, sizeof bits);
    for (i = 0; i < DIMS; i++) {
        int k = zero ? 0 : next_int(-12, 12);
        if (elem == CDB_VEC_BINARY) {
            v.x[i] = (double)(k > 0);
            if (k > 0)
                bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
        } else if (elem == CDB_VEC_I8) {
            v.x[i] = k;
            i8[i] = (int8_t)k;
        } else {
            f[i] = (float)k / 4.0f;
            v.x[i] = (double)k / 4.0;
            h[i] = elem == CDB_VEC_F16 ? f32_to_half(f[i]) : (uint16_t)(0);
            if (elem == CDB_VEC_BF16) {
                uint32_t b;
                memcpy(&b, &f[i], sizeof b);
                h[i] = (uint16_t)(b >> 16);
            }
        }
    }
    cdb_value_writer_init(&w, v.enc, sizeof v.enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    switch (elem) {
    case CDB_VEC_F32: cdb_value_write_vector(&w, elem, DIMS, f); break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16: cdb_value_write_vector(&w, elem, DIMS, h); break;
    case CDB_VEC_I8: cdb_value_write_vector(&w, elem, DIMS, i8); break;
    default: cdb_value_write_vector(&w, elem, DIMS, bits); break;
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    v.n = cdb_value_writer_len(&w);
    return v;
}

static double ref_distance(unsigned measure, const vec *a, const vec *b)
{
    double dot = 0, na = 0, nb = 0, l2 = 0;
    unsigned i;
    for (i = 0; i < DIMS; i++) {
        dot += a->x[i] * b->x[i];
        na += a->x[i] * a->x[i];
        nb += b->x[i] * b->x[i];
        l2 += (a->x[i] - b->x[i]) * (a->x[i] - b->x[i]);
    }
    switch (measure) {
    case CDB_VEC_COSINE: return (na == 0 || nb == 0) ? 1.0 : 1.0 - dot / (sqrt(na) * sqrt(nb));
    case CDB_VEC_DOT: return -dot;
    case CDB_VEC_EUCLIDEAN: return sqrt(l2);
    default: return l2; /* Hamming: bits are 0 or 1 */
    }
}

static uint64_t make_coll(const char *name, cdb_vector_elem elem, unsigned measure)
{
    cdb_vec_config c;
    uint64_t id;
    memset(&c, 0, sizeof c);
    c.elem = elem;
    c.dims = DIMS;
    c.measure = measure;
    c.model = "test-embedder";
    c.model_len = 13;
    c.model_version = "1.0";
    c.model_version_len = 3;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, name, strlen(name), &c, &id));
    return id;
}

static uint64_t insert(uint64_t coll, const vec *v, uint64_t ss, uint64_t se)
{
    uint64_t e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v->enc, v->n, ss, se, &e));
    return e;
}

/* ------------------------------------------------------------ oracle
 *
 * Blocks and indexes agree: every live entry's row points at a slot that
 * holds it with its vector, every occupied slot has a row, and the block
 * counters match their slots. */

static struct cdb_vec_coll *coll_of(uint64_t store)
{
    struct cdb_vec_coll *c = (struct cdb_vec_coll *)cdb_slots_find(&db.vec_slots, store);
    if (c != NULL)
        return c;
    TEST_FAIL_MESSAGE("collection not open");
    return NULL;
}

static uint64_t verify_coll(uint64_t store, const vec *want, const int *live, uint64_t n)
{
    static uint8_t page[PAGE], pg[PAGE];
    struct cdb_vec_coll *c = coll_of(store);
    cdb_index_cursor cur;
    uint8_t k[8], v[16], out[128];
    size_t klen, vlen, len;
    uint64_t occupied = 0, live_n = 0, blocks = 0, e;
    int done;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&cur, &c->blocks, page, NULL, 0));
    for (;;) {
        cdb_page_header h;
        const uint8_t *p;
        uint32_t count, used, i, nz = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done));
        if (done)
            break;
        blocks++;
        used = cdb_load_u32(v, CDB_ORDER_BIG);
        count = cdb_load_u32(v + 4, CDB_ORDER_BIG);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, cdb_load_u64(k, CDB_ORDER_BIG), pg, &h));
        TEST_ASSERT_EQUAL_HEX16(CDB_PAGE_TYPE_VECTOR, h.type);
        p = pg + cdb_page_header_size(db.pager.fmt);
        TEST_ASSERT_EQUAL_UINT32(count, cdb_load_u32(p + 8, CDB_ORDER_LITTLE));
        TEST_ASSERT_TRUE_MESSAGE(count > 0 && count <= c->slots, "block count");
        for (i = 0; i < count; i++)
            if (cdb_load_u64(p + 16 + i * 8u, CDB_ORDER_LITTLE))
                nz++;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(nz, used, "slots in use");
        TEST_ASSERT_TRUE_MESSAGE(cdb_load_u64(p + 16 + (count - 1u) * 8u, CDB_ORDER_LITTLE) != 0, "trailing empty slot");
        occupied += nz;
    }
    for (e = 1; e <= n; e++) {
        cdb_status s = cdb_vec_get(&db, store, e, out, sizeof out, &len, NULL, NULL);
        if (!live[e]) {
            TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, s);
            continue;
        }
        TEST_ASSERT_EQUAL_INT(CDB_OK, s);
        TEST_ASSERT_EQUAL_size_t(want[e].n, len);
        TEST_ASSERT_EQUAL_MEMORY(want[e].enc, out, len);
        live_n++;
    }
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(live_n, occupied, "occupied slots vs live entries");
    return blocks;
}

/* Exact search agrees with the reference, position by position. */
static void check_search(uint64_t store, unsigned measure, const vec *q, const vec *all, const int *live, uint64_t n,
                         size_t k)
{
    cdb_vec_hit hits[MAXN];
    double ref[MAXN];
    size_t got, want = 0, i, j;
    uint64_t e;
    for (e = 1; e <= n; e++)
        if (live[e])
            ref[want++] = ref_distance(measure, q, &all[e]);
    for (i = 1; i < want; i++) /* sort the reference distances */
        for (j = i; j > 0 && ref[j - 1] > ref[j]; j--) {
            double t = ref[j];
            ref[j] = ref[j - 1];
            ref[j - 1] = t;
        }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, store, q->enc, q->n, measure, hits, k, &got));
    TEST_ASSERT_EQUAL_size_t(want < k ? want : k, got);
    for (i = 0; i < got; i++) {
        TEST_ASSERT_TRUE(live[hits[i].entry]);
        TEST_ASSERT_DOUBLE_WITHIN(1e-5, ref[i], hits[i].distance);
        TEST_ASSERT_DOUBLE_WITHIN(1e-5, ref_distance(measure, q, &all[hits[i].entry]), hits[i].distance);
        if (i > 0) {
            TEST_ASSERT_TRUE(hits[i - 1].distance <= hits[i].distance);
            if (hits[i - 1].distance == hits[i].distance)
                TEST_ASSERT_TRUE(hits[i - 1].entry < hits[i].entry);
        }
    }
}

/* ------------------------------------------------------------ tests */

static void test_create_and_describe(void)
{
    cdb_vec_config c, got;
    char model[CDB_VEC_MODEL_MAX], version[CDB_VEC_MODEL_MAX];
    uint64_t id, id2;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    id = make_coll("memory", CDB_VEC_F32, CDB_VEC_COSINE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_describe(&db, id, &got, model, version));
    TEST_ASSERT_EQUAL_INT(CDB_VEC_F32, got.elem);
    TEST_ASSERT_EQUAL_UINT32(DIMS, got.dims);
    TEST_ASSERT_EQUAL_UINT(CDB_VEC_COSINE, got.measure);
    TEST_ASSERT_EQUAL_size_t(13, got.model_len);
    TEST_ASSERT_EQUAL_MEMORY("test-embedder", model, 13);
    TEST_ASSERT_EQUAL_MEMORY("1.0", version, 3);

    memset(&c, 0, sizeof c);
    c.elem = CDB_VEC_F32;
    c.dims = DIMS;
    c.measure = CDB_VEC_HAMMING; /* Hamming needs binary vectors */
    c.model = "m";
    c.model_len = 1;
    c.model_version = "1";
    c.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_create(&db, "x", 1, &c, &id2));
    c.elem = CDB_VEC_BINARY;
    c.measure = CDB_VEC_COSINE;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_create(&db, "x", 1, &c, &id2));
    c.elem = CDB_VEC_F32;
    c.measure = CDB_VEC_DOT;
    c.dims = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_create(&db, "x", 1, &c, &id2));
    c.dims = DIMS;
    c.model_len = 0; /* provenance is required (spec 7.3) */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_create(&db, "x", 1, &c, &id2));
    c.model_len = 1;
    c.dims = 300; /* 1200 bytes: more than a 1 KB page */
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_vec_create(&db, "x", 1, &c, &id2));
    c.dims = DIMS;
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_vec_create(&db, "memory", 6, &c, &id2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_vec_create(&db, "x", 1, &c, &id2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "x", 1, &c, &id2));
    TEST_ASSERT_TRUE(id2 > id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "memory", 6, &id, &found));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_describe(&db, id, &got, model, version));
    TEST_ASSERT_EQUAL_MEMORY("test-embedder", model, 13);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void search_case(cdb_vector_elem elem, unsigned base, const unsigned *measures, int nm)
{
    static vec all[MAXN + 1];
    static int live[MAXN + 1];
    uint64_t coll, e, n = 40;
    int m, t;

    rng = 777u + (uint32_t)elem * 31u + base;
    setUp();
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c", elem, base);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (e = 1; e <= n; e++) {
        all[e] = make_vec(elem, e == 7); /* entry 7: the zero vector */
        live[e] = 1;
        TEST_ASSERT_EQUAL_UINT64(e, insert(coll, &all[e], 0, 0));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    {
        uint64_t blocks = verify_coll(coll, all, live, n);
        /* 40 vectors of 16 floats fill several 1 KB blocks; small ones fit in one. */
        TEST_ASSERT_TRUE_MESSAGE(elem == CDB_VEC_I8 || elem == CDB_VEC_BINARY ? blocks >= 1 : blocks > 1,
                                 "spans several blocks");
    }
    for (t = 0; t < 4; t++) {
        vec q = make_vec(elem, 0);
        for (m = 0; m < nm; m++) {
            check_search(coll, measures[m], &q, all, live, n, 10);
            check_search(coll, measures[m], &q, all, live, n, MAXN);
        }
        if (t == 0) {
            /* CDB_VEC_BASE means the base measure. */
            cdb_vec_hit a[5], b[5];
            size_t na, nb, i;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q.enc, q.n, CDB_VEC_BASE, a, 5, &na));
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q.enc, q.n, base, b, 5, &nb));
            TEST_ASSERT_EQUAL_size_t(na, nb);
            for (i = 0; i < na; i++)
                TEST_ASSERT_EQUAL_UINT64(b[i].entry, a[i].entry);
        }
    }
    {
        vec self = all[3];
        cdb_vec_hit h[1];
        size_t got;
        if (base != CDB_VEC_DOT) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, self.enc, self.n, base, h, 1, &got));
            TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, h[0].distance); /* a vector is nearest to itself */
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_search_every_type_and_measure(void)
{
    const unsigned floats[3] = { CDB_VEC_COSINE, CDB_VEC_DOT, CDB_VEC_EUCLIDEAN };
    const unsigned hamming[1] = { CDB_VEC_HAMMING };
    search_case(CDB_VEC_F32, CDB_VEC_COSINE, floats, 3);
    search_case(CDB_VEC_F32, CDB_VEC_EUCLIDEAN, floats, 3);
    search_case(CDB_VEC_F16, CDB_VEC_COSINE, floats, 3);
    search_case(CDB_VEC_BF16, CDB_VEC_DOT, floats, 3);
    search_case(CDB_VEC_I8, CDB_VEC_COSINE, floats, 3);
    search_case(CDB_VEC_BINARY, CDB_VEC_HAMMING, hamming, 1);
}

static void test_changes_reuse_slots(void)
{
    static vec all[MAXN + 1];
    static int live[MAXN + 1];
    uint64_t coll, e, n = 0, blocks_full, blocks;
    int i;

    rng = 99u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c", CDB_VEC_F32, CDB_VEC_COSINE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 30; i++) {
        all[++n] = make_vec(CDB_VEC_F32, 0);
        live[n] = 1;
        insert(coll, &all[n], 0, 0);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    blocks_full = verify_coll(coll, all, live, n);

    /* Replace some, delete every other one. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (e = 1; e <= n; e++) {
        if (e % 2 == 0) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, e));
            live[e] = 0;
        } else if (e % 3 == 0) {
            all[e] = make_vec(CDB_VEC_F32, 0);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, e, all[e].enc, all[e].n));
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_coll(coll, all, live, n);
    {
        vec q = make_vec(CDB_VEC_F32, 0);
        check_search(coll, CDB_VEC_COSINE, &q, all, live, n, 8);
    }

    /* New vectors fill the freed slots: no new blocks, new IDs. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 15; i++) {
        all[++n] = make_vec(CDB_VEC_F32, 0);
        live[n] = 1;
        TEST_ASSERT_EQUAL_UINT64(n, insert(coll, &all[n], 0, 0));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    blocks = verify_coll(coll, all, live, n);
    TEST_ASSERT_EQUAL_UINT64(blocks_full, blocks);

    /* Emptying the collection releases every block. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (e = 1; e <= n; e++)
        if (live[e]) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, e));
            live[e] = 0;
        }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_UINT64(0, verify_coll(coll, all, live, n));
    {
        vec q = make_vec(CDB_VEC_F32, 0);
        cdb_vec_hit h[2];
        size_t got;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q.enc, q.n, CDB_VEC_BASE, h, 2, &got));
        TEST_ASSERT_EQUAL_size_t(0, got);
    }

    /* Refilled after every block was released: each new block is filled
     * before the next is started. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; n < MAXN; i++) {
        all[++n] = make_vec(CDB_VEC_F32, 0);
        live[n] = 1;
        insert(coll, &all[n], 0, 0);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    blocks = verify_coll(coll, all, live, n);
    TEST_ASSERT_EQUAL_UINT64(((uint64_t)i + coll_of(coll)->slots - 1u) / coll_of(coll)->slots, blocks);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_sources_with_documents(void)
{
    vec a, b, c;
    uint64_t coll, pool, doc_entry, ea, eb, ec, found_e[4], ss, se;
    uint8_t body[32], out[128];
    size_t n, len;
    cdb_value_writer w;

    rng = 4242u;
    a = make_vec(CDB_VEC_F32, 0);
    b = make_vec(CDB_VEC_F32, 0);
    c = make_vec(CDB_VEC_F32, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "docs", 4, CDB_MODEL_DOCUMENT, &pool));
    coll = make_coll("chunks", CDB_VEC_F32, CDB_VEC_COSINE);
    cdb_value_writer_init(&w, body, sizeof body, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, "doc", 3);
    cdb_value_end_object(&w);

    /* A document and the embeddings of its chunks, in one transaction. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, body, cdb_value_writer_len(&w), &doc_entry));
    ea = insert(coll, &a, pool, doc_entry);
    eb = insert(coll, &b, pool, doc_entry);
    ec = insert(coll, &c, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_by_source(&db, coll, pool, doc_entry, found_e, 4, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT64(ea, found_e[0]);
    TEST_ASSERT_EQUAL_UINT64(eb, found_e[1]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_by_source(&db, coll, pool, doc_entry + 1u, found_e, 4, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_get(&db, coll, ec, out, sizeof out, &len, &ss, &se));
    TEST_ASSERT_EQUAL_UINT64(0, ss);

    /* A replace keeps the source; a delete removes it. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, ea, c.enc, c.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, eb));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_get(&db, coll, ea, out, sizeof out, &len, &ss, &se));
    TEST_ASSERT_EQUAL_MEMORY(c.enc, out, c.n);
    TEST_ASSERT_EQUAL_UINT64(pool, ss);
    TEST_ASSERT_EQUAL_UINT64(doc_entry, se);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_by_source(&db, coll, pool, doc_entry, found_e, 4, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(ea, found_e[0]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_by_source(&db, coll, pool, doc_entry, found_e, 0, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static size_t tiny_doc(uint8_t *b, size_t cap, const char *text)
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

/* The state each vector's source is in, as search reports it. */
static void expect_states(uint64_t coll, const vec *q, const unsigned *want, uint64_t n)
{
    cdb_vec_hit hits[16];
    size_t got, i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q->enc, q->n, CDB_VEC_BASE, hits, 16, &got));
    TEST_ASSERT_EQUAL_size_t(n, got);
    for (i = 0; i < got; i++) {
        char msg[48];
        snprintf(msg, sizeof msg, "source state of entry %u", (unsigned)hits[i].entry);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(want[hits[i].entry], hits[i].source_state, msg);
    }
}

static void test_source_states(void)
{
    vec v[6], q;
    unsigned want[6];
    uint8_t body[32], patch[48];
    uint64_t pool, coll, d[4], e;
    size_t bl;
    int i, found;

    rng = 61u;
    for (i = 1; i <= 5; i++)
        v[i] = make_vec(CDB_VEC_F32, 0);
    q = make_vec(CDB_VEC_F32, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "docs", 4, CDB_MODEL_DOCUMENT, &pool));
    coll = make_coll("mem", CDB_VEC_F32, CDB_VEC_COSINE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 1; i <= 3; i++) {
        bl = tiny_doc(body, sizeof body, "text");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, pool, NULL, 0, body, bl, &d[i]));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, insert(coll, &v[i], pool, d[i])); /* same transaction */
    }
    TEST_ASSERT_EQUAL_UINT64(4, insert(coll, &v[4], 0, 0));
    TEST_ASSERT_EQUAL_UINT64(5, insert(coll, &v[5], coll, 1)); /* a source that is not a document */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    want[1] = want[2] = want[3] = CDB_VEC_SOURCE_CURRENT;
    want[4] = CDB_VEC_SOURCE_NONE;
    want[5] = CDB_VEC_SOURCE_UNCHECKED;
    expect_states(coll, &q, want, 5);

    /* Edit one document and delete another. */
    {
        cdb_value_writer w;
        cdb_value_writer_init(&w, patch, sizeof patch, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        cdb_value_begin_array(&w);
        cdb_value_begin_array(&w);
        cdb_value_write_int(&w, CDB_PATCH_SET);
        cdb_value_begin_array(&w);
        cdb_value_write_int(&w, 1);
        cdb_value_end_array(&w);
        cdb_value_write_string(&w, "corrected", 9);
        cdb_value_end_array(&w);
        cdb_value_end_array(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_patch(&db, pool, d[1], patch, cdb_value_writer_len(&w)));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_delete(&db, pool, d[2]));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }
    want[1] = CDB_VEC_SOURCE_OUTDATED;
    want[2] = CDB_VEC_SOURCE_DELETED;
    expect_states(coll, &q, want, 5);

    /* Re-embedding brings the vector up to date. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, 1, v[1].enc, v[1].n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    want[1] = CDB_VEC_SOURCE_CURRENT;
    expect_states(coll, &q, want, 5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* The recorded revisions persist; a dropped pool counts as deleted. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "mem", 3, &coll, &found));
    expect_states(coll, &q, want, 5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, pool));
    want[1] = want[3] = CDB_VEC_SOURCE_DELETED;
    expect_states(coll, &q, want, 5);
    (void)e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_transaction_rules(void)
{
    vec v, other;
    uint64_t coll, ham, e, e2;
    uint8_t bad[8] = { CDB_TAG_INT, 2 };
    cdb_vec_hit h[2];
    size_t n;

    need_open(2);
    rng = 5u;
    v = make_vec(CDB_VEC_F32, 0);
    other = make_vec(CDB_VEC_I8, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c", CDB_VEC_F32, CDB_VEC_COSINE);
    ham = make_coll("h", CDB_VEC_BINARY, CDB_VEC_HAMMING);
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 0, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_insert(&db, coll, other.enc, other.n, 0, 0, &e)); /* type */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_insert(&db, coll, bad, 2, 0, 0, &e));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 5, &e)); /* entry without store */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_insert(&db, ham, v.enc, v.n, 0, 0, &e));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_replace(&db, coll, 1, v.enc, v.n));
    e = insert(coll, &v, 0, 0);
    /* Changes in the same transaction are seen by later calls. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, e, v.enc, v.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, e));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_replace(&db, coll, e, v.enc, v.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_delete(&db, coll, e));
    e2 = insert(coll, &v, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(e2 > e);
    {
        uint8_t out[128];
        size_t len;
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_get(&db, coll, e, out, sizeof out, &len, NULL, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_get(&db, coll, e2, out, sizeof out, &len, NULL, NULL));
        TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_vec_get(&db, coll, e2, out, 10, &len, NULL, NULL));
    }
    /* Queries must match the collection; Hamming only for binary. */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_search(&db, coll, other.enc, other.n, 0, h, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_search(&db, coll, v.enc, v.n, CDB_VEC_HAMMING, h, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_search(&db, coll, v.enc, v.n, 9, h, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, v.enc, v.n, 0, h, 2, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(e2, h[0].entry);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Without its buffers a collection cannot be used. */
    cfg.vec_buffers = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_vec_search(&db, coll, v.enc, v.n, 0, h, 2, &n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_persists_and_replays(void)
{
    static vec all[8];
    static int live[8];
    uint64_t coll;
    int found;
    rng = 8u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c", CDB_VEC_I8, CDB_VEC_EUCLIDEAN);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    all[1] = make_vec(CDB_VEC_I8, 0);
    live[1] = 1;
    insert(coll, &all[1], 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "c", 1, &coll, &found));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    all[2] = make_vec(CDB_VEC_I8, 0);
    live[2] = 1;
    TEST_ASSERT_EQUAL_UINT64(2, insert(coll, &all[2], 0, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, 1));
    live[1] = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0); /* never reconciled */
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "c", 1, &coll, &found));
    verify_coll(coll, all, live, 2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    all[3] = make_vec(CDB_VEC_I8, 0);
    live[3] = 1;
    TEST_ASSERT_EQUAL_UINT64(3, insert(coll, &all[3], 0, 0)); /* IDs continue after replay */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_coll(coll, all, live, 3);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

/* S0: nothing. S1: collection, 20 vectors. S2: 5 replaced. S3: 6 deleted,
 * 4 more inserted. */
static vec sweep_v[32];

static void sweep_vectors(void)
{
    int i;
    rng = 31337u;
    for (i = 1; i < 32; i++)
        sweep_v[i] = make_vec(CDB_VEC_F32, 0);
}

static cdb_status workload(int *acked)
{
    cdb_vec_config c;
    uint64_t coll, e;
    cdb_status s;
    int i;
    *acked = 0;
    memset(&c, 0, sizeof c);
    c.elem = CDB_VEC_F32;
    c.dims = DIMS;
    c.measure = CDB_VEC_COSINE;
    c.model = "m";
    c.model_len = 1;
    c.model_version = "1";
    c.model_version_len = 1;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(cdb_vec_create(&db, "c", 1, &c, &coll));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 1; i <= 20; i++)
        TRY(cdb_vec_insert(&db, coll, sweep_v[i].enc, sweep_v[i].n, i % 4 ? 0 : 99, i % 4 ? 0 : (uint64_t)i, &e));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_db_reconcile(&db));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_vec_replace(&db, coll, 5, sweep_v[25].enc, sweep_v[25].n));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_vec_delete(&db, coll, 6));
    for (i = 21; i <= 24; i++)
        TRY(cdb_vec_insert(&db, coll, sweep_v[i].enc, sweep_v[i].n, 0, 0, &e));
    TRY(cdb_tx_commit(&db));
    *acked = 3;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static int observed(uint64_t *coll_out)
{
    static vec want[32];
    static int live[32];
    uint8_t out[128];
    size_t len;
    uint64_t coll;
    int found, k, i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "c", 1, &coll, &found));
    *coll_out = found ? coll : 0;
    if (!found)
        return 0;
    if (cdb_vec_get(&db, coll, 1, out, sizeof out, &len, NULL, NULL) == CDB_E_NOT_FOUND)
        k = 0;
    else if (cdb_vec_get(&db, coll, 21, out, sizeof out, &len, NULL, NULL) == CDB_OK)
        k = 3;
    else {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_get(&db, coll, 5, out, sizeof out, &len, NULL, NULL));
        k = memcmp(out, sweep_v[25].enc, len) == 0 ? 2 : 1;
    }
    memset(live, 0, sizeof live);
    for (i = 1; i <= 24; i++) {
        want[i] = sweep_v[i];
        live[i] = k >= 1 && (i <= 20 || k >= 3) && !(i == 6 && k >= 3);
    }
    if (k >= 2)
        want[5] = sweep_v[25];
    verify_coll(coll, want, live, 24);
    if (k >= 1) {
        uint64_t es[2];
        size_t n;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_by_source(&db, coll, 99, 8, es, 2, &n));
        TEST_ASSERT_EQUAL_size_t(1, n);
        TEST_ASSERT_EQUAL_UINT64(8, es[0]);
        check_search(coll, CDB_VEC_COSINE, &sweep_v[30], want, live, 24, 5);
    }
    return k;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[2] = { 0, 300 };
    uint64_t total, op;
    int acked, m;

    sweep_vectors();
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
                uint64_t coll;
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultenv_reboot(&fe);
                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(), "recovery open failed");
                k = observed(&coll);
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                if (coll) {
                    uint64_t e;
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, sweep_v[31].enc, sweep_v[31].n, 0, 0, &e));
                    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
                    TEST_ASSERT_TRUE_MESSAGE(k == 0 || e > (k >= 3 ? 24u : 20u), "an entry ID was reused");
                }
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_describe);
    RUN_TEST(test_search_every_type_and_measure);
    RUN_TEST(test_changes_reuse_slots);
    RUN_TEST(test_sources_with_documents);
    RUN_TEST(test_source_states);
    RUN_TEST(test_transaction_rules);
    RUN_TEST(test_persists_and_replays);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
