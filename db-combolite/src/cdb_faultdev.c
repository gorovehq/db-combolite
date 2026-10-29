#include <string.h>

#include "cdb_faultdev.h"

static int in_bounds(const cdb_faultdev *d, uint64_t offset, uint64_t len)
{
    return offset <= d->cfg.capacity && len <= d->cfg.capacity - offset;
}

int cdb_fault_clock_tick(cdb_fault_clock *clock)
{
    clock->ops++;
    if (clock->crash_at != 0 && clock->ops >= clock->crash_at) {
        clock->crashed = 1;
        return 1;
    }
    return 0;
}

static int is_crashed(const cdb_faultdev *d)
{
    return d->cfg.clock ? d->cfg.clock->crashed : d->crashed;
}

/* Counts a mutating operation. Returns 1 if the armed crash strikes it. */
static int count_op(cdb_faultdev *d)
{
    d->mutating_ops++;
    if (d->cfg.clock)
        return cdb_fault_clock_tick(d->cfg.clock);
    if (d->crash_at != 0 && d->mutating_ops >= d->crash_at) {
        d->crashed = 1;
        return 1;
    }
    return 0;
}

static cdb_status log_write(cdb_faultdev *d, uint64_t offset, const void *data, size_t len, int is_erase)
{
    cdb_fault_record *r;
    if (d->log_len >= d->cfg.log_cap)
        return CDB_E_BUFFER_TOO_SMALL;
    if (!is_erase && len > d->cfg.log_data_cap - d->log_data_len)
        return CDB_E_BUFFER_TOO_SMALL;
    r = &d->cfg.log[d->log_len++];
    r->offset = offset;
    r->len = len;
    r->is_erase = is_erase;
    r->data_off = d->log_data_len;
    if (!is_erase) {
        memcpy(d->cfg.log_data + d->log_data_len, data, len);
        d->log_data_len += len;
    }
    return CDB_OK;
}

static cdb_status fd_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const cdb_faultdev *d = (const cdb_faultdev *)ctx;
    if (is_crashed(d))
        return CDB_E_IO;
    if (offset > d->current_size || len > d->current_size - offset)
        return CDB_E_TRUNCATED;
    memcpy(buf, d->cfg.current + offset, len);
    return CDB_OK;
}

static cdb_status fd_program(void *ctx, uint64_t offset, const void *buf, size_t len)
{
    cdb_faultdev *d = (cdb_faultdev *)ctx;
    const uint8_t *src = (const uint8_t *)buf;
    cdb_status s;
    size_t i;

    if (is_crashed(d))
        return CDB_E_IO;
    if (!in_bounds(d, offset, len))
        return CDB_E_IO; /* device full */
    if (d->cfg.flags & CDB_FAULTDEV_FLASH) {
        for (i = 0; i < len; i++)
            if ((d->cfg.current[offset + i] & src[i]) != src[i])
                return CDB_E_INVALID_ARG; /* would set a bit without an erase */
    }
    /* The write is logged even if the crash strikes it: it was in flight. */
    if ((s = log_write(d, offset, src, len, 0)) != CDB_OK)
        return s;
    if (count_op(d))
        return CDB_E_IO;
    memcpy(d->cfg.current + offset, src, len);
    if (offset + len > d->current_size)
        d->current_size = offset + len;
    return CDB_OK;
}

static cdb_status fd_erase(void *ctx, uint64_t offset, uint64_t len)
{
    cdb_faultdev *d = (cdb_faultdev *)ctx;
    uint32_t block = d->cfg.geometry.erase_block_size;
    cdb_status s;

    if (is_crashed(d))
        return CDB_E_IO;
    if (block == 0)
        return CDB_OK;
    if (offset % block != 0 || len % block != 0 || !in_bounds(d, offset, len))
        return CDB_E_INVALID_ARG;
    if ((s = log_write(d, offset, NULL, (size_t)len, 1)) != CDB_OK)
        return s;
    if (count_op(d))
        return CDB_E_IO;
    memset(d->cfg.current + offset, 0xFF, (size_t)len);
    return CDB_OK;
}

static cdb_status fd_sync(void *ctx)
{
    cdb_faultdev *d = (cdb_faultdev *)ctx;
    if (is_crashed(d))
        return CDB_E_IO;
    if (count_op(d))
        return CDB_E_IO;
    memcpy(d->cfg.durable, d->cfg.current, d->cfg.capacity);
    d->durable_size = d->current_size;
    d->log_len = 0;
    d->log_data_len = 0;
    return CDB_OK;
}

static cdb_status fd_geometry(void *ctx, cdb_storage_geometry *out)
{
    const cdb_faultdev *d = (const cdb_faultdev *)ctx;
    *out = d->cfg.geometry;
    return CDB_OK;
}

static cdb_status fd_size(void *ctx, uint64_t *out)
{
    const cdb_faultdev *d = (const cdb_faultdev *)ctx;
    if (is_crashed(d))
        return CDB_E_IO;
    *out = d->current_size;
    return CDB_OK;
}

static void fd_close(void *ctx)
{
    (void)ctx;
}

static const cdb_storage_ops faultdev_ops = {
    fd_read, fd_program, fd_erase, fd_sync, fd_geometry, fd_size, fd_close
};

cdb_status cdb_faultdev_init(cdb_faultdev *dev, const cdb_faultdev_config *cfg, cdb_storage *out)
{
    if (dev == NULL || cfg == NULL || out == NULL || cfg->current == NULL || cfg->durable == NULL
        || cfg->log == NULL || cfg->log_data == NULL)
        return CDB_E_INVALID_ARG;
    memset(dev, 0, sizeof *dev);
    dev->cfg = *cfg;
    memset(dev->cfg.current, 0xFF, cfg->capacity);
    memset(dev->cfg.durable, 0xFF, cfg->capacity);
    out->ops = &faultdev_ops;
    out->ctx = dev;
    return CDB_OK;
}

cdb_status cdb_faultdev_rebind(cdb_faultdev *dev, cdb_storage *out)
{
    out->ops = &faultdev_ops;
    out->ctx = dev;
    return CDB_OK;
}

void cdb_faultdev_arm(cdb_faultdev *dev, uint64_t op, cdb_crash_mode mode,
                      size_t keep_writes, size_t torn_bytes)
{
    dev->crash_at = op == 0 ? 0 : dev->mutating_ops + op;
    dev->mode = mode;
    dev->keep_writes = keep_writes;
    dev->torn_bytes = torn_bytes;
}

void cdb_faultdev_crash_now(cdb_faultdev *dev, cdb_crash_mode mode,
                            size_t keep_writes, size_t torn_bytes)
{
    dev->crashed = 1;
    dev->mode = mode;
    dev->keep_writes = keep_writes;
    dev->torn_bytes = torn_bytes;
}

static void apply_record(cdb_faultdev *d, const cdb_fault_record *r, size_t len)
{
    if (r->is_erase) {
        memset(d->cfg.durable + r->offset, 0xFF, len);
    } else {
        memcpy(d->cfg.durable + r->offset, d->cfg.log_data + r->data_off, len);
        if (r->offset + len > d->durable_size)
            d->durable_size = r->offset + len;
    }
}

void cdb_faultdev_reboot(cdb_faultdev *dev)
{
    size_t keep = 0;
    size_t i;

    if (dev->cfg.clock) {
        dev->crashed = dev->cfg.clock->crashed;
        dev->mode = dev->cfg.clock->mode;
        dev->keep_writes = dev->cfg.clock->keep_writes;
        dev->torn_bytes = dev->cfg.clock->torn_bytes;
    }
    if (!dev->crashed) {
        /* Orderly restart: nothing is lost. */
        memcpy(dev->cfg.durable, dev->cfg.current, dev->cfg.capacity);
        dev->durable_size = dev->current_size;
    } else {
        if (dev->mode == CDB_CRASH_KEEP_PREFIX || dev->mode == CDB_CRASH_TORN)
            keep = dev->keep_writes < dev->log_len ? dev->keep_writes : dev->log_len;
        for (i = 0; i < keep; i++)
            apply_record(dev, &dev->cfg.log[i], dev->cfg.log[i].len);
        if (dev->mode == CDB_CRASH_REORDER && dev->log_len > 0) {
            const cdb_fault_record *r = &dev->cfg.log[dev->log_len - 1];
            apply_record(dev, r, r->len);
        }
        if (dev->mode == CDB_CRASH_TORN && keep < dev->log_len) {
            const cdb_fault_record *r = &dev->cfg.log[keep];
            apply_record(dev, r, dev->torn_bytes < r->len ? dev->torn_bytes : r->len);
        }
    }
    memcpy(dev->cfg.current, dev->cfg.durable, dev->cfg.capacity);
    dev->current_size = dev->durable_size;
    dev->log_len = 0;
    dev->log_data_len = 0;
    dev->crashed = 0;
    dev->crash_at = 0;
}

cdb_status cdb_faultdev_flip_bit(cdb_faultdev *dev, uint64_t offset, unsigned bit)
{
    if (offset >= dev->cfg.capacity || bit > 7)
        return CDB_E_INVALID_ARG;
    dev->cfg.current[offset] ^= (uint8_t)(1u << bit);
    dev->cfg.durable[offset] ^= (uint8_t)(1u << bit);
    return CDB_OK;
}
