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
#include "combolite/cdb_crypto.h"
#include "combolite/cdb_blob.h"
#include "combolite/cdb_crdt.h"
#include "cdb_audit.h"
#include "cdb_catalog.h"
#include "cdb_coord.h"
#include "cdb_journal.h"
#include "cdb_pager.h"
#include "cdb_slots.h"
#include "cdb_worker.h"
#include "cdb_trigger.h"

/* Staging hash table for transactions (spec 14.9, 14.4). */
#define CDB_STAGE_BUCKETS 512u
#define CDB_STAGE_IDX_BUCKETS 512u
#define CDB_STAGE_NIL 0xFFFFFFFFu

#define CDB_DB_PATH_MAX 200         /* longest path, including the terminating zero */

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

/* One committed change, as handed to the apply hook. data points into the
 * database's record buffer and is valid only during the call. */
typedef struct cdb_delta {
    uint64_t store_id;
    uint64_t entry_id;
    uint8_t op;          /* CDB_OP_* */
    const uint8_t *data; /* the model-specific payload after the op byte */
    size_t len;
    uint64_t commit_number; /* of the transaction this change belongs to */
} cdb_delta;

/* Full-text search (spec 8.10, 14.14; cdb_fts.h). */
#define CDB_FTS_MAX_PATHS 8u
#define CDB_FTS_MAX_DEPTH 8u

/* Which field of a document to index, given as the chain of field IDs from
 * the top-level object down through nested objects. */
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
 * recovery replays the same changes in the same order. It must also not
 * fail for logical reasons (a missing entry, a schema mismatch): those must
 * be caught before the change is journaled. A failure here marks the
 * database failed until it is reopened. */
typedef cdb_status (*cdb_apply_fn)(void *ctx, cdb_pager *pager, const cdb_delta *d);

/* Everything the database needs from the caller. All buffers are owned by
 * the caller and must stay valid, and unused by anything else, until
 * cdb_db_close returns. The struct itself is copied at open. */
typedef struct cdb_db_config {
    cdb_pager_config pager;
    uint8_t *record_buf;          /* largest record: 64 bytes + payload */
    size_t record_buf_size;
    cdb_apply_fn apply;           /* NULL: the built-in document apply (spec 8) */
    void *apply_ctx;
    cdb_durability durability;    /* default level; DEFAULT means NORMAL (spec 3.1) */
    unsigned normal_sync_every;   /* 0 means 32 */
    unsigned flags;               /* CDB_DB_ACCEPT_LOSS */
    cdb_index_work catalog_work;  /* the catalog, and the reconciliation marks (spec 14.4.8) */
    uint8_t *catalog_page;        /* page_size bytes, with catalog_work */
    uint8_t *doc_buffers;         /* documents: cdb_doc_buffers_size() bytes (cdb_doc.h) */
    size_t doc_max;               /* documents: largest body in bytes, kept in overflow pages
                                     past the revision page; 0 means a body fits one page */
    unsigned checkpoint_every;    /* documents: full revision every N; 0 means 16 */
    uint8_t *doc_stage;           /* documents: per-transaction staging (cdb_doc.h) */
    size_t doc_stage_size;
    uint8_t *idx_stage;           /* index & link staging: table unique checks & links */
    size_t idx_stage_size;
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
    uint8_t *ts_buffers;          /* time series: 3 * page_size bytes (cdb_ts.h) */
    uint8_t *ts_work;             /* time series: cdb_ts_work_size() bytes, for late points and reads */
    size_t ts_work_size;
    /* Time series: called when a point replaces one with the same timestamp
     * (spec 10.6), at apply, so again on recovery. Optional. */
    void (*ts_collision)(void *ctx, uint64_t store, uint64_t series, int64_t ts);
    void *ts_collision_ctx;
    uint8_t *table_buffers;       /* tables: cdb_table_buffers_size() bytes (cdb_table.h) */
    uint8_t *col_work;            /* column-oriented tables: cdb_rg_work_size() bytes for the
                                     largest group size and cap in use (cdb_rowgroup.h) */
    size_t col_work_size;
    /* Open stores (cdb_slots.h): the most document pools, vector
     * collections and time-series stores, each, open at once; 0: no cap.
     * Slots grow with use, in chunks of 4, from slot_mem first and then
     * the environment's allocator; past the cap the store used longest ago
     * is closed. A slot of each kind together is about 1.9 KB on a 64-bit
     * host (cdb_db_slot_mem_size). Recommended caps (estimates until the
     * host benchmarking tool in the backlog measures them):
     *   micro-controller    4, all in slot_mem (no allocator)
     *   single-board (SBC)  8
     *   budget PC           16
     *   workstation         0 (no cap), or 32 to bound memory
     *   server              0 (no cap) */
    unsigned max_open;
    cdb_slot_align *slot_mem;     /* optional caller memory for slots */
    size_t slot_mem_size;         /* in bytes */
    cdb_coord_config coord;       /* coordinators (spec 5.9): zeros take the defaults */
    unsigned worker_threads;      /* background worker pool threads; 0 means synchronous/disabled */
    /* Security & Encryption (spec 12). */
    const char *master_key_uri;
    int is_production;
    uint8_t aead_alg;
    cdb_key_provider_fn key_provider;
    void *key_provider_ctx;
    uint32_t rotation_interval_days;
    cdb_rotation_policy rotation_policy;
    cdb_signing_policy signing_policy;
    const cdb_identity *identity;
} cdb_db_config;

/* An open document pool (cdb_doc.h). */
struct cdb_doc_pool {
    cdb_slot slot;           /* first: in_use, store_id, use (cdb_slots.h) */
    int dirty;
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
    int is_table;            /* a relational table (cdb_table.h): only the table layer may write it */
    uint32_t keep;           /* tables without history: revisions kept per row (spec 9.11); 0 = all */
};

/* An open vector collection (cdb_vec.h). */
struct cdb_vec_coll {
    cdb_slot slot;           /* first: in_use, store_id, use (cdb_slots.h) */
    int dirty;               /* next_entry changed since the catalog has it */
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
};

/* An open time-series store (cdb_ts.h). */
struct cdb_ts_store {
    cdb_slot slot;           /* first: in_use, store_id, use (cdb_slots.h) */
    int dirty;               /* next_entry changed since the catalog has it */
    int held_any;            /* late points may be held, to fold in at reconciliation */
    uint64_t next_entry;     /* the next series ID */
    uint8_t late_mode;       /* CDB_TS_LATE_* */
    int64_t lateness;        /* ns, for CDB_TS_LATE_WITHIN */
    cdb_index keys;          /* series key -> series ID */
    cdb_index series;        /* series ID -> series record */
    cdb_index blocks;        /* (series, first timestamp) -> block page, count, last timestamp */
    cdb_index held;          /* (series, timestamp) -> a late point held until reconciliation */
    uint64_t touched_tx;     /* the open transaction changed this store */
};

/* An open table (cdb_table.h): its current columns and its indexes. The row
 * data is the document pool's; this is the table layer's own state, parsed
 * from the schema and settings. */
#define CDB_TABLE_MAX_COLUMNS 64u
#define CDB_TABLE_MAX_INDEXES 8u
#define CDB_TABLE_MAX_KEY_COLUMNS 8u
#define CDB_TABLE_DEFAULTS_MAX 1024u  /* all encoded default values together */

struct cdb_table_store {
    cdb_slot slot;           /* first: in_use, store_id, use (cdb_slots.h) */
    uint32_t version;        /* schema version the columns come from; 0: no columns yet */
    uint32_t keep;           /* revisions kept per row; 0 = all */
    uint32_t next_index;     /* the number the next created index gets */
    uint8_t layout;          /* 0 row-oriented, 1 column-oriented */
    uint32_t group_rows;     /* column-oriented: most rows per group */
    uint32_t chunk_cap;      /* column-oriented: most value bytes per column per group */
    cdb_index groups;        /* column-oriented: row groups by last entry ID */
    cdb_index pending;       /* column-oriented: changes not yet in a group */
    unsigned ncols;
    struct cdb_table_col {
        uint64_t id;         /* field ID */
        uint32_t added;      /* schema version that added it */
        uint16_t def_off;    /* default value in defaults[], if def_len */
        uint16_t def_len;
        uint8_t type;        /* cdb_value_type */
        uint8_t not_null;
    } cols[CDB_TABLE_MAX_COLUMNS]; /* in field-ID order */
    uint8_t defaults[CDB_TABLE_DEFAULTS_MAX];
    unsigned npk;
    uint64_t pk[CDB_TABLE_MAX_KEY_COLUMNS];
    unsigned nidx;
    struct cdb_table_idx {
        uint32_t number;     /* stable index number (0: the primary key) */
        uint8_t unique;
        unsigned n;
        uint64_t fields[CDB_TABLE_MAX_KEY_COLUMNS];
        cdb_index ix;
    } idx[CDB_TABLE_MAX_INDEXES];
    size_t before_len;       /* the row before an apply (cdb_table_apply_before) */
    uint32_t before_schema;
    int before_live;
};

/* Caller memory for n open stores of each kind, without an allocator. */
size_t cdb_db_slot_mem_size(unsigned n);

/* An open database. All fields are internal to the engine. */
typedef struct cdb_db {
    cdb_env env;
    cdb_db_config cfg;
    char parent[CDB_DB_PATH_MAX];       /* directory holding the archive */
    char archive[CDB_DB_PATH_MAX];      /* <base>.cdbarch */
    char workdir[CDB_DB_PATH_MAX];      /* <base>.cdbwork */
    char journal_path[CDB_DB_PATH_MAX]; /* <base>.cdbwork/main.0.cdb */
    char journal_tmp[CDB_DB_PATH_MAX];  /* the same with .new, used while rotating */
    char coorddir[CDB_DB_PATH_MAX];     /* <base>.cdbwork/coord */
    cdb_coord coord;                    /* this process's coordinator (spec 5) */
    cdb_storage archive_st;
    cdb_storage journal_st;
    cdb_pager pager;
    cdb_journal journal;
    /* Sharing the archive with other processes (spec 5.9). */
    char overlay_path[CDB_DB_PATH_MAX]; /* <base>.cdbwork/pages.<slot>.cdb (spec 5.9.1) */
    cdb_storage overlay_st;
    int have_overlay;     /* overlay_st is open */
    cdb_storage rj_st;    /* reads journals while recovering or merging */
    cdb_journal rj;
    int fresh;            /* the pager holds the archive's last commit plus only this
                             process's own transactions, so it may be committed as is */
    uint64_t pins_seq;    /* committed sequence at which the others' pins were last read */
    uint64_t applied;     /* highest commit number applied to the pager */
    uint64_t next_tx;     /* counter part of the next transaction ID */
    uint64_t last_tx;     /* counter of this slot's last committed transaction */
    uint64_t my_mark;     /* this slot's mark in the committed archive (spec 5.9.2) */
    uint64_t last_stamp;  /* stamp of this journal's last commit */
    int in_tx;            /* a transaction is open */
    uint64_t tx_id;
    uint64_t tx_start;    /* journal offset of the open transaction's first record */
    uint32_t tx_deltas;   /* data records the open transaction has written */
    cdb_durability tx_durability;
    unsigned unsynced;    /* commits since the journal was last synced */
    int failed;           /* an apply failed: the database must be reopened */
    cdb_catalog catalog;
    int has_catalog;      /* catalog_page was configured */
    /* Open stores, one set per model (cdb_slots.h): struct cdb_doc_pool,
     * struct cdb_vec_coll and struct cdb_ts_store. */
    cdb_slots pool_slots;
    cdb_slots vec_slots;
    cdb_slots ts_slots;
    cdb_slots table_slots;   /* struct cdb_table_store */
    size_t slot_mem_used;    /* bytes of cfg.slot_mem carved into chunks */
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
    uint32_t stage_buckets[CDB_STAGE_BUCKETS]; /* head offset in doc_stage or CDB_STAGE_NIL */
    uint8_t *idx_stage;
    size_t idx_stage_size;
    size_t stage_idx_used;
    int stage_idx_overflow;
    uint32_t stage_idx_buckets[CDB_STAGE_IDX_BUCKETS];
    int set_aside;        /* recovery set a companion aside (spec 1.12.2) */
    int via_table;        /* the table layer is calling the document engine (cdb_table.h) */
    unsigned op_depth;    /* operation scopes open (cdb_db_op_begin) */
    uint64_t op_id;       /* the outermost one's ID */
    uint64_t op_seq;      /* the last ID given out */
    /* Claimed entry-ID blocks still being handed out in memory (spec 5.9.3). */
    struct {
        uint64_t store;
        uint64_t next;
        uint64_t end;
    } id_ranges[CDB_COORD_MAX_BLOCKS];
    unsigned nid_ranges;
    /* Background heartbeat thread (spec 5.9.6, M20). */
    cdb_mutex *mutex;         /* protects cdb_db state against concurrent heartbeat/calls */
    cdb_cond *hb_cond;        /* wakes the heartbeat thread on shutdown */
    cdb_thread *hb_thread;    /* the heartbeat thread itself */
    int hb_stop;              /* set to 1 to tell the heartbeat thread to exit */
    int hb_running;           /* 1 if hb_thread is running and must be joined */
    /* Background worker threads (spec 5.9.6, M20). */
    cdb_worker_pool workers;
    /* Security, encryption & RBAC (spec 12). */
    uint8_t master_key[32];
    uint8_t integrity_key[32];
    int has_master_key;
    int has_integrity_key;
    uint8_t aead_alg;
    int is_production;
    cdb_key_provider_fn key_provider;
    void *key_provider_ctx;
    uint32_t rotation_interval_days;
    cdb_rotation_policy rotation_policy;
    cdb_signing_policy signing_policy;
    int rotation_expired;
    cdb_identity active_identity;
    int has_identity;
    struct {
        uint64_t enclave_id;
        uint32_t key_version;
        uint8_t enclave_key[32];
        uint8_t page_key[32];
        uint8_t hash_key[32];
        uint8_t index_key[32];
        int in_use;
    } enclaves[16];
    struct {
        uint64_t principal_id;
        uint32_t tenant_id;
        uint8_t pubkey[33];
        uint64_t scope;
        uint8_t perms;
        int in_use;
    } rbac_grants[32];
    unsigned num_grants;
    /* System audit series (spec 10.7, 12.7). */
    uint8_t last_audit_hash[32];
    uint64_t audit_seq;
    int audit_initialized;
    uint64_t audit_store_id;
    /* CRDT & P2P Replication (backlog L19-42). */
    uint32_t peer_id;
    cdb_hlc hlc;
    cdb_crdt_vector crdt_vector;
    int in_crdt_import;
    /* Event triggers (spec 11.6). */
    cdb_trigger_registry triggers;
} cdb_db;

/* Re-entrant synchronization locks across threads (spec 5.9.6, M20). */
void cdb_db_lock(cdb_db *db);
void cdb_db_unlock(cdb_db *db);

/* Opens <base>.cdbarch, recovering if needed, and starts a session. If the
 * archive does not exist and create is non-NULL, a new database is created
 * with those settings; otherwise CDB_E_NOT_FOUND.
 * Other errors: CDB_E_INVALID_ARG for missing buffers or a path too long;
 * CDB_E_READ_ONLY for a file this build may only read (not supported yet);
 * CDB_E_CORRUPT when a journal marked in use is missing or had to be set
 * aside, unless cfg->flags has CDB_DB_ACCEPT_LOSS; CDB_E_BUSY when every
 * coordinator slot is held by a live process.
 *
 * Several processes may have one database open (spec 5.9). Only the one
 * holding the baton writes the archive; the others keep their changes in
 * an overlay file until they reconcile, and see the others' changes once
 * those are reconciled. Opening may wait: see cdb_coord_join. */
cdb_status cdb_db_open(cdb_db *db, const cdb_env *env, const char *base, const cdb_db_config *cfg,
                       const cdb_header *create);

/* Keeps this process's coordinator alive (spec 5.9.6). Every engine entry
 * point that starts work writes a heartbeat when one is due; an application
 * that may sit idle longer than the dead timeout (3 s by default) calls this
 * from its own loop. CDB_E_BUSY if another process took this coordinator's
 * slot while it was silent; the database is then failed and must be closed. */
cdb_status cdb_db_tick(cdb_db *db);

/* Reconciles everything and, if no other process has the database open,
 * leaves it as one file at rest. CDB_E_STATE if an apply failed earlier:
 * the journal is then kept so the next open can replay it. CDB_E_BUSY when
 * the baton could not be had (spec 5.9.8); the database stays open and the
 * call may be repeated. */
cdb_status cdb_db_close(cdb_db *db);

/* An operation scope: every store opened inside it stays open until the
 * outermost scope ends, as stores a transaction uses do (cdb_slots.h).
 * Code that holds one store while it opens another of the same model
 * outside a transaction, such as a join, must run inside one. Scopes nest;
 * each begin needs its end. */
void cdb_db_op_begin(cdb_db *db);
void cdb_db_op_end(cdb_db *db);

/* Transactions. One at a time; CDB_E_STATE when the call does not fit the
 * current state (begin while open, put/commit/abort while none is open) or
 * after a failed apply. durability: CDB_DURABILITY_DEFAULT uses the
 * database's level. */
cdb_status cdb_tx_begin(cdb_db *db, cdb_durability durability);
/* Journals one change for the open transaction. op is CDB_OP_*; data is
 * copied. CDB_E_INVALID_ARG when the record would not fit record_buf. */
cdb_status cdb_tx_put(cdb_db *db, uint64_t store_id, uint64_t entry_id, uint8_t op,
                      const void *data, size_t len);
/* Journals a write-set key record (kind 4, spec 5.9.4, 14.4.7 rule 3) for a
 * unique index key written by the open transaction. */
cdb_status cdb_tx_put_key(cdb_db *db, uint64_t store_id, uint32_t index_num, const void *key, size_t key_len);
/* Entry-ID allocation from claimed blocks of CDB_COORD_ID_BLOCK (spec 5.9.3).
 * peek_entry ensures a block is claimed for store (at or above *next_entry)
 * and returns the next ID without consuming it; take_entry consumes it after
 * the insert record is journaled. */
cdb_status cdb_db_peek_entry(cdb_db *db, uint64_t store, const uint64_t *next_entry, uint64_t *id);
uint64_t cdb_db_take_entry(cdb_db *db, uint64_t store, uint64_t *next_entry, int *dirty);
/* Writes the commit record, syncs per the durability level, and applies
 * the changes to the pages. */
cdb_status cdb_tx_commit(cdb_db *db);
cdb_status cdb_tx_abort(cdb_db *db);

/* Folds every applied change into the archive (spec 3.2) and starts a new
 * journal. With other processes open it takes the baton, waiting for it if
 * need be, and merges every process's committed transactions (spec 5.9.1);
 * CDB_E_BUSY when the wait passes its cap. */
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

/* Space reclamation (spec 3.7, 16.2): sweeps the archive for pages and
 * nibblet records belonging to dropped stores, freeing them for reuse. */
cdb_status cdb_db_reclaim(cdb_db *db, uint64_t *pages_freed, uint64_t *nibblets_freed);

/* Security, enclaves, and key rotation (spec 12). */
cdb_status cdb_db_create_enclave(cdb_db *db, uint64_t enclave_id, const char *key_ref);
cdb_status cdb_db_open_enclave(cdb_db *db, uint64_t enclave_id, const char *key_ref);
cdb_status cdb_db_rotate_master_key(cdb_db *db, const char *new_master_key_uri);
cdb_status cdb_db_rotate_enclave_key(cdb_db *db, uint64_t enclave_id, const char *new_enclave_key_uri);
cdb_status cdb_db_check_key_rotation(cdb_db *db);

/* RBAC and principal identities (spec 12.5). */
cdb_status cdb_db_register_principal(cdb_db *db, uint64_t principal_id, uint32_t tenant_id,
                                     const uint8_t pubkey[33]);
cdb_status cdb_db_grant_role(cdb_db *db, uint64_t principal_id, uint32_t role_id,
                             uint64_t scope_store_or_enclave, uint8_t perm_mask);
cdb_status cdb_db_check_perm(cdb_db *db, uint64_t principal_id, uint64_t scope_store_or_enclave,
                             uint8_t required_perm);

/* Triggers (spec 11.6). */
cdb_status cdb_db_trigger_register(cdb_db *db, const char *name, size_t name_len,
                                   uint64_t store_id, uint8_t events,
                                   const char *proc_name, size_t proc_len,
                                   uint8_t max_depth);
cdb_status cdb_db_trigger_unregister(cdb_db *db, const char *name, size_t name_len);

/* Streaming blobs (spec 8.4, 14.15). */
cdb_status cdb_db_blob_write_begin(cdb_db *db, uint64_t store_id, uint8_t *scratch, cdb_blob_writer *w);
cdb_status cdb_db_blob_write_chunk(cdb_blob_writer *w, const void *chunk, size_t len);
cdb_status cdb_db_blob_write_end(cdb_blob_writer *w, cdb_blob_ref *out_ref);
cdb_status cdb_db_blob_write_abort(cdb_blob_writer *w);
cdb_status cdb_db_blob_read_begin(cdb_db *db, uint64_t store_id, const cdb_blob_ref *ref,
                                  uint8_t *page_buf, int verify_hash, cdb_blob_reader *r);
cdb_status cdb_db_blob_read_chunk(cdb_blob_reader *r, void *dest, size_t cap, size_t *out_len);
cdb_status cdb_db_blob_read_end(cdb_blob_reader *r);
cdb_status cdb_db_blob_free(cdb_db *db, uint64_t store_id, const cdb_blob_ref *ref, uint8_t *page_buf);

/* Staging cache for index keys and links (spec 14.4, backlog L69/L74). */
cdb_status cdb_stage_idx_put(cdb_db *db, uint64_t store, uint8_t idx_no, const uint8_t *key, size_t klen,
                             uint64_t entry, int live);
cdb_status cdb_stage_idx_get(const cdb_db *db, uint64_t store, uint8_t idx_no, const uint8_t *key, size_t klen,
                             uint64_t *entry, int *live, int *found);

/* CRDT & P2P Replication (spec backlog L19-42). */
cdb_status cdb_db_set_peer_id(cdb_db *db, uint32_t peer_id);
cdb_status cdb_db_get_peer_id(const cdb_db *db, uint32_t *peer_id);
cdb_status cdb_db_get_hlc(const cdb_db *db, cdb_hlc *hlc);
cdb_status cdb_db_get_crdt_vector(const cdb_db *db, cdb_crdt_vector *vector);

cdb_status cdb_db_crdt_export_slice(cdb_db *db, const cdb_crdt_vector *since_vector,
                                    uint8_t *out_buf, size_t cap, size_t *out_len);
cdb_status cdb_db_crdt_import_slice(cdb_db *db, const uint8_t *in_buf, size_t in_len,
                                    size_t *applied_count);
cdb_status cdb_db_crdt_import_deltas(cdb_db *db, uint32_t remote_peer_id,
                                     const cdb_crdt_delta_rec *recs, size_t count,
                                     size_t *applied_count);

#endif
