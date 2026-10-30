/* Host environment over POSIX files and directories. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
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

static const cdb_env_ops hostenv_ops = {
    he_open, he_remove, he_rename, he_mkdir, he_rmdir, he_exists, he_sync_dir, he_now, he_alloc, he_free
};

void cdb_hostenv_init(cdb_hostenv *he, cdb_env *out)
{
    memset(he, 0, sizeof *he);
    out->ops = &hostenv_ops;
    out->ctx = he;
}
