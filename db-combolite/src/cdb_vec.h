/* Vector collections (spec 7, 14.12).
 *
 * A vector collection is a catalog store with the vector model. Its element
 * type, dimension count, base measure and embedding model (name and
 * version) are fixed when it is created (spec 7.1-7.3). Each entry holds one
 * vector (spec 7.5) and may record the source it was computed from, such as
 * a document: (source store, source entry). For a document source the
 * document's revision at the vector's commit is recorded too, so search can
 * tell whether the vector still describes its source. Vectors are not
 * removed with their source (spec 6.7 keeps references by default).
 *
 * Vectors are packed into vector blocks (logical pages of type 0x0006,
 * spec 14.12.1). Three ordered indexes hold the rest:
 *   entries: entry ID -> block, slot, source, source revision (36 bytes)
 *   blocks:  block ID -> slots in use, slots (8 bytes)
 *   sources: (source store, source entry, entry ID) -> nothing
 * Vectors are derived data and keep no history: replace overwrites and
 * delete removes.
 *
 * Changes go through transactions and are applied at commit by the
 * built-in apply, which never fails for logical reasons. Search is exact:
 * every vector is compared with the query (spec 7.4).
 *
 * Buffers: cdb_db_config.vec_buffers holds three page_size buffers. */
#ifndef CDB_VEC_H
#define CDB_VEC_H

#include "combolite/cdb_value.h"
#include "cdb_db.h"

/* Measures (spec 7.7). Distances: smaller is closer. */
#define CDB_VEC_BASE 0u       /* in a query: the collection's base measure */
#define CDB_VEC_COSINE 1u     /* 1 - cosine similarity */
#define CDB_VEC_DOT 2u        /* -dot product */
#define CDB_VEC_EUCLIDEAN 3u  /* L2 distance */
#define CDB_VEC_HAMMING 4u    /* differing bits; binary vectors only */

#define CDB_VEC_MODEL_MAX 64u
#define CDB_VEC_MAX_DIMS 65535u

typedef struct cdb_vec_config {
    cdb_vector_elem elem;
    uint32_t dims;
    unsigned measure;          /* base measure */
    const char *model;         /* embedding model and version (spec 7.3) */
    size_t model_len;
    const char *model_version;
    size_t model_version_len;
} cdb_vec_config;

/* The state of a hit's source, as of the search. */
#define CDB_VEC_SOURCE_NONE 0u      /* the vector has no source */
#define CDB_VEC_SOURCE_CURRENT 1u   /* the document is unchanged since the vector was written */
#define CDB_VEC_SOURCE_OUTDATED 2u  /* the document has changed since: re-embed it */
#define CDB_VEC_SOURCE_DELETED 3u   /* the document, or its pool, is gone */
#define CDB_VEC_SOURCE_UNCHECKED 4u /* the source is not a document */

typedef struct cdb_vec_hit {
    uint64_t entry;
    double distance;
    uint64_t source_store;
    uint64_t source_entry;
    unsigned source_state;     /* CDB_VEC_SOURCE_* */
} cdb_vec_hit;

/* Creates a collection; commits immediately, like other catalog changes.
 * CDB_E_OVERFLOW if one vector does not fit in a page. */
cdb_status cdb_vec_create(cdb_db *db, const char *name, size_t len, const cdb_vec_config *cfg, uint64_t *id);

/* Copies the collection's settings; model and version go to the caller's
 * buffers (CDB_VEC_MODEL_MAX bytes each) and are not NUL-terminated. */
cdb_status cdb_vec_describe(cdb_db *db, uint64_t store, cdb_vec_config *cfg,
                            char *model, char *model_version);

/* Inside a transaction. vec is an encoded vector value (spec 14.6) with the
 * collection's element type and dimensions. source_store 0: no source.
 * replace keeps the source and records its current revision (re-embedding). */
cdb_status cdb_vec_insert(cdb_db *db, uint64_t store, const void *vec, size_t len,
                          uint64_t source_store, uint64_t source_entry, uint64_t *entry);
cdb_status cdb_vec_replace(cdb_db *db, uint64_t store, uint64_t entry, const void *vec, size_t len);
cdb_status cdb_vec_delete(cdb_db *db, uint64_t store, uint64_t entry);

/* Copies the committed vector as an encoded value, and its source. */
cdb_status cdb_vec_get(cdb_db *db, uint64_t store, uint64_t entry, void *out, size_t cap, size_t *len,
                       uint64_t *source_store, uint64_t *source_entry);

/* The entries whose source is (source_store, source_entry), ascending; at
 * most max of them, *n reports how many were copied. */
cdb_status cdb_vec_by_source(cdb_db *db, uint64_t store, uint64_t source_store, uint64_t source_entry,
                             uint64_t *entries, size_t max, size_t *n);

/* The k nearest committed vectors to query, nearest first (equal
 * distances: lower entry ID first). measure CDB_VEC_BASE uses the
 * collection's base measure. */
cdb_status cdb_vec_search(cdb_db *db, uint64_t store, const void *query, size_t len, unsigned measure,
                          cdb_vec_hit *hits, size_t k, size_t *n);

/* Engine hooks used by the built-in apply and reconciliation. *handled is
 * 0 when the store is not a vector collection. */
int cdb_vec_is_open(const cdb_db *db, uint64_t store);
cdb_status cdb_vec_apply(cdb_db *db, const cdb_delta *d, int *handled);
cdb_status cdb_vec_flush(cdb_db *db);

#endif
