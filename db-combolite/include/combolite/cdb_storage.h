/* Storage backend interface (spec 1.11.1).
 *
 * The engine reaches storage only through these operations, so the same
 * on-disk bytes work on a host file, a raw flash partition, or the
 * fault-injecting test device. Offsets and lengths are in bytes; the layers
 * above decide page alignment. */
#ifndef COMBOLITE_CDB_STORAGE_H
#define COMBOLITE_CDB_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

typedef struct cdb_storage_geometry {
    uint32_t erase_block_size; /* 0 when the medium has no erase step (host files) */
    uint32_t program_unit;     /* smallest writable unit, in bytes */
    uint64_t capacity;         /* fixed size in bytes; 0 when the medium can grow */
} cdb_storage_geometry;

typedef struct cdb_storage_ops {
    /* Reads exactly len bytes. Reading past the end returns CDB_E_TRUNCATED. */
    cdb_status (*read)(void *ctx, uint64_t offset, void *buf, size_t len);
    /* Writes exactly len bytes. A growable medium extends as needed. */
    cdb_status (*program)(void *ctx, uint64_t offset, const void *buf, size_t len);
    /* Erases whole erase blocks. A no-op on media without an erase step. */
    cdb_status (*erase)(void *ctx, uint64_t offset, uint64_t len);
    /* Barrier: returns once every earlier program and erase is durable (spec 3). */
    cdb_status (*sync)(void *ctx);
    cdb_status (*geometry)(void *ctx, cdb_storage_geometry *out);
    /* Current size in bytes: the end of the furthest byte ever written. */
    cdb_status (*size)(void *ctx, uint64_t *out);
    void (*close)(void *ctx);
    /* Optional: shrinks the medium to size bytes; durable after the next sync.
     * NULL on media that cannot shrink (raw flash partitions). */
    cdb_status (*truncate)(void *ctx, uint64_t size);
} cdb_storage_ops;

typedef struct cdb_storage {
    const cdb_storage_ops *ops;
    void *ctx;
} cdb_storage;

static inline cdb_status cdb_storage_read(const cdb_storage *s, uint64_t offset, void *buf, size_t len)
{
    return s->ops->read(s->ctx, offset, buf, len);
}

static inline cdb_status cdb_storage_program(const cdb_storage *s, uint64_t offset, const void *buf, size_t len)
{
    return s->ops->program(s->ctx, offset, buf, len);
}

static inline cdb_status cdb_storage_erase(const cdb_storage *s, uint64_t offset, uint64_t len)
{
    return s->ops->erase(s->ctx, offset, len);
}

static inline cdb_status cdb_storage_sync(const cdb_storage *s)
{
    return s->ops->sync(s->ctx);
}

static inline cdb_status cdb_storage_geometry_of(const cdb_storage *s, cdb_storage_geometry *out)
{
    return s->ops->geometry(s->ctx, out);
}

static inline cdb_status cdb_storage_size(const cdb_storage *s, uint64_t *out)
{
    return s->ops->size(s->ctx, out);
}

static inline cdb_status cdb_storage_truncate(const cdb_storage *s, uint64_t size)
{
    return s->ops->truncate ? s->ops->truncate(s->ctx, size) : CDB_E_UNSUPPORTED;
}

static inline void cdb_storage_close(const cdb_storage *s)
{
    if (s->ops && s->ops->close)
        s->ops->close(s->ctx);
}

/* ---------------------------------------------------------- host file */

#define CDB_HOSTFILE_CREATE 0x1u   /* create the file if it does not exist */
#define CDB_HOSTFILE_READONLY 0x2u /* open read-only; writes return CDB_E_READ_ONLY */

/* Caller-owned state for one open host file. */
typedef struct cdb_hostfile {
    int fd;
    unsigned flags;
} cdb_hostfile;

/* Opens path and binds *out to it. New files are created with mode 0600,
 * since a database may hold sensitive data. */
cdb_status cdb_hostfile_open(cdb_hostfile *hf, const char *path, unsigned flags, cdb_storage *out);

#endif
