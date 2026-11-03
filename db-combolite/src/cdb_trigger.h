/* Event Triggers, Trigger Safety & Scheduled Maintenance Coordination (Spec 11.6, 11.9, 11.12).
 *
 * Implements post-commit event triggers with hard recursion depth limits (default 8),
 * cascade mutation warnings, and background scheduled maintenance execution via cdb_worker.
 */
#ifndef CDB_TRIGGER_H
#define CDB_TRIGGER_H

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"
#include "cdb_worker.h"

struct cdb_db;

#define CDB_TRIGGER_AFTER_INSERT 0x01u
#define CDB_TRIGGER_AFTER_UPDATE 0x02u
#define CDB_TRIGGER_AFTER_DELETE 0x04u

#define CDB_MAX_TRIGGERS 16u
#define CDB_MAX_SCHEDULED_TASKS 16u
#define CDB_TRIGGER_MAX_NAME 32u
#define CDB_TRIGGER_DEFAULT_MAX_DEPTH 8u

typedef struct cdb_trigger_def {
    char name[CDB_TRIGGER_MAX_NAME];
    size_t name_len;
    uint64_t store_id;
    uint8_t events; /* Bitmask of CDB_TRIGGER_AFTER_* */
    char proc_name[CDB_TRIGGER_MAX_NAME];
    size_t proc_len;
    uint8_t max_depth;
} cdb_trigger_def;

typedef struct cdb_schedule_def {
    char name[CDB_TRIGGER_MAX_NAME];
    size_t name_len;
    uint64_t interval_ns;
    int64_t last_run_ns;
    void (*task_fn)(struct cdb_db *db, void *arg);
    void *arg;
} cdb_schedule_def;

typedef struct cdb_trigger_registry {
    cdb_trigger_def triggers[CDB_MAX_TRIGGERS];
    size_t n_triggers;

    cdb_schedule_def schedules[CDB_MAX_SCHEDULED_TASKS];
    size_t n_schedules;

    cdb_worker_pool *pool;
    uint64_t cascade_threshold;
    uint64_t total_dispatched;
    int cascade_warning_issued;
} cdb_trigger_registry;

/* Initializes the trigger and scheduled task registry */
void cdb_trigger_registry_init(cdb_trigger_registry *reg, cdb_worker_pool *pool, uint64_t cascade_threshold);

/* Registers an event trigger on a store */
cdb_status cdb_trigger_register(cdb_trigger_registry *reg, const char *name, size_t name_len,
                                uint64_t store_id, uint8_t events,
                                const char *proc_name, size_t proc_len,
                                uint8_t max_depth);

/* Unregisters a trigger by name */
cdb_status cdb_trigger_unregister(cdb_trigger_registry *reg, const char *name, size_t name_len);

/* Dispatches matching event triggers for a committed store mutation */
cdb_status cdb_trigger_dispatch(cdb_trigger_registry *reg, struct cdb_db *db,
                                uint64_t store_id, uint8_t event,
                                const cdb_value *old_val, const cdb_value *new_val,
                                unsigned current_depth);

/* Registers a scheduled maintenance task */
cdb_status cdb_schedule_register(cdb_trigger_registry *reg, const char *name, size_t name_len,
                                 uint64_t interval_ns, void (*task_fn)(struct cdb_db *db, void *arg), void *arg);

/* Evaluates and triggers any due scheduled maintenance tasks */
cdb_status cdb_schedule_tick(cdb_trigger_registry *reg, struct cdb_db *db, int64_t now_ns);

#endif /* CDB_TRIGGER_H */
