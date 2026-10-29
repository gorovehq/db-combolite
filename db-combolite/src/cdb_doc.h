/* Documents (spec 8, 14.9).
 *
 * A document pool is a catalog store with the document model. Each document
 * is an entry whose revisions form a chain: the original, then patches, with
 * a full checkpoint every checkpoint_every revisions, and a tombstone when it
 * is deleted. History is never discarded (spec 8.6, 8.12).
 *
 * Changes go through transactions: cdb_doc_insert/replace/patch/delete append
 * journal records, and the built-in document apply writes revisions when the
 * transaction commits (spec 11.3). Two indexes per pool locate revisions:
 *   head:  entry ID              -> head revision (ID, page, chain length, kind)
 *   revs:  (entry ID, revision)  -> revision page
 *
 * Buffers: cdb_db_config.doc_buffers holds five page_size buffers, and
 * cdb_db_config.catalog_work is shared with the pool indexes. */
#ifndef CDB_DOC_H
#define CDB_DOC_H

#include "cdb_db.h"

/* Revision kinds (spec 14.9.1). */
#define CDB_REV_ORIGINAL 0u
#define CDB_REV_DELTA 1u
#define CDB_REV_CHECKPOINT 2u
#define CDB_REV_TOMBSTONE 3u

/* Patch operations (spec 14.9.2). A patch is an array of operations, each an
 * array [op, path, value]: op 1 = set (value present), op 2 = remove; path is
 * an array of field IDs leading through nested objects. */
#define CDB_PATCH_SET 1
#define CDB_PATCH_REMOVE 2

typedef struct cdb_doc_info {
    uint64_t revision;
    uint8_t kind;
    uint64_t commit_number;
    uint64_t principal;
} cdb_doc_info;

/* Inside a transaction. The body is an encoded object (spec 14.6); summary
 * may be NULL. insert allocates the entry ID. */
cdb_status cdb_doc_insert(cdb_db *db, uint64_t store, const char *summary, size_t summary_len,
                          const void *body, size_t body_len, uint64_t *entry);
cdb_status cdb_doc_replace(cdb_db *db, uint64_t store, uint64_t entry, const char *summary,
                           size_t summary_len, const void *body, size_t body_len);
cdb_status cdb_doc_patch(cdb_db *db, uint64_t store, uint64_t entry, const void *patch, size_t patch_len);
cdb_status cdb_doc_delete(cdb_db *db, uint64_t store, uint64_t entry);

/* Reads a document as of revision rev (0 = current). Copies the body and, if
 * summary is not NULL, the summary. CDB_E_NOT_FOUND for unknown or deleted
 * documents (a tombstone revision read by number is also not found). */
cdb_status cdb_doc_get(cdb_db *db, uint64_t store, uint64_t entry, uint64_t rev,
                       void *body, size_t cap, size_t *body_len,
                       char *summary, size_t summary_cap, size_t *summary_len, cdb_doc_info *info);

/* Engine hooks used by cdb_db. */
cdb_status cdb_doc_apply(void *ctx, cdb_pager *pager, const cdb_delta *d);
cdb_status cdb_doc_flush_pools(cdb_db *db);

#endif
