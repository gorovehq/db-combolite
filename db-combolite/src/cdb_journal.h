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
#define CDB_WRITE_KEY_PAYLOAD_SIZE 8u
#define CDB_PREPARE_PAYLOAD_SIZE 8u
#define CDB_ROLLBACK_PAYLOAD_SIZE 8u

/* Format limit (spec 14.4.9 rule 4): disjoint ranges of dropped data records
 * in one transaction. */
#define CDB_TX_MAX_DROPS 32u

/* Companion type IDs (spec 1.12.3). */
#define CDB_COMPANION_MAIN_JOURNAL 1u

/* Record kinds (spec 14.4.2). */
#define CDB_RECORD_DATA 0u
#define CDB_RECORD_COMMIT 1u
#define CDB_RECORD_ABORT 2u
#define CDB_RECORD_WEIGHT 3u
#define CDB_RECORD_WRITE_KEY 4u
#define CDB_RECORD_PREPARE 5u
#define CDB_RECORD_ROLLBACK 6u

/* Record flags (spec 14.4.2). */
#define CDB_RECORD_FLAG_ENCRYPTED 0x01u
#define CDB_RECORD_FLAG_COMPRESSED 0x02u
#define CDB_RECORD_FLAG_CONTINUED 0x04u
#define CDB_RECORD_FLAG_REPLICATED 0x08u
#define CDB_RECORD_FLAG_ROLLBACKS 0x10u /* commit record: the transaction holds a rollback record (14.4.9) */

/* Data-change operation codes (spec 14.4.5). */
#define CDB_OP_INSERT 1u
#define CDB_OP_REPLACE 2u
#define CDB_OP_PATCH 3u
#define CDB_OP_DELETE 4u
#define CDB_OP_LINK 5u
#define CDB_OP_UNLINK 6u

/* The companion file header, decoded (spec 14.4.1). */
typedef struct cdb_journal_header {
    uint16_t type_id;      /* companion type, e.g. CDB_COMPANION_MAIN_JOURNAL */
    uint16_t slot;         /* slot number of the owning coordinator */
    uint8_t db_id[16];     /* must match the archive's database ID */
    uint64_t unsealed_seq; /* archive committed sequence at creation */
    uint64_t first_seq;    /* delta sequence of the first record in this file */
    uint64_t domain_id;    /* encryption domain; 0 = public */
} cdb_journal_header;

/* One delta record (spec 14.4.2). On append the caller fills every field
 * except seq; on read every field is filled and payload points into the
 * caller's buffer. */
typedef struct cdb_record {
    uint8_t kind;          /* CDB_RECORD_* */
    uint8_t flags;         /* encrypted, compressed (none used yet) */
    uint64_t seq;          /* delta sequence number, assigned by append */
    uint64_t tx_id;        /* owning transaction (spec 14.4.3) */
    uint64_t store_id;     /* target store */
    uint64_t entry_id;     /* target entry, or 0 */
    uint32_t key_version;
    const uint8_t *payload;
    uint32_t payload_len;
} cdb_record;

/* Commit record payload, decoded (spec 14.4.4). */
typedef struct cdb_commit_info {
    uint64_t commit_number; /* global commit number, from 1 */
    uint64_t timestamp;     /* ns since the Unix epoch; 0 if the platform has no clock */
    uint64_t principal;     /* identity that committed */
    uint32_t delta_count;   /* data records in the transaction */
    uint32_t track_mask;    /* companion types the transaction touched */
} cdb_commit_info;

/* An open journal file. The storage handle is copied by value. */
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

/* Reads and verifies the header. The caller scans the records.
 * CDB_E_CORRUPT for a damaged or foreign header (wrong magic, byte order
 * or CRC), CDB_E_UNSUPPORTED for an unknown epoch. */
cdb_status cdb_journal_open(cdb_journal *j, const cdb_storage *st, cdb_byte_order order);

/* Appends one record at the end, using buf (at least the record's size) to
 * assemble it. When encrypted, payload is encrypted under key and authenticated with AEAD tag. */
cdb_status cdb_journal_append_crypto(cdb_journal *j, cdb_record *r, uint8_t *buf, size_t buf_size,
                                     unsigned aead_alg, const uint8_t *key);

/* Appends one unencrypted record at the end. */
cdb_status cdb_journal_append(cdb_journal *j, cdb_record *r, uint8_t *buf, size_t buf_size);

/* Reads and verifies the record at offset into buf. If encrypted, decrypts payload with key. */
cdb_status cdb_journal_read_crypto(cdb_journal *j, uint64_t offset, uint8_t *buf, size_t buf_size,
                                   cdb_record *r, uint64_t *next,
                                   unsigned aead_alg, const uint8_t *key);

/* Reads and verifies an unencrypted record at offset into buf. */
cdb_status cdb_journal_read(cdb_journal *j, uint64_t offset, uint8_t *buf, size_t buf_size,
                            cdb_record *r, uint64_t *next);

/* Encode and decode a commit record's payload (spec 14.4.4). */
void cdb_commit_encode(const cdb_commit_info *c, uint8_t out[CDB_COMMIT_PAYLOAD_SIZE], cdb_byte_order order);
cdb_status cdb_commit_decode(const cdb_record *r, cdb_commit_info *c, cdb_byte_order order);

#endif
