/* File header, epoch 1 (spec 1.9, 1.10, 14.1).
 *
 * Two 512-byte slots, A at byte 0 and B at byte 512. One holds the committed
 * record; the other is the working copy. Checkpoints rewrite the working copy
 * without raising the committed sequence; a commit writes the working copy
 * with a higher committed sequence, after which the two swap roles. */
#ifndef CDB_HEADER_H
#define CDB_HEADER_H

#include <stdint.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_storage.h"

#define CDB_HEADER_SLOT_SIZE 512u
#define CDB_HEADER_SLOTS 2u
#define CDB_HEADER_EPOCH 1u
#define CDB_HEADER_COMPANION_SLOTS 12u
#define CDB_HEADER_REGION_ANCHORS 4u
#define CDB_HEADER_KEYREF_LEN 128u

#define CDB_ROLE_WORKING 0u
#define CDB_ROLE_COMMITTED 1u

/* Layout flags, header offset 11 (spec 14.1.4). */
#define CDB_LAYOUT_ADDR64 0x01u
#define CDB_LAYOUT_COMPACT_HEADER 0x02u
#define CDB_LAYOUT_DANGER_MODE 0x04u
#define CDB_LAYOUT_GOVERNANCE 0x08u
#define CDB_LAYOUT_KNOWN 0x0Fu

/* Feature flags this build understands, per class (spec 1.2). None are
 * assigned yet; each feature adds its bit here as it is implemented. */
#define CDB_FEATURES_COMPAT_KNOWN UINT64_C(0)
#define CDB_FEATURES_ROCOMPAT_KNOWN UINT64_C(0)
#define CDB_FEATURES_INCOMPAT_KNOWN UINT64_C(0)

typedef struct cdb_companion_slot {
    uint16_t type_id;
    uint8_t compat_class; /* 0 compatible, 1 read-only compatible, 2 incompatible */
    uint8_t state;        /* bit 0 provisioned, bit 1 in use */
    uint64_t watermark;   /* reconciled through this global commit number */
} cdb_companion_slot;

typedef struct cdb_region_anchor {
    uint8_t feature_index;
    uint32_t size_hint;
    uint64_t anchor;
} cdb_region_anchor;

typedef struct cdb_header {
    cdb_byte_order order;
    uint8_t epoch;
    uint8_t role;
    uint8_t layout_flags;
    uint8_t db_id[16];
    uint64_t committed_seq;
    uint64_t write_counter;
    uint64_t page_seq_hwm;
    uint64_t anchor_root;
    uint64_t append_point;
    uint8_t min_page_log2;
    uint8_t max_page_log2;
    uint8_t page_mode;
    uint8_t default_page_log2;
    uint8_t checksum_alg;
    uint8_t aead_alg;
    uint16_t ring_size;
    uint64_t compat_flags;
    uint64_t rocompat_flags;
    uint64_t incompat_flags;
    uint32_t app_id;
    uint32_t creator_version;
    uint32_t writer_version;
    uint64_t created_us;
    uint8_t key_ref[CDB_HEADER_KEYREF_LEN]; /* UTF-8, zero padded */
    cdb_companion_slot companions[CDB_HEADER_COMPANION_SLOTS];
    cdb_region_anchor anchors[CDB_HEADER_REGION_ANCHORS];
    uint64_t commit_hwm;
    uint64_t anchor_mirror_root;
    uint8_t content_hash_alg;
    uint8_t tamper_tag[16];
} cdb_header;

/* Serialises h into one slot image and fills in the CRC32C. */
cdb_status cdb_header_encode(const cdb_header *h, uint8_t out[CDB_HEADER_SLOT_SIZE]);

/* Parses and verifies one slot image. CDB_E_CORRUPT on CRC mismatch,
 * CDB_E_UNSUPPORTED for an unknown epoch, CDB_E_MALFORMED otherwise. */
cdb_status cdb_header_decode(const uint8_t in[CDB_HEADER_SLOT_SIZE], cdb_header *h);

/* Checks feature flags against this build (spec 1.2): an unknown
 * incompatible flag refuses the file; an unknown read-only-compatible flag
 * sets *read_only. Unknown compatible flags are ignored. */
cdb_status cdb_header_check_features(const cdb_header *h, int *read_only);

/* Header state of an open file. */
typedef struct cdb_header_state {
    cdb_header record;      /* the committed record in force */
    unsigned record_slot;   /* slot holding it */
    unsigned working_slot;  /* slot the next header write goes to */
    uint64_t write_counter; /* highest write counter seen in any valid slot */
    int read_only;
} cdb_header_state;

/* Writes a new file's header: slot A as the committed record and slot B as
 * its working copy, each followed by a barrier. h->committed_seq is set to 1. */
cdb_status cdb_header_create(const cdb_storage *st, const cdb_header *h, cdb_header_state *hs);

/* Reads both slots and selects the record (spec 1.9): highest committed
 * sequence, then the committed role, then the higher write counter.
 * High-water marks are the maximum across every valid slot (spec 12.1). */
cdb_status cdb_header_open(const cdb_storage *st, cdb_header_state *hs);

/* Every header write goes to the slot not written most recently, followed by
 * a barrier, so a torn write can only damage the older slot (spec 1.11.3).
 * Both functions refuse to lower a reservation high-water mark. */

/* Commit (step 4 of spec 3.2): h becomes the new committed record, with the
 * committed sequence raised by one. Role, sequence and counter are set here. */
cdb_status cdb_header_commit(const cdb_storage *st, cdb_header_state *hs, const cdb_header *h);

/* Checkpoint: rewrites the committed record unchanged except for the
 * reservation high-water marks, which must be durable before use (spec 12.1). */
cdb_status cdb_header_checkpoint(const cdb_storage *st, cdb_header_state *hs,
                                 uint64_t page_seq_hwm, uint64_t commit_hwm);

#endif
