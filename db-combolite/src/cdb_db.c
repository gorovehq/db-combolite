/* Database: archive, working directory, transactions and recovery.
 *
 * The life of a change, start to finish:
 *  1. cdb_tx_put appends a data record to the journal (main.0.cdb). Nothing
 *     in the archive changes yet.
 *  2. cdb_tx_commit appends a commit record, syncs the journal according to
 *     the durability level, and then "applies" the transaction: each data
 *     record is handed to the apply hook, which changes pager pages. Those
 *     page changes are not yet committed in the archive.
 *  3. cdb_db_reconcile commits the pager. The header's companion slot 0
 *     records the highest commit number now in the archive (the
 *     "watermark"), and a fresh empty journal replaces the old one.
 * If the process dies between 2 and 3, the archive still holds its previous
 * commit and the journal still holds the transaction. Opening the database
 * replays every committed transaction above the watermark through the same
 * apply hook (spec 3.3). That is why the apply hook must be deterministic and
 * must never fail for logical reasons: recovery runs it again on the same
 * records and expects the same result. */
#include <stdio.h>
#include <string.h>

#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_link.h"
#include "cdb_ts.h"
#include "cdb_vec.h"

#define JOURNAL_SLOT 0u            /* this process's coordinator slot; always 0 until multi-process */
#define DEFAULT_SYNC_EVERY 32u     /* NORMAL durability: a barrier every 32 commits (spec 14.4.6) */
#define COMPANION_PROVISIONED 0x01u /* companion slot state bits (spec 14.1.5) */
#define COMPANION_IN_USE 0x02u
#define CLASS_INCOMPATIBLE 2u      /* the main journal's compatibility class (spec 14.4.6 rule 2) */

/* -------------------------------------------------------- environment */

/* Thin wrappers over the caller-supplied environment (files and
 * directories, see cdb_env.h). */
static cdb_status env_open(cdb_db *db, const char *path, unsigned flags, cdb_storage *st)
{
    return db->env.ops->open(db->env.ctx, path, flags, st);
}

static cdb_status env_exists(cdb_db *db, const char *path, int *exists)
{
    return db->env.ops->exists(db->env.ctx, path, exists);
}

static cdb_status env_remove(cdb_db *db, const char *path)
{
    return db->env.ops->remove(db->env.ctx, path);
}

static cdb_status env_sync_dir(cdb_db *db, const char *path)
{
    return db->env.ops->sync_dir(db->env.ctx, path);
}

/* Builds every path the database uses from base (for example "dir/name"):
 *   parent        "dir" (or "." if base has no slash): synced when entries
 *                 inside it are created or removed
 *   archive       "dir/name.cdbarch"
 *   workdir       "dir/name.cdbwork"
 *   journal_path  "dir/name.cdbwork/main.0.cdb"
 *   journal_tmp   "dir/name.cdbwork/main.0.cdb.new"
 * CDB_E_INVALID_ARG if any path would not fit in CDB_DB_PATH_MAX, or if
 * base's directory part is empty (as in "/name"). */
static cdb_status set_paths(cdb_db *db, const char *base)
{
    const char *slash = strrchr(base, '/');
    int n;

    if (slash) {
        size_t len = (size_t)(slash - base);
        if (len == 0 || len >= sizeof db->parent)
            return CDB_E_INVALID_ARG;
        memcpy(db->parent, base, len);
        db->parent[len] = '\0';
    } else {
        strcpy(db->parent, ".");
    }
    n = snprintf(db->archive, sizeof db->archive, "%s.cdbarch", base);
    if (n < 0 || (size_t)n >= sizeof db->archive)
        return CDB_E_INVALID_ARG;
    n = snprintf(db->workdir, sizeof db->workdir, "%s.cdbwork", base);
    if (n < 0 || (size_t)n >= sizeof db->workdir)
        return CDB_E_INVALID_ARG;
    n = snprintf(db->journal_path, sizeof db->journal_path, "%s/main.%u.cdb", db->workdir, JOURNAL_SLOT);
    if (n < 0 || (size_t)n >= sizeof db->journal_path)
        return CDB_E_INVALID_ARG;
    n = snprintf(db->journal_tmp, sizeof db->journal_tmp, "%s.new", db->journal_path);
    if (n < 0 || (size_t)n >= sizeof db->journal_tmp)
        return CDB_E_INVALID_ARG;
    return CDB_OK;
}

/* -------------------------------------------------------- companion slot */

/* Companion slot 0 in the file header describes the main journal
 * (spec 14.4.6 rule 2). It is edited in the pager's in-memory copy of the
 * committed header record, which the next pager commit writes out. */
static cdb_companion_slot *journal_slot(cdb_db *db)
{
    return &db->pager.hs.record.companions[0];
}

/* Records the journal's state in the header; durable at the next pager commit.
 * in_use: a journal exists that may hold changes the archive lacks.
 * watermark: the highest commit number whose changes are in the archive. */
static void set_journal_slot(cdb_db *db, int in_use, uint64_t watermark)
{
    cdb_companion_slot *c = journal_slot(db);
    c->type_id = (uint16_t)CDB_COMPANION_MAIN_JOURNAL;
    c->compat_class = (uint8_t)CLASS_INCOMPATIBLE;
    c->state = (uint8_t)(COMPANION_PROVISIONED | (in_use ? COMPANION_IN_USE : 0u));
    c->watermark = watermark;
    db->pager.dirty = 1;
}

/* The watermark from the committed header; 0 for a file that never had a
 * journal. */
static uint64_t watermark_of(cdb_db *db)
{
    const cdb_companion_slot *c = journal_slot(db);
    return (c->state & COMPANION_PROVISIONED) ? c->watermark : 0;
}

/* -------------------------------------------------------- journal */

/* Creates an empty journal file at path, writes its header and syncs it.
 * The new file becomes db->journal / db->journal_st. unsealed_seq is the
 * archive committed sequence this journal's records will apply on top of
 * (spec 14.4.6 rule 3). The caller syncs the directory. */
static cdb_status new_journal(cdb_db *db, const char *path, uint64_t unsealed_seq)
{
    cdb_journal_header jh;
    cdb_status s;

    memset(&jh, 0, sizeof jh);
    jh.type_id = (uint16_t)CDB_COMPANION_MAIN_JOURNAL;
    jh.slot = (uint16_t)JOURNAL_SLOT;
    memcpy(jh.db_id, db->pager.hs.record.db_id, sizeof jh.db_id);
    jh.unsealed_seq = unsealed_seq;
    jh.first_seq = 1;
    if ((s = env_open(db, path, CDB_ENV_CREATE, &db->journal_st)) != CDB_OK)
        return s;
    if ((s = cdb_journal_create(&db->journal, &db->journal_st, db->pager.order, &jh)) != CDB_OK)
        return s;
    return cdb_storage_sync(&db->journal_st);
}

/* Applies the data records of transaction tx_id found between start and end.
 * Used both by a live commit and by recovery, so both run exactly the same
 * code on exactly the same bytes. Each data record's payload is one op byte
 * (CDB_OP_*) followed by the model-specific data (spec 14.4.5). Reads into
 * cfg.record_buf, so the delta's data pointer is only valid during the apply
 * call. */
static cdb_status apply_range(cdb_db *db, uint64_t tx_id, uint64_t start, uint64_t end, uint64_t commit_number)
{
    uint64_t off = start;
    while (off < end) {
        cdb_record r;
        uint64_t next;
        cdb_status s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size,
                                        &r, &next);
        if (s != CDB_OK)
            return s;
        if (r.kind == CDB_RECORD_DATA && r.tx_id == tx_id) {
            cdb_delta d;
            if (r.payload_len < 1)
                return CDB_E_MALFORMED;
            d.store_id = r.store_id;
            d.entry_id = r.entry_id;
            d.op = r.payload[0];
            d.data = r.payload + 1;
            d.len = r.payload_len - 1u;
            d.commit_number = commit_number;
            if ((s = db->cfg.apply(db->cfg.apply_ctx, &db->pager, &d)) != CDB_OK)
                return s;
        }
        off = next;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- recovery */

/* Renames the journal to main.0.cdb.stray so it is kept for inspection but
 * never applied (spec 14.4.6 rule 3), and syncs the directory so the rename
 * is durable. Returns CDB_OK: the caller decides whether that is acceptable
 * (it is not, unless the operator passed CDB_DB_ACCEPT_LOSS). */
static cdb_status set_aside(cdb_db *db)
{
    char stray[CDB_DB_PATH_MAX + 8];
    cdb_status s;
    cdb_storage_close(&db->journal_st);
    snprintf(stray, sizeof stray, "%s.stray", db->journal_path);
    if ((s = db->env.ops->rename(db->env.ctx, db->journal_path, stray)) != CDB_OK)
        return s;
    db->set_aside = 1;
    return env_sync_dir(db, db->workdir);
}

/* Replays committed transactions above the watermark (spec 3.3, 14.1.8).
 * *journal_ok is 0 when no usable journal was found.
 *
 * The scan relies on one process running one transaction at a time, so a
 * transaction's data records are contiguous: a data record with a new
 * transaction ID starts a new run, and the commit record that follows ends
 * it. A run with no commit record (an abort, or a crash before commit) is
 * simply never applied. Commit numbers seen in the journal also move
 * next_commit forward, so numbers are never handed out twice. */
static cdb_status replay(cdb_db *db, int *journal_ok)
{
    uint64_t watermark = watermark_of(db);
    uint64_t archive_seq = db->pager.hs.record.committed_seq;
    uint64_t off = CDB_JOURNAL_HEADER_SIZE, tx_start = 0, cur_tx = 0;
    int exists;
    cdb_status s;

    *journal_ok = 0;
    /* No journal at all: nothing to replay (the caller checks whether one
     * was expected). */
    if ((s = env_exists(db, db->journal_path, &exists)) != CDB_OK)
        return s;
    if (!exists)
        return CDB_OK;
    if ((s = env_open(db, db->journal_path, 0, &db->journal_st)) != CDB_OK)
        return s;
    if (cdb_journal_open(&db->journal, &db->journal_st, db->pager.order) != CDB_OK
        || memcmp(db->journal.hdr.db_id, db->pager.hs.record.db_id, 16) != 0)
        return set_aside(db); /* unreadable, or from another database (spec 1.12.2) */

    for (;;) {
        cdb_record r;
        uint64_t next;
        if (cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next) != CDB_OK)
            break; /* the first record that fails verification ends the journal (spec 3.3) */
        if (r.kind == CDB_RECORD_DATA && r.tx_id != cur_tx) {
            cur_tx = r.tx_id;
            tx_start = off;
        } else if (r.kind == CDB_RECORD_ABORT) {
            cur_tx = 0;
        } else if (r.kind == CDB_RECORD_COMMIT) {
            cdb_commit_info ci;
            if ((s = cdb_commit_decode(&r, &ci, db->pager.order)) != CDB_OK)
                break;
            if (ci.commit_number >= db->next_commit)
                db->next_commit = ci.commit_number + 1;
            /* Commits at or below the watermark are already in the archive. */
            if (r.tx_id == cur_tx && ci.commit_number > watermark) {
                /* Only a journal unsealed from this exact archive state may be
                 * replayed onto it. */
                if (db->journal.hdr.unsealed_seq != archive_seq)
                    return set_aside(db);
                if ((s = apply_range(db, cur_tx, tx_start, off, ci.commit_number)) != CDB_OK)
                    return s;
                db->applied = ci.commit_number;
            }
            cur_tx = 0;
        }
        off = next;
    }
    *journal_ok = 1;
    return CDB_OK;
}

/* Brings the archive up to date with whatever journal a previous session
 * left, then removes that journal. Steps: replay; refuse to open if a journal
 * marked in use is missing or unusable (unless the operator accepts the
 * loss); commit the replayed changes with the journal marked not in use;
 * delete the old journal and any half-made .new journal. After this, the
 * archive alone holds the whole database. */
static cdb_status recover(cdb_db *db)
{
    int in_use = (journal_slot(db)->state & COMPANION_IN_USE) != 0;
    int journal_ok, exists;
    cdb_status s;

    db->applied = watermark_of(db);
    if ((s = replay(db, &journal_ok)) != CDB_OK)
        return s;
    if (in_use && !journal_ok && !(db->cfg.flags & CDB_DB_ACCEPT_LOSS))
        return CDB_E_CORRUPT; /* changes may have been lost (spec 1.12.5) */

    /* Fold whatever was replayed into the archive before discarding the journal. */
    if (db->applied != watermark_of(db) || in_use) {
        if ((s = cdb_doc_flush_pools(db)) != CDB_OK)
            return s;
        set_journal_slot(db, 0, db->applied);
        if ((s = cdb_pager_commit(&db->pager)) != CDB_OK)
            return s;
    }
    if (journal_ok) {
        cdb_storage_close(&db->journal_st);
        if ((s = env_remove(db, db->journal_path)) != CDB_OK)
            return s;
    }
    /* A .new journal is left only by a crash during rotation, before the
     * rename; the old journal was still the real one, so it is discarded. */
    if ((s = env_exists(db, db->journal_tmp, &exists)) != CDB_OK)
        return s;
    if (exists && (s = env_remove(db, db->journal_tmp)) != CDB_OK)
        return s;
    /* Make the removals durable. */
    if ((s = env_exists(db, db->workdir, &exists)) != CDB_OK)
        return s;
    return exists ? env_sync_dir(db, db->workdir) : CDB_OK;
}

/* -------------------------------------------------------- session */

/* Creates the working directory and a fresh journal, then records the
 * journal as in use in the header. The order matters for crash safety
 * (spec 14.4.6 rule 2): the slot says "in use" only once the journal and its
 * directory entry are durable, so a missing journal marked in use can only
 * mean real loss. */
static cdb_status start_session(cdb_db *db)
{
    cdb_status s;

    if ((s = db->env.ops->mkdir(db->env.ctx, db->workdir)) != CDB_OK)
        return s;
    if ((s = env_sync_dir(db, db->parent)) != CDB_OK)
        return s;
    /* The journal is unsealed from the archive state the next commit creates. */
    if ((s = new_journal(db, db->journal_path, db->pager.hs.record.committed_seq + 1)) != CDB_OK)
        return s;
    if ((s = env_sync_dir(db, db->workdir)) != CDB_OK)
        return s;
    /* Only now, with the journal durable, record it as in use. */
    set_journal_slot(db, 1, db->applied);
    return cdb_pager_commit(&db->pager);
}

/* Caller memory for n slots of each kind, laid out the way the slot code
 * carves it: full chunks of CDB_SLOTS_CHUNK, then one smaller chunk for the
 * remainder. */
size_t cdb_db_slot_mem_size(unsigned n)
{
    unsigned chunks = (n + CDB_SLOTS_CHUNK - 1u) / CDB_SLOTS_CHUNK, last = n - (chunks - 1u) * CDB_SLOTS_CHUNK;
    size_t total = 0;
    if (n == 0)
        return 0;
    total += (chunks - 1u) * cdb_slots_chunk_bytes(sizeof(struct cdb_doc_pool), CDB_SLOTS_CHUNK)
             + cdb_slots_chunk_bytes(sizeof(struct cdb_doc_pool), last);
    total += (chunks - 1u) * cdb_slots_chunk_bytes(sizeof(struct cdb_vec_coll), CDB_SLOTS_CHUNK)
             + cdb_slots_chunk_bytes(sizeof(struct cdb_vec_coll), last);
    total += (chunks - 1u) * cdb_slots_chunk_bytes(sizeof(struct cdb_ts_store), CDB_SLOTS_CHUNK)
             + cdb_slots_chunk_bytes(sizeof(struct cdb_ts_store), last);
    return total;
}

/* Gives every slot chunk of all three sets back (allocator memory is
 * freed). Used at close and when an open fails partway. */
static void release_slots(cdb_db *db)
{
    cdb_slots_release(db, &db->pool_slots);
    cdb_slots_release(db, &db->vec_slots);
    cdb_slots_release(db, &db->ts_slots);
}

static cdb_status open_db(cdb_db *db, const char *base, const cdb_db_config *cfg, const cdb_header *create);

cdb_status cdb_db_open(cdb_db *db, const cdb_env *env, const char *base, const cdb_db_config *cfg,
                       const cdb_header *create)
{
    cdb_status s;

    if (db == NULL || env == NULL || base == NULL || cfg == NULL
        || (cfg->apply == NULL && (cfg->doc_buffers == NULL || cfg->catalog_page == NULL))
        || cfg->record_buf == NULL || cfg->record_buf_size < CDB_JOURNAL_HEADER_SIZE + CDB_COMMIT_PAYLOAD_SIZE)
        return CDB_E_INVALID_ARG;
    memset(db, 0, sizeof *db);
    db->env = *env;
    db->cfg = *cfg;
    if (db->cfg.durability == CDB_DURABILITY_DEFAULT)
        db->cfg.durability = CDB_DURABILITY_NORMAL;
    if (db->cfg.normal_sync_every == 0)
        db->cfg.normal_sync_every = DEFAULT_SYNC_EVERY;
    if (db->cfg.apply == NULL) {
        db->cfg.apply = cdb_doc_apply; /* documents (spec 8) */
        db->cfg.apply_ctx = db;
    }
    if (db->cfg.slot_mem == NULL)
        db->cfg.slot_mem_size = 0;
    cdb_slots_init(&db->pool_slots, sizeof(struct cdb_doc_pool), &cdb_doc_slot_ops);
    cdb_slots_init(&db->vec_slots, sizeof(struct cdb_vec_coll), &cdb_vec_slot_ops);
    cdb_slots_init(&db->ts_slots, sizeof(struct cdb_ts_store), &cdb_ts_slot_ops);
    if ((s = open_db(db, base, cfg, create)) != CDB_OK)
        release_slots(db); /* recovery may have opened stores */
    return s;
}

/* The body of cdb_db_open, after the configuration is copied and the slot
 * sets are set up: open or create the archive, open the catalog, recover,
 * start the session. Split out so that any failure in here lets
 * cdb_db_open release the slots in one place. */
static cdb_status open_db(cdb_db *db, const char *base, const cdb_db_config *cfg, const cdb_header *create)
{
    int exists;
    cdb_status s;

    if ((s = set_paths(db, base)) != CDB_OK)
        return s;

    /* Open the archive, or create it (syncing the parent directory so the
     * new file's name is durable). */
    if ((s = env_exists(db, db->archive, &exists)) != CDB_OK)
        return s;
    if (!exists) {
        if (create == NULL)
            return CDB_E_NOT_FOUND;
        if ((s = env_open(db, db->archive, CDB_ENV_CREATE, &db->archive_st)) != CDB_OK)
            return s;
        if ((s = cdb_pager_create(&db->pager, &db->archive_st, &cfg->pager, create)) != CDB_OK)
            return s;
        if ((s = env_sync_dir(db, db->parent)) != CDB_OK)
            return s;
    } else {
        if ((s = env_open(db, db->archive, 0, &db->archive_st)) != CDB_OK)
            return s;
        if ((s = cdb_pager_open(&db->pager, &db->archive_st, &cfg->pager)) != CDB_OK)
            return s;
    }
    if (db->pager.read_only)
        return CDB_E_READ_ONLY; /* read-only sessions come with the coordinator milestone */

    /* Commit numbers resume at the durable reservation mark: any number
     * below it may already have been used before a crash (spec 14.4.6
     * rule 5). Commit numbers start at 1. */
    db->commit_hwm = db->pager.hs.record.commit_hwm;
    db->next_commit = db->commit_hwm > 0 ? db->commit_hwm : 1;
    /* The catalog first: replaying document changes needs it. */
    if (cfg->catalog_page != NULL) {
        if ((s = cdb_catalog_open(&db->catalog, &db->pager, &cfg->catalog_work, cfg->catalog_page)) != CDB_OK)
            return s;
        db->has_catalog = 1;
    }
    if ((s = recover(db)) != CDB_OK)
        return s;
    if (db->next_commit <= db->applied)
        db->next_commit = db->applied + 1;
    db->next_tx = 1;
    return start_session(db);
}

/* -------------------------------------------------------- transactions */

/* After a failed apply, the pages no longer match the journal, so every
 * further operation is refused until the database is reopened. */
static cdb_status usable(const cdb_db *db)
{
    return db->failed ? CDB_E_STATE : CDB_OK;
}

/* Operation scopes (see cdb_db.h). Only the outermost begin takes a new ID;
 * nested scopes share it, so the whole outer scope counts as one operation
 * when the slot code decides which stores it may evict. An unmatched end is
 * ignored. */
void cdb_db_op_begin(cdb_db *db)
{
    if (db->op_depth++ == 0)
        db->op_id = ++db->op_seq;
}

void cdb_db_op_end(cdb_db *db)
{
    if (db->op_depth > 0)
        db->op_depth--;
}

/* Starts a transaction. Only one may be open at a time. The transaction ID
 * is the coordinator slot in the top 16 bits and a per-session counter in
 * the low 48 (spec 14.4.3). */
cdb_status cdb_tx_begin(cdb_db *db, cdb_durability durability)
{
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    db->in_tx = 1;
    db->tx_id = ((uint64_t)JOURNAL_SLOT << 48) | db->next_tx++;
    /* Remember where this transaction's records start in the journal; the
     * commit applies records from here. */
    db->tx_start = db->journal.end;
    db->tx_deltas = 0;
    db->stage_used = 0;
    db->stage_overflow = 0;
    db->tx_durability = durability == CDB_DURABILITY_DEFAULT ? db->cfg.durability : durability;
    return CDB_OK;
}

/* Journals one change. The payload (op byte + data) is assembled directly
 * at its final place inside record_buf, after the record header, so the
 * append does not copy it again. data may itself already lie inside
 * record_buf, hence memmove. The change is not applied until commit. */
cdb_status cdb_tx_put(cdb_db *db, uint64_t store_id, uint64_t entry_id, uint8_t op,
                      const void *data, size_t len)
{
    uint8_t *payload = db->cfg.record_buf + CDB_RECORD_HEADER_SIZE;
    cdb_record r;

    if (usable(db) != CDB_OK || !db->in_tx)
        return CDB_E_STATE;
    /* The whole record (header, op byte, data, trailer) must fit record_buf. */
    if (len > db->cfg.record_buf_size - CDB_RECORD_HEADER_SIZE - CDB_RECORD_TRAILER_SIZE - 1u
        || (data == NULL && len != 0))
        return CDB_E_INVALID_ARG;
    payload[0] = op;
    if (len)
        memmove(payload + 1, data, len);
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_DATA;
    r.tx_id = db->tx_id;
    r.store_id = store_id;
    r.entry_id = entry_id;
    r.payload = payload;
    r.payload_len = (uint32_t)(len + 1u);
    db->tx_deltas++;
    return cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size);
}

/* Hands out the next global commit number. Numbers are reserved in blocks
 * of 1024: before the first number of a new block is used, the header is
 * checkpointed with the raised mark, so a crash can never lead to a number
 * being reused (spec 14.4.6 rule 5). */
static cdb_status reserve_commit(cdb_db *db, uint64_t *number)
{
    if (db->next_commit >= db->commit_hwm) {
        uint64_t hwm = db->next_commit + CDB_COMMIT_RESERVE_BLOCK;
        cdb_status s = cdb_header_checkpoint(&db->pager.st, &db->pager.hs, db->pager.seq_hwm, hwm);
        if (s != CDB_OK)
            return s;
        db->commit_hwm = hwm;
    }
    *number = db->next_commit++;
    return CDB_OK;
}

/* An environment without a clock (no now function, or one that returns
 * CDB_E_UNSUPPORTED) is not an error: *have is 0. Other clock errors are
 * passed on. */
cdb_status cdb_db_now(cdb_db *db, int64_t *ns, int *have)
{
    cdb_status s;
    *ns = 0;
    *have = 0;
    if (db->env.ops->now == NULL)
        return CDB_OK;
    s = db->env.ops->now(db->env.ctx, ns);
    if (s == CDB_E_UNSUPPORTED) {
        *ns = 0;
        return CDB_OK;
    }
    if (s == CDB_OK)
        *have = 1;
    return s;
}

cdb_status cdb_tx_commit(cdb_db *db)
{
    uint8_t info[CDB_COMMIT_PAYLOAD_SIZE];
    cdb_commit_info ci;
    cdb_record r;
    uint64_t record_at;
    cdb_status s;

    if (usable(db) != CDB_OK || !db->in_tx)
        return CDB_E_STATE;
    db->in_tx = 0;
    /* An empty transaction leaves no trace and uses no commit number. */
    if (db->tx_deltas == 0)
        return CDB_OK;

    /* Build and append the commit record (spec 14.4.4). Its presence in
     * the journal is what makes the transaction count at replay. */
    memset(&ci, 0, sizeof ci);
    if ((s = reserve_commit(db, &ci.commit_number)) != CDB_OK)
        return s;
    {
        int64_t now;
        int have;
        if ((s = cdb_db_now(db, &now, &have)) != CDB_OK)
            return s;
        ci.timestamp = have ? (uint64_t)now : 0u; /* spec 14.4: ns since the epoch; 0 = no clock */
    }
    ci.delta_count = db->tx_deltas;
    ci.track_mask = 1u << CDB_COMPANION_MAIN_JOURNAL;
    cdb_commit_encode(&ci, info, db->pager.order);
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_COMMIT;
    r.tx_id = db->tx_id;
    r.payload = info;
    r.payload_len = CDB_COMMIT_PAYLOAD_SIZE;
    record_at = db->journal.end;
    if ((s = cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size)) != CDB_OK)
        return s;

    /* Durability (spec 3.1): FULL syncs the journal now, so the commit
     * survives a power loss once this returns. NORMAL syncs every Nth
     * commit, so up to N-1 recent commits can be lost. OFF never syncs
     * here; reconciliation and close make the changes durable. */
    switch (db->tx_durability) {
    case CDB_DURABILITY_FULL:
        if ((s = cdb_storage_sync(&db->journal_st)) != CDB_OK)
            return s;
        db->unsynced = 0;
        break;
    case CDB_DURABILITY_NORMAL:
        if (++db->unsynced >= db->cfg.normal_sync_every) {
            if ((s = cdb_storage_sync(&db->journal_st)) != CDB_OK)
                return s;
            db->unsynced = 0;
        }
        break;
    default:
        db->unsynced++;
        break;
    }

    /* The transaction is committed; make it visible through the pager. */
    if ((s = apply_range(db, db->tx_id, db->tx_start, record_at, ci.commit_number)) != CDB_OK) {
        db->failed = 1; /* the journal holds it; reopening replays it */
        return s;
    }
    db->applied = ci.commit_number;
    return CDB_OK;
}

/* Nothing was applied, so aborting only needs an abort record, which ends
 * the transaction's run of records for the replay scan. Records without a
 * commit record are never applied. */
cdb_status cdb_tx_abort(cdb_db *db)
{
    cdb_record r;
    if (usable(db) != CDB_OK || !db->in_tx)
        return CDB_E_STATE;
    db->in_tx = 0;
    if (db->tx_deltas == 0)
        return CDB_OK;
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_ABORT;
    r.tx_id = db->tx_id;
    return cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size);
}

/* -------------------------------------------------------- reconcile/close */

/* Replaces the journal with an empty one unsealed from the current archive
 * state. Called after every archive commit made while nothing in the journal
 * is above the watermark: reconciliation, catalog changes and compaction.
 * The new journal is written under a temporary name and renamed over the
 * old one, so a crash always leaves exactly one journal (spec 14.4.6). */
static cdb_status rotate_journal(cdb_db *db)
{
    cdb_storage old = db->journal_st;
    cdb_status s;
    /* The archive commit has already happened, so the current committed
     * sequence is the state the new journal applies on top of. */
    if ((s = new_journal(db, db->journal_tmp, db->pager.hs.record.committed_seq)) != CDB_OK)
        return s;
    cdb_storage_close(&old);
    if ((s = db->env.ops->rename(db->env.ctx, db->journal_tmp, db->journal_path)) != CDB_OK)
        return s;
    db->unsynced = 0;
    return env_sync_dir(db, db->workdir);
}

/* Commits every applied change to the archive (spec 3.2). Not allowed with
 * a transaction open. The new watermark is stored in the same header commit
 * as the pages, so the archive and the watermark always agree. */
cdb_status cdb_db_reconcile(cdb_db *db)
{
    cdb_status s;

    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if (db->applied == watermark_of(db) && !db->pager.dirty)
        return CDB_OK;
    /* Document pools, vector collections and time-series stores keep some
     * state in memory (index roots, next entry IDs; held late points for
     * time series); stage it into the catalog first so it is part of this
     * commit. */
    if ((s = cdb_doc_flush_pools(db)) != CDB_OK)
        return s;
    set_journal_slot(db, 1, db->applied);
    if ((s = cdb_pager_commit(&db->pager)) != CDB_OK)
        return s;
    /* Stores idle since the last reconciliation give their slots back. */
    cdb_slots_shrink(db, &db->pool_slots);
    cdb_slots_shrink(db, &db->vec_slots);
    cdb_slots_shrink(db, &db->ts_slots);
    /* Every change in the journal is now in the archive. */
    return rotate_journal(db);
}

/* Common start of every catalog change: no transaction may be open, a
 * catalog must be configured (CDB_E_UNSUPPORTED otherwise), and everything
 * applied so far is reconciled first. The catalog change then commits on its
 * own, after which the journal is rotated: catalog changes are never
 * journaled, they go straight into the archive (spec 4). */
static cdb_status catalog_ready(cdb_db *db)
{
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    return cdb_db_reconcile(db);
}

cdb_status cdb_db_create_store(cdb_db *db, const char *name, size_t len, uint32_t model, uint64_t *id)
{
    cdb_status s = catalog_ready(db);
    if (s == CDB_OK)
        s = cdb_catalog_create_store(&db->catalog, name, len, model, id);
    return s == CDB_OK ? rotate_journal(db) : s;
}

/* A read: no reconcile and no rotation needed. */
cdb_status cdb_db_find_store(cdb_db *db, const char *name, size_t len, uint64_t *id, int *found)
{
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    return cdb_catalog_find_store(&db->catalog, name, len, id, found);
}

cdb_status cdb_db_drop_store(cdb_db *db, uint64_t id)
{
    cdb_status s = catalog_ready(db);
    cdb_slot *slot;
    if (s == CDB_OK && cdb_link_is_store(db, id))
        s = CDB_E_INVALID_ARG; /* the link store is the engine's own */
    /* Its entries' links are tombstoned; the drop's commit carries that. */
    if (s == CDB_OK)
        s = cdb_link_on_drop(db, id);
    if (s == CDB_OK)
        s = cdb_catalog_drop_store(&db->catalog, id);
    if (s != CDB_OK)
        return s;
    /* Forget the store's open state, so it reads as dropped from now on. */
    if ((slot = cdb_slots_find(&db->pool_slots, id)) != NULL
        || (slot = cdb_slots_find(&db->vec_slots, id)) != NULL
        || (slot = cdb_slots_find(&db->ts_slots, id)) != NULL)
        slot->in_use = 0;
    return rotate_journal(db);
}

cdb_status cdb_db_catalog_begin(cdb_db *db)
{
    return catalog_ready(db);
}

cdb_status cdb_db_catalog_end(cdb_db *db, cdb_status s)
{
    return s == CDB_OK ? rotate_journal(db) : s;
}

cdb_status cdb_db_add_schema(cdb_db *db, uint64_t store_id, const void *schema, size_t len, uint32_t *version)
{
    cdb_status s = catalog_ready(db);
    if (s == CDB_OK)
        s = cdb_catalog_add_schema(&db->catalog, store_id, schema, len, version);
    return s == CDB_OK ? rotate_journal(db) : s;
}

/* Compaction requires a clean pager, hence the reconcile first. It makes
 * its own archive commits, so the journal is rotated afterwards to match the
 * new committed sequence. */
cdb_status cdb_db_compact(cdb_db *db, uint64_t max_moves, uint64_t *moved, uint64_t *reclaimed)
{
    cdb_status s;
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if ((s = cdb_db_reconcile(db)) != CDB_OK)
        return s;
    if ((s = cdb_pager_compact(&db->pager, max_moves, moved, reclaimed)) != CDB_OK)
        return s;
    return rotate_journal(db);
}

/* Close is a final reconciliation that also marks the journal "not in use",
 * followed by removing the journal and the working directory. The order
 * follows spec 14.4.6 rule 2: "in use" is cleared (and committed) before the
 * journal is removed. A failed database is refused: its journal must stay
 * so the next open can replay it. An open transaction is aborted. */
cdb_status cdb_db_close(cdb_db *db)
{
    cdb_status s;

    if (db->failed) {
        release_slots(db); /* their state is lost with the session anyway */
        return CDB_E_STATE;
    }
    if (db->in_tx && (s = cdb_tx_abort(db)) != CDB_OK)
        return s;
    if ((s = cdb_doc_flush_pools(db)) != CDB_OK)
        return s;
    set_journal_slot(db, 0, db->applied);
    if ((s = cdb_pager_commit(&db->pager)) != CDB_OK)
        return s;
    release_slots(db); /* everything they held is in the archive */
    /* The archive no longer needs the working directory. */
    cdb_storage_close(&db->journal_st);
    if ((s = env_remove(db, db->journal_path)) != CDB_OK)
        return s;
    if ((s = env_sync_dir(db, db->workdir)) != CDB_OK)
        return s;
    s = db->env.ops->rmdir(db->env.ctx, db->workdir);
    if (s != CDB_OK && s != CDB_E_STATE) /* a set-aside companion keeps it */
        return s;
    if ((s = env_sync_dir(db, db->parent)) != CDB_OK)
        return s;
    cdb_storage_close(&db->archive_st);
    return CDB_OK;
}
