#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_env.h"

static cdb_hostenv he;
static cdb_env env;

void setUp(void)
{
    cdb_hostenv_init(&he, &env);
}

void tearDown(void)
{
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mutex_lifecycle);
    RUN_TEST(test_mutex_mutual_exclusion);
    RUN_TEST(test_cond_signal);
    RUN_TEST(test_cond_broadcast);
    RUN_TEST(test_cond_timedwait_timeout);
    RUN_TEST(test_single_threaded_null_hooks);
    return UNITY_END();
}
