/* Relational tables, row-oriented (cdb_table.h; spec 9, 14.10.1).
 *
 * Every call that reaches the document engine sets db->via_table for the
 * duration, so the engine accepts the table as a pool (cdb_doc.c doc_pool). */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_rowgroup.h"
#include "cdb_varint.h"
#include "cdb_table.h"

/* Store settings object (field 16; spec 14.10.1): 1 keep, 2 layout (0 =
 * row, 1 = column), 3 indexes, 4 next index number; column-oriented only
 * (spec 14.10.2): 5 groups index root, 6 pending index root, 7 rows per
 * group, 8 value bytes per column per group. An index: 1 number, 2 field
 * IDs, 3 unique, 4 root. */
#define SET_KEEP 1u
#define SET_LAYOUT 2u
#define SET_INDEXES 3u
#define SET_NEXT_INDEX 4u
#define SET_GROUPS 5u
#define SET_PENDING 6u
#define SET_GROUP_ROWS 7u
#define SET_CHUNK_CAP 8u
#define IX_NUMBER 1u
#define IX_FIELDS 2u
#define IX_UNIQUE 3u
#define IX_ROOT 4u

/* Table schema (a schema version, spec 9.8): 1 columns, 2 primary-key field
 * IDs. A column: 1 field ID, 2 type, 3 not-null, 4 default, 5 added. */
#define SCH_COLUMNS 1u
#define SCH_PK 2u
#define COL_ID 1u
#define COL_TYPE 2u
#define COL_NOT_NULL 3u
#define COL_DEFAULT 4u
#define COL_ADDED 5u

/* Buffers in cfg.table_buffers, each a document buffer: the row being
 * written, the current row of an update, another row (unique checks,
 * scans), and the row before an apply. */
enum { T_ROW = 0, T_CUR = 1, T_OTHER = 2, T_BEFORE = 3, T_COUNT = 4 };

typedef struct cdb_table_store table;
typedef struct cdb_table_col col;
typedef struct cdb_table_idx idx;

size_t cdb_table_buffers_size(size_t page_size, size_t doc_max)
{
    return (size_t)T_COUNT * cdb_doc_buffer_size(page_size, doc_max);
}

static size_t tbuf_size(const cdb_db *db)
{
    return cdb_doc_buffer_size(db->pager.page_size, db->cfg.doc_max);
}

static uint8_t *tbuf(cdb_db *db, unsigned i)
{
    return db->cfg.table_buffers + (size_t)i * tbuf_size(db);
}

/* -------------------------------------------------------- types */

static int type_ok(int type)
{
    return type > (int)CDB_TYPE_NULL && type < (int)CDB_TYPE_UNKNOWN;
}

/* Index keys exist for these (cdb_key.h). */
static int indexable(int type)
{
    switch (type) {
    case CDB_TYPE_BOOL:
    case CDB_TYPE_INT:
    case CDB_TYPE_F64:
    case CDB_TYPE_F32:
    case CDB_TYPE_TIMESTAMP:
    case CDB_TYPE_REF:
    case CDB_TYPE_STRING:
    case CDB_TYPE_BINARY:
        return 1;
    default:
        return 0;
    }
}

/* -------------------------------------------------------- slots */

static cdb_status save_table(cdb_db *db, void *slot)
{
    (void)db;
    (void)slot;
    return CDB_OK; /* index roots never change; columns change by catalog commits */
}

const cdb_slot_ops cdb_table_slot_ops = { NULL, save_table };

/* Forgets a table's open state, and its pool's, after a catalog change, so
 * both reload. Catalog changes reconcile first, so nothing unsaved is lost. */
static void forget(cdb_db *db, uint64_t store)
{
    cdb_slot *s;
    if ((s = (cdb_slot *)cdb_slots_find(&db->table_slots, store)) != NULL)
        s->in_use = 0;
    if ((s = (cdb_slot *)cdb_slots_find(&db->pool_slots, store)) != NULL)
        s->in_use = 0;
}

/* -------------------------------------------------------- decoding */

static cdb_status get_field(const cdb_value *obj, cdb_byte_order o, uint64_t id, cdb_value *v, int *found)
{
    return cdb_value_object_get(obj, o, id, v, found);
}

static cdb_status get_uint(const cdb_value *obj, cdb_byte_order o, uint64_t id, uint64_t max, uint64_t *out)
{
    cdb_value v;
    int found;
    cdb_status s = get_field(obj, o, id, &v, &found);
    if (s != CDB_OK)
        return s;
    if (!found) {
        *out = 0;
        return CDB_OK;
    }
    if (v.type != CDB_TYPE_INT || v.as.i64 < 0 || (uint64_t)v.as.i64 > max)
        return CDB_E_CORRUPT;
    *out = (uint64_t)v.as.i64;
    return CDB_OK;
}

static cdb_status get_bool(const cdb_value *obj, cdb_byte_order o, uint64_t id, int *out)
{
    cdb_value v;
    int found;
    cdb_status s = get_field(obj, o, id, &v, &found);
    *out = 0;
    if (s != CDB_OK || !found)
        return s;
    if (v.type != CDB_TYPE_BOOL)
        return CDB_E_CORRUPT;
    *out = v.as.boolean;
    return CDB_OK;
}

/* Reads an array of up to max field IDs. */
static cdb_status get_ids(const cdb_value *obj, cdb_byte_order o, uint64_t id, uint64_t *ids, unsigned max,
                          unsigned *n)
{
    cdb_value a, item;
    cdb_value_iter it;
    int found, done;
    cdb_status s = get_field(obj, o, id, &a, &found);
    *n = 0;
    if (s != CDB_OK || !found)
        return s;
    if (a.type != CDB_TYPE_ARRAY || cdb_value_array_iter(&a, o, &it) != CDB_OK)
        return CDB_E_CORRUPT;
    for (;;) {
        if (cdb_value_array_next(&it, &item, &done) != CDB_OK)
            return CDB_E_CORRUPT;
        if (done)
            return CDB_OK;
        if (*n == max || item.type != CDB_TYPE_INT || item.as.i64 <= 0)
            return CDB_E_CORRUPT;
        ids[(*n)++] = (uint64_t)item.as.i64;
    }
}

cdb_status cdb_table_keep_setting(const uint8_t *settings, size_t len, cdb_byte_order o, uint32_t *keep)
{
    cdb_value obj;
    uint64_t k;
    cdb_status s;
    *keep = 0;
    if (len == 0)
        return CDB_OK;
    if (cdb_value_read(settings, len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if ((s = get_uint(&obj, o, SET_KEEP, UINT32_MAX, &k)) != CDB_OK)
        return s;
    *keep = (uint32_t)k;
    return CDB_OK;
}

/* Parses the settings into t: keep and the indexes, which it opens. */
static cdb_status parse_settings(cdb_db *db, table *t, const uint8_t *settings, size_t len)
{
    cdb_byte_order o = db->pager.order;
    cdb_value obj, a, item;
    cdb_value_iter it;
    uint64_t v;
    int found, done;
    cdb_status s;
    t->keep = 0;
    t->nidx = 0;
    t->next_index = 1;
    t->layout = 0;
    if (len == 0)
        return CDB_OK;
    if (cdb_value_read(settings, len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if ((s = get_uint(&obj, o, SET_KEEP, UINT32_MAX, &v)) != CDB_OK)
        return s;
    t->keep = (uint32_t)v;
    if ((s = get_uint(&obj, o, SET_NEXT_INDEX, UINT32_MAX, &v)) != CDB_OK)
        return s;
    t->next_index = v ? (uint32_t)v : 1u;
    if ((s = get_uint(&obj, o, SET_LAYOUT, CDB_TABLE_LAYOUT_COLUMN, &v)) != CDB_OK)
        return s;
    t->layout = (uint8_t)v;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN) {
        uint64_t groups, pending, rows, cap;
        if ((s = get_uint(&obj, o, SET_GROUPS, UINT64_MAX, &groups)) != CDB_OK
            || (s = get_uint(&obj, o, SET_PENDING, UINT64_MAX, &pending)) != CDB_OK
            || (s = get_uint(&obj, o, SET_GROUP_ROWS, CDB_RG_ROWS_MAX, &rows)) != CDB_OK
            || (s = get_uint(&obj, o, SET_CHUNK_CAP, UINT32_MAX, &cap)) != CDB_OK)
            return s;
        if ((groups == 0) != (pending == 0) || rows == 0 || cap == 0)
            return CDB_E_CORRUPT;
        t->group_rows = (uint32_t)rows;
        t->chunk_cap = (uint32_t)cap;
        memset(&t->groups, 0, sizeof t->groups);
        memset(&t->pending, 0, sizeof t->pending);
        /* Roots of 0: creation stopped before the indexes were made; the
         * table has no columns either (cdb_table_create). */
        if (groups != 0
            && ((s = cdb_index_open(&t->groups, &db->pager, groups, &db->cfg.catalog_work)) != CDB_OK
                || (s = cdb_index_open(&t->pending, &db->pager, pending, &db->cfg.catalog_work)) != CDB_OK))
            return s;
    }
    if ((s = get_field(&obj, o, SET_INDEXES, &a, &found)) != CDB_OK || !found)
        return s;
    if (a.type != CDB_TYPE_ARRAY || cdb_value_array_iter(&a, o, &it) != CDB_OK)
        return CDB_E_CORRUPT;
    for (;;) {
        idx *x;
        uint64_t root;
        if (cdb_value_array_next(&it, &item, &done) != CDB_OK)
            return CDB_E_CORRUPT;
        if (done)
            return CDB_OK;
        if (t->nidx == CDB_TABLE_MAX_INDEXES || item.type != CDB_TYPE_OBJECT)
            return CDB_E_CORRUPT;
        x = &t->idx[t->nidx];
        if ((s = get_uint(&item, o, IX_NUMBER, UINT32_MAX, &v)) != CDB_OK
            || (s = get_ids(&item, o, IX_FIELDS, x->fields, CDB_TABLE_MAX_KEY_COLUMNS, &x->n)) != CDB_OK
            || (s = get_uint(&item, o, IX_ROOT, UINT64_MAX, &root)) != CDB_OK)
            return s;
        {
            int u;
            if ((s = get_bool(&item, o, IX_UNIQUE, &u)) != CDB_OK)
                return s;
            x->unique = (uint8_t)u;
        }
        x->number = (uint32_t)v;
        if (x->n == 0 || root == 0)
            return CDB_E_CORRUPT;
        if ((s = cdb_index_open(&x->ix, &db->pager, root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        t->nidx++;
    }
}

/* Parses a schema version into t's columns. */
static cdb_status parse_schema(cdb_db *db, table *t, const uint8_t *sch, size_t len)
{
    cdb_byte_order o = db->pager.order;
    cdb_value obj, a, item, d;
    cdb_value_iter it;
    size_t used = 0;
    int found, done;
    cdb_status s;
    t->ncols = 0;
    if (cdb_value_read(sch, len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if ((s = get_ids(&obj, o, SCH_PK, t->pk, CDB_TABLE_MAX_KEY_COLUMNS, &t->npk)) != CDB_OK)
        return s;
    if ((s = get_field(&obj, o, SCH_COLUMNS, &a, &found)) != CDB_OK)
        return s;
    if (!found || a.type != CDB_TYPE_ARRAY || cdb_value_array_iter(&a, o, &it) != CDB_OK)
        return CDB_E_CORRUPT;
    for (;;) {
        col *c;
        uint64_t v;
        int nn;
        if (cdb_value_array_next(&it, &item, &done) != CDB_OK)
            return CDB_E_CORRUPT;
        if (done)
            return CDB_OK;
        if (t->ncols == CDB_TABLE_MAX_COLUMNS || item.type != CDB_TYPE_OBJECT)
            return CDB_E_CORRUPT;
        c = &t->cols[t->ncols];
        memset(c, 0, sizeof *c);
        if ((s = get_uint(&item, o, COL_ID, UINT64_MAX, &c->id)) != CDB_OK
            || (s = get_uint(&item, o, COL_TYPE, (uint64_t)CDB_TYPE_UNKNOWN, &v)) != CDB_OK)
            return s;
        c->type = (uint8_t)v;
        if ((s = get_bool(&item, o, COL_NOT_NULL, &nn)) != CDB_OK
            || (s = get_uint(&item, o, COL_ADDED, UINT32_MAX, &v)) != CDB_OK)
            return s;
        c->not_null = (uint8_t)nn;
        c->added = (uint32_t)v;
        if (c->id == 0 || !type_ok(c->type) || (t->ncols > 0 && c->id <= t->cols[t->ncols - 1u].id))
            return CDB_E_CORRUPT;
        if ((s = get_field(&item, o, COL_DEFAULT, &d, &found)) != CDB_OK)
            return s;
        if (found) {
            if (d.type != c->type || used + d.encoded_len > CDB_TABLE_DEFAULTS_MAX)
                return CDB_E_CORRUPT;
            memcpy(t->defaults + used, d.start, d.encoded_len);
            c->def_off = (uint16_t)used;
            c->def_len = (uint16_t)d.encoded_len;
            used += d.encoded_len;
        }
        t->ncols++;
    }
}

/* Finds the open table, loading it from the catalog into a slot.
 * CDB_E_INVALID_ARG: not a table; CDB_E_NOT_FOUND: unknown or dropped. */
static cdb_status table_get(cdb_db *db, uint64_t store, table **out)
{
    table *t = (table *)cdb_slots_find(&db->table_slots, store);
    cdb_store_info info;
    void *v;
    cdb_status s;
    if (t != NULL) {
        cdb_slots_touch(db, &db->table_slots, t);
        *out = t;
        return CDB_OK;
    }
    if (!db->has_catalog || db->cfg.table_buffers == NULL || db->cfg.doc_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_catalog_get_store(&db->catalog, store, &info)) != CDB_OK)
        return s;
    if (info.model != CDB_MODEL_RELATIONAL)
        return CDB_E_INVALID_ARG;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_slots_claim(db, &db->table_slots, &v)) != CDB_OK)
        return s;
    t = (table *)v;
    t->slot.store_id = store;
    if ((s = parse_settings(db, t, info.vector, info.vector_len)) != CDB_OK)
        return s;
    t->version = info.schema_version;
    if (t->version != 0) {
        size_t len;
        if ((s = cdb_catalog_get_schema(&db->catalog, store, t->version, tbuf(db, T_OTHER), tbuf_size(db), &len))
                != CDB_OK
            || (s = parse_schema(db, t, tbuf(db, T_OTHER), len)) != CDB_OK)
            return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    }
    t->slot.in_use = 1;
    cdb_slots_touch(db, &db->table_slots, t);
    *out = t;
    return CDB_OK;
}

static const col *col_of(const table *t, uint64_t id)
{
    unsigned i;
    for (i = 0; i < t->ncols; i++)
        if (t->cols[i].id == id)
            return &t->cols[i];
    return NULL;
}

/* -------------------------------------------------------- rows */

/* A column's value in a stored row read through the current columns
 * (cdb_table.h): *found 0 means null. The value keeps its stored type
 * (convert() widens it). */
static cdb_status row_value(cdb_db *db, const table *t, const cdb_value *row, uint32_t version, const col *c,
                            cdb_value *v, int *found)
{
    cdb_byte_order o = db->pager.order;
    cdb_status s = cdb_value_object_get(row, o, c->id, v, found);
    if (s != CDB_OK)
        return s;
    if (*found && v->type == CDB_TYPE_NULL)
        *found = 0;
    if (!*found && version < c->added && c->def_len) {
        *found = 1;
        return cdb_value_read(t->defaults + c->def_off, c->def_len, o, v);
    }
    return CDB_OK;
}

/* Writes value v as column c's type: int to decimal and f32 to f64 are
 * widened (spec 9.8); anything else is copied. */
static cdb_status write_as(cdb_value_writer *w, const col *c, const cdb_value *v)
{
    if (c->type == CDB_TYPE_DECIMAL && v->type == CDB_TYPE_INT)
        return cdb_value_write_decimal_i64(w, 0, v->as.i64);
    if (c->type == CDB_TYPE_F64 && v->type == CDB_TYPE_F32)
        return cdb_value_write_f64(w, (double)v->as.f32);
    return cdb_value_write_encoded(w, v->start, v->encoded_len);
}

/* Projects a stored row (written under schema `version`) through the
 * current columns into out. */
static cdb_status project(cdb_db *db, const table *t, const uint8_t *raw, size_t rlen, uint32_t version,
                          uint8_t *out, size_t cap, size_t *len)
{
    cdb_byte_order o = db->pager.order;
    cdb_value row;
    cdb_value_writer w;
    unsigned i;
    cdb_status s;
    if (cdb_value_read(raw, rlen, o, &row) != CDB_OK || row.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    cdb_value_writer_init(&w, out, cap, o, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 0; i < t->ncols; i++) {
        cdb_value v;
        int found;
        if ((s = row_value(db, t, &row, version, &t->cols[i], &v, &found)) != CDB_OK)
            return s;
        if (!found)
            continue;
        cdb_value_field(&w, t->cols[i].id);
        write_as(&w, &t->cols[i], &v);
    }
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error;
    *len = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* Builds the row a change will store, in T_ROW, under the current schema:
 * every input field must be a current column with a value of its type (or
 * null); a column the input leaves out takes base's value (update), else
 * its default (insert), else null; a not-null column left null is
 * CDB_E_SCHEMA. base is a projected row or NULL. */
static cdb_status normalize(cdb_db *db, const table *t, const uint8_t *in, size_t in_len, const uint8_t *base,
                            size_t base_len, int defaults, size_t *len)
{
    cdb_byte_order o = db->pager.order;
    cdb_value obj, bobj, fv;
    cdb_value_iter it;
    cdb_value_writer w;
    uint64_t fid;
    unsigned i;
    int done;
    cdb_status s;
    if (in == NULL || cdb_value_validate(in, in_len, o, CDB_DECIMAL_COMPACT, 0) != CDB_OK
        || cdb_value_read(in, in_len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    if (base != NULL && cdb_value_read(base, base_len, o, &bobj) != CDB_OK)
        return CDB_E_CORRUPT;
    /* Every input field is a column, of the column's type or null. */
    if (cdb_value_object_iter(&obj, o, &it) != CDB_OK)
        return CDB_E_INVALID_ARG;
    for (;;) {
        const col *c;
        if (cdb_value_object_next(&it, &fid, &fv, &done) != CDB_OK)
            return CDB_E_INVALID_ARG;
        if (done)
            break;
        if ((c = col_of(t, fid)) == NULL || (fv.type != CDB_TYPE_NULL && fv.type != c->type))
            return CDB_E_SCHEMA;
    }
    cdb_value_writer_init(&w, tbuf(db, T_ROW), tbuf_size(db), o, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 0; i < t->ncols; i++) {
        const col *c = &t->cols[i];
        cdb_value v;
        int found;
        if ((s = cdb_value_object_get(&obj, o, c->id, &v, &found)) != CDB_OK)
            return s;
        if (!found && base != NULL && (s = cdb_value_object_get(&bobj, o, c->id, &v, &found)) != CDB_OK)
            return s;
        if (!found && defaults && c->def_len) {
            if ((s = cdb_value_read(t->defaults + c->def_off, c->def_len, o, &v)) != CDB_OK)
                return s;
            found = 1;
        }
        if (found && v.type == CDB_TYPE_NULL)
            found = 0;
        if (!found) {
            if (c->not_null)
                return CDB_E_SCHEMA;
            continue;
        }
        cdb_value_field(&w, c->id);
        cdb_value_write_encoded(&w, v.start, v.encoded_len);
    }
    cdb_value_end_object(&w);
    if (w.error == CDB_E_BUFFER_TOO_SMALL)
        return CDB_E_OVERFLOW;
    if (w.error != CDB_OK)
        return w.error;
    *len = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* -------------------------------------------------------- keys */

static cdb_status key_part(cdb_key_builder *kb, const cdb_value *v, int found)
{
    if (!found)
        return cdb_key_null(kb);
    switch (v->type) {
    case CDB_TYPE_BOOL: return cdb_key_bool(kb, v->as.boolean);
    case CDB_TYPE_INT: return cdb_key_int(kb, v->as.i64);
    case CDB_TYPE_F64: return cdb_key_f64(kb, v->as.f64);
    case CDB_TYPE_F32: return cdb_key_f64(kb, (double)v->as.f32);
    case CDB_TYPE_TIMESTAMP: return cdb_key_timestamp(kb, v->as.ts.ticks);
    case CDB_TYPE_REF: return cdb_key_ref(kb, v->as.ref.store_id, v->as.ref.entry_id);
    case CDB_TYPE_STRING: return cdb_key_string(kb, (const char *)v->payload, v->payload_len);
    case CDB_TYPE_BINARY: return cdb_key_binary(kb, v->payload, v->payload_len);
    default: return CDB_E_CORRUPT;
    }
}

/* Index x's key for a row (raw, under `version`). The entry is appended
 * for a non-unique index or when a column is null; *null says which. */
static cdb_status row_key(cdb_db *db, const table *t, const idx *x, const uint8_t *raw, size_t rlen,
                          uint32_t version, uint64_t entry, uint8_t *key, size_t *klen, int *null)
{
    cdb_value row;
    cdb_key_builder kb;
    unsigned i;
    cdb_status s;
    *null = 0;
    if (cdb_value_read(raw, rlen, db->pager.order, &row) != CDB_OK)
        return CDB_E_CORRUPT;
    cdb_key_init(&kb, key, CDB_TABLE_KEY_MAX + 8u);
    for (i = 0; i < x->n; i++) {
        const col *c = col_of(t, x->fields[i]);
        cdb_value v;
        int found = 0;
        memset(&v, 0, sizeof v);
        if (c != NULL && (s = row_value(db, t, &row, version, c, &v, &found)) != CDB_OK)
            return s;
        if (!found)
            *null = 1;
        if ((s = key_part(&kb, &v, found)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
    }
    if ((!x->unique || *null) && (s = cdb_key_u64(&kb, entry)) != CDB_OK)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
    *klen = kb.len;
    return *klen > x->ix.max_key ? CDB_E_OVERFLOW : CDB_OK;
}

/* A key from an encoded array of values for an index's leading columns
 * (find, scan bounds). */
static cdb_status array_key(cdb_db *db, const idx *x, const uint8_t *arr, size_t len, uint8_t *key, size_t *klen)
{
    cdb_byte_order o = db->pager.order;
    cdb_value a, item;
    cdb_value_iter it;
    cdb_key_builder kb;
    unsigned n = 0;
    int done;
    cdb_status s;
    if (arr == NULL || cdb_value_read(arr, len, o, &a) != CDB_OK || a.type != CDB_TYPE_ARRAY
        || cdb_value_array_iter(&a, o, &it) != CDB_OK)
        return CDB_E_INVALID_ARG;
    cdb_key_init(&kb, key, CDB_TABLE_KEY_MAX);
    for (;;) {
        if (cdb_value_array_next(&it, &item, &done) != CDB_OK)
            return CDB_E_INVALID_ARG;
        if (done)
            break;
        if (n == x->n)
            return CDB_E_INVALID_ARG;
        if (item.type != CDB_TYPE_NULL && !indexable(item.type))
            return CDB_E_INVALID_ARG;
        if ((s = key_part(&kb, &item, item.type != CDB_TYPE_NULL)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : s;
        n++;
    }
    *klen = kb.len;
    return CDB_OK;
}

static const idx *index_of(const table *t, uint32_t number)
{
    unsigned i;
    for (i = 0; i < t->nidx; i++)
        if (t->idx[i].number == number)
            return &t->idx[i];
    return NULL;
}

/* -------------------------------------------------------- unique checks */

typedef struct uctx {
    cdb_db *db;
    const table *t;
    const idx *x;
    uint64_t self;
    const uint8_t *key;
    size_t klen;
    int clash;
} uctx;

/* Whether entry e, as the open transaction sees it, has key c->key. */
static cdb_status has_key(uctx *c, uint64_t e, size_t staged_off)
{
    uint8_t k[CDB_TABLE_KEY_MAX + 8u];
    size_t len, klen;
    uint32_t version;
    int live, null;
    cdb_status s;
    if (e == c->self)
        return CDB_OK;
    if ((s = cdb_doc_get_in_tx(c->db, c->t->slot.store_id, e, staged_off, tbuf(c->db, T_OTHER), tbuf_size(c->db),
                               &len, &live, &version)) != CDB_OK || !live)
        return s;
    if ((s = row_key(c->db, c->t, c->x, tbuf(c->db, T_OTHER), len, version, e, k, &klen, &null)) != CDB_OK)
        return s == CDB_E_OVERFLOW ? CDB_OK : s;
    if (!null && klen == c->klen && memcmp(k, c->key, klen) == 0)
        c->clash = 1;
    return CDB_OK;
}

static cdb_status tx_entry(void *ctx, uint64_t e, size_t staged_off)
{
    uctx *c = (uctx *)ctx;
    return c->clash ? CDB_OK : has_key(c, e, staged_off);
}

/* The row in T_ROW (for entry self) must not repeat a non-null key of a
 * unique index, as the open transaction sees the table; every key must also
 * fit its index. */
static cdb_status check_keys(cdb_db *db, const table *t, uint64_t self, size_t len)
{
    unsigned i;
    for (i = 0; i < t->nidx; i++) {
        const idx *x = &t->idx[i];
        uint8_t key[CDB_TABLE_KEY_MAX + 8u], v[8];
        size_t klen, vlen;
        int null, found;
        uctx c;
        cdb_status s = row_key(db, t, x, tbuf(db, T_ROW), len, t->version, self, key, &klen, &null);
        if (s != CDB_OK)
            return s;
        if (!x->unique || null)
            continue;
        memset(&c, 0, sizeof c);
        c.db = db;
        c.t = t;
        c.x = x;
        c.self = self;
        c.key = key;
        c.klen = klen;
        /* A committed row with the key, unless this transaction changed it. */
        if ((s = cdb_index_get((cdb_index *)&x->ix, key, klen, v, sizeof v, &vlen, &found)) != CDB_OK)
            return s;
        if (found && vlen == 8 && (s = has_key(&c, cdb_load_u64(v, CDB_ORDER_BIG), CDB_STAGE_OFF_NONE)) != CDB_OK)
            return s;
        /* Rows this transaction inserted or changed. The row in T_ROW is
         * untouched: has_key reads into T_OTHER. */
        if (!c.clash && (s = cdb_doc_tx_entries(db, t->slot.store_id, tx_entry, &c)) != CDB_OK)
            return s;
        if (c.clash)
            return CDB_E_EXISTS;
    }
    return CDB_OK;
}

/* Journals a write-set key record for every non-null unique index key of
 * the row in T_ROW (spec 5.9.4, 14.4.7 rule 3). */
static cdb_status write_keys(cdb_db *db, const table *t, uint64_t self, size_t len)
{
    unsigned i;
    for (i = 0; i < t->nidx; i++) {
        const idx *x = &t->idx[i];
        uint8_t key[CDB_TABLE_KEY_MAX + 8u];
        size_t klen;
        int null;
        cdb_status s;
        if (!x->unique)
            continue;
        if ((s = row_key(db, t, x, tbuf(db, T_ROW), len, t->version, self, key, &klen, &null)) != CDB_OK)
            return s;
        if (null)
            continue;
        if ((s = cdb_tx_put_key(db, t->slot.store_id, x->number, key, klen)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- row changes */

/* The common start of a row change: an open transaction and a table with
 * columns. */
static cdb_status tx_table(cdb_db *db, uint64_t store, table **t)
{
    cdb_status s;
    if (!db->in_tx || db->failed)
        return CDB_E_STATE;
    if ((s = table_get(db, store, t)) != CDB_OK)
        return s;
    if ((*t)->layout == CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_UNSUPPORTED; /* column-oriented rows: M18 layers 2-4 */
    return (*t)->version == 0 ? CDB_E_STATE : CDB_OK;
}

static cdb_status insert_row(cdb_db *db, uint64_t store, const void *row, size_t len, uint64_t *entry)
{
    table *t;
    size_t n;
    cdb_status s;
    if ((s = tx_table(db, store, &t)) != CDB_OK)
        return s;
    /* A new row has no entry ID yet; 0 stands in (it matches no row, and
     * the ID's length in a key is fixed, so key sizes are checked right). */
    if ((s = normalize(db, t, (const uint8_t *)row, len, NULL, 0, 1, &n)) != CDB_OK
        || (s = check_keys(db, t, 0, n)) != CDB_OK)
        return s;
    if ((s = cdb_doc_insert(db, store, NULL, 0, tbuf(db, T_ROW), n, entry)) != CDB_OK)
        return s;
    return write_keys(db, t, *entry, n);
}

static cdb_status replace_row(cdb_db *db, uint64_t store, uint64_t entry, const void *row, size_t len, int update)
{
    table *t;
    size_t n, cur = 0;
    uint32_t version;
    int live = 1;
    cdb_status s;
    if ((s = tx_table(db, store, &t)) != CDB_OK)
        return s;
    if (update) {
        /* The row as this transaction sees it, through the current columns. */
        size_t raw;
        if ((s = cdb_doc_get_in_tx(db, store, entry, CDB_STAGE_OFF_NONE, tbuf(db, T_OTHER), tbuf_size(db),
                                   &raw, &live, &version)) != CDB_OK)
            return s;
        if (!live)
            return CDB_E_NOT_FOUND;
        if ((s = project(db, t, tbuf(db, T_OTHER), raw, version, tbuf(db, T_CUR), tbuf_size(db), &cur)) != CDB_OK)
            return s;
    }
    if ((s = normalize(db, t, (const uint8_t *)row, len, update ? tbuf(db, T_CUR) : NULL, cur, 0, &n)) != CDB_OK
        || (s = check_keys(db, t, entry, n)) != CDB_OK)
        return s;
    if ((s = cdb_doc_replace(db, store, entry, NULL, 0, tbuf(db, T_ROW), n)) != CDB_OK)
        return s;
    return write_keys(db, t, entry, n);
}

cdb_status cdb_table_insert(cdb_db *db, uint64_t store, const void *row, size_t len, uint64_t *entry)
{
    cdb_status s;
    db->via_table = 1;
    s = insert_row(db, store, row, len, entry);
    db->via_table = 0;
    return s;
}

cdb_status cdb_table_replace(cdb_db *db, uint64_t store, uint64_t entry, const void *row, size_t len)
{
    cdb_status s;
    db->via_table = 1;
    s = replace_row(db, store, entry, row, len, 0);
    db->via_table = 0;
    return s;
}

cdb_status cdb_table_update(cdb_db *db, uint64_t store, uint64_t entry, const void *changes, size_t len)
{
    cdb_status s;
    db->via_table = 1;
    s = replace_row(db, store, entry, changes, len, 1);
    db->via_table = 0;
    return s;
}

cdb_status cdb_table_delete(cdb_db *db, uint64_t store, uint64_t entry)
{
    table *t;
    cdb_status s;
    db->via_table = 1;
    s = tx_table(db, store, &t);
    if (s == CDB_OK)
        s = cdb_doc_delete(db, store, entry);
    db->via_table = 0;
    return s;
}

/* -------------------------------------------------------- reads */

/* The committed row of entry, projected into out. */
static cdb_status read_row(cdb_db *db, table *t, uint64_t entry, void *out, size_t cap, size_t *len)
{
    cdb_doc_info info;
    size_t raw;
    cdb_status s = cdb_doc_get(db, t->slot.store_id, entry, 0, tbuf(db, T_OTHER), tbuf_size(db), &raw, NULL, 0,
                               NULL, &info);
    if (s != CDB_OK)
        return s;
    return project(db, t, tbuf(db, T_OTHER), raw, info.schema_version, (uint8_t *)out, cap, len);
}

cdb_status cdb_table_get(cdb_db *db, uint64_t store, uint64_t entry, void *out, size_t cap, size_t *len)
{
    table *t;
    cdb_status s;
    db->via_table = 1;
    s = table_get(db, store, &t);
    if (s == CDB_OK && t->layout == CDB_TABLE_LAYOUT_COLUMN)
        s = CDB_E_UNSUPPORTED;
    if (s == CDB_OK)
        s = read_row(db, t, entry, out, cap, len);
    db->via_table = 0;
    return s;
}

static cdb_status find_row(cdb_db *db, uint64_t store, const void *key, size_t key_len, uint64_t *entry, int *found)
{
    uint8_t k[CDB_TABLE_KEY_MAX], v[8];
    size_t klen, vlen;
    const idx *x;
    table *t;
    cdb_status s;
    *found = 0;
    if ((s = table_get(db, store, &t)) != CDB_OK)
        return s;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_UNSUPPORTED;
    if (t->npk == 0 || (x = index_of(t, 0)) == NULL)
        return CDB_E_STATE; /* no primary key */
    if ((s = array_key(db, x, (const uint8_t *)key, key_len, k, &klen)) != CDB_OK)
        return s;
    if ((s = cdb_index_get((cdb_index *)&x->ix, k, klen, v, sizeof v, &vlen, found)) != CDB_OK || !*found)
        return s;
    if (vlen != 8)
        return CDB_E_CORRUPT;
    *entry = cdb_load_u64(v, CDB_ORDER_BIG);
    return CDB_OK;
}

cdb_status cdb_table_find(cdb_db *db, uint64_t store, const void *key, size_t key_len, uint64_t *entry, int *found)
{
    cdb_status s;
    db->via_table = 1;
    s = find_row(db, store, key, key_len, entry, found);
    db->via_table = 0;
    return s;
}

static cdb_status scan(cdb_db *db, uint64_t store, uint32_t index, const void *from, size_t from_len,
                       const void *to, size_t to_len, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                       size_t *n)
{
    uint8_t lo[CDB_TABLE_KEY_MAX], hi[CDB_TABLE_KEY_MAX], k[CDB_TABLE_KEY_MAX + 8u], v[8];
    size_t lolen = 0, hilen = 0, klen, vlen;
    cdb_index_cursor c;
    const idx *x;
    table *t;
    int done;
    cdb_status s;
    *n = 0;
    if ((s = table_get(db, store, &t)) != CDB_OK)
        return s;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_UNSUPPORTED;
    if (index == CDB_TABLE_NO_INDEX) {
        /* Entry order: the committed live rows. */
        while (*n < max) {
            uint64_t e;
            if ((s = cdb_doc_next_live(db, store, cur->started ? cur->entry : 0, &e, &done)) != CDB_OK || done)
                return s;
            cur->started = 1;
            cur->entry = e;
            entries[(*n)++] = e;
        }
        return CDB_OK;
    }
    if ((x = index_of(t, index)) == NULL)
        return CDB_E_NOT_FOUND;
    if (from != NULL && (s = array_key(db, x, (const uint8_t *)from, from_len, lo, &lolen)) != CDB_OK)
        return s;
    if (to != NULL && (s = array_key(db, x, (const uint8_t *)to, to_len, hi, &hilen)) != CDB_OK)
        return s;
    /* Resume after the last key returned, or start at the lower bound. */
    if (cur->started)
        s = cdb_index_seek(&c, (cdb_index *)&x->ix, tbuf(db, T_OTHER), cur->key, cur->len);
    else
        s = cdb_index_seek(&c, (cdb_index *)&x->ix, tbuf(db, T_OTHER), lo, lolen);
    if (s != CDB_OK)
        return s;
    while (*n < max) {
        if ((s = cdb_index_next(&c, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK || done)
            return s;
        if (cur->started && klen == cur->len && memcmp(k, cur->key, klen) == 0)
            continue; /* returned by the last call */
        /* Past the upper bound: compared on the columns `to` gives. */
        if (to != NULL && memcmp(k, hi, klen < hilen ? klen : hilen) > 0)
            return CDB_OK;
        if (vlen != 8)
            return CDB_E_CORRUPT;
        memcpy(cur->key, k, klen);
        cur->len = klen;
        cur->started = 1;
        entries[(*n)++] = cdb_load_u64(v, CDB_ORDER_BIG);
    }
    return CDB_OK;
}

cdb_status cdb_table_scan(cdb_db *db, uint64_t store, uint32_t index, const void *from, size_t from_len,
                          const void *to, size_t to_len, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                          size_t *n)
{
    cdb_status s;
    db->via_table = 1;
    s = scan(db, store, index, from, from_len, to, to_len, cur, entries, max, n);
    db->via_table = 0;
    return s;
}

/* -------------------------------------------------------- apply hooks */

cdb_status cdb_table_apply_before(cdb_db *db, uint64_t store, uint64_t entry)
{
    cdb_doc_info info;
    table *t;
    cdb_status s;
    db->via_table = 1;
    s = table_get(db, store, &t);
    if (s == CDB_OK) {
        t->before_live = 0;
        if (t->nidx > 0) {
            s = cdb_doc_get(db, store, entry, 0, tbuf(db, T_BEFORE), tbuf_size(db), &t->before_len, NULL, 0, NULL,
                            &info);
            if (s == CDB_OK) {
                t->before_live = 1;
                t->before_schema = info.schema_version;
            } else if (s == CDB_E_NOT_FOUND) {
                s = CDB_OK;
            }
        }
    }
    db->via_table = 0;
    /* A table that cannot be read is skipped, as the apply never fails for
     * logical reasons; its indexes are rebuilt with it. */
    return s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG ? CDB_OK : s;
}

static cdb_status update_indexes(cdb_db *db, table *t, uint64_t entry)
{
    cdb_doc_info info;
    size_t len = 0;
    int live;
    unsigned i;
    cdb_status s = cdb_doc_get(db, t->slot.store_id, entry, 0, tbuf(db, T_OTHER), tbuf_size(db), &len, NULL, 0, NULL,
                               &info);
    if (s != CDB_OK && s != CDB_E_NOT_FOUND)
        return s;
    live = s == CDB_OK;
    for (i = 0; i < t->nidx; i++) {
        idx *x = &t->idx[i];
        uint8_t ok[CDB_TABLE_KEY_MAX + 8u], nk[CDB_TABLE_KEY_MAX + 8u], v[8];
        size_t oklen = 0, nklen = 0;
        int null, found;
        if (t->before_live && (s = row_key(db, t, x, tbuf(db, T_BEFORE), t->before_len, t->before_schema, entry, ok,
                                           &oklen, &null)) != CDB_OK)
            return s;
        if (live && (s = row_key(db, t, x, tbuf(db, T_OTHER), len, info.schema_version, entry, nk, &nklen, &null))
                        != CDB_OK)
            return s;
        if (t->before_live && live && oklen == nklen && memcmp(ok, nk, oklen) == 0)
            continue;
        if (t->before_live && (s = cdb_index_delete(&x->ix, ok, oklen, &found)) != CDB_OK)
            return s;
        cdb_store_u64(v, entry, CDB_ORDER_BIG);
        if (live && (s = cdb_index_put(&x->ix, nk, nklen, v, sizeof v)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

cdb_status cdb_table_apply_after(cdb_db *db, uint64_t store, uint64_t entry)
{
    table *t;
    cdb_status s;
    db->via_table = 1;
    s = table_get(db, store, &t);
    if (s == CDB_OK && t->nidx > 0)
        s = update_indexes(db, t, entry);
    db->via_table = 0;
    return s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG ? CDB_OK : s;
}

/* -------------------------------------------------------- catalog changes */

/* Encodes t's indexes and keep as the settings object. */
static cdb_status encode_settings(cdb_db *db, const table *t, uint64_t next_index, uint8_t *out, size_t cap,
                                  size_t *len)
{
    cdb_value_writer w;
    unsigned i, j;
    cdb_value_writer_init(&w, out, cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, SET_KEEP);
    cdb_value_write_int(&w, (int64_t)t->keep);
    cdb_value_field(&w, SET_LAYOUT);
    cdb_value_write_int(&w, (int64_t)t->layout);
    cdb_value_field(&w, SET_INDEXES);
    cdb_value_begin_array(&w);
    for (i = 0; i < t->nidx; i++) {
        const idx *x = &t->idx[i];
        cdb_value_begin_object(&w);
        cdb_value_field(&w, IX_NUMBER);
        cdb_value_write_int(&w, (int64_t)x->number);
        cdb_value_field(&w, IX_FIELDS);
        cdb_value_begin_array(&w);
        for (j = 0; j < x->n; j++)
            cdb_value_write_int(&w, (int64_t)x->fields[j]);
        cdb_value_end_array(&w);
        cdb_value_field(&w, IX_UNIQUE);
        cdb_value_write_bool(&w, x->unique);
        cdb_value_field(&w, IX_ROOT);
        cdb_value_write_int(&w, (int64_t)x->ix.root);
        cdb_value_end_object(&w);
    }
    cdb_value_end_array(&w);
    cdb_value_field(&w, SET_NEXT_INDEX);
    cdb_value_write_int(&w, (int64_t)next_index);
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN) {
        cdb_value_field(&w, SET_GROUPS);
        cdb_value_write_int(&w, (int64_t)t->groups.root);
        cdb_value_field(&w, SET_PENDING);
        cdb_value_write_int(&w, (int64_t)t->pending.root);
        cdb_value_field(&w, SET_GROUP_ROWS);
        cdb_value_write_int(&w, (int64_t)t->group_rows);
        cdb_value_field(&w, SET_CHUNK_CAP);
        cdb_value_write_int(&w, (int64_t)t->chunk_cap);
    }
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : w.error;
    *len = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* Encodes t's columns and primary key as a schema version. */
static cdb_status encode_schema(cdb_db *db, const table *t, uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    unsigned i;
    cdb_value_writer_init(&w, out, cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, SCH_COLUMNS);
    cdb_value_begin_array(&w);
    for (i = 0; i < t->ncols; i++) {
        const col *c = &t->cols[i];
        cdb_value_begin_object(&w);
        cdb_value_field(&w, COL_ID);
        cdb_value_write_int(&w, (int64_t)c->id);
        cdb_value_field(&w, COL_TYPE);
        cdb_value_write_int(&w, (int64_t)c->type);
        cdb_value_field(&w, COL_NOT_NULL);
        cdb_value_write_bool(&w, c->not_null);
        if (c->def_len) {
            cdb_value_field(&w, COL_DEFAULT);
            cdb_value_write_encoded(&w, t->defaults + c->def_off, c->def_len);
        }
        cdb_value_field(&w, COL_ADDED);
        cdb_value_write_int(&w, (int64_t)c->added);
        cdb_value_end_object(&w);
    }
    cdb_value_end_array(&w);
    if (t->npk > 0) {
        cdb_value_field(&w, SCH_PK);
        cdb_value_begin_array(&w);
        for (i = 0; i < t->npk; i++)
            cdb_value_write_int(&w, (int64_t)t->pk[i]);
        cdb_value_end_array(&w);
    }
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error == CDB_E_BUFFER_TOO_SMALL ? CDB_E_OVERFLOW : w.error;
    *len = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* Checks one column as given and adds its default to t (at used). */
static cdb_status check_column(cdb_db *db, table *t, const cdb_table_column *in, col *c, size_t *used)
{
    cdb_byte_order o = db->pager.order;
    memset(c, 0, sizeof *c);
    if (in->name == NULL || in->name_len == 0 || !type_ok((int)in->type))
        return CDB_E_INVALID_ARG;
    c->type = (uint8_t)in->type;
    c->not_null = (uint8_t)(in->not_null != 0);
    if (in->default_value != NULL) {
        cdb_value d;
        if (cdb_value_validate(in->default_value, in->default_len, o, CDB_DECIMAL_COMPACT, 0) != CDB_OK
            || cdb_value_read(in->default_value, in->default_len, o, &d) != CDB_OK || d.type != in->type
            || *used + in->default_len > CDB_TABLE_DEFAULTS_MAX)
            return CDB_E_INVALID_ARG;
        memcpy(t->defaults + *used, in->default_value, in->default_len);
        c->def_off = (uint16_t)*used;
        c->def_len = (uint16_t)in->default_len;
        *used += in->default_len;
    }
    return CDB_OK;
}

/* Writes t's settings and, with schema, a new schema version (which t's
 * columns record as `added` where they are new). */
static cdb_status save(cdb_db *db, uint64_t store, table *t, uint64_t next_index, int schema)
{
    uint8_t settings[CDB_CATALOG_SETTINGS_MAX];
    size_t len;
    cdb_status s;
    if ((s = encode_settings(db, t, next_index, settings, sizeof settings, &len)) != CDB_OK
        || (s = cdb_catalog_set_vector(&db->catalog, store, settings, len)) != CDB_OK)
        return s;
    if (!schema)
        return CDB_OK;
    if ((s = encode_schema(db, t, tbuf(db, T_OTHER), tbuf_size(db), &len)) != CDB_OK)
        return s;
    return cdb_catalog_add_schema(&db->catalog, store, tbuf(db, T_OTHER), len, &t->version);
}

/* Adds an index over fields to t: creates it and, if the table has rows,
 * fills it from them. CDB_E_EXISTS if a unique index meets a duplicate. */
static cdb_status add_index(cdb_db *db, uint64_t store, table *t, const uint64_t *fields, unsigned n, int unique,
                            uint32_t number)
{
    idx *x;
    unsigned i;
    uint64_t e = 0;
    int done;
    cdb_status s;
    if (n == 0 || n > CDB_TABLE_MAX_KEY_COLUMNS || t->nidx == CDB_TABLE_MAX_INDEXES)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < n; i++) {
        const col *c = col_of(t, fields[i]);
        if (c == NULL || !indexable(c->type))
            return CDB_E_INVALID_ARG;
    }
    x = &t->idx[t->nidx];
    memset(x, 0, sizeof *x);
    x->number = number;
    x->unique = (uint8_t)(unique != 0);
    x->n = n;
    memcpy(x->fields, fields, (size_t)n * sizeof fields[0]);
    if ((s = cdb_index_create(&x->ix, &db->pager, store, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work)) != CDB_OK)
        return s;
    /* Existing rows (none for a new table). */
    for (;;) {
        cdb_doc_info info;
        uint8_t key[CDB_TABLE_KEY_MAX + 8u], v[8];
        size_t len, klen;
        int null, found;
        if ((s = cdb_doc_next_live(db, store, e, &e, &done)) != CDB_OK || done)
            break;
        if ((s = cdb_doc_get(db, store, e, 0, tbuf(db, T_OTHER), tbuf_size(db), &len, NULL, 0, NULL, &info))
                != CDB_OK
            || (s = row_key(db, t, x, tbuf(db, T_OTHER), len, info.schema_version, e, key, &klen, &null)) != CDB_OK)
            return s;
        if (x->unique && !null) {
            if ((s = cdb_index_get(&x->ix, key, klen, v, sizeof v, &len, &found)) != CDB_OK)
                return s;
            if (found)
                return CDB_E_EXISTS;
        }
        cdb_store_u64(v, e, CDB_ORDER_BIG);
        if ((s = cdb_index_put(&x->ix, key, klen, v, sizeof v)) != CDB_OK)
            return s;
    }
    if (s != CDB_OK)
        return s;
    t->nidx++;
    return CDB_OK;
}

/* Runs a catalog change on the table: loads it fresh, calls fn, and makes
 * every open state reload afterwards. */
typedef cdb_status (*change_fn)(cdb_db *db, uint64_t store, table *t, uint64_t next_index, const void *arg);

static cdb_status change(cdb_db *db, uint64_t store, change_fn fn, const void *arg)
{
    table *t;
    cdb_status s;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    db->via_table = 1;
    forget(db, store);
    if ((s = table_get(db, store, &t)) == CDB_OK && t->version == 0)
        s = CDB_E_STATE;
    if (s == CDB_OK)
        s = fn(db, store, t, t->next_index, arg);
    forget(db, store);
    db->via_table = 0;
    return cdb_db_catalog_end(db, s);
}

cdb_status cdb_table_create(cdb_db *db, const char *name, size_t len, const cdb_table_def *def, uint64_t *store)
{
    uint8_t settings[CDB_CATALOG_SETTINGS_MAX];
    size_t slen, used = 0;
    uint64_t existing, pk[CDB_TABLE_MAX_KEY_COLUMNS];
    int found;
    unsigned i, j;
    table tt; /* about 3.5 KB */
    table *t = &tt;
    cdb_status s;

    if (db->in_tx || db->failed)
        return CDB_E_STATE;
    if (!db->has_catalog || db->cfg.table_buffers == NULL || db->cfg.doc_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if (def == NULL || def->columns == NULL || def->ncolumns == 0 || def->ncolumns > CDB_TABLE_MAX_COLUMNS
        || def->npk > CDB_TABLE_MAX_KEY_COLUMNS || (def->npk > 0 && def->primary_key == NULL))
        return CDB_E_INVALID_ARG;
    for (i = 0; i < def->ncolumns; i++)
        for (j = 0; j < i; j++)
            if (def->columns[i].name_len == def->columns[j].name_len && def->columns[i].name != NULL
                && def->columns[j].name != NULL
                && memcmp(def->columns[i].name, def->columns[j].name, def->columns[i].name_len) == 0)
                return CDB_E_INVALID_ARG;
    for (i = 0; i < def->npk; i++) {
        if (def->primary_key[i] >= def->ncolumns || !indexable((int)def->columns[def->primary_key[i]].type))
            return CDB_E_INVALID_ARG;
        for (j = 0; j < i; j++)
            if (def->primary_key[i] == def->primary_key[j])
                return CDB_E_INVALID_ARG;
    }
    /* The table state is built here, checked before any catalog change;
     * the next use of the table loads its own copy from the catalog. */
    memset(t, 0, sizeof *t);
    t->keep = def->keep;
    if (def->layout > CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_INVALID_ARG;
    t->layout = (uint8_t)def->layout;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN) {
        int alloc = db->env.ops->alloc != NULL && db->env.ops->free != NULL;
        t->group_rows = def->group_rows ? def->group_rows : CDB_RG_ROWS_DEFAULT;
        t->chunk_cap = def->chunk_cap ? def->chunk_cap : CDB_RG_CAP_DEFAULT;
        /* The entry-ID chunk must fit (cdb_rg_init). A row is never larger
         * than the cap (checked when it is written), so a group of one row
         * always fits too; the cap does not depend on doc_max. */
        if (t->group_rows > CDB_RG_ROWS_MAX
            || t->chunk_cap < (size_t)t->group_rows * (1u + CDB_VARINT_MAX_LEN))
            return CDB_E_INVALID_ARG;
        if (db->cfg.col_work == NULL)
            return CDB_E_UNSUPPORTED;
        if (db->cfg.col_work_size < cdb_rg_work_size(db->pager.page_size, t->group_rows, t->chunk_cap, !alloc))
            return CDB_E_BUFFER_TOO_SMALL;
    }
    for (i = 0; i < def->ncolumns; i++)
        if ((s = check_column(db, t, &def->columns[i], &t->cols[i], &used)) != CDB_OK)
            return s;
    if ((s = cdb_db_catalog_begin(db)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_find_store(&db->catalog, name, len, &existing, &found)) != CDB_OK)
        return cdb_db_catalog_end(db, s);
    if (found)
        return cdb_db_catalog_end(db, CDB_E_EXISTS);
    /* The store, then its column IDs, then the primary key index and the
     * settings, then the first schema version. */
    if ((s = encode_settings(db, t, 1, settings, sizeof settings, &slen)) != CDB_OK
        || (s = cdb_catalog_create_store_ex(&db->catalog, name, len, CDB_MODEL_RELATIONAL, settings, slen, store))
               != CDB_OK)
        return cdb_db_catalog_end(db, s);
    for (i = 0; i < def->ncolumns; i++) {
        if ((s = cdb_catalog_field_id(&db->catalog, *store, def->columns[i].name, def->columns[i].name_len,
                                      &t->cols[i].id)) != CDB_OK)
            return cdb_db_catalog_end(db, s);
        t->cols[i].added = 1;
        if (def->columns[i].not_null)
            t->cols[i].not_null = 1;
    }
    for (i = 0; i < def->npk; i++) {
        pk[i] = t->cols[def->primary_key[i]].id;
        t->cols[def->primary_key[i]].not_null = 1; /* primary-key columns are not-null (spec 9.9) */
    }
    t->ncols = def->ncolumns;
    t->npk = def->npk;
    memcpy(t->pk, pk, (size_t)def->npk * sizeof pk[0]);
    /* Columns in field-ID order (IDs were handed out in order, but sort to
     * be sure). */
    for (i = 1; i < t->ncols; i++)
        for (j = i; j > 0 && t->cols[j - 1u].id > t->cols[j].id; j--) {
            col c = t->cols[j];
            t->cols[j] = t->cols[j - 1u];
            t->cols[j - 1u] = c;
        }
    t->slot.store_id = *store;
    db->via_table = 1;
    s = CDB_OK;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN
        && ((s = cdb_index_create(&t->groups, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work))
                != CDB_OK
            || (s = cdb_index_create(&t->pending, &db->pager, *store, CDB_INDEX_FLAG_UNIQUE,
                                     &db->cfg.catalog_work)) != CDB_OK)) {
        db->via_table = 0;
        return cdb_db_catalog_end(db, s);
    }
    if (def->npk > 0)
        s = add_index(db, *store, t, pk, def->npk, 1, 0);
    if (s == CDB_OK)
        s = save(db, *store, t, 1, 1);
    db->via_table = 0;
    forget(db, *store);
    return cdb_db_catalog_end(db, s);
}

cdb_status cdb_table_describe(cdb_db *db, uint64_t store, cdb_table_column_info *cols, unsigned cap, unsigned *n,
                              uint32_t *version, uint32_t *keep)
{
    table *t;
    unsigned i, j;
    cdb_status s = table_get(db, store, &t);
    if (s != CDB_OK)
        return s;
    *n = t->ncols;
    if (version)
        *version = t->version;
    if (keep)
        *keep = t->keep;
    for (i = 0; i < t->ncols && i < cap; i++) {
        cols[i].field_id = t->cols[i].id;
        cols[i].type = (cdb_value_type)t->cols[i].type;
        cols[i].not_null = t->cols[i].not_null;
        cols[i].has_default = t->cols[i].def_len != 0;
        cols[i].added = t->cols[i].added;
        cols[i].primary = 0;
        for (j = 0; j < t->npk; j++)
            if (t->pk[j] == t->cols[i].id)
                cols[i].primary = 1;
    }
    return CDB_OK;
}

cdb_status cdb_table_column_id(cdb_db *db, uint64_t store, const char *name, size_t len, uint64_t *field_id)
{
    char got[CDB_CATALOG_NAME_MAX];
    size_t glen;
    unsigned i;
    table *t;
    cdb_status s = table_get(db, store, &t);
    if (s != CDB_OK)
        return s;
    for (i = 0; i < t->ncols; i++) {
        if ((s = cdb_catalog_field_name(&db->catalog, store, t->cols[i].id, got, sizeof got, &glen)) != CDB_OK)
            return s;
        if (glen == len && memcmp(got, name, len) == 0) {
            *field_id = t->cols[i].id;
            return CDB_OK;
        }
    }
    return CDB_E_NOT_FOUND;
}

static cdb_status do_add_column(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    const cdb_table_column *in = (const cdb_table_column *)arg;
    size_t used = 0;
    uint64_t id;
    unsigned i;
    col c;
    cdb_status s;
    for (i = 0; i < t->ncols; i++)
        if (t->cols[i].def_len && (size_t)t->cols[i].def_off + t->cols[i].def_len > used)
            used = (size_t)t->cols[i].def_off + t->cols[i].def_len;
    if (t->ncols == CDB_TABLE_MAX_COLUMNS)
        return CDB_E_OVERFLOW;
    if ((s = check_column(db, t, in, &c, &used)) != CDB_OK)
        return s;
    if (c.not_null && c.def_len == 0)
        return CDB_E_INVALID_ARG; /* existing rows would break it (spec 9.8) */
    {
        uint64_t have;
        if (cdb_table_column_id(db, store, in->name, in->name_len, &have) == CDB_OK)
            return CDB_E_EXISTS;
    }
    if ((s = cdb_catalog_field_id(&db->catalog, store, in->name, in->name_len, &id)) != CDB_OK)
        return s;
    c.id = id;
    c.added = t->version + 1u;
    t->cols[t->ncols++] = c; /* a new ID is the largest, so order holds */
    return save(db, store, t, next, 1);
}

cdb_status cdb_table_add_column(cdb_db *db, uint64_t store, const cdb_table_column *col_in, uint64_t *field_id)
{
    cdb_status s;
    if (col_in == NULL)
        return CDB_E_INVALID_ARG;
    s = change(db, store, do_add_column, col_in);
    if (s == CDB_OK) {
        db->via_table = 1;
        s = cdb_table_column_id(db, store, col_in->name, col_in->name_len, field_id);
        db->via_table = 0;
    }
    return s;
}

static cdb_status do_drop_column(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    uint64_t id = *(const uint64_t *)arg;
    unsigned i, j;
    cdb_status s;
    if (col_of(t, id) == NULL)
        return CDB_E_NOT_FOUND;
    for (i = 0; i < t->nidx; i++)
        for (j = 0; j < t->idx[i].n; j++)
            if (t->idx[i].fields[j] == id)
                return CDB_E_STATE; /* drop the index first (spec 9.8) */
    for (i = 0; i < t->ncols; i++)
        if (t->cols[i].id == id) {
            memmove(&t->cols[i], &t->cols[i + 1u], (size_t)(t->ncols - i - 1u) * sizeof t->cols[0]);
            t->ncols--;
            break;
        }
    if ((s = save(db, store, t, next, 1)) != CDB_OK)
        return s;
    return cdb_catalog_field_rename(&db->catalog, store, id, NULL, 0); /* the name is free again */
}

cdb_status cdb_table_drop_column(cdb_db *db, uint64_t store, uint64_t field_id)
{
    return change(db, store, do_drop_column, &field_id);
}

typedef struct rename_arg {
    uint64_t id;
    const char *name;
    size_t len;
} rename_arg;

static cdb_status do_rename(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    const rename_arg *r = (const rename_arg *)arg;
    (void)next;
    if (col_of(t, r->id) == NULL)
        return CDB_E_NOT_FOUND;
    return cdb_catalog_field_rename(&db->catalog, store, r->id, r->name, r->len);
}

cdb_status cdb_table_rename_column(cdb_db *db, uint64_t store, uint64_t field_id, const char *name, size_t len)
{
    rename_arg r;
    if (name == NULL || len == 0)
        return CDB_E_INVALID_ARG;
    r.id = field_id;
    r.name = name;
    r.len = len;
    return change(db, store, do_rename, &r);
}

typedef struct widen_arg {
    uint64_t id;
    cdb_value_type type;
} widen_arg;

static cdb_status do_widen(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    const widen_arg *a = (const widen_arg *)arg;
    unsigned i, j;
    col *c = (col *)col_of(t, a->id);
    if (c == NULL)
        return CDB_E_NOT_FOUND;
    if (!((c->type == CDB_TYPE_INT && a->type == CDB_TYPE_DECIMAL)
          || (c->type == CDB_TYPE_F32 && a->type == CDB_TYPE_F64)))
        return CDB_E_INVALID_ARG;
    if (c->def_len)
        return CDB_E_INVALID_ARG; /* its default would have the old type */
    for (i = 0; i < t->nidx; i++)
        for (j = 0; j < t->idx[i].n; j++)
            if (t->idx[i].fields[j] == a->id && !indexable((int)a->type))
                return CDB_E_STATE;
    c->type = (uint8_t)a->type;
    return save(db, store, t, next, 1);
}

cdb_status cdb_table_widen_column(cdb_db *db, uint64_t store, uint64_t field_id, cdb_value_type type)
{
    widen_arg a;
    a.id = field_id;
    a.type = type;
    return change(db, store, do_widen, &a);
}

static cdb_status do_keep(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    t->keep = *(const uint32_t *)arg;
    return save(db, store, t, next, 0);
}

cdb_status cdb_table_set_keep(cdb_db *db, uint64_t store, uint32_t keep)
{
    return change(db, store, do_keep, &keep);
}

typedef struct index_arg {
    const uint64_t *fields;
    unsigned n;
    int unique;
    uint32_t *number;
} index_arg;

static cdb_status do_create_index(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    const index_arg *a = (const index_arg *)arg;
    cdb_status s;
    if (t->layout == CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_UNSUPPORTED; /* it would be built from the row store */
    if (next == 0 || next >= UINT32_MAX)
        return CDB_E_OVERFLOW;
    if ((s = add_index(db, store, t, a->fields, a->n, a->unique, (uint32_t)next)) != CDB_OK)
        return s;
    *a->number = (uint32_t)next;
    return save(db, store, t, next + 1u, 0);
}

cdb_status cdb_table_create_index(cdb_db *db, uint64_t store, const uint64_t *fields, unsigned n, int unique,
                                  uint32_t *index)
{
    index_arg a;
    if (fields == NULL || index == NULL)
        return CDB_E_INVALID_ARG;
    a.fields = fields;
    a.n = n;
    a.unique = unique;
    a.number = index;
    return change(db, store, do_create_index, &a);
}

static cdb_status do_drop_index(cdb_db *db, uint64_t store, table *t, uint64_t next, const void *arg)
{
    uint32_t number = *(const uint32_t *)arg;
    unsigned i;
    if (number == 0)
        return CDB_E_STATE; /* the primary key's index stays */
    for (i = 0; i < t->nidx; i++)
        if (t->idx[i].number == number) {
            memmove(&t->idx[i], &t->idx[i + 1u], (size_t)(t->nidx - i - 1u) * sizeof t->idx[0]);
            t->nidx--;
            return save(db, store, t, next, 0);
        }
    return CDB_E_NOT_FOUND;
}

cdb_status cdb_table_drop_index(cdb_db *db, uint64_t store, uint32_t index)
{
    return change(db, store, do_drop_index, &index);
}

/* -------------------------------------------------------- column-oriented */

static cdb_status rowgroups(cdb_db *db, uint64_t store, cdb_rg *g)
{
    table *t;
    cdb_status s;
    if ((s = table_get(db, store, &t)) != CDB_OK)
        return s;
    if (t->layout != CDB_TABLE_LAYOUT_COLUMN)
        return CDB_E_INVALID_ARG;
    if (t->groups.root == 0)
        return CDB_E_STATE; /* creation did not finish */
    if (db->cfg.col_work == NULL)
        return CDB_E_UNSUPPORTED;
    return cdb_rg_init(g, db, store, &t->groups, t->group_rows, t->chunk_cap, db->cfg.col_work,
                       db->cfg.col_work_size);
}

cdb_status cdb_table_rowgroups(cdb_db *db, uint64_t store, struct cdb_rg *g)
{
    cdb_status s;
    db->via_table = 1;
    s = rowgroups(db, store, g);
    db->via_table = 0;
    return s;
}

cdb_status cdb_table_nib_moved(cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen, uint64_t old_addr,
                               uint64_t addr)
{
    cdb_rg g;
    cdb_status s = cdb_table_rowgroups(db, store, &g);
    /* A dropped or unknown store, or one that is not a column-oriented
     * table, does not own the chunk: the merge drops it. */
    if (s == CDB_E_NOT_FOUND || s == CDB_E_INVALID_ARG)
        return CDB_E_NOT_FOUND;
    if (s != CDB_OK)
        return s;
    return cdb_rg_nib_moved(&g, okey, oklen, old_addr, addr);
}
