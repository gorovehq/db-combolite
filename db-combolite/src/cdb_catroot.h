/* Catalog root slot at location 0 (spec 4.7, 14.3).
 *
 * Two 512-byte slots, A and B, sit at location 0: the first page boundary
 * after the header slots. A commit that changes the catalog writes the slot
 * not written most recently (step 3 of spec 3.2). On open, the newest valid
 * slot whose commit sequence is not above the header's is used. */
#ifndef CDB_CATROOT_H
#define CDB_CATROOT_H

#include <stdint.h>

#include "combolite/cdb_common.h"

#define CDB_CATROOT_SLOT_SIZE 512u
#define CDB_CATROOT_FORMAT 1u

typedef struct cdb_catroot {
    uint8_t slot_index;
    uint64_t commit_seq;
    uint64_t write_counter;
    uint64_t body_root;        /* logical page ID of the catalog body (primary) */
    uint64_t mirror_root;      /* logical page ID of its mirror (spec 3.5) */
    uint64_t enclave_dir_root;
    uint64_t schema_history_root;
    uint64_t quick_query_root;
    uint64_t dedup_root;
    uint64_t audit_root;
    uint64_t next_object_id;   /* never reused (spec 4.12) */
    uint64_t nib_root;         /* public nibblet free-space index (spec 14.18.3); 0 = none yet */
} cdb_catroot;

void cdb_catroot_encode(const cdb_catroot *r, uint8_t out[CDB_CATROOT_SLOT_SIZE], cdb_byte_order order);

/* CDB_E_NOT_FOUND when the slot was never written (no start magic),
 * CDB_E_CORRUPT when it was but does not verify. */
cdb_status cdb_catroot_decode(const uint8_t in[CDB_CATROOT_SLOT_SIZE], cdb_catroot *r, cdb_byte_order order);

#endif
