/* Links between entries (spec 6, 14.13).
 *
 * A link joins two entries, of any store, under a link type. Link types are
 * catalog objects (cdb_catalog_*_link_type) with a cardinality, whether it
 * is enforced or only recorded (6.9), and what deleting an entry does to its
 * links (6.7). Every link lives in one link store, a store of model
 * CDB_MODEL_LINK named "$links", in two indexes:
 *   forward: (source, type, target) -> weight (f32), time (ns), flags
 *   reverse: (target, type, source) -> nothing
 * A reference is a store ID and an entry ID, 8 bytes each; keys are
 * big-endian.
 *
 * Changes go through transactions: add, reinforce and remove journal LINK
 * and UNLINK records, applied at commit by the built-in apply, which never
 * fails for logical reasons. They are checked when they are made, as the
 * open transaction sees things: both ends must exist; the link must not
 * exist yet (live or tombstoned: a pair is never linked twice); an enforced
 * cardinality must hold. Removing tombstones the link and keeps its history.
 *
 * When an entry is deleted, its links in both directions are tombstoned,
 * flagged with the end that is gone; a cascade type erases them instead. A
 * refuse type forbids the delete while a live link of that type points at
 * the entry. Dropping a store tombstones the links of its entries.
 *
 * Time: a link's time is the environment's clock when there is one (ns
 * since the Unix epoch), else the time the caller gives. */
#ifndef CDB_LINK_H
#define CDB_LINK_H

#include "cdb_db.h"

/* Cardinalities (spec 6.5): 1:N allows each target one live source. */
#define CDB_CARD_1_1 1u
#define CDB_CARD_1_N 2u
#define CDB_CARD_N_M 3u

/* What deleting an entry does to its links (spec 6.7). */
#define CDB_LINK_ON_DELETE_TOMBSTONE 0u
#define CDB_LINK_ON_DELETE_CASCADE 1u
#define CDB_LINK_ON_DELETE_REFUSE 2u

/* Link flags. */
#define CDB_LINK_TOMBSTONE 0x01u
#define CDB_LINK_SOURCE_GONE 0x02u
#define CDB_LINK_TARGET_GONE 0x04u

/* Directions for listing. */
#define CDB_LINK_OUT 0
#define CDB_LINK_IN 1

#define CDB_LINK_STORE_NAME "$links"

typedef struct cdb_ref {
    uint64_t store;
    uint64_t entry;
} cdb_ref;

typedef struct cdb_link {
    uint64_t type;
    cdb_ref source;
    cdb_ref target;
    float weight;
    int64_t time;   /* created or last reinforced */
    uint8_t flags;  /* CDB_LINK_* */
} cdb_link;

/* Defines a link type; commits immediately, like other catalog changes, so
 * not inside a transaction. Creates the link store the first time. */
cdb_status cdb_link_define_type(cdb_db *db, const char *name, size_t len, unsigned cardinality, int enforced,
                                unsigned on_delete, uint64_t *type);
cdb_status cdb_link_find_type(cdb_db *db, const char *name, size_t len, uint64_t *type, int *found);

/* Inside a transaction. time is used only when the environment has no clock.
 * add: CDB_E_NOT_FOUND if an end or the type does not exist, CDB_E_EXISTS if
 * the pair is already linked under the type (or was), CDB_E_STATE if an
 * enforced cardinality would break. reinforce and remove: CDB_E_NOT_FOUND
 * unless the link is live. */
cdb_status cdb_link_add(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight, int64_t time);
cdb_status cdb_link_reinforce(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight,
                              int64_t time);
cdb_status cdb_link_remove(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target);

/* Committed links. get: *found 0 when there is none. list: the links out of
 * (or into) entry, of one type or all (type 0), in key order, starting after
 * *after (NULL: from the first); at most max. */
cdb_status cdb_link_get(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, cdb_link *out, int *found);
cdb_status cdb_link_list(cdb_db *db, cdb_ref entry, int direction, uint64_t type, const cdb_link *after,
                         cdb_link *out, size_t max, size_t *n);

/* Engine hooks. */
int cdb_link_is_store(cdb_db *db, uint64_t store);
cdb_status cdb_link_apply(cdb_db *db, const cdb_delta *d);
/* Before a delete is journaled: CDB_E_STATE if a live refuse-type link
 * points at the entry, as the open transaction sees it. */
cdb_status cdb_link_check_delete(cdb_db *db, uint64_t store, uint64_t entry);
/* After a delete is applied, and before a store's drop commits. */
cdb_status cdb_link_on_delete(cdb_db *db, uint64_t store, uint64_t entry);
cdb_status cdb_link_on_drop(cdb_db *db, uint64_t store);

#endif
