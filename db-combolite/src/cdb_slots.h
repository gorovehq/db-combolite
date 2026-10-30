/* Open-store slots: the in-memory state of open document pools, vector
 * collections and time-series stores, one set per model.
 *
 * A set grows in chunks of CDB_SLOTS_CHUNK slots as stores are opened, up
 * to cdb_db_config.max_open (0: no cap). At the cap, opening another store
 * evicts the one used longest ago, after saving its unsaved state to the
 * catalog; it reopens on its next use. A slot the open transaction or the
 * current operation scope (cdb_db_op_begin) has used is never evicted, nor
 * one its model pins. Reconciliation frees the chunks
 * whose stores have all been idle since the reconciliation before.
 *
 * Chunks come from caller memory (cdb_db_config.slot_mem) first, then from
 * the environment's allocator, so a host without one still works within the
 * memory it was given.
 *
 * A slot pointer stays valid until the next open of a store of the same
 * model outside a transaction or operation scope, or the next
 * reconciliation. Code that holds one store while opening another of the
 * same model, such as a join, must run inside a scope. */
#ifndef CDB_SLOTS_H
#define CDB_SLOTS_H

#include <stddef.h>

#include "combolite/cdb_common.h"

#define CDB_SLOTS_CHUNK 4u

struct cdb_db;

/* The first member of every slot. */
typedef struct cdb_slot {
    int in_use;          /* holds an open store; 0: free */
    uint64_t store_id;
    uint64_t last_use;   /* the set's clock at its last use */
    uint64_t tx_use;     /* the transaction it was last used in; 0: none */
    uint64_t op_use;     /* the operation scope it was last used in; 0: none */
} cdb_slot;

/* What a model tells the set about its slots. */
typedef struct cdb_slot_ops {
    /* Optional: the store must stay open, for example time-series points
     * held until reconciliation folds them in. */
    int (*pinned)(const void *slot);
    /* Records what the catalog does not have yet, staged for the next
     * reconciliation as the reconciliation itself would. */
    cdb_status (*save)(struct cdb_db *db, void *slot);
} cdb_slot_ops;

typedef struct cdb_slot_chunk cdb_slot_chunk;

typedef struct cdb_slots {
    size_t elem;                /* bytes per slot */
    const cdb_slot_ops *ops;
    cdb_slot_chunk *chunks;     /* in use */
    cdb_slot_chunk *spare;      /* caller-memory chunks given back, kept for reuse */
    unsigned count;             /* slots in the chunks in use */
    uint64_t clock;             /* counts uses */
    uint64_t floor;             /* the clock at the last reconciliation */
} cdb_slots;

/* Caller memory for slots is an array of this, so chunks are aligned. */
typedef union cdb_slot_align {
    uint64_t u;
    void *p;
    long double ld;
    void (*f)(void);
} cdb_slot_align;

/* Bytes of caller memory a chunk of n slots of elem bytes takes. */
size_t cdb_slots_chunk_bytes(size_t elem, unsigned n);

void cdb_slots_init(cdb_slots *s, size_t elem, const cdb_slot_ops *ops);

/* The open slot holding a store, or NULL. */
void *cdb_slots_find(const cdb_slots *s, uint64_t store);

/* Records a use, for eviction and shrinking. */
void cdb_slots_touch(const struct cdb_db *db, cdb_slots *s, void *slot);

/* A zeroed slot that is not in use: a free one, a new one, or the least
 * recently used one, evicted. The caller fills it, sets in_use and the
 * store, and touches it. CDB_E_BUFFER_TOO_SMALL: at the cap, or out of
 * memory, with nothing that may be evicted. */
cdb_status cdb_slots_claim(struct cdb_db *db, cdb_slots *s, void **out);

/* Iterates over the slots in use: NULL from first or next ends it. */
typedef struct cdb_slot_iter {
    cdb_slot_chunk *chunk;
    unsigned i;
    size_t elem;
} cdb_slot_iter;
void *cdb_slots_first(const cdb_slots *s, cdb_slot_iter *it);
void *cdb_slots_next(cdb_slot_iter *it);

/* At reconciliation, after every slot's state is saved: frees the chunks
 * whose slots were all idle since the last call. */
void cdb_slots_shrink(struct cdb_db *db, cdb_slots *s);

/* At close: gives every chunk back. */
void cdb_slots_release(struct cdb_db *db, cdb_slots *s);

#endif
