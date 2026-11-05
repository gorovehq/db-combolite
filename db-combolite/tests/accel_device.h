/* The tests every real accelerator device passes (spec 18.11), shared by
 * test_accel_cuda.c and test_accel_vulkan.c: its distances stay within
 * the bound it reports, its shortlists leave out nothing that could be
 * nearer, and the engine's searches through it give exactly the CPU
 * path's results. The includer installs a device in `env` (and in
 * dev_ops, dev_ctx) and calls run_device_tests between UNITY_BEGIN and
 * UNITY_END. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_vec.h"

static cdb_env env;
static const cdb_accel_ops *dev_ops; /* the device under test, for each fresh environment */
static void *dev_ctx;

static uint32_t rng;

static uint32_t next_u32(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

/* Roughly normal: the sum of four uniforms, centred. */
static float next_gauss(void)
{
    float s = 0.0f;
    int i;
    for (i = 0; i < 4; i++)
        s += (float)(next_u32() >> 8) / 16777216.0f;
    return s - 2.0f;
}

/* ------------------------------------------------------------ the contract
 *
 * The mirror's calls directly, on stored vectors (little-endian), against
 * the CPU path's distance (cdb_vec_cpu_distance). A host copy of the live
 * vectors is the reference. */

#define LIVE_MAX 4096u
#define VEC_MAX (384u * 4u)
#define WANT_MAX 4096u
#define NQ_MAX 8u

static uint64_t live_ids[LIVE_MAX];
static uint8_t live_vecs[LIVE_MAX][VEC_MAX];
static size_t live_n;

static uint64_t got_ids[NQ_MAX * WANT_MAX];
static float got_dists[NQ_MAX * WANT_MAX];
static uint8_t in_list[LIVE_MAX];

static size_t stored_bytes(unsigned elem, uint32_t dims)
{
    switch (elem) {
    case CDB_VEC_F32: return (size_t)dims * 4u;
    case CDB_VEC_F16:
    case CDB_VEC_BF16: return (size_t)dims * 2u;
    case CDB_VEC_I8: return dims;
    default: return ((size_t)dims + 7u) / 8u;
    }
}

/* A float as a half, mantissa truncated (any finite half will do). */
static uint16_t to_half(float f)
{
    uint32_t b;
    unsigned sign, e;
    int ex;
    memcpy(&b, &f, sizeof b);
    sign = (b >> 31) & 1u;
    ex = (int)((b >> 23) & 0xFFu) - 127 + 15;
    if (ex <= 0)
        return (uint16_t)(sign << 15);
    e = (unsigned)ex;
    return (uint16_t)((sign << 15) | (e << 10) | ((b >> 13) & 0x3FFu));
}

static void random_vec(unsigned elem, uint32_t dims, uint8_t *out)
{
    uint32_t i;
    memset(out, 0, stored_bytes(elem, dims));
    for (i = 0; i < dims; i++) {
        float f = next_gauss();
        uint32_t b;
        uint16_t h;
        memcpy(&b, &f, sizeof b);
        switch (elem) {
        case CDB_VEC_F32:
            out[i * 4u] = (uint8_t)b;
            out[i * 4u + 1u] = (uint8_t)(b >> 8);
            out[i * 4u + 2u] = (uint8_t)(b >> 16);
            out[i * 4u + 3u] = (uint8_t)(b >> 24);
            break;
        case CDB_VEC_F16:
        case CDB_VEC_BF16:
            h = elem == CDB_VEC_F16 ? to_half(f) : (uint16_t)(b >> 16);
            out[i * 2u] = (uint8_t)h;
            out[i * 2u + 1u] = (uint8_t)(h >> 8);
            break;
        case CDB_VEC_I8: {
            float x = f * 60.0f;
            out[i] = (uint8_t)(int8_t)(x > 127.0f ? 127 : x < -128.0f ? -128 : (int)x);
            break;
        }
        default:
            if (f > 0.0f)
                out[i / 8u] |= (uint8_t)(1u << (i % 8u));
            break;
        }
    }
    if (elem == CDB_VEC_BINARY && dims % 8u)
        out[(dims - 1u) / 8u] |= (uint8_t)(0xFFu << (dims % 8u)); /* unused bits set: the device must ignore them */
}

static size_t find_live(uint64_t id)
{
    size_t i;
    for (i = 0; i < live_n; i++)
        if (live_ids[i] == id)
            return i;
    return live_n;
}

static void put(cdb_accel_mirror *m, unsigned elem, uint32_t dims, const uint64_t *ids, size_t n)
{
    static uint8_t vecs[256u * VEC_MAX];
    size_t vb = stored_bytes(elem, dims), i;
    TEST_ASSERT_TRUE(n <= 256u);
    for (i = 0; i < n; i++) {
        size_t at = find_live(ids[i]);
        random_vec(elem, dims, vecs + i * vb);
        if (at == live_n) {
            TEST_ASSERT_TRUE(live_n < LIVE_MAX);
            live_ids[live_n++] = ids[i];
        }
        memcpy(live_vecs[at], vecs + i * vb, vb);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.accel->mirror_put(env.accel_ctx, m, ids, vecs, vb, n));
}

static void del(cdb_accel_mirror *m, const uint64_t *ids, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        size_t at = find_live(ids[i]);
        if (at < live_n) {
            live_n--;
            live_ids[at] = live_ids[live_n];
            memcpy(live_vecs[at], live_vecs[live_n], VEC_MAX);
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.accel->mirror_del(env.accel_ctx, m, ids, n));
}

static double worst; /* the largest error seen, as a fraction of its bound */

/* nq queries, `want` candidates each, checked against the CPU. */
static void check_nearest(cdb_accel_mirror *m, unsigned elem, uint32_t dims, unsigned measure, size_t nq, size_t want)
{
    static uint8_t queries[NQ_MAX * VEC_MAX];
    size_t counts[NQ_MAX], vb = stored_bytes(elem, dims), qi, i, r;
    double bounds[NQ_MAX];
    TEST_ASSERT_TRUE(nq <= NQ_MAX && want <= WANT_MAX);
    for (qi = 0; qi < nq; qi++)
        random_vec(elem, dims, queries + qi * vb);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.accel->nearest(env.accel_ctx, m, queries, nq, want, got_ids, got_dists, counts,
                                                     bounds));
    for (qi = 0; qi < nq; qi++) {
        const uint8_t *q = queries + qi * vb;
        const uint64_t *ids = got_ids + qi * want;
        const float *dists = got_dists + qi * want;
        size_t w = want < live_n ? want : live_n;
        double e = bounds[qi], g;
        TEST_ASSERT_EQUAL_size_t(w, counts[qi]);
        TEST_ASSERT_TRUE(e >= 0.0);
        if (measure == CDB_VEC_HAMMING)
            TEST_ASSERT_TRUE(e == 0.0);
        memset(in_list, 0, live_n);
        for (r = 0; r < w; r++) {
            size_t at = find_live(ids[r]);
            double cpu, err;
            TEST_ASSERT_TRUE_MESSAGE(at < live_n, "a candidate that is not live");
            TEST_ASSERT_FALSE_MESSAGE(in_list[at], "a candidate twice");
            in_list[at] = 1;
            if (r > 0)
                TEST_ASSERT_TRUE(dists[r - 1] <= dists[r]);
            cpu = cdb_vec_cpu_distance(elem, dims, measure, q, live_vecs[at], CDB_ORDER_LITTLE);
            err = fabs((double)dists[r] - cpu);
            if (err > e)
                printf("query %zu candidate %zu: device %.9g cpu %.9g error %.3g bound %.3g\n", qi, r,
                       (double)dists[r], cpu, err, e);
            TEST_ASSERT_TRUE_MESSAGE(err <= e, "a distance outside the bound");
            if (e > 0.0 && err / e > worst)
                worst = err / e;
            if (measure == CDB_VEC_HAMMING && r > 0 && dists[r - 1] == dists[r])
                TEST_ASSERT_TRUE_MESSAGE(ids[r - 1] < ids[r], "a Hamming tie not by lower ID");
        }
        /* Everything left out had a device distance of at least g, so a CPU
         * distance of at least g - e; with exact distances, a tie at g has a
         * higher ID than every candidate at g. */
        g = w ? (double)dists[w - 1] : 0.0;
        for (i = 0; i < live_n; i++) {
            double cpu;
            if (in_list[i])
                continue;
            cpu = cdb_vec_cpu_distance(elem, dims, measure, q, live_vecs[i], CDB_ORDER_LITTLE);
            TEST_ASSERT_TRUE_MESSAGE(cpu >= g - e, "a vector left out that could be nearer");
            if (measure == CDB_VEC_HAMMING && cpu == g)
                TEST_ASSERT_TRUE_MESSAGE(live_ids[i] > ids[w - 1], "a Hamming tie left out with a lower ID");
        }
    }
}

static void contract_case(unsigned elem, uint32_t dims, unsigned measure)
{
    cdb_accel_mirror *m = NULL;
    uint64_t ids[256], next_id = 1;
    size_t i, b;
    static const size_t wants[] = { 1, 16, 64, 700, 2500, 4000 }; /* the last more than are live */
    rng = 1000u + elem * 31u + measure * 7u + dims;
    live_n = 0;
    worst = 0.0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.accel->mirror_open(env.accel_ctx, 1, elem, dims, measure, CDB_ORDER_LITTLE,
                                                         100, &m));
    check_nearest(m, elem, dims, measure, 1, 10); /* empty: no candidates */

    /* 3000 vectors, more than the 100 expected: the mirror grows. */
    for (b = 0; b < 12; b++) {
        for (i = 0; i < 250; i++)
            ids[i] = next_id++;
        put(m, elem, dims, ids, 250);
    }
    for (i = 0; i < sizeof wants / sizeof wants[0]; i++)
        check_nearest(m, elem, dims, measure, i % 2u ? 1u : 3u, wants[i]);

    /* Replacements (one entry twice in a batch), deletes, new inserts. */
    for (i = 0; i < 200; i++)
        ids[i] = 1u + (next_u32() % 3000u);
    ids[199] = ids[0];
    put(m, elem, dims, ids, 200);
    for (b = 0; b < 2; b++) {
        for (i = 0; i < 150; i++)
            ids[i] = 1u + (next_u32() % 3000u);
        del(m, ids, 150);
    }
    for (i = 0; i < 200; i++)
        ids[i] = next_id++;
    put(m, elem, dims, ids, 200);
    for (i = 0; i < 4; i++)
        check_nearest(m, elem, dims, measure, NQ_MAX, i == 3 ? 900u : 32u);
    printf("elem %u dims %u measure %u: largest error %.3g of the bound\n", elem, (unsigned)dims, measure, worst);
    env.accel->mirror_close(env.accel_ctx, m);
}

static void test_f32(void)
{
    contract_case(CDB_VEC_F32, 384, CDB_VEC_COSINE);
    contract_case(CDB_VEC_F32, 384, CDB_VEC_DOT);
    contract_case(CDB_VEC_F32, 384, CDB_VEC_EUCLIDEAN);
    contract_case(CDB_VEC_F32, 37, CDB_VEC_EUCLIDEAN); /* not a multiple of 8: padded */
}

static void test_f16_bf16(void)
{
    contract_case(CDB_VEC_F16, 384, CDB_VEC_COSINE);
    contract_case(CDB_VEC_F16, 384, CDB_VEC_EUCLIDEAN);
    contract_case(CDB_VEC_BF16, 384, CDB_VEC_COSINE);
    contract_case(CDB_VEC_BF16, 384, CDB_VEC_DOT);
}

static void test_i8(void)
{
    contract_case(CDB_VEC_I8, 384, CDB_VEC_DOT);
    contract_case(CDB_VEC_I8, 384, CDB_VEC_COSINE);
    contract_case(CDB_VEC_I8, 100, CDB_VEC_EUCLIDEAN);
}

static void test_binary(void)
{
    contract_case(CDB_VEC_BINARY, 100, CDB_VEC_HAMMING); /* ties everywhere, unused bits set */
    contract_case(CDB_VEC_BINARY, 256, CDB_VEC_HAMMING);
}

/* Spec 18.7: what it cannot do, it declines. */
static void test_declines(void)
{
    cdb_accel_mirror *m = NULL;
    TEST_ASSERT_TRUE(env.accel->mirror_open(env.accel_ctx, 1, CDB_VEC_F32, 16, CDB_VEC_HAMMING, CDB_ORDER_LITTLE, 10,
                                            &m)
                     != CDB_OK);
    TEST_ASSERT_TRUE(env.accel->mirror_open(env.accel_ctx, 1, CDB_VEC_BINARY, 16, CDB_VEC_COSINE, CDB_ORDER_LITTLE,
                                            10, &m)
                     != CDB_OK);
    TEST_ASSERT_TRUE(env.accel->mirror_open(env.accel_ctx, 1, CDB_VEC_I8, 200000, CDB_VEC_DOT, CDB_ORDER_LITTLE, 10,
                                            &m)
                     != CDB_OK);
}

/* ------------------------------------------------------------ the engine
 *
 * Searches through the device must give exactly the CPU path's results,
 * ties and order included. Elements are small multiples of 1/4, so
 * distances tie often, the hard case for certifying (as test_accel). */

#define FCAP (2u * 1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 64u
#define DIMS 16u
#define K 10u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static uint8_t record_buf[2u * PAGE];
static uint8_t cat_work[48u * 1024u];
static cdb_index_entry cat_entries[1024];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t vec_buffers[3u * PAGE];
static cdb_db_config cfg;
static cdb_db db;

void setUp(void) {}
void tearDown(void) {}

static void open_db(void)
{
    cdb_faultenv_config c;
    cdb_header h;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 8192;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
    env.accel = dev_ops;
    env.accel_ctx = dev_ctx;
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
    cfg.catalog_work.entries_cap = 1024;
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_buffers;
    cfg.vec_buffers = vec_buffers;
    cfg.durability = CDB_DURABILITY_FULL;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5A, sizeof h.db_id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "a", &cfg, &h));
}

typedef struct vec {
    uint8_t enc[128];
    size_t n;
} vec;

static vec make_vec(cdb_vector_elem elem)
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
        int k = (int)(next_u32() >> 16) % 13 - 6;
        uint32_t b;
        f[i] = (float)k / 4.0f;
        i8[i] = (int8_t)k;
        if (k > 0)
            bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
        memcpy(&b, &f[i], sizeof b);
        h[i] = elem == CDB_VEC_F16 ? to_half(f[i]) : (uint16_t)(b >> 16); /* exact for these values */
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

static void same_as_cpu(uint64_t coll, const vec *q)
{
    cdb_vec_hit got[K], want[K];
    const cdb_accel_ops *ax = db.env.accel;
    size_t ng, nw, i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q->enc, q->n, CDB_VEC_BASE, got, K, &ng));
    db.env.accel = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q->enc, q->n, CDB_VEC_BASE, want, K, &nw));
    db.env.accel = ax;
    TEST_ASSERT_EQUAL_size_t(nw, ng);
    for (i = 0; i < nw; i++) {
        TEST_ASSERT_EQUAL_UINT64(want[i].entry, got[i].entry);
        TEST_ASSERT_TRUE(want[i].distance == got[i].distance);
        TEST_ASSERT_EQUAL_INT(want[i].source_state, got[i].source_state);
    }
}

/* Spec 18.4: a bulk search of 100 queries (two device requests) gives
 * each query the CPU path's answer. */
static void bulk_same_as_cpu(uint64_t coll, cdb_vector_elem elem)
{
    enum { NQ = 100 };
    static vec qv[NQ];
    static const void *qp[NQ];
    static size_t ql[NQ], counts[NQ];
    static cdb_vec_hit got[NQ * K];
    cdb_vec_hit want[K];
    const cdb_accel_ops *ax = db.env.accel;
    size_t i, j, nw;
    for (i = 0; i < NQ; i++) {
        qv[i] = make_vec(elem);
        qp[i] = qv[i].enc;
        ql[i] = qv[i].n;
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_bulk(&db, coll, qp, ql, NQ, CDB_VEC_BASE, CDB_VEC_AUTO, got, K,
                                                      counts));
    db.env.accel = NULL;
    for (i = 0; i < NQ; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, qp[i], ql[i], CDB_VEC_BASE, want, K, &nw));
        TEST_ASSERT_EQUAL_size_t(nw, counts[i]);
        for (j = 0; j < nw; j++) {
            TEST_ASSERT_EQUAL_UINT64(want[j].entry, got[i * K + j].entry);
            TEST_ASSERT_TRUE(want[j].distance == got[i * K + j].distance);
        }
    }
    db.env.accel = ax;
}

static void engine_case(cdb_vector_elem elem, unsigned measure)
{
    cdb_vec_config c;
    uint64_t coll, e;
    unsigned i;
    vec q;
    rng = 77u + (uint32_t)elem * 13u + measure;
    open_db();
    memset(&c, 0, sizeof c);
    c.elem = elem;
    c.dims = DIMS;
    c.measure = measure;
    c.model = "m";
    c.model_len = 1;
    c.model_version = "1";
    c.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "c", 1, &c, &coll));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 1500; i++) {
        vec v = make_vec(elem);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 0, &e));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    q = make_vec(elem);
    same_as_cpu(coll, &q); /* asks for the mirror */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&db));
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills);
    for (i = 0; i < 40; i++) {
        q = make_vec(elem);
        same_as_cpu(coll, &q);
    }
    bulk_same_as_cpu(coll, elem);
    /* Changes reach the mirror before its next use (spec 18.7). */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 50; i++) {
        vec v = make_vec(elem);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 0, &e));
    }
    for (i = 100; i < 160; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    for (i = 0; i < 20; i++) {
        q = make_vec(elem);
        same_as_cpu(coll, &q);
    }
    printf("elem %u measure %u: %llu certified, %llu widened, %llu on the CPU\n", (unsigned)elem, measure,
           (unsigned long long)db.accel_stats.certified, (unsigned long long)db.accel_stats.widened,
           (unsigned long long)db.accel_stats.cpu);
    TEST_ASSERT_TRUE(db.accel_stats.certified > 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_engine_matches_cpu(void)
{
    engine_case(CDB_VEC_F32, CDB_VEC_COSINE);
    engine_case(CDB_VEC_F32, CDB_VEC_DOT);
    engine_case(CDB_VEC_F32, CDB_VEC_EUCLIDEAN);
    engine_case(CDB_VEC_F16, CDB_VEC_EUCLIDEAN);
    engine_case(CDB_VEC_BF16, CDB_VEC_COSINE);
    engine_case(CDB_VEC_I8, CDB_VEC_DOT);
    engine_case(CDB_VEC_BINARY, CDB_VEC_HAMMING);
}

/* Every test on the device env holds now. */
static void run_device_tests(void)
{
    dev_ops = env.accel;
    dev_ctx = env.accel_ctx;
    RUN_TEST(test_declines);
    RUN_TEST(test_f32);
    RUN_TEST(test_f16_bf16);
    RUN_TEST(test_i8);
    RUN_TEST(test_binary);
    RUN_TEST(test_engine_matches_cpu);
}
