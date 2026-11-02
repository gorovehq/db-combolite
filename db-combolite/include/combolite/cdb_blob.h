/* Streaming API & Content-Addressed Blob Storage (spec 8.4, 14.15).
 *
 * Allows writing and reading large arbitrary binary content piece by piece
 * without holding the whole object in memory at once. Blobs are stored
 * across overflow pages (type 0x0007), content-addressed by SHA-256 (or
 * HMAC-SHA-256 in an enclave). */
#ifndef COMBOLITE_CDB_BLOB_H
#define COMBOLITE_CDB_BLOB_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_crypto.h"

/* Forward declarations */
struct cdb_pager;
struct cdb_db;

/* Blob reference (spec 14.15). */
typedef struct cdb_blob_ref {
    uint8_t hash[CDB_CRYPTO_HASH_SIZE]; /* Cryptographic SHA-256 / HMAC-SHA-256 */
    uint64_t total_len;                 /* Total content length in bytes */
    uint64_t first_page;                /* Logical ID of first page (0x0007); 0 if empty */
    uint64_t page_count;                /* Total number of pages occupied */
    uint32_t flags;                     /* Bit 0: external file; Bit 1: enclave */
} cdb_blob_ref;

/* Streaming blob writer context. Fixed size, zero heap allocation. */
typedef struct cdb_blob_writer {
    struct cdb_pager *pager;
    uint64_t store_id;
    cdb_crypto_hash_ctx hash_ctx;
    uint64_t first_page;
    uint64_t curr_page;
    uint64_t page_count;
    uint64_t total_bytes;
    size_t page_room;
    size_t curr_page_fill;
    uint8_t *scratch;   /* Exactly p->page_size bytes */
    int is_enclave;
    int is_open;
} cdb_blob_writer;

/* Begins streaming write of a blob owned by store_id. scratch must hold
 * p->page_size bytes. If enclave_key is non-NULL, the content hash is
 * computed as HMAC-SHA-256 under the key; otherwise standard SHA-256 is used. */
cdb_status cdb_blob_write_begin(struct cdb_pager *p, uint64_t store_id, uint8_t *scratch,
                               const uint8_t *enclave_key, cdb_blob_writer *w);

/* Appends len bytes from data to the streaming blob. */
cdb_status cdb_blob_write_chunk(cdb_blob_writer *w, const void *data, size_t len);

/* Finalizes the blob stream, writes out the final page, computes the final hash,
 * and fills out_ref. scratch is no longer in use once this returns. */
cdb_status cdb_blob_write_end(cdb_blob_writer *w, cdb_blob_ref *out_ref);

/* Aborts an in-progress blob stream, releasing all pages allocated so far. */
cdb_status cdb_blob_write_abort(cdb_blob_writer *w);

/* Streaming blob reader context. Fixed size, zero heap allocation. */
typedef struct cdb_blob_reader {
    struct cdb_pager *pager;
    uint64_t store_id;
    cdb_blob_ref ref;
    uint64_t curr_page;
    uint64_t next_page;
    uint64_t bytes_remaining;
    size_t curr_offset_in_page;
    size_t curr_page_bytes;
    cdb_crypto_hash_ctx hash_ctx;
    uint8_t *page_buf;  /* Exactly p->page_size bytes */
    int verify_hash;
    int is_open;
} cdb_blob_reader;

/* Begins streaming read of a blob. page_buf must hold p->page_size bytes.
 * If verify_hash is true, the reader incrementally hashes the stream and
 * verifies the hash against ref->hash upon reaching EOF.
 * If enclave_key is non-NULL, the hash is verified using HMAC-SHA-256. */
cdb_status cdb_blob_read_begin(struct cdb_pager *p, uint64_t store_id, const cdb_blob_ref *ref,
                              uint8_t *page_buf, int verify_hash, const uint8_t *enclave_key,
                              cdb_blob_reader *r);

/* Reads up to cap bytes into dest. *out_len receives the number of bytes read.
 * When EOF is reached (*out_len == 0 and remaining == 0), if verify_hash was
 * requested, the content hash is verified; if mismatch, returns CDB_E_CORRUPT. */
cdb_status cdb_blob_read_chunk(cdb_blob_reader *r, void *dest, size_t cap, size_t *out_len);

/* Finishes reading. */
cdb_status cdb_blob_read_end(cdb_blob_reader *r);

/* Releases every page of the blob at ref->first_page. page_buf is p->page_size. */
cdb_status cdb_blob_free(struct cdb_pager *p, uint64_t store_id, const cdb_blob_ref *ref, uint8_t *page_buf);

#endif /* COMBOLITE_CDB_BLOB_H */
