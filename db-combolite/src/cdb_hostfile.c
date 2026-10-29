/* Host-file storage backend (spec 1.11.1), using POSIX positional I/O. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "combolite/cdb_storage.h"

static cdb_status hf_errno_status(void)
{
    return errno == ENOENT ? CDB_E_NOT_FOUND : CDB_E_IO;
}

/* off_t is signed; reject offsets it cannot represent. */
static int hf_offset_ok(uint64_t offset, size_t len)
{
    const uint64_t max = (uint64_t)INT64_MAX;
    return offset <= max && (uint64_t)len <= max - offset;
}

static cdb_status hf_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const cdb_hostfile *hf = (const cdb_hostfile *)ctx;
    uint8_t *p = (uint8_t *)buf;

    if (!hf_offset_ok(offset, len))
        return CDB_E_INVALID_ARG;
    while (len > 0) {
        ssize_t n = pread(hf->fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return CDB_E_IO;
        }
        if (n == 0)
            return CDB_E_TRUNCATED;
        p += n;
        len -= (size_t)n;
        offset += (uint64_t)n;
    }
    return CDB_OK;
}

static cdb_status hf_program(void *ctx, uint64_t offset, const void *buf, size_t len)
{
    const cdb_hostfile *hf = (const cdb_hostfile *)ctx;
    const uint8_t *p = (const uint8_t *)buf;

    if (hf->flags & CDB_HOSTFILE_READONLY)
        return CDB_E_READ_ONLY;
    if (!hf_offset_ok(offset, len))
        return CDB_E_INVALID_ARG;
    while (len > 0) {
        ssize_t n = pwrite(hf->fd, p, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return CDB_E_IO;
        }
        p += n;
        len -= (size_t)n;
        offset += (uint64_t)n;
    }
    return CDB_OK;
}

/* Host files have no erase step; the bytes are simply overwritten later. */
static cdb_status hf_erase(void *ctx, uint64_t offset, uint64_t len)
{
    const cdb_hostfile *hf = (const cdb_hostfile *)ctx;
    (void)offset;
    (void)len;
    return (hf->flags & CDB_HOSTFILE_READONLY) ? CDB_E_READ_ONLY : CDB_OK;
}

static cdb_status hf_sync(void *ctx)
{
    const cdb_hostfile *hf = (const cdb_hostfile *)ctx;
    if (hf->flags & CDB_HOSTFILE_READONLY)
        return CDB_OK;
    while (fsync(hf->fd) != 0) {
        if (errno != EINTR)
            return CDB_E_IO;
    }
    return CDB_OK;
}

static cdb_status hf_geometry(void *ctx, cdb_storage_geometry *out)
{
    (void)ctx;
    out->erase_block_size = 0;
    out->program_unit = 1;
    out->capacity = 0;
    return CDB_OK;
}

static cdb_status hf_size(void *ctx, uint64_t *out)
{
    const cdb_hostfile *hf = (const cdb_hostfile *)ctx;
    struct stat st;
    if (fstat(hf->fd, &st) != 0)
        return CDB_E_IO;
    *out = (uint64_t)st.st_size;
    return CDB_OK;
}

static void hf_close(void *ctx)
{
    cdb_hostfile *hf = (cdb_hostfile *)ctx;
    if (hf->fd >= 0) {
        close(hf->fd);
        hf->fd = -1;
    }
}

static const cdb_storage_ops hostfile_ops = {
    hf_read, hf_program, hf_erase, hf_sync, hf_geometry, hf_size, hf_close
};

cdb_status cdb_hostfile_open(cdb_hostfile *hf, const char *path, unsigned flags, cdb_storage *out)
{
    int oflags;

    if (hf == NULL || path == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    if ((flags & CDB_HOSTFILE_READONLY) && (flags & CDB_HOSTFILE_CREATE))
        return CDB_E_INVALID_ARG;
    oflags = (flags & CDB_HOSTFILE_READONLY) ? O_RDONLY : O_RDWR;
    if (flags & CDB_HOSTFILE_CREATE)
        oflags |= O_CREAT;
    oflags |= O_CLOEXEC;

    do {
        hf->fd = open(path, oflags, S_IRUSR | S_IWUSR);
    } while (hf->fd < 0 && errno == EINTR);
    if (hf->fd < 0)
        return hf_errno_status();
    hf->flags = flags;
    out->ops = &hostfile_ops;
    out->ctx = hf;
    return CDB_OK;
}
