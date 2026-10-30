/* Time series (spec 10, 14.11). */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_ts.h"
#include "cdb_varint.h"

/* Settings fields (spec 14.11.1), in store record field 16. */
enum { S_LATE_MODE = 1, S_LATENESS, S_KEYS, S_SERIES, S_BLOCKS, S_HELD };

#define REC_HDR 48u            /* series record before its fields */
#define REC_MAX (REC_HDR + CDB_TS_MAX_FIELDS * 18u)
#define BLOCK_VAL 20u          /* page (8), count (4), last timestamp (8) */
#define KEY_MAX 512u           /* series key buffer; the keys index's own limit applies first */
#define DEFAULT_WINDOW_NS INT64_C(3600000000000) /* one hour */

typedef struct cdb_ts_store ts_store;

/* -------------------------------------------------------- buffers */

enum { B_READ = 0, B_OUT = 1, B_CURSOR = 2 };

static uint8_t *tbuf(cdb_db *db, unsigned i)
{
    return db->cfg.ts_buffers + (size_t)i * db->pager.page_size;
}

static size_t capacity(const cdb_db *db)
{
    return cdb_page_capacity(db->pager.fmt, db->pager.page_size);
}

size_t cdb_ts_work_size(void)
{
    return (CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point);
}

static cdb_ts_point *work(cdb_db *db)
{
    return (cdb_ts_point *)(void *)db->cfg.ts_work;
}

/* -------------------------------------------------------- settings and stores */

static cdb_status encode_settings(cdb_db *db, const ts_store *t, uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, out, cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, S_LATE_MODE); cdb_value_write_int(&w, t->late_mode);
    cdb_value_field(&w, S_LATENESS);  cdb_value_write_int(&w, t->lateness);
    cdb_value_field(&w, S_KEYS);      cdb_value_write_int(&w, (int64_t)t->keys.root);
    cdb_value_field(&w, S_SERIES);    cdb_value_write_int(&w, (int64_t)t->series.root);
    cdb_value_field(&w, S_BLOCKS);    cdb_value_write_int(&w, (int64_t)t->blocks.root);
    cdb_value_field(&w, S_HELD);      cdb_value_write_int(&w, (int64_t)t->held.root);
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

static cdb_status get_int(const cdb_value *obj, cdb_byte_order o, uint64_t f, int64_t lo, int64_t hi, int64_t *out)
{
    cdb_value v;
    int found;
    if (cdb_value_object_get(obj, o, f, &v, &found) != CDB_OK || !found || v.type != CDB_TYPE_INT || v.as.i64 < lo
        || v.as.i64 > hi)
        return CDB_E_CORRUPT;
    *out = v.as.i64;
    return CDB_OK;
}

/* Records the series counter, staged for reconciliation. */
static cdb_status save_store(cdb_db *db, void *slot)
{
    ts_store *t = (ts_store *)slot;
    cdb_status s;
    if (!t->dirty)
        return CDB_OK;
    if ((s = cdb_catalog_stage_next_entry(&db->catalog, t->slot.store_id, t->next_entry)) != CDB_OK)
        return s;
    t->dirty = 0;
    return CDB_OK;
}

/* Held points stay unseen until reconciliation folds them in, which only
 * reaches open stores. */
static int store_pinned(const void *slot)
{
    return ((const ts_store *)slot)->held_any;
}

const cdb_slot_ops cdb_ts_slot_ops = { store_pinned, save_store };

static cdb_status ts_get(cdb_db *db, uint64_t store, ts_store **out)
{
    ts_store *free_slot = (ts_store *)cdb_slots_find(&db->ts_slots, store);
    cdb_store_info info;
    cdb_value obj;
    int64_t v[6];
    cdb_byte_order o = db->pager.order;
    unsigned i;
    cdb_status s;
    void *slot;

    if (free_slot != NULL) {
        cdb_slots_touch(db, &db->ts_slots, free_slot);
        *out = free_slot;
        return CDB_OK;
    }
    if (!db->has_catalog || db->cfg.ts_buffers == NULL || db->cfg.ts_work == NULL
        || db->cfg.ts_work_size < cdb_ts_work_size())
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    if (info.model != CDB_MODEL_TIME_SERIES)
        return CDB_E_INVALID_ARG;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if (cdb_value_read(info.vector, info.vector_len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    for (i = 0; i < 6; i++)
        if ((s = get_int(&obj, o, i + 1u, i == 0 ? 0 : i == 1 ? 0 : 1, i == 0 ? CDB_TS_LATE_HOLD : INT64_MAX, &v[i]))
            != CDB_OK)
            return s;
    if ((s = cdb_slots_claim(db, &db->ts_slots, &slot)) != CDB_OK)
        return s;
    free_slot = (ts_store *)slot;
    free_slot->slot.store_id = store;
    free_slot->next_entry = info.next_entry_id ? info.next_entry_id : 1;
    free_slot->late_mode = (uint8_t)v[0];
    free_slot->lateness = v[1];
    if ((s = cdb_index_open(&free_slot->keys, &db->pager, (uint64_t)v[2], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->series, &db->pager, (uint64_t)v[3], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->blocks, &db->pager, (uint64_t)v[4], &db->cfg.catalog_work)) != CDB_OK
        || (s = cdb_index_open(&free_slot->held, &db->pager, (uint64_t)v[5], &db->cfg.catalog_work)) != CDB_OK)
        return s;
    {
        /* Whether late points wait to be folded in; a value too large for
         * no buffer at all is one. */
        cdb_index_cursor c;
        uint8_t k[16];
        size_t klen, vlen;
        int done = 1;
        s = cdb_index_seek(&c, &free_slot->held, tbuf(db, B_CURSOR), NULL, 0);
        if (s == CDB_OK)
            s = cdb_index_next(&c, k, sizeof k, &klen, NULL, 0, &vlen, &done);
        if (s == CDB_E_BUFFER_TOO_SMALL)
            done = 0;
        else if (s != CDB_OK)
            return s;
        free_slot->held_any = !done;
    }
    free_slot->slot.in_use = 1;
    cdb_slots_touch(db, &db->ts_slots, free_slot);
    *out = free_slot;
    return CDB_OK;
}

int cdb_ts_is_open(cdb_db *db, uint64_t store)
{
    return cdb_slots_find(&db->ts_slots, store) != NULL;
}

cdb_status cdb_ts_create(cdb_db *db, const char *name, size_t len, unsigned late_mode, int64_t lateness,
                         uint64_t *store)
{
    uint8_t settings[CDB_CATALOG_SETTINGS_MAX];
    size_t slen;
    uint64_t existing, got;
    int found;
    ts_store t;
    cdb_status s;

    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if (!db->has_catalog || db->cfg.ts_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if (late_mode > CDB_TS_LATE_HOLD || lateness < 0)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_find_store(&db->catalog, name, len, &existing, &found)) != CDB_OK)
        return cdb_db_catalog_end(db, s);
    if (found)
        return cdb_db_catalog_end(db, CDB_E_EXISTS);
    memset(&t, 0, sizeof t);
    t.late_mode = (uint8_t)late_mode;
    t.lateness = lateness;
    *store = cdb_catalog_next_store_id(&db->catalog);
    s = cdb_index_create(&t.keys, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX, &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&t.series, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&t.blocks, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                             &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = cdb_index_create(&t.held, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX,
                             &db->cfg.catalog_work);
    if (s == CDB_OK)
        s = encode_settings(db, &t, settings, sizeof settings, &slen);
    if (s == CDB_OK)
        s = cdb_catalog_create_store_ex(&db->catalog, name, len, CDB_MODEL_TIME_SERIES, settings, slen, &got);
    if (s == CDB_OK && got != *store)
        s = CDB_E_CORRUPT;
    return cdb_db_catalog_end(db, s);
}

cdb_status cdb_ts_flush(cdb_db *db);

/* -------------------------------------------------------- keys and records */

static size_t k_u64(uint8_t *k, uint64_t v)
{
    cdb_store_u64(k, v, CDB_ORDER_BIG);
    return 8;
}

/* (series, timestamp): the timestamp big-endian with its sign bit flipped. */
static size_t k_series_ts(uint8_t *k, uint64_t series, int64_t ts)
{
    cdb_store_u64(k, series, CDB_ORDER_BIG);
    cdb_store_u64(k + 8, (uint64_t)ts ^ UINT64_C(0x8000000000000000), CDB_ORDER_BIG);
    return 16;
}

static int64_t key_ts(const uint8_t *k)
{
    return (int64_t)(cdb_load_u64(k + 8, CDB_ORDER_BIG) ^ UINT64_C(0x8000000000000000));
}

/* The series key: the name, then each tag's key and value, tags sorted by
 * key (a key may appear once). */
static cdb_status series_key(const char *name, size_t len, const cdb_ts_tag *tags, size_t ntags, size_t max,
                             uint8_t *out, size_t *klen)
{
    size_t order[CDB_TS_MAX_TAGS], i, j;
    cdb_key_builder kb;
    if (name == NULL || len == 0 || ntags > CDB_TS_MAX_TAGS || (tags == NULL && ntags))
        return CDB_E_INVALID_ARG;
    for (i = 0; i < ntags; i++) {
        size_t at = i;
        if (tags[i].key == NULL || tags[i].value == NULL)
            return CDB_E_INVALID_ARG;
        while (at > 0) { /* insertion sort by key bytes */
            const cdb_ts_tag *a = &tags[order[at - 1]], *b = &tags[i];
            size_t m = a->key_len < b->key_len ? a->key_len : b->key_len;
            int c = memcmp(a->key, b->key, m);
            if (c == 0 && a->key_len == b->key_len)
                return CDB_E_INVALID_ARG; /* a tag key twice */
            if (c < 0 || (c == 0 && a->key_len < b->key_len))
                break;
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    cdb_key_init(&kb, out, max);
    if (cdb_key_string(&kb, name, len) != CDB_OK)
        return CDB_E_OVERFLOW;
    for (j = 0; j < ntags; j++) {
        const cdb_ts_tag *t = &tags[order[j]];
        if (cdb_key_string(&kb, t->key, t->key_len) != CDB_OK || cdb_key_string(&kb, t->value, t->value_len) != CDB_OK)
            return CDB_E_OVERFLOW;
    }
    *klen = kb.len;
    return CDB_OK;
}

/* A series record: its definition, its last point and its latest block's
 * encoder state. */
typedef struct srec {
    cdb_ts_def def;
    int has_points;
    int64_t last_ts;       /* the newest timestamp ever stored */
    int64_t last_block;    /* the key timestamp of the newest block */
    cdb_tsblock_state st;  /* after that block's last point */
} srec;

static size_t state_bytes(uint8_t type)
{
    switch (type) {
    case CDB_TYPE_INT: return 8;
    case CDB_TYPE_F64:
    case CDB_TYPE_F32: return 9;
    case CDB_TYPE_STRING: return 4;
    default: return 0;
    }
}

static cdb_status rec_put(ts_store *t, uint64_t series, const srec *r)
{
    uint8_t k[8], v[REC_MAX];
    size_t n = REC_HDR, i;
    memset(v, 0, REC_HDR);
    v[0] = r->def.precision;
    v[1] = (uint8_t)(r->def.change_of_value ? 1 : 0);
    v[2] = (uint8_t)r->def.nfields;
    v[3] = (uint8_t)(r->has_points ? 1 : 0);
    cdb_store_u64(v + 4, (uint64_t)r->def.window, CDB_ORDER_BIG);
    cdb_store_u64(v + 12, (uint64_t)r->last_ts, CDB_ORDER_BIG);
    cdb_store_u64(v + 20, (uint64_t)r->last_block, CDB_ORDER_BIG);
    cdb_store_u32(v + 28, r->st.count, CDB_ORDER_BIG);
    cdb_store_u64(v + 32, (uint64_t)r->st.t.prev_t, CDB_ORDER_BIG);
    cdb_store_u64(v + 40, (uint64_t)r->st.t.prev_delta, CDB_ORDER_BIG);
    for (i = 0; i < r->def.nfields; i++) {
        uint8_t type = r->def.fields[i].type;
        cdb_store_u64(v + n, r->def.fields[i].id, CDB_ORDER_BIG);
        v[n + 8] = type;
        n += 9;
        if (type == CDB_TYPE_INT) {
            cdb_store_u64(v + n, (uint64_t)r->st.ints[i].prev, CDB_ORDER_BIG);
        } else if (type == CDB_TYPE_F64 || type == CDB_TYPE_F32) {
            cdb_store_u64(v + n, r->st.floats[i].prev_bits, CDB_ORDER_BIG);
            v[n + 8] = (uint8_t)(r->st.floats[i].started ? 1 : 0);
        } else if (type == CDB_TYPE_STRING) {
            cdb_store_u32(v + n, r->st.dict_n[i], CDB_ORDER_BIG);
        }
        n += state_bytes(type);
    }
    return cdb_index_put(&t->series, k, k_u64(k, series), v, n);
}

static cdb_status rec_get(ts_store *t, uint64_t series, srec *r, int *found)
{
    uint8_t k[8], v[REC_MAX];
    size_t vlen, n = REC_HDR, i;
    cdb_status s = cdb_index_get(&t->series, k, k_u64(k, series), v, sizeof v, &vlen, found);
    if (s != CDB_OK || !*found)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
    memset(r, 0, sizeof *r);
    if (vlen < REC_HDR || v[0] > CDB_TS_NANOS || v[1] > 1 || v[2] > CDB_TS_MAX_FIELDS || v[3] > 1)
        return CDB_E_CORRUPT;
    r->def.precision = v[0];
    r->def.change_of_value = v[1];
    r->def.nfields = v[2];
    r->has_points = v[3];
    r->def.window = (int64_t)cdb_load_u64(v + 4, CDB_ORDER_BIG);
    r->last_ts = (int64_t)cdb_load_u64(v + 12, CDB_ORDER_BIG);
    r->last_block = (int64_t)cdb_load_u64(v + 20, CDB_ORDER_BIG);
    r->st.count = cdb_load_u32(v + 28, CDB_ORDER_BIG);
    r->st.t.prev_t = (int64_t)cdb_load_u64(v + 32, CDB_ORDER_BIG);
    r->st.t.prev_delta = (int64_t)cdb_load_u64(v + 40, CDB_ORDER_BIG);
    for (i = 0; i < r->def.nfields; i++) {
        uint8_t type;
        if (vlen - n < 9)
            return CDB_E_CORRUPT;
        r->def.fields[i].id = cdb_load_u64(v + n, CDB_ORDER_BIG);
        type = r->def.fields[i].type = v[n + 8];
        n += 9;
        if (vlen - n < state_bytes(type))
            return CDB_E_CORRUPT;
        if (type == CDB_TYPE_INT) {
            r->st.ints[i].prev = (int64_t)cdb_load_u64(v + n, CDB_ORDER_BIG);
        } else if (type == CDB_TYPE_F64 || type == CDB_TYPE_F32) {
            r->st.floats[i].prev_bits = cdb_load_u64(v + n, CDB_ORDER_BIG);
            r->st.floats[i].started = v[n + 8];
        } else if (type == CDB_TYPE_STRING) {
            r->st.dict_n[i] = cdb_load_u32(v + n, CDB_ORDER_BIG);
        }
        n += state_bytes(type);
    }
    return n == vlen && r->def.window > 0 ? CDB_OK : CDB_E_CORRUPT;
}

/* The start of ts's window: the multiple of w at or below ts (clamped at
 * the bottom of the range). */
static int64_t window_of(int64_t ts, int64_t w)
{
    int64_t q = ts / w;
    if (ts % w < 0)
        q--;
    if (q < INT64_MIN / w)
        return INT64_MIN;
    return q * w;
}

static int64_t window_end(int64_t start, int64_t w)
{
    return start > INT64_MAX - w ? INT64_MAX : start + w;
}

static void layout_of(const srec *r, uint64_t series, int64_t ts, cdb_ts_layout *L)
{
    memset(L, 0, sizeof *L);
    L->series = series;
    L->window_start = window_of(ts, r->def.window);
    L->window_end = window_end(L->window_start, r->def.window);
    L->precision = r->def.precision;
    L->flags = r->def.change_of_value ? (uint8_t)CDB_TS_BLOCK_COV : 0u;
    L->nfields = r->def.nfields;
    memcpy(L->fields, r->def.fields, sizeof L->fields);
}

/* A point's values: an object of field ID -> value, each of its field's type.
 * The point refers to bytes in obj. */
static cdb_status point_of(const cdb_ts_def *def, int64_t ts, const uint8_t *obj, size_t len, cdb_byte_order o,
                           cdb_ts_point *p)
{
    cdb_value v, fv;
    cdb_value_iter it;
    uint64_t fid;
    int done;
    unsigned i = 0;
    memset(p, 0, sizeof *p);
    p->ts = ts;
    if (cdb_value_validate(obj, len, o, CDB_DECIMAL_UNBOUNDED, 0) != CDB_OK || cdb_value_read(obj, len, o, &v) != CDB_OK
        || v.type != CDB_TYPE_OBJECT || cdb_value_object_iter(&v, o, &it) != CDB_OK)
        return CDB_E_INVALID_ARG;
    for (;;) {
        cdb_ts_val *x;
        if (cdb_value_object_next(&it, &fid, &fv, &done) != CDB_OK)
            return CDB_E_INVALID_ARG;
        if (done)
            return CDB_OK;
        while (i < def->nfields && def->fields[i].id < fid)
            i++;
        if (i == def->nfields || def->fields[i].id != fid || def->fields[i].type != (uint8_t)fv.type)
            return CDB_E_INVALID_ARG; /* an unknown field, or a value of the wrong type */
        x = &p->v[i];
        x->present = 1;
        switch (fv.type) {
        case CDB_TYPE_INT: x->bits = (uint64_t)fv.as.i64; break;
        case CDB_TYPE_F64: memcpy(&x->bits, &fv.as.f64, 8); break;
        case CDB_TYPE_F32: {
            uint32_t b;
            memcpy(&b, &fv.as.f32, 4);
            x->bits = b;
            break;
        }
        case CDB_TYPE_BOOL: x->bits = fv.as.boolean ? 1u : 0u; break;
        case CDB_TYPE_STRING:
            x->bytes = fv.payload;
            x->len = fv.payload_len;
            break;
        default:
            x->bytes = fv.start;
            x->len = fv.encoded_len;
            break;
        }
        i++;
    }
}

static int same_values(const cdb_ts_def *def, const cdb_ts_point *a, const cdb_ts_point *b)
{
    unsigned i;
    for (i = 0; i < def->nfields; i++) {
        const cdb_ts_val *x = &a->v[i], *y = &b->v[i];
        if ((x->present != 0) != (y->present != 0))
            return 0;
        if (!x->present)
            continue;
        if (x->bits != y->bits || x->len != y->len || (x->len && memcmp(x->bytes, y->bytes, x->len) != 0))
            return 0;
    }
    return 1;
}

/* -------------------------------------------------------- payloads */

/* INSERT (create a series): precision, change-of-value, window (8),
 * nfields, then (field ID (8), type) each, then the key length (2) and key.
 * PATCH (append): a count (varint), then per point the timestamp (8) and the
 * object's length (varint) and bytes. DELETE (drop before): a timestamp (8).
 * Big-endian. */

static size_t encode_create(const cdb_ts_def *def, const uint8_t *key, size_t klen, uint8_t *out)
{
    size_t n = 0, i;
    out[n++] = def->precision;
    out[n++] = (uint8_t)(def->change_of_value ? 1 : 0);
    cdb_store_u64(out + n, (uint64_t)def->window, CDB_ORDER_BIG);
    n += 8;
    out[n++] = (uint8_t)def->nfields;
    for (i = 0; i < def->nfields; i++) {
        cdb_store_u64(out + n, def->fields[i].id, CDB_ORDER_BIG);
        out[n + 8] = def->fields[i].type;
        n += 9;
    }
    cdb_store_u16(out + n, (uint16_t)klen, CDB_ORDER_BIG);
    memcpy(out + n + 2, key, klen);
    return n + 2 + klen;
}

static int decode_create(const uint8_t *p, size_t len, cdb_ts_def *def, const uint8_t **key, size_t *klen)
{
    size_t n = 11, i;
    memset(def, 0, sizeof *def);
    if (len < 11 || p[0] > CDB_TS_NANOS || p[1] > 1 || p[10] > CDB_TS_MAX_FIELDS)
        return 0;
    def->precision = p[0];
    def->change_of_value = p[1];
    def->window = (int64_t)cdb_load_u64(p + 2, CDB_ORDER_BIG);
    def->nfields = p[10];
    if (len - n < (size_t)def->nfields * 9u + 2u || def->window <= 0)
        return 0;
    for (i = 0; i < def->nfields; i++) {
        def->fields[i].id = cdb_load_u64(p + n, CDB_ORDER_BIG);
        def->fields[i].type = p[n + 8];
        n += 9;
    }
    *klen = cdb_load_u16(p + n, CDB_ORDER_BIG);
    *key = p + n + 2;
    return len == n + 2 + *klen;
}

/* The next point of an append payload at *at; 0 when malformed. */
static int next_point(const uint8_t *p, size_t len, size_t *at, int64_t *ts, const uint8_t **obj, size_t *olen)
{
    uint64_t l;
    size_t k;
    if (len - *at < 8)
        return 0;
    *ts = (int64_t)cdb_load_u64(p + *at, CDB_ORDER_BIG);
    *at += 8;
    if (cdb_varint_decode(p + *at, len - *at, &l, &k) != CDB_OK || l > len - *at - k)
        return 0;
    *obj = p + *at + k;
    *olen = (size_t)l;
    *at += k + (size_t)l;
    return 1;
}

/* -------------------------------------------------------- the open transaction */

typedef struct txrec {
    uint8_t op;
    uint64_t series;
    const uint8_t *data;
    size_t len;
} txrec;

/* This transaction's next record for the store, at or after *off. The
 * record is in record_buf until the next call. */
static cdb_status tx_next(cdb_db *db, ts_store *t, uint64_t *off, txrec *r, int *done)
{
    *done = 0;
    if (t->touched_tx != db->tx_id) {
        *done = 1;
        return CDB_OK;
    }
    while (*off < db->journal.end) {
        cdb_record rec;
        uint64_t next;
        cdb_status s = cdb_journal_read(&db->journal, *off, db->cfg.record_buf, db->cfg.record_buf_size, &rec, &next);
        if (s != CDB_OK)
            return s;
        *off = next;
        if (rec.kind == CDB_RECORD_DATA && rec.tx_id == db->tx_id && rec.store_id == t->slot.store_id
            && rec.payload_len >= 1) {
            r->op = rec.payload[0];
            r->series = rec.entry_id;
            r->data = rec.payload + 1;
            r->len = rec.payload_len - 1u;
            return CDB_OK;
        }
    }
    *done = 1;
    return CDB_OK;
}

/* A series as the open transaction sees it: the committed record, or the
 * definition from this transaction's create; last_ts follows its appends. */
static cdb_status tx_series(cdb_db *db, ts_store *t, uint64_t series, srec *r, int *found)
{
    uint64_t off = db->tx_start;
    txrec x;
    int done;
    cdb_status s = rec_get(t, series, r, found);
    if (s != CDB_OK)
        return s;
    for (;;) {
        if ((s = tx_next(db, t, &off, &x, &done)) != CDB_OK || done)
            return s;
        if (x.series != series)
            continue;
        if (x.op == CDB_OP_INSERT && !*found) {
            const uint8_t *key;
            size_t klen;
            if (!decode_create(x.data, x.len, &r->def, &key, &klen))
                return CDB_E_CORRUPT;
            *found = 1;
        } else if (x.op == CDB_OP_PATCH && *found) {
            uint64_t count;
            size_t at, k;
            if (cdb_varint_decode(x.data, x.len, &count, &k) != CDB_OK)
                return CDB_E_CORRUPT;
            at = k;
            while (count-- > 0) {
                int64_t ts;
                const uint8_t *obj;
                size_t olen;
                if (!next_point(x.data, x.len, &at, &ts, &obj, &olen))
                    return CDB_E_CORRUPT;
                if (!r->has_points || ts > r->last_ts)
                    r->last_ts = ts;
                r->has_points = 1;
            }
        }
    }
}

static cdb_status tx_store(cdb_db *db, uint64_t store, ts_store **t)
{
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if (db->cfg.apply != cdb_doc_apply)
        return CDB_E_UNSUPPORTED; /* only the built-in apply knows time series */
    return ts_get(db, store, t);
}

/* A series ID by key: committed, or created by this transaction. */
static cdb_status find_series(cdb_db *db, ts_store *t, const uint8_t *key, size_t klen, uint64_t *series, int *found)
{
    uint8_t v[8];
    size_t vlen;
    uint64_t off = db->tx_start;
    txrec x;
    int done;
    cdb_status s = cdb_index_get(&t->keys, key, klen, v, sizeof v, &vlen, found);
    if (s != CDB_OK)
        return s;
    if (*found) {
        if (vlen != 8)
            return CDB_E_CORRUPT;
        *series = cdb_load_u64(v, CDB_ORDER_BIG);
        return CDB_OK;
    }
    if (!db->in_tx)
        return CDB_OK;
    for (;;) {
        cdb_ts_def def;
        const uint8_t *k;
        size_t kl;
        if ((s = tx_next(db, t, &off, &x, &done)) != CDB_OK || done)
            return s;
        if (x.op == CDB_OP_INSERT && decode_create(x.data, x.len, &def, &k, &kl) && kl == klen
            && memcmp(k, key, klen) == 0) {
            *series = x.series;
            *found = 1;
            return CDB_OK;
        }
    }
}

/* -------------------------------------------------------- the API */

static cdb_status check_def(const cdb_ts_def *def)
{
    unsigned i;
    if (def->precision > CDB_TS_NANOS || def->window < 0 || def->nfields > CDB_TS_MAX_FIELDS)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < def->nfields; i++)
        if (def->fields[i].type >= CDB_TYPE_UNKNOWN || (i > 0 && def->fields[i].id <= def->fields[i - 1].id))
            return CDB_E_INVALID_ARG;
    return CDB_OK;
}

/* One hour in the precision's ticks. */
static int64_t default_window(unsigned precision)
{
    int64_t w = DEFAULT_WINDOW_NS;
    unsigned p;
    for (p = precision; p < CDB_TS_NANOS; p++)
        w /= 1000;
    return w;
}

cdb_status cdb_ts_series(cdb_db *db, uint64_t store, const char *name, size_t len, const cdb_ts_tag *tags,
                         size_t ntags, const cdb_ts_def *def, uint64_t *series, int *created)
{
    uint8_t key[KEY_MAX], payload[16 + CDB_TS_MAX_FIELDS * 9u + KEY_MAX];
    size_t klen;
    int found;
    ts_store *t;
    cdb_ts_def d;
    cdb_status s;

    *created = 0;
    if ((s = def ? tx_store(db, store, &t) : ts_get(db, store, &t)) != CDB_OK)
        return s;
    if ((s = series_key(name, len, tags, ntags, t->keys.max_key < KEY_MAX ? t->keys.max_key : KEY_MAX, key, &klen))
        != CDB_OK)
        return s;
    if ((s = find_series(db, t, key, klen, series, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_OK;
    if (def == NULL)
        return CDB_E_NOT_FOUND;
    if ((s = check_def(def)) != CDB_OK)
        return s;
    d = *def;
    if (d.window == 0)
        d.window = default_window(d.precision);
    *series = t->next_entry;
    if ((s = cdb_tx_put(db, store, *series, CDB_OP_INSERT, payload, encode_create(&d, key, klen, payload))) != CDB_OK)
        return s;
    t->next_entry++;
    t->dirty = 1;
    t->touched_tx = db->tx_id;
    *created = 1;
    return CDB_OK;
}

cdb_status cdb_ts_describe(cdb_db *db, uint64_t store, uint64_t series, cdb_ts_def *def)
{
    ts_store *t;
    srec r;
    int found;
    cdb_status s;
    if ((s = ts_get(db, store, &t)) != CDB_OK)
        return s;
    if ((s = db->in_tx ? tx_series(db, t, series, &r, &found) : rec_get(t, series, &r, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    *def = r.def;
    return CDB_OK;
}

/* Clock nanoseconds in the precision's ticks. */
static int64_t ticks_of(int64_t ns, unsigned precision)
{
    unsigned p;
    for (p = precision; p < CDB_TS_NANOS; p++)
        ns /= 1000;
    return ns;
}

static int64_t ticks_to_ns(int64_t ticks, unsigned precision)
{
    unsigned p;
    for (p = precision; p < CDB_TS_NANOS; p++) {
        if (ticks > INT64_MAX / 1000 || ticks < INT64_MIN / 1000)
            return ticks < 0 ? INT64_MIN : INT64_MAX;
        ticks *= 1000;
    }
    return ticks;
}

cdb_status cdb_ts_append(cdb_db *db, uint64_t store, uint64_t series, const cdb_ts_in *points, size_t n)
{
    uint8_t *p;
    size_t at = 0, i, room;
    ts_store *t;
    srec r;
    int found;
    cdb_status s;

    if ((s = tx_store(db, store, &t)) != CDB_OK)
        return s;
    if (n == 0)
        return CDB_OK;
    if (points == NULL)
        return CDB_E_INVALID_ARG;
    if ((s = tx_series(db, t, series, &r, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    /* The payload is built in the output page buffer... unless it is too big
     * for one record: then refuse, the caller sends fewer points at once. */
    p = tbuf(db, B_OUT);
    room = db->cfg.record_buf_size - CDB_RECORD_HEADER_SIZE - CDB_RECORD_TRAILER_SIZE - 1u;
    if (room > db->pager.page_size)
        room = db->pager.page_size;
    at = cdb_varint_encode(n, p);
    for (i = 0; i < n; i++) {
        cdb_ts_point pt;
        int64_t ts = points[i].ts;
        if (points[i].use_clock) {
            int64_t now;
            int have;
            if ((s = cdb_db_now(db, &now, &have)) != CDB_OK)
                return s;
            if (!have)
                return CDB_E_UNSUPPORTED; /* no timestamp and no clock (spec 10.2) */
            ts = ticks_of(now, r.def.precision);
        }
        if ((s = point_of(&r.def, ts, (const uint8_t *)points[i].values, points[i].len, db->pager.order, &pt)) != CDB_OK)
            return s;
        if (r.has_points && ts < r.last_ts) {
            int64_t late_ns = ticks_to_ns(r.last_ts, r.def.precision) - ticks_to_ns(ts, r.def.precision);
            if (t->late_mode == CDB_TS_LATE_REJECT || (t->late_mode == CDB_TS_LATE_WITHIN && late_ns > t->lateness))
                return CDB_E_STATE; /* a late point the store refuses (spec 10.5) */
        }
        if (room - at < 8u + CDB_VARINT_MAX_LEN + points[i].len)
            return CDB_E_OVERFLOW;
        cdb_store_u64(p + at, (uint64_t)ts, CDB_ORDER_BIG);
        at += 8;
        at += cdb_varint_encode(points[i].len, p + at);
        memcpy(p + at, points[i].values, points[i].len);
        at += points[i].len;
        if (!r.has_points || ts > r.last_ts)
            r.last_ts = ts;
        r.has_points = 1;
    }
    if ((s = cdb_tx_put(db, store, series, CDB_OP_PATCH, p, at)) != CDB_OK)
        return s;
    t->touched_tx = db->tx_id;
    return CDB_OK;
}

cdb_status cdb_ts_drop_before(cdb_db *db, uint64_t store, uint64_t series, int64_t ts)
{
    uint8_t p[8];
    ts_store *t;
    srec r;
    int found;
    cdb_status s;
    if ((s = tx_store(db, store, &t)) != CDB_OK)
        return s;
    if ((s = tx_series(db, t, series, &r, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    cdb_store_u64(p, (uint64_t)ts, CDB_ORDER_BIG);
    if ((s = cdb_tx_put(db, store, series, CDB_OP_DELETE, p, sizeof p)) != CDB_OK)
        return s;
    t->touched_tx = db->tx_id;
    return CDB_OK;
}

/* -------------------------------------------------------- blocks */

typedef struct blk {
    uint64_t lid;
    uint32_t count;
    int64_t first, last;
} blk;

static cdb_status blk_put(ts_store *t, uint64_t series, const blk *b)
{
    uint8_t k[16], v[BLOCK_VAL];
    cdb_store_u64(v, b->lid, CDB_ORDER_BIG);
    cdb_store_u32(v + 8, b->count, CDB_ORDER_BIG);
    cdb_store_u64(v + 12, (uint64_t)b->last, CDB_ORDER_BIG);
    return cdb_index_put(&t->blocks, k, k_series_ts(k, series, b->first), v, BLOCK_VAL);
}

static cdb_status blk_del(ts_store *t, uint64_t series, int64_t first)
{
    uint8_t k[16];
    int found;
    return cdb_index_delete(&t->blocks, k, k_series_ts(k, series, first), &found);
}

/* The series' first block whose key is at or after from; *found 0 when none. */
static cdb_status blk_at(cdb_db *db, ts_store *t, uint64_t series, int64_t from, blk *b, int *found)
{
    cdb_index_cursor c;
    uint8_t k[16], got[16], v[BLOCK_VAL];
    size_t klen, vlen;
    int done;
    cdb_status s;
    *found = 0;
    if ((s = cdb_index_seek(&c, &t->blocks, tbuf(db, B_CURSOR), k, k_series_ts(k, series, from))) != CDB_OK
        || (s = cdb_index_next(&c, got, sizeof got, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
    if (done || klen != 16 || cdb_load_u64(got, CDB_ORDER_BIG) != series)
        return CDB_OK;
    if (vlen != BLOCK_VAL)
        return CDB_E_CORRUPT;
    b->first = key_ts(got);
    b->lid = cdb_load_u64(v, CDB_ORDER_BIG);
    b->count = cdb_load_u32(v + 8, CDB_ORDER_BIG);
    b->last = (int64_t)cdb_load_u64(v + 12, CDB_ORDER_BIG);
    *found = 1;
    return CDB_OK;
}

/* Reads block b into the read buffer; *pl is its payload. */
static cdb_status blk_read(cdb_db *db, const ts_store *t, const blk *b, const uint8_t **pl, size_t *len)
{
    cdb_page_header h;
    cdb_status s = cdb_pager_read(&db->pager, b->lid, tbuf(db, B_READ), &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    if (h.type != CDB_PAGE_TYPE_TS_BLOCK || h.store_id != t->slot.store_id)
        return CDB_E_CORRUPT;
    *pl = tbuf(db, B_READ) + cdb_page_header_size(db->pager.fmt);
    *len = h.payload_len;
    return CDB_OK;
}

static cdb_status blk_write(cdb_db *db, const ts_store *t, uint64_t lid, const uint8_t *pl, size_t len)
{
    cdb_page_header h;
    memset(&h, 0, sizeof h);
    h.type = CDB_PAGE_TYPE_TS_BLOCK;
    h.store_id = t->slot.store_id;
    return cdb_pager_write(&db->pager, lid, &h, pl, len);
}

/* Writes points [0, n) of the work array (or the one point p) as new
 * blocks, splitting while they don't fit; *last gets the last block's key
 * and state when st is given. reuse: a page to write the first block to. */
static cdb_status write_points(cdb_db *db, ts_store *t, const srec *r, uint64_t series, const cdb_ts_point *pts,
                               size_t n, int late, uint64_t reuse, int64_t *last_key, cdb_tsblock_state *st)
{
    size_t done = 0;
    cdb_ts_layout L;
    cdb_status s;
    while (done < n) {
        size_t take = n - done, len = 0;
        blk b = { 0, 0, 0, 0 };
        if (take > CDB_TS_BLOCK_MAX_POINTS)
            take = CDB_TS_BLOCK_MAX_POINTS;
        layout_of(r, series, pts[done].ts, &L);
        if (late)
            L.flags |= CDB_TS_BLOCK_LATE;
        for (;;) {
            s = cdb_tsblock_build(&L, pts + done, take, db->pager.order, tbuf(db, B_OUT), capacity(db), &len, st);
            if (s != CDB_E_OVERFLOW || take == 1)
                break;
            take /= 2; /* split: halve until it fits */
        }
        if (s != CDB_OK)
            return s;
        b.lid = reuse;
        reuse = 0;
        if (b.lid == 0 && (s = cdb_pager_alloc(&db->pager, &b.lid)) != CDB_OK)
            return s;
        if ((s = blk_write(db, t, b.lid, tbuf(db, B_OUT), len)) != CDB_OK)
            return s;
        b.count = (uint32_t)take;
        b.first = pts[done].ts;
        b.last = pts[done + take - 1].ts;
        if ((s = blk_put(t, series, &b)) != CDB_OK)
            return s;
        if (last_key)
            *last_key = b.first;
        done += take;
    }
    return CDB_OK;
}

/* The block holding ts, or where it belongs: the last block of ts's window
 * starting at or before ts, else the window's first block. */
static cdb_status blk_for(cdb_db *db, ts_store *t, const srec *r, uint64_t series, int64_t ts, blk *b, int *found)
{
    int64_t ws = window_of(ts, r->def.window), we = window_end(ws, r->def.window), from = ws;
    blk cur = { 0, 0, 0, 0 };
    int f;
    cdb_status s;
    *found = 0;
    for (;;) {
        if ((s = blk_at(db, t, series, from, &cur, &f)) != CDB_OK)
            return s;
        if (!f || cur.first >= we || (cur.first > ts && *found))
            return CDB_OK;
        *b = cur;
        *found = 1;
        if (cur.first > ts || cur.first == INT64_MAX)
            return CDB_OK;
        from = cur.first + 1;
    }
}

/* -------------------------------------------------------- applying points */

static void collided(cdb_db *db, const ts_store *t, uint64_t series, int64_t ts)
{
    if (db->cfg.ts_collision)
        db->cfg.ts_collision(db->cfg.ts_collision_ctx, t->slot.store_id, series, ts);
}

/* Merges point p into the block it belongs to: a new point, a duplicate (no
 * change) or a collision (its fields replace the stored ones). */
static cdb_status merge_point(cdb_db *db, ts_store *t, srec *r, uint64_t series, const cdb_ts_point *p)
{
    cdb_ts_point *pts = work(db);
    const uint8_t *pl;
    size_t len, n, i;
    cdb_ts_layout L;
    blk b = { 0, 0, 0, 0 };
    int found, latest;
    cdb_status s;

    if ((s = blk_for(db, t, r, series, p->ts, &b, &found)) != CDB_OK)
        return s;
    if (!found)
        return write_points(db, t, r, series, p, 1, 1, 0, NULL, NULL); /* an empty window */
    if ((s = blk_read(db, t, &b, &pl, &len)) != CDB_OK
        || (s = cdb_tsblock_decode(pl, len, db->pager.order, &L, pts, CDB_TS_BLOCK_MAX_POINTS, &n, NULL)) != CDB_OK)
        return s;
    for (i = 0; i < n && pts[i].ts < p->ts; i++)
        ;
    if (i < n && pts[i].ts == p->ts) {
        unsigned f;
        cdb_ts_point merged = pts[i];
        if (same_values(&r->def, &pts[i], p))
            return CDB_OK; /* a true duplicate is stored once */
        for (f = 0; f < r->def.nfields; f++)
            if (p->v[f].present)
                merged.v[f] = p->v[f];
        if (same_values(&r->def, &pts[i], &merged))
            return CDB_OK; /* nothing changes */
        pts[i] = merged;
        collided(db, t, series, p->ts);
    } else {
        memmove(&pts[i + 1], &pts[i], (n - i) * sizeof *pts);
        pts[i] = *p;
        n++;
    }
    /* The block is written again; its key may change, and it may split. */
    latest = b.first == r->last_block;
    if ((s = blk_del(t, series, b.first)) != CDB_OK)
        return s;
    if (latest)
        return write_points(db, t, r, series, pts, n, 1, b.lid, &r->last_block, &r->st);
    return write_points(db, t, r, series, pts, n, 1, b.lid, NULL, NULL);
}

/* The latest block's last point, decoded into the work array's first slot. */
static cdb_status last_point(cdb_db *db, ts_store *t, const srec *r, uint64_t series, cdb_ts_point **last)
{
    cdb_ts_point *pts = work(db);
    const uint8_t *pl;
    size_t len, n;
    cdb_ts_layout L;
    blk b = { 0, 0, 0, 0 };
    int found;
    cdb_status s;
    if ((s = blk_at(db, t, series, r->last_block, &b, &found)) != CDB_OK)
        return s;
    if (!found || b.first != r->last_block)
        return CDB_E_CORRUPT;
    if ((s = blk_read(db, t, &b, &pl, &len)) != CDB_OK
        || (s = cdb_tsblock_decode(pl, len, db->pager.order, &L, pts, CDB_TS_BLOCK_MAX_POINTS, &n, NULL)) != CDB_OK)
        return s;
    *last = &pts[n - 1];
    return CDB_OK;
}

/* Applies one point. Never fails for logical reasons. */
static cdb_status apply_point(cdb_db *db, ts_store *t, srec *r, uint64_t series, const cdb_ts_point *p)
{
    cdb_status s;
    if (!r->has_points) {
        r->has_points = 1;
        r->last_ts = p->ts;
        return write_points(db, t, r, series, p, 1, 0, 0, &r->last_block, &r->st);
    }
    if (p->ts > r->last_ts) {
        blk b = { 0, 0, 0, 0 };
        int found;
        if (r->def.change_of_value) {
            cdb_ts_point *last;
            if ((s = last_point(db, t, r, series, &last)) != CDB_OK)
                return s;
            if (same_values(&r->def, last, p))
                return CDB_OK; /* the value has not changed (spec 10.6) */
        }
        r->last_ts = p->ts;
        if (window_of(p->ts, r->def.window) == window_of(r->last_block, r->def.window)) {
            const uint8_t *pl;
            size_t len, out;
            cdb_tsblock_state st = r->st;
            if ((s = blk_at(db, t, series, r->last_block, &b, &found)) != CDB_OK)
                return s;
            if (!found || b.first != r->last_block)
                return CDB_E_CORRUPT;
            if ((s = blk_read(db, t, &b, &pl, &len)) != CDB_OK)
                return s;
            s = cdb_tsblock_append(pl, len, db->pager.order, &st, p, tbuf(db, B_OUT), capacity(db), &out);
            if (s == CDB_OK) {
                r->st = st;
                b.count++;
                b.last = p->ts;
                if ((s = blk_write(db, t, b.lid, tbuf(db, B_OUT), out)) != CDB_OK)
                    return s;
                return blk_put(t, series, &b);
            }
            if (s != CDB_E_OVERFLOW)
                return s;
        }
        return write_points(db, t, r, series, p, 1, 0, 0, &r->last_block, &r->st); /* a new block */
    }
    if (t->late_mode == CDB_TS_LATE_HOLD && p->ts < r->last_ts)
        return CDB_OK; /* held: the caller parks it */
    return merge_point(db, t, r, series, p);
}

/* Parks a late point until reconciliation (hold and forward). */
static cdb_status hold(ts_store *t, uint64_t series, int64_t ts, const uint8_t *obj, size_t len)
{
    uint8_t k[16];
    t->held_any = 1;
    return cdb_index_put(&t->held, k, k_series_ts(k, series, ts), obj, len);
}

/* Removes every point older than ts. */
static cdb_status drop_before(cdb_db *db, ts_store *t, srec *r, uint64_t series, int64_t ts)
{
    for (;;) {
        blk b = { 0, 0, 0, 0 };
        int found;
        cdb_status s = blk_at(db, t, series, INT64_MIN, &b, &found);
        if (s != CDB_OK)
            return s;
        if (!found || b.first >= ts)
            break;
        if ((s = blk_del(t, series, b.first)) != CDB_OK)
            return s;
        if (b.last < ts) {
            if ((s = cdb_pager_free(&db->pager, b.lid)) != CDB_OK)
                return s;
            if (b.first == r->last_block)
                r->has_points = 0; /* the newest block went: nothing is left */
        } else {
            cdb_ts_point *pts = work(db);
            const uint8_t *pl;
            size_t len, n, i;
            cdb_ts_layout L;
            int latest = b.first == r->last_block;
            if ((s = blk_read(db, t, &b, &pl, &len)) != CDB_OK
                || (s = cdb_tsblock_decode(pl, len, db->pager.order, &L, pts, CDB_TS_BLOCK_MAX_POINTS, &n, NULL))
                       != CDB_OK)
                return s;
            for (i = 0; i < n && pts[i].ts < ts; i++)
                ;
            s = latest ? write_points(db, t, r, series, pts + i, n - i, (L.flags & CDB_TS_BLOCK_LATE) != 0, b.lid,
                                      &r->last_block, &r->st)
                       : write_points(db, t, r, series, pts + i, n - i, (L.flags & CDB_TS_BLOCK_LATE) != 0, b.lid,
                                      NULL, NULL);
            if (s != CDB_OK)
                return s;
            break; /* later blocks are newer than ts */
        }
    }
    /* Held points older than ts go too. Their values (at most a page) are
     * read into the output buffer and not used. */
    for (;;) {
        cdb_index_cursor c;
        uint8_t k[16], got[16];
        size_t klen, vlen;
        int done, found;
        cdb_status s = cdb_index_seek(&c, &t->held, tbuf(db, B_CURSOR), k, k_series_ts(k, series, INT64_MIN));
        if (s == CDB_OK)
            s = cdb_index_next(&c, got, sizeof got, &klen, tbuf(db, B_OUT), db->pager.page_size, &vlen, &done);
        if (s != CDB_OK)
            return s;
        if (done || klen != 16 || cdb_load_u64(got, CDB_ORDER_BIG) != series || key_ts(got) >= ts)
            return CDB_OK;
        if ((s = cdb_index_delete(&t->held, got, 16, &found)) != CDB_OK)
            return s;
    }
}

/* -------------------------------------------------------- apply */

cdb_status cdb_ts_apply(cdb_db *db, const cdb_delta *d, int *handled)
{
    ts_store *t;
    srec r;
    int found;
    cdb_status s = ts_get(db, d->store_id, &t);
    *handled = 0;
    if (s == CDB_E_INVALID_ARG || s == CDB_E_UNSUPPORTED)
        return CDB_OK; /* not a time-series store */
    *handled = 1;
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK; /* dropped */
    if (s != CDB_OK)
        return s;
    if (d->entry_id >= t->next_entry) {
        t->next_entry = d->entry_id + 1; /* replay never hands an ID out again */
        t->dirty = 1;
    }
    if ((s = rec_get(t, d->entry_id, &r, &found)) != CDB_OK)
        return s;
    switch (d->op) {
    case CDB_OP_INSERT: {
        const uint8_t *key;
        size_t klen;
        uint8_t v[8];
        if (found || !decode_create(d->data, d->len, &r.def, &key, &klen))
            return CDB_OK;
        r.has_points = 0;
        r.last_ts = r.last_block = 0;
        memset(&r.st, 0, sizeof r.st);
        cdb_store_u64(v, d->entry_id, CDB_ORDER_BIG);
        if ((s = cdb_index_put(&t->keys, key, klen, v, sizeof v)) != CDB_OK)
            return s;
        return rec_put(t, d->entry_id, &r);
    }
    case CDB_OP_PATCH: {
        uint64_t count;
        size_t at, k;
        if (!found || cdb_varint_decode(d->data, d->len, &count, &k) != CDB_OK)
            return CDB_OK;
        at = k;
        while (count-- > 0) {
            cdb_ts_point p;
            int64_t ts;
            const uint8_t *obj;
            size_t olen;
            if (!next_point(d->data, d->len, &at, &ts, &obj, &olen))
                break;
            if (point_of(&r.def, ts, obj, olen, db->pager.order, &p) != CDB_OK)
                continue; /* checked when made: only a damaged record lands here */
            if (r.has_points && ts < r.last_ts && t->late_mode == CDB_TS_LATE_HOLD) {
                if ((s = hold(t, d->entry_id, ts, obj, olen)) != CDB_OK)
                    return s;
                continue;
            }
            if ((s = apply_point(db, t, &r, d->entry_id, &p)) != CDB_OK)
                return s;
        }
        return rec_put(t, d->entry_id, &r);
    }
    case CDB_OP_DELETE:
        if (!found || d->len != 8)
            return CDB_OK;
        if ((s = drop_before(db, t, &r, d->entry_id, (int64_t)cdb_load_u64(d->data, CDB_ORDER_BIG))) != CDB_OK)
            return s;
        return rec_put(t, d->entry_id, &r);
    default:
        return CDB_OK;
    }
}

/* At reconciliation: held points are folded into their blocks, oldest
 * first; entry counters reach the catalog. */
cdb_status cdb_ts_flush(cdb_db *db)
{
    cdb_slot_iter it;
    ts_store *t;
    for (t = (ts_store *)cdb_slots_first(&db->ts_slots, &it); t != NULL; t = (ts_store *)cdb_slots_next(&it)) {
        cdb_status s;
        for (;;) {
            /* The held point's values go in record_buf: reconciliation is
             * not reading the journal while it flushes. */
            uint8_t *obj = db->cfg.record_buf;
            cdb_index_cursor c;
            uint8_t k[16];
            size_t klen, vlen;
            int done, found;
            uint64_t series;
            cdb_ts_point p;
            srec r;
            if ((s = cdb_index_seek(&c, &t->held, tbuf(db, B_CURSOR), NULL, 0)) != CDB_OK
                || (s = cdb_index_next(&c, k, sizeof k, &klen, obj, db->cfg.record_buf_size, &vlen, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            if (klen != 16)
                return CDB_E_CORRUPT;
            series = cdb_load_u64(k, CDB_ORDER_BIG);
            if ((s = cdb_index_delete(&t->held, k, 16, &found)) != CDB_OK || (s = rec_get(t, series, &r, &found)) != CDB_OK)
                return s;
            if (!found || point_of(&r.def, key_ts(k), obj, vlen, db->pager.order, &p) != CDB_OK)
                continue;
            if ((s = merge_point(db, t, &r, series, &p)) != CDB_OK || (s = rec_put(t, series, &r)) != CDB_OK)
                return s;
        }
        t->held_any = 0;
        if ((s = save_store(db, t)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- reading */

static cdb_status write_row(const srec *r, const cdb_ts_point *p, cdb_byte_order o, uint8_t *out, size_t cap,
                            size_t *len)
{
    cdb_value_writer w;
    unsigned f;
    cdb_value_writer_init(&w, out, cap, o, CDB_DECIMAL_UNBOUNDED);
    cdb_value_begin_object(&w);
    for (f = 0; f < r->def.nfields; f++) {
        const cdb_ts_val *v = &p->v[f];
        if (!v->present)
            continue;
        cdb_value_field(&w, r->def.fields[f].id);
        switch (r->def.fields[f].type) {
        case CDB_TYPE_INT: cdb_value_write_int(&w, (int64_t)v->bits); break;
        case CDB_TYPE_F64: {
            double d;
            memcpy(&d, &v->bits, 8);
            cdb_value_write_f64(&w, d);
            break;
        }
        case CDB_TYPE_F32: {
            uint32_t b = (uint32_t)v->bits;
            float x;
            memcpy(&x, &b, 4);
            cdb_value_write_f32(&w, x);
            break;
        }
        case CDB_TYPE_BOOL: cdb_value_write_bool(&w, v->bits != 0); break;
        case CDB_TYPE_STRING: cdb_value_write_string(&w, (const char *)v->bytes, v->len); break;
        default: cdb_value_write_encoded(&w, v->bytes, v->len); break;
        }
    }
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

cdb_status cdb_ts_range(cdb_db *db, uint64_t store, uint64_t series, int64_t from, int64_t to, uint8_t *buf,
                        size_t cap, cdb_ts_row *rows, size_t max, size_t *n, int64_t *next)
{
    ts_store *t;
    srec r;
    size_t used = 0;
    int64_t at;
    int found;
    cdb_status s;
    *n = 0;
    *next = to;
    if ((s = ts_get(db, store, &t)) != CDB_OK)
        return s;
    if ((s = rec_get(t, series, &r, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if (!r.has_points || from >= to)
        return CDB_OK;
    at = window_of(from, r.def.window);
    for (;;) {
        cdb_ts_point *pts = work(db);
        const uint8_t *pl;
        size_t len, cnt, i;
        cdb_ts_layout L;
        blk b = { 0, 0, 0, 0 };
        if ((s = blk_at(db, t, series, at, &b, &found)) != CDB_OK)
            return s;
        if (!found || b.first >= to)
            return CDB_OK;
        if (b.last >= from) {
            if ((s = blk_read(db, t, &b, &pl, &len)) != CDB_OK
                || (s = cdb_tsblock_decode(pl, len, db->pager.order, &L, pts, CDB_TS_BLOCK_MAX_POINTS, &cnt, NULL))
                       != CDB_OK)
                return s;
            for (i = 0; i < cnt; i++) {
                size_t rl;
                if (pts[i].ts < from)
                    continue;
                if (pts[i].ts >= to)
                    return CDB_OK;
                if (*n == max || write_row(&r, &pts[i], db->pager.order, buf + used, cap - used, &rl) != CDB_OK) {
                    *next = pts[i].ts; /* full: continue from this point */
                    return *n ? CDB_OK : CDB_E_BUFFER_TOO_SMALL;
                }
                rows[*n].ts = pts[i].ts;
                rows[*n].off = used;
                rows[*n].len = rl;
                used += rl;
                (*n)++;
            }
        }
        if (b.first == INT64_MAX)
            return CDB_OK;
        at = b.first + 1;
    }
}
