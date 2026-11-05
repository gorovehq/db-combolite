/* The accelerator path (spec 18), on the fake accelerator: every
 * accelerated search must give exactly what the CPU path gives on the same
 * database, whatever the accelerator's noise, declines or failures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_fakeaccel.h"
#include "cdb_faultenv.h"
#include "cdb_profile.h"
#include "cdb_vec.h"

#define FCAP (2u * 1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 64u
#define DIMS 16u
#define K 10u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;
static cdb_fakeaccel fake;

/* Buffers for two processes on the one environment. */
typedef struct proc {
    uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
    cdb_pager_node nodes[NODES];
    cdb_free_extent free_list[4096];
    uint8_t record_buf[2u * PAGE];
    uint8_t cat_work[48u * 1024u];
    cdb_index_entry cat_entries[1024];
    uint8_t cat_page[PAGE];
    uint8_t doc_buffers[8u * PAGE];
    uint8_t vec_buffers[3u * PAGE];
    cdb_db_config cfg;
    cdb_db d;
} proc;

static proc pa, pb;
#define db (pa.d)

static void config(proc *p)
{
    memset(&p->cfg, 0, sizeof p->cfg);
    p->cfg.pager.arena = p->arena;
    p->cfg.pager.arena_size = sizeof p->arena;
    p->cfg.pager.nodes = p->nodes;
    p->cfg.pager.node_cap = NODES;
    p->cfg.pager.free_list = p->free_list;
    p->cfg.pager.free_cap = sizeof p->free_list / sizeof p->free_list[0];
    p->cfg.record_buf = p->record_buf;
    p->cfg.record_buf_size = sizeof p->record_buf;
    p->cfg.catalog_work.bytes = p->cat_work;
    p->cfg.catalog_work.bytes_size = sizeof p->cat_work;
    p->cfg.catalog_work.entries = p->cat_entries;
    p->cfg.catalog_work.entries_cap = 1024;
    p->cfg.catalog_page = p->cat_page;
    p->cfg.doc_buffers = p->doc_buffers;
    p->cfg.vec_buffers = p->vec_buffers;
    p->cfg.durability = CDB_DURABILITY_FULL;
}

void setUp(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 8192;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
    cdb_fakeaccel_init(&fake, &env);
    config(&pa);
    config(&pb);
}

void tearDown(void) {}

static cdb_status open_proc(proc *p)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5A, sizeof h.db_id);
    return cdb_db_open(&p->d, &env, "a", &p->cfg, &h);
}

/* ------------------------------------------------------------ vectors
 *
 * Elements are small multiples of 1/4, so distances tie often: the hard
 * case for certifying a shortlist. */

typedef struct vec {
    uint8_t enc[128];
    size_t n;
} vec;

static uint32_t rng;

static int next_int(int lo, int hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)((rng >> 16) % (uint32_t)(hi - lo + 1));
}

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
        int k = next_int(-6, 6);
        f[i] = (float)k / 4.0f;
        i8[i] = (int8_t)k;
        if (k > 0)
            bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
        {
            uint32_t b;
            memcpy(&b, &f[i], sizeof b);
            h[i] = (uint16_t)(b >> 16); /* bf16; exact for these values, and f16 below */
        }
        if (elem == CDB_VEC_F16) {
            /* (k/4) as a half: exact for |k| <= 6 */
            uint32_t b;
            unsigned sign, e;
            memcpy(&b, &f[i], sizeof b);
            sign = (b >> 31) & 1u;
            if ((b & 0x7FFFFFFFu) == 0) {
                h[i] = (uint16_t)(sign << 15);
            } else {
                e = ((b >> 23) & 0xFFu) - 127u + 15u;
                h[i] = (uint16_t)((sign << 15) | (e << 10) | ((b >> 13) & 0x3FFu));
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

static uint64_t make_coll(cdb_db *d, const char *name, cdb_vector_elem elem, unsigned measure)
{
    cdb_vec_config c;
    uint64_t id;
    memset(&c, 0, sizeof c);
    c.elem = elem;
    c.dims = DIMS;
    c.measure = measure;
    c.model = "m";
    c.model_len = 1;
    c.model_version = "1";
    c.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(d, name, strlen(name), &c, &id));
    return id;
}

static void fill(cdb_db *d, uint64_t coll, cdb_vector_elem elem, unsigned n)
{
    unsigned i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(d, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < n; i++) {
        vec v = make_vec(elem);
        uint64_t e;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(d, coll, v.enc, v.n, 0, 0, &e));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(d));
}

/* Searches with the accelerator and without it (the CPU path on the same
 * database), and checks the two agree exactly, ties and order included. */
static void same_as_cpu(cdb_db *d, uint64_t coll, const vec *q)
{
    cdb_vec_hit got[K], want[K];
    const cdb_accel_ops *ax = d->env.accel;
    size_t ng, nw, i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(d, coll, q->enc, q->n, CDB_VEC_BASE, got, K, &ng));
    d->env.accel = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(d, coll, q->enc, q->n, CDB_VEC_BASE, want, K, &nw));
    d->env.accel = ax;
    TEST_ASSERT_EQUAL_size_t(nw, ng);
    for (i = 0; i < nw; i++) {
        TEST_ASSERT_EQUAL_UINT64(want[i].entry, got[i].entry);
        TEST_ASSERT_TRUE(want[i].distance == got[i].distance);
        TEST_ASSERT_EQUAL_INT(want[i].source_state, got[i].source_state);
    }
}

/* A first search asks for the mirror; the next reconciliation that commits
 * something, or tick, fills it (spec 18.6). */
static void mirror(cdb_db *d, uint64_t coll, cdb_vector_elem elem)
{
    vec q = make_vec(elem);
    same_as_cpu(d, coll, &q);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(d));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(d));
}

/* The fault environment's sleep hook: the other process runs meanwhile. */
static void tick_both(void *arg)
{
    (void)arg;
    if (pa.d.coord.joined)
        (void)cdb_db_tick(&pa.d);
    if (pb.d.coord.joined)
        (void)cdb_db_tick(&pb.d);
}

/* ------------------------------------------------------------ tests */

static void search_case(cdb_vector_elem elem, unsigned measure, double noise)
{
    uint64_t coll, fills;
    int i;
    setUp(); /* a fresh environment for each case */
    rng = 7u + (uint32_t)elem * 13u + measure;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pa));
    fake.noise = noise;
    coll = make_coll(&db, "c", elem, measure);
    fill(&db, coll, elem, 400);
    fills = db.accel_stats.fills;
    mirror(&db, coll, elem);
    TEST_ASSERT_EQUAL_UINT64(fills + 1u, db.accel_stats.fills);
    for (i = 0; i < 30; i++) {
        vec q = make_vec(elem);
        same_as_cpu(&db, coll, &q);
    }
    TEST_ASSERT_TRUE(db.accel_stats.certified > 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_UINT(0, fake.live); /* closed with the database */
}

/* Spec 18.3, 18.9, 18.10: every type and measure, the CPU's answer. */
static void test_certified_matches_cpu(void)
{
    search_case(CDB_VEC_F32, CDB_VEC_COSINE, 0.0);
    search_case(CDB_VEC_F32, CDB_VEC_DOT, 0.0);
    search_case(CDB_VEC_F32, CDB_VEC_EUCLIDEAN, 0.0);
    search_case(CDB_VEC_F16, CDB_VEC_EUCLIDEAN, 0.0);
    search_case(CDB_VEC_BF16, CDB_VEC_COSINE, 0.0);
    search_case(CDB_VEC_I8, CDB_VEC_DOT, 0.0);
    search_case(CDB_VEC_BINARY, CDB_VEC_HAMMING, 0.0);
}

/* Exact distances certify on ties (spec 18.10): binary never widens. */
static void test_exact_ties_never_widen(void)
{
    search_case(CDB_VEC_BINARY, CDB_VEC_HAMMING, 0.0);
    TEST_ASSERT_EQUAL_UINT64(0, db.accel_stats.widened);
}

/* With noise near the gaps between distances, shortlists fail their check
 * and widen, or go to the CPU, and the answers still match. */
static void test_noise_widens_and_still_matches(void)
{
    search_case(CDB_VEC_F32, CDB_VEC_EUCLIDEAN, 0.3);
    TEST_ASSERT_TRUE(db.accel_stats.widened + db.accel_stats.cpu > 1);
    search_case(CDB_VEC_I8, CDB_VEC_DOT, 25.0); /* i8 dot products span hundreds */
    TEST_ASSERT_TRUE(db.accel_stats.widened + db.accel_stats.cpu > 1);
}

/* Spec 18.7: changes applied in the process reach the mirror before its
 * next use; more than its queue holds makes it stale, filled again. */
static void test_mirror_follows_changes(void)
{
    uint64_t coll, e;
    vec q;
    int i;
    rng = 99u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pa));
    coll = make_coll(&db, "c", CDB_VEC_F32, CDB_VEC_EUCLIDEAN);
    fill(&db, coll, CDB_VEC_F32, 300);
    mirror(&db, coll, CDB_VEC_F32);
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills);

    /* Inserts, a replace and deletes, then searches without reconciling. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 40; i++) {
        vec v = make_vec(CDB_VEC_F32);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 0, &e));
    }
    q = make_vec(CDB_VEC_F32);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_replace(&db, coll, 3, q.enc, q.n));
    for (i = 10; i < 30; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, (uint64_t)i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    same_as_cpu(&db, coll, &q); /* the replaced vector is its own nearest */
    for (i = 0; i < 20; i++) {
        vec r = make_vec(CDB_VEC_F32);
        same_as_cpu(&db, coll, &r);
    }
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills); /* no refill was needed */
    TEST_ASSERT_TRUE(db.accel_stats.certified > 0);

    /* More changes than the queue holds: stale, and filled again. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < (int)CDB_ACCEL_QUEUE + 10; i++) {
        vec v = make_vec(CDB_VEC_F32);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, coll, v.enc, v.n, 0, 0, &e));
        if (i % 500 == 499) {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_TRUE(db.accel_stats.stale >= 1);
    same_as_cpu(&db, coll, &q); /* on the CPU meanwhile */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_UINT64(2, db.accel_stats.fills);
    same_as_cpu(&db, coll, &q);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Spec 18.7: declined and failing calls fall back, never reach the caller. */
static void test_declines_and_failures(void)
{
    uint64_t coll, bits;
    vec q;
    int i;
    rng = 5u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pa));
    fake.decline_elems = 1u << CDB_VEC_BINARY;
    coll = make_coll(&db, "c", CDB_VEC_F32, CDB_VEC_COSINE);
    bits = make_coll(&db, "b", CDB_VEC_BINARY, CDB_VEC_HAMMING);
    fill(&db, coll, CDB_VEC_F32, 300);
    fill(&db, bits, CDB_VEC_BINARY, 300);

    /* A declined type is never mirrored: the CPU answers. */
    mirror(&db, bits, CDB_VEC_BINARY);
    q = make_vec(CDB_VEC_BINARY);
    same_as_cpu(&db, bits, &q);
    TEST_ASSERT_EQUAL_UINT64(0, db.accel_stats.fills);

    /* A failing fill leaves the mirror wanted; the next one succeeds. */
    fake.fail_put_at = fake.puts + 1u;
    mirror(&db, coll, CDB_VEC_F32);
    TEST_ASSERT_EQUAL_UINT64(0, db.accel_stats.fills);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&db));
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills);

    /* A failing nearest: that search on the CPU, the same answer. */
    fake.fail_nearest_at = fake.nearests + 1u;
    q = make_vec(CDB_VEC_F32);
    same_as_cpu(&db, coll, &q);

    /* A failing delete makes the mirror stale; answers stay right. */
    fake.fail_del_at = fake.dels + 1u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&db, coll, 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    for (i = 0; i < 5; i++) {
        vec r = make_vec(CDB_VEC_F32);
        same_as_cpu(&db, coll, &r);
    }
    TEST_ASSERT_TRUE(db.accel_stats.stale >= 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_UINT(0, fake.live);
}

/* Spec 18.8: accel_mem caps the mirrors together; the least recently used
 * is evicted; a collection that cannot fit stays on the CPU; one that
 * grows past the budget goes stale. */
static void test_budget_and_eviction(void)
{
    uint64_t a, b, big;
    vec q;
    rng = 17u;
    pa.cfg.accel_mem = 300u * DIMS * 4u + 1000u; /* one 300-vector f32 collection, not two */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pa));
    a = make_coll(&db, "a", CDB_VEC_F32, CDB_VEC_EUCLIDEAN);
    b = make_coll(&db, "b", CDB_VEC_F32, CDB_VEC_EUCLIDEAN);
    big = make_coll(&db, "big", CDB_VEC_F32, CDB_VEC_EUCLIDEAN);
    fill(&db, a, CDB_VEC_F32, 300);
    fill(&db, b, CDB_VEC_F32, 300);
    fill(&db, big, CDB_VEC_F32, 400);

    mirror(&db, a, CDB_VEC_F32);
    TEST_ASSERT_EQUAL_UINT(1, fake.live);
    mirror(&db, b, CDB_VEC_F32); /* evicts a */
    TEST_ASSERT_EQUAL_UINT(1, fake.live);
    TEST_ASSERT_TRUE(db.accel_stats.evicted >= 1);
    q = make_vec(CDB_VEC_F32);
    same_as_cpu(&db, a, &q);
    same_as_cpu(&db, b, &q);

    /* Too big for the budget alone: never mirrored, always right. */
    mirror(&db, big, CDB_VEC_F32);
    same_as_cpu(&db, big, &q);
    TEST_ASSERT_TRUE(db.accel_bytes <= pa.cfg.accel_mem);

    /* b grows past the budget: stale, and the CPU answers. */
    mirror(&db, b, CDB_VEC_F32);
    fill(&db, b, CDB_VEC_F32, 50);
    same_as_cpu(&db, b, &q);
    TEST_ASSERT_TRUE(db.accel_bytes <= pa.cfg.accel_mem);
    TEST_ASSERT_TRUE(db.accel_stats.stale >= 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Spec 18.12: process B changes a collection A mirrors; at A's rebase the
 * mirror catches up from the change log, with no refill. */
static void test_catch_up_across_processes(void)
{
    uint64_t coll, e;
    vec q;
    int i;
    rng = 23u;
    pa.cfg.coord.max_coords = 4;
    pb.cfg.coord.max_coords = 4;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pa));
    coll = make_coll(&db, "c", CDB_VEC_F32, CDB_VEC_COSINE);
    fill(&db, coll, CDB_VEC_F32, 300);
    mirror(&db, coll, CDB_VEC_F32);
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills);

    /* While one process waits (to join, or for the baton), the other runs,
     * as test_coord does: the fault environment's sleep ticks both. */
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&pb));
    TEST_ASSERT_EQUAL_UINT(1, pb.d.coord.me.slot);
    pb.d.env.accel = NULL; /* B has no accelerator */
    /* B takes the baton from A first, so that B, not A, reconciles B's
     * changes: A then learns of them only from the archive moving. (Had A
     * replayed B's journal as baton holder, its mirror would have had them
     * through the apply, spec 5.9.1.) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&pb.d));
    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&pb.d, CDB_DURABILITY_DEFAULT));
    for (i = 0; i < 30; i++) {
        vec v = make_vec(CDB_VEC_F32);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&pb.d, coll, v.enc, v.n, 0, 0, &e));
    }
    for (i = 20; i < 40; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_delete(&pb.d, coll, (uint64_t)i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&pb.d));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&pb.d)); /* A yields its standing baton on tick */

    /* A sees the archive move at its next tick, rebases and catches up. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&db));
    TEST_ASSERT_TRUE(db.accel_stats.catchups >= 1);
    TEST_ASSERT_EQUAL_UINT64(0, db.accel_stats.stale);
    for (i = 0; i < 15; i++) {
        q = make_vec(CDB_VEC_F32);
        same_as_cpu(&db, coll, &q);
    }
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills); /* caught up, not refilled */
    TEST_ASSERT_TRUE(db.accel_stats.certified > 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&pb.d));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

int main(void)
{
    UNITY_BEGIN();
#ifndef CDB_PROFILE_HOST
    return UNITY_END(); /* the mcu profile has no accelerator (spec 18.1) */
#endif
    RUN_TEST(test_certified_matches_cpu);
    RUN_TEST(test_exact_ties_never_widen);
    RUN_TEST(test_noise_widens_and_still_matches);
    RUN_TEST(test_mirror_follows_changes);
    RUN_TEST(test_declines_and_failures);
    RUN_TEST(test_budget_and_eviction);
    RUN_TEST(test_catch_up_across_processes);
    return UNITY_END();
}
