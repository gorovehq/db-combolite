/* Documents: revision chains, patches and the document apply (spec 8, 14.9). */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_schema.h"
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

enum { B_DOC0 = 0, B_DOC1 = 1, B_PAGE = 2, B_WORK = 3, B_SUMMARY = 4, B_SCHEMA = 5 };

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
    free_slot->schema_version = info.schema_version;
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

/* Whether a revision with this summary and body fits in one page. */
static int revision_fits(const cdb_db *db, int has_summary, size_t summary_len, size_t body_len)
{
    size_t cap = capacity(db), need = ENV_SIZE;
    if (has_summary) {
        if (summary_len > cap)
            return 0;
        need += 1u + cdb_varint_size(summary_len) + summary_len;
    }
    return need <= cap && body_len <= cap - need;
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
        info->schema_version = m.target.schema;
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
        if (head.chain + 1u < checkpoint_every(db)) {
            materialized m;
            size_t plen;
            if ((s = materialize(db, pool, d->entry_id, 0, &m)) != CDB_OK)
                return s;
            s = diff(db, buf(db, m.buffer), m.len, body, blen, &plen);
            if (s == CDB_OK && revision_fits(db, summary != NULL, slen, plen)) {
                e.kind = CDB_REV_DELTA;
                e.body = buf(db, B_WORK);
                e.body_len = plen;
                return commit_revision(db, pool, &e, head.chain + 1u);
            }
            if (s != CDB_OK && s != CDB_E_OVERFLOW)
                return s;
            /* The patch would not fit its page; the whole document does
             * (checked when the change was made). */
        }
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
            return commit_revision(db, pool, &e, 0);
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

/* -------------------------------------------------------- schemas */

/* Loads the pool's current schema into the schema buffer. *active is 0
 * when the pool has no schema or one that accepts every document. */
static cdb_status schema_load(cdb_db *db, struct cdb_doc_pool *pool, int *active)
{
    cdb_status s;
    *active = 0;
    if (pool->schema_version == 0)
        return CDB_OK;
    if (db->schema_version != pool->schema_version || db->schema_store != pool->store_id) {
        db->schema_version = 0;
        if ((s = cdb_catalog_get_schema(&db->catalog, pool->store_id, pool->schema_version, buf(db, B_SCHEMA),
                                        db->pager.page_size, &db->schema_len)) != CDB_OK)
            return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
        s = cdb_schema_check(buf(db, B_SCHEMA), db->schema_len, db->pager.order, &db->schema_trivial);
        if (s != CDB_OK)
            return s == CDB_E_UNSUPPORTED ? s : CDB_E_CORRUPT; /* unknown rules: a newer build wrote it */
        db->schema_store = pool->store_id;
        db->schema_version = pool->schema_version;
    }
    *active = !db->schema_trivial;
    return CDB_OK;
}

static cdb_status schema_validate(cdb_db *db, const uint8_t *doc, size_t len)
{
    return cdb_schema_validate(buf(db, B_SCHEMA), db->schema_len, doc, len, db->pager.order);
}

cdb_status cdb_doc_set_schema(cdb_db *db, uint64_t store, const void *schema, size_t len, uint32_t *version)
{
    struct cdb_doc_pool *pool;
    int trivial;
    cdb_status s;
    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = pool_get(db, store, &pool)) != CDB_OK)
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
    if ((s = pool_get(db, store, &pool)) != CDB_OK)
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
} tx_view;

static void stage_find(const cdb_db *db, uint64_t store, uint64_t entry, staged *st)
{
    const uint8_t *p = db->cfg.doc_stage;
    size_t off = 0;
    memset(st, 0, sizeof *st);
    while (off < db->stage_used) {
        const uint8_t *r = p + off;
        size_t len = cdb_load_u32(r + 16, CDB_ORDER_BIG);
        if (cdb_load_u64(r, CDB_ORDER_BIG) == store && cdb_load_u64(r + 8, CDB_ORDER_BIG) == entry) {
            st->found = 1;
            st->off = off;
            st->len = len;
            st->summary_len = cdb_load_u32(r + 20, CDB_ORDER_BIG);
            st->flags = r[24];
            return;
        }
        off += STAGE_HDR + len;
    }
}

/* Records the document's new state, replacing its old record. state is
 * never inside doc_stage. */
static void stage_put(cdb_db *db, uint64_t store, uint64_t entry, const staged *old, const tx_view *v,
                      const uint8_t *state)
{
    uint8_t *p = db->cfg.doc_stage, *r;
    size_t avail, len = v->live ? v->len : 0u;
    if (old->found) {
        size_t size = STAGE_HDR + old->len;
        memmove(p + old->off, p + old->off + size, db->stage_used - old->off - size);
        db->stage_used -= size;
    }
    avail = p ? db->cfg.doc_stage_size - db->stage_used : 0u;
    if (avail < STAGE_HDR || len > avail - STAGE_HDR) {
        db->stage_overflow = 1;
        return;
    }
    r = p + db->stage_used;
    memset(r, 0, STAGE_HDR);
    cdb_store_u64(r, store, CDB_ORDER_BIG);
    cdb_store_u64(r + 8, entry, CDB_ORDER_BIG);
    cdb_store_u32(r + 16, (uint32_t)len, CDB_ORDER_BIG);
    cdb_store_u32(r + 20, (uint32_t)v->summary_len, CDB_ORDER_BIG);
    r[24] = (uint8_t)((v->live ? 0u : STAGE_DELETED) | (v->has_summary ? STAGE_SUMMARY : 0u));
    if (len)
        memcpy(r + STAGE_HDR, state, len);
    db->stage_used += STAGE_HDR + len;
}

/* The committed document with the open transaction's journaled changes to
 * it applied in order, as the document apply will apply them. With
 * need_state, a live document's body ends up in document buffer v->b. */
static cdb_status replay_tx(cdb_db *db, struct cdb_doc_pool *pool, uint64_t entry, int need_state, tx_view *v)
{
    uint64_t off;
    cdb_status s;

    memset(v, 0, sizeof *v);
    if (need_state) {
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
    for (off = db->tx_start; off < db->journal.end;) {
        cdb_record r;
        cdb_delta d;
        uint64_t next;
        if ((s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &r, &next)) != CDB_OK)
            return s;
        off = next;
        if (r.kind != CDB_RECORD_DATA || r.tx_id != db->tx_id || r.store_id != pool->store_id
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
            }
            break;
        }
        case CDB_OP_PATCH:
            if (v->live && need_state && (s = apply_patch(db, &v->b, &v->len, d.data, d.len)) != CDB_OK)
                return s;
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
    }
    return CDB_OK;
}

/* -------------------------------------------------------- transactions */

static cdb_status tx_pool(cdb_db *db, uint64_t store, struct cdb_doc_pool **pool, int *guarded)
{
    cdb_status s;
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED;
    if ((s = pool_get(db, store, pool)) != CDB_OK)
        return s;
    return schema_load(db, *pool, guarded);
}

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
    if (summary_len > cap || body_len > cap)
        return CDB_E_OVERFLOW;
    stage_find(db, pool->store_id, entry, &st);
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
    /* The revision must fit in one page, with the summary it will carry. */
    if (!revision_fits(db, now.has_summary, now.summary_len, body_len))
        return CDB_E_OVERFLOW;
    if (guarded && (s = schema_validate(db, (const uint8_t *)body, body_len)) != CDB_OK)
        return s;
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
    if ((s = cdb_tx_put(db, pool->store_id, entry, op, p, len)) != CDB_OK)
        return s;
    stage_put(db, pool->store_id, entry, &st, &now, (const uint8_t *)body);
    return CDB_OK;
}

cdb_status cdb_doc_insert(cdb_db *db, uint64_t store, const char *summary, size_t summary_len,
                          const void *body, size_t body_len, uint64_t *entry)
{
    struct cdb_doc_pool *pool;
    int guarded;
    cdb_status s;
    if ((s = tx_pool(db, store, &pool, &guarded)) != CDB_OK)
        return s;
    if ((s = put_body(db, pool, guarded, pool->next_entry, CDB_OP_INSERT, summary, summary_len, body,
                      body_len)) != CDB_OK)
        return s;
    *entry = pool->next_entry++;
    pool->dirty = 1;
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
    if (ENV_SIZE + patch_len > capacity(db))
        return CDB_E_OVERFLOW;
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
    if ((s = cdb_tx_put(db, store, entry, CDB_OP_DELETE, NULL, 0)) != CDB_OK)
        return s;
    memset(&now, 0, sizeof now);
    stage_put(db, store, entry, &st, &now, NULL);
    return CDB_OK;
}
