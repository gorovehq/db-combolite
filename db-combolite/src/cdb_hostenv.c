/* Host environment over POSIX files and directories.
 *
 * Each operation is a thin wrapper over the POSIX call, mapping errno to a
 * status: ENOENT to CDB_E_NOT_FOUND, anything else to CDB_E_IO (rmdir adds
 * CDB_E_STATE for "not empty"). This is outside the core, so it may use the
 * C library's malloc for the optional alloc hooks. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "combolite/cdb_env.h"

static cdb_status errno_status(void)
{
    return errno == ENOENT ? CDB_E_NOT_FOUND : CDB_E_IO;
}

/* Opens path into a free cdb_hostfile slot of the caller's cdb_hostenv. */
static cdb_status he_open(void *ctx, const char *path, unsigned flags, cdb_storage *out)
{
    cdb_hostenv *he = (cdb_hostenv *)ctx;
    unsigned hf_flags = 0;
    int i;
    cdb_status s;

    /* A slot is free once its file has been closed (cdb_storage_close). */
    for (i = 0; i < CDB_HOSTENV_MAX_FILES; i++)
        if (!he->used[i] || he->files[i].fd < 0)
            break;
    if (i == CDB_HOSTENV_MAX_FILES)
        return CDB_E_BUFFER_TOO_SMALL;
    if (flags & CDB_ENV_CREATE)
        hf_flags |= CDB_HOSTFILE_CREATE;
    if (flags & CDB_ENV_READONLY)
        hf_flags |= CDB_HOSTFILE_READONLY;
    if ((s = cdb_hostfile_open(&he->files[i], path, hf_flags, out)) != CDB_OK)
        return s;
    he->used[i] = 1;
    return CDB_OK;
}

static cdb_status he_remove(void *ctx, const char *path)
{
    (void)ctx;
    return unlink(path) == 0 ? CDB_OK : errno_status();
}

static cdb_status he_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return rename(from, to) == 0 ? CDB_OK : errno_status();
}

/* Owner-only permissions, like the files (a database may hold sensitive
 * data). An existing directory counts as success. */
static cdb_status he_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    if (mkdir(path, S_IRWXU) == 0)
        return CDB_OK;
    return errno == EEXIST ? CDB_OK : errno_status();
}

static cdb_status he_rmdir(void *ctx, const char *path)
{
    (void)ctx;
    if (rmdir(path) == 0)
        return CDB_OK;
    if (errno == ENOTEMPTY || errno == EEXIST)
        return CDB_E_STATE; /* POSIX allows either for a directory that is not empty */
    return errno_status();
}

static cdb_status he_exists(void *ctx, const char *path, int *exists)
{
    struct stat st;
    (void)ctx;
    if (stat(path, &st) == 0) {
        *exists = 1;
        return CDB_OK;
    }
    if (errno == ENOENT) {
        *exists = 0;
        return CDB_OK;
    }
    return CDB_E_IO;
}

/* fsync on the directory itself makes its entries durable. */
static cdb_status he_sync_dir(void *ctx, const char *path)
{
    int fd;
    cdb_status s = CDB_OK;
    (void)ctx;
    do {
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno_status();
    while (fsync(fd) != 0) {
        if (errno != EINTR) {
            s = CDB_E_IO;
            break;
        }
    }
    close(fd);
    return s;
}

/* Wall-clock time (CLOCK_REALTIME) in ns since the Unix epoch. */
static cdb_status he_now(void *ctx, int64_t *ns)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return CDB_E_UNSUPPORTED;
    *ns = (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
    return CDB_OK;
}

static void *he_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void he_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

/* readdir, skipping "." and "..". */
static cdb_status he_list(void *ctx, const char *dir, int (*fn)(void *arg, const char *name), void *arg)
{
    DIR *d;
    struct dirent *e;
    (void)ctx;
    if ((d = opendir(dir)) == NULL)
        return errno_status();
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(arg, e->d_name))
            break;
    }
    closedir(d);
    return CDB_OK;
}

static cdb_status he_mono(void *ctx, int64_t *ns)
{
    struct timespec ts;
    (void)ctx;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return CDB_E_UNSUPPORTED;
    *ns = (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
    return CDB_OK;
}

/* /dev/urandom: POSIX enough, and no feature macros beyond the ones above. */
static cdb_status he_random(void *ctx, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    int fd;
    (void)ctx;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return CDB_E_UNSUPPORTED;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            close(fd);
            return CDB_E_IO;
        }
        p += r;
        n -= (size_t)r;
    }
    close(fd);
    return CDB_OK;
}

static void he_sleep(void *ctx, int64_t ns)
{
    struct timespec ts;
    (void)ctx;
    if (ns <= 0)
        return;
    ts.tv_sec = (time_t)(ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

struct cdb_mutex {
    pthread_mutex_t m;
};

struct cdb_cond {
    pthread_cond_t c;
};

struct cdb_thread {
    pthread_t thread;
    void (*fn)(void *arg);
    void *arg;
};

static cdb_status he_mutex_create(void *ctx, cdb_mutex **out)
{
    cdb_mutex *m;
    pthread_mutexattr_t attr;
    (void)ctx;
    if (out == NULL)
        return CDB_E_INVALID_ARG;
    m = (cdb_mutex *)malloc(sizeof *m);
    if (m == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    if (pthread_mutexattr_init(&attr) != 0) {
        free(m);
        return CDB_E_IO;
    }
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return CDB_E_IO;
    }
    if (pthread_mutex_init(&m->m, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return CDB_E_IO;
    }
    pthread_mutexattr_destroy(&attr);
    *out = m;
    return CDB_OK;
}

static cdb_status he_mutex_lock(void *ctx, cdb_mutex *m)
{
    (void)ctx;
    if (m == NULL)
        return CDB_E_INVALID_ARG;
    return pthread_mutex_lock(&m->m) == 0 ? CDB_OK : CDB_E_IO;
}

static cdb_status he_mutex_unlock(void *ctx, cdb_mutex *m)
{
    (void)ctx;
    if (m == NULL)
        return CDB_E_INVALID_ARG;
    return pthread_mutex_unlock(&m->m) == 0 ? CDB_OK : CDB_E_IO;
}

static void he_mutex_destroy(void *ctx, cdb_mutex *m)
{
    (void)ctx;
    if (m == NULL)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

static cdb_status he_cond_create(void *ctx, cdb_cond **out)
{
    cdb_cond *c;
    pthread_condattr_t attr;
    (void)ctx;
    if (out == NULL)
        return CDB_E_INVALID_ARG;
    c = (cdb_cond *)malloc(sizeof *c);
    if (c == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    if (pthread_condattr_init(&attr) != 0) {
        free(c);
        return CDB_E_IO;
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        free(c);
        return CDB_E_IO;
    }
    if (pthread_cond_init(&c->c, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        free(c);
        return CDB_E_IO;
    }
    pthread_condattr_destroy(&attr);
    *out = c;
    return CDB_OK;
}

static cdb_status he_cond_signal(void *ctx, cdb_cond *c)
{
    (void)ctx;
    if (c == NULL)
        return CDB_E_INVALID_ARG;
    return pthread_cond_signal(&c->c) == 0 ? CDB_OK : CDB_E_IO;
}

static cdb_status he_cond_broadcast(void *ctx, cdb_cond *c)
{
    (void)ctx;
    if (c == NULL)
        return CDB_E_INVALID_ARG;
    return pthread_cond_broadcast(&c->c) == 0 ? CDB_OK : CDB_E_IO;
}

static cdb_status he_cond_wait(void *ctx, cdb_cond *c, cdb_mutex *m, int64_t timeout_ns)
{
    (void)ctx;
    if (c == NULL || m == NULL)
        return CDB_E_INVALID_ARG;
    if (timeout_ns <= 0)
        return pthread_cond_wait(&c->c, &m->m) == 0 ? CDB_OK : CDB_E_IO;
    {
        struct timespec ts;
        int rc;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
            return CDB_E_UNSUPPORTED;
        ts.tv_sec += (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec += (long)(timeout_ns % 1000000000);
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        rc = pthread_cond_timedwait(&c->c, &m->m, &ts);
        if (rc == ETIMEDOUT)
            return CDB_E_TIMEOUT;
        return rc == 0 ? CDB_OK : CDB_E_IO;
    }
}

static void he_cond_destroy(void *ctx, cdb_cond *c)
{
    (void)ctx;
    if (c == NULL)
        return;
    pthread_cond_destroy(&c->c);
    free(c);
}

static void *he_thread_trampoline(void *arg)
{
    cdb_thread *t = (cdb_thread *)arg;
    t->fn(t->arg);
    return NULL;
}

static cdb_status he_thread_create(void *ctx, cdb_thread **out, void (*fn)(void *arg), void *arg)
{
    cdb_thread *t;
    (void)ctx;
    if (out == NULL || fn == NULL)
        return CDB_E_INVALID_ARG;
    t = (cdb_thread *)malloc(sizeof *t);
    if (t == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->thread, NULL, he_thread_trampoline, t) != 0) {
        free(t);
        return CDB_E_IO;
    }
    *out = t;
    return CDB_OK;
}

static cdb_status he_thread_join(void *ctx, cdb_thread *t)
{
    int rc;
    (void)ctx;
    if (t == NULL)
        return CDB_E_INVALID_ARG;
    rc = pthread_join(t->thread, NULL);
    free(t);
    return rc == 0 ? CDB_OK : CDB_E_IO;
}

static const cdb_env_ops hostenv_ops = {
    he_open, he_remove, he_rename, he_mkdir, he_rmdir, he_exists, he_sync_dir, he_now, he_alloc, he_free,
    he_list, he_mono, he_random, he_sleep,
    he_mutex_create, he_mutex_lock, he_mutex_unlock, he_mutex_destroy,
    he_cond_create, he_cond_signal, he_cond_broadcast, he_cond_wait, he_cond_destroy,
    he_thread_create, he_thread_join
};

void cdb_hostenv_init(cdb_hostenv *he, cdb_env *out)
{
    memset(he, 0, sizeof *he);
    out->ops = &hostenv_ops;
    out->ctx = he;
}
