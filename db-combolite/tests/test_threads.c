#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_fakeaccel.h"
#include "cdb_profile.h"
#include "cdb_vec.h"
#include "cdb_worker.h"
#include "combolite/cdb_env.h"

#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

static uint8_t doc_buf[4096];
static uint8_t vec_buf[3 * PAGE];
static uint8_t vec_w[8192];

static cdb_hostenv he;
static cdb_env env;
static char base_path[200];

static uint8_t arena1[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes1[NODES];
static cdb_free_extent free_list1[512];
static uint8_t record_buf1[1024];
static uint8_t cat_work1[16384];
static cdb_index_entry cat_entries1[512];
static uint8_t cat_page1[PAGE];

static uint8_t arena2[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes2[NODES];
static cdb_free_extent free_list2[512];
static uint8_t record_buf2[1024];
static uint8_t cat_work2[16384];
static cdb_index_entry cat_entries2[512];
static uint8_t cat_page2[PAGE];

static void cleanup_files(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s.cdbarch", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/main.0.cdb", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/main.1.cdb", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/pages.0.cdb", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/pages.1.cdb", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/coord/slot.0", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/coord/slot.1", base_path);
    unlink(p);
    snprintf(p, sizeof p, "%s.cdbwork/coord", base_path);
    rmdir(p);
    snprintf(p, sizeof p, "%s.cdbwork", base_path);
    rmdir(p);
}

void setUp(void)
{
    cdb_hostenv_init(&he, &env);
    snprintf(base_path, sizeof base_path, "%s/th_db_%ld", CDB_TEST_TMPDIR, (long)getpid());
    cleanup_files();
}

void tearDown(void)
{
    cleanup_files();
}

static void test_mutex_lifecycle(void)
{
    cdb_mutex *m = NULL;
    TEST_ASSERT_NOT_NULL(env.ops->mutex_create);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, env.ops->mutex_create(env.ctx, NULL));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));
    TEST_ASSERT_NOT_NULL(m);

    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, env.ops->mutex_lock(env.ctx, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, env.ops->mutex_unlock(env.ctx, NULL));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_lock(env.ctx, m));
    /* Recursive locking */
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_lock(env.ctx, m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_unlock(env.ctx, m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_unlock(env.ctx, m));

    env.ops->mutex_destroy(env.ctx, m);
    /* Destroy with NULL is safe */
    env.ops->mutex_destroy(env.ctx, NULL);
}

typedef struct worker_counter_args {
    cdb_env *e;
    cdb_mutex *m;
    int *counter;
    int increments;
} worker_counter_args;

static void worker_increment(void *arg)
{
    worker_counter_args *a = (worker_counter_args *)arg;
    for (int i = 0; i < a->increments; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, a->e->ops->mutex_lock(a->e->ctx, a->m));
        (*a->counter)++;
        TEST_ASSERT_EQUAL_INT(CDB_OK, a->e->ops->mutex_unlock(a->e->ctx, a->m));
    }
}

static void test_mutex_mutual_exclusion(void)
{
    cdb_mutex *m = NULL;
    int counter = 0;
    cdb_thread *t[4];
    worker_counter_args args[4];

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));

    for (int i = 0; i < 4; i++) {
        args[i].e = &env;
        args[i].m = m;
        args[i].counter = &counter;
        args[i].increments = 5000;
        TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_create(env.ctx, &t[i], worker_increment, &args[i]));
    }

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_join(env.ctx, t[i]));
    }

    TEST_ASSERT_EQUAL_INT(20000, counter);
    env.ops->mutex_destroy(env.ctx, m);
}

typedef struct cond_waiter_args {
    cdb_env *e;
    cdb_mutex *m;
    cdb_cond *c;
    int *ready;
} cond_waiter_args;

static void worker_cond_waiter(void *arg)
{
    cond_waiter_args *a = (cond_waiter_args *)arg;
    TEST_ASSERT_EQUAL_INT(CDB_OK, a->e->ops->mutex_lock(a->e->ctx, a->m));
    while (!*a->ready) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, a->e->ops->cond_wait(a->e->ctx, a->c, a->m, 0));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, a->e->ops->mutex_unlock(a->e->ctx, a->m));
}

static void test_cond_signal(void)
{
    cdb_mutex *m = NULL;
    cdb_cond *c = NULL;
    cdb_thread *t = NULL;
    int ready = 0;
    cond_waiter_args args;

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->cond_create(env.ctx, &c));

    args.e = &env;
    args.m = m;
    args.c = c;
    args.ready = &ready;

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_create(env.ctx, &t, worker_cond_waiter, &args));

    /* Sleep briefly to let worker enter cond_wait */
    env.ops->sleep(env.ctx, 20000000); /* 20 ms */

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_lock(env.ctx, m));
    ready = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->cond_signal(env.ctx, c));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_unlock(env.ctx, m));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_join(env.ctx, t));

    env.ops->cond_destroy(env.ctx, c);
    env.ops->mutex_destroy(env.ctx, m);
}

static void test_cond_broadcast(void)
{
    cdb_mutex *m = NULL;
    cdb_cond *c = NULL;
    cdb_thread *t[3];
    int ready = 0;
    cond_waiter_args args[3];

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->cond_create(env.ctx, &c));

    for (int i = 0; i < 3; i++) {
        args[i].e = &env;
        args[i].m = m;
        args[i].c = c;
        args[i].ready = &ready;
        TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_create(env.ctx, &t[i], worker_cond_waiter, &args[i]));
    }

    env.ops->sleep(env.ctx, 20000000); /* 20 ms */

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_lock(env.ctx, m));
    ready = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->cond_broadcast(env.ctx, c));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_unlock(env.ctx, m));

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->thread_join(env.ctx, t[i]));
    }

    env.ops->cond_destroy(env.ctx, c);
    env.ops->mutex_destroy(env.ctx, m);
}

static void test_cond_timedwait_timeout(void)
{
    cdb_mutex *m = NULL;
    cdb_cond *c = NULL;
    int64_t t0 = 0, t1 = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->cond_create(env.ctx, &c));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_lock(env.ctx, m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mono(env.ctx, &t0));

    /* 30 ms timeout */
    TEST_ASSERT_EQUAL_INT(CDB_E_TIMEOUT, env.ops->cond_wait(env.ctx, c, m, 30000000));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mono(env.ctx, &t1));
    TEST_ASSERT_TRUE((t1 - t0) >= 25000000); /* at least ~25 ms passed */

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_unlock(env.ctx, m));

    env.ops->cond_destroy(env.ctx, c);
    env.ops->mutex_destroy(env.ctx, m);
}

static void test_single_threaded_null_hooks(void)
{
    cdb_env st_env = env;
    cdb_env_ops st_ops = *env.ops;
    st_ops.mutex_create = NULL;
    st_ops.thread_create = NULL;
    st_env.ops = &st_ops;

    TEST_ASSERT_NULL(st_env.ops->mutex_create);
    TEST_ASSERT_NULL(st_env.ops->thread_create);
}

static cdb_status test_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    (void)ctx;
    (void)p;
    (void)d;
    return CDB_OK;
}

static cdb_header test_hdr(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    for (int i = 0; i < 16; i++)
        h.db_id[i] = (uint8_t)(0x50 + i);
    return h;
}

static void init_db_cfg(cdb_db_config *cfg, uint8_t *arena, cdb_pager_node *nodes,
                        cdb_free_extent *free_list, uint8_t *record_buf,
                        uint8_t *cat_work, cdb_index_entry *cat_entries, uint8_t *cat_page)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->pager.arena = arena;
    cfg->pager.arena_size = NODES * 2u * PAGE + 2u * PAGE;
    cfg->pager.nodes = nodes;
    cfg->pager.node_cap = NODES;
    cfg->pager.free_list = free_list;
    cfg->pager.free_cap = 512;
    cfg->record_buf = record_buf;
    cfg->record_buf_size = 1024;
    cfg->apply = test_apply;
    cfg->catalog_work.bytes = cat_work;
    cfg->catalog_work.bytes_size = 16384;
    cfg->catalog_work.entries = cat_entries;
    cfg->catalog_work.entries_cap = 512;
    cfg->catalog_page = cat_page;
    cfg->durability = CDB_DURABILITY_FULL;
}

static void test_heartbeat_thread_runs_and_stops(void)
{
    cdb_db db;
    cdb_db_config cfg;
    cdb_header h = test_hdr();

    init_db_cfg(&cfg, arena1, nodes1, free_list1, record_buf1, cat_work1, cat_entries1, cat_page1);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, base_path, &cfg, &h));
    TEST_ASSERT_EQUAL_INT(1, db.hb_running);
    TEST_ASSERT_NOT_NULL(db.mutex);
    TEST_ASSERT_NOT_NULL(db.hb_cond);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(0, db.hb_running);
    TEST_ASSERT_NULL(db.mutex);
    TEST_ASSERT_NULL(db.hb_cond);
}

static void test_idle_process_kept_alive_by_heartbeat_thread(void)
{
    cdb_db db1, db2;
    cdb_db_config cfg1, cfg2;
    cdb_header h = test_hdr();

    init_db_cfg(&cfg1, arena1, nodes1, free_list1, record_buf1, cat_work1, cat_entries1, cat_page1);
    /* Beat every 40 ms, dead after 3 intervals = 120 ms */
    cfg1.coord.beat_ns = 40000000;
    cfg1.coord.dead_after = 3;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db1, &env, base_path, &cfg1, &h));
    TEST_ASSERT_EQUAL_INT(1, db1.hb_running);
    TEST_ASSERT_EQUAL_UINT32(0, db1.coord.me.slot);

    /* Sleep for 250 ms: more than 2x the 120 ms dead timeout!
     * We do NOT call cdb_db_tick(&db1). The background heartbeat thread must
     * keep db1's coordinator slot alive on its own! */
    env.ops->sleep(env.ctx, 250000000);

    /* Open second process */
    init_db_cfg(&cfg2, arena2, nodes2, free_list2, record_buf2, cat_work2, cat_entries2, cat_page2);
    cfg2.coord.beat_ns = 40000000;
    cfg2.coord.dead_after = 3;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db2, &env, base_path, &cfg2, NULL));
    /* db2 should see db1 as live in slot 0, and db2 should get slot 1 */
    TEST_ASSERT_EQUAL_UINT32(1, db2.coord.me.slot);
    TEST_ASSERT_EQUAL_INT(1, db2.coord.peers[0].live);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db1));
}

typedef struct task_counter_ctx {
    cdb_mutex *m;
    int count;
} task_counter_ctx;

static void worker_inc_task(void *arg)
{
    task_counter_ctx *ctx = (task_counter_ctx *)arg;
    (void)env.ops->mutex_lock(env.ctx, ctx->m);
    ctx->count++;
    (void)env.ops->mutex_unlock(env.ctx, ctx->m);
}

static void test_worker_pool_dispatch_and_wait_all(void)
{
    cdb_worker_pool pool;
    task_counter_ctx ctx;
    cdb_mutex *m = NULL;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &m));
    ctx.m = m;
    ctx.count = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_init(&pool, &env, 3));
    TEST_ASSERT_EQUAL_UINT(3, pool.nthreads);

    for (i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_inc_task, &ctx, NULL));
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, 0));
    TEST_ASSERT_EQUAL_INT(10, ctx.count);

    cdb_worker_pool_destroy(&pool);
    env.ops->mutex_destroy(env.ctx, m);
}

typedef struct task_flag_ctx {
    int started;
    int done;
} task_flag_ctx;

static void worker_flag_task(void *arg)
{
    task_flag_ctx *ctx = (task_flag_ctx *)arg;
    ctx->started = 1;
    env.ops->sleep(env.ctx, 15000000); /* 15 ms */
    ctx->done = 1;
}

static void test_worker_pool_wait_individual_task(void)
{
    cdb_worker_pool pool;
    task_flag_ctx ctx1 = {0, 0};
    task_flag_ctx ctx2 = {0, 0};
    uint64_t id1 = 0, id2 = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_init(&pool, &env, 2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_flag_task, &ctx1, &id1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_flag_task, &ctx2, &id2));
    TEST_ASSERT_NOT_EQUAL(0, id1);
    TEST_ASSERT_NOT_EQUAL(0, id2);
    TEST_ASSERT_NOT_EQUAL(id1, id2);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, id1));
    TEST_ASSERT_EQUAL_INT(1, ctx1.done);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, id2));
    TEST_ASSERT_EQUAL_INT(1, ctx2.done);

    cdb_worker_pool_destroy(&pool);
}

static void worker_noop_task(void *arg)
{
    (void)arg;
    env.ops->sleep(env.ctx, 20000000); /* 20 ms */
}

static void test_worker_pool_queue_full(void)
{
    cdb_worker_pool pool;
    unsigned i;

    /* 1 worker thread so queue fills up quickly */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_init(&pool, &env, 1));

    for (i = 0; i < CDB_WORKER_MAX_TASKS; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_noop_task, NULL, NULL));
    }

    /* Queue should now be full */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_worker_pool_submit(&pool, worker_noop_task, NULL, NULL));

    /* Wait for all to complete */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, 0));

    /* After draining, submission succeeds again */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_noop_task, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, 0));

    cdb_worker_pool_destroy(&pool);
}

static void test_worker_pool_sync_fallback(void)
{
    cdb_worker_pool pool;
    task_flag_ctx ctx = {0, 0};
    uint64_t tid = 0;

    /* Pool with 0 threads: operates synchronously */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_init(&pool, &env, 0));
    TEST_ASSERT_EQUAL_UINT(0, pool.nthreads);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&pool, worker_flag_task, &ctx, &tid));
    TEST_ASSERT_EQUAL_INT(1, ctx.done);
    TEST_ASSERT_NOT_EQUAL(0, tid);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, tid));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&pool, 0));

    cdb_worker_pool_destroy(&pool);
}

static void test_db_worker_threads_and_vec_refresh_async(void)
{
    static cdb_db db;
    cdb_db_config cfg;
    cdb_header h = test_hdr();
    cdb_vec_config vc;
    uint64_t vcoll = 0;
    cdb_vec_refresh_task rtask;
    uint64_t rtask_id = 0;
    float vdata[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    uint64_t eid = 0;
    int i;

    init_db_cfg(&cfg, arena1, nodes1, free_list1, record_buf1, cat_work1, cat_entries1, cat_page1);
    cfg.apply = NULL;
    cfg.doc_buffers = doc_buf;
    cfg.vec_buffers = vec_buf;
    cfg.vec_work = vec_w;
    cfg.vec_work_size = sizeof vec_w;
    cfg.worker_threads = 2;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, base_path, &cfg, &h));
    TEST_ASSERT_EQUAL_UINT(2, db.workers.nthreads);

    memset(&vc, 0, sizeof vc);
    vc.elem = CDB_VEC_F32;
    vc.dims = 4;
    vc.measure = CDB_VEC_EUCLIDEAN;
    vc.model = "test";
    vc.model_len = 4;
    vc.model_version = "1";
    vc.model_version_len = 1;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "test_vecs", 9, &vc, &vcoll));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create_graph(&db, vcoll, 2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (i = 0; i < 6; i++) {
        uint8_t enc[64];
        cdb_value_writer w;
        vdata[0] = (float)i;
        cdb_value_writer_init(&w, enc, sizeof enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_vector(&w, CDB_VEC_F32, 4, vdata));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, vcoll, enc, cdb_value_writer_len(&w), 0, 0, &eid));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    memset(&rtask, 0, sizeof rtask);
    rtask.store = vcoll;
    rtask.budget = 10;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_refresh_async(&db, &rtask, &rtask_id));
    TEST_ASSERT_NOT_EQUAL(0, rtask_id);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&db.workers, rtask_id));
    TEST_ASSERT_EQUAL_INT(CDB_OK, rtask.status);
    TEST_ASSERT_EQUAL_UINT(0, rtask.remaining);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_UINT(0, db.workers.nthreads);
}

/* A worker task that holds the database's lock, as a background refresh or
 * mirror refill does. */
typedef struct lock_holder {
    cdb_db *db;
    cdb_mutex *m;   /* guards started */
    int started;
    int finished;   /* written under the database lock */
} lock_holder;

static void hold_lock(void *arg)
{
    lock_holder *h = (lock_holder *)arg;
    cdb_db_lock(h->db);
    env.ops->mutex_lock(env.ctx, h->m);
    h->started = 1;
    env.ops->mutex_unlock(env.ctx, h->m);
    env.ops->sleep(env.ctx, 150000000); /* 150 ms */
    h->finished = 1;
    cdb_db_unlock(h->db);
}

/* Every public data call takes the database's lock, so one made while a
 * worker task holds it waits for the task (spec 5.9.6). */
static void test_worker_task_excludes_data_calls(void)
{
    static cdb_db db;
    static lock_holder h;
    cdb_db_config cfg;
    cdb_header hd = test_hdr();
    uint64_t task = 0, id = 0;
    int found = 1, started = 0;

    init_db_cfg(&cfg, arena1, nodes1, free_list1, record_buf1, cat_work1, cat_entries1, cat_page1);
    cfg.worker_threads = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, base_path, &cfg, &hd));
    TEST_ASSERT_EQUAL_UINT(1, db.workers.nthreads);
    memset(&h, 0, sizeof h);
    h.db = &db;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &h.m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&db.workers, hold_lock, &h, &task));
    while (!started) {
        env.ops->mutex_lock(env.ctx, h.m);
        started = h.started;
        env.ops->mutex_unlock(env.ctx, h.m);
        if (!started)
            env.ops->sleep(env.ctx, 1000000);
    }
    /* The task holds the lock now: this call must wait for it. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "nothing", 7, &id, &found));
    TEST_ASSERT_EQUAL_INT(1, h.finished);
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&db.workers, task));
    env.ops->mutex_destroy(env.ctx, h.m);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Spec 18.6: with worker threads, a mirror is filled on the pool a chunk
 * at a time, and the application's calls run between chunks: the main
 * thread sees the mirror part-filled at least once. Then searches use it
 * and match the CPU path. */
#define BIG_PAGE 4096u
#define BIG_NODES 128u
static uint8_t big_arena[BIG_NODES * 2u * BIG_PAGE + 2u * BIG_PAGE];
static cdb_pager_node big_nodes[BIG_NODES];
static cdb_free_extent big_free[4096];
static uint8_t big_record[2u * BIG_PAGE];
static uint8_t big_cat_work[256u * 1024u];
static cdb_index_entry big_cat_entries[4096];
static uint8_t big_cat_page[BIG_PAGE];
static uint8_t big_doc[8u * BIG_PAGE];
static uint8_t big_vec[3u * BIG_PAGE];

static void test_worker_refills_in_chunks(void)
{
    static cdb_db db;
    static cdb_fakeaccel fake;
    cdb_db_config cfg;
    cdb_header h = test_hdr();
    cdb_vec_config vc;
    cdb_vec_hit got[5], want[5];
    uint64_t vcoll = 0, eid = 0, id = 0;
    uint8_t enc[64];
    size_t qlen = 0, ng, nw, i;
    float v[4];
    int found, seen_filling = 0, ready = 0, k;

#ifndef CDB_PROFILE_HOST
    TEST_IGNORE_MESSAGE("the mcu profile has no accelerator (spec 18.1)");
#endif
    /* Larger than the other tests' buffers: 5000 vectors on 4 KB pages. */
    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = big_arena;
    cfg.pager.arena_size = sizeof big_arena;
    cfg.pager.nodes = big_nodes;
    cfg.pager.node_cap = BIG_NODES;
    cfg.pager.free_list = big_free;
    cfg.pager.free_cap = sizeof big_free / sizeof big_free[0];
    cfg.record_buf = big_record;
    cfg.record_buf_size = sizeof big_record;
    cfg.catalog_work.bytes = big_cat_work;
    cfg.catalog_work.bytes_size = cdb_index_work_size(BIG_PAGE);
    cfg.catalog_work.entries = big_cat_entries;
    cfg.catalog_work.entries_cap = cdb_index_entries_needed(BIG_PAGE);
    TEST_ASSERT_TRUE(cfg.catalog_work.bytes_size <= sizeof big_cat_work);
    TEST_ASSERT_TRUE(cfg.catalog_work.entries_cap <= sizeof big_cat_entries / sizeof big_cat_entries[0]);
    cfg.catalog_page = big_cat_page;
    cfg.doc_buffers = big_doc;
    cfg.vec_buffers = big_vec;
    cfg.durability = CDB_DURABILITY_NORMAL;
    cfg.accel_min_bytes = 1; /* small test collections still reach the accelerator (spec 18.4) */
    cfg.worker_threads = 1;
    h.default_page_log2 = 12u;
    cdb_fakeaccel_init(&fake, &env);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, base_path, &cfg, &h));
    memset(&vc, 0, sizeof vc);
    vc.elem = CDB_VEC_F32;
    vc.dims = 4;
    vc.measure = CDB_VEC_EUCLIDEAN;
    vc.model = "test";
    vc.model_len = 4;
    vc.model_version = "1";
    vc.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "chunks", 6, &vc, &vcoll));
    for (k = 0; k < 5000; k++) {
        cdb_value_writer w;
        if (k % 100 == 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        v[0] = (float)(k % 97);
        v[1] = (float)(k % 89);
        v[2] = (float)(k % 83);
        v[3] = (float)(k % 7);
        cdb_value_writer_init(&w, enc, sizeof enc, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_vector(&w, CDB_VEC_F32, 4, v));
        qlen = cdb_value_writer_len(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, vcoll, enc, qlen, 0, 0, &eid));
        if (k % 100 == 99)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); /* this fixture's pager is small */
        if (k % 1000 == 999)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    }

    /* The first search asks for the mirror: a fill is queued on the pool. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, vcoll, enc, qlen, CDB_VEC_BASE, got, 5, &ng));
    for (k = 0; k < 20000 && !ready; k++) {
        int state;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_find_store(&db, "x", 1, &id, &found)); /* a data call */
        cdb_db_lock(&db);
        state = db.accel[0].state;
        cdb_db_unlock(&db);
        seen_filling |= state == CDB_ACCEL_FILLING;
        ready = state == CDB_ACCEL_READY;
        if (!ready)
            env.ops->sleep(env.ctx, 100000); /* 0.1 ms */
    }
    TEST_ASSERT_TRUE(ready);
    TEST_ASSERT_TRUE(seen_filling); /* a call ran while the mirror was part-filled */
    TEST_ASSERT_EQUAL_UINT64(1, db.accel_stats.fills);
    TEST_ASSERT_TRUE(fake.puts >= 5000u / 256u); /* in chunks */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, vcoll, enc, qlen, CDB_VEC_BASE, got, 5, &ng));
    db.env.accel = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search(&db, vcoll, enc, qlen, CDB_VEC_BASE, want, 5, &nw));
    db.env.accel = env.accel;
    TEST_ASSERT_EQUAL_size_t(nw, ng);
    for (i = 0; i < nw; i++) {
        TEST_ASSERT_EQUAL_UINT64(want[i].entry, got[i].entry);
        TEST_ASSERT_TRUE(want[i].distance == got[i].distance);
    }
    TEST_ASSERT_TRUE(db.accel_stats.certified >= 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_UINT(0, fake.live);
    env.accel = NULL; /* the other tests run without it */
    env.accel_ctx = NULL;
}

/* A worker task making data calls until told to stop, counting those that
 * finished while a bulk search was running. */
typedef struct bulk_watch {
    cdb_db *db;
    cdb_mutex *m; /* guards the rest */
    int started, running, stop, during;
} bulk_watch;

static void call_until_stopped(void *arg)
{
    bulk_watch *w = (bulk_watch *)arg;
    for (;;) {
        uint64_t id = 0;
        int found = 0, stop;
        (void)cdb_db_find_store(w->db, "x", 1, &id, &found); /* a data call: takes the lock */
        env.ops->mutex_lock(env.ctx, w->m);
        w->started = 1;
        w->during += w->running;
        stop = w->stop;
        env.ops->mutex_unlock(env.ctx, w->m);
        if (stop)
            return;
        env.ops->sleep(env.ctx, 10000); /* 10 us */
    }
}

/* Spec 18.6, 18.4: a bulk search lets go of the database's lock between
 * batches of queries, so another thread's calls run before it finishes. */
static void test_bulk_search_lets_others_in(void)
{
    enum { NQ = 3200 }; /* 50 batches */
    static cdb_db db;
    static bulk_watch w;
    static uint8_t qenc[NQ][64];
    static const void *qp[NQ];
    static size_t ql[NQ], counts[NQ];
    static cdb_vec_hit hits[NQ * 5];
    cdb_db_config cfg;
    cdb_header h = test_hdr();
    cdb_vec_config vc;
    uint64_t vcoll = 0, eid = 0, task = 0;
    float v[4];
    int k, started = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = big_arena;
    cfg.pager.arena_size = sizeof big_arena;
    cfg.pager.nodes = big_nodes;
    cfg.pager.node_cap = BIG_NODES;
    cfg.pager.free_list = big_free;
    cfg.pager.free_cap = sizeof big_free / sizeof big_free[0];
    cfg.record_buf = big_record;
    cfg.record_buf_size = sizeof big_record;
    cfg.catalog_work.bytes = big_cat_work;
    cfg.catalog_work.bytes_size = cdb_index_work_size(BIG_PAGE);
    cfg.catalog_work.entries = big_cat_entries;
    cfg.catalog_work.entries_cap = cdb_index_entries_needed(BIG_PAGE);
    cfg.catalog_page = big_cat_page;
    cfg.doc_buffers = big_doc;
    cfg.vec_buffers = big_vec;
    cfg.durability = CDB_DURABILITY_NORMAL;
    cfg.worker_threads = 1;
    h.default_page_log2 = 12u;
    env.accel = NULL; /* the CPU path: the slow case */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, base_path, &cfg, &h));
    memset(&vc, 0, sizeof vc);
    vc.elem = CDB_VEC_F32;
    vc.dims = 4;
    vc.measure = CDB_VEC_EUCLIDEAN;
    vc.model = "test";
    vc.model_len = 4;
    vc.model_version = "1";
    vc.model_version_len = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_create(&db, "bulk", 4, &vc, &vcoll));
    for (k = 0; k < 5000; k++) {
        cdb_value_writer wr;
        if (k % 100 == 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT));
        v[0] = (float)(k % 97);
        v[1] = (float)(k % 89);
        v[2] = (float)(k % 83);
        v[3] = (float)(k % 7);
        cdb_value_writer_init(&wr, qenc[k % NQ], sizeof qenc[0], CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_vector(&wr, CDB_VEC_F32, 4, v));
        ql[k % NQ] = cdb_value_writer_len(&wr);
        qp[k % NQ] = qenc[k % NQ];
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_insert(&db, vcoll, qenc[k % NQ], ql[k % NQ], 0, 0, &eid));
        if (k % 100 == 99)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    }

    memset(&w, 0, sizeof w);
    w.db = &db;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mutex_create(env.ctx, &w.m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_submit(&db.workers, call_until_stopped, &w, &task));
    while (!started) {
        env.ops->mutex_lock(env.ctx, w.m);
        started = w.started;
        env.ops->mutex_unlock(env.ctx, w.m);
        if (!started)
            env.ops->sleep(env.ctx, 100000);
    }
    env.ops->mutex_lock(env.ctx, w.m);
    w.running = 1;
    env.ops->mutex_unlock(env.ctx, w.m);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_vec_search_bulk(&db, vcoll, qp, ql, NQ, CDB_VEC_BASE, CDB_VEC_AUTO, hits, 5,
                                                      counts));
    env.ops->mutex_lock(env.ctx, w.m);
    w.running = 0;
    w.stop = 1;
    env.ops->mutex_unlock(env.ctx, w.m);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_worker_pool_wait(&db.workers, task));
    {
        char msg[64];
        snprintf(msg, sizeof msg, "%d calls ran during the bulk search", w.during);
        TEST_MESSAGE(msg);
    }
    TEST_ASSERT_TRUE(w.during >= 5);
    TEST_ASSERT_EQUAL_size_t(5, counts[NQ - 1]);
    env.ops->mutex_destroy(env.ctx, w.m);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mutex_lifecycle);
    RUN_TEST(test_mutex_mutual_exclusion);
    RUN_TEST(test_cond_signal);
    RUN_TEST(test_cond_broadcast);
    RUN_TEST(test_cond_timedwait_timeout);
    RUN_TEST(test_single_threaded_null_hooks);
    RUN_TEST(test_heartbeat_thread_runs_and_stops);
    RUN_TEST(test_idle_process_kept_alive_by_heartbeat_thread);
    RUN_TEST(test_worker_pool_dispatch_and_wait_all);
    RUN_TEST(test_worker_pool_wait_individual_task);
    RUN_TEST(test_worker_pool_queue_full);
    RUN_TEST(test_worker_pool_sync_fallback);
    RUN_TEST(test_db_worker_threads_and_vec_refresh_async);
    RUN_TEST(test_worker_task_excludes_data_calls);
    RUN_TEST(test_worker_refills_in_chunks);
    RUN_TEST(test_bulk_search_lets_others_in);
    return UNITY_END();
}
