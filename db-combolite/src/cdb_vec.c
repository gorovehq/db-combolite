/* Vector collections (spec 7, 14.12). */
#include <math.h>
#include <string.h>

#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_varint.h"
#include "cdb_vec.h"

/* Vector block (spec 14.12.1): store ID (8), vector count (4), element
 * type (1), flags (1), dimensions (2); entry IDs (8 each); cached norms
 * (f32 each) when flagged; vector data from a 16-byte boundary of the page. */
#define BLOCK_HDR 16u
#define BLOCK_NORMS 0x01u

#define ENTRY_VAL 36u   /* block (8), slot (4), source store (8), source entry (8), source revision (8) */
#define BLOCKS_VAL 8u   /* slots in use (4), slots (4) */

/* Payload flag: a source follows. */
#define PAYLOAD_SOURCE 0x01u

/* Settings fields (spec 14.12.3), kept in store record field 16. */
enum { S_ELEM = 1, S_DIMS, S_MEASURE, S_MODEL, S_VERSION, S_ENTRIES, S_BLOCKS, S_SOURCES, S_NORMS };

typedef struct cdb_vec_coll coll;

static uint8_t *vbuf(cdb_db *db, unsigned i)
{
    return db->cfg.vec_buffers + (size_t)i * db->pager.page_size;
}

enum { V_BLOCK = 0, V_BUILD = 1, V_CURSOR = 2 };

/* -------------------------------------------------------- layout */

static size_t vector_bytes(unsigned elem, uint32_t dims)
{
    switch (elem) {
    case CDB_VEC_F32: return (size_t)dims * 4u;
    case CDB_VEC_F16:
    case CDB_VEC_BF16: return (size_t)dims * 2u;
    case CDB_VEC_I8: return dims;
    case CDB_VEC_BINARY: return ((size_t)dims + 7u) / 8u;
    default: return 0;
    }
}

/* Offset of the vector data in a block of count vectors: the first 16-byte
 * boundary of the page after the entry IDs and norms. */
static size_t data_off(const cdb_db *db, const coll *c, uint32_t count)
{
    size_t h = cdb_page_header_size(db->pager.fmt);
    size_t end = h + BLOCK_HDR + (size_t)count * (8u + (c->norms ? 4u : 0u));
    return ((end + 15u) & ~(size_t)15u) - h;
}

static size_t block_len(const cdb_db *db, const coll *c, uint32_t count)
{
    return data_off(db, c, count) + (size_t)count * c->vec_bytes;
}

static uint32_t block_slots(const cdb_db *db, const coll *c)
{
    size_t cap = cdb_page_capacity(db->pager.fmt, db->pager.page_size);
    size_t n = c->vec_bytes ? cap / c->vec_bytes : 0;
    if (n > 0xFFFFFFu)
        n = 0xFFFFFFu;
    while (n > 0 && block_len(db, c, (uint32_t)n) > cap)
        n--;
    return (uint32_t)n;
}

static int measure_fits(unsigned elem, unsigned measure)
{
    if (elem == CDB_VEC_BINARY)
        return measure == CDB_VEC_HAMMING;
    return measure >= CDB_VEC_COSINE && measure <= CDB_VEC_EUCLIDEAN;
}

/* -------------------------------------------------------- settings */

static cdb_status encode_settings(const cdb_db *db, const cdb_vec_config *cfg, const coll *c,
                                  uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, out, cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, S_ELEM);
    cdb_value_write_int(&w, (int64_t)cfg->elem);
    cdb_value_field(&w, S_DIMS);
    cdb_value_write_int(&w, (int64_t)cfg->dims);
    cdb_value_field(&w, S_MEASURE);
    cdb_value_write_int(&w, (int64_t)cfg->measure);
    cdb_value_field(&w, S_MODEL);
    cdb_value_write_string(&w, cfg->model, cfg->model_len);
    cdb_value_field(&w, S_VERSION);
    cdb_value_write_string(&w, cfg->model_version, cfg->model_version_len);
    cdb_value_field(&w, S_ENTRIES);
    cdb_value_write_int(&w, (int64_t)c->entries.root);
    cdb_value_field(&w, S_BLOCKS);
    cdb_value_write_int(&w, (int64_t)c->blocks.root);
    cdb_value_field(&w, S_SOURCES);
    cdb_value_write_int(&w, (int64_t)c->sources.root);
    cdb_value_field(&w, S_NORMS);
    cdb_value_write_bool(&w, c->norms != 0);
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

static cdb_status get_int(const cdb_value *obj, cdb_byte_order o, uint64_t f, int64_t lo, int64_t hi, int64_t *out)
{
    cdb_value v;
    int found;
    if (cdb_value_object_get(obj, o, f, &v, &found) != CDB_OK || !found || v.type != CDB_TYPE_INT
        || v.as.i64 < lo || v.as.i64 > hi)
        return CDB_E_CORRUPT;
    *out = v.as.i64;
    return CDB_OK;
}

static cdb_status get_text(const cdb_value *obj, cdb_byte_order o, uint64_t f, char *out, size_t *len)
{
    cdb_value v;
    int found;
    if (cdb_value_object_get(obj, o, f, &v, &found) != CDB_OK || !found || v.type != CDB_TYPE_STRING
        || v.payload_len > CDB_VEC_MODEL_MAX)
        return CDB_E_CORRUPT;
    if (out)
        memcpy(out, v.payload, v.payload_len);
    *len = v.payload_len;
    return CDB_OK;
}

/* Decodes settings into c (roots in *roots) and, optionally, cfg with its
 * model strings copied to model and version. */
static cdb_status decode_settings(const cdb_db *db, const uint8_t *in, size_t len, coll *c, uint64_t roots[3],
                                  cdb_vec_config *cfg, char *model, char *version)
{
    cdb_byte_order o = db->pager.order;
    cdb_value obj, v;
    int64_t elem, dims, measure, r[3];
    size_t mlen, vlen;
    int found, i;

    if (cdb_value_read(in, len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if (get_int(&obj, o, S_ELEM, CDB_VEC_F32, CDB_VEC_BINARY, &elem) != CDB_OK
        || get_int(&obj, o, S_DIMS, 1, CDB_VEC_MAX_DIMS, &dims) != CDB_OK
        || get_int(&obj, o, S_MEASURE, CDB_VEC_COSINE, CDB_VEC_HAMMING, &measure) != CDB_OK
        || get_text(&obj, o, S_MODEL, model, &mlen) != CDB_OK
        || get_text(&obj, o, S_VERSION, version, &vlen) != CDB_OK)
        return CDB_E_CORRUPT;
    for (i = 0; i < 3; i++)
        if (get_int(&obj, o, (uint64_t)S_ENTRIES + (uint64_t)i, 1, INT64_MAX, &r[i]) != CDB_OK)
            return CDB_E_CORRUPT;
    if (cdb_value_object_get(&obj, o, S_NORMS, &v, &found) != CDB_OK || !found || v.type != CDB_TYPE_BOOL)
        return CDB_E_CORRUPT;
    if (!measure_fits((unsigned)elem, (unsigned)measure))
        return CDB_E_CORRUPT;
    c->elem = (uint8_t)elem;
    c->dims = (uint32_t)dims;
    c->measure = (uint8_t)measure;
    c->norms = (uint8_t)(v.as.boolean != 0);
    c->vec_bytes = vector_bytes(c->elem, c->dims);
    for (i = 0; i < 3; i++)
        roots[i] = (uint64_t)r[i];
    if (cfg) {
        memset(cfg, 0, sizeof *cfg);
        cfg->elem = (cdb_vector_elem)elem;
        cfg->dims = (uint32_t)dims;
        cfg->measure = (unsigned)measure;
        cfg->model = model;
        cfg->model_len = mlen;
        cfg->model_version = version;
        cfg->model_version_len = vlen;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- collections */

int cdb_vec_is_open(const cdb_db *db, uint64_t store)
{
    int i;
    for (i = 0; i < CDB_DB_MAX_POOLS; i++)
        if (db->vecs[i].in_use && db->vecs[i].store_id == store)
            return 1;
    return 0;
}

/* CDB_E_INVALID_ARG: not a vector collection; CDB_E_NOT_FOUND: no such
 * store, or it was dropped. */
static cdb_status coll_get(cdb_db *db, uint64_t store, coll **out)
{
    coll *free_slot = NULL;
    cdb_store_info info;
    uint64_t roots[3];
    cdb_status s;
    int i;

    for (i = 0; i < CDB_DB_MAX_POOLS; i++) {
        coll *c = &db->vecs[i];
        if (c->in_use && c->store_id == store) {
            *out = c;
            return CDB_OK;
        }
        if (!c->in_use && free_slot == NULL)
            free_slot = c;
    }
    if (!db->has_catalog)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    if (info.model != CDB_MODEL_VECTOR)
        return CDB_E_INVALID_ARG;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if (db->cfg.vec_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if (info.vector_len == 0)
        return CDB_E_CORRUPT;
    if (free_slot == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    memset(free_slot, 0, sizeof *free_slot);
    if ((s = decode_settings(db, info.vector, info.vector_len, free_slot, roots, NULL, NULL, NULL)) != CDB_OK)
        return s;
    free_slot->store_id = store;
    free_slot->slots = block_slots(db, free_slot);
    if (free_slot->slots == 0)
        return CDB_E_CORRUPT;
    free_slot->next_entry = info.next_entry_id ? info.next_entry_id : 1;
    free_slot->touched_tx = UINT64_MAX;
    if ((s = cdb_index_open(&free_slot->entries, &db->pager, roots[0], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->blocks, &db->pager, roots[1], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->sources, &db->pager, roots[2], &db->cfg.catalog_work)) != CDB_OK)
        return s;
    free_slot->in_use = 1;
    *out = free_slot;
    return CDB_OK;
}

cdb_status cdb_vec_flush(cdb_db *db)
{
    int i;
    for (i = 0; i < CDB_DB_MAX_POOLS; i++) {
        coll *c = &db->vecs[i];
        if (c->in_use && c->dirty) {
            cdb_status s = cdb_catalog_stage_next_entry(&db->catalog, c->store_id, c->next_entry);
            if (s != CDB_OK)
                return s;
            c->dirty = 0;
        }
    }
    return CDB_OK;
}

cdb_status cdb_vec_create(cdb_db *db, const char *name, size_t len, const cdb_vec_config *cfg, uint64_t *id)
{
    uint8_t settings[CDB_CATALOG_SETTINGS_MAX];
    size_t slen;
    uint64_t existing, got;
    int found;
    coll c;
    cdb_status s;

    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if (!db->has_catalog || db->cfg.vec_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if (cfg == NULL || cfg->elem > CDB_VEC_BINARY || cfg->dims == 0 || cfg->dims > CDB_VEC_MAX_DIMS
        || !measure_fits(cfg->elem, cfg->measure) || cfg->model == NULL || cfg->model_len == 0
        || cfg->model_len > CDB_VEC_MODEL_MAX || cfg->model_version == NULL || cfg->model_version_len == 0
        || cfg->model_version_len > CDB_VEC_MODEL_MAX)
        return CDB_E_INVALID_ARG;
    memset(&c, 0, sizeof c);
    c.elem = (uint8_t)cfg->elem;
    c.dims = cfg->dims;
    c.measure = (uint8_t)cfg->measure;
    c.norms = cfg->measure == CDB_VEC_COSINE;
    c.vec_bytes = vector_bytes(c.elem, c.dims);
    if (block_slots(db, &c) == 0)
        return CDB_E_OVERFLOW; /* one vector does not fit in a page */
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    /* Nothing is written for a name that is taken. */
    if ((s = cdb_catalog_find_store(&db->catalog, name, len, &existing, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;
    /* The indexes are created first, for the ID the store will get; the
     * store record names them, and one commit makes it all durable. */
    *id = cdb_catalog_next_store_id(&db->catalog);
    s = cdb_index_create(&c.entries, &db->pager, *id, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&c.blocks, &db->pager, *id, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&c.sources, &db->pager, *id, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                             &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = encode_settings(db, cfg, &c, settings, sizeof settings, &slen);
    if (s == CDB_OK)
        s = cdb_catalog_create_store_ex(&db->catalog, name, len, CDB_MODEL_VECTOR, settings, slen, &got);
    if (s == CDB_OK && got != *id)
        s = CDB_E_CORRUPT;
    return cdb_db_catalog_end(db, s);
}

cdb_status cdb_vec_describe(cdb_db *db, uint64_t store, cdb_vec_config *cfg, char *model, char *model_version)
{
    cdb_store_info info;
    uint64_t roots[3];
    coll *c, tmp;
    cdb_status s;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    return decode_settings(db, info.vector, info.vector_len, &tmp, roots, cfg, model, model_version);
}

/* -------------------------------------------------------- elements */

static float half_to_float(uint16_t h)
{
    unsigned e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)
        v = ldexpf((float)m, -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = ldexpf((float)(m | 0x400u), (int)e - 25);
    return (h & 0x8000u) ? -v : v;
}

static double elem_at(const uint8_t *d, size_t i, unsigned elem, cdb_byte_order o)
{
    switch (elem) {
    case CDB_VEC_F32: {
        uint32_t bits = cdb_load_u32(d + i * 4u, o);
        float f;
        memcpy(&f, &bits, sizeof f);
        return f;
    }
    case CDB_VEC_F16:
        return half_to_float(cdb_load_u16(d + i * 2u, o));
    case CDB_VEC_BF16: {
        uint32_t bits = (uint32_t)cdb_load_u16(d + i * 2u, o) << 16;
        float f;
        memcpy(&f, &bits, sizeof f);
        return f;
    }
    default:
        return (double)(int8_t)d[i];
    }
}

static double norm_of(const coll *c, const uint8_t *v, cdb_byte_order o)
{
    double sum = 0.0;
    uint32_t i;
    for (i = 0; i < c->dims; i++) {
        double x = elem_at(v, i, c->elem, o);
        sum += x * x;
    }
    return sqrt(sum);
}

static unsigned popcount8(uint8_t b)
{
    unsigned n = 0;
    while (b) {
        b &= (uint8_t)(b - 1u);
        n++;
    }
    return n;
}

/* Distance between query q (norm qn) and stored vector v (norm vn, or a
 * negative value when not cached). */
static double distance(const coll *c, unsigned measure, const uint8_t *q, double qn, const uint8_t *v, double vn,
                       cdb_byte_order o)
{
    uint32_t i;
    if (measure == CDB_VEC_HAMMING) {
        size_t n = c->vec_bytes;
        unsigned bits = 0;
        for (i = 0; i < n; i++) {
            uint8_t x = (uint8_t)(q[i] ^ v[i]);
            if (i + 1u == n && c->dims % 8u)
                x &= (uint8_t)((1u << (c->dims % 8u)) - 1u);
            bits += popcount8(x);
        }
        return (double)bits;
    }
    if (measure == CDB_VEC_EUCLIDEAN) {
        double sum = 0.0;
        for (i = 0; i < c->dims; i++) {
            double d = elem_at(q, i, c->elem, o) - elem_at(v, i, c->elem, o);
            sum += d * d;
        }
        return sqrt(sum);
    }
    {
        double dot = 0.0;
        for (i = 0; i < c->dims; i++)
            dot += elem_at(q, i, c->elem, o) * elem_at(v, i, c->elem, o);
        if (measure == CDB_VEC_DOT)
            return -dot;
        if (vn < 0.0)
            vn = norm_of(c, v, o);
        if (qn == 0.0 || vn == 0.0)
            return 1.0; /* a zero vector is similar to nothing */
        return 1.0 - dot / (qn * vn);
    }
}

/* Checks that an encoded value is a vector of the collection's shape; *data
 * points at its elements. */
static cdb_status vector_of(const cdb_db *db, const coll *c, const uint8_t *in, size_t len, int full,
                            const uint8_t **data)
{
    cdb_value v;
    if (in == NULL)
        return CDB_E_INVALID_ARG;
    if (full && cdb_value_validate(in, len, db->pager.order, CDB_DECIMAL_COMPACT, 0) != CDB_OK)
        return CDB_E_INVALID_ARG;
    if (cdb_value_read(in, len, db->pager.order, &v) != CDB_OK || v.encoded_len != len || v.type != CDB_TYPE_VECTOR
        || v.as.vector.elem != (cdb_vector_elem)c->elem || v.as.vector.dims != c->dims
        || v.as.vector.data_len != c->vec_bytes)
        return CDB_E_INVALID_ARG;
    *data = v.as.vector.data;
    return CDB_OK;
}

/* -------------------------------------------------------- index records */

typedef struct entry_rec {
    uint64_t block;
    uint32_t slot;
    uint64_t source_store;
    uint64_t source_entry;
    uint64_t source_rev;   /* the document's revision when the vector was written */
} entry_rec;

static size_t k_u64(uint8_t *k, uint64_t a)
{
    cdb_store_u64(k, a, CDB_ORDER_BIG);
    return 8;
}

static size_t k_source(uint8_t *k, uint64_t store, uint64_t entry, uint64_t vec_entry)
{
    cdb_store_u64(k, store, CDB_ORDER_BIG);
    cdb_store_u64(k + 8, entry, CDB_ORDER_BIG);
    cdb_store_u64(k + 16, vec_entry, CDB_ORDER_BIG);
    return 24;
}

static cdb_status entry_get(coll *c, uint64_t entry, entry_rec *r, int *found)
{
    uint8_t k[8], v[ENTRY_VAL];
    size_t vlen;
    cdb_status s = cdb_index_get(&c->entries, k, k_u64(k, entry), v, sizeof v, &vlen, found);
    if (s != CDB_OK || !*found)
        return s;
    if (vlen != ENTRY_VAL)
        return CDB_E_CORRUPT;
    r->block = cdb_load_u64(v, CDB_ORDER_BIG);
    r->slot = cdb_load_u32(v + 8, CDB_ORDER_BIG);
    r->source_store = cdb_load_u64(v + 12, CDB_ORDER_BIG);
    r->source_entry = cdb_load_u64(v + 20, CDB_ORDER_BIG);
    r->source_rev = cdb_load_u64(v + 28, CDB_ORDER_BIG);
    return CDB_OK;
}

static cdb_status entry_put(coll *c, uint64_t entry, const entry_rec *r)
{
    uint8_t k[8], v[ENTRY_VAL];
    cdb_store_u64(v, r->block, CDB_ORDER_BIG);
    cdb_store_u32(v + 8, r->slot, CDB_ORDER_BIG);
    cdb_store_u64(v + 12, r->source_store, CDB_ORDER_BIG);
    cdb_store_u64(v + 20, r->source_entry, CDB_ORDER_BIG);
    cdb_store_u64(v + 28, r->source_rev, CDB_ORDER_BIG);
    return cdb_index_put(&c->entries, k, k_u64(k, entry), v, sizeof v);
}

static cdb_status blocks_put(coll *c, uint64_t block, uint32_t used, uint32_t count)
{
    uint8_t k[8], v[BLOCKS_VAL];
    cdb_store_u32(v, used, CDB_ORDER_BIG);
    cdb_store_u32(v + 4, count, CDB_ORDER_BIG);
    return cdb_index_put(&c->blocks, k, k_u64(k, block), v, sizeof v);
}

static cdb_status blocks_get(coll *c, uint64_t block, uint32_t *used, uint32_t *count)
{
    uint8_t k[8], v[BLOCKS_VAL];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&c->blocks, k, k_u64(k, block), v, sizeof v, &vlen, &found);
    if (s != CDB_OK)
        return s;
    if (!found || vlen != BLOCKS_VAL)
        return CDB_E_CORRUPT;
    *used = cdb_load_u32(v, CDB_ORDER_BIG);
    *count = cdb_load_u32(v + 4, CDB_ORDER_BIG);
    return CDB_OK;
}

/* -------------------------------------------------------- blocks */

typedef struct block {
    const uint8_t *p;   /* payload */
    uint32_t count;
} block;

static cdb_status block_read(cdb_db *db, const coll *c, uint64_t lid, block *b)
{
    cdb_page_header h;
    cdb_status s = cdb_pager_read(&db->pager, lid, vbuf(db, V_BLOCK), &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    b->p = vbuf(db, V_BLOCK) + cdb_page_header_size(db->pager.fmt);
    if (h.type != CDB_PAGE_TYPE_VECTOR || h.payload_len < BLOCK_HDR)
        return CDB_E_CORRUPT;
    b->count = cdb_load_u32(b->p + 8, db->pager.order);
    if (cdb_load_u64(b->p, db->pager.order) != c->store_id || b->p[12] != c->elem
        || cdb_load_u16(b->p + 14, db->pager.order) != (uint16_t)c->dims || b->count > c->slots
        || h.payload_len != block_len(db, c, b->count))
        return CDB_E_CORRUPT;
    return CDB_OK;
}

static uint64_t slot_entry(const cdb_db *db, const block *b, uint32_t i)
{
    return cdb_load_u64(b->p + BLOCK_HDR + (size_t)i * 8u, db->pager.order);
}

static const uint8_t *slot_data(const cdb_db *db, const coll *c, const block *b, uint32_t i)
{
    return b->p + data_off(db, c, b->count) + (size_t)i * c->vec_bytes;
}

static double slot_norm(const cdb_db *db, const coll *c, const block *b, uint32_t i)
{
    uint32_t bits;
    float f;
    if (!c->norms)
        return -1.0;
    bits = cdb_load_u32(b->p + BLOCK_HDR + (size_t)b->count * 8u + (size_t)i * 4u, db->pager.order);
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* Writes block lid with count slots: the slots of old (NULL: none) that
 * remain, with slot set to (entry, data) or emptied when data is NULL. */
static cdb_status block_write(cdb_db *db, const coll *c, uint64_t lid, const block *old, uint32_t count,
                              uint32_t slot, uint64_t entry, const uint8_t *data)
{
    cdb_byte_order o = db->pager.order;
    uint8_t *nb = vbuf(db, V_BUILD);
    size_t len = block_len(db, c, count), doff = data_off(db, c, count), noff = BLOCK_HDR + (size_t)count * 8u;
    cdb_page_header t;
    uint32_t i;

    memset(nb, 0, len);
    cdb_store_u64(nb, c->store_id, o);
    cdb_store_u32(nb + 8, count, o);
    nb[12] = c->elem;
    nb[13] = c->norms ? (uint8_t)BLOCK_NORMS : 0u;
    cdb_store_u16(nb + 14, (uint16_t)c->dims, o);
    for (i = 0; old && i < count && i < old->count; i++) {
        memcpy(nb + BLOCK_HDR + (size_t)i * 8u, old->p + BLOCK_HDR + (size_t)i * 8u, 8);
        if (c->norms)
            memcpy(nb + noff + (size_t)i * 4u, old->p + BLOCK_HDR + (size_t)old->count * 8u + (size_t)i * 4u, 4);
        memcpy(nb + doff + (size_t)i * c->vec_bytes, slot_data(db, c, old, i), c->vec_bytes);
    }
    if (slot < count) {
        uint8_t *e = nb + BLOCK_HDR + (size_t)slot * 8u, *d = nb + doff + (size_t)slot * c->vec_bytes;
        cdb_store_u64(e, data ? entry : 0u, o);
        if (data)
            memcpy(d, data, c->vec_bytes);
        else
            memset(d, 0, c->vec_bytes);
        if (c->norms) {
            float f = data ? (float)norm_of(c, data, o) : 0.0f;
            uint32_t bits;
            memcpy(&bits, &f, sizeof bits);
            cdb_store_u32(nb + noff + (size_t)slot * 4u, bits, o);
        }
    }
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_VECTOR;
    t.store_id = c->store_id;
    return cdb_pager_write(&db->pager, lid, &t, nb, len);
}

/* Puts a new vector in the first block with a free slot, or a new block. */
static cdb_status place(cdb_db *db, coll *c, uint64_t entry, const uint8_t *data, entry_rec *r)
{
    cdb_index_cursor cur;
    uint8_t k[8], v[BLOCKS_VAL];
    size_t klen, vlen;
    int done;
    cdb_status s;

    if ((s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
        return s;
    for (;;) {
        uint32_t used, count, slot;
        block b;
        if ((s = cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        if (klen != 8 || vlen != BLOCKS_VAL)
            return CDB_E_CORRUPT;
        used = cdb_load_u32(v, CDB_ORDER_BIG);
        count = cdb_load_u32(v + 4, CDB_ORDER_BIG);
        if (used >= c->slots)
            continue;
        r->block = cdb_load_u64(k, CDB_ORDER_BIG);
        if ((s = block_read(db, c, r->block, &b)) != CDB_OK)
            return s;
        if (b.count != count)
            return CDB_E_CORRUPT;
        for (slot = 0; slot < b.count && slot_entry(db, &b, slot) != 0; slot++)
            ;
        r->slot = slot;
        count = slot < b.count ? b.count : slot + 1u;
        if ((s = block_write(db, c, r->block, &b, count, slot, entry, data)) != CDB_OK)
            return s;
        return blocks_put(c, r->block, used + 1u, count);
    }
    if ((s = cdb_pager_alloc(&db->pager, &r->block)) != CDB_OK)
        return s;
    r->slot = 0;
    if ((s = block_write(db, c, r->block, NULL, 1, 0, entry, data)) != CDB_OK)
        return s;
    return blocks_put(c, r->block, 1, 1);
}

/* Empties the entry's slot; a block left empty is released. */
static cdb_status unplace(cdb_db *db, coll *c, uint64_t entry, const entry_rec *r)
{
    uint32_t used, count;
    block b;
    cdb_status s;
    if ((s = blocks_get(c, r->block, &used, &count)) != CDB_OK || (s = block_read(db, c, r->block, &b)) != CDB_OK)
        return s;
    if (r->slot >= b.count || slot_entry(db, &b, r->slot) != entry || used == 0)
        return CDB_E_CORRUPT;
    if (used == 1) {
        int found;
        if ((s = cdb_pager_free(&db->pager, r->block)) != CDB_OK)
            return s;
        {
            uint8_t k[8];
            return cdb_index_delete(&c->blocks, k, k_u64(k, r->block), &found);
        }
    }
    /* Trailing empty slots are dropped, so blocks stay dense. */
    count = b.count;
    while (count > 0 && (count - 1u == r->slot || slot_entry(db, &b, count - 1u) == 0))
        count--;
    if ((s = block_write(db, c, r->block, &b, count, r->slot, 0, NULL)) != CDB_OK)
        return s;
    return blocks_put(c, r->block, used - 1u, count);
}

/* -------------------------------------------------------- apply */

static cdb_status parse_payload(const cdb_db *db, const coll *c, const cdb_delta *d, int *has_source,
                                uint64_t *ss, uint64_t *se, const uint8_t **data)
{
    const uint8_t *p = d->data, *end = d->data + d->len;
    size_t n;
    *has_source = 0;
    *ss = *se = 0;
    if (d->len < 1)
        return CDB_E_MALFORMED;
    if (*p++ & PAYLOAD_SOURCE) {
        if (cdb_varint_decode(p, (size_t)(end - p), ss, &n) != CDB_OK)
            return CDB_E_MALFORMED;
        p += n;
        if (cdb_varint_decode(p, (size_t)(end - p), se, &n) != CDB_OK)
            return CDB_E_MALFORMED;
        p += n;
        *has_source = 1;
    }
    return vector_of(db, c, p, (size_t)(end - p), 0, data) == CDB_OK ? CDB_OK : CDB_E_MALFORMED;
}

/* The source document's head revision now; 0 when the source is not a
 * document. Changes are applied in commit order, so replay sees the same. */
static cdb_status source_rev(cdb_db *db, uint64_t store, uint64_t entry, uint64_t *rev)
{
    int live;
    cdb_status s = cdb_doc_head(db, store, entry, rev, &live);
    if (s == CDB_E_INVALID_ARG || s == CDB_E_NOT_FOUND || s == CDB_E_UNSUPPORTED) {
        *rev = 0;
        return CDB_OK;
    }
    return s;
}

/* The vector apply. Like the document apply it never fails for logical
 * reasons: it runs again during recovery. */
cdb_status cdb_vec_apply(cdb_db *db, const cdb_delta *d, int *handled)
{
    uint8_t k[24];
    entry_rec r;
    coll *c;
    int found;
    cdb_status s;

    *handled = 0;
    s = coll_get(db, d->store_id, &c);
    if (s == CDB_E_INVALID_ARG)
        return CDB_OK; /* not a vector collection */
    *handled = 1;
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    if (d->entry_id == 0)
        return CDB_OK;
    if (d->entry_id >= c->next_entry) {
        c->next_entry = d->entry_id + 1u; /* replay never hands an ID out again */
        c->dirty = 1;
    }
    if ((s = entry_get(c, d->entry_id, &r, &found)) != CDB_OK)
        return s;

    switch (d->op) {
    case CDB_OP_INSERT:
    case CDB_OP_REPLACE: {
        const uint8_t *data;
        uint64_t ss, se;
        int has_source;
        if (parse_payload(db, c, d, &has_source, &ss, &se, &data) != CDB_OK)
            return CDB_OK;
        if (!found) {
            if (d->op == CDB_OP_REPLACE)
                return CDB_OK; /* nothing to replace */
            if ((s = place(db, c, d->entry_id, data, &r)) != CDB_OK)
                return s;
            r.source_store = has_source ? ss : 0u;
            r.source_entry = has_source ? se : 0u;
            r.source_rev = 0;
            if (r.source_store && (s = source_rev(db, ss, se, &r.source_rev)) != CDB_OK)
                return s;
            if (r.source_store
                && (s = cdb_index_put(&c->sources, k, k_source(k, ss, se, d->entry_id), k, 0)) != CDB_OK)
                return s;
            return entry_put(c, d->entry_id, &r);
        } else {
            uint32_t used, count;
            block b;
            if ((s = blocks_get(c, r.block, &used, &count)) != CDB_OK || (s = block_read(db, c, r.block, &b)) != CDB_OK)
                return s;
            if (r.slot >= b.count || slot_entry(db, &b, r.slot) != d->entry_id)
                return CDB_E_CORRUPT;
            if ((s = block_write(db, c, r.block, &b, b.count, r.slot, d->entry_id, data)) != CDB_OK)
                return s;
            if (r.source_store == 0)
                return CDB_OK;
            /* The source stays; the vector now describes its current revision. */
            if ((s = source_rev(db, r.source_store, r.source_entry, &r.source_rev)) != CDB_OK)
                return s;
            return entry_put(c, d->entry_id, &r);
        }
    }
    case CDB_OP_DELETE:
        if (!found)
            return CDB_OK;
        if ((s = unplace(db, c, d->entry_id, &r)) != CDB_OK)
            return s;
        if (r.source_store
            && (s = cdb_index_delete(&c->sources, k, k_source(k, r.source_store, r.source_entry, d->entry_id),
                                     &found)) != CDB_OK)
            return s;
        return cdb_index_delete(&c->entries, k, k_u64(k, d->entry_id), &found);
    default:
        return CDB_OK;
    }
}

/* -------------------------------------------------------- transactions */

static cdb_status tx_coll(cdb_db *db, uint64_t store, coll **c)
{
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED;
    return coll_get(db, store, c);
}

/* Whether the entry exists as the open transaction sees it: committed, then
 * the transaction's own changes to this collection, if it made any. */
static cdb_status live_in_tx(cdb_db *db, coll *c, uint64_t entry, int *live)
{
    entry_rec r;
    uint64_t off;
    cdb_status s = entry_get(c, entry, &r, live);
    if (s != CDB_OK || c->touched_tx != db->tx_id)
        return s;
    for (off = db->tx_start; off < db->journal.end;) {
        cdb_record rec;
        uint64_t next;
        if ((s = cdb_journal_read(&db->journal, off, db->cfg.record_buf, db->cfg.record_buf_size, &rec, &next)) != CDB_OK)
            return s;
        off = next;
        if (rec.kind != CDB_RECORD_DATA || rec.tx_id != db->tx_id || rec.store_id != c->store_id
            || rec.entry_id != entry || rec.payload_len < 1)
            continue;
        if (rec.payload[0] == CDB_OP_INSERT)
            *live = 1;
        else if (rec.payload[0] == CDB_OP_DELETE)
            *live = 0;
    }
    return CDB_OK;
}

static cdb_status put_vector(cdb_db *db, coll *c, uint64_t entry, uint8_t op, const void *vec, size_t len,
                             uint64_t ss, uint64_t se)
{
    uint8_t *p = vbuf(db, V_BUILD);
    const uint8_t *data;
    size_t n = 0;
    cdb_status s;
    if ((s = vector_of(db, c, (const uint8_t *)vec, len, 1, &data)) != CDB_OK)
        return s;
    if (len + 1u + 2u * CDB_VARINT_MAX_LEN > db->pager.page_size)
        return CDB_E_OVERFLOW;
    p[n++] = ss ? (uint8_t)PAYLOAD_SOURCE : 0u;
    if (ss) {
        n += cdb_varint_encode(ss, p + n);
        n += cdb_varint_encode(se, p + n);
    }
    memcpy(p + n, vec, len);
    n += len;
    if ((s = cdb_tx_put(db, c->store_id, entry, op, p, n)) != CDB_OK)
        return s;
    c->touched_tx = db->tx_id;
    return CDB_OK;
}

cdb_status cdb_vec_insert(cdb_db *db, uint64_t store, const void *vec, size_t len,
                          uint64_t source_store, uint64_t source_entry, uint64_t *entry)
{
    coll *c;
    cdb_status s;
    if ((s = tx_coll(db, store, &c)) != CDB_OK)
        return s;
    if (source_store == 0 && source_entry != 0)
        return CDB_E_INVALID_ARG;
    if ((s = put_vector(db, c, c->next_entry, CDB_OP_INSERT, vec, len, source_store, source_entry)) != CDB_OK)
        return s;
    *entry = c->next_entry++;
    c->dirty = 1;
    return CDB_OK;
}

cdb_status cdb_vec_replace(cdb_db *db, uint64_t store, uint64_t entry, const void *vec, size_t len)
{
    coll *c;
    int live;
    cdb_status s;
    if ((s = tx_coll(db, store, &c)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= c->next_entry)
        return CDB_E_NOT_FOUND;
    if ((s = live_in_tx(db, c, entry, &live)) != CDB_OK)
        return s;
    if (!live)
        return CDB_E_NOT_FOUND;
    return put_vector(db, c, entry, CDB_OP_REPLACE, vec, len, 0, 0);
}

cdb_status cdb_vec_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    coll *c;
    int live;
    cdb_status s;
    if ((s = tx_coll(db, store, &c)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= c->next_entry)
        return CDB_E_NOT_FOUND;
    if ((s = live_in_tx(db, c, entry, &live)) != CDB_OK)
        return s;
    if (!live)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_tx_put(db, store, entry, CDB_OP_DELETE, NULL, 0)) != CDB_OK)
        return s;
    c->touched_tx = db->tx_id;
    return CDB_OK;
}

/* -------------------------------------------------------- reading */

cdb_status cdb_vec_get(cdb_db *db, uint64_t store, uint64_t entry, void *out, size_t cap, size_t *len,
                       uint64_t *source_store, uint64_t *source_entry)
{
    cdb_value_writer w;
    entry_rec r;
    block b;
    coll *c;
    int found;
    cdb_status s;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if ((s = entry_get(c, entry, &r, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if ((s = block_read(db, c, r.block, &b)) != CDB_OK)
        return s;
    if (r.slot >= b.count || slot_entry(db, &b, r.slot) != entry)
        return CDB_E_CORRUPT;
    cdb_value_writer_init(&w, (uint8_t *)out, cap, db->pager.order, CDB_DECIMAL_COMPACT);
    if ((s = cdb_value_write_vector_raw(&w, (cdb_vector_elem)c->elem, c->dims, slot_data(db, c, &b, r.slot),
                                        c->vec_bytes)) != CDB_OK)
        return s;
    *len = cdb_value_writer_len(&w);
    if (source_store)
        *source_store = r.source_store;
    if (source_entry)
        *source_entry = r.source_entry;
    return CDB_OK;
}

cdb_status cdb_vec_by_source(cdb_db *db, uint64_t store, uint64_t source_store, uint64_t source_entry,
                             uint64_t *entries, size_t max, size_t *n)
{
    cdb_index_cursor cur;
    uint8_t k[24], got[24];
    size_t klen, vlen;
    coll *c;
    int done;
    cdb_status s;
    *n = 0;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (entries == NULL && max)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_index_seek(&cur, &c->sources, vbuf(db, V_CURSOR), k, k_source(k, source_store, source_entry, 0)))
        != CDB_OK)
        return s;
    while (*n < max) {
        uint8_t none[1];
        if ((s = cdb_index_next(&cur, got, sizeof got, &klen, none, sizeof none, &vlen, &done)) != CDB_OK)
            return s;
        if (done || klen != 24 || memcmp(got, k, 16) != 0)
            break;
        entries[(*n)++] = cdb_load_u64(got + 16, CDB_ORDER_BIG);
    }
    return CDB_OK;
}

/* Keeps hits ordered by distance, then by entry ID, at most k of them. */
static void add_hit(cdb_vec_hit *hits, size_t k, size_t *n, uint64_t entry, double dist)
{
    size_t pos = *n;
    while (pos > 0 && (hits[pos - 1].distance > dist || (hits[pos - 1].distance == dist && hits[pos - 1].entry > entry)))
        pos--;
    if (pos >= k)
        return;
    if (*n < k)
        (*n)++;
    memmove(hits + pos + 1, hits + pos, (*n - pos - 1u) * sizeof *hits);
    hits[pos].entry = entry;
    hits[pos].distance = dist;
    hits[pos].source_store = hits[pos].source_entry = 0;
    hits[pos].source_state = CDB_VEC_SOURCE_NONE;
}

/* Fills in each hit's source and whether the vector still describes it. */
static cdb_status source_states(cdb_db *db, coll *c, cdb_vec_hit *hits, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        entry_rec r;
        uint64_t rev;
        int found, live;
        cdb_status s;
        if ((s = entry_get(c, hits[i].entry, &r, &found)) != CDB_OK)
            return s;
        if (!found)
            return CDB_E_CORRUPT;
        hits[i].source_store = r.source_store;
        hits[i].source_entry = r.source_entry;
        if (r.source_store == 0) {
            hits[i].source_state = CDB_VEC_SOURCE_NONE;
            continue;
        }
        s = cdb_doc_head(db, r.source_store, r.source_entry, &rev, &live);
        if (s == CDB_E_NOT_FOUND)
            hits[i].source_state = CDB_VEC_SOURCE_DELETED;
        else if (s == CDB_E_INVALID_ARG || s == CDB_E_UNSUPPORTED)
            hits[i].source_state = CDB_VEC_SOURCE_UNCHECKED;
        else if (s != CDB_OK)
            return s;
        else if (!live)
            hits[i].source_state = CDB_VEC_SOURCE_DELETED;
        else
            hits[i].source_state = rev == r.source_rev ? CDB_VEC_SOURCE_CURRENT : CDB_VEC_SOURCE_OUTDATED;
    }
    return CDB_OK;
}

cdb_status cdb_vec_search(cdb_db *db, uint64_t store, const void *query, size_t len, unsigned measure,
                          cdb_vec_hit *hits, size_t k, size_t *n)
{
    cdb_byte_order o = db->pager.order;
    cdb_index_cursor cur;
    const uint8_t *q;
    uint8_t key[8], val[BLOCKS_VAL];
    size_t klen, vlen;
    double qn = 0.0;
    coll *c;
    int done;
    cdb_status s;

    *n = 0;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (measure == CDB_VEC_BASE)
        measure = c->measure;
    if (!measure_fits(c->elem, measure) || (hits == NULL && k))
        return CDB_E_INVALID_ARG;
    if ((s = vector_of(db, c, (const uint8_t *)query, len, 1, &q)) != CDB_OK)
        return s;
    if (k == 0)
        return CDB_OK;
    if (measure == CDB_VEC_COSINE)
        qn = norm_of(c, q, o);
    if ((s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
        return s;
    for (;;) {
        block b;
        uint32_t i;
        if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            return source_states(db, c, hits, *n);
        if (klen != 8)
            return CDB_E_CORRUPT;
        if ((s = block_read(db, c, cdb_load_u64(key, CDB_ORDER_BIG), &b)) != CDB_OK)
            return s;
        for (i = 0; i < b.count; i++) {
            uint64_t e = slot_entry(db, &b, i);
            if (e == 0)
                continue;
            add_hit(hits, k, n, e,
                    distance(c, measure, q, qn, slot_data(db, c, &b, i),
                             measure == CDB_VEC_COSINE ? slot_norm(db, c, &b, i) : -1.0, o));
        }
    }
}
