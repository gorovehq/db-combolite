/* Fault-injecting environment, for crash tests that span several files.
 *
 * Files are fault devices on one shared crash clock. Directory entries are
 * modelled too: a create, remove or rename survives a crash only if its
 * directory was synced afterwards, and files inside a directory whose own
 * creation was never made durable vanish with it. Directory operations count
 * as mutating operations, so they are crash points as well.
 *
 * Its own memory comes from the caller. The allocator it offers the engine
 * uses malloc, keeps track of every block, and frees them all at a reboot,
 * as a real crash would. Not part of the production library. */
#ifndef CDB_FAULTENV_H
#define CDB_FAULTENV_H

#include "combolite/cdb_env.h"
#include "cdb_faultdev.h"

#define CDB_FAULTENV_MAX_FILES 10
#define CDB_FAULTENV_MAX_DIRS 4
#define CDB_FAULTENV_NAME 160
#define CDB_FAULTENV_MAX_ALLOCS 256
#define CDB_FAULTENV_MAX_LOCKS 16

typedef struct cdb_faultenv_file {
    char name[CDB_FAULTENV_NAME];
    char durable_name[CDB_FAULTENV_NAME];
    int exists;
    int durable_exists;
    uint64_t gen;      /* which file this is: new at each create, kept by renames */
    cdb_faultdev dev;
} cdb_faultenv_file;

/* A lock held through the lock hook (spec 5.8). It belongs to the file it
 * was taken on, not to the path: a rename keeps it, and a file created
 * later at the same path is a different file. */
struct cdb_lock {
    int used;
    int exclusive;
    int file;          /* index into files */
    uint64_t gen;
};

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
    int64_t mono_ns;  /* the monotonic clock: moves only by sleep and cdb_faultenv_advance */
    uint64_t rng;     /* the random source: xorshift64*, the same sequence on every run */
    void (*on_sleep)(void *arg); /* runs after each sleep, not nested */
    void *on_sleep_arg;
    int in_sleep_hook;
    int no_alloc;     /* the allocator refuses: on after init is off */
    void *allocs[CDB_FAULTENV_MAX_ALLOCS]; /* live blocks */
    unsigned live;    /* how many */
    uint64_t next_gen;
    cdb_lock locks[CDB_FAULTENV_MAX_LOCKS];
} cdb_faultenv;

/* Sets the environment's clock: on, and the time it reports (ns since the
 * Unix epoch); off, the environment has no clock. Survives reboots. */
void cdb_faultenv_set_time(cdb_faultenv *fe, int on, int64_t ns);

/* Moves the monotonic clock forward by ns, as time passing would. The
 * environment's sleep does the same, so waits in tests take no real time.
 * Survives reboots. */
void cdb_faultenv_advance(cdb_faultenv *fe, int64_t ns);

/* Calls fn(arg) after each sleep, so a test can let other coordinators run
 * (beat) while one waits, as other processes would. A sleep inside fn does
 * not call it again. NULL turns it off. */
void cdb_faultenv_on_sleep(cdb_faultenv *fe, void (*fn)(void *arg), void *arg);

/* Turns the allocator on (the default) or off: off, it returns NULL, as a
 * host with no allocator would, so the engine uses caller memory only.
 * Survives reboots. */
void cdb_faultenv_set_alloc(cdb_faultenv *fe, int on);

/* Blocks the engine has allocated and not yet freed. */
unsigned cdb_faultenv_live_allocs(const cdb_faultenv *fe);

cdb_status cdb_faultenv_init(cdb_faultenv *fe, const cdb_faultenv_config *cfg, cdb_env *out);

/* Locks held now. Every process in a test shares this environment, so a
 * test ends one process's life by closing its lock handles (lock_close), as
 * the system would; a reboot releases them all, and handles from before it
 * are abandoned, never closed. */
unsigned cdb_faultenv_held_locks(const cdb_faultenv *fe);

/* Rebinds *env to the same environment without the lock hook, for the
 * heartbeat fallback (spec 5.8). */
void cdb_faultenv_without_locks(cdb_env *env);

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
