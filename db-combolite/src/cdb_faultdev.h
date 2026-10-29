/* Fault-injecting in-memory storage device, for crash-safety tests (spec 3.6).
 *
 * The device keeps two images: "current" (what reads see) and "durable"
 * (what survives power loss). Programs and erases change the current image
 * and are logged; sync makes everything durable and clears the log.
 *
 * A crash is armed to strike at the Nth mutating operation (program, erase or
 * sync). The operation it strikes is logged but fails, and every later call
 * fails with CDB_E_IO. cdb_faultdev_reboot() then builds the post-crash image
 * from the durable image plus whatever part of the log the crash mode keeps.
 *
 * All memory is supplied by the caller. Not part of the production library. */
#ifndef CDB_FAULTDEV_H
#define CDB_FAULTDEV_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_storage.h"

typedef enum cdb_crash_mode {
    CDB_CRASH_DROP_UNSYNCED, /* every write since the last sync is lost */
    CDB_CRASH_KEEP_PREFIX,   /* the first keep_writes logged writes survive */
    CDB_CRASH_TORN,          /* as KEEP_PREFIX, then only torn_bytes of the next write survive */
    CDB_CRASH_REORDER        /* only the most recent unsynced write survives: the device
                              * wrote it before earlier ones, as real disks may */
} cdb_crash_mode;

#define CDB_FAULTDEV_FLASH 0x1u /* flash rules: program may only clear bits; erase sets 0xFF */

/* A crash clock shared by several devices (and a fault environment), so one
 * armed crash strikes whichever device performs the Nth mutating operation
 * and cuts power to all of them at once. */
typedef struct cdb_fault_clock {
    uint64_t ops;      /* mutating operations attempted so far, across devices */
    uint64_t crash_at; /* 0 = disarmed */
    int crashed;
    cdb_crash_mode mode;
    size_t keep_writes;
    size_t torn_bytes;
} cdb_fault_clock;

typedef struct cdb_fault_record {
    uint64_t offset;
    size_t len;
    size_t data_off; /* into the log data buffer; unused for erases */
    int is_erase;
    int is_truncate; /* offset holds the new size */
} cdb_fault_record;

typedef struct cdb_faultdev_config {
    uint8_t *current;          /* capacity bytes */
    uint8_t *durable;          /* capacity bytes */
    size_t capacity;
    cdb_fault_record *log;     /* room for log_cap records */
    size_t log_cap;
    uint8_t *log_data;         /* copies of logged write data */
    size_t log_data_cap;
    cdb_storage_geometry geometry;
    unsigned flags;
    cdb_fault_clock *clock;    /* optional: share crashes with other devices */
} cdb_faultdev_config;

typedef struct cdb_faultdev {
    cdb_faultdev_config cfg;
    uint64_t current_size;
    uint64_t durable_size;
    size_t log_len;
    size_t log_data_len;
    uint64_t mutating_ops; /* programs, erases and syncs attempted so far */
    uint64_t crash_at;     /* 0 = disarmed; otherwise crash on this mutating op */
    int crashed;
    cdb_crash_mode mode;
    size_t keep_writes;
    size_t torn_bytes;
} cdb_faultdev;

/* Initialises the device as freshly erased (0xFF) media and binds *out to it. */
cdb_status cdb_faultdev_init(cdb_faultdev *dev, const cdb_faultdev_config *cfg, cdb_storage *out);

/* Binds *out to an already initialised device, keeping its contents. */
cdb_status cdb_faultdev_rebind(cdb_faultdev *dev, cdb_storage *out);

/* Arms a crash at mutating operation number op (1-based, counted from now). */
void cdb_faultdev_arm(cdb_faultdev *dev, uint64_t op, cdb_crash_mode mode,
                      size_t keep_writes, size_t torn_bytes);

/* Cuts power immediately, between operations. */
void cdb_faultdev_crash_now(cdb_faultdev *dev, cdb_crash_mode mode,
                            size_t keep_writes, size_t torn_bytes);

/* Rebuilds the image a reader would find after the crash, then resumes. */
void cdb_faultdev_reboot(cdb_faultdev *dev);

/* Counts one mutating operation on a shared clock. Returns 1 when the armed
 * crash strikes it. Used by the fault environment for directory operations. */
int cdb_fault_clock_tick(cdb_fault_clock *clock);

/* Simulates media damage: flips one bit in both images. */
cdb_status cdb_faultdev_flip_bit(cdb_faultdev *dev, uint64_t offset, unsigned bit);

#endif
