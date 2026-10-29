/* Delta journal: a companion file of delta and commit records
 * (spec 1.3, 1.12.2, 3.3, 14.4).
 *
 * A journal is a 64-byte companion header followed by records. Each record
 * is a 48-byte header, a payload and a 16-byte trailer whose first four bytes
 * are a CRC32C over header and payload. Records are only ever appended; a
 * reader stops at the first record that fails verification (spec 3.3). */
#ifndef CDB_JOURNAL_H
#define CDB_JOURNAL_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_storage.h"

#define CDB_JOURNAL_HEADER_SIZE 64u
#define CDB_RECORD_HEADER_SIZE 48u
#define CDB_RECORD_TRAILER_SIZE 16u
#define CDB_COMMIT_PAYLOAD_SIZE 34u

/* Companion type IDs (spec 1.12.3). */
#define CDB_COMPANION_MAIN_JOURNAL 1u

/* Record kinds (spec 14.4.2). */
#define CDB_RECORD_DATA 0u
#define CDB_RECORD_COMMIT 1u
#define CDB_RECORD_ABORT 2u
#define CDB_RECORD_WEIGHT 3u

/* Data-change operation codes (spec 14.4.5). */
#define CDB_OP_INSERT 1u
#define CDB_OP_REPLACE 2u
#define CDB_OP_PATCH 3u
#define CDB_OP_DELETE 4u
#define CDB_OP_LINK 5u
#define CDB_OP_UNLINK 6u

typedef struct cdb_journal_header {
    uint16_t type_id;
    uint16_t slot;
    uint8_t db_id[16];
    uint64_t unsealed_seq; /* archive committed sequence at creation */
    uint64_t first_seq;
    uint64_t domain_id;
} cdb_journal_header;

typedef struct cdb_record {
    uint8_t kind;
    uint8_t flags;
    uint64_t seq;
    uint64_t tx_id;
    uint64_t store_id;
    uint64_t entry_id;
    uint32_t key_version;
    const uint8_t *payload;
    uint32_t payload_len;
} cdb_record;

typedef struct cdb_commit_info {
    uint64_t commit_number;
    uint64_t timestamp;
    uint64_t principal;
    uint32_t delta_count;
    uint32_t track_mask;
} cdb_commit_info;

typedef struct cdb_journal {
    cdb_storage st;
    cdb_byte_order order;
    cdb_journal_header hdr;
    uint64_t end;      /* offset where the next record goes */
    uint64_t next_seq; /* delta sequence of the next record */
} cdb_journal;

/* Writes a new journal's header (no barrier: the caller syncs). */
cdb_status cdb_journal_create(cdb_journal *j, const cdb_storage *st, cdb_byte_order order,
                              const cdb_journal_header *hdr);

/* Reads and verifies the header. The caller scans the records. */
cdb_status cdb_journal_open(cdb_journal *j, const cdb_storage *st, cdb_byte_order order);

/* Appends one record at the end, using buf (at least the record's size) to
 * assemble it. Sets r->seq. No barrier. */
cdb_status cdb_journal_append(cdb_journal *j, cdb_record *r, uint8_t *buf, size_t buf_size);

/* Reads the record at offset into buf and verifies it. On success *next is
 * the offset of the following record. A torn or damaged record returns
 * CDB_E_CORRUPT or CDB_E_TRUNCATED: it marks the end of the journal. */
cdb_status cdb_journal_read(cdb_journal *j, uint64_t offset, uint8_t *buf, size_t buf_size,
                            cdb_record *r, uint64_t *next);

void cdb_commit_encode(const cdb_commit_info *c, uint8_t out[CDB_COMMIT_PAYLOAD_SIZE], cdb_byte_order order);
cdb_status cdb_commit_decode(const cdb_record *r, cdb_commit_info *c, cdb_byte_order order);

#endif
