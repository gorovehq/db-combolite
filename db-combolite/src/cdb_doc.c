/* Documents: revision chains, patches and the document apply (spec 8, 14.9). */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_varint.h"

/* Record envelope (spec 14.9.1): entry, revision, parent, commit, principal
 * (8 bytes each), schema version (4), kind (1), flags (1), reserved (2);
 * then the summary (a string value) if flagged, then the body. */
#define ENV_SIZE 48u
#define ENV_FLAG_SUMMARY 0x01u

/* Head index value: revision, page, chain length, kind. */
#define HEAD_SIZE 21u

#define MAX_CHAIN 64u
#define MAX_PATH 16u
#define DEFAULT_CHECKPOINT_EVERY 16u

/* Insert/replace payload flag: a summary follows. */
#define PAYLOAD_SUMMARY 0x01u

/* -------------------------------------------------------- buffers */

static size_t capacity(const cdb_db *db)
{
    return cdb_page_capacity(db->pager.fmt, db->pager.page_size);
}

static uint8_t *buf(cdb_db *db, unsigned i)
{
    return db->cfg.doc_buffers + (size_t)i * db->pager.page_size;
}

enum { B_DOC0 = 0, B_DOC1 = 1, B_PAGE = 2, B_WORK = 3, B_SUMMARY = 4 };

static unsigned checkpoint_every(const cdb_db *db)
{
    return db->cfg.checkpoint_every ? db->cfg.checkpoint_every : DEFAULT_CHECKPOINT_EVERY;
}

/* -------------------------------------------------------- pools */

static cdb_status pool_get(cdb_db *db, uint64_t store, struct cdb_doc_pool **out)
{
    struct cdb_doc_pool *free_slot = NULL;
    cdb_store_info info;
    cdb_status s;
    int i;

    for (i = 0; i < CDB_DB_MAX_POOLS; i++) {
        struct cdb_doc_pool *p = &db->pools[i];
        if (p->in_use && p->store_id == store) {
            *out = p;
            return CDB_OK;
        }
        if (!p->in_use && free_slot == NULL)
            free_slot = p;
    }
    if (!db->has_catalog || db->cfg.doc_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    if (info.model != CDB_MODEL_DOCUMENT)
        return CDB_E_INVALID_ARG;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if (free_slot == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->store_id = store;
    free_slot->next_entry = info.next_entry_id ? info.next_entry_id : 1;
    if (info.head_root && info.rev_root) {
        if ((s = cdb_index_open(&free_slot->head, &db->pager, info.head_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        if ((s = cdb_index_open(&free_slot->revs, &db->pager, info.rev_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        free_slot->have_indexes = 1;
    }
    free_slot->in_use = 1;
    *out = free_slot;
    return CDB_OK;
}

static cdb_status pool_indexes(cdb_db *db, struct cdb_doc_pool *p)
{
    cdb_status s;
    if (p->have_indexes)
        return CDB_OK;
    if ((s = cdb_index_create(&p->head, &db->pager, p->store_id, CDB_INDEX_FLAG_UNIQUE,
                              &db->cfg.catalog_work)) != CDB_OK)
        return s;
    if ((s = cdb_index_create(&p->revs, &db->pager, p->store_id, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                              &db->cfg.catalog_work)) != CDB_OK)
        return s;
    p->have_indexes = 1;
    p->dirty = 1;
    return CDB_OK;
}

cdb_status cdb_doc_flush_pools(cdb_db *db)
{
    int i;
    for (i = 0; i < CDB_DB_MAX_POOLS; i++) {
        struct cdb_doc_pool *p = &db->pools[i];
        if (p->in_use && p->dirty) {
            cdb_status s = cdb_catalog_stage_pool(&db->catalog, p->store_id, p->head.root, p->revs.root,
                                                  p->next_entry);
            if (s != CDB_OK)
                return s;
            p->dirty = 0;
        }
    }
    return CDB_OK;
}

/* -------------------------------------------------------- index records */

typedef struct head_rec {
    uint64_t rev;
    uint64_t lid;
    uint32_t chain; /* deltas since the last full revision */
    uint8_t kind;
} head_rec;

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

static cdb_status head_get(struct cdb_doc_pool *p, uint64_t entry, head_rec *h, int *found)
{
    uint8_t k[16], v[HEAD_SIZE];
    size_t vlen;
    cdb_status s;
    *found = 0;
    if (!p->have_indexes)
        return CDB_OK;
    if ((s = cdb_index_get(&p->head, k, k_head(k, entry), v, sizeof v, &vlen, found)) != CDB_OK)
        return s;
    if (!*found)
        return CDB_OK;
    if (vlen != HEAD_SIZE)
        return CDB_E_CORRUPT;
    h->rev = cdb_load_u64(v, CDB_ORDER_BIG);
    h->lid = cdb_load_u64(v + 8, CDB_ORDER_BIG);
    h->chain = cdb_load_u32(v + 16, CDB_ORDER_BIG);
    h->kind = v[20];
    return CDB_OK;
}

static cdb_status head_put(struct cdb_doc_pool *p, uint64_t entry, const head_rec *h)
{
    uint8_t k[16], v[HEAD_SIZE];
    cdb_store_u64(v, h->rev, CDB_ORDER_BIG);
    cdb_store_u64(v + 8, h->lid, CDB_ORDER_BIG);
    cdb_store_u32(v + 16, h->chain, CDB_ORDER_BIG);
    v[20] = h->kind;
    return cdb_index_put(&p->head, k, k_head(k, entry), v, sizeof v);
}

static cdb_status rev_lid(struct cdb_doc_pool *p, uint64_t entry, uint64_t rev, uint64_t *lid)
{
    uint8_t k[16], v[8];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&p->revs, k, k_rev(k, entry, rev), v, sizeof v, &vlen, &found);
    if (s != CDB_OK)
        return s;
    if (!found || vlen != 8)
        return CDB_E_CORRUPT; /* the head said this revision exists */
    *lid = cdb_load_u64(v, CDB_ORDER_BIG);
    return CDB_OK;
}

/* -------------------------------------------------------- envelopes */

typedef struct envelope {
    uint64_t entry, rev, parent, commit, principal;
    uint32_t schema;
    uint8_t kind, flags;
    const uint8_t *summary;
    size_t summary_len;
    const uint8_t *body;
    size_t body_len;
} envelope;

/* Reads revision page lid into the page buffer and parses its envelope. */
static cdb_status read_revision(cdb_db *db, uint64_t lid, envelope *e)
{
    cdb_page_header h;
    const uint8_t *pl, *p, *end;
    cdb_byte_order o = db->pager.order;
    cdb_status s = cdb_pager_read(&db->pager, lid, buf(db, B_PAGE), &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
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
    if (e->flags & ENV_FLAG_SUMMARY) {
        cdb_value v;
        if (cdb_value_read(p, (size_t)(end - p), o, &v) != CDB_OK || v.type != CDB_TYPE_STRING)
            return CDB_E_CORRUPT;
        e->summary = v.payload;
        e->summary_len = v.payload_len;
        p += v.encoded_len;
    }
    e->body = p;
    e->body_len = (size_t)(end - p);
    return CDB_OK;
}

/* Builds a revision payload in the page buffer and writes it to a new page. */
static cdb_status write_revision(cdb_db *db, struct cdb_doc_pool *pool, const envelope *e, uint64_t *lid)
{
    uint8_t *pl = buf(db, B_PAGE);
    size_t cap = capacity(db), len = ENV_SIZE;
    cdb_byte_order o = db->pager.order;
    cdb_page_header t;
    cdb_status s;

    memset(pl, 0, ENV_SIZE);
    cdb_store_u64(pl, e->entry, o);
    cdb_store_u64(pl + 8, e->rev, o);
    cdb_store_u64(pl + 16, e->parent, o);
    cdb_store_u64(pl + 24, e->commit, o);
    cdb_store_u64(pl + 32, e->principal, o);
    cdb_store_u32(pl + 40, e->schema, o);
    pl[44] = e->kind;
    pl[45] = e->summary ? (uint8_t)ENV_FLAG_SUMMARY : 0u;
    if (e->summary) {
        cdb_value_writer w;
        cdb_value_writer_init(&w, pl + len, cap - len, o, CDB_DECIMAL_COMPACT);
        if ((s = cdb_value_write_string(&w, (const char *)e->summary, e->summary_len)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
        len += cdb_value_writer_len(&w);
    }
    if (e->body_len > cap - len)
        return CDB_E_OVERFLOW; /* documents larger than a page need overflow pages */
    if (e->body_len)
        memmove(pl + len, e->body, e->body_len);
    len += e->body_len;
    if ((s = cdb_pager_alloc(&db->pager, lid)) != CDB_OK)
        return s;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DOCUMENT;
    t.store_id = pool->store_id;
    return cdb_pager_write(&db->pager, *lid, &t, pl, len);
}

/* -------------------------------------------------------- patches */

typedef struct patch_op {
    int64_t op;
    uint64_t path[MAX_PATH];
    unsigned depth;
    const uint8_t *value; /* encoded value, for set */
    size_t value_len;
} patch_op;

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

/* Writes the chain of objects that leads to a set at a new path. */
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
 * of a path that does not exist changes nothing. */
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
            if (!handled && fid > target) {
                handled = 1;
                if (op->op == CDB_PATCH_SET && (s = write_chain(w, op, depth)) != CDB_OK)
                    return s;
            }
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
            if ((s = cdb_value_field(w, fid)) != CDB_OK || (s = cdb_value_write_encoded(w, v.start, v.encoded_len)) != CDB_OK)
                return s;
        }
    }
    if (!handled && op->op == CDB_PATCH_SET && (s = write_chain(w, op, depth)) != CDB_OK)
        return s;
    return cdb_value_end_object(w);
}

/* Applies a patch to the document in buffer *cur (length *len), alternating
 * between the two document buffers. *cur and *len follow the result. */
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
        cdb_value_writer_init(&w, buf(db, next), capacity(db), o, CDB_DECIMAL_COMPACT);
        if ((s = rewrite(&w, &doc, o, &op, 0)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
        *cur = next;
        *len = cdb_value_writer_len(&w);
    }
}

/* A patch turning document a into document b: top-level fields that differ
 * are set, and fields b lacks are removed. Written to the work buffer. */
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
    cdb_value_writer_init(&w, buf(db, B_WORK), capacity(db), o, CDB_DECIMAL_COMPACT);
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

typedef struct materialized {
    unsigned buffer;     /* B_DOC0 or B_DOC1 */
    size_t len;
    int has_summary;     /* the summary is in the summary buffer */
    size_t summary_len;
    envelope target;     /* envelope of the requested revision */
} materialized;

/* Rebuilds revision rev (0 = head): from the nearest full revision at or
 * before it, applying each patch in turn. */
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
    for (;;) {
        uint64_t lid;
        if (n >= MAX_CHAIN)
            return CDB_E_CORRUPT;
        if (cur == head.rev)
            lid = head.lid;
        else if ((s = rev_lid(pool, entry, cur, &lid)) != CDB_OK)
            return s;
        if ((s = read_revision(db, lid, &e)) != CDB_OK)
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

    /* The full revision at the base of the chain, then each patch. */
    if ((s = read_revision(db, lids[n - 1], &e)) != CDB_OK)
        return s;
    memcpy(buf(db, B_DOC0), e.body, e.body_len);
    m->buffer = B_DOC0;
    m->len = e.body_len;
    if (e.summary) {
        memcpy(buf(db, B_SUMMARY), e.summary, e.summary_len);
        m->has_summary = 1;
        m->summary_len = e.summary_len;
    }
    for (i = n - 1; i-- > 0;) {
        if ((s = read_revision(db, lids[i], &e)) != CDB_OK)
            return s;
        if (e.summary) {
            memcpy(buf(db, B_SUMMARY), e.summary, e.summary_len);
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

    if ((s = pool_get(db, store, &pool)) != CDB_OK)
        return s;
    if ((s = materialize(db, pool, entry, rev, &m)) != CDB_OK)
        return s;
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
    }
    return CDB_OK;
}

/* -------------------------------------------------------- apply */

/* Splits an insert/replace payload: flags, optional summary, body. */
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

/* Writes revision e for the entry and points the head at it. */
static cdb_status commit_revision(cdb_db *db, struct cdb_doc_pool *pool, envelope *e, uint32_t chain)
{
    uint8_t k[16], v[8];
    head_rec h;
    uint64_t lid;
    cdb_status s;
    if ((s = write_revision(db, pool, e, &lid)) != CDB_OK)
        return s;
    cdb_store_u64(v, lid, CDB_ORDER_BIG);
    if ((s = cdb_index_put(&pool->revs, k, k_rev(k, e->entry, e->rev), v, sizeof v)) != CDB_OK)
        return s;
    h.rev = e->rev;
    h.lid = lid;
    h.chain = chain;
    h.kind = e->kind;
    return head_put(pool, e->entry, &h);
}

/* The document apply. It never fails for logical reasons (a missing or
 * deleted document, a dropped pool): it runs after the commit record is
 * durable and again during recovery, so a logical failure here would make
 * the database impossible to open. Only I/O and corruption fail. */
cdb_status cdb_doc_apply(void *ctx, cdb_pager *pager, const cdb_delta *d)
{
    cdb_db *db = (cdb_db *)ctx;
    struct cdb_doc_pool *pool;
    head_rec head;
    envelope e;
    int found, live;
    cdb_status s;
    (void)pager;

    s = pool_get(db, d->store_id, &pool);
    if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG)
        return CDB_OK; /* no such document pool: nothing to change */
    if (s != CDB_OK)
        return s;
    if ((s = pool_indexes(db, pool)) != CDB_OK)
        return s;
    if (d->entry_id == 0)
        return CDB_OK;
    if (d->entry_id >= pool->next_entry) {
        pool->next_entry = d->entry_id + 1; /* replay never hands an ID out again */
        pool->dirty = 1;
    }
    if ((s = head_get(pool, d->entry_id, &head, &found)) != CDB_OK)
        return s;
    live = found && head.kind != CDB_REV_TOMBSTONE;

    memset(&e, 0, sizeof e);
    e.entry = d->entry_id;
    e.rev = found ? head.rev + 1 : 1;
    e.parent = found ? head.rev : 0;
    e.commit = d->commit_number;

    switch (d->op) {
    case CDB_OP_INSERT:
    case CDB_OP_REPLACE: {
        const uint8_t *summary, *body;
        size_t slen, blen;
        if (split_payload(d, &summary, &slen, &body, &blen) != CDB_OK)
            return CDB_OK;
        if (!live) {
            if (d->op == CDB_OP_REPLACE)
                return CDB_OK; /* nothing to replace */
            e.kind = CDB_REV_ORIGINAL;
            e.summary = summary;
            e.summary_len = slen;
            e.body = body;
            e.body_len = blen;
            return commit_revision(db, pool, &e, 0);
        }
        e.summary = summary; /* a summary changes only when one is given */
        e.summary_len = slen;
        if (head.chain + 1u >= checkpoint_every(db)) {
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
            return commit_revision(db, pool, &e, 0);
        } else {
            materialized m;
            size_t plen;
            if ((s = materialize(db, pool, d->entry_id, 0, &m)) != CDB_OK)
                return s;
            if ((s = diff(db, buf(db, m.buffer), m.len, body, blen, &plen)) != CDB_OK)
                return s;
            e.kind = CDB_REV_DELTA;
            e.body = buf(db, B_WORK);
            e.body_len = plen;
            return commit_revision(db, pool, &e, head.chain + 1u);
        }
    }
    case CDB_OP_PATCH:
        if (!live || validate_patch(d->data, d->len, db->pager.order) != CDB_OK)
            return CDB_OK;
        if (head.chain + 1u >= checkpoint_every(db)) {
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
            return commit_revision(db, pool, &e, 0);
        }
        e.kind = CDB_REV_DELTA;
        e.body = d->data;
        e.body_len = d->len;
        return commit_revision(db, pool, &e, head.chain + 1u);
    case CDB_OP_DELETE:
        if (!live)
            return CDB_OK;
        e.kind = CDB_REV_TOMBSTONE;
        return commit_revision(db, pool, &e, 0);
    default:
        return CDB_OK;
    }
}

/* -------------------------------------------------------- transactions */

static cdb_status tx_pool(cdb_db *db, uint64_t store, struct cdb_doc_pool **pool)
{
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED;
    return pool_get(db, store, pool);
}

static cdb_status put_body(cdb_db *db, uint64_t store, uint64_t entry, uint8_t op, const char *summary,
                           size_t summary_len, const void *body, size_t body_len)
{
    uint8_t *p = buf(db, B_WORK);
    size_t cap = capacity(db), len = 0, summary_value;
    cdb_value v;

    if (body == NULL || cdb_value_validate((const uint8_t *)body, body_len, db->pager.order, CDB_DECIMAL_COMPACT, 0) != CDB_OK
        || cdb_value_read((const uint8_t *)body, body_len, db->pager.order, &v) != CDB_OK || v.type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    if (summary == NULL && summary_len)
        return CDB_E_INVALID_ARG;
    /* The revision must fit in one page: envelope, summary value, body. */
    summary_value = summary ? 1u + cdb_varint_size(summary_len) + summary_len : 0;
    if (summary_len > cap || body_len > cap || ENV_SIZE + summary_value + body_len > cap)
        return CDB_E_OVERFLOW;
    p[len++] = summary ? (uint8_t)PAYLOAD_SUMMARY : 0u;
    if (summary) {
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
    return cdb_tx_put(db, store, entry, op, p, len);
}

cdb_status cdb_doc_insert(cdb_db *db, uint64_t store, const char *summary, size_t summary_len,
                          const void *body, size_t body_len, uint64_t *entry)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool)) != CDB_OK)
        return s;
    if ((s = put_body(db, store, pool->next_entry, CDB_OP_INSERT, summary, summary_len, body, body_len)) != CDB_OK)
        return s;
    *entry = pool->next_entry++;
    pool->dirty = 1;
    return CDB_OK;
}

cdb_status cdb_doc_replace(cdb_db *db, uint64_t store, uint64_t entry, const char *summary,
                           size_t summary_len, const void *body, size_t body_len)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    return put_body(db, store, entry, CDB_OP_REPLACE, summary, summary_len, body, body_len);
}

cdb_status cdb_doc_patch(cdb_db *db, uint64_t store, uint64_t entry, const void *patch, size_t patch_len)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    if (patch == NULL || validate_patch((const uint8_t *)patch, patch_len, db->pager.order) != CDB_OK)
        return CDB_E_INVALID_ARG;
    if (ENV_SIZE + patch_len > capacity(db))
        return CDB_E_OVERFLOW;
    return cdb_tx_put(db, store, entry, CDB_OP_PATCH, patch, patch_len);
}

cdb_status cdb_doc_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    struct cdb_doc_pool *pool;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= pool->next_entry)
        return CDB_E_NOT_FOUND;
    return cdb_tx_put(db, store, entry, CDB_OP_DELETE, NULL, 0);
}
