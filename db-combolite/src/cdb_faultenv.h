/* Fault-injecting environment, for crash tests that span several files.
 *
 * Files are fault devices on one shared crash clock. Directory entries are
 * modelled too: a create, remove or rename survives a crash only if its
 * directory was synced afterwards, and files inside a directory whose own
 * creation was never made durable vanish with it. Directory operations count
 * as mutating operations, so they are crash points as well.
 *
 * All memory comes from the caller. Not part of the production library. */
#ifndef CDB_FAULTENV_H
#define CDB_FAULTENV_H

#include "combolite/cdb_env.h"
#include "cdb_faultdev.h"

#define CDB_FAULTENV_MAX_FILES 6
#define CDB_FAULTENV_MAX_DIRS 4
#define CDB_FAULTENV_NAME 160

typedef struct cdb_faultenv_file {
    char name[CDB_FAULTENV_NAME];
    char durable_name[CDB_FAULTENV_NAME];
    int exists;
    int durable_exists;
    cdb_faultdev dev;
} cdb_faultenv_file;

typedef struct cdb_faultenv_dir {
    char name[CDB_FAULTENV_NAME];
    int exists;
    int durable_exists;
} cdb_faultenv_dir;

typedef struct cdb_faultenv_config {
    uint8_t *arena;            /* 3 * file_capacity bytes per file slot */
    size_t file_capacity;
    cdb_fault_record *records; /* records_per_file per file slot */
    size_t records_per_file;
} cdb_faultenv_config;

typedef struct cdb_faultenv {
    cdb_faultenv_config cfg;
    cdb_fault_clock clock;
    cdb_faultenv_file files[CDB_FAULTENV_MAX_FILES];
    cdb_faultenv_dir dirs[CDB_FAULTENV_MAX_DIRS];
    int has_time;     /* the clock: off after init */
    int64_t time_ns;
} cdb_faultenv;

/* Sets the environment's clock: on, and the time it reports (ns since the
 * Unix epoch); off, the environment has no clock. Survives reboots. */
void cdb_faultenv_set_time(cdb_faultenv *fe, int on, int64_t ns);

cdb_status cdb_faultenv_init(cdb_faultenv *fe, const cdb_faultenv_config *cfg, cdb_env *out);

/* Arms a crash at mutating operation number op, counted from now, across
 * every file and directory operation. */
void cdb_faultenv_arm(cdb_faultenv *fe, uint64_t op, cdb_crash_mode mode,
                      size_t keep_writes, size_t torn_bytes);

void cdb_faultenv_crash_now(cdb_faultenv *fe, cdb_crash_mode mode,
                            size_t keep_writes, size_t torn_bytes);

/* Restarts after a crash, or after an orderly shutdown if none happened. */
void cdb_faultenv_reboot(cdb_faultenv *fe);

/* The device behind a path, for tests that damage or inspect bytes. */
cdb_faultdev *cdb_faultenv_device(cdb_faultenv *fe, const char *path);

#endif
