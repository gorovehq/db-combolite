/* Worker threads and task dispatch (spec 5.9.6, M20).
 *
 * Provides a lightweight task queue backed by a pool of worker threads.
 * Memory comes from the caller: task slots and worker thread handles are
 * sized by configuration and fixed at init (no heap allocations in the core).
 *
 * When thread hooks in cdb_env are NULL (single-threaded environments or
 * microcontrollers), or when nthreads is 0, submitted tasks execute
 * synchronously on the calling thread, providing identical behavior. */
#ifndef CDB_WORKER_H
#define CDB_WORKER_H

#include <stdint.h>
#include <stddef.h>
#include "combolite/cdb_common.h"
#include "combolite/cdb_env.h"

#define CDB_WORKER_MAX_TASKS 16u
#define CDB_WORKER_MAX_THREADS 4u

typedef struct cdb_worker_task {
    uint64_t id;
    void (*fn)(void *arg);
    void *arg;
    int done;
    cdb_status status;
} cdb_worker_task;

typedef struct cdb_worker_pool {
    cdb_env env;
    cdb_mutex *mutex;
    cdb_cond *cond_work;  /* signaled when a new task is available */
    cdb_cond *cond_done;  /* signaled when a task completes */
    cdb_thread *threads[CDB_WORKER_MAX_THREADS];
    unsigned nthreads;
    int stop;
    uint64_t next_id;
    cdb_worker_task tasks[CDB_WORKER_MAX_TASKS];
    unsigned head; /* dequeue index */
    unsigned tail; /* enqueue index */
    unsigned count;
} cdb_worker_pool;

/* Initializes a worker pool with nthreads worker threads.
 * If nthreads is 0, or if env->ops->thread_create is NULL, the pool operates
 * in synchronous fallback mode (tasks execute immediately on submit). */
cdb_status cdb_worker_pool_init(cdb_worker_pool *pool, const cdb_env *env, unsigned nthreads);

/* Submits a task to the pool. If synchronous, runs fn(arg) immediately and returns.
 * If asynchronous, enqueues the task and signals worker threads. If task_id is non-NULL,
 * receives a stable handle to wait for completion with cdb_worker_pool_wait.
 * Returns CDB_E_BUFFER_TOO_SMALL if task queue is full. */
cdb_status cdb_worker_pool_submit(cdb_worker_pool *pool, void (*fn)(void *arg), void *arg, uint64_t *task_id);

/* Waits until the specified task completes. If task_id is 0, waits until all
 * currently queued tasks complete. */
cdb_status cdb_worker_pool_wait(cdb_worker_pool *pool, uint64_t task_id);

/* Stops all worker threads and releases synchronization resources. */
void cdb_worker_pool_destroy(cdb_worker_pool *pool);

#endif
