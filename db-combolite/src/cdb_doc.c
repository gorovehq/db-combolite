/* Documents: revision chains, patches and the document apply (spec 8, 14.9).
 *
 * Two halves, which must agree:
 * - The public calls (cdb_doc_insert, _replace, _patch, _delete) run inside
 *   an open transaction. They check everything that could go wrong (store,
 *   document exists, schema, size limits) against the document as the
 *   transaction sees it, and only then journal a change record. They do not
 *   touch document pages.
 * - cdb_doc_apply runs when the transaction commits, and again during
 *   recovery. It turns each change record into a new revision page and
 *   updates the head and revisions indexes, plus any full-text index and
 *   vector-source bookkeeping. It must never fail for logical reasons
 *   (spec 14.9.3): anything odd (a missing document, a dropped store) makes
 *   it do nothing, because a failure would recur on every reopen.
 *
 * A document's current state is rebuilt by starting from the nearest full
 * revision (original or checkpoint) and applying the patches stored in the
 * delta revisions after it, in order. */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_fts.h"
#include "cdb_key.h"
#include "cdb_link.h"
#include "cdb_nib.h"
#include "cdb_overflow.h"
#include "cdb_schema.h"
#include "cdb_table.h"
#include "cdb_ts.h"
#include "cdb_vec.h"
#include "cdb_varint.h"

/* Record envelope (spec 14.9.1): entry, revision, parent, commit, principal
 * (8 bytes each), schema version (4), kind (1), flags (1), reserved (2);
 * then the summary (a string value) if flagged, then the body. */
#define ENV_SIZE 48u
#define ENV_FLAG_SUMMARY 0x01u
#define ENV_FLAG_OVERFLOW 0x02u /* body length (8) and chain (address width) follow the summary */

/* Head index value: revision, page, chain length, kind. */
#define HEAD_SIZE 21u

#define MAX_CHAIN 64u    /* longest delta chain read back; a longer one is treated as corrupt */
#define MAX_PATH 16u     /* deepest field path a patch operation may use */
#define DEFAULT_CHECKPOINT_EVERY 16u

/* Insert/replace payload flag: a summary follows. */
#define PAYLOAD_SUMMARY 0x01u

/* -------------------------------------------------------- buffers */

/* Payload bytes of one page. */
static size_t capacity(const cdb_db *db)
{
    return cdb_page_capacity(db->pager.fmt, db->pager.page_size);
}

/* One document buffer: the largest body plus a page of slack (or one page
 * when bodies always fit a page). */
size_t cdb_doc_buffer_size(size_t page_size, size_t doc_max)
{
    return doc_max ? doc_max + page_size : page_size;
}

/* Three document buffers, three page buffers, and the two pages nibblets
 * work in (cdb_nib.h), which come last. */
size_t cdb_doc_buffers_size(size_t page_size, size_t doc_max)
{
    return 3u * cdb_doc_buffer_size(page_size, doc_max) + 5u * page_size;
}

/* The longest body a document may have. */
static size_t body_max(const cdb_db *db)
{
    return db->cfg.doc_max ? db->cfg.doc_max : capacity(db);
}

static size_t dbuf_size(const cdb_db *db)
{
    return cdb_doc_buffer_size(db->pager.page_size, db->cfg.doc_max);
}

/* Document buffers first (a body, or a change payload in B_WORK), then the
 * page-sized ones. B_DOC0 and B_DOC1 take turns holding a document while
 * patches are applied (one is read, the other written). B_PAGE receives
 * revision pages and overflow pages; B_SUMMARY keeps a summary safe while
 * B_PAGE is reused; B_SCHEMA caches the pool's current schema. */
enum { B_DOC0 = 0, B_DOC1 = 1, B_WORK = 2, B_PAGE = 3, B_SUMMARY = 4, B_SCHEMA = 5 };

/* Start of buffer i inside cfg.doc_buffers. */
static uint8_t *buf(cdb_db *db, unsigned i)
{
    if (i <= B_WORK)
        return db->cfg.doc_buffers + (size_t)i * dbuf_size(db);
    return db->cfg.doc_buffers + 3u * dbuf_size(db) + (size_t)(i - B_PAGE) * db->pager.page_size;
}

/* Logical page IDs inside revision pages use the file's address width and
 * byte order. (Index values, by contrast, store them as 8 big-endian bytes.) */
static uint64_t load_lid(const cdb_db *db, const uint8_t *b)
{
    return db->pager.addr_width == 8 ? cdb_load_u64(b, db->pager.order) : cdb_load_u32(b, db->pager.order);
}

static void store_lid(const cdb_db *db, uint8_t *b, uint64_t lid)
{
    if (db->pager.addr_width == 8)
        cdb_store_u64(b, lid, db->pager.order);
    else
        cdb_store_u32(b, (uint32_t)lid, db->pager.order);
}

static unsigned checkpoint_every(const cdb_db *db)
{
    return db->cfg.checkpoint_every ? db->cfg.checkpoint_every : DEFAULT_CHECKPOINT_EVERY;
}

/* -------------------------------------------------------- pools */

/* Records a pool's roots and entry counter, staged for reconciliation.
 * Used both by the flush before every archive commit and by the slot code
 * before it evicts the pool (cdb_slots.h). */
static cdb_status save_pool(cdb_db *db, void *slot)
{
    struct cdb_doc_pool *p = (struct cdb_doc_pool *)slot;
    cdb_status s;
    if (!p->dirty)
        return CDB_OK;
    if ((s = cdb_catalog_stage_pool(&db->catalog, p->slot.store_id, p->head.root, p->revs.root, p->next_entry))
        != CDB_OK)
        return s;
    p->dirty = 0;
    return CDB_OK;
}

/* Document pools pin nothing: any idle pool may be evicted once saved. */
const cdb_slot_ops cdb_doc_slot_ops = { NULL, save_pool };

/* Finds the in-memory state of document pool `store`, loading it from the
 * catalog into a slot (cdb_slots.h) when it is not open. Loading opens the
 * head and revisions indexes (if the pool has any documents yet) and the
 * full-text indexes (if it has full-text search). Every call records a use
 * of the slot, which keeps it from eviction for the open transaction or
 * operation scope.
 * Errors: CDB_E_UNSUPPORTED without a catalog or doc_buffers, or for a
 * full-text pool when the full-text buffers are not configured;
 * CDB_E_INVALID_ARG if the store is not a document pool; CDB_E_NOT_FOUND if
 * it is unknown or dropped; CDB_E_BUFFER_TOO_SMALL if no slot can be had
 * (at max_open, or out of memory, with nothing evictable). */
static cdb_status pool_get(cdb_db *db, uint64_t store, struct cdb_doc_pool **out)
{
    struct cdb_doc_pool *free_slot = (struct cdb_doc_pool *)cdb_slots_find(&db->pool_slots, store);
    cdb_store_info info;
    cdb_status s;
    void *v;

    if (free_slot != NULL) {
        cdb_slots_touch(db, &db->pool_slots, free_slot);
        *out = free_slot;
        return CDB_OK;
    }
    if (!db->has_catalog || db->cfg.doc_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    if (info.model != CDB_MODEL_DOCUMENT && info.model != CDB_MODEL_RELATIONAL)
        return CDB_E_INVALID_ARG;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_slots_claim(db, &db->pool_slots, &v)) != CDB_OK)
        return s;
    free_slot = (struct cdb_doc_pool *)v;
    free_slot->slot.store_id = store;
    free_slot->next_entry = info.next_entry_id ? info.next_entry_id : 1;
    free_slot->schema_version = info.schema_version;
    /* A table is a pool too (spec 9.7); its settings say how many
     * revisions each row keeps. */
    if (info.model == CDB_MODEL_RELATIONAL) {
        free_slot->is_table = 1;
        if ((s = cdb_table_keep_setting(info.vector, info.vector_len, db->pager.order, &free_slot->keep)) != CDB_OK)
            return s;
    }
    if (info.head_root && info.rev_root) {
        if ((s = cdb_index_open(&free_slot->head, &db->pager, info.head_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        if ((s = cdb_index_open(&free_slot->revs, &db->pager, info.rev_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        free_slot->have_indexes = 1;
    }
    if (info.full_text_len) {
        uint64_t terms, postings, lengths;
        if (db->cfg.fts_buffers == NULL || db->cfg.fts_tokens == NULL
            || db->cfg.fts_tokens_cap < cdb_fts_tokens_needed(db->pager.page_size, db->cfg.doc_max))
            return CDB_E_UNSUPPORTED; /* the index would fall behind its documents */
        if ((s = cdb_fts_decode(info.full_text, info.full_text_len, db->pager.order, &free_slot->fts, &terms,
                                &postings, &lengths)) != CDB_OK)
            return s;
        if ((s = cdb_index_open(&free_slot->fts_terms, &db->pager, terms, &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_open(&free_slot->fts_postings, &db->pager, postings, &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_open(&free_slot->fts_lengths, &db->pager, lengths, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        free_slot->fts_on = 1;
    }
    free_slot->slot.in_use = 1;
    cdb_slots_touch(db, &db->pool_slots, free_slot);
    *out = free_slot;
    return CDB_OK;
}

/* pool_get for the public document calls. A table is read and written only
 * through the table layer, which enforces its columns and constraints, so
 * here it is not a document pool unless that layer is the caller. */
static cdb_status doc_pool(cdb_db *db, uint64_t store, struct cdb_doc_pool **out)
{
    cdb_status s = pool_get(db, store, out);
    if (s == CDB_OK && (*out)->is_table && !db->via_table)
        return CDB_E_INVALID_ARG;
    return s;
}

/* Creates the pool's head and revisions indexes on its first write. Marks
 * the pool dirty so the next reconciliation records the new roots in the
 * store record. */
static cdb_status pool_indexes(cdb_db *db, struct cdb_doc_pool *p)
{
    cdb_status s;
    if (p->have_indexes)
        return CDB_OK;
    if ((s = cdb_index_create(&p->head, &db->pager, p->slot.store_id, CDB_INDEX_FLAG_UNIQUE,
                              &db->cfg.catalog_work)) != CDB_OK)
        return s;
    if ((s = cdb_index_create(&p->revs, &db->pager, p->slot.store_id, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                              &db->cfg.catalog_work)) != CDB_OK)
        return s;
    p->have_indexes = 1;
    p->dirty = 1;
    return CDB_OK;
}

/* Called by cdb_db before every archive commit: stages each changed pool's
 * index roots and entry counter into its catalog record (no commit here),
 * then does the same for vector collections and time-series stores. */
cdb_status cdb_doc_flush_pools(cdb_db *db)
{
    cdb_slot_iter it;
    void *p;
    /* Nibblet pages under half full are merged first (spec 16.4); moving a
     * record updates its owner's indexes, which this flush then saves. */
    cdb_status ms = cdb_nib_merge(db, CDB_NIB_MERGE_BUDGET);
    if (ms != CDB_OK)
        return ms;
    for (p = cdb_slots_first(&db->pool_slots, &it); p != NULL; p = cdb_slots_next(&it)) {
        cdb_status s = save_pool(db, p);
        if (s != CDB_OK)
            return s;
    }
    {
        cdb_status s = cdb_vec_flush(db);
        if (s != CDB_OK)
            return s;
        if ((s = cdb_ts_flush(db)) != CDB_OK)
            return s;
        return cdb_table_flush(db);
    }
}

/* -------------------------------------------------------- index records */

/* Head index value (spec 14.9.2), 21 bytes, all big-endian: the head
 * revision ID (8), its page (8), chain length (4), revision kind (1). */
typedef struct head_rec {
    uint64_t rev;
    uint64_t lid;
    uint32_t chain; /* deltas since the last full revision */
    uint8_t kind;
} head_rec;

/* Index keys: entry ID (head index), or entry ID + revision ID (revisions
 * index), in the order-preserving key encoding, so revisions of one entry
 * sort together in revision order. */
static size_t k_head(uint8_t *k, uint64_t entry)
{
    cdb_key_builder kb;
    cdb_key_init(&kb, k, 16);
    cdb_key_u64(&kb, entry);
    return kb.len;
}

static size_t k_rev(uint8_t *k, uint64_t entry, uint64_t rev)
{
    cdb_key_builder kb;
    cdb_key_init(&kb, k, 16);
    cdb_key_u64(&kb, entry);
    cdb_key_u64(&kb, rev);
    return kb.len;
}

/* Reads the head record. A pool with no indexes yet has no documents. The
 * head's revision is on a page of its own (21 bytes) or a nibblet (23
 * bytes: the slot follows; spec 14.18.2); h->lid holds either address. */
static cdb_status head_get(struct cdb_doc_pool *p, uint64_t entry, head_rec *h, int *found)
{
    uint8_t k[16], v[HEAD_SIZE + 2u], a[10];
    size_t vlen;
    cdb_status s;
    *found = 0;
    if (!p->have_indexes)
        return CDB_OK;
    if ((s = cdb_index_get(&p->head, k, k_head(k, entry), v, sizeof v, &vlen, found)) != CDB_OK)
        return s;
    if (!*found)
        return CDB_OK;
    if (vlen != HEAD_SIZE && vlen != HEAD_SIZE + 2u)
        return CDB_E_CORRUPT;
    h->rev = cdb_load_u64(v, CDB_ORDER_BIG);
    memcpy(a, v + 8, 8);
    memcpy(a + 8, v + HEAD_SIZE, vlen - HEAD_SIZE);
    if ((s = cdb_nib_addr_load(a, 8u + (vlen - HEAD_SIZE), &h->lid)) != CDB_OK)
        return s;
    h->chain = cdb_load_u32(v + 16, CDB_ORDER_BIG);
    h->kind = v[20];
    return CDB_OK;
}

static cdb_status head_put(struct cdb_doc_pool *p, uint64_t entry, const head_rec *h)
{
    uint8_t k[16], v[HEAD_SIZE + 2u], a[10];
    size_t alen = cdb_nib_addr_store(a, h->lid);
    cdb_store_u64(v, h->rev, CDB_ORDER_BIG);
    memcpy(v + 8, a, 8);
    cdb_store_u32(v + 16, h->chain, CDB_ORDER_BIG);
    v[20] = h->kind;
    memcpy(v + HEAD_SIZE, a + 8, alen - 8u);
    return cdb_index_put(&p->head, k, k_head(k, entry), v, HEAD_SIZE + (alen - 8u));
}

/* Address of revision rev of entry: a page, or a nibblet (spec 14.18.2).
 * Only called for revisions known to exist, so a miss is CDB_E_CORRUPT. */
static cdb_status rev_lid(struct cdb_doc_pool *p, uint64_t entry, uint64_t rev, uint64_t *lid)
{
    uint8_t k[16], v[10];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&p->revs, k, k_rev(k, entry, rev), v, sizeof v, &vlen, &found);
    if (s != CDB_OK)
        return s;
    if (!found)
        return CDB_E_CORRUPT; /* the head said this revision exists */
    return cdb_nib_addr_load(v, vlen, lid);
}

/* Records revision rev's address in the revisions index. */
static cdb_status rev_put(struct cdb_doc_pool *p, uint64_t entry, uint64_t rev, uint64_t addr)
{
    uint8_t k[16], v[10];
    return cdb_index_put(&p->revs, k, k_rev(k, entry, rev), v, cdb_nib_addr_store(v, addr));
}

/* -------------------------------------------------------- envelopes */

/* A revision's envelope, decoded (spec 14.9.1), with pointers to its
 * summary and body. summary is NULL when the revision has none. */
typedef struct envelope {
    uint64_t entry, rev, parent, commit, principal;
    uint32_t schema;
    uint8_t kind, flags;
    const uint8_t *summary;
    size_t summary_len;
    const uint8_t *body;
    size_t body_len;
    uint64_t chain_first;    /* overflow chain of the body, if any (read_revision); 0 = none */
    size_t chain_len;        /* bytes in that chain */
} envelope;

/* Reads revision page lid into the page buffer and parses its envelope. The
 * body and summary point into the page buffer, except for a body in an
 * overflow chain: with dest it is gathered there (and the summary moves to
 * the summary buffer, since the page buffer reads the chain); without dest,
 * e->body is NULL and only its length is known. */
static cdb_status read_revision(cdb_db *db, uint64_t lid, envelope *e, uint8_t *dest)
{
    cdb_page_header h;
    const uint8_t *pl, *p, *end;
    cdb_byte_order o = db->pager.order;
    cdb_status s;
    if (cdb_nib_is(lid)) {
        /* A nibblet: its data is the payload a page would have had. */
        size_t n;
        memset(&h, 0, sizeof h);
        if ((s = cdb_nib_get(db, lid, buf(db, B_PAGE) + cdb_page_header_size(db->pager.fmt), capacity(db), &n))
            != CDB_OK)
            return s;
        h.type = CDB_PAGE_TYPE_DOCUMENT;
        h.payload_len = (uint32_t)n;
    } else if ((s = cdb_pager_read(&db->pager, lid, buf(db, B_PAGE), &h)) != CDB_OK) {
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    }
    if (h.type != CDB_PAGE_TYPE_DOCUMENT || h.payload_len < ENV_SIZE)
        return CDB_E_CORRUPT;
    pl = buf(db, B_PAGE) + cdb_page_header_size(db->pager.fmt);
    end = pl + h.payload_len;
    memset(e, 0, sizeof *e);
    e->entry = cdb_load_u64(pl, o);
    e->rev = cdb_load_u64(pl + 8, o);
    e->parent = cdb_load_u64(pl + 16, o);
    e->commit = cdb_load_u64(pl + 24, o);
    e->principal = cdb_load_u64(pl + 32, o);
    e->schema = cdb_load_u32(pl + 40, o);
    e->kind = pl[44];
    e->flags = pl[45];
    p = pl + ENV_SIZE;
    if (e->kind > CDB_REV_TOMBSTONE)
        return CDB_E_CORRUPT;
    /* Optional summary: one string value. */
    if (e->flags & ENV_FLAG_SUMMARY) {
        cdb_value v;
        if (cdb_value_read(p, (size_t)(end - p), o, &v) != CDB_OK || v.type != CDB_TYPE_STRING)
            return CDB_E_CORRUPT;
        e->summary = v.payload;
        e->summary_len = v.payload_len;
        p += v.encoded_len;
    }
    /* Body in overflow: total length (8), first chain page (address
     * width), then the part of the body that fits this page. The total must
     * exceed the inline part (or there would be no chain) and stay within
     * the configured limit. */
    if (e->flags & ENV_FLAG_OVERFLOW) {
        size_t aw = db->pager.addr_width, inline_len;
        uint64_t total, first;
        if ((size_t)(end - p) < 8u + aw)
            return CDB_E_CORRUPT;
        total = cdb_load_u64(p, o);
        first = load_lid(db, p + 8);
        p += 8u + aw;
        inline_len = (size_t)(end - p);
        if (total <= inline_len || total > body_max(db))
            return CDB_E_CORRUPT; /* or doc_max was lowered */
        e->body_len = (size_t)total;
        e->chain_first = first;
        e->chain_len = (size_t)total - inline_len;
        if (dest == NULL)
            return CDB_OK;
        if (e->summary) {
            memmove(buf(db, B_SUMMARY), e->summary, e->summary_len);
            e->summary = buf(db, B_SUMMARY);
        }
        memcpy(dest, p, inline_len);
        e->body = dest;
        return cdb_overflow_read(&db->pager, first, h.store_id, dest + inline_len, e->body_len - inline_len,
                                 buf(db, B_PAGE));
    }
    /* Ordinary case: the body is the rest of the payload. */
    e->body = p;
    e->body_len = (size_t)(end - p);
    return CDB_OK;
}

/* Bytes the summary takes in a revision page: a string value (one tag byte,
 * a varint length, the bytes). */
static size_t summary_bytes(int has_summary, size_t summary_len)
{
    return has_summary ? 1u + cdb_varint_size(summary_len) + summary_len : 0u;
}

/* Whether a revision with this summary and body can be written: the
 * envelope and summary fit the revision page, and the body fits there too
 * or, with doc_max set, is at most doc_max bytes (an overflow chain holds
 * the rest, after its length and first page). */
static int revision_fits(const cdb_db *db, int has_summary, size_t summary_len, size_t body_len)
{
    size_t cap = capacity(db), need = ENV_SIZE;
    if (has_summary && summary_len > cap)
        return 0;
    need += summary_bytes(has_summary, summary_len);
    if (need > cap)
        return 0;
    if (body_len <= cap - need)
        return 1;
    return db->cfg.doc_max && body_len <= db->cfg.doc_max && need + 8u + db->pager.addr_width <= cap;
}

/* Builds a revision payload in the page buffer and writes it to a new page.
 * A body that does not fit goes first to an overflow chain, all but the
 * part the revision page holds; the chain uses the page buffer meanwhile.
 * (That is why the chain is written before the page is assembled.) A new
 * logical page is allocated for every revision: revisions are history and
 * are never rewritten. e->body must not point into B_PAGE. */
static cdb_status write_revision(cdb_db *db, struct cdb_doc_pool *pool, const envelope *e, uint64_t *lid)
{
    uint8_t *pl = buf(db, B_PAGE);
    size_t cap = capacity(db), len = ENV_SIZE, inline_len = e->body_len;
    size_t head = ENV_SIZE + summary_bytes(e->summary != NULL, e->summary_len);
    cdb_byte_order o = db->pager.order;
    uint64_t first = 0;
    cdb_page_header t;
    cdb_status s;

    if (head > cap)
        return CDB_E_OVERFLOW;
    if (e->body_len > cap - head) {
        if (!revision_fits(db, e->summary != NULL, e->summary_len, e->body_len))
            return CDB_E_OVERFLOW;
        inline_len = cap - head - 8u - db->pager.addr_width;
        if ((s = cdb_overflow_write(&db->pager, pool->slot.store_id, e->body + inline_len, e->body_len - inline_len, pl,
                                    &first)) != CDB_OK)
            return s;
    }
    memset(pl, 0, ENV_SIZE);
    cdb_store_u64(pl, e->entry, o);
    cdb_store_u64(pl + 8, e->rev, o);
    cdb_store_u64(pl + 16, e->parent, o);
    cdb_store_u64(pl + 24, e->commit, o);
    cdb_store_u64(pl + 32, e->principal, o);
    cdb_store_u32(pl + 40, e->schema, o);
    pl[44] = e->kind;
    pl[45] = (uint8_t)((e->summary ? ENV_FLAG_SUMMARY : 0u) | (first ? ENV_FLAG_OVERFLOW : 0u));
    if (e->summary) {
        cdb_value_writer w;
        cdb_value_writer_init(&w, pl + len, cap - len, o, CDB_DECIMAL_COMPACT);
        if ((s = cdb_value_write_string(&w, (const char *)e->summary, e->summary_len)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
        len += cdb_value_writer_len(&w);
    }
    if (len != head)
        return CDB_E_CORRUPT; /* the summary did not take the size counted */
    /* With a chain: total length and first chain page come before the
     * inline part of the body. */
    if (first) {
        cdb_store_u64(pl + len, (uint64_t)e->body_len, o);
        store_lid(db, pl + len + 8, first);
        len += 8u + db->pager.addr_width;
    }
    if (inline_len)
        memmove(pl + len, e->body, inline_len);
    len += inline_len;
    /* A small revision becomes a nibblet (spec 16.1), found by its owner
     * key (entry, revision). */
    if (!first && cdb_nib_fits(db, len)) {
        uint8_t k[16];
        return cdb_nib_put(db, pool->slot.store_id, CDB_NIB_KIND_REVISION, k, k_rev(k, e->entry, e->rev), pl, len,
                           lid);
    }
    if ((s = cdb_pager_alloc(&db->pager, lid)) != CDB_OK)
        return s;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DOCUMENT;
    t.store_id = pool->slot.store_id;
    return cdb_pager_write(&db->pager, *lid, &t, pl, len);
}

/* -------------------------------------------------------- patches */

/* One parsed patch operation [op, path, value] (spec 14.9.2). value points
 * into the patch bytes. */
typedef struct patch_op {
    int64_t op;
    uint64_t path[MAX_PATH];
    unsigned depth;
    const uint8_t *value; /* encoded value, for set */
    size_t value_len;
} patch_op;

/* Parses one operation. It must be exactly [int op, [field IDs...], value]
 * for a set, or [2, [field IDs...]] for a remove; the path must have 1 to
 * MAX_PATH non-negative IDs. Anything else is CDB_E_INVALID_ARG. */
static cdb_status parse_op(const cdb_value *opv, cdb_byte_order o, patch_op *op)
{
    cdb_value_iter it, pi;
    cdb_value item, seg;
    int done;
    cdb_status s;

    if (opv->type != CDB_TYPE_ARRAY)
        return CDB_E_INVALID_ARG;
    memset(op, 0, sizeof *op);
    if ((s = cdb_value_array_iter(opv, o, &it)) != CDB_OK)
        return s;
    if ((s = cdb_value_array_next(&it, &item, &done)) != CDB_OK || done || item.type != CDB_TYPE_INT)
        return CDB_E_INVALID_ARG;
    op->op = item.as.i64;
    if ((s = cdb_value_array_next(&it, &item, &done)) != CDB_OK || done || item.type != CDB_TYPE_ARRAY)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_value_array_iter(&item, o, &pi)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_array_next(&pi, &seg, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        if (seg.type != CDB_TYPE_INT || seg.as.i64 < 0 || op->depth >= MAX_PATH)
            return CDB_E_INVALID_ARG;
        op->path[op->depth++] = (uint64_t)seg.as.i64;
    }
    if (op->depth == 0)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_value_array_next(&it, &item, &done)) != CDB_OK)
        return s;
    if (op->op == CDB_PATCH_SET) {
        if (done)
            return CDB_E_INVALID_ARG;
        op->value = item.start;
        op->value_len = item.encoded_len;
        if ((s = cdb_value_array_next(&it, &item, &done)) != CDB_OK || !done)
            return CDB_E_INVALID_ARG;
    } else if (op->op != CDB_PATCH_REMOVE || !done) {
        return CDB_E_INVALID_ARG;
    }
    return CDB_OK;
}

/* Checks that a patch is a canonical array of well-formed operations. */
static cdb_status validate_patch(const uint8_t *patch, size_t len, cdb_byte_order o)
{
    cdb_value arr, opv;
    cdb_value_iter it;
    patch_op op;
    int done;
    cdb_status s;
    if (cdb_value_validate(patch, len, o, CDB_DECIMAL_COMPACT, 0) != CDB_OK)
        return CDB_E_INVALID_ARG;
    if (cdb_value_read(patch, len, o, &arr) != CDB_OK || arr.type != CDB_TYPE_ARRAY)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_value_array_iter(&arr, o, &it)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_array_next(&it, &opv, &done)) != CDB_OK)
            return s;
        if (done)
            return CDB_OK;
        if ((s = parse_op(&opv, o, &op)) != CDB_OK)
            return s;
    }
}

/* Writes the chain of objects that leads to a set at a new path: field
 * path[depth], then nested single-field objects down to the value. */
static cdb_status write_chain(cdb_value_writer *w, const patch_op *op, unsigned depth)
{
    cdb_status s;
    if ((s = cdb_value_field(w, op->path[depth])) != CDB_OK)
        return s;
    if (depth + 1 == op->depth)
        return cdb_value_write_encoded(w, op->value, op->value_len);
    if ((s = cdb_value_begin_object(w)) != CDB_OK)
        return s;
    if ((s = write_chain(w, op, depth + 1)) != CDB_OK)
        return s;
    return cdb_value_end_object(w);
}

/* Writes object obj (NULL: empty) with one operation applied at depth.
 * A set through a field that is not an object replaces that field; a remove
 * of a path that does not exist changes nothing.
 *
 * Objects store their fields in ascending field-ID order (the canonical
 * form, spec 14.6.5), so this copies fields across in order and inserts or
 * replaces the target field at its sorted position. `handled` records that
 * the target's position has been passed. */
static cdb_status rewrite(cdb_value_writer *w, const cdb_value *obj, cdb_byte_order o,
                          const patch_op *op, unsigned depth)
{
    uint64_t target = op->path[depth];
    int handled = 0;
    cdb_status s;

    if ((s = cdb_value_begin_object(w)) != CDB_OK)
        return s;
    if (obj != NULL) {
        cdb_value_iter it;
        cdb_value v;
        uint64_t fid;
        int done;
        if ((s = cdb_value_object_iter(obj, o, &it)) != CDB_OK)
            return s;
        for (;;) {
            if ((s = cdb_value_object_next(&it, &fid, &v, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            /* Passed the target's position without seeing it: the field is
             * new, so a set writes it here. */
            if (!handled && fid > target) {
                handled = 1;
                if (op->op == CDB_PATCH_SET && (s = write_chain(w, op, depth)) != CDB_OK)
                    return s;
            }
            /* The target field: at the end of the path, set replaces it and
             * remove drops it; otherwise recurse into it if it is an object,
             * or (for a set) replace it with a new chain. A remove through a
             * non-object falls through and keeps the field unchanged. */
            if (fid == target) {
                handled = 1;
                if (depth + 1 == op->depth) {
                    if (op->op == CDB_PATCH_SET && (s = write_chain(w, op, depth)) != CDB_OK)
                        return s;
                    continue;
                }
                if (v.type == CDB_TYPE_OBJECT) {
                    if ((s = cdb_value_field(w, fid)) != CDB_OK || (s = rewrite(w, &v, o, op, depth + 1)) != CDB_OK)
                        return s;
                    continue;
                }
                if (op->op == CDB_PATCH_SET) {
                    if ((s = write_chain(w, op, depth)) != CDB_OK)
                        return s;
                    continue;
                }
            }
            /* Any other field is copied unchanged. */
            if ((s = cdb_value_field(w, fid)) != CDB_OK || (s = cdb_value_write_encoded(w, v.start, v.encoded_len)) != CDB_OK)
                return s;
        }
    }
    /* The target sorts after every existing field (or obj is empty). */
    if (!handled && op->op == CDB_PATCH_SET && (s = write_chain(w, op, depth)) != CDB_OK)
        return s;
    return cdb_value_end_object(w);
}

/* Applies a patch to the document in buffer *cur (length *len), alternating
 * between the two document buffers. *cur and *len follow the result.
 * Each operation rewrites the whole document into the other buffer. A
 * malformed stored patch is CDB_E_CORRUPT; a result too big for a document
 * buffer is CDB_E_OVERFLOW. */
static cdb_status apply_patch(cdb_db *db, unsigned *cur, size_t *len, const uint8_t *patch, size_t plen)
{
    cdb_byte_order o = db->pager.order;
    cdb_value arr, opv;
    cdb_value_iter it;
    int done;
    cdb_status s;

    if (cdb_value_read(patch, plen, o, &arr) != CDB_OK || arr.type != CDB_TYPE_ARRAY)
        return CDB_E_CORRUPT;
    if ((s = cdb_value_array_iter(&arr, o, &it)) != CDB_OK)
        return s;
    for (;;) {
        unsigned next = *cur == B_DOC0 ? B_DOC1 : B_DOC0;
        cdb_value_writer w;
        cdb_value doc;
        patch_op op;
        if ((s = cdb_value_array_next(&it, &opv, &done)) != CDB_OK)
            return s;
        if (done)
            return CDB_OK;
        if (parse_op(&opv, o, &op) != CDB_OK)
            return CDB_E_CORRUPT;
        if (cdb_value_read(buf(db, *cur), *len, o, &doc) != CDB_OK || doc.type != CDB_TYPE_OBJECT)
            return CDB_E_CORRUPT;
        cdb_value_writer_init(&w, buf(db, next), dbuf_size(db), o, CDB_DECIMAL_COMPACT);
        if ((s = rewrite(&w, &doc, o, &op, 0)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
        *cur = next;
        *len = cdb_value_writer_len(&w);
    }
}

/* A patch turning document a into document b: top-level fields that differ
 * are set, and fields b lacks are removed. Written to the work buffer.
 * This is how a replace is stored as a delta (spec 14.9.2). Both objects
 * list fields in ascending ID order, so one merge-style pass over the two
 * field lists finds every difference. A changed field is replaced whole;
 * nested objects are not diffed. CDB_E_OVERFLOW if the patch does not fit
 * the work buffer. */
static cdb_status diff(cdb_db *db, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, size_t *plen)
{
    cdb_byte_order o = db->pager.order;
    cdb_value va, vb, fa, fb;
    cdb_value_iter ia, ib;
    uint64_t ida = 0, idb = 0;
    int da, db_done;
    cdb_value_writer w;
    cdb_status s;

    if (cdb_value_read(a, alen, o, &va) != CDB_OK || cdb_value_read(b, blen, o, &vb) != CDB_OK)
        return CDB_E_CORRUPT;
    cdb_value_writer_init(&w, buf(db, B_WORK), dbuf_size(db), o, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    if ((s = cdb_value_object_iter(&va, o, &ia)) != CDB_OK || (s = cdb_value_object_iter(&vb, o, &ib)) != CDB_OK)
        return s;
    if ((s = cdb_value_object_next(&ia, &ida, &fa, &da)) != CDB_OK || (s = cdb_value_object_next(&ib, &idb, &fb, &db_done)) != CDB_OK)
        return s;
    while (!da || !db_done) {
        if (!db_done && (da || idb < ida)) {          /* only in b: set */
            cdb_value_begin_array(&w);
            cdb_value_write_int(&w, CDB_PATCH_SET);
            cdb_value_begin_array(&w);
            cdb_value_write_int(&w, (int64_t)idb);
            cdb_value_end_array(&w);
            cdb_value_write_encoded(&w, fb.start, fb.encoded_len);
            cdb_value_end_array(&w);
            s = cdb_value_object_next(&ib, &idb, &fb, &db_done);
        } else if (!da && (db_done || ida < idb)) {   /* only in a: remove */
            cdb_value_begin_array(&w);
            cdb_value_write_int(&w, CDB_PATCH_REMOVE);
            cdb_value_begin_array(&w);
            cdb_value_write_int(&w, (int64_t)ida);
            cdb_value_end_array(&w);
            cdb_value_end_array(&w);
            s = cdb_value_object_next(&ia, &ida, &fa, &da);
        } else {                                      /* in both: set if changed */
            if (fa.encoded_len != fb.encoded_len || memcmp(fa.start, fb.start, fa.encoded_len) != 0) {
                cdb_value_begin_array(&w);
                cdb_value_write_int(&w, CDB_PATCH_SET);
                cdb_value_begin_array(&w);
                cdb_value_write_int(&w, (int64_t)idb);
                cdb_value_end_array(&w);
                cdb_value_write_encoded(&w, fb.start, fb.encoded_len);
                cdb_value_end_array(&w);
            }
            if ((s = cdb_value_object_next(&ia, &ida, &fa, &da)) == CDB_OK)
                s = cdb_value_object_next(&ib, &idb, &fb, &db_done);
        }
        if (s != CDB_OK)
            return s;
    }
    cdb_value_end_array(&w);
    if (w.error != CDB_OK)
        return w.error == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : w.error;
    *plen = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* -------------------------------------------------------- reading */

/* A rebuilt document: the body is in document buffer `buffer`, the summary
 * (if any) in B_SUMMARY. target.summary and target.body are cleared, since
 * they pointed into buffers that were reused while rebuilding. */
typedef struct materialized {
    unsigned buffer;     /* B_DOC0 or B_DOC1 */
    size_t len;
    int has_summary;     /* the summary is in the summary buffer */
    size_t summary_len;
    envelope target;     /* envelope of the requested revision */
} materialized;

/* Rebuilds revision rev (0 = head): from the nearest full revision at or
 * before it, applying each patch in turn.
 * Uses B_DOC0, B_DOC1, B_WORK, B_PAGE and B_SUMMARY. CDB_E_NOT_FOUND for an
 * unknown entry, a revision past the head, or a tombstone. */
static cdb_status materialize(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, uint64_t rev,
                              materialized *m)
{
    uint64_t lids[MAX_CHAIN], cur;
    unsigned n = 0, i;
    head_rec head;
    envelope e;
    int found;
    cdb_status s;

    memset(m, 0, sizeof *m);
    if ((s = head_get(pool, entry, &head, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    cur = rev ? rev : head.rev;
    if (cur > head.rev)
        return CDB_E_NOT_FOUND;
    /* Pass 1: walk back through parent links from the target revision until
     * a full revision (anything but a delta), collecting the pages in
     * lids[] (newest first). Envelopes are checked against the IDs we
     * expect, and parents must strictly decrease, so a damaged chain cannot
     * loop. */
    for (;;) {
        uint64_t lid;
        if (n >= MAX_CHAIN)
            return CDB_E_CORRUPT;
        if (cur == head.rev)
            lid = head.lid;
        else if ((s = rev_lid(pool, entry, cur, &lid)) != CDB_OK)
            return s;
        if ((s = read_revision(db, lid, &e, NULL)) != CDB_OK)
            return s;
        if (e.entry != entry || e.rev != cur)
            return CDB_E_CORRUPT;
        if (n == 0)
            m->target = e;
        lids[n++] = lid;
        if (e.kind != CDB_REV_DELTA)
            break;
        if (e.parent == 0 || e.parent >= cur)
            return CDB_E_CORRUPT;
        cur = e.parent;
    }
    m->target.summary = NULL;
    m->target.body = NULL;
    if (m->target.kind == CDB_REV_TOMBSTONE)
        return CDB_E_NOT_FOUND;
    if (e.kind == CDB_REV_TOMBSTONE)
        return CDB_E_CORRUPT; /* a delta cannot follow a tombstone */

    /* Pass 2: the full revision at the base of the chain, then each patch
     * (patches in overflow chains are gathered in the work buffer). The
     * summary is taken from the newest revision in the chain that has one. */
    if ((s = read_revision(db, lids[n - 1], &e, buf(db, B_DOC0))) != CDB_OK)
        return s;
    if (e.body != buf(db, B_DOC0))
        memcpy(buf(db, B_DOC0), e.body, e.body_len);
    m->buffer = B_DOC0;
    m->len = e.body_len;
    if (e.summary) {
        memmove(buf(db, B_SUMMARY), e.summary, e.summary_len);
        m->has_summary = 1;
        m->summary_len = e.summary_len;
    }
    for (i = n - 1; i-- > 0;) {
        if ((s = read_revision(db, lids[i], &e, buf(db, B_WORK))) != CDB_OK)
            return s;
        if (e.summary) {
            memmove(buf(db, B_SUMMARY), e.summary, e.summary_len);
            m->has_summary = 1;
            m->summary_len = e.summary_len;
        }
        if ((s = apply_patch(db, &m->buffer, &m->len, e.body, e.body_len)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

cdb_status cdb_doc_get(cdb_db *db, uint64_t store, uint64_t entry, uint64_t rev,
                       void *body, size_t cap, size_t *body_len,
                       char *summary, size_t summary_cap, size_t *summary_len, cdb_doc_info *info)
{
    struct cdb_doc_pool *pool;
    materialized m;
    cdb_status s;

    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if ((s = materialize(db, pool, entry, rev, &m)) != CDB_OK)
        return s;
    /* Report sizes even when the caller's buffers are too small, so the
     * caller can retry. */
    *body_len = m.len;
    if (m.len > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memcpy(body, buf(db, m.buffer), m.len);
    if (summary_len)
        *summary_len = m.has_summary ? m.summary_len : 0;
    if (summary && m.has_summary) {
        if (m.summary_len > summary_cap)
            return CDB_E_BUFFER_TOO_SMALL;
        memcpy(summary, buf(db, B_SUMMARY), m.summary_len);
    }
    if (info) {
        info->revision = m.target.rev;
        info->kind = m.target.kind;
        info->commit_number = m.target.commit;
        info->principal = m.target.principal;
        info->schema_version = m.target.schema;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- apply */

/* Splits an insert/replace payload: flags, optional summary, body
 * (spec 14.9.3). The outputs point into d->data. */
static cdb_status split_payload(const cdb_delta *d, const uint8_t **summary, size_t *slen,
                                const uint8_t **body, size_t *blen)
{
    const uint8_t *p = d->data, *end = d->data + d->len;
    *summary = NULL;
    *slen = 0;
    if (d->len < 1)
        return CDB_E_MALFORMED;
    if (*p++ & PAYLOAD_SUMMARY) {
        uint64_t len;
        size_t n;
        if (cdb_varint_decode(p, (size_t)(end - p), &len, &n) != CDB_OK || len > (uint64_t)(end - p) - n)
            return CDB_E_MALFORMED;
        *summary = p + n;
        *slen = (size_t)len;
        p += n + (size_t)len;
    }
    *body = p;
    *blen = (size_t)(end - p);
    return CDB_OK;
}

cdb_status cdb_doc_split_payload(const cdb_delta *d, const uint8_t **summary, size_t *slen,
                                 const uint8_t **body, size_t *blen)
{
    return split_payload(d, summary, slen, body, blen);
}

/* Writes revision e for the entry and points the head at it. chain is the
 * number of deltas since the last full revision, including this one (0 for
 * a full revision or tombstone). */
static cdb_status commit_revision(cdb_db *db, struct cdb_doc_pool *pool, envelope *e, uint32_t chain)
{
    head_rec h;
    uint64_t lid;
    cdb_status s;
    if ((s = write_revision(db, pool, e, &lid)) != CDB_OK || (s = rev_put(pool, e->entry, e->rev, lid)) != CDB_OK)
        return s;
    h.rev = e->rev;
    h.lid = lid;
    h.chain = chain;
    h.kind = e->kind;
    return head_put(pool, e->entry, &h);
}

/* -------------------------------------------------------- kept revisions
 *
 * A table without history keeps only its last `keep` revisions per row
 * (spec 9.11). Its new revisions are always full, and after each one the
 * revisions that fell out of the window are freed: their storage (a page
 * and its overflow chain, or a nibblet) and their keys in the revisions
 * index. */

/* Releases the storage of the revision at address lid. */
static cdb_status release(cdb_db *db, struct cdb_doc_pool *pool, uint64_t lid)
{
    envelope e;
    cdb_status s;
    if (cdb_nib_is(lid))
        return cdb_nib_free(db, lid);
    if ((s = read_revision(db, lid, &e, NULL)) != CDB_OK)
        return s;
    if (e.chain_first && (s = cdb_overflow_free(&db->pager, e.chain_first, pool->slot.store_id, e.chain_len,
                                                buf(db, B_PAGE))) != CDB_OK)
        return s;
    return cdb_pager_free(&db->pager, lid);
}

/* Frees one revision: its storage and its key. */
static cdb_status free_revision(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, uint64_t rev, uint64_t lid)
{
    uint8_t k[16];
    int found;
    cdb_status s = release(db, pool, lid);
    return s == CDB_OK ? cdb_index_delete(&pool->revs, k, k_rev(k, entry, rev), &found) : s;
}

/* Rewrites revision rev as a checkpoint if it is a delta, so it no longer
 * needs the revisions before it. Its envelope fields stay the same. */
static cdb_status make_full(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, uint64_t rev)
{
    materialized m;
    envelope e;
    uint64_t old, lid;
    cdb_status s;
    if ((s = rev_lid(pool, entry, rev, &old)) != CDB_OK || (s = read_revision(db, old, &e, NULL)) != CDB_OK)
        return s;
    if (e.kind != CDB_REV_DELTA)
        return CDB_OK;
    if ((s = materialize(db, pool, entry, rev, &m)) != CDB_OK)
        return s;
    e = m.target;
    e.kind = CDB_REV_CHECKPOINT;
    e.summary = m.has_summary ? buf(db, B_SUMMARY) : NULL;
    e.summary_len = m.summary_len;
    e.body = buf(db, m.buffer);
    e.body_len = m.len;
    if ((s = release(db, pool, old)) != CDB_OK || (s = write_revision(db, pool, &e, &lid)) != CDB_OK)
        return s;
    return rev_put(pool, entry, rev, lid);
}

/* Frees revision `from` and every older one still present, newest first,
 * stopping at the first that is gone (an earlier prune took the rest). */
static cdb_status free_from(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, uint64_t from)
{
    uint64_t r;
    for (r = from; r >= 1; r--) {
        uint8_t k[16], v[10];
        size_t vlen;
        uint64_t lid;
        int found;
        cdb_status s = cdb_index_get(&pool->revs, k, k_rev(k, entry, r), v, sizeof v, &vlen, &found);
        if (s != CDB_OK)
            return s;
        if (!found)
            break;
        if ((s = cdb_nib_addr_load(v, vlen, &lid)) != CDB_OK || (s = free_revision(db, pool, entry, r, lid)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* After revision head of entry is written: keep the last `keep` revisions,
 * the oldest of them full, and free the rest. */
static cdb_status prune(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, uint64_t head)
{
    cdb_status s;
    if (pool->keep == 0 || head <= pool->keep)
        return CDB_OK;
    if ((s = make_full(db, pool, entry, head - pool->keep + 1u)) != CDB_OK)
        return s;
    return free_from(db, pool, entry, head - pool->keep);
}

/* commit_revision for a table that keeps only some revisions. */
static cdb_status commit_kept(cdb_db *db, struct cdb_doc_pool *pool, envelope *e, uint32_t chain)
{
    cdb_status s = commit_revision(db, pool, e, chain);
    return s == CDB_OK ? prune(db, pool, e->entry, e->rev) : s;
}

/* The document apply. It never fails for logical reasons (a missing or
 * deleted document, a dropped pool): it runs after the commit record is
 * durable and again during recovery, so a logical failure here would make
 * the database impossible to open. Only I/O and corruption fail. */
static cdb_status apply_change(cdb_db *db, const cdb_delta *d)
{
    struct cdb_doc_pool *pool;
    head_rec head;
    envelope e;
    int found, live;
    cdb_status s;

    s = pool_get(db, d->store_id, &pool);
    if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG)
        return CDB_OK; /* no such document pool: nothing to change */
    if (s != CDB_OK)
        return s;
    if ((s = pool_indexes(db, pool)) != CDB_OK)
        return s;
    if (d->entry_id == 0)
        return CDB_OK;
    /* Keep the counter past every ID seen, including during replay. */
    if (d->entry_id >= pool->next_entry) {
        pool->next_entry = d->entry_id + 1; /* replay never hands an ID out again */
        pool->dirty = 1;
    }
    if ((s = head_get(pool, d->entry_id, &head, &found)) != CDB_OK)
        return s;
    live = found && head.kind != CDB_REV_TOMBSTONE;

    /* The new revision follows the head: revision IDs count up from 1 per
     * entry, and the parent is the previous head. */
    memset(&e, 0, sizeof e);
    e.entry = d->entry_id;
    e.rev = found ? head.rev + 1 : 1;
    e.parent = found ? head.rev : 0;
    e.commit = d->commit_number;
    /* Schema changes reconcile first and start a new journal, so the
     * current version is the one the change was validated under. */
    e.schema = pool->schema_version;

    switch (d->op) {
    case CDB_OP_INSERT:
    case CDB_OP_REPLACE: {
        const uint8_t *summary, *body;
        size_t slen, blen;
        if (split_payload(d, &summary, &slen, &body, &blen) != CDB_OK)
            return CDB_OK;
        /* Insert (also after a delete): the body is written whole. */
        if (!live) {
            if (d->op == CDB_OP_REPLACE)
                return CDB_OK; /* nothing to replace */
            e.kind = CDB_REV_ORIGINAL;
            e.summary = summary;
            e.summary_len = slen;
            e.body = body;
            e.body_len = blen;
            return commit_kept(db, pool, &e, 0);
        }
        e.summary = summary; /* a summary changes only when one is given */
        e.summary_len = slen;
        /* Replace of a live document: store it as a delta (the diff from
         * the current state) unless the chain is due for a checkpoint. */
        if (pool->keep == 0 && head.chain + 1u < checkpoint_every(db)) {
            materialized m;
            size_t plen;
            if ((s = materialize(db, pool, d->entry_id, 0, &m)) != CDB_OK)
                return s;
            s = diff(db, buf(db, m.buffer), m.len, body, blen, &plen);
            if (s == CDB_OK && revision_fits(db, summary != NULL, slen, plen)) {
                e.kind = CDB_REV_DELTA;
                e.body = buf(db, B_WORK);
                e.body_len = plen;
                return commit_kept(db, pool, &e, head.chain + 1u);
            }
            if (s != CDB_OK && s != CDB_E_OVERFLOW)
                return s;
            /* The patch would not fit its page; the whole document does
             * (checked when the change was made). */
        }
        /* Checkpoint: the new body whole. A checkpoint carries the current
         * summary forward when the change gives none (spec 14.9.2). */
        {
            materialized m;
            if (summary == NULL) {
                if ((s = materialize(db, pool, d->entry_id, 0, &m)) != CDB_OK)
                    return s;
                if (m.has_summary) {
                    e.summary = buf(db, B_SUMMARY);
                    e.summary_len = m.summary_len;
                }
            }
            e.kind = CDB_REV_CHECKPOINT;
            e.body = body;
            e.body_len = blen;
            return commit_kept(db, pool, &e, 0);
        }
    }
    case CDB_OP_PATCH:
        /* The patch was validated when journaled; a bad one here is skipped,
         * never failed. Normally it is stored as given, as a delta; when the
         * chain is due, the patched document is stored as a checkpoint. */
        if (!live || validate_patch(d->data, d->len, db->pager.order) != CDB_OK)
            return CDB_OK;
        if (pool->keep || head.chain + 1u >= checkpoint_every(db)) {
            materialized m;
            if ((s = materialize(db, pool, d->entry_id, 0, &m)) != CDB_OK)
                return s;
            if ((s = apply_patch(db, &m.buffer, &m.len, d->data, d->len)) != CDB_OK)
                return s;
            e.kind = CDB_REV_CHECKPOINT;
            e.summary = m.has_summary ? buf(db, B_SUMMARY) : NULL;
            e.summary_len = m.summary_len;
            e.body = buf(db, m.buffer);
            e.body_len = m.len;
            return commit_kept(db, pool, &e, 0);
        }
        e.kind = CDB_REV_DELTA;
        e.body = d->data;
        e.body_len = d->len;
        return commit_kept(db, pool, &e, head.chain + 1u);
    case CDB_OP_DELETE:
        /* A tombstone revision with no body; history stays readable. */
        if (!live)
            return CDB_OK;
        if (pool->keep) {
            /* A table without history: the row and its kept revisions go
             * (spec 9.11). */
            uint8_t k[16];
            if ((s = free_from(db, pool, d->entry_id, head.rev)) != CDB_OK
                || (s = cdb_index_delete(&pool->head, k, k_head(k, d->entry_id), &found)) != CDB_OK)
                return s;
        } else {
            e.kind = CDB_REV_TOMBSTONE;
            if ((s = commit_revision(db, pool, &e, 0)) != CDB_OK)
                return s;
        }
        return cdb_link_on_delete(db, d->store_id, d->entry_id); /* its links (spec 6.7) */
    default:
        return CDB_OK;
    }
}

/* The document's current state for the full-text index, copied out of the
 * document buffers. body is NULL when it is not live.
 * With copy, body and summary are copied into fts_buffers, because the
 * apply that follows reuses the document buffers. Without copy they point
 * into the document buffers. */
static cdb_status fts_capture(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, cdb_fts_doc *out,
                              int copy)
{
    materialized m;
    cdb_status s;
    memset(out, 0, sizeof *out);
    s = materialize(db, pool, entry, 0, &m);
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    out->body = buf(db, m.buffer);
    out->body_len = m.len;
    if (m.has_summary) {
        out->summary = buf(db, B_SUMMARY);
        out->summary_len = m.summary_len;
    }
    if (copy) {
        uint8_t *b = db->cfg.fts_buffers, *sm = db->cfg.fts_buffers + dbuf_size(db);
        memcpy(b, out->body, out->body_len);
        out->body = b;
        if (out->summary) {
            memcpy(sm, out->summary, out->summary_len);
            out->summary = sm;
        }
    }
    return CDB_OK;
}

/* The document apply: the change itself, then the full-text index from the
 * document's state before and after it.
 *
 * This is the default cdb_apply_fn, and it dispatches by store: the link
 * store goes to cdb_link_apply, vector collections to cdb_vec_apply,
 * time-series stores to cdb_ts_apply, and document pools are handled here. A store that is none of these (unknown,
 * dropped, another model) is ignored, as the never-fail rule requires. */
cdb_status cdb_doc_apply(void *ctx, cdb_pager *pager, const cdb_delta *d)
{
    cdb_db *db = (cdb_db *)ctx;
    struct cdb_doc_pool *pool;
    cdb_fts_doc before, after;
    head_rec h0, h1;
    int had, has;
    cdb_status s;
    (void)pager;

    /* The built-in apply serves every data model it knows. */
    if (cdb_link_is_store(db, d->store_id))
        return cdb_link_apply(db, d);
    if (cdb_vec_is_open(db, d->store_id)) {
        int handled;
        return cdb_vec_apply(db, d, &handled);
    }
    if (cdb_ts_is_open(db, d->store_id)) {
        int handled;
        return cdb_ts_apply(db, d, &handled);
    }
    /* Not a document pool (or documents not configured): it may be a vector
     * collection or time-series store that is not open yet; each model's
     * apply says whether it took the change. */
    s = pool_get(db, d->store_id, &pool);
    if (s == CDB_E_INVALID_ARG || s == CDB_E_UNSUPPORTED) {
        int handled;
        cdb_status vs = cdb_vec_apply(db, d, &handled);
        if (handled)
            return vs;
        vs = cdb_ts_apply(db, d, &handled);
        if (handled)
            return vs;
    }
    if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG)
        return CDB_OK; /* no such pool or collection: nothing to change */
    if (s != CDB_OK)
        return s;
    if (pool->is_table && d->entry_id != 0) {
        int is_col = 0;
        cdb_table_is_column(db, d->store_id, &is_col);
        if (d->entry_id >= pool->next_entry) {
            pool->next_entry = d->entry_id + 1;
            pool->dirty = 1;
        }
        if (is_col) {
            if (pool->keep == 0 || pool->keep > 1) {
                if ((s = apply_change(db, d)) != CDB_OK)
                    return s;
            }
            return cdb_table_col_apply(db, d->store_id, d);
        }
        /* A table: its indexes follow the row from before to after. */
        if ((s = cdb_table_apply_before(db, d->store_id, d->entry_id)) != CDB_OK
            || (s = apply_change(db, d)) != CDB_OK)
            return s;
        return cdb_table_apply_after(db, d->store_id, d->entry_id);
    }
    if (!pool->fts_on || d->entry_id == 0)
        return apply_change(db, d);
    /* With full-text search: capture the document before, apply, and if a
     * new revision was written, update the index from before to after. */
    if ((s = head_get(pool, d->entry_id, &h0, &had)) != CDB_OK
        || (s = fts_capture(db, pool, d->entry_id, &before, 1)) != CDB_OK)
        return s;
    if ((s = apply_change(db, d)) != CDB_OK)
        return s;
    if ((s = head_get(pool, d->entry_id, &h1, &has)) != CDB_OK)
        return s;
    if (!has || (had && h1.rev == h0.rev))
        return CDB_OK; /* the change was a no-op */
    if ((s = fts_capture(db, pool, d->entry_id, &after, 0)) != CDB_OK)
        return s;
    return cdb_fts_update(db, pool, d->entry_id, &before, &after);
}

/* The head revision of a document, for other models that refer to it.
 * CDB_E_INVALID_ARG: the store is not a document pool. */
cdb_status cdb_doc_head(cdb_db *db, uint64_t store, uint64_t entry, uint64_t *rev, int *live)
{
    struct cdb_doc_pool *pool;
    head_rec h;
    int found;
    cdb_status s;
    *rev = 0;
    *live = 0;
    if ((s = pool_get(db, store, &pool)) != CDB_OK)
        return s;
    if ((s = head_get(pool, entry, &h, &found)) != CDB_OK || !found)
        return s;
    *rev = h.rev;
    *live = h.kind != CDB_REV_TOMBSTONE;
    return CDB_OK;
}

/* -------------------------------------------------------- full text */

/* A catalog change: reconciles first (cdb_db_catalog_begin), builds the
 * index, records its settings and roots in the store record (which
 * commits), then rotates the journal (cdb_db_catalog_end). */
cdb_status cdb_doc_create_full_text(cdb_db *db, uint64_t store, const cdb_fts_config *cfg)
{
    struct cdb_doc_pool *pool;
    uint8_t settings[CDB_CATALOG_FULL_TEXT_MAX];
    size_t len;
    cdb_status s;

    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (pool->fts_on)
        return CDB_E_EXISTS;
    if ((s = cdb_fts_check_config(cfg)) != CDB_OK)
        return s;
    if (db->cfg.fts_buffers == NULL || db->cfg.fts_tokens == NULL
        || db->cfg.fts_tokens_cap < cdb_fts_tokens_needed(db->pager.page_size, db->cfg.doc_max))
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    pool->fts = *cfg;
    s = cdb_fts_create(db, pool);
    if (s == CDB_OK && pool->have_indexes) {
        /* Index the documents already in the pool: walk the head index
         * (every entry, in ID order), skip deleted ones, and add each live
         * document as a change from "nothing" to its current state. The
         * cursor's page buffer lies inside fts_buffers, after one document
         * buffer and one page. */
        cdb_index_cursor c;
        uint8_t k[16], v[HEAD_SIZE + 2u];
        size_t klen, vlen;
        int done;
        s = cdb_index_seek(&c, &pool->head, db->cfg.fts_buffers + dbuf_size(db) + db->pager.page_size, NULL, 0);
        while (s == CDB_OK) {
            cdb_fts_doc none, doc;
            if ((s = cdb_index_next(&c, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK || done)
                break;
            if (klen != 8 || (vlen != HEAD_SIZE && vlen != HEAD_SIZE + 2u)) {
                s = CDB_E_CORRUPT;
                break;
            }
            if (v[20] == CDB_REV_TOMBSTONE)
                continue;
            memset(&none, 0, sizeof none);
            if ((s = fts_capture(db, pool, cdb_load_u64(k, CDB_ORDER_BIG), &doc, 0)) == CDB_OK)
                s = cdb_fts_update(db, pool, cdb_load_u64(k, CDB_ORDER_BIG), &none, &doc);
        }
    }
    if (s == CDB_OK)
        s = cdb_fts_encode(cfg, pool->fts_terms.root, pool->fts_postings.root, pool->fts_lengths.root,
                           db->pager.order, settings, sizeof settings, &len);
    if (s == CDB_OK)
        s = cdb_catalog_set_full_text(&db->catalog, store, settings, len);
    if (s == CDB_OK)
        pool->fts_on = 1;
    /* On failure the pages written so far are unreferenced: space, not damage. */
    return cdb_db_catalog_end(db, s);
}

cdb_status cdb_doc_search(cdb_db *db, uint64_t store, const char *query, size_t len, int mode,
                          cdb_fts_hit *hits, size_t max, size_t *n)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    *n = 0;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (!pool->fts_on)
        return CDB_E_NOT_FOUND;
    return cdb_fts_search(db, pool, query, len, mode, hits, max, n);
}

/* -------------------------------------------------------- schemas */

/* Loads the pool's current schema into the schema buffer. *active is 0
 * when the pool has no schema or one that accepts every document.
 * The buffer is a one-entry cache keyed by (store, version), so repeated
 * changes to one pool do not re-read the catalog. A schema with rules this
 * build does not know gives CDB_E_UNSUPPORTED, which refuses the write:
 * the engine never enforces less than a schema says (spec 14.9.4). */
static cdb_status schema_load(cdb_db *db, struct cdb_doc_pool *pool, int *active)
{
    cdb_status s;
    *active = 0;
    if (pool->schema_version == 0 || pool->is_table)
        return CDB_OK; /* a table's schema is its columns, checked by the table layer */
    if (db->schema_version != pool->schema_version || db->schema_store != pool->slot.store_id) {
        db->schema_version = 0;
        if ((s = cdb_catalog_get_schema(&db->catalog, pool->slot.store_id, pool->schema_version, buf(db, B_SCHEMA),
                                        db->pager.page_size, &db->schema_len)) != CDB_OK)
            return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
        s = cdb_schema_check(buf(db, B_SCHEMA), db->schema_len, db->pager.order, &db->schema_trivial);
        if (s != CDB_OK)
            return s == CDB_E_UNSUPPORTED ? s : CDB_E_CORRUPT; /* unknown rules: a newer build wrote it */
        db->schema_store = pool->slot.store_id;
        db->schema_version = pool->schema_version;
    }
    *active = !db->schema_trivial;
    return CDB_OK;
}

static cdb_status schema_validate(cdb_db *db, const uint8_t *doc, size_t len)
{
    return cdb_schema_validate(buf(db, B_SCHEMA), db->schema_len, doc, len, db->pager.order);
}

/* Checks the schema is well formed before storing it, so a bad schema can
 * never become current. */
cdb_status cdb_doc_set_schema(cdb_db *db, uint64_t store, const void *schema, size_t len, uint32_t *version)
{
    struct cdb_doc_pool *pool;
    int trivial;
    cdb_status s;
    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if ((s = cdb_schema_check((const uint8_t *)schema, len, db->pager.order, &trivial)) != CDB_OK)
        return s;
    if ((s = cdb_db_add_schema(db, store, schema, len, version)) != CDB_OK)
        return s;
    pool->schema_version = *version;
    return CDB_OK;
}

cdb_status cdb_doc_get_schema(cdb_db *db, uint64_t store, uint32_t version, void *schema, size_t cap,
                              size_t *len, uint32_t *current)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (current)
        *current = pool->schema_version;
    if (version == 0)
        version = pool->schema_version;
    if (version == 0 || version > pool->schema_version)
        return CDB_E_NOT_FOUND;
    return cdb_catalog_get_schema(&db->catalog, store, version, schema, cap, len);
}

/* -------------------------------------------------------- staging
 *
 * Each change is checked against the document as the open transaction sees
 * it, which the pages do not show until commit: the resulting revision must
 * fit in one page, and in a pool with a schema it must conform. doc_stage is
 * a cache of those states: each record is store ID (8), entry ID (8), state
 * length (4), summary length (4), flags (1), reserved (7), then the state.
 * When a state does not fit, the document's record is dropped and the
 * transaction is marked as overflowed; from then on a document without a
 * record is rebuilt from its committed state and the transaction's own
 * journal records. Without doc_stage every such lookup reads the journal. */

#define STAGE_HDR 32u
#define STAGE_DELETED 0x1u
#define STAGE_SUMMARY 0x2u

/* Where a document's record sits in doc_stage, if it has one. Only the
 * summary's length is staged, not its text: the size checks need nothing
 * more. */
typedef struct staged {
    int found;
    size_t off;      /* of the record in doc_stage */
    size_t len;      /* state length */
    size_t summary_len;
    uint8_t flags;
} staged;

/* A document as the open transaction sees it. */
typedef struct tx_view {
    int live;
    unsigned b;          /* with need_state: the document buffer holding it */
    size_t len;
    int has_summary;
    size_t summary_len;
    uint32_t schema;     /* with need_state: the schema version the state was written under */
} tx_view;

/* Staging records are indexed in a hash table of CDB_STAGE_BUCKETS buckets.
 * Buckets store the byte offset of the chain head in doc_stage, or CDB_STAGE_NIL.
 * Bytes 28..31 of each record's STAGE_HDR store next_off in big-endian order. */
static inline unsigned stage_bucket(uint64_t store, uint64_t entry)
{
    uint64_t h = (store * 0x517cc1b727220a95ULL) ^ (entry * 0x9e3779b97f4a7c15ULL);
    return (unsigned)(((h ^ (h >> 32)) & (uint64_t)(CDB_STAGE_BUCKETS - 1u)));
}

/* Rebuilds the staging hash table over all records in doc_stage. */
static void stage_reindex(cdb_db *db)
{
    size_t off = 0;
    memset(db->stage_buckets, 0xFF, sizeof db->stage_buckets);
    if (db->cfg.doc_stage == NULL)
        return;
    while (off < db->stage_used) {
        uint8_t *r = db->cfg.doc_stage + off;
        uint64_t store = cdb_load_u64(r, CDB_ORDER_BIG);
        uint64_t entry = cdb_load_u64(r + 8, CDB_ORDER_BIG);
        size_t len = cdb_load_u32(r + 16, CDB_ORDER_BIG);
        unsigned b = stage_bucket(store, entry);
        cdb_store_u32(r + 28, db->stage_buckets[b], CDB_ORDER_BIG);
        db->stage_buckets[b] = (uint32_t)off;
        off += STAGE_HDR + len;
    }
}

/* Hash lookup of the staging record for (store, entry). */
static void stage_find(const cdb_db *db, uint64_t store, uint64_t entry, staged *st)
{
    const uint8_t *p = db->cfg.doc_stage;
    unsigned b;
    uint32_t off;
    memset(st, 0, sizeof *st);
    if (p == NULL || db->stage_used == 0)
        return;
    b = stage_bucket(store, entry);
    off = db->stage_buckets[b];
    while (off != CDB_STAGE_NIL) {
        const uint8_t *r = p + off;
        if (cdb_load_u64(r, CDB_ORDER_BIG) == store && cdb_load_u64(r + 8, CDB_ORDER_BIG) == entry) {
            st->found = 1;
            st->off = off;
            st->len = cdb_load_u32(r + 16, CDB_ORDER_BIG);
            st->summary_len = cdb_load_u32(r + 20, CDB_ORDER_BIG);
            st->flags = r[24];
            return;
        }
        off = cdb_load_u32(r + 28, CDB_ORDER_BIG);
    }
}

/* Records the document's new state, replacing its old record. state is
 * never inside doc_stage.
 * The invariant that keeps this safe: a document either has an up-to-date
 * record here, or stage_overflow is set (and replay_tx then reads the
 * journal). So the old record is always removed first, and if the new one
 * does not fit (or there is no doc_stage at all), the overflow flag is set
 * instead. Cannot fail. */
static void stage_put(cdb_db *db, uint64_t store, uint64_t entry, const staged *old, const tx_view *v,
                      const uint8_t *state)
{
    uint8_t *p = db->cfg.doc_stage, *r;
    size_t avail, len = v->live ? v->len : 0u;
    size_t rec_off;
    /* Remove the old record by closing the gap. */
    if (old->found) {
        size_t size = STAGE_HDR + old->len;
        memmove(p + old->off, p + old->off + size, db->stage_used - old->off - size);
        db->stage_used -= size;
    }
    avail = p ? db->cfg.doc_stage_size - db->stage_used : 0u;
    if (avail < STAGE_HDR || len > avail - STAGE_HDR) {
        db->stage_overflow = 1;
        if (old->found)
            stage_reindex(db);
        return;
    }
    /* Append the new record at the end. */
    rec_off = db->stage_used;
    r = p + rec_off;
    memset(r, 0, STAGE_HDR);
    cdb_store_u64(r, store, CDB_ORDER_BIG);
    cdb_store_u64(r + 8, entry, CDB_ORDER_BIG);
    cdb_store_u32(r + 16, (uint32_t)len, CDB_ORDER_BIG);
    cdb_store_u32(r + 20, (uint32_t)v->summary_len, CDB_ORDER_BIG);
    r[24] = (uint8_t)((v->live ? 0u : STAGE_DELETED) | (v->has_summary ? STAGE_SUMMARY : 0u));
    if (len)
        memcpy(r + STAGE_HDR, state, len);
    db->stage_used += STAGE_HDR + len;
    if (old->found) {
        stage_reindex(db);
    } else {
        unsigned b = stage_bucket(store, entry);
        cdb_store_u32(r + 28, db->stage_buckets[b], CDB_ORDER_BIG);
        db->stage_buckets[b] = (uint32_t)rec_off;
    }
}

/* The committed document with the open transaction's journaled changes to
 * it applied in order, as the document apply will apply them. With
 * need_state, a live document's body ends up in document buffer v->b.
 * Without need_state only liveness is worked out, which is much cheaper.
 * Used when the document has no staging record. */
static cdb_status replay_tx(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, int need_state, tx_view *v)
{
    uint64_t off;
    cdb_status s;

    /* Start from the committed state (as of the last applied commit). */
    memset(v, 0, sizeof *v);
    int is_col = 0;
    if (pool->is_table)
        cdb_table_is_column(db, pool->slot.store_id, &is_col);

    if (is_col) {
        size_t rlen = 0;
        int live = 0;
        uint32_t ver = 0;
        s = cdb_table_col_committed_row(db, pool->slot.store_id, entry, buf(db, B_DOC0), capacity(db),
                                        &rlen, &live, &ver);
        if (s != CDB_OK && s != CDB_E_NOT_FOUND)
            return s;
        v->live = live;
        if (v->live) {
            v->b = B_DOC0;
            v->len = rlen;
            v->has_summary = 0;
            v->summary_len = 0;
            v->schema = ver;
        }
    } else if (need_state) {
        materialized m;
        s = materialize(db, pool, entry, 0, &m);
        if (s != CDB_OK && s != CDB_E_NOT_FOUND)
            return s;
        v->live = s == CDB_OK;
        if (v->live) {
            v->b = m.buffer;
            v->len = m.len;
            v->has_summary = m.has_summary;
            v->summary_len = m.summary_len;
            v->schema = m.target.schema;
        }
    } else {
        head_rec h;
        int found;
        if ((s = head_get(pool, entry, &h, &found)) != CDB_OK)
            return s;
        v->live = found && h.kind != CDB_REV_TOMBSTONE;
    }
    if (!db->stage_overflow)
        return CDB_OK; /* every change of this transaction is staged */
    /* Then replay this transaction's own records for the entry, read back
     * from the journal, mirroring what the apply will do with them. */
    for (off = db->tx_start; off < db->journal.end;) {
        cdb_record r;
        cdb_delta d;
        uint64_t next;
        if ((s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next)) != CDB_OK)
            return s;
        off = next;
        if (r.kind != CDB_RECORD_DATA || r.tx_id != db->tx_id || r.store_id != pool->slot.store_id
            || r.entry_id != entry || r.payload_len < 1)
            continue;
        memset(&d, 0, sizeof d);
        d.op = r.payload[0];
        d.data = r.payload + 1;
        d.len = r.payload_len - 1u;
        switch (d.op) {
        case CDB_OP_INSERT:
        case CDB_OP_REPLACE: {
            const uint8_t *summary, *body;
            size_t slen, blen;
            if (!v->live && d.op == CDB_OP_REPLACE)
                break;
            if (split_payload(&d, &summary, &slen, &body, &blen) != CDB_OK)
                return CDB_E_CORRUPT;
            if (!v->live)
                v->has_summary = 0;
            v->live = 1;
            if (summary) { /* a summary changes only when one is given */
                v->has_summary = 1;
                v->summary_len = slen;
            }
            if (need_state) {
                memcpy(buf(db, B_DOC0), body, blen);
                v->b = B_DOC0;
                v->len = blen;
                v->schema = pool->schema_version; /* changes are made under the current schema */
            }
            break;
        }
        case CDB_OP_PATCH:
            if (v->live && need_state && (s = apply_patch(db, &v->b, &v->len, d.data, d.len)) != CDB_OK)
                return s;
            v->schema = pool->schema_version;
            break;
        case CDB_OP_DELETE:
            v->live = 0;
            break;
        default:
            break;
        }
    }
    return CDB_OK;
}

/* The document as the open transaction sees it: from its staging record if
 * it has one, otherwise rebuilt by replay_tx. A staged body is copied into
 * B_DOC0. */
static cdb_status tx_doc(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, const staged *st,
                         int need_state, tx_view *v)
{
    if (!st->found)
        return replay_tx(db, pool, entry, need_state, v);
    memset(v, 0, sizeof *v);
    v->live = !(st->flags & STAGE_DELETED);
    v->has_summary = (st->flags & STAGE_SUMMARY) != 0;
    v->summary_len = st->summary_len;
    if (v->live && need_state) {
        memcpy(buf(db, B_DOC0), db->cfg.doc_stage + st->off + STAGE_HDR, st->len);
        v->b = B_DOC0;
        v->len = st->len;
        v->schema = pool->schema_version; /* staged: changed by this transaction */
    }
    return CDB_OK;
}

/* -------------------------------------------------------- transactions */

/* Common checks for every document change: a transaction must be open, the
 * database must use the built-in apply (a custom apply would not know how to
 * apply document records), and the store must be a document pool. *guarded
 * says whether the pool has an active schema to validate against. */
static cdb_status tx_pool(cdb_db *db, uint64_t store, struct cdb_doc_pool **pool, int *guarded)
{
    cdb_status s;
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED;
    if ((s = doc_pool(db, store, pool)) != CDB_OK)
        return s;
    return schema_load(db, *pool, guarded);
}

/* Insert and replace. Steps: the body must be a valid, canonical object;
 * for a replace, the document must be live as the transaction sees it; the
 * resulting revision (with the summary it will carry) must fit; it must
 * satisfy the schema. Only then is the payload built in B_WORK
 * (flags, optional summary, body: spec 14.9.3) and journaled, and the new
 * state staged. Errors: CDB_E_INVALID_ARG, CDB_E_NOT_FOUND, CDB_E_OVERFLOW,
 * CDB_E_SCHEMA; nothing is journaled on any error. */
static cdb_status put_body(cdb_db *db, struct cdb_doc_pool *pool, int guarded, uint64_t entry, uint8_t op,
                           const char *summary, size_t summary_len, const void *body, size_t body_len)
{
    uint8_t *p = buf(db, B_WORK);
    size_t cap = capacity(db), len = 0;
    cdb_value v;
    staged st;
    tx_view now;
    cdb_status s;

    if (body == NULL || cdb_value_validate((const uint8_t *)body, body_len, db->pager.order, CDB_DECIMAL_COMPACT, 0) != CDB_OK
        || cdb_value_read((const uint8_t *)body, body_len, db->pager.order, &v) != CDB_OK || v.type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    if (summary == NULL && summary_len)
        return CDB_E_INVALID_ARG;
    int is_col = 0;
    if (pool->is_table)
        cdb_table_is_column(db, pool->slot.store_id, &is_col);

    if (summary_len > cap || (!is_col && body_len > body_max(db)))
        return CDB_E_OVERFLOW;
    if (is_col) {
        uint32_t ccap = 0;
        cdb_table_chunk_cap(db, pool->slot.store_id, &ccap);
        if (body_len > ccap)
            return CDB_E_OVERFLOW;
    }
    stage_find(db, pool->slot.store_id, entry, &st);
    memset(&now, 0, sizeof now);
    if (op == CDB_OP_REPLACE) {
        if ((s = tx_doc(db, pool, entry, &st, 1, &now)) != CDB_OK)
            return s;
        if (!now.live)
            return CDB_E_NOT_FOUND;
    }
    now.live = 1;
    now.len = body_len;
    if (summary) {
        now.has_summary = 1;
        now.summary_len = summary_len;
    }
    /* The revision must be writable, with the summary it will carry. */
    if (!is_col && !revision_fits(db, now.has_summary, now.summary_len, body_len))
        return CDB_E_OVERFLOW;
    if (guarded && (s = schema_validate(db, (const uint8_t *)body, body_len)) != CDB_OK)
        return s;
    p[len++] = summary ? (uint8_t)PAYLOAD_SUMMARY : 0u;
    if (summary) {
        /* The value writer is used only to check the summary is UTF-8; the
         * payload itself stores a plain varint length and the bytes. */
        cdb_value_writer w;
        cdb_value_writer_init(&w, p + len, cap - len, db->pager.order, CDB_DECIMAL_COMPACT);
        if (cdb_value_write_string(&w, summary, summary_len) != CDB_OK)
            return CDB_E_INVALID_ARG; /* not UTF-8 */
        len += cdb_varint_encode(summary_len, p + len);
        memcpy(p + len, summary, summary_len);
        len += summary_len;
    }
    memcpy(p + len, body, body_len);
    len += body_len;
    if ((s = cdb_tx_put(db, pool->slot.store_id, entry, op, p, len)) != CDB_OK)
        return s;
    stage_put(db, pool->slot.store_id, entry, &st, &now, (const uint8_t *)body);
    return CDB_OK;
}

/* The entry ID is taken from the pool's counter, which advances only after
 * the change is journaled. An aborted insert still uses up its ID; IDs are
 * never reused. */
cdb_status cdb_doc_insert(cdb_db *db, uint64_t store, const char *summary, size_t summary_len,
                          const void *body, size_t body_len, uint64_t *entry)
{
    struct cdb_doc_pool *pool;
    uint64_t id;
    int guarded;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool, &guarded)) != CDB_OK
        || (s = cdb_db_peek_entry(db, store, &pool->next_entry, &id)) != CDB_OK)
        return s;
    if ((s = put_body(db, pool, guarded, id, CDB_OP_INSERT, summary, summary_len, body, body_len)) != CDB_OK)
        return s;
    *entry = cdb_db_take_entry(db, store, &pool->next_entry, &pool->dirty);
    return CDB_OK;
}

cdb_status cdb_doc_replace(cdb_db *db, uint64_t store, uint64_t entry, const char *summary,
                           size_t summary_len, const void *body, size_t body_len)
{
    struct cdb_doc_pool *pool;
    int guarded;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool, &guarded)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    return put_body(db, pool, guarded, entry, CDB_OP_REPLACE, summary, summary_len, body, body_len);
}

/* A patch is checked twice for size: the patch itself must fit as a delta
 * revision's body, and the patched document must fit as a later checkpoint.
 * The schema check runs on the patched document. */
cdb_status cdb_doc_patch(cdb_db *db, uint64_t store, uint64_t entry, const void *patch, size_t patch_len)
{
    struct cdb_doc_pool *pool;
    staged st;
    tx_view now;
    int guarded;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool, &guarded)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    if (patch == NULL || validate_patch((const uint8_t *)patch, patch_len, db->pager.order) != CDB_OK)
        return CDB_E_INVALID_ARG;
    if (!revision_fits(db, 0, 0, patch_len))
        return CDB_E_OVERFLOW; /* the patch is a delta revision's body */
    stage_find(db, store, entry, &st);
    if ((s = tx_doc(db, pool, entry, &st, 1, &now)) != CDB_OK)
        return s;
    if (!now.live)
        return CDB_E_NOT_FOUND;
    if ((s = apply_patch(db, &now.b, &now.len, (const uint8_t *)patch, patch_len)) != CDB_OK)
        return s;
    /* A later checkpoint holds the whole result and the summary. */
    if (!revision_fits(db, now.has_summary, now.summary_len, now.len))
        return CDB_E_OVERFLOW;
    if (guarded && (s = schema_validate(db, buf(db, now.b), now.len)) != CDB_OK)
        return s;
    if ((s = cdb_tx_put(db, store, entry, CDB_OP_PATCH, patch, patch_len)) != CDB_OK)
        return s;
    stage_put(db, store, entry, &st, &now, buf(db, now.b));
    return CDB_OK;
}

/* Delete only needs liveness, not the document's contents. Link rules are
 * checked here, before journaling, so the apply never has to refuse. */
cdb_status cdb_doc_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    struct cdb_doc_pool *pool;
    staged st;
    tx_view now;
    int guarded;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool, &guarded)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    stage_find(db, store, entry, &st);
    if ((s = tx_doc(db, pool, entry, &st, 0, &now)) != CDB_OK)
        return s;
    if (!now.live)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_link_check_delete(db, store, entry)) != CDB_OK)
        return s; /* a refuse-type link points at it (spec 6.7) */
    if ((s = cdb_tx_put(db, store, entry, CDB_OP_DELETE, NULL, 0)) != CDB_OK)
        return s;
    memset(&now, 0, sizeof now);
    stage_put(db, store, entry, &st, &now, NULL);
    return CDB_OK;
}

cdb_status cdb_doc_live_in_tx(cdb_db *db, uint64_t store, uint64_t entry, int *live)
{
    struct cdb_doc_pool *pool;
    staged st;
    tx_view v;
    cdb_status s;
    *live = 0;
    if ((s = pool_get(db, store, &pool)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_OK;
    stage_find(db, store, entry, &st);
    if ((s = tx_doc(db, pool, entry, &st, 0, &v)) != CDB_OK)
        return s;
    *live = v.live;
    return CDB_OK;
}

cdb_status cdb_doc_get_in_tx(cdb_db *db, uint64_t store, uint64_t entry, size_t staged_off, void *body,
                             size_t cap, size_t *len, int *live, uint32_t *schema)
{
    struct cdb_doc_pool *pool;
    staged st;
    tx_view v;
    cdb_status s;
    *live = 0;
    *len = 0;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_OK;
    if (staged_off != CDB_STAGE_OFF_NONE && staged_off + STAGE_HDR <= db->stage_used
        && db->cfg.doc_stage != NULL
        && cdb_load_u64(db->cfg.doc_stage + staged_off, CDB_ORDER_BIG) == store
        && cdb_load_u64(db->cfg.doc_stage + staged_off + 8, CDB_ORDER_BIG) == entry) {
        const uint8_t *r = db->cfg.doc_stage + staged_off;
        st.found = 1;
        st.off = staged_off;
        st.len = cdb_load_u32(r + 16, CDB_ORDER_BIG);
        st.summary_len = cdb_load_u32(r + 20, CDB_ORDER_BIG);
        st.flags = r[24];
    } else {
        stage_find(db, store, entry, &st);
    }
    if (st.found) {
        *live = !(st.flags & STAGE_DELETED);
        if (!*live)
            return CDB_OK;
        *len = st.len;
        if (st.len > cap)
            return CDB_E_BUFFER_TOO_SMALL;
        memcpy(body, db->cfg.doc_stage + st.off + STAGE_HDR, st.len);
        *live = 1;
        *schema = pool->schema_version;
        return CDB_OK;
    }
    if ((s = tx_doc(db, pool, entry, &st, 1, &v)) != CDB_OK)
        return s;
    if (!v.live)
        return CDB_OK;
    *len = v.len;
    if (v.len > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memcpy(body, buf(db, v.b), v.len);
    *live = 1;
    *schema = v.schema;
    return CDB_OK;
}

cdb_status cdb_doc_next_live(cdb_db *db, uint64_t store, uint64_t after, uint64_t *entry, int *done)
{
    struct cdb_doc_pool *pool;
    cdb_index_cursor c;
    uint8_t k[16], v[HEAD_SIZE + 2u];
    size_t klen, vlen;
    cdb_status s;
    *done = 1;
    if ((s = doc_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (!pool->have_indexes || after == UINT64_MAX)
        return CDB_OK;
    if ((s = cdb_index_seek(&c, &pool->head, buf(db, B_PAGE), k, k_head(k, after + 1u))) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_index_next(&c, k, sizeof k, &klen, v, sizeof v, &vlen, done)) != CDB_OK || *done)
            return s;
        if (klen != 8 || (vlen != HEAD_SIZE && vlen != HEAD_SIZE + 2u))
            return CDB_E_CORRUPT;
        if (v[20] != CDB_REV_TOMBSTONE) {
            *entry = cdb_load_u64(k, CDB_ORDER_BIG);
            return CDB_OK;
        }
    }
}

/* A revision nibblet moved (cdb_nib_merge): the revisions index, and the
 * head if it is the head revision, point at the new address. */
cdb_status cdb_doc_nib_moved(cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen, uint64_t old_addr,
                             uint64_t addr)
{
    struct cdb_doc_pool *pool;
    uint8_t v[10];
    size_t vlen;
    uint64_t entry, rev, cur;
    head_rec h;
    int found;
    cdb_status s;
    if (oklen != 16)
        return CDB_E_CORRUPT;
    s = pool_get(db, store, &pool);
    if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG || (s == CDB_OK && !pool->have_indexes))
        return CDB_E_NOT_FOUND; /* the store is gone */
    if (s != CDB_OK)
        return s;
    entry = cdb_load_u64(okey, CDB_ORDER_BIG);
    rev = cdb_load_u64(okey + 8, CDB_ORDER_BIG);
    if ((s = cdb_index_get(&pool->revs, okey, oklen, v, sizeof v, &vlen, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_nib_addr_load(v, vlen, &cur)) != CDB_OK)
        return s;
    if (cur != old_addr)
        return CDB_E_NOT_FOUND;
    if ((s = rev_put(pool, entry, rev, addr)) != CDB_OK || (s = head_get(pool, entry, &h, &found)) != CDB_OK)
        return s;
    if (found && h.rev == rev) {
        h.lid = addr;
        return head_put(pool, entry, &h);
    }
    return CDB_OK;
}

cdb_status cdb_doc_set_keep(cdb_db *db, uint64_t store, uint32_t keep)
{
    struct cdb_doc_pool *pool;
    cdb_status s = doc_pool(db, store, &pool);
    if (s == CDB_OK)
        pool->keep = keep;
    return s;
}

/* Staged records first, then (once staging overflowed) the journal: an
 * entry may be reported more than once. Each entry ID and the position of
 * the next record are taken before fn runs, so fn may read documents (which
 * reuses the record buffer) but must not change any. */
cdb_status cdb_doc_tx_entries(cdb_db *db, uint64_t store, cdb_doc_entry_fn fn, void *ctx)
{
    size_t off = 0;
    uint64_t joff;
    cdb_status s;
    if (!db->in_tx)
        return CDB_OK;
    while (off < db->stage_used) {
        const uint8_t *r = db->cfg.doc_stage + off;
        size_t rec_off = off;
        uint64_t entry = cdb_load_u64(r + 8, CDB_ORDER_BIG);
        int mine = cdb_load_u64(r, CDB_ORDER_BIG) == store;
        off += STAGE_HDR + cdb_load_u32(r + 16, CDB_ORDER_BIG);
        if (mine && (s = fn(ctx, entry, rec_off)) != CDB_OK)
            return s;
    }
    if (!db->stage_overflow)
        return CDB_OK;
    for (joff = db->tx_start; joff < db->journal.end;) {
        cdb_record r;
        uint64_t next;
        if ((s = cdb_journal_read(&db->journal, joff, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next))
            != CDB_OK)
            return s;
        joff = next;
        if (r.kind == CDB_RECORD_DATA && r.tx_id == db->tx_id && r.store_id == store && r.entry_id != 0
            && (s = fn(ctx, r.entry_id, CDB_STAGE_OFF_NONE)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}
