/* Worker threads and task queue implementation (spec 5.9.6, M20). */
#include <string.h>

#include "cdb_worker.h"

static void worker_thread_loop(void *arg)
{
    cdb_worker_pool *pool = (cdb_worker_pool *)arg;
    const cdb_env *e;

    if (pool == NULL)
        return;
    e = &pool->env;
    if (e->ops == NULL || e->ops->mutex_lock == NULL || e->ops->mutex_unlock == NULL)
        return;

    (void)e->ops->mutex_lock(e->ctx, pool->mutex);
    while (!pool->stop) {
        unsigned i;
        unsigned slot = CDB_WORKER_MAX_TASKS;
        void (*fn)(void *);
        void *task_arg;

        for (i = 0; i < pool->count; i++) {
            unsigned idx = (pool->head + i) % CDB_WORKER_MAX_TASKS;
            if (!pool->tasks[idx].done && pool->tasks[idx].fn != NULL) {
                slot = idx;
                break;
            }
        }

        if (slot == CDB_WORKER_MAX_TASKS) {
            if (e->ops->cond_wait != NULL && pool->cond_work != NULL)
                (void)e->ops->cond_wait(e->ctx, pool->cond_work, pool->mutex, 0);
            continue;
        }

        fn = pool->tasks[slot].fn;
        task_arg = pool->tasks[slot].arg;
        pool->tasks[slot].fn = NULL; /* mark in-progress so other workers do not pick it */

        (void)e->ops->mutex_unlock(e->ctx, pool->mutex);
        fn(task_arg);
        (void)e->ops->mutex_lock(e->ctx, pool->mutex);

        pool->tasks[slot].done = 1;

        while (pool->count > 0 && pool->tasks[pool->head].done) {
            pool->tasks[pool->head].id = 0;
            pool->tasks[pool->head].fn = NULL;
            pool->tasks[pool->head].arg = NULL;
            pool->tasks[pool->head].done = 0;
            pool->head = (pool->head + 1u) % CDB_WORKER_MAX_TASKS;
            pool->count--;
        }

        if (e->ops->cond_broadcast != NULL && pool->cond_done != NULL)
            (void)e->ops->cond_broadcast(e->ctx, pool->cond_done);
    }
    (void)e->ops->mutex_unlock(e->ctx, pool->mutex);
}

cdb_status cdb_worker_pool_init(cdb_worker_pool *pool, const cdb_env *env, unsigned nthreads)
{
    unsigned i;
    cdb_status s;

    if (pool == NULL || env == NULL)
        return CDB_E_INVALID_ARG;

    memset(pool, 0, sizeof *pool);
    pool->env = *env;

    if (nthreads == 0 || env->ops == NULL || env->ops->thread_create == NULL ||
        env->ops->mutex_create == NULL || env->ops->cond_create == NULL) {
        pool->nthreads = 0;
        return CDB_OK;
    }

    if (nthreads > CDB_WORKER_MAX_THREADS)
        nthreads = CDB_WORKER_MAX_THREADS;

    if ((s = env->ops->mutex_create(env->ctx, &pool->mutex)) != CDB_OK)
        return s;
    if ((s = env->ops->cond_create(env->ctx, &pool->cond_work)) != CDB_OK) {
        if (env->ops->mutex_destroy != NULL)
            env->ops->mutex_destroy(env->ctx, pool->mutex);
        pool->mutex = NULL;
        return s;
    }
    if ((s = env->ops->cond_create(env->ctx, &pool->cond_done)) != CDB_OK) {
        if (env->ops->cond_destroy != NULL)
            env->ops->cond_destroy(env->ctx, pool->cond_work);
        if (env->ops->mutex_destroy != NULL)
            env->ops->mutex_destroy(env->ctx, pool->mutex);
        pool->cond_work = NULL;
        pool->mutex = NULL;
        return s;
    }

    for (i = 0; i < nthreads; i++) {
        if ((s = env->ops->thread_create(env->ctx, &pool->threads[i], worker_thread_loop, pool)) != CDB_OK) {
            unsigned j;
            pool->stop = 1;
            if (env->ops->cond_broadcast != NULL)
                env->ops->cond_broadcast(env->ctx, pool->cond_work);
            for (j = 0; j < i; j++)
                if (env->ops->thread_join != NULL)
                    env->ops->thread_join(env->ctx, pool->threads[j]);
            if (env->ops->cond_destroy != NULL) {
                env->ops->cond_destroy(env->ctx, pool->cond_done);
                env->ops->cond_destroy(env->ctx, pool->cond_work);
            }
            if (env->ops->mutex_destroy != NULL)
                env->ops->mutex_destroy(env->ctx, pool->mutex);
            memset(pool, 0, sizeof *pool);
            return s;
        }
        pool->nthreads++;
    }

    return CDB_OK;
}

cdb_status cdb_worker_pool_submit(cdb_worker_pool *pool, void (*fn)(void *arg), void *arg, uint64_t *task_id)
{
    uint64_t id;
    const cdb_env *e;

    if (pool == NULL || fn == NULL)
        return CDB_E_INVALID_ARG;

    if (pool->nthreads == 0) {
        fn(arg);
        if (task_id != NULL) {
            id = ++pool->next_id;
            if (id == 0)
                id = ++pool->next_id;
            *task_id = id;
        }
        return CDB_OK;
    }

    e = &pool->env;
    (void)e->ops->mutex_lock(e->ctx, pool->mutex);

    if (pool->count >= CDB_WORKER_MAX_TASKS) {
        (void)e->ops->mutex_unlock(e->ctx, pool->mutex);
        return CDB_E_BUFFER_TOO_SMALL;
    }

    id = ++pool->next_id;
    if (id == 0)
        id = ++pool->next_id;

    pool->tasks[pool->tail].id = id;
    pool->tasks[pool->tail].fn = fn;
    pool->tasks[pool->tail].arg = arg;
    pool->tasks[pool->tail].done = 0;
    pool->tail = (pool->tail + 1u) % CDB_WORKER_MAX_TASKS;
    pool->count++;

    if (task_id != NULL)
        *task_id = id;

    if (e->ops->cond_signal != NULL && pool->cond_work != NULL)
        (void)e->ops->cond_signal(e->ctx, pool->cond_work);

    (void)e->ops->mutex_unlock(e->ctx, pool->mutex);
    return CDB_OK;
}

cdb_status cdb_worker_pool_wait(cdb_worker_pool *pool, uint64_t task_id)
{
    const cdb_env *e;

    if (pool == NULL)
        return CDB_E_INVALID_ARG;
    if (pool->nthreads == 0)
        return CDB_OK;

    e = &pool->env;
    (void)e->ops->mutex_lock(e->ctx, pool->mutex);

    for (;;) {
        if (pool->stop)
            break;
        if (task_id == 0) {
            if (pool->count == 0)
                break;
        } else {
            unsigned i;
            int found = 0;
            for (i = 0; i < pool->count; i++) {
                unsigned idx = (pool->head + i) % CDB_WORKER_MAX_TASKS;
                if (pool->tasks[idx].id == task_id) {
                    if (!pool->tasks[idx].done)
                        found = 1;
                    break;
                }
            }
            if (!found)
                break;
        }
        if (e->ops->cond_wait != NULL && pool->cond_done != NULL)
            (void)e->ops->cond_wait(e->ctx, pool->cond_done, pool->mutex, 0);
        else
            break;
    }

    (void)e->ops->mutex_unlock(e->ctx, pool->mutex);
    return CDB_OK;
}

void cdb_worker_pool_destroy(cdb_worker_pool *pool)
{
    unsigned i;
    const cdb_env *e;

    if (pool == NULL || pool->nthreads == 0)
        return;

    e = &pool->env;
    (void)e->ops->mutex_lock(e->ctx, pool->mutex);
    pool->stop = 1;
    if (e->ops->cond_broadcast != NULL) {
        if (pool->cond_work != NULL)
            (void)e->ops->cond_broadcast(e->ctx, pool->cond_work);
        if (pool->cond_done != NULL)
            (void)e->ops->cond_broadcast(e->ctx, pool->cond_done);
    }
    (void)e->ops->mutex_unlock(e->ctx, pool->mutex);

    for (i = 0; i < pool->nthreads; i++) {
        if (pool->threads[i] != NULL && e->ops->thread_join != NULL)
            (void)e->ops->thread_join(e->ctx, pool->threads[i]);
        pool->threads[i] = NULL;
    }
    pool->nthreads = 0;

    if (pool->cond_done != NULL && e->ops->cond_destroy != NULL)
        e->ops->cond_destroy(e->ctx, pool->cond_done);
    if (pool->cond_work != NULL && e->ops->cond_destroy != NULL)
        e->ops->cond_destroy(e->ctx, pool->cond_work);
    if (pool->mutex != NULL && e->ops->mutex_destroy != NULL)
        e->ops->mutex_destroy(e->ctx, pool->mutex);

    pool->cond_done = NULL;
    pool->cond_work = NULL;
    pool->mutex = NULL;
}
