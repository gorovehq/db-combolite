#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "cdb_db.h"
#include "cdb_doc.h"
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
    return UNITY_END();
}
