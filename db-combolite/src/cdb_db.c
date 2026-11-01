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

#include "cdb_crc32c.h"
#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_key.h"
#include "cdb_link.h"
#include "cdb_table.h"
#include "cdb_ts.h"
#include "cdb_vec.h"

#define DEFAULT_SYNC_EVERY 32u     /* NORMAL durability: a barrier every 32 commits (spec 14.4.6) */
#define TX_RESERVE_BLOCK 1024u     /* transaction counters reserved per slot write (spec 14.4.3) */
#define TX_COUNTER_MASK 0xFFFFFFFFFFFFull /* the low 48 bits of a transaction ID */
#define MARKS_STORE 0u             /* store ID recorded in the marks index's pages */
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
 *   coorddir      "dir/name.cdbwork/coord"
 * The journal's paths depend on the coordinator slot (journal_paths).
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
    n = snprintf(db->coorddir, sizeof db->coorddir, "%s/coord", db->workdir);
    if (n < 0 || (size_t)n >= sizeof db->coorddir)
        return CDB_E_INVALID_ARG;
    /* The longest journal name must fit too. */
    if (strlen(db->workdir) + sizeof "/main.65535.cdb.new" > CDB_DB_PATH_MAX)
        return CDB_E_INVALID_ARG;
    return CDB_OK;
}

/* The journal of a slot, and its temporary name while rotating
 * (spec 14.4.7 rule 1). set_paths has checked that both fit. */
static void journal_paths(const cdb_db *db, unsigned slot, char *path, char *tmp)
{
    int n = snprintf(path, CDB_DB_PATH_MAX, "%s/main.%u.cdb", db->workdir, slot);
    if (n < 0 || n >= (int)CDB_DB_PATH_MAX)
        path[0] = '\0'; /* not reached: set_paths checked the length */
    if (tmp != NULL) {
        n = snprintf(tmp, CDB_DB_PATH_MAX, "%s/main.%u.cdb.new", db->workdir, slot);
        if (n < 0 || n >= (int)CDB_DB_PATH_MAX)
            tmp[0] = '\0';
    }
}

/* The private page overlay of a slot (spec 5.9.1, 14.4.7 rule 8). */
static void overlay_path_of(const cdb_db *db, unsigned slot, char *path)
{
    int n = snprintf(path, CDB_DB_PATH_MAX, "%s/pages.%u.cdb", db->workdir, slot);
    if (n < 0 || n >= (int)CDB_DB_PATH_MAX)
        path[0] = '\0';
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

/* -------------------------------------------------------- marks */

/* The reconciliation-marks index (spec 14.4.8): for each coordinator slot,
 * the transaction counter through which its journal is in the archive. It
 * borrows the catalog's work memory, which is never in use at the same time
 * (marks are read and written only during recovery and reconciliation,
 * after the catalog's own changes). *have is 0 when the archive has none
 * and create is 0. */
static cdb_status marks_open(cdb_db *db, cdb_index *ix, int create, int *have)
{
    uint64_t root = db->pager.hs.record.marks_root;
    cdb_status s;
    *have = 0;
    if (root == 0) {
        if (!create)
            return CDB_OK;
        if ((s = cdb_index_create(ix, &db->pager, MARKS_STORE, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        db->pager.hs.record.marks_root = ix->root; /* durable with the next commit */
        db->pager.dirty = 1;
    } else if ((s = cdb_index_open(ix, &db->pager, root, &db->cfg.catalog_work)) != CDB_OK) {
        return s;
    }
    *have = 1;
    return CDB_OK;
}

static size_t mark_key(unsigned slot, uint8_t *buf, size_t cap)
{
    cdb_key_builder k;
    cdb_key_init(&k, buf, cap);
    (void)cdb_key_u64(&k, slot);
    return k.len;
}

/* The slot's mark; 0 when it has none. */
static cdb_status mark_get(cdb_db *db, unsigned slot, uint64_t *mark)
{
    uint8_t key[16], val[8];
    size_t klen = mark_key(slot, key, sizeof key), vlen;
    cdb_index ix;
    int have, found;
    cdb_status s;
    *mark = 0;
    if ((s = marks_open(db, &ix, 0, &have)) != CDB_OK || !have)
        return s;
    if ((s = cdb_index_get(&ix, key, klen, val, sizeof val, &vlen, &found)) != CDB_OK)
        return s;
    if (found) {
        if (vlen != sizeof val)
            return CDB_E_CORRUPT;
        *mark = cdb_load_u64(val, db->pager.order);
    }
    return CDB_OK;
}

/* Records the slot's mark; part of the next pager commit. */
static cdb_status mark_set(cdb_db *db, unsigned slot, uint64_t mark)
{
    uint8_t key[16], val[8];
    size_t klen = mark_key(slot, key, sizeof key);
    cdb_index ix;
    int have;
    cdb_status s;
    if ((s = marks_open(db, &ix, 1, &have)) != CDB_OK)
        return s;
    cdb_store_u64(val, mark, db->pager.order);
    return cdb_index_put(&ix, key, klen, val, sizeof val);
}

/* Before any commit that folds this session's transactions into the
 * archive: its mark moves to the last one committed (spec 5.9.2). my_mark
 * follows only once that commit has succeeded (own_mark_committed). */
static cdb_status stage_own_mark(cdb_db *db)
{
    if (db->last_tx <= db->my_mark)
        return CDB_OK;
    return mark_set(db, db->coord.me.slot, db->last_tx);
}

static void own_mark_committed(cdb_db *db)
{
    db->my_mark = db->last_tx;
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
    jh.slot = db->coord.me.slot;
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
static cdb_status apply_range(cdb_db *db, cdb_journal *j, uint64_t tx_id, uint64_t start, uint64_t end,
                              uint64_t commit_number)
{
    uint64_t off = start;
    while (off < end) {
        cdb_record r;
        uint64_t next;
        cdb_status s = cdb_journal_read(j, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next);
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

/* Renames a journal to <name>.stray so it is kept for inspection but never
 * applied (spec 14.4.6 rule 3), and syncs the directory so the rename is
 * durable. Returns CDB_OK: the caller decides whether that is acceptable
 * (it is not, unless the operator passed CDB_DB_ACCEPT_LOSS). */
static cdb_status set_aside(cdb_db *db, const char *path)
{
    char stray[CDB_DB_PATH_MAX + 8];
    cdb_status s;
    snprintf(stray, sizeof stray, "%s.stray", path);
    if ((s = db->env.ops->rename(db->env.ctx, path, stray)) != CDB_OK)
        return s;
    db->set_aside = 1;
    return env_sync_dir(db, db->workdir);
}

/* Opens slot's journal as db->rj and checks its header. */
static cdb_status open_journal(cdb_db *db, unsigned slot)
{
    char path[CDB_DB_PATH_MAX];
    cdb_status s;
    journal_paths(db, slot, path, NULL);
    if ((s = env_open(db, path, 0, &db->rj_st)) != CDB_OK)
        return s;
    if ((s = cdb_journal_open(&db->rj, &db->rj_st, db->pager.order)) != CDB_OK)
        cdb_storage_close(&db->rj_st);
    return s;
}

/* One slot's journal while recovery or reconciliation merges them (spec
 * 5.9.1, 14.4.7 rule 5). */
struct jcursor {
    unsigned slot;
    int dead_slot;      /* a dead peer's slot file exists and should be removed */
    int has_journal;    /* main.<slot>.cdb exists */
    uint64_t dead_hwm;  /* tx_hwm from a dead peer's slot file */
    uint64_t mark;      /* the slot's transactions through this counter are in the archive */
    uint64_t off;       /* where the scan resumes */
    uint64_t top;       /* highest counter of a committed transaction seen */
    uint64_t unsealed;  /* the archive state the journal was started from */
    int have;           /* there is a next transaction to apply: */
    uint64_t stamp, counter, tx_id, start, commit_at;
};

/* The merge order (spec 5.9.2): stamp, then slot, then counter. */
static int goes_first(const struct jcursor *a, const struct jcursor *b)
{
    if (a->stamp != b->stamp)
        return a->stamp < b->stamp;
    if (a->slot != b->slot)
        return a->slot < b->slot;
    return a->counter < b->counter;
}

/* Moves the cursor to its journal's next committed transaction above the
 * slot's mark (c->have is 0 when there is none), noting the highest
 * committed counter on the way.
 *
 * The scan relies on one process running one transaction at a time, so a
 * transaction's data records are contiguous: a data record with a new
 * transaction ID starts a new run, and the commit record that follows ends
 * it. A run with no commit record (an abort, or a crash before commit) is
 * never applied. The first record that fails verification ends the journal
 * (spec 3.3). */
static cdb_status scan_next(cdb_db *db, struct jcursor *c)
{
    uint64_t off = c->off, start = 0, cur = 0;
    cdb_status s;

    c->have = 0;
    if ((s = open_journal(db, c->slot)) != CDB_OK)
        return s;
    for (;;) {
        cdb_record r;
        uint64_t next;
        if (cdb_journal_read(&db->rj, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next) != CDB_OK)
            break;
        if ((r.kind == CDB_RECORD_DATA || r.kind == CDB_RECORD_WRITE_KEY) && r.tx_id != cur) {
            cur = r.tx_id;
            start = off;
        } else if (r.kind == CDB_RECORD_ABORT) {
            cur = 0;
        } else if (r.kind == CDB_RECORD_COMMIT) {
            cdb_commit_info ci;
            uint64_t counter = r.tx_id & TX_COUNTER_MASK;
            int ours = r.tx_id == cur && (r.tx_id >> 48) == c->slot;
            cur = 0;
            if (cdb_commit_decode(&r, &ci, db->pager.order) != CDB_OK)
                break;
            if (ours && counter > c->top)
                c->top = counter;
            if (ours && counter > c->mark) {
                c->have = 1;
                c->stamp = ci.timestamp;
                c->counter = counter;
                c->tx_id = r.tx_id;
                c->start = start;
                c->commit_at = off;
                off = next;
                break;
            }
        }
        off = next;
    }
    c->off = off;
    cdb_storage_close(&db->rj_st);
    return CDB_OK;
}

/* Finds slot's journal, if there is one, and readies a cursor on it: a
 * damaged or foreign journal, or one whose next transaction would need an
 * archive state newer than this one, is set aside instead. *usable is 1 when
 * the cursor is ready. */
static cdb_status cursor_open(cdb_db *db, unsigned slot, struct jcursor *c, int *usable)
{
    char path[CDB_DB_PATH_MAX], tmp[CDB_DB_PATH_MAX];
    int exists, foreign;
    cdb_status s;

    *usable = 0;
    memset(c, 0, sizeof *c);
    c->slot = slot;
    journal_paths(db, slot, path, tmp);
    /* A .new journal on a slot with no live writer is left only by a crash
     * during rotation, before the rename; the old journal was still the real
     * one, so it is discarded. */
    if (!db->coord.peers[slot].live && (slot != db->coord.me.slot || db->journal_st.ops == NULL)) {
        if ((s = env_exists(db, tmp, &exists)) != CDB_OK)
            return s;
        if (exists && (s = env_remove(db, tmp)) != CDB_OK)
            return s;
    }
    if ((s = env_exists(db, path, &exists)) != CDB_OK || !exists)
        return s;
    s = open_journal(db, slot);
    if (s == CDB_E_CORRUPT || s == CDB_E_UNSUPPORTED || s == CDB_E_MALFORMED)
        return set_aside(db, path); /* unreadable (spec 1.12.2) */
    if (s != CDB_OK)
        return s;
    foreign = memcmp(db->rj.hdr.db_id, db->pager.hs.record.db_id, 16) != 0 || db->rj.hdr.slot != slot;
    c->unsealed = db->rj.hdr.unsealed_seq;
    c->off = CDB_JOURNAL_HEADER_SIZE;
    cdb_storage_close(&db->rj_st);
    if (foreign)
        return set_aside(db, path); /* another database's, or another slot's */
    if ((s = mark_get(db, slot, &c->mark)) != CDB_OK || (s = scan_next(db, c)) != CDB_OK)
        return s;
    /* Its transactions apply only onto the archive state it was started
     * from, or a later one that other slots' reconciliations made (spec
     * 14.4.6 rule 3, 14.4.7). Nothing to apply: no state is needed. */
    if (c->have && c->unsealed > db->pager.hs.record.committed_seq)
        return set_aside(db, path);
    c->has_journal = 1;
    *usable = 1;
    return CDB_OK;
}

/* Publishes the archive commit the pager is reading from (spec 5.9.7),
 * then re-reads the header to be sure no commit moved past it before the
 * pin was visible. */
static cdb_status pin_archive(cdb_db *db)
{
    for (;;) {
        cdb_header_state hs;
        uint64_t seq;
        cdb_status s;
        if ((s = cdb_header_open(&db->archive_st, &hs)) != CDB_OK)
            return s;
        if (hs.record.committed_seq != db->pager.hs.record.committed_seq
            && (s = cdb_pager_reload(&db->pager)) != CDB_OK)
            return s;
        seq = db->pager.hs.record.committed_seq;
        if ((s = cdb_coord_publish_pin(&db->coord, seq)) != CDB_OK)
            return s;
        if ((s = cdb_header_open(&db->archive_st, &hs)) != CDB_OK)
            return s;
        if (hs.record.committed_seq == seq)
            return CDB_OK;
    }
}

/* Checks the baton fence and updates the pager's reuse floor from the
 * others' published reader pins (spec 5.9.7, 5.9.8). */
static cdb_status fence_and_pins(cdb_db *db)
{
    cdb_status s;
    if ((s = cdb_coord_baton_fence(&db->coord)) != CDB_OK)
        return s;
    cdb_pager_set_reuse_below(&db->pager, db->coord.min_pin);
    db->pins_seq = db->pager.hs.record.committed_seq;
    return CDB_OK;
}

/* Raises each store's catalog entry-ID counter past every block listed by
 * any slot (spec 5.9.3). */
static cdb_status raise_block_counters(cdb_db *db)
{
    unsigned sl, i;
    cdb_status s;
    if (!db->has_catalog)
        return CDB_OK;
    for (sl = 0; sl < db->coord.cfg.max_coords; sl++) {
        cdb_coord_rec r;
        int valid = 0;
        if (sl == db->coord.me.slot) {
            r = db->coord.me;
            valid = 1;
        } else if ((s = cdb_coord_read_slot(&db->coord, sl, &r, &valid)) != CDB_OK) {
            return s;
        }
        if (!valid)
            continue;
        for (i = 0; i < r.nblocks; i++) {
            uint64_t store = r.blocks[i].store;
            uint64_t end = r.blocks[i].first + CDB_COORD_ID_BLOCK;
            cdb_store_info info;
            s = cdb_catalog_get_store(&db->catalog, store, &info);
            if (s == CDB_E_NOT_FOUND || (s == CDB_OK && (info.flags & CDB_STORE_DROPPED)))
                continue;
            if (s != CDB_OK)
                return s;
            if (info.next_entry_id < end
                && (s = cdb_catalog_stage_next_entry(&db->catalog, store, end)) != CDB_OK)
                return s;
        }
    }
    return CDB_OK;
}

/* Drops any held block from this slot's table whose store was dropped or
 * whose catalog counter is now at or past the block's end (spec 5.9.3). */
static cdb_status prune_held_blocks(cdb_db *db)
{
    unsigned i = 0;
    cdb_status s;
    if (!db->has_catalog)
        return CDB_OK;
    while (i < db->coord.me.nblocks) {
        uint64_t store = db->coord.me.blocks[i].store;
        uint64_t first = db->coord.me.blocks[i].first;
        cdb_store_info info;
        s = cdb_catalog_get_store(&db->catalog, store, &info);
        if (s == CDB_E_NOT_FOUND || (s == CDB_OK && (info.flags & CDB_STORE_DROPPED))
            || (s == CDB_OK && info.next_entry_id >= first + CDB_COORD_ID_BLOCK)) {
            if ((s = cdb_coord_drop_block(&db->coord, store, first)) != CDB_OK)
                return s;
            continue;
        }
        if (s != CDB_OK)
            return s;
        i++;
    }
    return CDB_OK;
}

/* Merges every slot's committed transactions above its mark onto the
 * archive in (stamp, slot, counter) order, commits them with the new marks,
 * and cleans up any dead slots whose journals are now folded in (spec 5.9.1,
 * 5.9.2, 14.4.7 rules 5 and 7). keep_in_use is 0 at recovery and when the
 * last live process closes, 1 otherwise. */
static cdb_status merge_journals(cdb_db *db, int keep_in_use)
{
    struct jcursor cur[CDB_COORD_MAX];
    int in_use = (journal_slot(db)->state & COMPANION_IN_USE) != 0;
    int recovering = !keep_in_use && db->journal_st.ops == NULL;
    int exists, usable, marks_moved = 0, has_blocks = 0, removed = 0;
    uint64_t base_wm = watermark_of(db);
    unsigned n = 0, slot, i;
    cdb_status s;

    db->applied = base_wm;
    for (slot = 0; slot < db->coord.cfg.max_coords; slot++) {
        cdb_coord_rec srec;
        int svalid = 0, slot_in_use = 0;
        int dead = slot != db->coord.me.slot && !db->coord.peers[slot].live;
        if (slot == db->coord.me.slot) {
            srec = db->coord.me;
            svalid = 1;
            slot_in_use = db->journal_st.ops != NULL || (!recovering && (srec.flags & CDB_COORD_JOURNAL) != 0);
        } else {
            if ((s = cdb_coord_read_slot(&db->coord, slot, &srec, &svalid)) != CDB_OK)
                return s;
            slot_in_use = svalid && (srec.flags & CDB_COORD_JOURNAL) != 0;
        }
        if (svalid && srec.nblocks > 0)
            has_blocks = 1;
        if ((s = cursor_open(db, slot, &cur[n], &usable)) != CDB_OK)
            return s;
        if (slot_in_use && (!usable || db->set_aside) && !(db->cfg.flags & CDB_DB_ACCEPT_LOSS))
            return CDB_E_CORRUPT; /* changes may have been lost (spec 1.12.5, 14.4.7 rule 7) */
        if (!usable && dead && svalid) {
            memset(&cur[n], 0, sizeof cur[n]);
            cur[n].slot = slot;
            if ((s = mark_get(db, slot, &cur[n].mark)) != CDB_OK)
                return s;
        }
        if (usable || (dead && svalid)) {
            cur[n].dead_slot = dead && svalid;
            cur[n].dead_hwm = dead && svalid ? srec.tx_hwm : 0;
            n++;
        }
    }
    if (in_use && (n == 0 || db->set_aside) && !(db->cfg.flags & CDB_DB_ACCEPT_LOSS))
        return CDB_E_CORRUPT; /* changes may have been lost (spec 1.12.5) */

    for (;;) {
        struct jcursor *best = NULL;
        for (i = 0; i < n; i++)
            if (cur[i].have && (best == NULL || goes_first(&cur[i], best)))
                best = &cur[i];
        if (best == NULL)
            break;
        if ((s = open_journal(db, best->slot)) != CDB_OK)
            return s;
        s = apply_range(db, &db->rj, best->tx_id, best->start, best->commit_at, db->applied + 1u);
        cdb_storage_close(&db->rj_st);
        if (s != CDB_OK)
            return s;
        db->applied++;
        cdb_coord_baton_progress(&db->coord, db->applied - base_wm);
        if ((s = scan_next(db, best)) != CDB_OK)
            return s;
    }

    /* Fold whatever was replayed into the archive, with the marks that
     * say so, before discarding dead slots' journals. */
    for (i = 0; i < n; i++) {
        if (cur[i].dead_hwm > cur[i].top)
            cur[i].top = cur[i].dead_hwm;
        marks_moved |= cur[i].top > cur[i].mark;
    }
    if (db->last_tx > db->my_mark)
        marks_moved = 1;
    if (db->applied != base_wm || in_use != keep_in_use || marks_moved || has_blocks || db->pager.dirty) {
        if ((s = cdb_doc_flush_pools(db)) != CDB_OK || (s = raise_block_counters(db)) != CDB_OK)
            return s;
        for (i = 0; i < n; i++)
            if (cur[i].top > cur[i].mark && (s = mark_set(db, cur[i].slot, cur[i].top)) != CDB_OK)
                return s;
        if ((s = stage_own_mark(db)) != CDB_OK)
            return s;
        set_journal_slot(db, keep_in_use, db->applied);
        if ((s = fence_and_pins(db)) != CDB_OK || (s = cdb_pager_commit(&db->pager)) != CDB_OK)
            return s;
        for (i = 0; i < n; i++)
            if (cur[i].slot == db->coord.me.slot && cur[i].top > db->my_mark)
                db->my_mark = cur[i].top;
        if (db->last_tx > db->my_mark)
            own_mark_committed(db);
        if (!recovering && (s = prune_held_blocks(db)) != CDB_OK)
            return s;
    }

    if (recovering && (db->coord.me.flags & CDB_COORD_JOURNAL)
        && (s = cdb_coord_set_journal(&db->coord, 0)) != CDB_OK)
        return s;
    if (recovering) {
        if ((s = env_exists(db, db->overlay_path, &exists)) != CDB_OK)
            return s;
        if (exists && (s = env_remove(db, db->overlay_path)) != CDB_OK)
            return s;
        removed |= exists;
    }
    for (i = 0; i < n; i++) {
        char path[CDB_DB_PATH_MAX], opath[CDB_DB_PATH_MAX];
        unsigned sl = cur[i].slot;
        if (sl == db->coord.me.slot) {
            if (recovering && cur[i].has_journal) {
                journal_paths(db, sl, path, NULL);
                if ((s = env_remove(db, path)) != CDB_OK)
                    return s;
                removed = 1;
            }
        } else if (!db->coord.peers[sl].live) {
            if (cur[i].dead_slot && (s = cdb_coord_remove_slot(&db->coord, sl)) != CDB_OK)
                return s;
            overlay_path_of(db, sl, opath);
            if ((s = env_exists(db, opath, &exists)) != CDB_OK)
                return s;
            if (exists && (s = env_remove(db, opath)) != CDB_OK)
                return s;
            removed |= exists;
            if (cur[i].has_journal) {
                journal_paths(db, sl, path, NULL);
                if ((s = env_remove(db, path)) != CDB_OK)
                    return s;
                removed = 1;
            }
        }
    }
    if (!removed)
        return CDB_OK;
    if ((s = env_exists(db, db->workdir, &exists)) != CDB_OK)
        return s;
    return exists ? env_sync_dir(db, db->workdir) : CDB_OK;
}

static cdb_status recover(cdb_db *db)
{
    return merge_journals(db, 0);
}

/* -------------------------------------------------------- session */

/* Creates the working directory and a fresh journal, then records the
 * journal as in use in the slot and the header. The order matters for crash
 * safety (spec 14.4.6 rule 2, 14.4.7 rule 7): "in use" is set only once the
 * journal and its directory entry are durable, so a missing journal marked
 * in use can only mean real loss. */
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
    if ((s = cdb_coord_set_journal(&db->coord, 1)) != CDB_OK)
        return s;
    /* Only now, with the journal durable, record it as in use. */
    set_journal_slot(db, 1, db->applied);
    if ((s = fence_and_pins(db)) != CDB_OK || (s = cdb_pager_commit(&db->pager)) != CDB_OK)
        return s;
    return pin_archive(db);
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
    total += (chunks - 1u) * cdb_slots_chunk_bytes(sizeof(struct cdb_table_store), CDB_SLOTS_CHUNK)
             + cdb_slots_chunk_bytes(sizeof(struct cdb_table_store), last);
    return total;
}

/* Gives every slot chunk of all four sets back (allocator memory is freed)
 * and forgets cached link and schema state. Used at close, on rebase or
 * reload, and when an open fails partway. */
static void release_slots(cdb_db *db)
{
    cdb_slots_release(db, &db->pool_slots);
    cdb_slots_release(db, &db->vec_slots);
    cdb_slots_release(db, &db->ts_slots);
    cdb_slots_release(db, &db->table_slots);
    db->slot_mem_used = 0;
    memset(&db->links, 0, sizeof db->links);
    db->schema_store = 0;
}

/* Empties this slot's overlay file and directs the pager's private writes to
 * it (spec 5.9.1). The pager must be clean. */
static cdb_status enter_overlay(cdb_db *db)
{
    cdb_status s;
    if (!db->have_overlay) {
        if ((s = env_open(db, db->overlay_path, CDB_ENV_CREATE, &db->overlay_st)) != CDB_OK)
            return s;
        db->have_overlay = 1;
    }
    if ((s = cdb_storage_truncate(&db->overlay_st, 0)) != CDB_OK)
        return s;
    cdb_pager_set_guard(&db->pager, NULL, NULL);
    db->fresh = 0;
    return cdb_pager_set_private(&db->pager, &db->overlay_st);
}

static cdb_status rotate_journal(cdb_db *db);

/* Drops this process's private pages, reloads the latest archive state, and
 * re-applies its own committed transactions above its mark onto a fresh
 * overlay while rotating its journal to the new committed sequence (spec
 * 5.9.1, 14.4.7 rule 2). */
static cdb_status rebase(cdb_db *db)
{
    cdb_header_state hs;
    cdb_status s;

    if ((s = cdb_header_open(&db->archive_st, &hs)) != CDB_OK)
        return s;
    if (db->pager.dirty || hs.record.committed_seq != db->pager.hs.record.committed_seq) {
        if ((s = cdb_pager_reload(&db->pager)) != CDB_OK)
            return s;
    }
    if ((s = pin_archive(db)) != CDB_OK || (s = enter_overlay(db)) != CDB_OK)
        return s;
    release_slots(db);
    if ((s = cdb_catalog_open(&db->catalog, &db->pager, &db->cfg.catalog_work, db->cfg.catalog_page)) != CDB_OK)
        return s;
    if ((s = prune_held_blocks(db)) != CDB_OK)
        return s;
    db->applied = watermark_of(db);
    if ((s = mark_get(db, db->coord.me.slot, &db->my_mark)) != CDB_OK)
        return s;

    if (db->last_tx <= db->my_mark) {
        if (db->journal_st.ops != NULL)
            return rotate_journal(db);
        if ((s = new_journal(db, db->journal_path, db->pager.hs.record.committed_seq)) != CDB_OK)
            return s;
        return env_sync_dir(db, db->workdir);
    }

    if (db->journal_st.ops != NULL) {
        cdb_storage_close(&db->journal_st);
        memset(&db->journal_st, 0, sizeof db->journal_st);
    }
    if ((s = new_journal(db, db->journal_tmp, db->pager.hs.record.committed_seq)) != CDB_OK)
        return s;
    {
        struct jcursor c;
        memset(&c, 0, sizeof c);
        c.slot = db->coord.me.slot;
        c.mark = db->my_mark;
        c.off = CDB_JOURNAL_HEADER_SIZE;
        while ((s = scan_next(db, &c)) == CDB_OK && c.have) {
            uint64_t off = c.start;
            if ((s = open_journal(db, c.slot)) != CDB_OK)
                return s;
            while (off < c.off) {
                cdb_record r;
                uint64_t next;
                if ((s = cdb_journal_read(&db->rj, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next))
                    != CDB_OK)
                    break;
                if ((r.kind == CDB_RECORD_DATA || r.kind == CDB_RECORD_WRITE_KEY || r.kind == CDB_RECORD_COMMIT)
                    && r.tx_id == c.tx_id) {
                    if ((s = cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size))
                        != CDB_OK)
                        break;
                    if (r.kind == CDB_RECORD_DATA) {
                        cdb_delta d;
                        if (r.payload_len < 1) {
                            s = CDB_E_MALFORMED;
                            break;
                        }
                        d.store_id = r.store_id;
                        d.entry_id = r.entry_id;
                        d.op = r.payload[0];
                        d.data = r.payload + 1;
                        d.len = r.payload_len - 1u;
                        d.commit_number = 0; /* assigned at reconciliation (spec 5.9.2) */
                        if ((s = db->cfg.apply(db->cfg.apply_ctx, &db->pager, &d)) != CDB_OK) {
                            db->failed = 1;
                            break;
                        }
                    }
                }
                off = next;
            }
            cdb_storage_close(&db->rj_st);
            if (s != CDB_OK)
                return s;
        }
        if (s != CDB_OK)
            return s;
    }
    if ((s = cdb_storage_sync(&db->journal_st)) != CDB_OK)
        return s;
    if ((s = db->env.ops->rename(db->env.ctx, db->journal_tmp, db->journal_path)) != CDB_OK)
        return s;
    db->unsynced = 0;
    return env_sync_dir(db, db->workdir);
}

static cdb_status maybe_rebase(cdb_db *db)
{
    cdb_header_state hs;
    cdb_status s;
    if (db->in_tx || cdb_coord_baton_held(&db->coord))
        return CDB_OK;
    if ((s = cdb_header_open(&db->archive_st, &hs)) != CDB_OK)
        return s;
    if (hs.record.committed_seq == db->pager.hs.record.committed_seq)
        return CDB_OK;
    return rebase(db);
}

/* After a baton operation (reconciliation, catalog change, compaction):
 * keep a standing baton while alone and uncontested, otherwise release
 * it and switch back to the overlay (spec 5.9.8). */
static cdb_status finish_baton(cdb_db *db)
{
    cdb_status s;
    if (db->coord.live_others == 0 && !cdb_coord_baton_wanted(&db->coord)) {
        db->fresh = 1;
        if (db->have_overlay && (s = cdb_storage_truncate(&db->overlay_st, 0)) != CDB_OK)
            return s;
        return CDB_OK;
    }
    if ((s = cdb_coord_baton_release(&db->coord)) != CDB_OK)
        return s;
    return enter_overlay(db);
}

static cdb_status open_db(cdb_db *db, const char *base, const cdb_db_config *cfg, const cdb_header *create);

cdb_status cdb_db_open(cdb_db *db, const cdb_env *env, const char *base, const cdb_db_config *cfg,
                       const cdb_header *create)
{
    cdb_status s;

    if (db == NULL || env == NULL || base == NULL || cfg == NULL
        || (cfg->apply == NULL && cfg->doc_buffers == NULL)
        || cfg->catalog_page == NULL || cfg->catalog_work.bytes == NULL /* the marks index needs it too */
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
    cdb_slots_init(&db->table_slots, sizeof(struct cdb_table_store), &cdb_table_slot_ops);
    if ((s = open_db(db, base, cfg, create)) != CDB_OK) {
        if (db->have_overlay) {
            cdb_storage_close(&db->overlay_st);
            (void)env_remove(db, db->overlay_path);
        }
        if (db->journal_st.ops != NULL)
            cdb_storage_close(&db->journal_st);
        if (db->archive_st.ops != NULL)
            cdb_storage_close(&db->archive_st);
        release_slots(db); /* recovery may have opened stores */
        (void)cdb_coord_leave(&db->coord); /* so the next opener need not wait it out */
    }
    return s;
}

/* The body of cdb_db_open, after the configuration is copied and the slot
 * sets are set up: open or create the archive, join the coordinators, and
 * either recover and take a standing baton (when alone) or start in private
 * overlay mode (when other processes are already live). */
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

    /* Join the coordinators before deciding whether the working directory
     * is a crashed session's (spec 1.12.1, 14.4.7 rule 9): only with no
     * other live coordinator may recovery replay and remove journals. */
    if ((s = db->env.ops->mkdir(db->env.ctx, db->workdir)) != CDB_OK)
        return s;
    if ((s = env_sync_dir(db, db->parent)) != CDB_OK)
        return s;
    if ((s = cdb_coord_join(&db->coord, &db->env, db->coorddir, db->pager.hs.record.db_id, db->pager.order,
                            &cfg->coord)) != CDB_OK)
        return s;
    journal_paths(db, db->coord.me.slot, db->journal_path, db->journal_tmp);
    overlay_path_of(db, db->coord.me.slot, db->overlay_path);
    if ((s = pin_archive(db)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_open(&db->catalog, &db->pager, &cfg->catalog_work, cfg->catalog_page)) != CDB_OK)
        return s;
    db->has_catalog = 1;

    if (db->coord.live_others == 0) {
        int granted = 0;
        if ((s = cdb_coord_baton_take(&db->coord, 0, &granted)) != CDB_OK)
            return s;
        if (!granted)
            return CDB_E_BUSY;
        cdb_pager_set_guard(&db->pager, (cdb_status (*)(void *))cdb_coord_guard, &db->coord);
        if ((s = recover(db)) != CDB_OK)
            return s;
        /* This slot's counters go on above everything it may have used before:
         * its mark in the archive, and the high-water mark a previous owner of
         * the slot reserved (spec 5.9.2). */
        if ((s = mark_get(db, db->coord.me.slot, &db->my_mark)) != CDB_OK)
            return s;
        db->last_tx = db->my_mark;
        db->next_tx = db->coord.me.tx_hwm > db->my_mark ? db->coord.me.tx_hwm : db->my_mark + 1u;
        if ((s = start_session(db)) != CDB_OK)
            return s;
        return finish_baton(db);
    }

    {
        struct jcursor c;
        int usable = 0;
        db->applied = watermark_of(db);
        if ((s = mark_get(db, db->coord.me.slot, &db->my_mark)) != CDB_OK)
            return s;
        db->last_tx = db->my_mark;
        /* A dead slot taken over while others are live adopts its journal
         * (spec 5.9.1, 14.4.7 rule 9). */
        if ((s = cursor_open(db, db->coord.me.slot, &c, &usable)) != CDB_OK)
            return s;
        if ((db->coord.me.flags & CDB_COORD_JOURNAL) && (!usable || db->set_aside)
            && !(db->cfg.flags & CDB_DB_ACCEPT_LOSS))
            return CDB_E_CORRUPT;
        if (usable) {
            while (c.have)
                if ((s = scan_next(db, &c)) != CDB_OK)
                    return s;
            if (c.top > db->last_tx)
                db->last_tx = c.top;
        }
        db->next_tx = db->coord.me.tx_hwm > db->last_tx ? db->coord.me.tx_hwm : db->last_tx + 1u;
        if ((s = rebase(db)) != CDB_OK)
            return s;
        return cdb_coord_set_journal(&db->coord, 1);
    }
}

/* -------------------------------------------------------- transactions */

/* After a failed apply, the pages no longer match the journal, so every
 * further operation is refused until the database is reopened. */
static cdb_status usable(const cdb_db *db)
{
    return db->failed ? CDB_E_STATE : CDB_OK;
}

/* Writes a heartbeat if one is due (spec 5.9.6), updates the reuse floor,
 * and drops a baton that another coordinator has overridden. Losing the
 * slot to another process means this session's journal may already have
 * been taken over: nothing more may be written. */
static cdb_status beat_only(cdb_db *db)
{
    cdb_status s = cdb_coord_beat(&db->coord, 0);
    if (s == CDB_E_BUSY) {
        db->failed = 1;
        return s;
    }
    if (s != CDB_OK)
        return s;
    cdb_pager_set_reuse_below(&db->pager, db->coord.min_pin);
    if (cdb_coord_baton_held(&db->coord) && db->coord.max_cts > db->coord.me.cts) {
        if ((s = cdb_coord_baton_release(&db->coord)) != CDB_OK)
            return s;
        if (!db->in_tx && db->op_depth <= 1)
            return rebase(db);
    }
    return CDB_OK;
}

static cdb_status heartbeat(cdb_db *db)
{
    cdb_status s = beat_only(db);
    if (s != CDB_OK)
        return s;
    if (!db->in_tx && db->op_depth <= 1) {
        /* A standing baton is reconciled and released as soon as another
         * live process asks for it (spec 5.9.8). */
        if (cdb_coord_baton_held(&db->coord) && cdb_coord_baton_wanted(&db->coord))
            return cdb_db_reconcile(db);
        return maybe_rebase(db);
    }
    return CDB_OK;
}

cdb_status cdb_db_tick(cdb_db *db)
{
    cdb_status s = usable(db);
    return s == CDB_OK ? heartbeat(db) : s;
}

/* Operation scopes (see cdb_db.h). Only the outermost begin takes a new ID;
 * nested scopes share it, so the whole outer scope counts as one operation
 * when the slot code decides which stores it may evict. An unmatched end is
 * ignored. The outermost begin is also a heartbeat and rebase point; a
 * failed beat shows up at the next call that can report it. */
void cdb_db_op_begin(cdb_db *db)
{
    if (db->op_depth++ == 0) {
        db->op_id = ++db->op_seq;
        if (!db->failed)
            (void)heartbeat(db);
    }
}

void cdb_db_op_end(cdb_db *db)
{
    if (db->op_depth > 0)
        db->op_depth--;
}

/* Starts a transaction. Only one may be open at a time. The transaction ID
 * is the coordinator slot in the top 16 bits and the slot's counter in the
 * low 48 (spec 14.4.3). Counters are reserved durably in the slot, a block
 * at a time, before any of the block is used. */
cdb_status cdb_tx_begin(cdb_db *db, cdb_durability durability)
{
    cdb_status s;
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if ((s = heartbeat(db)) != CDB_OK)
        return s;
    if (cdb_coord_baton_held(&db->coord)
        && (db->pins_seq != db->pager.hs.record.committed_seq || db->pager.reuse_below != 0)) {
        s = fence_and_pins(db);
        if (s == CDB_E_BUSY) {
            if ((s = cdb_coord_baton_release(&db->coord)) != CDB_OK || (s = rebase(db)) != CDB_OK)
                return s;
        } else if (s != CDB_OK) {
            return s;
        }
    }
    if (db->next_tx >= db->coord.me.tx_hwm
        && (s = cdb_coord_reserve_tx(&db->coord, db->next_tx + TX_RESERVE_BLOCK)) != CDB_OK)
        return s;
    db->in_tx = 1;
    db->tx_id = ((uint64_t)db->coord.me.slot << 48) | db->next_tx++;
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

/* Journals a write-set key record (kind 4, spec 5.9.4, 14.4.7 rule 3):
 * target store = table, target entry = 0, payload = 4-byte index number +
 * 4-byte CRC32C of the encoded key. Counts only for collision detection. */
cdb_status cdb_tx_put_key(cdb_db *db, uint64_t store_id, uint32_t index_num, const void *key, size_t key_len)
{
    uint8_t payload[CDB_WRITE_KEY_PAYLOAD_SIZE];
    cdb_record r;

    if (usable(db) != CDB_OK || !db->in_tx)
        return CDB_E_STATE;
    cdb_store_u32(payload + 0, index_num, db->pager.order);
    cdb_store_u32(payload + 4, cdb_crc32c(key, key_len), db->pager.order);
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_WRITE_KEY;
    r.tx_id = db->tx_id;
    r.store_id = store_id;
    r.entry_id = 0;
    r.payload = payload;
    r.payload_len = CDB_WRITE_KEY_PAYLOAD_SIZE;
    return cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size);
}

/* Ensures a 1024-ID block is claimed for store and returns its next ID
 * without consuming it yet (spec 5.9.3). */
cdb_status cdb_db_peek_entry(cdb_db *db, uint64_t store, const uint64_t *next_entry, uint64_t *id)
{
    uint64_t floor, first;
    unsigned i;
    cdb_status s;

    for (i = 0; i < db->nid_ranges; i++) {
        if (db->id_ranges[i].store == store) {
            if (db->id_ranges[i].next < db->id_ranges[i].end) {
                *id = db->id_ranges[i].next;
                return CDB_OK;
            }
            db->id_ranges[i] = db->id_ranges[--db->nid_ranges];
            break;
        }
    }
    if ((s = prune_held_blocks(db)) != CDB_OK)
        return s;
    if (db->nid_ranges == CDB_COORD_MAX_BLOCKS) {
        /* Evict an in-memory range whose block was already folded into the
         * catalog and dropped from coord/<slot>. */
        for (i = 0; i < db->nid_ranges; i++) {
            unsigned j;
            int held = 0;
            for (j = 0; j < db->coord.me.nblocks; j++)
                if (db->coord.me.blocks[j].store == db->id_ranges[i].store)
                    held = 1;
            if (!held) {
                db->id_ranges[i] = db->id_ranges[--db->nid_ranges];
                break;
            }
        }
        if (db->nid_ranges == CDB_COORD_MAX_BLOCKS)
            return CDB_E_BUFFER_TOO_SMALL;
    }
    floor = *next_entry ? *next_entry : 1u;
    if (db->has_catalog) {
        cdb_store_info info;
        s = cdb_catalog_get_store(&db->catalog, store, &info);
        if (s == CDB_OK && info.next_entry_id > floor)
            floor = info.next_entry_id;
        else if (s != CDB_OK && s != CDB_E_NOT_FOUND)
            return s;
    }
    if ((s = cdb_coord_claim_block(&db->coord, store, floor, &first)) != CDB_OK)
        return s;
    db->id_ranges[db->nid_ranges].store = store;
    db->id_ranges[db->nid_ranges].next = first;
    db->id_ranges[db->nid_ranges].end = first + CDB_COORD_ID_BLOCK;
    db->nid_ranges++;
    *id = first;
    return CDB_OK;
}

uint64_t cdb_db_take_entry(cdb_db *db, uint64_t store, uint64_t *next_entry, int *dirty)
{
    uint64_t id = *next_entry ? *next_entry : 1u;
    unsigned i;
    for (i = 0; i < db->nid_ranges; i++) {
        if (db->id_ranges[i].store == store) {
            id = db->id_ranges[i].next++;
            if (db->id_ranges[i].next >= db->id_ranges[i].end)
                db->id_ranges[i] = db->id_ranges[--db->nid_ranges];
            break;
        }
    }
    if (id >= *next_entry)
        *next_entry = id + 1u;
    if (dirty != NULL)
        *dirty = 1;
    return id;
}

/* The commit stamp (spec 5.9.2, 14.4.4): the primary's clock in ns, kept
 * rising within this journal so the merge never reorders one process's own
 * transactions. */
static uint64_t next_stamp(cdb_db *db)
{
    int64_t t = cdb_coord_clock(&db->coord);
    uint64_t stamp = t > 0 ? (uint64_t)t : 0u;
    if (stamp <= db->last_stamp)
        stamp = db->last_stamp + 1u;
    db->last_stamp = stamp;
    return stamp;
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

static cdb_status append_abort(cdb_db *db)
{
    cdb_record r;
    if (db->tx_deltas == 0)
        return CDB_OK;
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_ABORT;
    r.tx_id = db->tx_id;
    return cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size);
}

#define WS_CACHE_CAP 64u

struct ws_item {
    uint64_t store;
    uint64_t entry;   /* 0 for CDB_RECORD_WRITE_KEY */
    uint64_t key_tag; /* 8-byte payload of CDB_RECORD_WRITE_KEY */
};

static int ws_from_record(const cdb_record *r, struct ws_item *out)
{
    if (r->kind == CDB_RECORD_DATA) {
        out->store = r->store_id;
        out->entry = r->entry_id;
        out->key_tag = 0;
        return 1;
    }
    if (r->kind == CDB_RECORD_WRITE_KEY && r->payload_len == CDB_WRITE_KEY_PAYLOAD_SIZE) {
        out->store = r->store_id;
        out->entry = 0;
        out->key_tag = cdb_load_u64(r->payload, CDB_ORDER_LITTLE);
        return 1;
    }
    return 0;
}

static cdb_status own_has_item(cdb_db *db, const struct ws_item *own, unsigned nown, uint64_t more_at,
                               uint64_t own_end, const struct ws_item *needle, int *match)
{
    uint64_t off;
    unsigned i;
    *match = 0;
    for (i = 0; i < nown; i++) {
        if (own[i].store == needle->store && own[i].entry == needle->entry && own[i].key_tag == needle->key_tag) {
            *match = 1;
            return CDB_OK;
        }
    }
    for (off = more_at; off < own_end;) {
        cdb_record r;
        struct ws_item it;
        uint64_t next;
        cdb_status s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next);
        if (s != CDB_OK)
            return s;
        off = next;
        if (r.tx_id == db->tx_id && ws_from_record(&r, &it)
            && it.store == needle->store && it.entry == needle->entry && it.key_tag == needle->key_tag) {
            *match = 1;
            return CDB_OK;
        }
    }
    return CDB_OK;
}

/* Scans peer slot's journal above its archive mark for any transaction whose
 * write set overlaps this transaction's (spec 5.9.4). An overlapping
 * committed transaction or an overlapping earlier prepare rejects this
 * transaction with CDB_E_COLLISION; an overlapping later prepare is waited
 * out until it commits, aborts, or its coordinator dies. */
static cdb_status check_slot_collisions(cdb_db *db, unsigned slot, uint64_t my_stamp,
                                        const struct ws_item *own, unsigned nown,
                                        uint64_t more_at, uint64_t own_end)
{
    char path[CDB_DB_PATH_MAX];
    uint64_t mark = 0;
    int exists;
    cdb_status s;

    if ((s = mark_get(db, slot, &mark)) != CDB_OK)
        return s;
    journal_paths(db, slot, path, NULL);

    for (;;) {
        uint64_t off, cur_tx = 0, prep_stamp = 0;
        int cur_overlap = 0, cur_prepared = 0;

        if ((s = env_exists(db, path, &exists)) != CDB_OK || !exists)
            return s;
        s = open_journal(db, slot);
        if (s == CDB_E_NOT_FOUND || s == CDB_E_CORRUPT || s == CDB_E_MALFORMED || s == CDB_E_UNSUPPORTED)
            return CDB_OK;
        if (s != CDB_OK)
            return s;
        if (memcmp(db->rj.hdr.db_id, db->pager.hs.record.db_id, 16) != 0 || db->rj.hdr.slot != slot) {
            cdb_storage_close(&db->rj_st);
            return CDB_OK;
        }

        off = CDB_JOURNAL_HEADER_SIZE;
        for (;;) {
            cdb_record r;
            struct ws_item it;
            uint64_t next, counter;
            if (cdb_journal_read(&db->rj, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next) != CDB_OK)
                break;
            off = next;
            if ((r.tx_id >> 48) != slot)
                continue;
            counter = r.tx_id & TX_COUNTER_MASK;
            if (r.kind == CDB_RECORD_DATA || r.kind == CDB_RECORD_WRITE_KEY) {
                if (r.tx_id != cur_tx) {
                    cur_tx = r.tx_id;
                    cur_overlap = 0;
                    cur_prepared = 0;
                    prep_stamp = 0;
                }
                if (counter > mark && !cur_overlap && ws_from_record(&r, &it)) {
                    if ((s = own_has_item(db, own, nown, more_at, own_end, &it, &cur_overlap)) != CDB_OK) {
                        cdb_storage_close(&db->rj_st);
                        return s;
                    }
                }
            } else if (r.kind == CDB_RECORD_PREPARE && r.tx_id == cur_tx
                       && r.payload_len == CDB_PREPARE_PAYLOAD_SIZE) {
                if (counter > mark) {
                    cur_prepared = 1;
                    prep_stamp = cdb_load_u64(r.payload, db->pager.order);
                }
            } else if (r.kind == CDB_RECORD_ABORT && r.tx_id == cur_tx) {
                cur_tx = 0;
                cur_overlap = 0;
                cur_prepared = 0;
            } else if (r.kind == CDB_RECORD_COMMIT && r.tx_id == cur_tx) {
                cdb_commit_info ci;
                if (cdb_commit_decode(&r, &ci, db->pager.order) == CDB_OK && counter > mark && cur_overlap) {
                    cdb_storage_close(&db->rj_st);
                    return CDB_E_COLLISION;
                }
                cur_tx = 0;
                cur_overlap = 0;
                cur_prepared = 0;
            }
        }
        cdb_storage_close(&db->rj_st);

        if (cur_tx != 0 && cur_prepared && cur_overlap) {
            int64_t step;
            if (prep_stamp < my_stamp || (prep_stamp == my_stamp && slot < db->coord.me.slot))
                return CDB_E_COLLISION;
            if (!db->coord.have_mono || db->env.ops->sleep == NULL || !db->coord.peers[slot].live)
                return CDB_OK;
            step = db->coord.cfg.settle_ns > 0 ? db->coord.cfg.settle_ns : 100000;
            (void)db->env.ops->sleep(db->env.ctx, step);
            if ((s = beat_only(db)) != CDB_OK)
                return s;
            if (!db->coord.peers[slot].live)
                return CDB_OK;
            continue;
        }
        return CDB_OK;
    }
}

static cdb_status check_collisions(cdb_db *db, uint64_t own_end, uint64_t my_stamp)
{
    struct ws_item own[WS_CACHE_CAP];
    unsigned nown = 0, slot;
    uint64_t off = db->tx_start, more_at = own_end;
    cdb_status s;

    while (off < own_end) {
        cdb_record r;
        struct ws_item it;
        uint64_t next;
        if ((s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next)) != CDB_OK)
            return s;
        if (r.tx_id == db->tx_id && ws_from_record(&r, &it)) {
            if (nown < WS_CACHE_CAP) {
                own[nown++] = it;
            } else {
                more_at = off;
                break;
            }
        }
        off = next;
    }
    for (slot = 0; slot < db->coord.cfg.max_coords; slot++) {
        if (slot == db->coord.me.slot)
            continue;
        if ((s = check_slot_collisions(db, slot, my_stamp, own, nown, more_at, own_end)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* The coordination half of a commit (spec 5.9.4), run while the transaction
 * is still marked open so that heartbeats taken while it waits (the settle
 * interval, a joiner's first-commit wait, waiting out a later prepare) never
 * rebase and rotate the journal under it. Returns the stamp the commit
 * record carries; CDB_E_COLLISION when this transaction is the later
 * writer. */
static cdb_status coordinate_commit(cdb_db *db, uint64_t *stamp)
{
    cdb_record r;
    uint64_t prep_at;
    cdb_status s;

    s = cdb_coord_commit_ready(&db->coord);
    if (s == CDB_E_BUSY) {
        db->failed = 1; /* the slot was lost while silent (spec 5.9.6) */
        return s;
    }
    if (s != CDB_OK)
        return s;
    cdb_pager_set_reuse_below(&db->pager, db->coord.min_pin);
    if (cdb_coord_baton_held(&db->coord) && db->coord.max_cts > db->coord.me.cts
        && (s = cdb_coord_baton_release(&db->coord)) != CDB_OK)
        return s;

    /* If another process reconciled into the archive while this transaction
     * was open, its view was older than the archive and the reconciled write
     * sets are no longer in any journal: it is rejected (spec 5.9.4). */
    if (!cdb_coord_baton_held(&db->coord) || db->coord.live_others > 0) {
        cdb_header_state hs;
        if ((s = cdb_header_open(&db->archive_st, &hs)) != CDB_OK)
            return s;
        if (hs.record.committed_seq != db->pager.hs.record.committed_seq)
            return CDB_E_COLLISION;
    }

    *stamp = next_stamp(db);
    if (db->coord.live_others == 0)
        return CDB_OK; /* only coordinator: no prepare, settle or scan */

    /* Prepare, settle, scan (spec 5.9.4, 14.4.7 rule 4). */
    {
        uint8_t prep[CDB_PREPARE_PAYLOAD_SIZE];
        cdb_store_u64(prep, *stamp, db->pager.order);
        memset(&r, 0, sizeof r);
        r.kind = (uint8_t)CDB_RECORD_PREPARE;
        r.tx_id = db->tx_id;
        r.payload = prep;
        r.payload_len = CDB_PREPARE_PAYLOAD_SIZE;
        prep_at = db->journal.end;
        if ((s = cdb_journal_append(&db->journal, &r, db->cfg.record_buf, db->cfg.record_buf_size)) != CDB_OK)
            return s;
    }
    /* Other processes read this journal while it is unsynced: the page
     * cache shares it, so no sync is needed for them to see it. */
    if (db->coord.have_mono && db->env.ops->sleep != NULL && db->coord.cfg.settle_ns > 0)
        (void)db->env.ops->sleep(db->env.ctx, db->coord.cfg.settle_ns);
    return check_collisions(db, prep_at, *stamp);
}

cdb_status cdb_tx_commit(cdb_db *db)
{
    uint8_t info[CDB_COMMIT_PAYLOAD_SIZE];
    cdb_commit_info ci;
    cdb_record r;
    uint64_t stamp = 0, record_at, counter;
    cdb_status s;

    if (usable(db) != CDB_OK || !db->in_tx)
        return CDB_E_STATE;
    /* An empty transaction leaves no trace and uses no commit number. */
    if (db->tx_deltas == 0) {
        db->in_tx = 0;
        return CDB_OK;
    }

    s = coordinate_commit(db, &stamp);
    if (s != CDB_OK) {
        /* The transaction ends here: an abort record closes its run of
         * records, so neither replay nor another process's scan counts it. */
        cdb_status as = db->failed ? CDB_OK : append_abort(db);
        db->in_tx = 0;
        if (as != CDB_OK)
            return as;
        /* A rejected writer moves to the current archive at once, so its
         * retry sees what it collided with (spec 5.9.4). */
        if (s == CDB_E_COLLISION && db->op_depth <= 1 && !cdb_coord_baton_held(&db->coord)
            && (as = rebase(db)) != CDB_OK)
            return as;
        return s;
    }
    db->in_tx = 0;

    /* Build and append the commit record (spec 14.4.4). Its presence in
     * the journal is what makes the transaction count at replay. The commit
     * number is left 0: reconciliation assigns it (spec 5.9.2). When a
     * prepare record was written, it carries the same stamp. */
    memset(&ci, 0, sizeof ci);
    ci.timestamp = stamp;
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

    counter = db->tx_id & TX_COUNTER_MASK;
    /* The transaction is committed; make it visible through the pager. Its
     * commit number is the one reconciliation will give it while this
     * session is the only one contributing, and 0 on an overlay (spec 5.9.2). */
    s = apply_range(db, &db->journal, db->tx_id, db->tx_start, record_at, db->fresh ? db->applied + 1u : 0u);
    if (s == CDB_E_BUSY && cdb_coord_baton_held(&db->coord)) {
        /* A standing baton expired while applying: give it up and replay
         * onto the overlay instead (spec 5.9.8). */
        db->last_tx = counter;
        if ((s = cdb_coord_baton_release(&db->coord)) == CDB_OK)
            s = rebase(db);
        if (s != CDB_OK)
            db->failed = 1;
        return s;
    }
    if (s != CDB_OK) {
        db->failed = 1; /* the journal holds it; reopening replays it */
        return s;
    }
    if (db->fresh)
        db->applied++;
    db->last_tx = counter;
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

/* Releases the baton and rebases when an archive write or fence check fails
 * because the baton was lost (spec 5.9.8). */
static cdb_status on_baton_err(cdb_db *db, cdb_status s)
{
    if (s == CDB_E_BUSY) {
        (void)cdb_coord_baton_release(&db->coord);
        (void)rebase(db);
    }
    return s;
}

/* Takes the baton if needed, commits every slot's applied changes to the
 * archive, and leaves the baton held so a caller (catalog change,
 * compaction, close) can continue under the same baton session (spec 5.9.8).
 * When closing is 0, the journal is rotated and the reader pin updated. */
static cdb_status reconcile_hold(cdb_db *db, int closing)
{
    int keep_in_use;
    cdb_status s;

    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if ((s = beat_only(db)) != CDB_OK)
        return s;
    if (!cdb_coord_baton_held(&db->coord)) {
        int granted = 0;
        if ((s = cdb_coord_baton_take(&db->coord, 0, &granted)) != CDB_OK)
            return s;
        if (!granted)
            return CDB_E_BUSY;
    }
    if ((s = fence_and_pins(db)) != CDB_OK)
        return on_baton_err(db, s);
    if (db->unsynced > 0) {
        if ((s = cdb_storage_sync(&db->journal_st)) != CDB_OK)
            return s;
        db->unsynced = 0;
    }
    keep_in_use = !closing || db->coord.live_others > 0;

    /* Fast path: this process held the baton from the last archive commit
     * and no other slot has a journal, so its pager already holds the exact
     * state to commit. */
    if (db->fresh && db->coord.journals_others == 0) {
        int in_use = (journal_slot(db)->state & COMPANION_IN_USE) != 0;
        cdb_pager_set_guard(&db->pager, (cdb_status (*)(void *))cdb_coord_guard, &db->coord);
        if (db->applied != watermark_of(db) || db->pager.dirty || in_use != keep_in_use
            || db->last_tx > db->my_mark || db->coord.me.nblocks > 0) {
            if ((s = cdb_doc_flush_pools(db)) != CDB_OK || (s = raise_block_counters(db)) != CDB_OK
                || (s = stage_own_mark(db)) != CDB_OK)
                return on_baton_err(db, s);
            set_journal_slot(db, keep_in_use, db->applied);
            if ((s = fence_and_pins(db)) != CDB_OK || (s = cdb_pager_commit(&db->pager)) != CDB_OK)
                return on_baton_err(db, s);
            own_mark_committed(db);
            if (!closing && (s = prune_held_blocks(db)) != CDB_OK)
                return s;
        }
        if (!closing) {
            cdb_slots_shrink(db, &db->pool_slots);
            cdb_slots_shrink(db, &db->vec_slots);
            cdb_slots_shrink(db, &db->ts_slots);
            cdb_slots_shrink(db, &db->table_slots);
            if (db->last_tx <= db->my_mark && db->journal.end > CDB_JOURNAL_HEADER_SIZE) {
                if ((s = rotate_journal(db)) != CDB_OK)
                    return s;
            }
            if ((s = pin_archive(db)) != CDB_OK)
                return s;
        }
        return CDB_OK;
    }

    if ((s = cdb_pager_reload(&db->pager)) != CDB_OK || (s = cdb_pager_set_private(&db->pager, NULL)) != CDB_OK)
        return s;
    cdb_pager_set_guard(&db->pager, (cdb_status (*)(void *))cdb_coord_guard, &db->coord);
    cdb_pager_set_reuse_below(&db->pager, db->coord.min_pin);
    release_slots(db);
    if ((s = cdb_catalog_open(&db->catalog, &db->pager, &db->cfg.catalog_work, db->cfg.catalog_page)) != CDB_OK)
        return s;
    if ((s = merge_journals(db, keep_in_use)) != CDB_OK)
        return on_baton_err(db, s);
    db->fresh = 1;
    if (!closing) {
        if ((s = rotate_journal(db)) != CDB_OK || (s = pin_archive(db)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* Commits every applied change to the archive (spec 3.2, 5.9.1). Not allowed
 * with a transaction open. The new watermark is stored in the same header
 * commit as the pages, so the archive and the watermark always agree. */
cdb_status cdb_db_reconcile(cdb_db *db)
{
    cdb_status s = reconcile_hold(db, 0);
    return s == CDB_OK ? finish_baton(db) : s;
}

/* Common start of every catalog change: no transaction may be open, a
 * catalog must be configured (CDB_E_UNSUPPORTED otherwise), and everything
 * applied so far is reconciled first under the baton. The catalog change
 * then commits on its own, after which finish_archive_op rotates the journal
 * and finishes the baton session (spec 4, 5.9.8). */
static cdb_status catalog_ready(cdb_db *db)
{
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    return reconcile_hold(db, 0);
}

static cdb_status finish_archive_op(cdb_db *db, cdb_status op_status)
{
    cdb_status s;
    if (op_status != CDB_OK) {
        if (cdb_coord_baton_held(&db->coord)
            && (op_status == CDB_E_BUSY || db->coord.live_others > 0 || cdb_coord_baton_wanted(&db->coord)))
            (void)cdb_coord_baton_release(&db->coord);
        if (db->pager.dirty || !cdb_coord_baton_held(&db->coord))
            (void)rebase(db);
        return op_status;
    }
    if ((s = rotate_journal(db)) != CDB_OK || (s = pin_archive(db)) != CDB_OK)
        return s;
    return finish_baton(db);
}

cdb_status cdb_db_create_store(cdb_db *db, const char *name, size_t len, uint32_t model, uint64_t *id)
{
    cdb_status s = catalog_ready(db);
    if (s != CDB_OK)
        return s;
    return finish_archive_op(db, cdb_catalog_create_store(&db->catalog, name, len, model, id));
}

/* A read: no reconcile and no rotation needed, but rebases if the archive
 * has moved. */
cdb_status cdb_db_find_store(cdb_db *db, const char *name, size_t len, uint64_t *id, int *found)
{
    cdb_status s;
    if (usable(db) != CDB_OK)
        return CDB_E_STATE;
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    if ((s = heartbeat(db)) != CDB_OK)
        return s;
    return cdb_catalog_find_store(&db->catalog, name, len, id, found);
}

cdb_status cdb_db_drop_store(cdb_db *db, uint64_t id)
{
    cdb_status s = catalog_ready(db);
    cdb_slot *slot;
    if (s != CDB_OK)
        return s;
    if (cdb_link_is_store(db, id))
        return finish_archive_op(db, CDB_E_INVALID_ARG); /* the link store is the engine's own */
    /* Its entries' links are tombstoned; the drop's commit carries that. */
    if ((s = cdb_link_on_drop(db, id)) == CDB_OK)
        s = cdb_catalog_drop_store(&db->catalog, id);
    if (s != CDB_OK)
        return finish_archive_op(db, s);
    /* Forget the store's open state, so it reads as dropped from now on. */
    if ((slot = cdb_slots_find(&db->pool_slots, id)) != NULL
        || (slot = cdb_slots_find(&db->vec_slots, id)) != NULL
        || (slot = cdb_slots_find(&db->ts_slots, id)) != NULL)
        slot->in_use = 0;
    if ((slot = cdb_slots_find(&db->table_slots, id)) != NULL)
        slot->in_use = 0;
    {
        unsigned i = 0;
        while (i < db->nid_ranges) {
            if (db->id_ranges[i].store == id)
                db->id_ranges[i] = db->id_ranges[--db->nid_ranges];
            else
                i++;
        }
    }
    return finish_archive_op(db, prune_held_blocks(db));
}

cdb_status cdb_db_catalog_begin(cdb_db *db)
{
    return catalog_ready(db);
}

cdb_status cdb_db_catalog_end(cdb_db *db, cdb_status s)
{
    return finish_archive_op(db, s);
}

cdb_status cdb_db_add_schema(cdb_db *db, uint64_t store_id, const void *schema, size_t len, uint32_t *version)
{
    cdb_status s = catalog_ready(db);
    if (s != CDB_OK)
        return s;
    return finish_archive_op(db, cdb_catalog_add_schema(&db->catalog, store_id, schema, len, version));
}

/* Compaction requires a clean pager, hence the reconcile first. It makes
 * its own archive commits under the same baton, so the journal is rotated
 * afterwards to match the new committed sequence. */
cdb_status cdb_db_compact(cdb_db *db, uint64_t max_moves, uint64_t *moved, uint64_t *reclaimed)
{
    cdb_status s;
    if (usable(db) != CDB_OK || db->in_tx)
        return CDB_E_STATE;
    if ((s = reconcile_hold(db, 0)) != CDB_OK)
        return s;
    return finish_archive_op(db, cdb_pager_compact(&db->pager, max_moves, moved, reclaimed));
}

/* Close is a final reconciliation under the baton, followed by removing
 * this slot's journal and overlay, and, when no other process is live,
 * marking the header's companion slot "not in use" and removing the working
 * directory (spec 14.4.6 rule 2, 14.4.7 rule 7). A failed database is
 * refused: its journal must stay so the next open can replay it. An open
 * transaction is aborted. */
cdb_status cdb_db_close(cdb_db *db)
{
    int alone;
    cdb_status s;

    if (db->failed) {
        if (db->have_overlay) {
            cdb_storage_close(&db->overlay_st);
            (void)env_remove(db, db->overlay_path);
        }
        if (db->journal_st.ops != NULL)
            cdb_storage_close(&db->journal_st);
        if (db->archive_st.ops != NULL)
            cdb_storage_close(&db->archive_st);
        release_slots(db); /* their state is lost with the session anyway */
        (void)cdb_coord_leave(&db->coord); /* the journal stays for the next open */
        return CDB_E_STATE;
    }
    if (db->in_tx && (s = cdb_tx_abort(db)) != CDB_OK)
        return s;
    if (db->coord.live_others == 0 && db->coord.journals_others == 0 && db->fresh)
        db->coord.me.nblocks = 0;
    if ((s = reconcile_hold(db, 1)) != CDB_OK)
        return s;
    alone = db->coord.live_others == 0;
    release_slots(db); /* everything they held is in the archive */
    if ((s = cdb_coord_set_journal(&db->coord, 0)) != CDB_OK)
        return s;
    if (db->have_overlay) {
        cdb_storage_close(&db->overlay_st);
        db->have_overlay = 0;
        if ((s = env_remove(db, db->overlay_path)) != CDB_OK && s != CDB_E_NOT_FOUND)
            return s;
    }
    cdb_storage_close(&db->journal_st);
    if ((s = env_remove(db, db->journal_path)) != CDB_OK)
        return s;
    if ((s = env_sync_dir(db, db->workdir)) != CDB_OK)
        return s;
    if ((s = cdb_coord_leave(&db->coord)) != CDB_OK)
        return s;
    if (alone) {
        s = db->env.ops->rmdir(db->env.ctx, db->coorddir);
        if (s != CDB_OK && s != CDB_E_STATE && s != CDB_E_NOT_FOUND) /* a leftover lobby file keeps it */
            return s;
        s = db->env.ops->rmdir(db->env.ctx, db->workdir);
        if (s != CDB_OK && s != CDB_E_STATE) /* a set-aside companion keeps it */
            return s;
        if ((s = env_sync_dir(db, db->parent)) != CDB_OK)
            return s;
    }
    cdb_storage_close(&db->archive_st);
    return CDB_OK;
}
