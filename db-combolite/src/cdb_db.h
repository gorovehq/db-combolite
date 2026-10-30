/* Database: archive, working directory, transactions and recovery
 * (spec 1.3, 1.12, 3, 11.3, 14.1.8, 14.4). Single process for now.
 *
 * At rest a database is <base>.cdbarch. While open it also has the working
 * directory <base>.cdbwork/ holding the main journal main.0.cdb. A
 * transaction's changes are appended to the journal; once its commit record
 * is written they are applied to the pager through the apply hook. Applied
 * changes reach the archive at reconciliation, which commits the pager with
 * the journal's watermark and starts a fresh journal. On open, committed
 * transactions above the watermark are replayed (spec 3.3). */
#ifndef CDB_DB_H
#define CDB_DB_H

#include "combolite/cdb_env.h"
#include "cdb_catalog.h"
#include "cdb_journal.h"
#include "cdb_pager.h"

#define CDB_DB_PATH_MAX 200
#define CDB_DB_MAX_POOLS 8
#define CDB_COMMIT_RESERVE_BLOCK 1024u

/* Durability levels (spec 3.1). */
typedef enum cdb_durability {
    CDB_DURABILITY_DEFAULT = 0, /* use the database setting */
    CDB_DURABILITY_FULL,        /* acknowledged after the commit record's barrier */
    CDB_DURABILITY_NORMAL,      /* barriers every normal_sync_every commits */
    CDB_DURABILITY_OFF          /* barriers only at reconciliation */
} cdb_durability;

/* Open the database even if a companion is missing or set aside and data may
 * have been lost (spec 1.12.5). The operator accepts the loss. */
#define CDB_DB_ACCEPT_LOSS 0x1u

typedef struct cdb_delta {
    uint64_t store_id;
    uint64_t entry_id;
    uint8_t op;          /* CDB_OP_* */
    const uint8_t *data;
    size_t len;
    uint64_t commit_number; /* of the transaction this change belongs to */
} cdb_delta;

/* Full-text search (spec 8.10, 14.14; cdb_fts.h). */
#define CDB_FTS_MAX_PATHS 8u
#define CDB_FTS_MAX_DEPTH 8u

typedef struct cdb_fts_path {
    uint64_t ids[CDB_FTS_MAX_DEPTH]; /* field IDs through nested objects */
    unsigned depth;
} cdb_fts_path;

typedef struct cdb_fts_config {
    cdb_fts_path paths[CDB_FTS_MAX_PATHS];
    unsigned npaths;
    int summary;                     /* index the summary too */
} cdb_fts_config;

/* A word in caller memory; tf counts its occurrences once grouped. */
typedef struct cdb_fts_token {
    const uint8_t *p;
    uint32_t len;
    uint32_t tf;
} cdb_fts_token;

/* Applies one committed change to the pages. It must be deterministic:
 * recovery replays the same changes in the same order. */
typedef cdb_status (*cdb_apply_fn)(void *ctx, cdb_pager *pager, const cdb_delta *d);

typedef struct cdb_db_config {
    cdb_pager_config pager;
    uint8_t *record_buf;          /* largest record: 64 bytes + payload */
    size_t record_buf_size;
    cdb_apply_fn apply;           /* NULL: the built-in document apply (spec 8) */
    void *apply_ctx;
    cdb_durability durability;    /* default level; DEFAULT means NORMAL (spec 3.1) */
    unsigned normal_sync_every;   /* 0 means 32 */
    unsigned flags;
    cdb_index_work catalog_work;  /* optional: the catalog is available when set */
    uint8_t *catalog_page;        /* page_size bytes, with catalog_work */
    uint8_t *doc_buffers;         /* documents: cdb_doc_buffers_size() bytes (cdb_doc.h) */
    size_t doc_max;               /* documents: largest body in bytes, kept in overflow pages
                                     past the revision page; 0 means a body fits one page */
    unsigned checkpoint_every;    /* documents: full revision every N; 0 means 16 */
    uint8_t *doc_stage;           /* documents: per-transaction staging (cdb_doc.h) */
    size_t doc_stage_size;
    uint8_t *fts_buffers;         /* full-text: cdb_fts_buffers_size() bytes (cdb_fts.h) */
    uint8_t *vec_buffers;         /* vectors: 3 * page_size bytes (cdb_vec.h) */
    uint8_t *vec_work;            /* vectors with a neighbour graph: working memory (cdb_vec.h) */
    size_t vec_work_size;
    unsigned vec_refresh_budget;  /* graph entries refreshed per reconciliation; 0 means 256 */
    unsigned vec_ef;              /* approximate search: candidates kept; 0 means 64 */
    uint64_t vec_exact_below;     /* automatic path: exact below this many vectors; 0 means 10000 */
    unsigned vec_mode;            /* operator's search path: CDB_VEC_AUTO, _EXACT or _APPROX */
    cdb_fts_token *fts_tokens;    /* full-text: words of two document versions */
    size_t fts_tokens_cap;        /* at least cdb_fts_tokens_needed() */
} cdb_db_config;

typedef struct cdb_db {
    cdb_env env;
    cdb_db_config cfg;
    char parent[CDB_DB_PATH_MAX];
    char archive[CDB_DB_PATH_MAX];
    char workdir[CDB_DB_PATH_MAX];
    char journal_path[CDB_DB_PATH_MAX];
    char journal_tmp[CDB_DB_PATH_MAX];
    cdb_storage archive_st;
    cdb_storage journal_st;
    cdb_pager pager;
    cdb_journal journal;
    uint64_t next_commit; /* next global commit number to hand out */
    uint64_t commit_hwm;  /* numbers below this are reserved durably */
    uint64_t applied;     /* highest commit number applied to the pager */
    uint64_t next_tx;
    int in_tx;
    uint64_t tx_id;
    uint64_t tx_start;
    uint32_t tx_deltas;
    cdb_durability tx_durability;
    unsigned unsynced;
    int failed;           /* an apply failed: the database must be reopened */
    cdb_catalog catalog;
    int has_catalog;
    /* Open document pools (cdb_doc.h). */
    struct cdb_doc_pool {
        int in_use;
        int dirty;
        uint64_t store_id;
        cdb_index head;
        cdb_index revs;
        int have_indexes;
        uint64_t next_entry;
        uint32_t schema_version; /* current; 0 = no schema */
        int fts_on;              /* the pool has a full-text index */
        cdb_fts_config fts;
        cdb_index fts_terms;     /* term -> document frequency */
        cdb_index fts_postings;  /* (term, entry) -> term frequency */
        cdb_index fts_lengths;   /* entry -> words indexed; 0 -> pool totals */
    } pools[CDB_DB_MAX_POOLS];
    /* Open vector collections (cdb_vec.h). */
    struct cdb_vec_coll {
        int in_use;
        int dirty;               /* next_entry changed since the catalog has it */
        uint64_t store_id;
        uint8_t elem;            /* cdb_vector_elem */
        uint8_t measure;         /* base measure, CDB_VEC_* */
        uint8_t norms;           /* blocks cache each vector's norm */
        uint32_t dims;
        size_t vec_bytes;        /* bytes per vector */
        uint32_t slots;          /* vectors per block */
        cdb_index entries;       /* entry -> block, slot, source */
        cdb_index blocks;        /* block -> slots in use, slots */
        cdb_index sources;       /* (source store, source entry, entry) -> nothing */
        uint64_t next_entry;
        uint64_t touched_tx;     /* the open transaction changed this collection */
        uint64_t free_from;      /* blocks below this ID have no free slot: a hint, kept in memory */
        int graph_on;            /* it has a neighbour graph (spec 6.4, 7.4) */
        uint32_t k;              /* neighbours per entry */
        cdb_index graph;         /* entry -> neighbour record; 0 -> live count */
        cdb_index reverse;       /* (neighbour, entry): entry lists neighbour; (0, entry): an entry point */
        cdb_index pending;       /* entries whose list needs work */
    } vecs[CDB_DB_MAX_POOLS];
    /* The one link store (cdb_link.h), looked up on first use. */
    struct cdb_link_store {
        int looked;              /* the catalog was asked for it */
        uint64_t store_id;       /* 0: there is none yet */
        int open;                /* its indexes are open */
        cdb_index fwd;           /* (source, type, target) -> weight, time, flags */
        cdb_index rev;           /* (target, type, source) -> nothing */
    } links;
    /* The schema held in the schema buffer (cdb_doc.h). */
    uint64_t schema_store;
    uint32_t schema_version;
    size_t schema_len;
    int schema_trivial;   /* it accepts every document */
    size_t stage_used;    /* bytes of doc_stage in use by the open transaction */
    int stage_overflow;   /* a change could not be staged: consult the journal */
    int set_aside;        /* recovery set a companion aside (spec 1.12.2) */
} cdb_db;

/* Opens <base>.cdbarch, recovering if needed, and starts a session. If the
 * archive does not exist and create is non-NULL, a new database is created
 * with those settings; otherwise CDB_E_NOT_FOUND. */
cdb_status cdb_db_open(cdb_db *db, const cdb_env *env, const char *base, const cdb_db_config *cfg,
                       const cdb_header *create);

/* Reconciles everything and leaves the database as one file at rest. */
cdb_status cdb_db_close(cdb_db *db);

cdb_status cdb_tx_begin(cdb_db *db, cdb_durability durability);
cdb_status cdb_tx_put(cdb_db *db, uint64_t store_id, uint64_t entry_id, uint8_t op,
                      const void *data, size_t len);
cdb_status cdb_tx_commit(cdb_db *db);
cdb_status cdb_tx_abort(cdb_db *db);

/* Folds every applied change into the archive (spec 3.2) and starts a new journal. */
cdb_status cdb_db_reconcile(cdb_db *db);

/* The environment's clock, in nanoseconds since the Unix epoch. *have is 0
 * (and *ns 0) when the environment has none. Commit records carry it. */
cdb_status cdb_db_now(cdb_db *db, int64_t *ns, int *have);

/* Catalog changes (spec 4). Each reconciles first, then commits the change
 * immediately. Need catalog_work and catalog_page in the configuration. */
cdb_status cdb_db_create_store(cdb_db *db, const char *name, size_t len, uint32_t model, uint64_t *id);
cdb_status cdb_db_find_store(cdb_db *db, const char *name, size_t len, uint64_t *id, int *found);
cdb_status cdb_db_drop_store(cdb_db *db, uint64_t id);
cdb_status cdb_db_add_schema(cdb_db *db, uint64_t store_id, const void *schema, size_t len, uint32_t *version);

/* For catalog changes made by the data models: begin reconciles; the caller
 * then changes pages and commits through the catalog; end starts the new
 * journal if s is CDB_OK and returns s otherwise. */
cdb_status cdb_db_catalog_begin(cdb_db *db);
cdb_status cdb_db_catalog_end(cdb_db *db, cdb_status s);

/* Maintenance (spec 3.4, 3.7): reconciles, moves up to max_moves live pages
 * down into reusable space, and releases the free tail of the archive. */
cdb_status cdb_db_compact(cdb_db *db, uint64_t max_moves, uint64_t *moved, uint64_t *reclaimed);

#endif
