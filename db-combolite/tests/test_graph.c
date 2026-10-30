#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_faultenv.h"
#include "cdb_vec.h"

#define FCAP (1536u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define DIMS 16u
#define MAXN 700u
#define K 8u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
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
static uint8_t vec_work[64u * 1024u];
static cdb_db_config cfg;
static cdb_db db;

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
    cfg.vec_buffers = vec_buffers;
    cfg.vec_work = vec_work;
    cfg.vec_work_size = sizeof vec_work;
    cfg.durability = CDB_DURABILITY_NORMAL;
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x6B, sizeof h.db_id);
    return cdb_db_open(&db, &env, "g", &cfg, &h);
}

/* ------------------------------------------------------------ vectors */

typedef struct vec {
    double x[DIMS];
    uint8_t enc[96];
    size_t n;
} vec;

static vec all[MAXN + 8];
static int live[MAXN + 8];
static uint64_t count;   /* entries handed out */
static uint32_t rng;
static unsigned graph_k = K;   /* K of the graph under test */
static cdb_vector_elem elem_of = CDB_VEC_F32;
static unsigned measure_of = CDB_VEC_COSINE;

static int next_int(int lo, int hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 16) % (uint32_t)(hi - lo + 1));
}

/* Values are small multiples of 1/4 (exact in f32) or small integers (int8).
 * center: NULL for a random vector, else a point near it. */
static vec make_vec(const vec *center)
{
    vec v;
    cdb_value_writer w;
    float f[DIMS];
    int8_t i8[DIMS];
    unsigned i;
    for (i = 0; i < DIMS; i++) {
        int k = center ? (int)(center->x[i] * (elem_of == CDB_VEC_I8 ? 1.0 : 4.0)) + next_int(-2, 2) : next_int(-20, 20);
        if (elem_of == CDB_VEC_I8) {
            i8[i] = (int8_t)k;
            v.x[i] = k;
        } else {
            f[i] = (float)k / 4.0f;
            v.x[i] = (double)k / 4.0;
        }
    }
    cdb_value_writer_init(&w, v.enc, sizeof v.enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (elem_of == CDB_VEC_I8)
        cdb_value_write_vector(&w, elem_of, DIMS, i8);
    else
        cdb_value_write_vector(&w, elem_of, DIMS, f);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    v.n = cdb_value_writer_len(&w);
    return v;
}

/* The distance the library stores: computed in double from the same values
 * (cosine with norms rounded to f32, as blocks cache them), then rounded. */
static double ref_dist(unsigned m, const vec *a, const vec *b)
{
    double dot = 0, na = 0, nb = 0, l2 = 0;
    unsigned i;
    for (i = 0; i < DIMS; i++) {
        dot += a->x[i] * b->x[i];
        na += a->x[i] * a->x[i];
        nb += b->x[i] * b->x[i];
        l2 += (a->x[i] - b->x[i]) * (a->x[i] - b->x[i]);
    }
    if (m == CDB_VEC_COSINE) {
        double fa = (float)sqrt(na), fb = (float)sqrt(nb);
        return (fa == 0.0 || fb == 0.0) ? 1.0 : 1.0 - dot / (fa * fb);
    }
    if (m == CDB_VEC_DOT)
        return -dot;
    return sqrt(l2);
}

static uint64_t make_coll(const char *name)
{
    cdb_vec_config c;
    uint64_t id;
    memset(&c, 0, sizeof c);
    c.elem = elem_of;
    c.dims = DIMS;
    c.measure = measure_of;
    c.model = "m";
    c.model_len = 1;
    c.model_version = "1";
    c.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, name, strlen(name), &c, &id));
    return id;
}

static void add(uint64_t coll, const vec *v)
{
    uint64_t e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v->enc, v->n, 0, 0, &e));
    TEST_ASSERT_EQUAL_UINT64(++count, e);
    all[e] = *v;
    live[e] = 1;
}

static size_t pending_left(uint64_t coll)
{
    size_t n;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 1, &n)); /* refreshes one, then counts */
    return n;
}

/* ------------------------------------------------------------ oracle */

static struct cdb_vec_coll *coll_of(uint64_t store)
{
    struct cdb_vec_coll *c = (struct cdb_vec_coll *)cdb_slots_find(&db.vec_slots, store);
    if (c != NULL)
        return c;
    TEST_FAIL_MESSAGE("collection not open");
    return NULL;
}

/* The exact K nearest of p among live entries, by (distance as f32, entry). */
static unsigned reference_list(uint64_t p, uint64_t *ids, float *ds)
{
    unsigned n = 0, i;
    uint64_t u;
    for (u = 1; u <= count; u++) {
        float d;
        if (!live[u] || u == p)
            continue;
        d = (float)ref_dist(measure_of, &all[p], &all[u]);
        if (n == graph_k && !(d < ds[graph_k - 1] || (d == ds[graph_k - 1] && u < ids[graph_k - 1])))
            continue;
        if (n == graph_k)
            n--;
        for (i = n; i > 0 && (ds[i - 1] > d || (ds[i - 1] == d && ids[i - 1] > u)); i--) {
            ds[i] = ds[i - 1];
            ids[i] = ids[i - 1];
        }
        ds[i] = d;
        ids[i] = u;
        n++;
    }
    return n;
}

/* Structure that holds at every moment: records list live entries with the
 * right distances, in order; the reverse index mirrors the records; the
 * live count is right. With exact set, every list must also be fresh and
 * exactly the K nearest. */
static void verify_graph(uint64_t store, int exact)
{
    static uint8_t page[PAGE];
    struct cdb_vec_coll *c;
    cdb_index_cursor cur;
    cdb_vec_hit hits[K + 2];
    uint8_t k[16], v[16];
    size_t klen, vlen, n, rows = 0, listed = 0, livecount = 0, eps = 0;
    uint64_t p;
    int done, fresh;

    for (p = 1; p <= count; p++) {
        size_t i;
        if (!live[p]) {
            TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_neighbours(&db, store, p, hits, K, &n, &fresh));
            continue;
        }
        livecount++;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_neighbours(&db, store, p, hits, K + 2, &n, &fresh));
        TEST_ASSERT_TRUE(n <= graph_k);
        listed += n;
        for (i = 0; i < n; i++) {
            float d = (float)ref_dist(measure_of, &all[p], &all[hits[i].entry]);
            TEST_ASSERT_TRUE_MESSAGE(hits[i].entry != p && live[hits[i].entry], "a list holds a dead or self entry");
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(d, (float)hits[i].distance, "stored distance");
            if (i > 0)
                TEST_ASSERT_TRUE_MESSAGE(hits[i - 1].distance < hits[i].distance
                                             || (hits[i - 1].distance == hits[i].distance && hits[i - 1].entry < hits[i].entry),
                                         "list order");
        }
        if (exact) {
            uint64_t ids[K];
            float ds[K];
            unsigned m = reference_list(p, ids, ds), j;
            char msg[64];
            snprintf(msg, sizeof msg, "list of entry %u", (unsigned)p);
            TEST_ASSERT_TRUE_MESSAGE(fresh, "pending after a full refresh");
            TEST_ASSERT_EQUAL_UINT_MESSAGE(m, n, msg);
            for (j = 0; j < m; j++)
                TEST_ASSERT_EQUAL_UINT64_MESSAGE(ids[j], hits[j].entry, msg);
        }
    }
    c = coll_of(store); /* open by now */
    /* The reverse index holds exactly the listed pairs. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&cur, &c->reverse, page, NULL, 0));
    for (;;) {
        uint64_t nbr, e;
        size_t i;
        int found = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done));
        if (done)
            break;
        nbr = cdb_load_u64(k, CDB_ORDER_BIG);
        e = cdb_load_u64(k + 8, CDB_ORDER_BIG);
        if (nbr == 0) { /* an entry point: spread (1) or added to reach a piece (2) */
            TEST_ASSERT_TRUE_MESSAGE(e >= 1 && e <= count && live[e], "a dead entry point");
            TEST_ASSERT_EQUAL_size_t_MESSAGE(1, vlen, "entry point kind");
            TEST_ASSERT_TRUE_MESSAGE(v[0] == 1 || v[0] == 2, "entry point kind");
            if (v[0] == 1)
                eps++;
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(e <= count && live[e], "a reverse row of a dead entry");
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_neighbours(&db, store, e, hits, K, &n, &fresh));
        for (i = 0; i < n; i++)
            found |= hits[i].entry == nbr;
        TEST_ASSERT_TRUE_MESSAGE(found, "a reverse row without its list entry");
        rows++;
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(listed, rows, "reverse rows vs listed neighbours");
    {
        /* Live count; after a full refresh, at least one spread entry point
         * and at most the larger of 8 and the square root of the live count. */
        uint8_t key[8], meta[16];
        size_t ml;
        int found;
        cdb_store_u64(key, 0, CDB_ORDER_BIG);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(&c->graph, key, 8, meta, sizeof meta, &ml, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_size_t(8, ml);
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(livecount, cdb_load_u64(meta, CDB_ORDER_BIG), "live count");
        if (exact)
            TEST_ASSERT_TRUE_MESSAGE((eps >= 1 || livecount == 0) && eps * eps <= (livecount > 64 ? livecount + 2 * eps : 64),
                                     "spread entry point count");
    }
}

static void reset_data(uint32_t seed)
{
    memset(live, 0, sizeof live);
    count = 0;
    rng = seed;
}

/* ------------------------------------------------------------ tests */

static void test_build_is_exact(void)
{
    uint64_t coll;
    size_t left, n;
    cdb_vec_hit hits[4];
    int fresh, i;

    reset_data(11u);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 60; i++) {
        vec v = make_vec(NULL);
        add(coll, &v);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create_graph(&db, coll, K));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_neighbours(&db, coll, 1, hits, 4, &n, &fresh));
    TEST_ASSERT_FALSE(fresh); /* queued, not built yet */
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 0, &left));
    TEST_ASSERT_EQUAL_size_t(0, left);
    verify_graph(coll, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_neighbours(&db, coll, 1, hits, 4, &n, &fresh));
    TEST_ASSERT_TRUE(fresh);
    TEST_ASSERT_EQUAL_size_t(4, n); /* max limits what is copied */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* It persists. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    verify_graph(coll, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void follow_changes(cdb_vector_elem elem, unsigned measure)
{
    uint64_t coll, e;
    size_t left, before;
    int i;

    elem_of = elem;
    measure_of = measure;
    reset_data(23u + (uint32_t)measure);
    setUp();
    cfg.vec_refresh_budget = 5;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create_graph(&db, coll, K)); /* on an empty collection */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 40; i++) {
        vec v = make_vec(NULL);
        add(coll, &v);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    verify_graph(coll, 0);

    /* Reconciliation refreshes at most its budget. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    before = pending_left(coll);
    TEST_ASSERT_EQUAL_size_t(40 - 5 - 1, before);
    verify_graph(coll, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 0, &left));
    TEST_ASSERT_EQUAL_size_t(0, left);
    verify_graph(coll, 1);

    /* Deletes, replaces and inserts, then a full refresh. */
    for (i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        for (e = 1; e <= count; e++) {
            if (!live[e] || (e + (uint64_t)i) % 7 != 0)
                continue;
            if (e % 2) {
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, e));
                live[e] = 0;
            } else {
                all[e] = make_vec(NULL);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, e, all[e].enc, all[e].n));
            }
        }
        {
            vec v = make_vec(&all[2]);
            add(coll, &v);
        }
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
        verify_graph(coll, 0);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 0, &left));
        verify_graph(coll, 1);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    elem_of = CDB_VEC_F32;
    measure_of = CDB_VEC_COSINE;
}

static void test_graph_follows_changes(void)
{
    follow_changes(CDB_VEC_F32, CDB_VEC_COSINE);
    follow_changes(CDB_VEC_I8, CDB_VEC_EUCLIDEAN);
    follow_changes(CDB_VEC_F32, CDB_VEC_DOT);
}

/* Approximate search finds nearly what exact search finds on clustered
 * data, and never misses a pending vector. */
static void test_approximate_recall(void)
{
    static vec centers[60];
    cdb_vec_hit ex[10], ap[10];
    uint64_t coll;
    size_t ne, na, left, found = 0, wanted = 0;
    int i, t;

    reset_data(77u);
    cfg.vec_exact_below = 100; /* the automatic rule picks the walk here */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c");
    /* 60 clusters of 10: more pieces than the 25 spread entry points. */
    for (i = 0; i < 60; i++)
        centers[i] = make_vec(NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 600; i++) {
        vec v = make_vec(&centers[i % 60]);
        add(coll, &v);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create_graph(&db, coll, K));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 0, &left));
    TEST_ASSERT_EQUAL_size_t(0, left);
    verify_graph(coll, 1);
    /* Every vector is found by a walk for its own value. */
    for (i = 1; i <= 600; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_mode(&db, coll, all[i].enc, all[i].n, 0, CDB_VEC_APPROX, ap, 1, &na));
        TEST_ASSERT_EQUAL_size_t(1, na);
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, 0.0, ap[0].distance, "a vector the walk can't reach");
    }
    for (t = 0; t < 40; t++) {
        vec q = make_vec(&centers[t % 60]);
        size_t a, b;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_mode(&db, coll, q.enc, q.n, 0, CDB_VEC_EXACT, ex, 10, &ne));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, coll, q.enc, q.n, 0, ap, 10, &na)); /* automatic: walk */
        TEST_ASSERT_EQUAL_size_t(10, na);
        for (a = 0; a < ne; a++) {
            wanted++;
            for (b = 0; b < na; b++)
                if (ap[b].entry == ex[a].entry) {
                    found++;
                    break;
                }
        }
        for (b = 1; b < na; b++)
            TEST_ASSERT_TRUE(ap[b - 1].distance <= ap[b].distance);
    }
    {
        char msg[64];
        snprintf(msg, sizeof msg, "recall@10: %u of %u", (unsigned)found, (unsigned)wanted);
        TEST_MESSAGE(msg);
    }
    TEST_ASSERT_TRUE_MESSAGE(found * 100u >= wanted * 90u, "recall below 90%");

    /* A pending vector identical to the query is found first. */
    {
        vec q = make_vec(&centers[3]);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        add(coll, &q);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_mode(&db, coll, q.enc, q.n, 0, CDB_VEC_APPROX, ap, 3, &na));
        TEST_ASSERT_EQUAL_UINT64(count, ap[0].entry);
        TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, ap[0].distance);
    }
    /* Another measure on the walk is ranked with that measure. */
    {
        vec q = make_vec(&centers[5]);
        size_t b;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_mode(&db, coll, q.enc, q.n, CDB_VEC_EUCLIDEAN, CDB_VEC_APPROX,
                                                          ap, 10, &na));
        TEST_ASSERT_EQUAL_size_t(10, na);
        for (b = 0; b < na; b++) {
            TEST_ASSERT_DOUBLE_WITHIN(1e-6, ref_dist(CDB_VEC_EUCLIDEAN, &q, &all[ap[b].entry]), ap[b].distance);
            if (b)
                TEST_ASSERT_TRUE(ap[b - 1].distance <= ap[b].distance);
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rejects_bad_use(void)
{
    uint64_t coll;
    cdb_vec_hit hits[2];
    size_t n;
    int fresh, found;
    reset_data(5u);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    coll = make_coll("c");
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_vec_refresh(&db, coll, 0, &n)); /* no graph yet */
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_vec_neighbours(&db, coll, 1, hits, 2, &n, &fresh));
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_vec_create_graph(&db, coll, K + 1)); /* 1 KB pages hold 8 */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_vec_create_graph(&db, coll, CDB_VEC_MAX_K + 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_vec_create_graph(&db, coll, K));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    db.cfg.vec_work = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_vec_create_graph(&db, coll, K));
    db.cfg.vec_work = vec_work;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create_graph(&db, coll, K));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_vec_create_graph(&db, coll, K));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_vec_neighbours(&db, coll, 1, hits, 2, &n, &fresh));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));

    /* Without working memory a collection with a graph cannot be used. */
    cfg.vec_work = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "c", 1, &coll, &found));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_vec_refresh(&db, coll, 0, &n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* ------------------------------------------------------- crash sweep */

static vec sweep_v[48];

/* S1: 20 vectors, graph created. S2: fully refreshed. S3: 3 deleted, 2
 * replaced, 3 inserted, reconciled with a budget. S4: fully refreshed. */
static cdb_status workload(int *acked)
{
    cdb_vec_config c;
    uint64_t coll, e;
    size_t left;
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
        TRY(cdb_vec_insert(&db, coll, sweep_v[i].enc, sweep_v[i].n, 0, 0, &e));
    TRY(cdb_tx_commit(&db));
    TRY(cdb_vec_create_graph(&db, coll, 6));
    *acked = 1;
    TRY(cdb_vec_refresh(&db, coll, 0, &left));
    *acked = 2;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(cdb_vec_delete(&db, coll, 3));
    TRY(cdb_vec_delete(&db, coll, 9));
    TRY(cdb_vec_delete(&db, coll, 14));
    TRY(cdb_vec_replace(&db, coll, 5, sweep_v[40].enc, sweep_v[40].n));
    TRY(cdb_vec_replace(&db, coll, 11, sweep_v[41].enc, sweep_v[41].n));
    for (i = 21; i <= 23; i++)
        TRY(cdb_vec_insert(&db, coll, sweep_v[i].enc, sweep_v[i].n, 0, 0, &e));
    TRY(cdb_tx_commit(&db));
    TRY(cdb_db_reconcile(&db));
    *acked = 3;
    TRY(cdb_vec_refresh(&db, coll, 0, &left));
    *acked = 4;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

static int observed(uint64_t *coll_out)
{
    uint8_t out[96];
    size_t len;
    uint64_t coll, e;
    int found, k;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "c", 1, &coll, &found));
    *coll_out = found ? coll : 0;
    if (!found || cdb_vec_get(&db, coll, 1, out, sizeof out, &len, NULL, NULL) == CDB_E_NOT_FOUND)
        return 0;
    k = cdb_vec_get(&db, coll, 21, out, sizeof out, &len, NULL, NULL) == CDB_OK ? 3 : 1;
    memset(live, 0, sizeof live);
    count = k >= 3 ? 23 : 20;
    for (e = 1; e <= count; e++) {
        all[e] = sweep_v[e];
        live[e] = !(k >= 3 && (e == 3 || e == 9 || e == 14));
    }
    if (k >= 3) {
        all[5] = sweep_v[40];
        all[11] = sweep_v[41];
    }
    {
        cdb_vec_hit h[1];
        size_t n;
        int fresh;
        cdb_status s = cdb_vec_neighbours(&db, coll, 1, h, 1, &n, &fresh);
        if (s == CDB_E_STATE)
            return 0; /* the graph was never created */
        TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    }
    verify_graph(coll, 0);
    return k;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[2] = { 0, 300 };
    uint64_t total, op;
    int acked, m, i;

    reset_data(4049u);
    for (i = 1; i < 48; i++)
        sweep_v[i] = make_vec(NULL);
    graph_k = 6;
    cfg.vec_refresh_budget = 2; /* reconciliation leaves work for the refresh */
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
                size_t left;
                int k;
                init_env();
                cdb_faultenv_arm(&fe, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultenv_reboot(&fe);
                TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, open_db(), "recovery open failed");
                k = observed(&coll);
                TEST_ASSERT_TRUE_MESSAGE(k >= (acked >= 3 ? 3 : (acked ? 1 : 0)), "an acknowledged commit was lost");
                if (k >= 1) {
                    cdb_vec_hit h[1];
                    size_t n;
                    int fresh;
                    if (cdb_vec_neighbours(&db, coll, 1, h, 1, &n, &fresh) == CDB_OK) {
                        /* Whatever was left pending, a refresh makes the graph exact. */
                        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh(&db, coll, 0, &left));
                        TEST_ASSERT_EQUAL_size_t(0, left);
                        verify_graph(coll, 1);
                    }
                }
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_is_exact);
    RUN_TEST(test_graph_follows_changes);
    RUN_TEST(test_approximate_recall);
    RUN_TEST(test_rejects_bad_use);
    RUN_TEST(test_crash_sweep);
    graph_k = K;
    return UNITY_END();
}
