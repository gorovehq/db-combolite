/* Environment: the files and directories a database uses (spec 1.3, 1.12).
 *
 * At rest a database is one archive file. While open it also has a working
 * directory of companion files. Creating, removing or renaming a file is
 * durable only after its directory has been synced, so the engine calls
 * sync_dir explicitly wherever the spec needs that guarantee. */
#ifndef COMBOLITE_CDB_ENV_H
#define COMBOLITE_CDB_ENV_H

#include "combolite/cdb_common.h"
#include "combolite/cdb_storage.h"

#define CDB_ENV_CREATE 0x1u   /* open: create the file if it does not exist */
#define CDB_ENV_READONLY 0x2u /* open: read-only access */

/* Opaque handle types for concurrency primitives (spec 5.9.6, M20). */
typedef struct cdb_mutex cdb_mutex;
typedef struct cdb_cond cdb_cond;
typedef struct cdb_thread cdb_thread;

/* The operations an environment provides. ctx is the environment's own
 * state, passed back on every call. Paths are zero-terminated strings using
 * '/' as the separator. A missing file or directory should be reported as
 * CDB_E_NOT_FOUND, other failures as CDB_E_IO. */
typedef struct cdb_env_ops {
    /* Opens a file. The storage stays valid until it is closed. */
    cdb_status (*open)(void *ctx, const char *path, unsigned flags, cdb_storage *out);
    cdb_status (*remove)(void *ctx, const char *path);
    /* Replaces `to` if it exists; durable once the directory is synced. */
    cdb_status (*rename)(void *ctx, const char *from, const char *to);
    /* Creating a directory that already exists is not an error. */
    cdb_status (*mkdir)(void *ctx, const char *path);
    /* CDB_E_STATE: the directory is not empty, for example because a
     * companion was set aside in it (spec 1.12.2). */
    cdb_status (*rmdir)(void *ctx, const char *path);
    /* *exists is 1 if path names an existing file or directory. */
    cdb_status (*exists)(void *ctx, const char *path, int *exists);
    /* Makes every create, remove and rename inside the directory durable. */
    cdb_status (*sync_dir)(void *ctx, const char *path);
    /* Optional: the wall-clock time in nanoseconds since 1970-01-01 UTC.
     * NULL, or CDB_E_UNSUPPORTED, means the platform has no clock. */
    cdb_status (*now)(void *ctx, int64_t *ns);
    /* Optional, as a pair: memory the engine may grow into and give back,
     * such as open-store slots (cdb_db_config.max_open). alloc returns
     * memory aligned for any object, or NULL when there is none to give.
     * NULL hooks mean the engine uses only memory the caller supplied. */
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *p);
    /* The hooks below are what coordination between processes needs (spec
     * 5.9). Each is optional; without mono and sleep a database can still
     * be used, but cannot tell whether another process is alive, so it
     * treats every other coordinator as dead.
     *
     * Optional: calls fn once for each file or directory name directly in
     * dir (the name alone, without dir), in any order. A nonzero return from
     * fn stops the listing early; list then returns CDB_OK. */
    cdb_status (*list)(void *ctx, const char *dir, int (*fn)(void *arg, const char *name), void *arg);
    /* Optional: a monotonic clock in nanoseconds from an arbitrary start.
     * It never goes backwards while the process runs. */
    cdb_status (*mono)(void *ctx, int64_t *ns);
    /* Optional: fills buf with n unpredictable bytes (session identities). */
    cdb_status (*random)(void *ctx, void *buf, size_t n);
    /* Optional: waits at least ns nanoseconds. */
    void (*sleep)(void *ctx, int64_t ns);
    /* Concurrency hooks (spec 5.9.6, M20). Optional: NULL hooks mean the
     * environment is single-threaded (typical for microcontrollers). */
    /* Creates a mutex. *out must be destroyed with mutex_destroy. */
    cdb_status (*mutex_create)(void *ctx, cdb_mutex **out);
    cdb_status (*mutex_lock)(void *ctx, cdb_mutex *m);
    cdb_status (*mutex_unlock)(void *ctx, cdb_mutex *m);
    void (*mutex_destroy)(void *ctx, cdb_mutex *m);
    /* Creates a condition variable. *out must be destroyed with cond_destroy. */
    cdb_status (*cond_create)(void *ctx, cdb_cond **out);
    cdb_status (*cond_signal)(void *ctx, cdb_cond *c);
    cdb_status (*cond_broadcast)(void *ctx, cdb_cond *c);
    /* Waits on cond while unlocking m. If timeout_ns <= 0, waits indefinitely.
     * If timeout_ns > 0, waits at most timeout_ns nanoseconds.
     * Returns CDB_OK when signaled, CDB_E_TIMEOUT on expiration. */
    cdb_status (*cond_wait)(void *ctx, cdb_cond *c, cdb_mutex *m, int64_t timeout_ns);
    void (*cond_destroy)(void *ctx, cdb_cond *c);
    /* Spawns a thread running fn(arg). The thread must be joined with thread_join. */
    cdb_status (*thread_create)(void *ctx, cdb_thread **out, void (*fn)(void *arg), void *arg);
    cdb_status (*thread_join)(void *ctx, cdb_thread *t);
} cdb_env_ops;

/* ------------------------------------------------- accelerator (spec 18) */

/* A collection's vectors held on the device: the accelerator's own. */
typedef struct cdb_accel_mirror cdb_accel_mirror;

/* An optional compute accelerator, such as a GPU, for vector arithmetic
 * (spec 18.7). It keeps mirrors, device copies of collections, and answers
 * nearest-neighbour requests over them; the engine rescores every answer
 * on the CPU and checks it is complete (18.3, 18.10), so results match a
 * build without one. Any call may decline with a non-OK status; the engine
 * then uses the CPU path or treats the mirror as stale, never failing its
 * own caller. Calls on different mirrors may overlap; one mirror is never
 * called from two threads at once. Vectors are passed in their stored
 * form: vec_bytes each, elements in `order`. */
typedef struct cdb_accel_ops {
    /* A mirror for one collection, sized for `expect` vectors. elem is a
     * cdb_vector_elem and measure a CDB_VEC_* distance (cdb_vec.h). */
    cdb_status (*mirror_open)(void *ctx, uint64_t store, unsigned elem, uint32_t dims, unsigned measure,
                              cdb_byte_order order, uint64_t expect, cdb_accel_mirror **out);
    /* Inserts or replaces n vectors: ids[i] is the i-th of vecs. */
    cdb_status (*mirror_put)(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, const void *vecs,
                             size_t vec_bytes, size_t n);
    cdb_status (*mirror_del)(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, size_t n);
    void (*mirror_close)(void *ctx, cdb_accel_mirror *m);
    /* For each of nq queries (vec_bytes each, as stored), up to `want`
     * candidates nearest first, into ids and dists (nq * want each); counts[i]
     * is how many. bounds[i] bounds how far any distance for query i may be
     * from the CPU's (spec 18.10); 0 only for distances computed exactly,
     * whose ties must then be ordered by lower entry ID. */
    cdb_status (*nearest)(void *ctx, cdb_accel_mirror *m, const void *queries, size_t nq, size_t want,
                          uint64_t *ids, float *dists, size_t *counts, double *bounds);
} cdb_accel_ops;

/* An environment handle: an operation table plus its state, and an
 * optional accelerator with its own state (spec 18.7; NULL: none). Copied
 * by value; ctx and accel_ctx must outlive every copy. */
typedef struct cdb_env {
    const cdb_env_ops *ops;
    void *ctx;
    const cdb_accel_ops *accel;
    void *accel_ctx;
} cdb_env;

/* ------------------------------------------------------- host (POSIX) */

#define CDB_HOSTENV_MAX_FILES 16

/* Caller-owned state for the host environment: room for the files that can
 * be open at the same time. */
typedef struct cdb_hostenv {
    cdb_hostfile files[CDB_HOSTENV_MAX_FILES];
    int used[CDB_HOSTENV_MAX_FILES];
} cdb_hostenv;

/* Binds *out to the host (POSIX) environment backed by *he. *he must stay
 * valid while *out is in use. Opening more than CDB_HOSTENV_MAX_FILES files
 * at once gives CDB_E_BUFFER_TOO_SMALL. */
void cdb_hostenv_init(cdb_hostenv *he, cdb_env *out);

#endif
