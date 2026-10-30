/* Full-text search (spec 8.10, 14.14).
 *
 * A document pool can have one full-text index over named fields (paths of
 * field IDs; strings, and strings directly inside arrays) and optionally the
 * summary. Three ordered indexes hold it:
 *   terms:    term                -> document frequency (4 bytes)
 *   postings: (term, entry ID)    -> term frequency (4 bytes)
 *   lengths:  entry ID            -> words indexed (4 bytes)
 *             0                   -> live documents (8), words in all (8)
 * Terms are keyed with cdb_key_string, so a term's postings are contiguous
 * and ordered by entry ID.
 *
 * Words: the simple splitter (splitter 1) breaks text on ASCII characters
 * other than letters and digits and on common Unicode punctuation and
 * spaces, folds ASCII letters to lower case and cuts words to 64 bytes on a
 * character boundary. No stemming, no stop words.
 *
 * The index changes in the document apply, at commit, from the document's
 * state before and after: it never fails for logical reasons.
 *
 * Buffers: cdb_db_config.fts_buffers holds cdb_fts_buffers_size() bytes: a
 * document buffer (cdb_doc.h) and two page_size buffers. fts_tokens holds
 * fts_tokens_cap words, at least cdb_fts_tokens_needed(): a version of a
 * document has fewer words than bytes (body up to doc_max, or one page in
 * all; summary within a page), so both versions of a changed document always
 * fit and no word is ever left out. Pools are refused with a smaller array. */
#ifndef CDB_FTS_H
#define CDB_FTS_H

#include "cdb_db.h"

#define CDB_FTS_SPLITTER_SIMPLE 1u
#define CDB_FTS_MAX_WORD 64u
#define CDB_FTS_MAX_QUERY_TERMS 16u

/* Search modes. */
#define CDB_FTS_ANY 0
#define CDB_FTS_ALL 1

typedef struct cdb_fts_hit {
    uint64_t entry;
    double score;
} cdb_fts_hit;

/* A document version as the index sees it; body NULL = not live. */
typedef struct cdb_fts_doc {
    const uint8_t *body;
    size_t body_len;
    const uint8_t *summary;
    size_t summary_len;
} cdb_fts_doc;

/* Splits text into words, appending at most cap - *n of them to out.
 * Returns the number of words found, including any that did not fit. */
size_t cdb_fts_split(const uint8_t *text, size_t len, cdb_fts_token *out, size_t cap, size_t *n);

/* The indexed words of a document, sorted and grouped (tf set). *total is
 * the number of words indexed, counting repeats. */
cdb_status cdb_fts_collect(const cdb_fts_config *cfg, const cdb_fts_doc *d, cdb_byte_order order,
                           cdb_fts_token *out, size_t cap, size_t *unique, size_t *total);

/* Compares words as folded bytes. */
int cdb_fts_word_cmp(const cdb_fts_token *a, const cdb_fts_token *b);

/* Settings encoding (spec 14.14), kept in the store record. */
cdb_status cdb_fts_check_config(const cdb_fts_config *cfg);
cdb_status cdb_fts_encode(const cdb_fts_config *cfg, uint64_t terms, uint64_t postings, uint64_t lengths,
                          cdb_byte_order order, uint8_t *out, size_t cap, size_t *len);
cdb_status cdb_fts_decode(const uint8_t *in, size_t len, cdb_byte_order order, cdb_fts_config *cfg,
                          uint64_t *terms, uint64_t *postings, uint64_t *lengths);

/* Buffer sizes for a page size and doc_max (cdb_doc.h). */
size_t cdb_fts_buffers_size(size_t page_size, size_t doc_max);
size_t cdb_fts_tokens_needed(size_t page_size, size_t doc_max);

/* Index maintenance and search, for cdb_doc. */
cdb_status cdb_fts_create(cdb_db *db, struct cdb_doc_pool *pool);
cdb_status cdb_fts_update(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry,
                          const cdb_fts_doc *before, const cdb_fts_doc *after);
cdb_status cdb_fts_search(cdb_db *db, struct cdb_doc_pool *pool, const char *query, size_t len, int mode,
                          cdb_fts_hit *hits, size_t max, size_t *n);

#endif
