/* Links between entries (spec 6, 14.13).
 *
 * Layout reminder: the forward index is keyed (source, type, target) and
 * holds the link's value; the reverse index is keyed (target, type, source)
 * and holds nothing. So "links out of X" is a prefix scan of the forward
 * index, and "links into X" is a prefix scan of the reverse index followed
 * by a lookup of each row in the forward index. Every key part is fixed
 * width and big-endian, so byte order is sort order.
 *
 * As with documents, the public calls check everything inside the open
 * transaction and journal a record; cdb_link_apply writes the rows at
 * commit and must never fail for logical reasons. Journal records go to the
 * link store with entry ID 0; the payload names the link. */
#include <string.h>

#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_link.h"
#include "cdb_table.h"
#include "cdb_ts.h"
#include "cdb_vec.h"

#define CDB_REF_LEN 25u
#define KEY_LEN (CDB_REF_LEN + 8u + CDB_REF_LEN) /* 58u: ref, type, ref */
#define VAL_LEN 17u                              /* weight (f32 bits, 4), time (8), flags (1), removed count (4) */
#define LINK_PAYLOAD (1u + 8u + CDB_REF_LEN + CDB_REF_LEN + 4u + 8u) /* 71u: mode (1), type, source, target, weight, time */
#define UNLINK_PAYLOAD (8u + CDB_REF_LEN + CDB_REF_LEN)              /* 58u: type, source, target */
#define STAGE_LINK_LEN (8u + 2u * CDB_REF_LEN)                       /* 58u */
#define MODE_CREATE 1u     /* LINK record: add (or revive) the link */
#define MODE_REINFORCE 2u  /* LINK record: update a live link's weight and time */

/* -------------------------------------------------------- the store */

/* Whether store is the link store. The catalog is asked once per session
 * (by name); the answer is cached in db->links. */
int cdb_link_is_store(cdb_db *db, uint64_t store)
{
    struct cdb_link_store *ls = &db->links;
    if (!ls->store_id && !ls->looked && db->has_catalog) {
        uint64_t id;
        int found;
        if (cdb_catalog_find_store(&db->catalog, CDB_LINK_STORE_NAME, sizeof CDB_LINK_STORE_NAME - 1u, &id, &found)
                == CDB_OK) {
            ls->looked = 1;
            if (found)
                ls->store_id = id;
        }
    }
    return store != 0 && store == ls->store_id;
}

/* Opens the link store's indexes. CDB_E_NOT_FOUND when there are no links
 * yet. With create (inside a catalog change), makes what is missing: the
 * store commits at once; its index roots are staged for the next commit. */
static cdb_status open_store(cdb_db *db, int create)
{
    struct cdb_link_store *ls = &db->links;
    cdb_store_info info;
    cdb_status s;
    if (ls->open)
        return CDB_OK;
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    (void)cdb_link_is_store(db, 0);
    if (!ls->store_id) {
        if (!create)
            return CDB_E_NOT_FOUND;
        if ((s = cdb_catalog_create_store(&db->catalog, CDB_LINK_STORE_NAME, sizeof CDB_LINK_STORE_NAME - 1u,
                                          CDB_MODEL_LINK, &ls->store_id)) != CDB_OK)
            return s;
        ls->looked = 1;
    }
    if ((s = cdb_catalog_get_store(&db->catalog, ls->store_id, &info)) != CDB_OK)
        return s;
    if (info.head_root && info.rev_root) {
        if ((s = cdb_index_open(&ls->fwd, &db->pager, info.head_root, &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_open(&ls->rev, &db->pager, info.rev_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
    } else {
        /* The store exists but has no index roots yet: either it was just
         * created above, or a crash came between the store's commit and its
         * first type's. The roots are kept in the store record's document
         * pool fields (12 and 13, spec 14.3.3). */
        if (!create)
            return CDB_E_NOT_FOUND;
        if ((s = cdb_index_create(&ls->fwd, &db->pager, ls->store_id, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                                  &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_create(&ls->rev, &db->pager, ls->store_id, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                                     &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_catalog_stage_pool(&db->catalog, ls->store_id, ls->fwd.root, ls->rev.root, 0)) != CDB_OK)
            return s;
    }
    ls->open = 1;
    return CDB_OK;
}

/* Looks up a link type, reusing *t when it already holds that type (so a
 * scan over many links of one type reads the catalog once).
 * CDB_E_NOT_FOUND for an ID that is not a link type. */
static cdb_status type_of(cdb_db *db, uint64_t type, cdb_link_type_info *t)
{
    if (t->id == type && type != 0)
        return CDB_OK; /* the one looked up last */
    return cdb_catalog_get_link_type(&db->catalog, type, t);
}

/* -------------------------------------------------------- rows */

/* A reference as 25 bytes: store ID (8), entry ID (8), flags (1), ts (8), big-endian. */
static void put_ref(uint8_t *p, cdb_ref r)
{
    cdb_store_u64(p, r.store, CDB_ORDER_BIG);
    cdb_store_u64(p + 8, r.entry, CDB_ORDER_BIG);
    p[16] = (uint8_t)r.flags;
    cdb_store_u64(p + 17, (uint64_t)r.ts, CDB_ORDER_BIG);
}

static cdb_ref get_ref(const uint8_t *p)
{
    cdb_ref r;
    r.store = cdb_load_u64(p, CDB_ORDER_BIG);
    r.entry = cdb_load_u64(p + 8, CDB_ORDER_BIG);
    r.flags = (uint32_t)p[16];
    r.ts = (int64_t)cdb_load_u64(p + 17, CDB_ORDER_BIG);
    return r;
}

static void k_fwd(uint8_t *k, uint64_t type, cdb_ref s, cdb_ref t)
{
    put_ref(k, s);
    cdb_store_u64(k + CDB_REF_LEN, type, CDB_ORDER_BIG);
    put_ref(k + CDB_REF_LEN + 8u, t);
}

static void k_rev(uint8_t *k, uint64_t type, cdb_ref s, cdb_ref t)
{
    put_ref(k, t);
    cdb_store_u64(k + CDB_REF_LEN, type, CDB_ORDER_BIG);
    put_ref(k + CDB_REF_LEN + 8u, s);
}

/* Forward-index value: the weight's IEEE-754 bits, the time, the flags and
 * the removal count (VAL_LEN bytes, big-endian). */
static void encode_val(uint8_t *v, const cdb_link *l)
{
    uint32_t bits;
    memcpy(&bits, &l->weight, sizeof bits);
    cdb_store_u32(v, bits, CDB_ORDER_BIG);
    cdb_store_u64(v + 4, (uint64_t)l->time, CDB_ORDER_BIG);
    v[12] = l->flags;
    cdb_store_u32(v + 13, l->removed, CDB_ORDER_BIG);
}

static void decode_val(const uint8_t *v, cdb_link *l)
{
    uint32_t bits = cdb_load_u32(v, CDB_ORDER_BIG);
    memcpy(&l->weight, &bits, sizeof bits);
    l->time = (int64_t)cdb_load_u64(v + 4, CDB_ORDER_BIG);
    l->flags = v[12];
    l->removed = cdb_load_u32(v + 13, CDB_ORDER_BIG);
}

/* Row helpers. row_get reads a committed link (live or tombstoned);
 * row_put updates the forward row only (the reverse row never changes);
 * row_insert adds both rows; row_erase deletes both. */
static cdb_status row_get(struct cdb_link_store *ls, uint64_t type, cdb_ref s, cdb_ref t, cdb_link *l, int *found)
{
    uint8_t k[KEY_LEN], v[VAL_LEN];
    size_t vlen;
    cdb_status st;
    k_fwd(k, type, s, t);
    if ((st = cdb_index_get(&ls->fwd, k, KEY_LEN, v, sizeof v, &vlen, found)) != CDB_OK || !*found)
        return st;
    if (vlen != VAL_LEN)
        return CDB_E_CORRUPT;
    l->type = type;
    l->source = s;
    l->target = t;
    decode_val(v, l);
    return CDB_OK;
}

static cdb_status row_put(struct cdb_link_store *ls, const cdb_link *l)
{
    uint8_t k[KEY_LEN], v[VAL_LEN];
    k_fwd(k, l->type, l->source, l->target);
    encode_val(v, l);
    return cdb_index_put(&ls->fwd, k, KEY_LEN, v, VAL_LEN);
}

static cdb_status row_insert(struct cdb_link_store *ls, const cdb_link *l)
{
    uint8_t k[KEY_LEN];
    cdb_status s;
    if ((s = row_put(ls, l)) != CDB_OK)
        return s;
    k_rev(k, l->type, l->source, l->target);
    return cdb_index_put(&ls->rev, k, KEY_LEN, k, 0);
}

static cdb_status row_erase(struct cdb_link_store *ls, const cdb_link *l)
{
    uint8_t k[KEY_LEN];
    int found;
    cdb_status s;
    k_fwd(k, l->type, l->source, l->target);
    if ((s = cdb_index_delete(&ls->fwd, k, KEY_LEN, &found)) != CDB_OK)
        return s;
    k_rev(k, l->type, l->source, l->target);
    return cdb_index_delete(&ls->rev, k, KEY_LEN, &found);
}

/* The next key at or after pos (KEY_LEN bytes) that starts with prefix;
 * *done when there is none. Seeks afresh each time, so the index may
 * change between calls. The cursor uses the catalog page. */
static cdb_status scan_at(cdb_db *db, cdb_index *ix, const uint8_t *prefix, size_t plen, uint8_t *pos, int *done)
{
    cdb_index_cursor c;
    uint8_t k[KEY_LEN], v[VAL_LEN];
    size_t klen, vlen;
    cdb_status s;
    if ((s = cdb_index_seek(&c, ix, db->cfg.catalog_page, pos, KEY_LEN)) != CDB_OK
        || (s = cdb_index_next(&c, k, sizeof k, &klen, v, sizeof v, &vlen, done)) != CDB_OK)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
    if (*done || klen != KEY_LEN || memcmp(k, prefix, plen) != 0) {
        *done = 1;
        return CDB_OK;
    }
    memcpy(pos, k, KEY_LEN);
    return CDB_OK;
}

/* The key right after k, as a big-endian number; 0 at the very end.
 * Scans use it to step past the row they just handled. */
static int key_succ(uint8_t *k)
{
    size_t i = KEY_LEN;
    while (i-- > 0)
        if (++k[i] != 0)
            return 1;
    return 0;
}

/* A row of either index as the link it names (the reverse one is read
 * from the forward index). */
static cdb_status row_at(struct cdb_link_store *ls, int reverse, const uint8_t *k, cdb_link *l, int *found)
{
    cdb_ref a = get_ref(k), b = get_ref(k + CDB_REF_LEN + 8u);
    uint64_t type = cdb_load_u64(k + CDB_REF_LEN, CDB_ORDER_BIG);
    return reverse ? row_get(ls, type, b, a, l, found) : row_get(ls, type, a, b, l, found);
}

/* -------------------------------------------------------- the open transaction
 *
 * Links have no staging cache: to see things as the open transaction does,
 * the code reads the transaction's own link records back from the journal
 * (from tx_start to the journal's end). */

/* A decoded LINK or UNLINK payload. */
typedef struct txrec {
    uint8_t op, mode;
    uint64_t type;
    cdb_ref source, target;
    float weight;
    int64_t time;
} txrec;

/* Decodes a payload (without its op byte). Returns 0 for anything that is
 * not an exactly sized LINK or UNLINK payload; callers then ignore it. */
static int parse(const uint8_t op, const uint8_t *p, size_t len, txrec *r)
{
    memset(r, 0, sizeof *r);
    r->op = op;
    if (op == CDB_OP_LINK && len == LINK_PAYLOAD && (p[0] == MODE_CREATE || p[0] == MODE_REINFORCE)) {
        uint32_t bits;
        r->mode = p[0];
        p++;
        r->type = cdb_load_u64(p, CDB_ORDER_BIG);
        r->source = get_ref(p + 8);
        r->target = get_ref(p + 8 + CDB_REF_LEN);
        bits = cdb_load_u32(p + 8 + 2 * CDB_REF_LEN, CDB_ORDER_BIG);
        memcpy(&r->weight, &bits, sizeof bits);
        r->time = (int64_t)cdb_load_u64(p + 8 + 2 * CDB_REF_LEN + 4, CDB_ORDER_BIG);
        return 1;
    }
    if (op == CDB_OP_UNLINK && len == UNLINK_PAYLOAD) {
        r->type = cdb_load_u64(p, CDB_ORDER_BIG);
        r->source = get_ref(p + 8);
        r->target = get_ref(p + 8 + CDB_REF_LEN);
        return 1;
    }
    return 0;
}

/* The open transaction's link record at or after *off; *done at the end. */
static cdb_status tx_next(cdb_db *db, uint64_t *off, txrec *r, int *done)
{
    *done = 0;
    while (*off < db->journal.end) {
        cdb_record rec;
        uint64_t next, at = *off;
        cdb_status s = cdb_journal_read(&db->journal, *off, db->cfg.record_buf, db->cfg.record_buf_size, &rec, &next);
        if (s != CDB_OK)
            return s;
        *off = next;
        if (rec.kind == CDB_RECORD_DATA && rec.tx_id == db->tx_id && !cdb_tx_dropped(db, at)
            && rec.store_id == db->links.store_id
            && rec.payload_len >= 1 && parse(rec.payload[0], rec.payload + 1, rec.payload_len - 1u, r))
            return CDB_OK;
    }
    *done = 1;
    return CDB_OK;
}

static int same_ref(cdb_ref a, cdb_ref b)
{
    return a.store == b.store && a.entry == b.entry && a.flags == b.flags && a.ts == b.ts;
}

static int same_pair(const txrec *r, uint64_t type, cdb_ref s, cdb_ref t)
{
    return r->type == type && same_ref(r->source, s) && same_ref(r->target, t);
}

/* NONE: never linked. REMOVED: removed on purpose, may be revived. TOMB:
 * tombstoned because an end went, for good. */
enum { NONE = 0, LIVE = 1, TOMB = 2, REMOVED = 3 };

/* A stored row's state: tombstoned rows are TOMB if an end-gone flag is
 * set, else REMOVED. */
static int state_of(const cdb_link *l)
{
    if (!(l->flags & CDB_LINK_TOMBSTONE))
        return LIVE;
    return (l->flags & (CDB_LINK_SOURCE_GONE | CDB_LINK_TARGET_GONE)) ? TOMB : REMOVED;
}

static void stage_link_key(uint8_t *k, uint64_t type, cdb_ref s, cdb_ref t)
{
    cdb_store_u64(k, type, CDB_ORDER_BIG);
    put_ref(k + 8, s);
    put_ref(k + 8 + CDB_REF_LEN, t);
}

/* A link as the open transaction sees it: the committed row, then the
 * transaction's own records for the pair, in order. The state changes
 * mirror cdb_link_apply: a create makes a never-linked or removed pair live,
 * an unlink makes a live pair removed; reinforce changes no state. */
static cdb_status tx_state(cdb_db *db, uint64_t type, cdb_ref s, cdb_ref t, int *state)
{
    cdb_link l;
    uint64_t off = db->tx_start, st_entry;
    txrec r;
    int found, done, st_live, st_found;
    cdb_status st;
    uint8_t lk[STAGE_LINK_LEN];

    stage_link_key(lk, type, s, t);
    if ((st = cdb_stage_idx_get(db, db->links.store_id, 0, lk, sizeof lk, &st_entry, &st_live, &st_found)) != CDB_OK)
        return st;
    if (st_found) {
        *state = st_live ? LIVE : REMOVED;
        return CDB_OK;
    }

    if ((st = row_get(&db->links, type, s, t, &l, &found)) != CDB_OK)
        return st;
    *state = found ? state_of(&l) : NONE;
    if (!db->stage_idx_overflow)
        return CDB_OK;

    for (;;) {
        if ((st = tx_next(db, &off, &r, &done)) != CDB_OK || done)
            return st;
        if (!same_pair(&r, type, s, t))
            continue;
        if (r.op == CDB_OP_LINK && r.mode == MODE_CREATE && (*state == NONE || *state == REMOVED))
            *state = LIVE;
        else if (r.op == CDB_OP_UNLINK && *state == LIVE)
            *state = REMOVED;
    }
}

/* Whether an entry has a live link of the type in one direction, as the
 * open transaction sees it; with refuse_only, of any refuse type (type 0).
 * Two passes: committed rows with e at the chosen end (each checked with
 * tx_state, since this transaction may have removed it), then links this
 * transaction created (which are not in the indexes yet). Used for the
 * cardinality checks and for refuse-on-delete. */
static cdb_status any_live(cdb_db *db, int direction, cdb_ref e, uint64_t type, int refuse_only, int *any)
{
    struct cdb_link_store *ls = &db->links;
    cdb_index *ix = direction == CDB_LINK_IN ? &ls->rev : &ls->fwd;
    cdb_link_type_info ti;
    uint8_t prefix[CDB_REF_LEN + 8u], pos[KEY_LEN];
    size_t plen = type ? CDB_REF_LEN + 8u : CDB_REF_LEN;
    uint64_t off = db->tx_start;
    txrec r;
    int done, found, state;
    cdb_status s;

    *any = 0;
    memset(&ti, 0, sizeof ti);
    /* Prefix: the entry, plus the type when one is given. The scan starts
     * at the smallest key with that prefix. */
    put_ref(prefix, e);
    cdb_store_u64(prefix + CDB_REF_LEN, type, CDB_ORDER_BIG);
    memset(pos, 0, sizeof pos);
    memcpy(pos, prefix, plen);
    /* Pass 1: committed links. */
    for (;;) {
        cdb_link l;
        if ((s = scan_at(db, ix, prefix, plen, pos, &done)) != CDB_OK || done)
            break;
        if ((s = row_at(ls, direction == CDB_LINK_IN, pos, &l, &found)) != CDB_OK)
            return s;
        if (found && !(l.flags & CDB_LINK_TOMBSTONE)) {
            int counts = 1;
            if (refuse_only) {
                if ((s = type_of(db, l.type, &ti)) != CDB_OK)
                    return s;
                counts = ti.on_delete == CDB_LINK_ON_DELETE_REFUSE;
            }
            if (counts && (s = tx_state(db, l.type, l.source, l.target, &state)) != CDB_OK)
                return s;
            if (counts && state == LIVE) {
                *any = 1;
                return CDB_OK;
            }
        }
        if (!key_succ(pos))
            break;
    }
    if (s != CDB_OK)
        return s;
    /* Pass 2: links this transaction made. */
    if (!db->stage_idx_overflow) {
        size_t off_st = 0;
        while (off_st < db->stage_idx_used) {
            const uint8_t *rec = db->idx_stage + off_st;
            uint64_t st_store = cdb_load_u64(rec, CDB_ORDER_BIG);
            uint16_t r_klen = cdb_load_u16(rec + 20, CDB_ORDER_BIG);
            size_t rec_len = 24u + (size_t)r_klen;
            if (st_store == ls->store_id && r_klen == STAGE_LINK_LEN) {
                const uint8_t *k = rec + 24u;
                uint64_t r_type = cdb_load_u64(k, CDB_ORDER_BIG);
                cdb_ref r_source = get_ref(k + 8);
                cdb_ref r_target = get_ref(k + 8 + CDB_REF_LEN);
                cdb_ref end = direction == CDB_LINK_IN ? r_target : r_source;
                if (same_ref(end, e) && (!type || r_type == type)) {
                    int counts = 1;
                    if (refuse_only) {
                        if ((s = type_of(db, r_type, &ti)) != CDB_OK)
                            return s;
                        counts = (ti.on_delete == CDB_LINK_ON_DELETE_REFUSE);
                    }
                    if (counts && (s = tx_state(db, r_type, r_source, r_target, &state)) != CDB_OK)
                        return s;
                    if (counts && state == LIVE) {
                        *any = 1;
                        return CDB_OK;
                    }
                }
            }
            off_st += rec_len;
        }
        return CDB_OK;
    }

    for (;;) {
        cdb_ref end;
        if ((s = tx_next(db, &off, &r, &done)) != CDB_OK || done)
            return s;
        end = direction == CDB_LINK_IN ? r.target : r.source;
        if (r.op != CDB_OP_LINK || r.mode != MODE_CREATE || !same_ref(end, e)
            || (type && r.type != type))
            continue;
        if (refuse_only) {
            if ((s = type_of(db, r.type, &ti)) != CDB_OK)
                return s;
            if (ti.on_delete != CDB_LINK_ON_DELETE_REFUSE)
                continue;
        }
        if ((s = tx_state(db, r.type, r.source, r.target, &state)) != CDB_OK)
            return s;
        if (state == LIVE) {
            *any = 1;
            return CDB_OK;
        }
    }
}

static cdb_status enclave_ref_live(cdb_db *db, cdb_ref r, int *live)
{
    size_t i;
    *live = 0;
    if (r.entry == 0)
        return CDB_OK;
    if (r.store != 0) {
        cdb_store_info sinfo;
        uint64_t target_enclave = r.store;
        if (cdb_catalog_get_store(&db->catalog, r.store, &sinfo) == CDB_OK && sinfo.domain_id != 0)
            target_enclave = sinfo.domain_id;
        for (i = 0; i < sizeof(db->enclaves) / sizeof(db->enclaves[0]); i++) {
            if (db->enclaves[i].in_use && db->enclaves[i].enclave_id == target_enclave) {
                *live = 1;
                return CDB_OK;
            }
        }
        return CDB_OK;
    }
    for (i = 0; i < sizeof(db->enclaves) / sizeof(db->enclaves[0]); i++) {
        if (db->enclaves[i].in_use) {
            *live = 1;
            return CDB_OK;
        }
    }
    return CDB_OK;
}

/* Whether an entry exists as the open transaction sees it (spec 6.1, 6.11).
 * Checks documents, tables, vectors, time series, and cross-boundary enclave pointers. */
static cdb_status ref_live(cdb_db *db, cdb_ref r, int *live)
{
    cdb_store_info info;
    cdb_status s;
    *live = 0;
    if (r.flags & CDB_REF_OPAQUE)
        return enclave_ref_live(db, r, live);

    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, r.store, &info)) != CDB_OK) {
        if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG)
            return CDB_OK;
        return s;
    }
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_OK;

    switch (info.model) {
    case CDB_MODEL_DOCUMENT:
        return cdb_doc_live_in_tx(db, r.store, r.entry, live);
    case CDB_MODEL_RELATIONAL:
        return cdb_table_live_in_tx(db, r.store, r.entry, live);
    case CDB_MODEL_VECTOR:
        return cdb_vec_live_in_tx(db, r.store, r.entry, live);
    case CDB_MODEL_TIME_SERIES:
        if ((s = cdb_ts_live_in_tx(db, r.store, r.entry, live)) != CDB_OK || !*live)
            return s;
        if (r.flags & CDB_REF_POINT)
            return cdb_ts_point_live_in_tx(db, r.store, r.entry, r.ts, live);
        return CDB_OK;
    default:
        *live = 0;
        return CDB_OK;
    }
}

/* -------------------------------------------------------- types */

/* A catalog change (reconcile, change, rotate the journal). The first type
 * also creates the link store and its indexes. */
static cdb_status link_define_type_in(cdb_db *db, const char *name, size_t len, unsigned cardinality, int enforced,
                                      unsigned on_delete, uint64_t *type);

cdb_status cdb_link_define_type(cdb_db *db, const char *name, size_t len, unsigned cardinality, int enforced,
                                unsigned on_delete, uint64_t *type)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_define_type_in(db, name, len, cardinality, enforced, on_delete, type);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_define_type_in(cdb_db *db, const char *name, size_t len, unsigned cardinality, int enforced,
                                      unsigned on_delete, uint64_t *type)
{
    cdb_link_type_info info;
    cdb_status s;
    if (cardinality < CDB_CARD_1_1 || cardinality > CDB_CARD_N_M || on_delete > CDB_LINK_ON_DELETE_REFUSE
        || (enforced != 0 && enforced != 1) || name == NULL || len == 0 || len > CDB_CATALOG_NAME_MAX)
        return CDB_E_INVALID_ARG;
    memset(&info, 0, sizeof info);
    memcpy(info.name, name, len);
    info.name_len = len;
    info.cardinality = (uint8_t)cardinality;
    info.enforced = (uint8_t)enforced;
    info.on_delete = (uint8_t)on_delete;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    s = open_store(db, 1);
    if (s == CDB_OK)
        s = cdb_catalog_create_link_type(&db->catalog, &info, type);
    return cdb_db_catalog_end(db, s);
}

static cdb_status link_find_type_in(cdb_db *db, const char *name, size_t len, uint64_t *type, int *found);

cdb_status cdb_link_find_type(cdb_db *db, const char *name, size_t len, uint64_t *type, int *found)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_find_type_in(db, name, len, type, found);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_find_type_in(cdb_db *db, const char *name, size_t len, uint64_t *type, int *found)
{
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    return cdb_catalog_find_link_type(&db->catalog, name, len, type, found);
}

/* -------------------------------------------------------- changes */

/* Common checks for every link change: an open, healthy transaction, a
 * link store, and a known link type (loaded into *ti). */
static cdb_status tx_ready(cdb_db *db, uint64_t type, cdb_link_type_info *ti)
{
    cdb_status s;
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = open_store(db, 0)) != CDB_OK)
        return s; /* no link store: no link types either */
    memset(ti, 0, sizeof *ti);
    return type_of(db, type, ti);
}

/* Journals a LINK record. The time is fixed here, when the change is made
 * (the environment's clock, else the caller's value), so replay writes the
 * same time again. */
static cdb_status put_link(cdb_db *db, uint8_t mode, uint64_t type, cdb_ref source, cdb_ref target, float weight,
                           int64_t time)
{
    uint8_t p[LINK_PAYLOAD];
    uint32_t bits;
    int64_t now;
    int have;
    cdb_status s;
    if ((s = cdb_db_now(db, &now, &have)) != CDB_OK)
        return s;
    p[0] = mode;
    cdb_store_u64(p + 1, type, CDB_ORDER_BIG);
    put_ref(p + 9, source);
    put_ref(p + 9 + CDB_REF_LEN, target);
    memcpy(&bits, &weight, sizeof bits);
    cdb_store_u32(p + 9 + 2 * CDB_REF_LEN, bits, CDB_ORDER_BIG);
    cdb_store_u64(p + 9 + 2 * CDB_REF_LEN + 4, (uint64_t)(have ? now : time), CDB_ORDER_BIG);
    return cdb_tx_put(db, db->links.store_id, 0, CDB_OP_LINK, p, sizeof p);
}

static cdb_status link_add_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight, int64_t time);

cdb_status cdb_link_add(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight, int64_t time)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_add_in(db, type, source, target, weight, time);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_add_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight, int64_t time)
{
    cdb_link_type_info ti;
    int live, state, any;
    cdb_status s;
    if ((s = tx_ready(db, type, &ti)) != CDB_OK)
        return s;
    if (weight != weight)
        return CDB_E_INVALID_ARG; /* NaN */
    if ((s = ref_live(db, source, &live)) != CDB_OK)
        return s;
    if (!live)
        return CDB_E_NOT_FOUND;
    if ((s = ref_live(db, target, &live)) != CDB_OK)
        return s;
    if (!live)
        return CDB_E_NOT_FOUND;
    if ((s = tx_state(db, type, source, target, &state)) != CDB_OK)
        return s;
    if (state == LIVE || state == TOMB)
        return CDB_E_EXISTS; /* linked, or tombstoned with an end gone; a removed link is revived */
    /* Enforced cardinality: 1:N and 1:1 allow the target only one live
     * source; 1:1 also allows the source only one live target. */
    if (ti.enforced && ti.cardinality != CDB_CARD_N_M) {
        if ((s = any_live(db, CDB_LINK_IN, target, type, 0, &any)) != CDB_OK)
            return s;
        if (any)
            return CDB_E_STATE; /* the target has its one source */
        if (ti.cardinality == CDB_CARD_1_1) {
            if ((s = any_live(db, CDB_LINK_OUT, source, type, 0, &any)) != CDB_OK)
                return s;
            if (any)
                return CDB_E_STATE; /* the source has its one target */
        }
    }
    if ((s = put_link(db, MODE_CREATE, type, source, target, weight, time)) != CDB_OK)
        return s;
    {
        uint8_t lk[STAGE_LINK_LEN];
        stage_link_key(lk, type, source, target);
        return cdb_stage_idx_put(db, db->links.store_id, 0, lk, sizeof lk, type, 1);
    }
}

static cdb_status link_reinforce_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight,
                                    int64_t time);

cdb_status cdb_link_reinforce(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight,
                              int64_t time)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_reinforce_in(db, type, source, target, weight, time);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_reinforce_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, float weight,
                                    int64_t time)
{
    cdb_link_type_info ti;
    int state;
    cdb_status s;
    if ((s = tx_ready(db, type, &ti)) != CDB_OK)
        return s;
    if (weight != weight)
        return CDB_E_INVALID_ARG;
    if ((s = tx_state(db, type, source, target, &state)) != CDB_OK)
        return s;
    if (state != LIVE)
        return CDB_E_NOT_FOUND;
    if ((s = put_link(db, MODE_REINFORCE, type, source, target, weight, time)) != CDB_OK)
        return s;
    {
        uint8_t lk[STAGE_LINK_LEN];
        stage_link_key(lk, type, source, target);
        return cdb_stage_idx_put(db, db->links.store_id, 0, lk, sizeof lk, type, 1);
    }
}

static cdb_status link_remove_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target);

cdb_status cdb_link_remove(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_remove_in(db, type, source, target);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_remove_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target)
{
    cdb_link_type_info ti;
    uint8_t p[UNLINK_PAYLOAD];
    int state;
    cdb_status s;
    if ((s = tx_ready(db, type, &ti)) != CDB_OK)
        return s;
    if ((s = tx_state(db, type, source, target, &state)) != CDB_OK)
        return s;
    if (state != LIVE)
        return CDB_E_NOT_FOUND;
    cdb_store_u64(p, type, CDB_ORDER_BIG);
    put_ref(p + 8, source);
    put_ref(p + 8 + CDB_REF_LEN, target);
    if ((s = cdb_tx_put(db, db->links.store_id, 0, CDB_OP_UNLINK, p, sizeof p)) != CDB_OK)
        return s;
    {
        uint8_t lk[STAGE_LINK_LEN];
        stage_link_key(lk, type, source, target);
        return cdb_stage_idx_put(db, db->links.store_id, 0, lk, sizeof lk, type, 0);
    }
}

/* Only incoming links are checked: a refuse type protects the target of a
 * link from being deleted while something points at it. */
cdb_status cdb_link_check_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    cdb_ref e;
    int any;
    cdb_status s = open_store(db, 0);
    if (s == CDB_E_NOT_FOUND || s == CDB_E_UNSUPPORTED)
        return CDB_OK; /* no links at all */
    if (s != CDB_OK)
        return s;
    e = cdb_ref_entry(store, entry);
    if ((s = any_live(db, CDB_LINK_IN, e, 0, 1, &any)) != CDB_OK)
        return s;
    return any ? CDB_E_STATE : CDB_OK;
}

/* -------------------------------------------------------- apply */

/* Never fails for logical reasons: it runs again during recovery.
 * Any record that no longer makes sense (an unreadable payload, a create
 * for a pair that is live or gone for good, a reinforce or unlink of a link
 * that is not live) is skipped. */
cdb_status cdb_link_apply(cdb_db *db, const cdb_delta *d)
{
    struct cdb_link_store *ls = &db->links;
    cdb_link l;
    txrec r;
    int found;
    cdb_status s = open_store(db, 0);
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    if (!parse(d->op, d->data, d->len, &r))
        return CDB_OK;
    if ((s = row_get(ls, r.type, r.source, r.target, &l, &found)) != CDB_OK)
        return s;
    if (r.op == CDB_OP_LINK && r.mode == MODE_CREATE) {
        if (found) {
            if (state_of(&l) != REMOVED)
                return CDB_OK;
            l.weight = r.weight; /* revived: live again, the removal counted */
            l.time = r.time;
            l.flags = 0;
            l.removed++;
            return row_put(ls, &l);
        }
        l.type = r.type;
        l.source = r.source;
        l.target = r.target;
        l.weight = r.weight;
        l.time = r.time;
        l.flags = 0;
        l.removed = 0;
        return row_insert(ls, &l);
    }
    if (!found || (l.flags & CDB_LINK_TOMBSTONE))
        return CDB_OK;
    if (r.op == CDB_OP_LINK) { /* reinforce */
        l.weight = r.weight;
        l.time = r.time;
    } else {
        l.flags |= CDB_LINK_TOMBSTONE;
    }
    return row_put(ls, &l);
}

/* Applies "this end is gone" to every link with an end matching prefix:
 * tombstone (with the gone flag), or erase for a cascade type unless
 * at_drop. Both indexes are walked: one for each end. */
static cdb_status ends_gone(cdb_db *db, const uint8_t *prefix, size_t plen, int at_drop)
{
    struct cdb_link_store *ls = &db->links;
    cdb_link_type_info ti;
    int reverse;
    cdb_status s = open_store(db, 0);
    if (s == CDB_E_NOT_FOUND || s == CDB_E_UNSUPPORTED)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    memset(&ti, 0, sizeof ti);
    /* Pass 0 walks the forward index (the gone entry is the source);
     * pass 1 the reverse index (the gone entry is the target). */
    for (reverse = 0; reverse < 2; reverse++) {
        uint8_t pos[KEY_LEN];
        memset(pos, 0, sizeof pos);
        memcpy(pos, prefix, plen);
        for (;;) {
            cdb_link l;
            int done, found;
            if ((s = scan_at(db, reverse ? &ls->rev : &ls->fwd, prefix, plen, pos, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            if ((s = row_at(ls, reverse, pos, &l, &found)) != CDB_OK)
                return s;
            if (!found)
                return CDB_E_CORRUPT; /* a reverse row without its link */
            if (!at_drop && (s = type_of(db, l.type, &ti)) != CDB_OK && s != CDB_E_NOT_FOUND)
                return s;
            if (!at_drop && s == CDB_OK && ti.on_delete == CDB_LINK_ON_DELETE_CASCADE) {
                if ((s = row_erase(ls, &l)) != CDB_OK)
                    return s;
                continue; /* the row is gone: pos now names the next one */
            }
            l.flags |= (uint8_t)(CDB_LINK_TOMBSTONE | (reverse ? CDB_LINK_TARGET_GONE : CDB_LINK_SOURCE_GONE));
            if ((s = row_put(ls, &l)) != CDB_OK)
                return s;
            if (!key_succ(pos))
                break;
        }
    }
    return CDB_OK;
}

/* One entry is gone: prefix = its 16-byte reference (store ID and entry ID).
 * This matches standard entry links as well as point links under that series. */
cdb_status cdb_link_on_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    uint8_t prefix[16];
    cdb_store_u64(prefix, store, CDB_ORDER_BIG);
    cdb_store_u64(prefix + 8, entry, CDB_ORDER_BIG);
    return ends_gone(db, prefix, sizeof prefix, 0);
}

/* A whole store is gone: prefix = its 8-byte ID, matching every entry.
 * Links are always tombstoned here, never cascaded. */
cdb_status cdb_link_on_drop(cdb_db *db, uint64_t store)
{
    uint8_t prefix[8];
    cdb_store_u64(prefix, store, CDB_ORDER_BIG);
    return ends_gone(db, prefix, sizeof prefix, 1);
}

/* -------------------------------------------------------- reading */

static cdb_status link_get_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, cdb_link *out, int *found);

cdb_status cdb_link_get(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, cdb_link *out, int *found)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_get_in(db, type, source, target, out, found);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_get_in(cdb_db *db, uint64_t type, cdb_ref source, cdb_ref target, cdb_link *out, int *found)
{
    cdb_status s = open_store(db, 0);
    *found = 0;
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    return row_get(&db->links, type, source, target, out, found);
}

static cdb_status link_list_in(cdb_db *db, cdb_ref entry, int direction, uint64_t type, const cdb_link *after,
                               cdb_link *out, size_t max, size_t *n);

cdb_status cdb_link_list(cdb_db *db, cdb_ref entry, int direction, uint64_t type, const cdb_link *after,
                         cdb_link *out, size_t max, size_t *n)
{
    cdb_status r;
    cdb_db_lock(db);
    r = link_list_in(db, entry, direction, type, after, out, max, n);
    cdb_db_unlock(db);
    return r;
}

static cdb_status link_list_in(cdb_db *db, cdb_ref entry, int direction, uint64_t type, const cdb_link *after,
                               cdb_link *out, size_t max, size_t *n)
{
    struct cdb_link_store *ls = &db->links;
    int reverse = direction == CDB_LINK_IN;
    uint8_t prefix[CDB_REF_LEN + 8u], pos[KEY_LEN];
    size_t plen = type ? CDB_REF_LEN + 8u : CDB_REF_LEN;
    cdb_status s = open_store(db, 0);
    *n = 0;
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    if (direction != CDB_LINK_OUT && direction != CDB_LINK_IN)
        return CDB_E_INVALID_ARG;
    put_ref(prefix, entry);
    cdb_store_u64(prefix + CDB_REF_LEN, type, CDB_ORDER_BIG);
    memset(pos, 0, sizeof pos);
    memcpy(pos, prefix, plen);
    /* Paging: resume just after the key of the last link returned. */
    if (after) {
        if (reverse)
            k_rev(pos, after->type, after->source, after->target);
        else
            k_fwd(pos, after->type, after->source, after->target);
        if (!key_succ(pos))
            return CDB_OK;
    }
    while (*n < max) {
        int done, found;
        if ((s = scan_at(db, reverse ? &ls->rev : &ls->fwd, prefix, plen, pos, &done)) != CDB_OK || done)
            return s;
        if ((s = row_at(ls, reverse, pos, &out[*n], &found)) != CDB_OK)
            return s;
        if (!found)
            return CDB_E_CORRUPT;
        (*n)++;
        if (!key_succ(pos))
            break;
    }
    return CDB_OK;
}
