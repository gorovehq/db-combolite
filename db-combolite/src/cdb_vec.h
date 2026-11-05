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
 * Neighbour graph (spec 6.4, 6.8, 7.4): optional, added with
 * cdb_vec_create_graph. Each entry's record lists its K nearest neighbours
 * under the base measure, nearest first (spec 14.12.4):
 *   graph:   entry ID -> record; 0 -> live count
 *   reverse: (neighbour, entry) -> nothing: entry lists neighbour;
 *            (0, entry) -> nothing: entry is an entry point
 *   pending: entry ID -> nothing: its list needs work
 * A commit only queues work: changed entries become pending, and a deleted
 * or replaced vector is removed from every list that held it, queuing those
 * entries too. Reconciliation refreshes up to vec_refresh_budget pending
 * entries per collection; cdb_vec_refresh does the rest. A fresh list is
 * exact: the K nearest live vectors, ties to the lower entry ID.
 *
 * Buffers: cdb_db_config.vec_buffers holds three page_size buffers. A
 * collection with a graph also needs vec_work (cdb_vec_work_size). */
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
#define CDB_VEC_MAX_K 64u
#define CDB_VEC_DEFAULT_K 16u

/* Search paths (spec 7.7). AUTO: exact below vec_exact_below vectors, for
 * a measure other than the base one, or without a graph; else approximate. */
#define CDB_VEC_AUTO 0u
#define CDB_VEC_EXACT 1u
#define CDB_VEC_APPROX 2u

/* A collection's fixed settings. The model strings are the caller's
 * (copied at create; filled with pointers to the caller's buffers by
 * describe). */
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

/* One search result (or one stored neighbour). */
typedef struct cdb_vec_hit {
    uint64_t entry;
    double distance;           /* under the measure searched with; smaller is closer */
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
 * replace keeps the source and records its current revision (re-embedding).
 * Errors as for documents: CDB_E_STATE outside a transaction,
 * CDB_E_INVALID_ARG for a vector of the wrong shape, CDB_E_NOT_FOUND for
 * replace or delete of an entry that is not live as the transaction sees
 * it; delete also refuses (CDB_E_STATE) while a refuse-type link points at
 * the entry. */
cdb_status cdb_vec_insert(cdb_db *db, uint64_t store, const void *vec, size_t len,
                          uint64_t source_store, uint64_t source_entry, uint64_t *entry);
cdb_status cdb_vec_replace(cdb_db *db, uint64_t store, uint64_t entry, const void *vec, size_t len);
cdb_status cdb_vec_delete(cdb_db *db, uint64_t store, uint64_t entry);

/* Whether a vector exists as the open transaction sees it (for links).
 * CDB_E_INVALID_ARG: not a vector collection. */
cdb_status cdb_vec_live_in_tx(cdb_db *db, uint64_t store, uint64_t entry, int *live);

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

/* As cdb_vec_search, choosing the path: CDB_VEC_AUTO defers to the
 * operator's vec_mode, then to the automatic rule. APPROX without a graph
 * searches exactly. The approximate path walks the graph from its entry
 * points under the base measure, adds pending vectors exactly, and ranks
 * what it found with the requested measure. */
cdb_status cdb_vec_search_mode(cdb_db *db, uint64_t store, const void *query, size_t len, unsigned measure,
                               unsigned mode, cdb_vec_hit *hits, size_t k, size_t *n);

/* Adds a neighbour graph with k neighbours per entry (0: 16) and queues
 * every vector; commits immediately. CDB_E_EXISTS if there is one,
 * CDB_E_OVERFLOW if a record of k neighbours does not fit an index value. */
cdb_status cdb_vec_create_graph(cdb_db *db, uint64_t store, uint32_t k);

/* Refreshes up to budget pending entries (0: all) and commits. *remaining
 * (if not NULL) is how many stay pending. Not inside a transaction. */
cdb_status cdb_vec_refresh(cdb_db *db, uint64_t store, size_t budget, size_t *remaining);

/* Asynchronous neighbour-graph refresh task and dispatch (spec 5.9.6, M20). */
typedef struct cdb_vec_refresh_task {
    cdb_db *db;
    uint64_t store;
    size_t budget;
    size_t remaining;
    cdb_status status;
} cdb_vec_refresh_task;

/* Submits a neighbour-graph refresh to the database's background worker pool (spec 5.9.6, M20).
 * If pool has worker threads, executes asynchronously; otherwise executes immediately
 * on the caller thread. When task completes, task->status holds the refresh result
 * and task->remaining holds the count of entries still pending. */
cdb_status cdb_vec_refresh_async(cdb_db *db, cdb_vec_refresh_task *task, uint64_t *task_id);

/* The stored ranking of an entry (spec 6.4): its neighbours, nearest
 * first. *fresh is 0 while the entry is pending (its list may be empty or
 * incomplete). CDB_E_NOT_FOUND for an entry that does not exist. */
cdb_status cdb_vec_neighbours(cdb_db *db, uint64_t store, uint64_t entry, cdb_vec_hit *hits, size_t max,
                              size_t *n, int *fresh);

/* ---- Change log and named change cursors (spec 14.12.5, 18.12) ----
 *
 * Every collection created now keeps a change log: which entries each
 * archive commit inserted, replaced or deleted, keyed by that commit's
 * sequence. Reconciliation trims it below the readers' floor and the lowest
 * named cursor, so an application that wants the changes since it last
 * looked, across sessions, holds them with a named cursor. */

#define CDB_VEC_CURSOR_NAME_MAX 64u

/* One changed entry: the sequence of the archive commit that changed it,
 * and whether it is live now (0: deleted since). An entry changed under
 * several sequences is listed once for each. */
typedef struct cdb_vec_change {
    uint64_t seq;
    uint64_t entry;
    int live;
} cdb_vec_change;

/* Adds a change log to a collection created before logs existed; it holds
 * every change from now on. Commits immediately. CDB_E_EXISTS if it has
 * one. */
cdb_status cdb_vec_track_changes(cdb_db *db, uint64_t store);

/* The collection's change stamp, the sequence of its newest change (or its
 * trimmed-through sequence when no change is kept), and *oldest, the
 * sequence the log holds every change after. CDB_E_STATE without a log. */
cdb_status cdb_vec_change_stamp(cdb_db *db, uint64_t store, uint64_t *stamp, uint64_t *oldest);

/* Lists the changes after (*seq, *entry), in that order, at most max of
 * them; *n says how many. Start with *seq the sequence to read after and
 * *entry UINT64_MAX; both are advanced past what is returned, so call
 * again until *n is 0. CDB_E_STATE without a log, or when the log no
 * longer reaches back to *seq (it was trimmed: read the collection whole
 * instead). Not inside a transaction. */
cdb_status cdb_vec_changes(cdb_db *db, uint64_t store, uint64_t *seq, uint64_t *entry, cdb_vec_change *out,
                           size_t max, size_t *n);

/* Opens the named cursor, creating it at the change stamp if it does not
 * exist; *seq is the sequence it has read up to. Commits immediately when
 * it creates. Names are 1 to CDB_VEC_CURSOR_NAME_MAX bytes. CDB_E_STATE
 * without a log. */
cdb_status cdb_vec_cursor_open(cdb_db *db, uint64_t store, const char *name, size_t len, uint64_t *seq);

/* Advances the named cursor to seq, in the open transaction: one ordinary
 * commit, no reconciliation (spec 14.12.5). A lower seq changes nothing.
 * CDB_E_NOT_FOUND if there is no such cursor. */
cdb_status cdb_vec_cursor_advance(cdb_db *db, uint64_t store, const char *name, size_t len, uint64_t seq);

/* Drops the named cursor, so it no longer holds the log. Commits
 * immediately. CDB_E_NOT_FOUND if there is no such cursor. */
cdb_status cdb_vec_cursor_drop(cdb_db *db, uint64_t store, const char *name, size_t len);

/* ---- Accelerator mirrors (spec 18), host profile; no-ops on mcu ---- */

/* Fills the mirrors searches have asked for; run at reconciliation and at
 * each cdb_db_tick. */
cdb_status cdb_vec_accel_maintain(cdb_db *db);
/* At the end of a rebase: catches each mirror up from its change log. */
cdb_status cdb_vec_accel_rebase(cdb_db *db);
/* After this process's own reconciliation has committed the archive. */
void cdb_vec_accel_committed(cdb_db *db);
/* The oldest archive sequence a ready mirror reflects (UINT64_MAX: none):
 * the reader pin is held at or below it (spec 18.12). */
uint64_t cdb_vec_accel_floor(const cdb_db *db);
/* The store was dropped: its mirror goes. */
void cdb_vec_accel_forget(cdb_db *db, uint64_t store);
/* At close: every mirror is closed. */
void cdb_vec_accel_close(cdb_db *db);
/* The distance the CPU path computes between a query and a stored vector,
 * both in their stored form: for an accelerator, or a test, to compare
 * its own against. */
double cdb_vec_cpu_distance(unsigned elem, uint32_t dims, unsigned measure, const uint8_t *q, const uint8_t *v,
                            cdb_byte_order o);

/* Bytes of vec_work a collection with this shape and k needs. */
size_t cdb_vec_work_size(cdb_vector_elem elem, uint32_t dims, uint32_t k);

/* Engine hooks used by the built-in apply and reconciliation. *handled is
 * 0 when the store is not a vector collection. */
int cdb_vec_is_open(const cdb_db *db, uint64_t store);
cdb_status cdb_vec_apply(cdb_db *db, const cdb_delta *d, int *handled);
cdb_status cdb_vec_flush(cdb_db *db);
extern const cdb_slot_ops cdb_vec_slot_ops; /* open collections (cdb_slots.h) */

#endif
