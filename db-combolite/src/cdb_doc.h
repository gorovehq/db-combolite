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
 * Schemas (spec 8.3): a pool may have a schema (cdb_schema.h). Every change
 * is validated when it is made, against the document as the open transaction
 * sees it; a violation is CDB_E_SCHEMA and nothing is journaled. Existing
 * documents are not re-validated when the schema changes (spec 4.3).
 *
 * Size: with cdb_db_config.doc_max 0, a revision (envelope, summary and
 * body) fits one page. With doc_max set, a body may be up to doc_max bytes:
 * what does not fit the revision page continues in an overflow chain (spec
 * 14.15). The summary always fits the revision page.
 *
 * Buffers: cdb_db_config.doc_buffers holds cdb_doc_buffers_size() bytes:
 * three document buffers (cdb_doc_buffer_size() each: doc_max and a page,
 * or one page) and three page_size buffers. A change's journal record holds
 * the whole body, so record_buf must hold doc_max plus two pages.
 * cdb_db_config.catalog_work is shared with the pool indexes. For pools with
 * a schema, the optional cdb_db_config.doc_stage caches the state of each
 * document the open transaction changed (24 bytes plus the document). It
 * never limits a transaction: once it is full, a change to a document it
 * does not hold reads that document's changes back from the journal. */
#ifndef CDB_DOC_H
#define CDB_DOC_H

#include "cdb_db.h"
#include "cdb_fts.h"
#include "cdb_schema.h"

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

/* Facts about the revision cdb_doc_get returned. */
typedef struct cdb_doc_info {
    uint64_t revision;       /* revision ID; revisions count up from 1 per document */
    uint8_t kind;            /* CDB_REV_* */
    uint64_t commit_number;  /* global commit that wrote it */
    uint64_t principal;      /* who wrote it (0 until identities exist) */
    uint32_t schema_version; /* the schema it was written under; 0 = none */
} cdb_doc_info;

/* Inside a transaction. The body is an encoded object (spec 14.6); summary
 * may be NULL. insert allocates the entry ID.
 * Each call checks the change against the document as this transaction sees
 * it and journals it; the pages change at commit. All input buffers are
 * copied. Errors (nothing is journaled on any of them):
 *   CDB_E_STATE        no open transaction
 *   CDB_E_INVALID_ARG  not a document pool, or the body/patch is malformed
 *   CDB_E_NOT_FOUND    replace/patch/delete of a document that does not exist
 *                      (or is deleted) as the transaction sees it
 *   CDB_E_OVERFLOW     the resulting revision would be too large
 *   CDB_E_SCHEMA       the result breaks the pool's schema
 * A summary given to replace becomes the new summary; without one, the old
 * summary is kept. */
cdb_status cdb_doc_insert(cdb_db *db, uint64_t store, const char *summary, size_t summary_len,
                          const void *body, size_t body_len, uint64_t *entry);
cdb_status cdb_doc_replace(cdb_db *db, uint64_t store, uint64_t entry, const char *summary,
                           size_t summary_len, const void *body, size_t body_len);
cdb_status cdb_doc_patch(cdb_db *db, uint64_t store, uint64_t entry, const void *patch, size_t patch_len);
cdb_status cdb_doc_delete(cdb_db *db, uint64_t store, uint64_t entry);

/* Reads a document as of revision rev (0 = current). Copies the body and, if
 * summary is not NULL, the summary. CDB_E_NOT_FOUND for unknown or deleted
 * documents (a tombstone revision read by number is also not found).
 * Reads see committed changes only, not the open transaction's.
 * CDB_E_BUFFER_TOO_SMALL when body or summary does not fit; *body_len and
 * *summary_len are set first, so the call can be retried. summary_len and
 * info may be NULL. The summary is not zero-terminated. */
cdb_status cdb_doc_get(cdb_db *db, uint64_t store, uint64_t entry, uint64_t rev,
                       void *body, size_t cap, size_t *body_len,
                       char *summary, size_t summary_cap, size_t *summary_len, cdb_doc_info *info);

/* Adds a schema version and makes it current; commits immediately, like other
 * catalog changes, so not inside a transaction. An empty object ({}) accepts
 * every document, which is how a schema is cleared. */
cdb_status cdb_doc_set_schema(cdb_db *db, uint64_t store, const void *schema, size_t len, uint32_t *version);

/* Copies schema version (0 = current) and reports the current version.
 * CDB_E_NOT_FOUND if the pool has no such version. */
cdb_status cdb_doc_get_schema(cdb_db *db, uint64_t store, uint32_t version, void *schema, size_t cap,
                              size_t *len, uint32_t *current);

/* Full-text search (spec 8.10, 14.14; cdb_fts.h). create builds the index
 * over the documents already in the pool and commits immediately, like other
 * catalog changes; CDB_E_EXISTS if the pool has one. Needs fts_buffers and
 * fts_tokens in the configuration, from then on whenever the pool is used.
 * search ranks live documents by BM25: CDB_FTS_ANY matches any query word,
 * CDB_FTS_ALL every word. At most max hits, best first (ties: lower entry
 * ID first). A query has at most 16 distinct words. */
cdb_status cdb_doc_create_full_text(cdb_db *db, uint64_t store, const cdb_fts_config *cfg);
cdb_status cdb_doc_search(cdb_db *db, uint64_t store, const char *query, size_t len, int mode,
                          cdb_fts_hit *hits, size_t max, size_t *n);

/* The head revision of a document (0 and *live 0 when there is none), for
 * other models that refer to documents. CDB_E_INVALID_ARG: not a pool. */
cdb_status cdb_doc_head(cdb_db *db, uint64_t store, uint64_t entry, uint64_t *rev, int *live);

/* Whether a document exists as the open transaction sees it (for links).
 * CDB_E_INVALID_ARG: not a document pool; CDB_E_NOT_FOUND: no such store. */
cdb_status cdb_doc_live_in_tx(cdb_db *db, uint64_t store, uint64_t entry, int *live);

/* For the table layer (cdb_table.h), which sets db->via_table around these.
 * get_in_tx copies a document's body as the open transaction sees it
 * (committed state outside a transaction); *live 0 when it does not exist or
 * is deleted. tx_entries calls fn with each entry of the store the open
 * transaction has changed, possibly more than once; nothing outside one. */
typedef cdb_status (*cdb_doc_entry_fn)(void *ctx, uint64_t entry);
cdb_status cdb_doc_get_in_tx(cdb_db *db, uint64_t store, uint64_t entry, void *body, size_t cap, size_t *len,
                             int *live, uint32_t *schema);
cdb_status cdb_doc_tx_entries(cdb_db *db, uint64_t store, cdb_doc_entry_fn fn, void *ctx);
/* The first committed live entry after `after`, in entry order; *done 1 at
 * the end. Uses the page buffer, so calls must not interleave with reads. */
cdb_status cdb_doc_next_live(cdb_db *db, uint64_t store, uint64_t after, uint64_t *entry, int *done);
/* Nibblet merging (cdb_nib.h) moved a revision; CDB_E_NOT_FOUND if its
 * store or revision is gone, so the record is dropped. */
cdb_status cdb_doc_nib_moved(cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen, uint64_t addr);
/* A table's revision window changed (spec 9.11): the open pool follows. */
cdb_status cdb_doc_set_keep(cdb_db *db, uint64_t store, uint32_t keep);

/* Buffer sizes for a page size and doc_max (0: bodies fit one page). */
size_t cdb_doc_buffer_size(size_t page_size, size_t doc_max);
size_t cdb_doc_buffers_size(size_t page_size, size_t doc_max);

/* Engine hooks used by cdb_db. */
cdb_status cdb_doc_apply(void *ctx, cdb_pager *pager, const cdb_delta *d);
cdb_status cdb_doc_flush_pools(cdb_db *db);
extern const cdb_slot_ops cdb_doc_slot_ops; /* open pools (cdb_slots.h) */

#endif
