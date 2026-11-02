/* Vector collections (spec 7, 14.12).
 *
 * Map of this file, top to bottom:
 * - layout and settings: block geometry and the settings object kept in the
 *   store record (field 16);
 * - collections: opening a collection into a slot, creating one;
 * - elements and distances: reading elements of any type as numbers, and
 *   the four measures;
 * - blocks and entries: where each vector lives (block, slot) and the
 *   indexes that record it;
 * - the transaction side (insert, replace, delete: checked and journaled)
 *   and the apply side (writing blocks at commit, never failing for
 *   logical reasons);
 * - search: exact (measure every vector) and approximate (walk the
 *   neighbour graph);
 * - the neighbour graph: queueing work at commit, refreshing pending
 *   entries in batches, and choosing entry points.
 * Block pages are written in the file's byte order; every index key and
 * value is big-endian. */
#include <math.h>
#include <string.h>

#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_link.h"
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

/* Neighbour graph: record header, bytes per neighbour, live-count record,
 * most entry points. Entry points are reverse rows under neighbour 0. */
#define REC_HDR 12u
#define NB_SIZE 12u
#define MEASURE_VERSION 1u
#define META_LEN 8u
#define MAX_EP 1024u
#define MIN_EP 8u   /* entry points the smallest vec_work holds */
#define EP_SPREAD 1u  /* entry point rows: chosen to spread over the collection */
#define EP_REACH 2u   /* added because a walk could not reach the entry */
#define DEFAULT_BUDGET 256u
#define DEFAULT_EF 64u
#define DEFAULT_EXACT_BELOW 10000u /* bench_graph: exact was faster up to 8000 (spec 7.7) */

/* Settings fields (spec 14.12.3), kept in store record field 16. */
enum { S_ELEM = 1, S_DIMS, S_MEASURE, S_MODEL, S_VERSION, S_ENTRIES, S_BLOCKS, S_SOURCES, S_NORMS,
       S_K, S_GRAPH, S_REVERSE, S_PENDING /* optional: the neighbour graph */ };

typedef struct cdb_vec_coll coll;

/* The three page buffers of cfg.vec_buffers. */
static uint8_t *vbuf(cdb_db *db, unsigned i)
{
    return db->cfg.vec_buffers + (size_t)i * db->pager.page_size;
}

/* V_BLOCK: a block page read for searching or editing; V_BUILD: a block
 * payload being assembled for writing; V_CURSOR: an index cursor's page. */
enum { V_BLOCK = 0, V_BUILD = 1, V_CURSOR = 2 };

/* -------------------------------------------------------- layout */

/* Bytes one vector takes in a block: binary vectors pack 8 dimensions per
 * byte. 0 for an unknown element type. */
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

/* Payload bytes of a block holding count vectors. */
static size_t block_len(const cdb_db *db, const coll *c, uint32_t count)
{
    return data_off(db, c, count) + (size_t)count * c->vec_bytes;
}

/* Most vectors one block page can hold (capped at 2^24 - 1): start from
 * an upper bound and step down until header, IDs, norms, padding and data
 * fit. 0 means a single vector does not fit a page. */
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

/* Binary vectors support only Hamming; the others cosine, dot or
 * Euclidean (spec 14.12.3). */
static int measure_fits(unsigned elem, unsigned measure)
{
    if (elem == CDB_VEC_BINARY)
        return measure == CDB_VEC_HAMMING;
    return measure >= CDB_VEC_COSINE && measure <= CDB_VEC_EUCLIDEAN;
}

/* -------------------------------------------------------- settings */

/* Encodes the settings object (spec 14.12.3). Fields 10-13 are written
 * only once the collection has a graph. */
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
    if (c->graph_on) {
        cdb_value_field(&w, S_K);
        cdb_value_write_int(&w, (int64_t)c->k);
        cdb_value_field(&w, S_GRAPH);
        cdb_value_write_int(&w, (int64_t)c->graph.root);
        cdb_value_field(&w, S_REVERSE);
        cdb_value_write_int(&w, (int64_t)c->reverse.root);
        cdb_value_field(&w, S_PENDING);
        cdb_value_write_int(&w, (int64_t)c->pending.root);
    }
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

/* Required integer field f in [lo, hi], or CDB_E_CORRUPT. */
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

/* Required string field f of at most CDB_VEC_MODEL_MAX bytes, copied to
 * out when out is not NULL. */
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

/* Decodes settings into c (index roots in roots: entries, blocks, sources,
 * then graph, reverse, pending when c->graph_on) and, optionally, cfg with
 * its model strings copied to model and version. */
static cdb_status decode_settings(const cdb_db *db, const uint8_t *in, size_t len, coll *c, uint64_t roots[6],
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
    c->graph_on = 0;
    if (cdb_value_object_get(&obj, o, S_K, &v, &found) != CDB_OK)
        return CDB_E_CORRUPT;
    if (found) {
        int64_t kk, g;
        if (get_int(&obj, o, S_K, 1, CDB_VEC_MAX_K, &kk) != CDB_OK)
            return CDB_E_CORRUPT;
        for (i = 0; i < 3; i++) {
            if (get_int(&obj, o, (uint64_t)S_GRAPH + (uint64_t)i, 1, INT64_MAX, &g) != CDB_OK)
                return CDB_E_CORRUPT;
            roots[3 + i] = (uint64_t)g;
        }
        c->graph_on = 1;
        c->k = (uint32_t)kk;
    }
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

/* Whether the collection is in a slot now (the apply's fast path). */
int cdb_vec_is_open(const cdb_db *db, uint64_t store)
{
    return cdb_slots_find(&db->vec_slots, store) != NULL;
}

/* Records the entry counter, staged for reconciliation. The index roots
 * never change (an index's root ID is fixed), so the counter is the only
 * in-memory state the catalog lacks. Also the slot code's save hook, run
 * before an eviction. */
static cdb_status save_coll(cdb_db *db, void *slot)
{
    coll *c = (coll *)slot;
    cdb_status s;
    if (!c->dirty)
        return CDB_OK;
    if ((s = cdb_catalog_stage_next_entry(&db->catalog, c->slot.store_id, c->next_entry)) != CDB_OK)
        return s;
    c->dirty = 0;
    return CDB_OK;
}

const cdb_slot_ops cdb_vec_slot_ops = { NULL, save_coll };

/* Finds an open collection, or opens it from its catalog record into a
 * slot: decodes the settings, works out the block geometry and opens the
 * indexes (and the graph's, when it has one). Records a use of the slot.
 * CDB_E_INVALID_ARG: not a vector collection; CDB_E_NOT_FOUND: no such
 * store, or it was dropped. CDB_E_UNSUPPORTED without the vector buffers,
 * or for a graph without enough vec_work. */
static cdb_status coll_get(cdb_db *db, uint64_t store, coll **out)
{
    coll *free_slot = (coll *)cdb_slots_find(&db->vec_slots, store);
    cdb_store_info info;
    uint64_t roots[6];
    cdb_status s;
    void *v;

    if (free_slot != NULL) {
        cdb_slots_touch(db, &db->vec_slots, free_slot);
        *out = free_slot;
        return CDB_OK;
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
    if ((s = cdb_slots_claim(db, &db->vec_slots, &v)) != CDB_OK)
        return s;
    free_slot = (coll *)v;
    if ((s = decode_settings(db, info.vector, info.vector_len, free_slot, roots, NULL, NULL, NULL)) != CDB_OK)
        return s;
    free_slot->slot.store_id = store;
    free_slot->slots = block_slots(db, free_slot);
    if (free_slot->slots == 0)
        return CDB_E_CORRUPT;
    free_slot->next_entry = info.next_entry_id ? info.next_entry_id : 1;
    free_slot->touched_tx = UINT64_MAX;
    if ((s = cdb_index_open(&free_slot->entries, &db->pager, roots[0], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->blocks, &db->pager, roots[1], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->sources, &db->pager, roots[2], &db->cfg.catalog_work)) != CDB_OK)
        return s;
    if (free_slot->graph_on) {
        if (db->cfg.vec_work == NULL
            || db->cfg.vec_work_size < cdb_vec_work_size((cdb_vector_elem)free_slot->elem, free_slot->dims, free_slot->k))
            return CDB_E_UNSUPPORTED; /* the graph could not be kept up to date */
        if ((s = cdb_index_open(&free_slot->graph, &db->pager, roots[3], &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_open(&free_slot->reverse, &db->pager, roots[4], &db->cfg.catalog_work)) != CDB_OK
            || (s = cdb_index_open(&free_slot->pending, &db->pager, roots[5], &db->cfg.catalog_work)) != CDB_OK)
            return s;
    }
    free_slot->slot.in_use = 1;
    cdb_slots_touch(db, &db->vec_slots, free_slot);
    *out = free_slot;
    return CDB_OK;
}

static cdb_status refresh_coll(cdb_db *db, coll *c, size_t budget, size_t *processed);

/* At reconciliation: refresh up to the budget of each graph (spec 6.8),
 * then record next entry IDs. */
cdb_status cdb_vec_flush(cdb_db *db)
{
    cdb_slot_iter it;
    coll *c;
    for (c = (coll *)cdb_slots_first(&db->vec_slots, &it); c != NULL; c = (coll *)cdb_slots_next(&it)) {
        cdb_status s;
        if (c->graph_on) {
            size_t done;
            s = refresh_coll(db, c, db->cfg.vec_refresh_budget ? db->cfg.vec_refresh_budget : DEFAULT_BUDGET, &done);
            if (s != CDB_OK)
                return s;
        }
        if ((s = save_coll(db, c)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* Validates the settings, checks one vector fits a page, then (as a
 * catalog change) creates the three indexes and the store record in one
 * commit. Norms are cached only for cosine. */
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
        return cdb_db_catalog_end(db, s);
    if (found)
        return cdb_db_catalog_end(db, CDB_E_EXISTS);
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
    uint64_t roots[6];
    coll *c, tmp;
    cdb_status s;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    return decode_settings(db, info.vector, info.vector_len, &tmp, roots, cfg, model, model_version);
}

/* -------------------------------------------------------- elements */

/* IEEE 754 half precision to float: 1 sign bit, 5 exponent bits (bias 15),
 * 10 mantissa bits. Exponent 0 is a subnormal (m * 2^-24), 31 is infinity
 * or NaN; otherwise (1024 + m) * 2^(e - 25). */
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

/* Element i of a stored vector as a double. bf16 is the top 16 bits of an
 * f32, so it widens by a shift. */
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

/* Euclidean length of a stored vector (used for cosine). */
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

/* Set bits in a byte (each step clears the lowest set bit). */
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
 * negative value when not cached). Smaller is closer for every measure
 * (spec 14.12.3). Both vectors are in stored form, in byte order o.
 * The arithmetic is done in double. */
static double distance(const coll *c, unsigned measure, const uint8_t *q, double qn, const uint8_t *v, double vn,
                       cdb_byte_order o)
{
    uint32_t i;
    /* Hamming: count differing bits, ignoring the unused high bits of the
     * last byte when dims is not a multiple of 8. */
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
    /* Dot product: -dot; cosine: 1 - dot / (|q| |v|). */
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

/* Checks that an encoded value is a vector of the collection's shape with
 * finite elements; *data points at its elements. With full, the encoding is
 * also validated as canonical (done for caller input; journaled payloads
 * were validated when they were made). Anything else is CDB_E_INVALID_ARG. */
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
    if (c->elem == CDB_VEC_F32 || c->elem == CDB_VEC_F16 || c->elem == CDB_VEC_BF16) {
        uint32_t i;
        for (i = 0; i < c->dims; i++) {
            double x = elem_at(v.as.vector.data, i, c->elem, db->pager.order);
            if (x != x || x - x != 0.0)
                return CDB_E_INVALID_ARG;
        }
    }
    *data = v.as.vector.data;
    return CDB_OK;
}

/* -------------------------------------------------------- index records */

/* An entries-index value (spec 14.12.3), decoded. */
typedef struct entry_rec {
    uint64_t block;
    uint32_t slot;
    uint64_t source_store;
    uint64_t source_entry;
    uint64_t source_rev;   /* the document's revision when the vector was written */
} entry_rec;

/* Keys: one big-endian u64 (entries, blocks, graph, pending), or the
 * sources key (source store, source entry, vector entry). Big-endian
 * integers sort as numbers under memcmp. */
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

/* Entries and blocks index rows. blocks_get treats a missing block as
 * corruption: callers only ask about blocks an entry points to. */
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

/* A block page read into V_BLOCK. count is its number of slots (used or
 * empty). */
typedef struct block {
    const uint8_t *p;   /* payload */
    uint32_t count;
} block;

/* Reads a block and checks its header against the collection: owner,
 * element type, dimensions, a slot count within the block size, and a
 * payload length that matches that count exactly. */
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
    if (cdb_load_u64(b->p, db->pager.order) != c->slot.store_id || b->p[12] != c->elem
        || cdb_load_u16(b->p + 14, db->pager.order) != (uint16_t)c->dims || b->count > c->slots
        || h.payload_len != block_len(db, c, b->count))
        return CDB_E_CORRUPT;
    return CDB_OK;
}

/* Slot i's entry ID (0: empty), its vector data, and its cached norm (-1
 * when the collection caches none). */
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
 * remain, with slot set to (entry, data) or emptied when data is NULL.
 * The block is rebuilt in V_BUILD rather than edited in place because the
 * data offset depends on the slot count, so changing the count moves the
 * vector data. Slots of old beyond count are dropped. */
static cdb_status block_write(cdb_db *db, const coll *c, uint64_t lid, const block *old, uint32_t count,
                              uint32_t slot, uint64_t entry, const uint8_t *data)
{
    cdb_byte_order o = db->pager.order;
    uint8_t *nb = vbuf(db, V_BUILD);
    size_t len = block_len(db, c, count), doff = data_off(db, c, count), noff = BLOCK_HDR + (size_t)count * 8u;
    cdb_page_header t;
    uint32_t i;

    memset(nb, 0, len);
    cdb_store_u64(nb, c->slot.store_id, o);
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
    t.store_id = c->slot.store_id;
    return cdb_pager_write(&db->pager, lid, &t, nb, len);
}

/* Puts a new vector in the first block with a free slot, or a new block.
 * The free slot is the first empty one, or a new slot at the end. The
 * blocks index says how full each block is, so full blocks are skipped
 * without reading them; free_from (a hint kept in memory only) moves past
 * them. Fills r->block and r->slot. */
static cdb_status place(cdb_db *db, coll *c, uint64_t entry, const uint8_t *data, entry_rec *r)
{
    cdb_index_cursor cur;
    uint8_t k[8], v[BLOCKS_VAL];
    size_t klen, vlen;
    int done;
    cdb_status s;

    /* The scan starts at the hint, so appends don't read every block. */
    if ((s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), k, k_u64(k, c->free_from))) != CDB_OK)
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
        r->block = cdb_load_u64(k, CDB_ORDER_BIG);
        if (used >= c->slots) {
            c->free_from = r->block + 1u;
            continue;
        }
        c->free_from = r->block;
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
    /* Every block is full: start a new one-slot block. */
    if ((s = cdb_pager_alloc(&db->pager, &r->block)) != CDB_OK)
        return s;
    r->slot = 0;
    if ((s = block_write(db, c, r->block, NULL, 1, 0, entry, data)) != CDB_OK)
        return s;
    return blocks_put(c, r->block, 1, 1);
}

/* Empties the entry's slot; a block left empty is released (its page freed
 * and its blocks row deleted). Checks the slot really holds this entry. */
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
    if (r->block < c->free_from)
        c->free_from = r->block;
    /* Trailing empty slots are dropped, so blocks stay dense. */
    count = b.count;
    while (count > 0 && (count - 1u == r->slot || slot_entry(db, &b, count - 1u) == 0))
        count--;
    if ((s = block_write(db, c, r->block, &b, count, r->slot, 0, NULL)) != CDB_OK)
        return s;
    return blocks_put(c, r->block, used - 1u, count);
}

/* -------------------------------------------------------- neighbour graph
 *
 * Record (spec 14.12.2, 14.12.4): entry ID (8), count (2), flags (1), measure
 * version (1), then per neighbour its entry ID (8) and distance (f32, 4),
 * nearest first; equal distances: lower entry ID first. Key 0 of the graph
 * index holds the live vector count (8 bytes, META_LEN). The entry points
 * are rows (0, entry) of the reverse index, whose 1-byte value is their
 * kind (spec 14.12.4). Index values are big-endian. */

/* One neighbour in a list, and a whole list. */
typedef struct nb {
    uint64_t entry;
    float dist;
} nb;

typedef struct grec {
    uint32_t count;
    nb n[CDB_VEC_MAX_K];
} grec;

/* One pending entry being refreshed, in vec_work; its vector follows. */
typedef struct bitem {
    uint64_t entry;
    double norm;     /* cached norm, as its block holds it (cosine), else -1 */
    int live;
    float d;         /* distance to the vector being scanned */
    grec rec;
} bitem;

/* A search candidate. */
typedef struct cand {
    uint64_t entry;
    double dist;
} cand;

/* The list order: by distance, ties to the lower entry ID. */
static int nb_less(float d1, uint64_t e1, float d2, uint64_t e2)
{
    return d1 < d2 || (d1 == d2 && e1 < e2);
}

/* Offers (e, d) to a list of at most k: kept in order, without duplicates.
 * Returns 1 when it went in; *dropped is the entry it pushed out, or 0. */
static int rec_offer(grec *r, uint32_t k, uint64_t e, float d, uint64_t *dropped)
{
    uint32_t i, pos;
    *dropped = 0;
    for (i = 0; i < r->count; i++)
        if (r->n[i].entry == e)
            return 0;
    if (r->count == k) {
        if (!nb_less(d, e, r->n[k - 1].dist, r->n[k - 1].entry))
            return 0;
        *dropped = r->n[k - 1].entry;
        r->count--;
    }
    for (pos = r->count; pos > 0 && nb_less(d, e, r->n[pos - 1].dist, r->n[pos - 1].entry); pos--)
        r->n[pos] = r->n[pos - 1];
    r->n[pos].entry = e;
    r->n[pos].dist = d;
    r->count++;
    return 1;
}

/* Removes e from a list; returns whether it was there. */
static int rec_remove(grec *r, uint64_t e)
{
    uint32_t i;
    for (i = 0; i < r->count; i++) {
        if (r->n[i].entry == e) {
            memmove(&r->n[i], &r->n[i + 1], (r->count - i - 1u) * sizeof r->n[0]);
            r->count--;
            return 1;
        }
    }
    return 0;
}

/* Reads and writes an entry's graph record (layout above). rec_get checks
 * the stored entry ID, the count against K and the exact length. */
static cdb_status rec_get(coll *c, uint64_t entry, grec *r, int *found)
{
    uint8_t k[8], v[REC_HDR + NB_SIZE * CDB_VEC_MAX_K];
    size_t vlen;
    uint32_t i;
    cdb_status s = cdb_index_get(&c->graph, k, k_u64(k, entry), v, sizeof v, &vlen, found);
    if (s != CDB_OK || !*found)
        return s;
    if (vlen < REC_HDR || cdb_load_u64(v, CDB_ORDER_BIG) != entry)
        return CDB_E_CORRUPT;
    r->count = cdb_load_u16(v + 8, CDB_ORDER_BIG);
    if (r->count > c->k || vlen != REC_HDR + (size_t)r->count * NB_SIZE)
        return CDB_E_CORRUPT;
    for (i = 0; i < r->count; i++) {
        uint32_t bits = cdb_load_u32(v + REC_HDR + i * NB_SIZE + 8u, CDB_ORDER_BIG);
        r->n[i].entry = cdb_load_u64(v + REC_HDR + i * NB_SIZE, CDB_ORDER_BIG);
        memcpy(&r->n[i].dist, &bits, sizeof bits);
    }
    return CDB_OK;
}

static cdb_status rec_put(coll *c, uint64_t entry, const grec *r)
{
    uint8_t k[8], v[REC_HDR + NB_SIZE * CDB_VEC_MAX_K];
    uint32_t i;
    memset(v, 0, REC_HDR);
    cdb_store_u64(v, entry, CDB_ORDER_BIG);
    cdb_store_u16(v + 8, (uint16_t)r->count, CDB_ORDER_BIG);
    v[11] = MEASURE_VERSION;
    for (i = 0; i < r->count; i++) {
        uint32_t bits;
        memcpy(&bits, &r->n[i].dist, sizeof bits);
        cdb_store_u64(v + REC_HDR + i * NB_SIZE, r->n[i].entry, CDB_ORDER_BIG);
        cdb_store_u32(v + REC_HDR + i * NB_SIZE + 8u, bits, CDB_ORDER_BIG);
    }
    return cdb_index_put(&c->graph, k, k_u64(k, entry), v, REC_HDR + (size_t)r->count * NB_SIZE);
}

/* A 16-byte key of two big-endian u64s: reverse rows (neighbour, entry). */
static size_t k_pair(uint8_t *k, uint64_t a, uint64_t b)
{
    cdb_store_u64(k, a, CDB_ORDER_BIG);
    cdb_store_u64(k + 8, b, CDB_ORDER_BIG);
    return 16;
}

/* entry lists neighbour. */
static cdb_status rev_put(coll *c, uint64_t neighbour, uint64_t entry)
{
    uint8_t k[16];
    return cdb_index_put(&c->reverse, k, k_pair(k, neighbour, entry), k, 0);
}

static cdb_status rev_del(coll *c, uint64_t neighbour, uint64_t entry)
{
    uint8_t k[16];
    int found;
    return cdb_index_delete(&c->reverse, k, k_pair(k, neighbour, entry), &found);
}

/* The pending set: entries whose list must be worked out again. */
static cdb_status pending_put(coll *c, uint64_t entry)
{
    uint8_t k[8];
    return cdb_index_put(&c->pending, k, k_u64(k, entry), k, 0);
}

static cdb_status pending_del(coll *c, uint64_t entry)
{
    uint8_t k[8];
    int found;
    return cdb_index_delete(&c->pending, k, k_u64(k, entry), &found);
}

static cdb_status pending_has(coll *c, uint64_t entry, int *found)
{
    uint8_t k[8], v[1];
    size_t vlen;
    return cdb_index_get(&c->pending, k, k_u64(k, entry), v, sizeof v, &vlen, found);
}

/* The live vector count under graph key 0 (missing reads as 0), and
 * meta_live to move it by one; going below 0 is CDB_E_CORRUPT. */
static cdb_status meta_get(coll *c, uint64_t *live)
{
    uint8_t k[8], v[META_LEN];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&c->graph, k, k_u64(k, 0), v, sizeof v, &vlen, &found);
    *live = 0;
    if (s != CDB_OK || !found)
        return s;
    if (vlen != META_LEN)
        return CDB_E_CORRUPT;
    *live = cdb_load_u64(v, CDB_ORDER_BIG);
    return CDB_OK;
}

static cdb_status meta_put(coll *c, uint64_t live)
{
    uint8_t k[8], v[META_LEN];
    cdb_store_u64(v, live, CDB_ORDER_BIG);
    return cdb_index_put(&c->graph, k, k_u64(k, 0), v, META_LEN);
}

static cdb_status meta_live(coll *c, int delta)
{
    uint64_t live;
    cdb_status s = meta_get(c, &live);
    if (s != CDB_OK)
        return s;
    if (delta < 0 && live == 0)
        return CDB_E_CORRUPT;
    return meta_put(c, delta < 0 ? live - 1u : live + 1u);
}

/* The entry point at or after from (reverse rows under neighbour 0) and
 * its kind; 0 when there are no more. */
static cdb_status ep_next(cdb_db *db, coll *c, uint64_t from, uint64_t *e, unsigned *kind)
{
    cdb_index_cursor cur;
    uint8_t k[16], got[16], v[2];
    size_t klen, vlen;
    int done;
    cdb_status s;
    *e = 0;
    if ((s = cdb_index_seek(&cur, &c->reverse, vbuf(db, V_CURSOR), k, k_pair(k, 0, from))) != CDB_OK
        || (s = cdb_index_next(&cur, got, sizeof got, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
        return s;
    if (done || klen != 16 || cdb_load_u64(got, CDB_ORDER_BIG) != 0)
        return CDB_OK;
    *e = cdb_load_u64(got + 8, CDB_ORDER_BIG);
    *kind = vlen == 1 ? v[0] : 0u;
    return *e == 0 || (*kind != EP_SPREAD && *kind != EP_REACH) ? CDB_E_CORRUPT : CDB_OK;
}

/* An entry's entry point kind; 0 when it is not one. */
static cdb_status ep_get(coll *c, uint64_t e, unsigned *kind)
{
    uint8_t k[16], v[2];
    size_t vlen;
    int found;
    cdb_status s = cdb_index_get(&c->reverse, k, k_pair(k, 0, e), v, sizeof v, &vlen, &found);
    *kind = 0;
    if (s != CDB_OK || !found)
        return s;
    if (vlen != 1 || (v[0] != EP_SPREAD && v[0] != EP_REACH))
        return CDB_E_CORRUPT;
    *kind = v[0];
    return CDB_OK;
}

/* Makes e an entry point of the given kind (replacing any earlier kind). */
static cdb_status ep_put(coll *c, uint64_t e, unsigned kind)
{
    uint8_t k[16], v[1];
    v[0] = (uint8_t)kind;
    return cdb_index_put(&c->reverse, k, k_pair(k, 0, e), v, 1);
}

/* Takes an entry out of the graph: its record and the reverse rows of its
 * list, and itself from every list that holds it. Those entries become
 * pending, so their lists are filled again at the next refresh. */
static cdb_status graph_detach(cdb_db *db, coll *c, uint64_t entry)
{
    grec r;
    uint32_t i;
    int found;
    cdb_status s;
    if ((s = rec_get(c, entry, &r, &found)) != CDB_OK)
        return s;
    if (found) {
        uint8_t k[8];
        for (i = 0; i < r.count; i++)
            if ((s = rev_del(c, r.n[i].entry, entry)) != CDB_OK)
                return s;
        if ((s = cdb_index_delete(&c->graph, k, k_u64(k, entry), &found)) != CDB_OK)
            return s;
    }
    /* Every lister: the reverse rows (entry, lister). Each round seeks
     * afresh to the first remaining row, because the row is deleted (and
     * other indexes written) before the next one is read. */
    for (;;) {
        cdb_index_cursor cur;
        uint8_t k[16], got[16], none[1];
        size_t klen, vlen;
        uint64_t lister;
        int done;
        grec lr;
        if ((s = cdb_index_seek(&cur, &c->reverse, vbuf(db, V_CURSOR), k, k_pair(k, entry, 0))) != CDB_OK
            || (s = cdb_index_next(&cur, got, sizeof got, &klen, none, sizeof none, &vlen, &done)) != CDB_OK)
            return s;
        if (done || klen != 16 || memcmp(got, k, 8) != 0)
            return CDB_OK;
        lister = cdb_load_u64(got + 8, CDB_ORDER_BIG);
        if ((s = rev_del(c, entry, lister)) != CDB_OK || (s = rec_get(c, lister, &lr, &found)) != CDB_OK)
            return s;
        if (found && rec_remove(&lr, entry) && (s = rec_put(c, lister, &lr)) != CDB_OK)
            return s;
        if ((s = pending_put(c, lister)) != CDB_OK)
            return s;
    }
}

/* vec_work, aligned for the structures placed in it. */
static uint8_t *work_base(const cdb_db *db, size_t *avail)
{
    uintptr_t p = (uintptr_t)db->cfg.vec_work, a = (p + 7u) & ~(uintptr_t)7u;
    size_t skip = (size_t)(a - p);
    *avail = db->cfg.vec_work_size > skip ? db->cfg.vec_work_size - skip : 0;
    return db->cfg.vec_work + skip;
}

/* Bytes one refresh batch item takes in vec_work: the bitem, then its
 * vector, rounded up to 8. */
static size_t item_stride(size_t vec_bytes)
{
    return (sizeof(bitem) + vec_bytes + 7u) & ~(size_t)7u;
}

/* One chosen entry point in vec_work: its ID, then its vector. */
static size_t ep_stride(size_t vec_bytes)
{
    return (8u + vec_bytes + 7u) & ~(size_t)7u;
}

/* The smallest vec_work that serves every use: a refresh batch of one,
 * MIN_EP + 1 entry points being chosen, or one approximate search with the
 * default ef. More memory gives bigger refresh batches. */
size_t cdb_vec_work_size(cdb_vector_elem elem, uint32_t dims, uint32_t k)
{
    size_t vb = vector_bytes(elem, dims), need = item_stride(vb) + 8u, ep = (MIN_EP + 1u) * ep_stride(vb) + 8u;
    size_t search = 2u * DEFAULT_EF * sizeof(cand) + 512u * 8u + 8u;
    (void)k; /* records are sized for CDB_VEC_MAX_K */
    if (ep > need)
        need = ep;
    if (search > need)
        need = search;
    return need + need / 7u + 64u; /* an eighth more lists the entries to check */
}

/* Copies an entry's vector into out; *norm gets its cached norm or -1. */
static cdb_status fetch_vector(cdb_db *db, coll *c, uint64_t entry, uint8_t *out, double *norm, int *found)
{
    entry_rec r;
    block b;
    cdb_status s;
    if ((s = entry_get(c, entry, &r, found)) != CDB_OK || !*found)
        return s;
    if ((s = block_read(db, c, r.block, &b)) != CDB_OK)
        return s;
    if (r.slot >= b.count || slot_entry(db, &b, r.slot) != entry)
        return CDB_E_CORRUPT;
    memcpy(out, slot_data(db, c, &b, r.slot), c->vec_bytes);
    if (norm)
        *norm = slot_norm(db, c, &b, r.slot);
    return CDB_OK;
}

/* The ID of chosen entry point i (stored in host order: this array is
 * scratch memory, never written to disk). */
static uint64_t ep_id(const uint8_t *w, size_t stride, size_t i)
{
    uint64_t e;
    memcpy(&e, w + i * stride, sizeof e);
    return e;
}

/* Raises r to the distance of the closest pair of the chosen, then keeps,
 * in order, each chosen point farther than r from those already kept. The
 * closest points go first, so the rest stay spread out. */
static size_t ep_thin(const coll *c, unsigned spread, uint8_t *w, size_t stride, size_t n, double *r,
                      cdb_byte_order o)
{
    size_t i, j, kept = 0;
    double m = -1.0;
    for (i = 0; i < n; i++)
        for (j = i + 1u; j < n; j++) {
            double d = distance(c, spread, w + i * stride + 8u, 0.0, w + j * stride + 8u, -1.0, o);
            if (m < 0.0 || d < m)
                m = d;
        }
    *r = m;
    for (i = 0; i < n; i++) {
        for (j = 0; j < kept; j++)
            if (distance(c, spread, w + j * stride + 8u, 0.0, w + i * stride + 8u, -1.0, o) <= *r)
                break;
        if (j < kept)
            continue;
        if (kept != i)
            memmove(w + kept * stride, w + i * stride, stride);
        kept++;
    }
    return kept;
}

/* Chooses entry points that cover the collection (spec 7.4): about the
 * square root of the live count (at least MIN_EP, at most MAX_EP and what
 * vec_work holds). One pass over the blocks keeps each vector farther than
 * r from those chosen (Euclidean, or Hamming for binary vectors); when too
 * many are chosen, r grows to their closest pair and they are thinned. The
 * rows under neighbour 0 are then made to match. */
static cdb_status choose_entry_points(cdb_db *db, coll *c, uint8_t *w, size_t avail)
{
    unsigned spread = c->elem == CDB_VEC_BINARY ? CDB_VEC_HAMMING : CDB_VEC_EUCLIDEAN, kind;
    cdb_byte_order o = db->pager.order;
    size_t stride = ep_stride(c->vec_bytes), want, n = 0, i;
    uint64_t live, e, from;
    double r = 0.0;
    cdb_index_cursor cur;
    uint8_t key[8], val[BLOCKS_VAL];
    size_t klen, vlen;
    int done;
    cdb_status s;

    /* How many: sqrt(live), clamped, and leaving room for one more than
     * wanted in vec_work (the newcomer that triggers a thinning). */
    if ((s = meta_get(c, &live)) != CDB_OK)
        return s;
    want = (size_t)ceil(sqrt((double)live));
    if (want < MIN_EP)
        want = MIN_EP;
    if (want > MAX_EP)
        want = MAX_EP;
    if (avail / stride < want + 1u)
        want = avail / stride > 1u ? avail / stride - 1u : 0;
    if (want == 0)
        return CDB_E_UNSUPPORTED;
    /* One pass over every block, in block order. r starts at 0, so the
     * first vectors are all kept until the first thinning. */
    if ((s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
        return s;
    for (;;) {
        block b;
        uint32_t t;
        if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        if ((s = block_read(db, c, cdb_load_u64(key, CDB_ORDER_BIG), &b)) != CDB_OK)
            return s;
        for (t = 0; t < b.count; t++) {
            const uint8_t *v = slot_data(db, c, &b, t);
            e = slot_entry(db, &b, t);
            if (e == 0)
                continue;
            for (i = 0; i < n; i++)
                if (distance(c, spread, w + i * stride + 8u, 0.0, v, -1.0, o) <= r)
                    break;
            if (i < n)
                continue;
            memcpy(w + n * stride, &e, sizeof e);
            memcpy(w + n * stride + 8u, v, c->vec_bytes);
            n++;
            while (n > want)
                n = ep_thin(c, spread, w, stride, n, &r, o);
        }
    }
    /* Make the stored rows match: drop spread rows no longer chosen (reach
     * rows are left alone), then mark every chosen entry as spread. */
    for (from = 1;; from = e + 1u) {
        if ((s = ep_next(db, c, from, &e, &kind)) != CDB_OK)
            return s;
        if (e == 0)
            break;
        if (kind != EP_SPREAD)
            continue;
        for (i = 0; i < n && ep_id(w, stride, i) != e; i++)
            ;
        if (i == n && (s = rev_del(c, 0, e)) != CDB_OK)
            return s;
    }
    for (i = 0; i < n; i++) {
        if ((s = ep_get(c, ep_id(w, stride, i), &kind)) != CDB_OK)
            return s;
        if (kind != EP_SPREAD && (s = ep_put(c, ep_id(w, stride, i), EP_SPREAD)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* Working state of the approximate path, in vec_work: the candidates still
 * to expand, the best found so far (sorted), and the visited set. */
typedef struct walk {
    cand *cands;
    size_t ncand;
    cand *best;
    size_t nbest;
    size_t ef;
    uint64_t *seen;
    size_t seen_cap;
    size_t seen_n;
} walk;

/* Marks e visited; 0 if it already was. When the set is full, nothing is
 * marked and 1 is returned: the walk then stops expanding (see full).
 * The set is an open-addressing hash table of entry IDs (0 marks an empty
 * cell, which is safe since entry ID 0 is never used), probed linearly. */
static int visit(walk *w, uint64_t e)
{
    size_t h = (size_t)((e * 0x9E3779B97F4A7C15ull) % w->seen_cap);
    while (w->seen[h] != 0) {
        if (w->seen[h] == e)
            return 0;
        h = (h + 1u) % w->seen_cap;
    }
    if (w->seen_n + 1u >= w->seen_cap)
        return 1;
    w->seen[h] = e;
    w->seen_n++;
    return 1;
}

static int full(const walk *w)
{
    return w->seen_n + 1u >= w->seen_cap;
}

/* Offers a found vector to the best list and the candidates.
 * best holds at most ef vectors, sorted by (distance, entry). With expand,
 * a vector that made it into best is also queued as a candidate whose
 * list will be followed; when the queue is full it replaces the farthest
 * candidate, if it is nearer. */
static void consider(walk *w, uint64_t e, double d, int expand)
{
    size_t i, pos;
    for (i = 0; i < w->nbest; i++)
        if (w->best[i].entry == e)
            return;
    if (w->nbest == w->ef && !(d < w->best[w->ef - 1].dist || (d == w->best[w->ef - 1].dist && e < w->best[w->ef - 1].entry)))
        return;
    if (w->nbest == w->ef)
        w->nbest--;
    for (pos = w->nbest; pos > 0 && (w->best[pos - 1].dist > d || (w->best[pos - 1].dist == d && w->best[pos - 1].entry > e)); pos--)
        w->best[pos] = w->best[pos - 1];
    w->best[pos].entry = e;
    w->best[pos].dist = d;
    w->nbest++;
    if (!expand)
        return;
    if (w->ncand < w->ef) {
        w->cands[w->ncand].entry = e;
        w->cands[w->ncand++].dist = d;
    } else {
        size_t worst = 0;
        for (i = 1; i < w->ncand; i++)
            if (w->cands[i].dist > w->cands[worst].dist)
                worst = i;
        if (d < w->cands[worst].dist) {
            w->cands[worst].entry = e;
            w->cands[worst].dist = d;
        }
    }
}

/* Measures a vector by entry and offers it. Always under the base measure:
 * the graph's lists are ordered by it. An entry that no longer exists is
 * skipped. */
static cdb_status reach(cdb_db *db, coll *c, walk *w, const uint8_t *q, double qn, uint64_t e, int expand)
{
    entry_rec r;
    block b;
    int found;
    cdb_status s;
    if ((s = entry_get(c, e, &r, &found)) != CDB_OK || !found)
        return s;
    if ((s = block_read(db, c, r.block, &b)) != CDB_OK)
        return s;
    if (r.slot >= b.count || slot_entry(db, &b, r.slot) != e)
        return CDB_E_CORRUPT;
    consider(w, e,
             distance(c, c->measure, q, qn, slot_data(db, c, &b, r.slot),
                      c->measure == CDB_VEC_COSINE ? slot_norm(db, c, &b, r.slot) : -1.0, db->pager.order),
             expand);
    return CDB_OK;
}

/* Whether the walk visited e (same hash as visit, without inserting). */
static int seen_has(const walk *w, uint64_t e)
{
    size_t h = (size_t)((e * 0x9E3779B97F4A7C15ull) % w->seen_cap);
    while (w->seen[h] != 0) {
        if (w->seen[h] == e)
            return 1;
        h = (h + 1u) % w->seen_cap;
    }
    return 0;
}

/* Lays a walk out in mem: ef candidates and best (at least k), then the
 * visited set. 0 when there is too little room. */
static int walk_setup(walk *w, uint8_t *mem, size_t avail, size_t ef, size_t k)
{
    memset(w, 0, sizeof *w);
    w->ef = ef < k ? k : ef;
    if (avail < 2u * k * sizeof(cand) + 64u * sizeof(uint64_t))
        return 0;
    if (avail < 2u * w->ef * sizeof(cand) + 64u * sizeof(uint64_t))
        w->ef = (avail - 64u * sizeof(uint64_t)) / (2u * sizeof(cand)); /* what fits */
    w->cands = (cand *)(void *)mem;
    w->best = w->cands + w->ef;
    w->seen = (uint64_t *)(void *)(w->best + w->ef);
    w->seen_cap = (avail - 2u * w->ef * sizeof(cand)) / sizeof(uint64_t);
    memset(w->seen, 0, w->seen_cap * sizeof(uint64_t));
    return 1;
}

/* A best-first walk over the graph under the base measure, from every entry
 * point but skip; the nearest ef of them start it. A walk with a skip is a
 * reachability check: it stops as soon as a list leads to skip. */
static cdb_status walk_run(cdb_db *db, coll *c, walk *w, const uint8_t *q, double qn, uint64_t skip)
{
    uint64_t e, from;
    unsigned kind;
    cdb_status s;
    /* Measure every entry point; the nearest become the first candidates. */
    for (from = 1;; from = e + 1u) {
        if ((s = ep_next(db, c, from, &e, &kind)) != CDB_OK)
            return s;
        if (e == 0)
            break;
        if (e != skip && visit(w, e) && (s = reach(db, c, w, q, qn, e, 1)) != CDB_OK)
            return s;
    }
    /* Best first: take the nearest candidate and measure the neighbours its
     * list names, until no candidate can improve the best list or the
     * visited set fills up. */
    while (w->ncand > 0 && !full(w)) {
        size_t m = 0, j;
        cand next;
        grec r;
        int found;
        for (j = 1; j < w->ncand; j++)
            if (w->cands[j].dist < w->cands[m].dist)
                m = j;
        next = w->cands[m];
        w->cands[m] = w->cands[--w->ncand];
        if (w->nbest == w->ef && next.dist > w->best[w->ef - 1].dist)
            break; /* nothing left can improve the best list */
        if ((s = rec_get(c, next.entry, &r, &found)) != CDB_OK)
            return s;
        for (j = 0; found && j < r.count && !full(w); j++) {
            if (skip && r.n[j].entry == skip) {
                (void)visit(w, skip);
                return CDB_OK;
            }
            if (visit(w, r.n[j].entry) && (s = reach(db, c, w, q, qn, r.n[j].entry, 1)) != CDB_OK)
                return s;
        }
    }
    return CDB_OK;
}

/* Entries whose reachability is checked after a refresh, in vec_work. */
typedef struct checks {
    uint64_t *ids;
    size_t n, cap;
    int overflow;    /* too many: every listed entry is checked */
} checks;

/* Max-heap sift-down, for sort_ids. */
static void sift_down(uint64_t *a, size_t i, size_t n)
{
    for (;;) {
        size_t c = 2u * i + 1u;
        uint64_t t;
        if (c >= n)
            return;
        if (c + 1u < n && a[c + 1u] > a[c])
            c++;
        if (a[i] >= a[c])
            return;
        t = a[i];
        a[i] = a[c];
        a[c] = t;
        i = c;
    }
}

/* Heapsort: in place, and no allocation (the core has none). */
static void sort_ids(uint64_t *a, size_t n)
{
    size_t i;
    for (i = n / 2u; i > 0; i--)
        sift_down(a, i - 1u, n);
    for (i = n; i > 1; i--) {
        uint64_t t = a[0];
        a[0] = a[i - 1u];
        a[i - 1u] = t;
        sift_down(a, 0, i - 1u);
    }
}

/* Sorts and removes duplicates in place; returns the new count. */
static size_t unique_ids(uint64_t *a, size_t n)
{
    size_t i, m = 0;
    sort_ids(a, n);
    for (i = 0; i < n; i++)
        if (m == 0 || a[m - 1] != a[i])
            a[m++] = a[i];
    return m;
}

/* Lists e to be checked. When the list is full it is deduplicated first;
 * if it is still more than half full, the list gives up (overflow) and the
 * check falls back to checking everything. */
static void check_add(checks *k, uint64_t e)
{
    if (k->overflow || e == 0)
        return;
    if (k->n == k->cap) {
        k->n = unique_ids(k->ids, k->n);
        if (k->n * 2u > k->cap) {
            k->overflow = 1;
            return;
        }
    }
    k->ids[k->n++] = e;
}

/* Checks that a walk from the other entry points reaches e, with e's own
 * vector as the query. An entry it can't reach becomes an entry point; one
 * added that way stops being one once it is reached. Pending entries,
 * spread entry points and entries that no longer exist are skipped.
 * Uses V_BUILD for e's vector. */
static cdb_status check_entry(cdb_db *db, coll *c, uint64_t e, uint8_t *mem, size_t avail)
{
    uint8_t *v = vbuf(db, V_BUILD);
    unsigned kind;
    int found, pend;
    walk w;
    cdb_status s;
    if ((s = pending_has(c, e, &pend)) != CDB_OK || pend)
        return s; /* searches measure pending vectors directly */
    if ((s = ep_get(c, e, &kind)) != CDB_OK || kind == EP_SPREAD)
        return s;
    if ((s = fetch_vector(db, c, e, v, NULL, &found)) != CDB_OK || !found)
        return s;
    if (!walk_setup(&w, mem, avail, db->cfg.vec_ef ? db->cfg.vec_ef : DEFAULT_EF, 1))
        return CDB_E_UNSUPPORTED;
    if ((s = walk_run(db, c, &w, v, c->measure == CDB_VEC_COSINE ? norm_of(c, v, db->pager.order) : 0.0, e))
        != CDB_OK)
        return s;
    if (seen_has(&w, e))
        return kind == EP_REACH ? rev_del(c, 0, e) : CDB_OK;
    return kind == 0 ? ep_put(c, e, EP_REACH) : CDB_OK;
}

/* After a refresh: entry points are chosen again, then the entries it
 * touched, and those added as entry points before, are checked. */
static cdb_status check_coverage(cdb_db *db, coll *c, checks *k, uint8_t *mem, size_t avail)
{
    uint64_t e, from;
    unsigned kind;
    size_t i;
    cdb_status s;
    if ((s = choose_entry_points(db, c, mem, avail)) != CDB_OK)
        return s;
    for (from = 1; !k->overflow; from = e + 1u) {
        if ((s = ep_next(db, c, from, &e, &kind)) != CDB_OK)
            return s;
        if (e == 0)
            break;
        if (kind == EP_REACH)
            check_add(k, e);
    }
    if (!k->overflow) {
        k->n = unique_ids(k->ids, k->n);
        for (i = 0; i < k->n; i++)
            if ((s = check_entry(db, c, k->ids[i], mem, avail)) != CDB_OK)
                return s;
        return CDB_OK;
    }
    /* Too many to list: every entry with a list, and every entry point. */
    for (from = 1;; from = e + 1u) {
        cdb_index_cursor cur;
        uint8_t key[8], got[8], rec[REC_HDR + NB_SIZE * CDB_VEC_MAX_K];
        size_t klen, vlen;
        int done;
        if ((s = cdb_index_seek(&cur, &c->graph, vbuf(db, V_CURSOR), key, k_u64(key, from))) != CDB_OK
            || (s = cdb_index_next(&cur, got, sizeof got, &klen, rec, sizeof rec, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        e = cdb_load_u64(got, CDB_ORDER_BIG);
        if ((s = check_entry(db, c, e, mem, avail)) != CDB_OK)
            return s;
    }
    for (from = 1;; from = e + 1u) {
        if ((s = ep_next(db, c, from, &e, &kind)) != CDB_OK)
            return s;
        if (e == 0)
            break;
        if ((s = check_entry(db, c, e, mem, avail)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* Whether e is one of the n batch items. */
static int in_batch(const uint8_t *items, size_t stride, size_t n, uint64_t e)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (((const bitem *)(const void *)(items + i * stride))->entry == e)
            return 1;
    return 0;
}

/* Refreshes pending entries, at most budget of them (0: all), in batches
 * that fit vec_work. Each batch reads every vector once: it builds the
 * batch's lists and offers the batch's vectors to the fresh lists of the
 * others. Changes only pager pages; the caller commits. */
static cdb_status refresh_coll(cdb_db *db, coll *c, size_t budget, size_t *processed)
{
    cdb_byte_order o = db->pager.order;
    size_t avail, stride = item_stride(c->vec_bytes), cap, n, i;
    uint8_t *items = work_base(db, &avail);
    checks chk;
    cdb_status s;

    /* The last eighth of vec_work lists the entries to check. */
    chk.cap = avail / 8u / sizeof(uint64_t);
    chk.n = 0;
    chk.overflow = 0;
    avail -= chk.cap * sizeof(uint64_t);
    chk.ids = (uint64_t *)(void *)(items + avail);
    *processed = 0;
    cap = avail / stride;
    if (cap == 0)
        return CDB_E_UNSUPPORTED;
    for (;;) {
        cdb_index_cursor cur;
        uint8_t key[8], val[BLOCKS_VAL];
        size_t klen, vlen;
        int done, any = 0, has_other_pending = 1;

        /* The next batch of pending entries. */
        n = cap;
        if (budget && budget - *processed < n)
            n = budget - *processed;
        if (n == 0)
            break;
        if ((s = cdb_index_seek(&cur, &c->pending, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
            return s;
        for (i = 0; i < n; i++) {
            bitem *it = (bitem *)(void *)(items + i * stride);
            if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK)
                return s;
            if (done) {
                has_other_pending = 0;
                break;
            }
            memset(it, 0, sizeof *it);
            it->entry = cdb_load_u64(key, CDB_ORDER_BIG);
        }
        n = i;
        if (n == 0)
            break;
        /* Load each batch entry's vector (it may have been deleted since it
         * was queued; then it is just taken off the pending list below). */
        for (i = 0; i < n; i++) {
            bitem *it = (bitem *)(void *)(items + i * stride);
            if ((s = fetch_vector(db, c, it->entry, (uint8_t *)(it + 1), &it->norm, &it->live)) != CDB_OK)
                return s;
            any |= it->live;
        }

        /* One pass over every vector. */
        if (any && (s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
            return s;
        while (any) {
            block b;
            uint32_t j;
            if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            if ((s = block_read(db, c, cdb_load_u64(key, CDB_ORDER_BIG), &b)) != CDB_OK)
                return s;
            for (j = 0; j < b.count; j++) {
                uint64_t u = slot_entry(db, &b, j), dropped;
                const uint8_t *ud = slot_data(db, c, &b, j);
                double un = slot_norm(db, c, &b, j);
                int pend = 0, found, changed = 0;
                grec ur;
                if (u == 0)
                    continue;
                /* Offer u to every batch list, remembering each distance
                 * in it->d for the reverse offer below. */
                for (i = 0; i < n; i++) {
                    bitem *it = (bitem *)(void *)(items + i * stride);
                    if (!it->live || it->entry == u)
                        continue;
                    it->d = (float)distance(c, c->measure, (const uint8_t *)(it + 1), it->norm, ud, un, o);
                    (void)rec_offer(&it->rec, c->k, u, it->d, &dropped);
                }
                if (in_batch(items, stride, n, u))
                    continue;
                /* A fresh list takes the batch's vectors that are nearer. */
                if (has_other_pending) {
                    if ((s = pending_has(c, u, &pend)) != CDB_OK)
                        return s;
                    if (pend)
                        continue;
                }
                if ((s = rec_get(c, u, &ur, &found)) != CDB_OK)
                    return s;
                if (!found)
                    continue;
                for (i = 0; i < n; i++) {
                    bitem *it = (bitem *)(void *)(items + i * stride);
                    if (!it->live || !rec_offer(&ur, c->k, it->entry, it->d, &dropped))
                        continue;
                    changed = 1;
                    if ((s = rev_put(c, it->entry, u)) != CDB_OK || (dropped && (s = rev_del(c, dropped, u)) != CDB_OK))
                        return s;
                    check_add(&chk, dropped); /* it lost a way in */
                }
                if (changed && (s = rec_put(c, u, &ur)) != CDB_OK)
                    return s;
            }
        }

        /* Store the batch's lists, replacing the reverse rows of each old
         * list with those of the new one. Every entry touched is listed for
         * the coverage check. */
        for (i = 0; i < n; i++) {
            bitem *it = (bitem *)(void *)(items + i * stride);
            if (it->live) {
                grec old;
                uint32_t j;
                int found;
                if ((s = rec_get(c, it->entry, &old, &found)) != CDB_OK)
                    return s;
                for (j = 0; found && j < old.count; j++) {
                    if ((s = rev_del(c, old.n[j].entry, it->entry)) != CDB_OK)
                        return s;
                    check_add(&chk, old.n[j].entry);
                }
                check_add(&chk, it->entry);
                if ((s = rec_put(c, it->entry, &it->rec)) != CDB_OK)
                    return s;
                for (j = 0; j < it->rec.count; j++)
                    if ((s = rev_put(c, it->rec.n[j].entry, it->entry)) != CDB_OK)
                        return s;
            }
            if ((s = pending_del(c, it->entry)) != CDB_OK)
                return s;
        }
        *processed += n;
    }
    return *processed ? check_coverage(db, c, &chk, items, avail) : CDB_OK;
}

/* -------------------------------------------------------- apply */

/* Splits an insert/replace payload (spec 14.12.3): flags, an optional
 * source (two varints), then the encoded vector. CDB_E_MALFORMED if any
 * part is missing or the vector has the wrong shape. */
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
        /* Insert: place the vector, record its source (and the source
         * document's revision now), and queue it for the graph. */
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
            if ((s = entry_put(c, d->entry_id, &r)) != CDB_OK)
                return s;
            if (c->graph_on && ((s = pending_put(c, d->entry_id)) != CDB_OK || (s = meta_live(c, +1)) != CDB_OK))
                return s;
            return CDB_OK;
        } else {
            /* Replace (an insert of an existing entry also lands here):
             * overwrite the vector in its slot. Any source in the payload
             * is ignored; the recorded source is kept. */
            uint32_t used, count;
            block b;
            if ((s = blocks_get(c, r.block, &used, &count)) != CDB_OK || (s = block_read(db, c, r.block, &b)) != CDB_OK)
                return s;
            if (r.slot >= b.count || slot_entry(db, &b, r.slot) != d->entry_id)
                return CDB_E_CORRUPT;
            if ((s = block_write(db, c, r.block, &b, b.count, r.slot, d->entry_id, data)) != CDB_OK)
                return s;
            /* A new vector: its place in the graph is worked out again. */
            if (c->graph_on && ((s = graph_detach(db, c, d->entry_id)) != CDB_OK
                                || (s = pending_put(c, d->entry_id)) != CDB_OK))
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
        /* Remove it from the graph (and as an entry point), its slot, its
         * source row and its entries row; then tell the links. */
        if (!found)
            return CDB_OK;
        if (c->graph_on && ((s = graph_detach(db, c, d->entry_id)) != CDB_OK
                            || (s = pending_del(c, d->entry_id)) != CDB_OK || (s = rev_del(c, 0, d->entry_id)) != CDB_OK
                            || (s = meta_live(c, -1)) != CDB_OK))
            return s;
        if ((s = unplace(db, c, d->entry_id, &r)) != CDB_OK)
            return s;
        if (r.source_store
            && (s = cdb_index_delete(&c->sources, k, k_source(k, r.source_store, r.source_entry, d->entry_id),
                                     &found)) != CDB_OK)
            return s;
        if ((s = cdb_index_delete(&c->entries, k, k_u64(k, d->entry_id), &found)) != CDB_OK)
            return s;
        return cdb_link_on_delete(db, d->store_id, d->entry_id); /* its links (spec 6.7) */
    default:
        return CDB_OK;
    }
}

/* -------------------------------------------------------- transactions */

/* Common checks for every vector change: an open, healthy transaction, the
 * built-in apply (which knows vector records), and a vector collection. */
static cdb_status tx_coll(cdb_db *db, uint64_t store, coll **c)
{
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED;
    return coll_get(db, store, c);
}

/* Whether the entry exists as the open transaction sees it: committed, then
 * the transaction's own changes to this collection, if it made any.
 * touched_tx avoids reading the journal back when this transaction has not
 * changed the collection. Only inserts and deletes change liveness. */
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
        if (rec.kind != CDB_RECORD_DATA || rec.tx_id != db->tx_id || rec.store_id != c->slot.store_id
            || rec.entry_id != entry || rec.payload_len < 1)
            continue;
        if (rec.payload[0] == CDB_OP_INSERT)
            *live = 1;
        else if (rec.payload[0] == CDB_OP_DELETE)
            *live = 0;
    }
    return CDB_OK;
}

/* Validates the vector fully, builds the payload in V_BUILD (flags,
 * optional source varints, the vector) and journals it. The size check
 * keeps the payload within the one page buffer. */
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
    if ((s = cdb_tx_put(db, c->slot.store_id, entry, op, p, n)) != CDB_OK)
        return s;
    c->touched_tx = db->tx_id;
    return CDB_OK;
}

/* The entry ID comes from the collection's counter, which advances only
 * after the change is journaled. The source is not checked to exist. */
cdb_status cdb_vec_insert(cdb_db *db, uint64_t store, const void *vec, size_t len,
                          uint64_t source_store, uint64_t source_entry, uint64_t *entry)
{
    coll *c;
    uint64_t id;
    cdb_status s;
    if ((s = tx_coll(db, store, &c)) != CDB_OK)
        return s;
    if (source_store == 0 && source_entry != 0)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_db_peek_entry(db, store, &c->next_entry, &id)) != CDB_OK
        || (s = put_vector(db, c, id, CDB_OP_INSERT, vec, len, source_store, source_entry)) != CDB_OK)
        return s;
    *entry = cdb_db_take_entry(db, store, &c->next_entry, &c->dirty);
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
    if ((s = cdb_link_check_delete(db, store, entry)) != CDB_OK)
        return s; /* a refuse-type link points at it (spec 6.7) */
    if ((s = cdb_tx_put(db, store, entry, CDB_OP_DELETE, NULL, 0)) != CDB_OK)
        return s;
    c->touched_tx = db->tx_id;
    return CDB_OK;
}

cdb_status cdb_vec_live_in_tx(cdb_db *db, uint64_t store, uint64_t entry, int *live)
{
    coll *c;
    entry_rec r;
    cdb_status s;
    *live = 0;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (entry == 0 || entry >= c->next_entry)
        return CDB_OK;
    return db->in_tx ? live_in_tx(db, c, entry, live) : entry_get(c, entry, &r, live);
}

/* -------------------------------------------------------- reading */

/* Re-encodes the stored elements as a vector value in out. A small out
 * gives the value writer's CDB_E_BUFFER_TOO_SMALL. */
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
    /* A prefix scan: rows (source store, source entry, *) in entry order. */
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

/* Keeps hits ordered by distance, then by entry ID, at most k of them.
 * An insertion sort into a list of at most k: fine for the small k a
 * search asks for. */
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

/* Fills in each hit's source and whether the vector still describes it:
 * the source document's head revision is compared with the one recorded
 * when the vector was written (spec 14.12.3). */
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

/* The exact path: every vector is compared with the query, block by
 * block. Cached norms are used for cosine. */
static cdb_status search_exact(cdb_db *db, coll *c, const uint8_t *q, unsigned measure, cdb_vec_hit *hits,
                               size_t k, size_t *n)
{
    cdb_byte_order o = db->pager.order;
    cdb_index_cursor cur;
    uint8_t key[8], val[BLOCKS_VAL];
    size_t klen, vlen;
    double qn = measure == CDB_VEC_COSINE ? norm_of(c, q, o) : 0.0;
    int done;
    cdb_status s;

    if ((s = cdb_index_seek(&cur, &c->blocks, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
        return s;
    for (;;) {
        block b;
        uint32_t i;
        if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            return CDB_OK;
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

/* The approximate path (spec 7.4, 7.7): a best-first walk over the graph
 * from its entry points under the base measure, plus every pending vector
 * measured directly; what it found is ranked with the requested measure. */
static cdb_status search_approx(cdb_db *db, coll *c, const uint8_t *q, unsigned measure, cdb_vec_hit *hits,
                                size_t k, size_t *n)
{
    cdb_byte_order o = db->pager.order;
    double qn = c->measure == CDB_VEC_COSINE ? norm_of(c, q, o) : 0.0;
    size_t avail, i;
    uint8_t *base = work_base(db, &avail);
    walk w;
    cdb_status s;

    if (!walk_setup(&w, base, avail, db->cfg.vec_ef ? db->cfg.vec_ef : DEFAULT_EF, k))
        return search_exact(db, c, q, measure, hits, k, n); /* too little room to walk */
    if ((s = walk_run(db, c, &w, q, qn, 0)) != CDB_OK)
        return s;
    /* Pending vectors are not in the graph yet: measure each directly. */
    {
        cdb_index_cursor cur;
        uint8_t key[8], none[1];
        size_t klen, vlen;
        int done;
        if ((s = cdb_index_seek(&cur, &c->pending, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
            return s;
        for (;;) {
            if ((s = cdb_index_next(&cur, key, sizeof key, &klen, none, sizeof none, &vlen, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            if ((s = reach(db, c, &w, q, qn, cdb_load_u64(key, CDB_ORDER_BIG), 0)) != CDB_OK)
                return s;
        }
    }
    /* Rank what was found with the requested measure. */
    for (i = 0; i < w.nbest; i++) {
        double d = w.best[i].dist;
        if (measure != c->measure) {
            uint8_t *tmp = vbuf(db, V_BUILD);
            double vn;
            int found;
            if ((s = fetch_vector(db, c, w.best[i].entry, tmp, &vn, &found)) != CDB_OK)
                return s;
            if (!found)
                continue;
            d = distance(c, measure, q, measure == CDB_VEC_COSINE ? norm_of(c, q, o) : 0.0, tmp,
                         c->norms ? vn : -1.0, o);
        }
        add_hit(hits, k, n, w.best[i].entry, d);
    }
    return CDB_OK;
}

/* Validates the query, picks the path (see cdb_vec.h), searches, then
 * fills in each hit's source state. Searches see committed vectors only. */
cdb_status cdb_vec_search_mode(cdb_db *db, uint64_t store, const void *query, size_t len, unsigned measure,
                               unsigned mode, cdb_vec_hit *hits, size_t k, size_t *n)
{
    const uint8_t *q;
    coll *c;
    int approx = 0;
    cdb_status s;

    *n = 0;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (measure == CDB_VEC_BASE)
        measure = c->measure;
    if (!measure_fits(c->elem, measure) || (hits == NULL && k) || mode > CDB_VEC_APPROX)
        return CDB_E_INVALID_ARG;
    if ((s = vector_of(db, c, (const uint8_t *)query, len, 1, &q)) != CDB_OK)
        return s;
    if (k == 0)
        return CDB_OK;
    /* The caller's mode, else the operator's, else the automatic rule:
     * approximate only with a graph, under the base measure, and with at
     * least vec_exact_below live vectors. */
    if (mode == CDB_VEC_AUTO)
        mode = db->cfg.vec_mode;
    if (c->graph_on && mode == CDB_VEC_APPROX) {
        approx = 1;
    } else if (c->graph_on && mode == CDB_VEC_AUTO && measure == c->measure) {
        uint64_t live;
        if ((s = meta_get(c, &live)) != CDB_OK)
            return s;
        approx = live >= (db->cfg.vec_exact_below ? db->cfg.vec_exact_below : DEFAULT_EXACT_BELOW);
    }
    s = approx ? search_approx(db, c, q, measure, hits, k, n) : search_exact(db, c, q, measure, hits, k, n);
    return s == CDB_OK ? source_states(db, c, hits, *n) : s;
}

cdb_status cdb_vec_search(cdb_db *db, uint64_t store, const void *query, size_t len, unsigned measure,
                          cdb_vec_hit *hits, size_t k, size_t *n)
{
    return cdb_vec_search_mode(db, store, query, len, measure, CDB_VEC_AUTO, hits, k, n);
}

/* -------------------------------------------------------- graph upkeep */

/* A catalog change: checks K and the working memory, creates the graph,
 * reverse and pending indexes, queues every existing entry, sets the live
 * count, and rewrites the settings with fields 10-13 (spec 14.12.4). */
cdb_status cdb_vec_create_graph(cdb_db *db, uint64_t store, uint32_t k)
{
    char model[CDB_VEC_MODEL_MAX], version[CDB_VEC_MODEL_MAX];
    uint8_t settings[CDB_CATALOG_SETTINGS_MAX];
    cdb_store_info info;
    cdb_vec_config cfg;
    uint64_t roots[6], live = 0;
    size_t slen;
    coll *c, tmp;
    cdb_status s;

    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (c->graph_on)
        return CDB_E_EXISTS;
    if (k == 0)
        k = CDB_VEC_DEFAULT_K;
    if (k > CDB_VEC_MAX_K)
        return CDB_E_INVALID_ARG;
    if (REC_HDR + (size_t)k * NB_SIZE > cdb_index_max_value(&db->pager))
        return CDB_E_OVERFLOW; /* a record of k neighbours does not fit an index value */
    if (db->cfg.vec_work == NULL || db->cfg.vec_work_size < cdb_vec_work_size((cdb_vector_elem)c->elem, c->dims, k))
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    s = cdb_index_create(&c->graph, &db->pager, store, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&c->reverse, &db->pager, store, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                             &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&c->pending, &db->pager, store, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work);
    if (s == CDB_OK) {
        /* Every vector already there is queued. */
        cdb_index_cursor cur;
        uint8_t key[8], val[ENTRY_VAL];
        size_t klen, vlen;
        int done;
        s = cdb_index_seek(&cur, &c->entries, vbuf(db, V_CURSOR), NULL, 0);
        while (s == CDB_OK) {
            if ((s = cdb_index_next(&cur, key, sizeof key, &klen, val, sizeof val, &vlen, &done)) != CDB_OK || done)
                break;
            if ((s = pending_put(c, cdb_load_u64(key, CDB_ORDER_BIG))) == CDB_OK)
                live++;
        }
    }
    c->graph_on = 1;
    c->k = k;
    if (s == CDB_OK)
        s = meta_put(c, live);
    if (s == CDB_OK)
        s = cdb_catalog_get_store(&db->catalog, store, &info);
    if (s == CDB_OK)
        s = decode_settings(db, info.vector, info.vector_len, &tmp, roots, &cfg, model, version);
    if (s == CDB_OK)
        s = encode_settings(db, &cfg, c, settings, sizeof settings, &slen);
    if (s == CDB_OK)
        s = cdb_catalog_set_vector(&db->catalog, store, settings, slen);
    if (s != CDB_OK)
        c->graph_on = 0; /* the pages written so far are unreferenced: space, not damage */
    return cdb_db_catalog_end(db, s);
}

/* Counts the pending set by walking it. */
static cdb_status count_pending(cdb_db *db, coll *c, size_t *n)
{
    cdb_index_cursor cur;
    uint8_t key[8], none[1];
    size_t klen, vlen;
    int done;
    cdb_status s;
    *n = 0;
    if ((s = cdb_index_seek(&cur, &c->pending, vbuf(db, V_CURSOR), NULL, 0)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_index_next(&cur, key, sizeof key, &klen, none, sizeof none, &vlen, &done)) != CDB_OK || done)
            return s;
        (*n)++;
    }
}

/* Run like a catalog change: reconcile first, refresh, commit the pages,
 * then rotate the journal. */
cdb_status cdb_vec_refresh(cdb_db *db, uint64_t store, size_t budget, size_t *remaining)
{
    size_t done;
    coll *c;
    cdb_status s;
    if (db == NULL)
        return CDB_E_INVALID_ARG;
    cdb_db_lock(db);
    if (db->in_tx || db->failed) {
        cdb_db_unlock(db);
        return CDB_E_STATE;
    }
    if ((s = coll_get(db, store, &c)) != CDB_OK) {
        cdb_db_unlock(db);
        return s;
    }
    if (!c->graph_on) {
        cdb_db_unlock(db);
        return CDB_E_STATE;
    }
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK) {
        cdb_db_unlock(db);
        return s;
    }
    s = refresh_coll(db, c, budget, &done);
    if (s == CDB_OK && db->pager.dirty)
        s = cdb_pager_commit(&db->pager);
    if ((s = cdb_db_catalog_end(db, s)) != CDB_OK) {
        cdb_db_unlock(db);
        return s;
    }
    if (remaining)
        s = count_pending(db, c, remaining);
    cdb_db_unlock(db);
    return s;
}

static void vec_refresh_worker_fn(void *arg)
{
    cdb_vec_refresh_task *t = (cdb_vec_refresh_task *)arg;
    if (t == NULL || t->db == NULL)
        return;
    t->status = cdb_vec_refresh(t->db, t->store, t->budget, &t->remaining);
}

cdb_status cdb_vec_refresh_async(cdb_db *db, cdb_vec_refresh_task *task, uint64_t *task_id)
{
    if (db == NULL || task == NULL)
        return CDB_E_INVALID_ARG;

    task->db = db;
    task->status = CDB_OK;
    task->remaining = 0;
    return cdb_worker_pool_submit(&db->workers, vec_refresh_worker_fn, task, task_id);
}

/* Copies the stored list (base-measure distances, as stored in f32) and
 * the neighbours' source states. CDB_E_STATE without a graph. */
cdb_status cdb_vec_neighbours(cdb_db *db, uint64_t store, uint64_t entry, cdb_vec_hit *hits, size_t max,
                              size_t *n, int *fresh)
{
    entry_rec er;
    grec r;
    uint32_t i;
    coll *c;
    int found, pend;
    cdb_status s;
    *n = 0;
    *fresh = 0;
    if ((s = coll_get(db, store, &c)) != CDB_OK)
        return s;
    if (!c->graph_on)
        return CDB_E_STATE;
    if (hits == NULL && max)
        return CDB_E_INVALID_ARG;
    if ((s = entry_get(c, entry, &er, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if ((s = pending_has(c, entry, &pend)) != CDB_OK || (s = rec_get(c, entry, &r, &found)) != CDB_OK)
        return s;
    *fresh = !pend;
    for (i = 0; found && i < r.count && *n < max; i++) {
        hits[*n].entry = r.n[i].entry;
        hits[*n].distance = r.n[i].dist;
        (*n)++;
    }
    return source_states(db, c, hits, *n);
}
