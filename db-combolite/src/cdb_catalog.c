/* Catalog (spec 4, 14.3). */
#include <string.h>

#include "combolite/cdb_value.h"
#include "cdb_catalog.h"
#include "cdb_endian.h"
#include "cdb_key.h"

#define KEY_OBJECT 0x01u
#define KEY_NAME 0x02u
#define KEY_SCHEMA 0x03u
#define KEY_FIELD_BY_NAME 0x04u
#define KEY_FIELD_BY_ID 0x05u
#define KEY_NEXT_ID 0x06u /* the object-ID counter, also kept in the body */

#define KEY_MAX 128u
#define REF_SIZE 16u /* two record page IDs: primary and mirror */

/* Field IDs of a store record (the catalog's own field dictionary). */
enum {
    F_KIND = 1, F_ID, F_NAME, F_MODEL, F_DOMAIN, F_PAGE_LOG2, F_COMPRESSION,
    F_SCHEMA_VERSION, F_NEXT_FIELD, F_CREATED, F_FLAGS,
    F_HEAD_ROOT, F_REV_ROOT, F_NEXT_ENTRY, /* optional: document pools */
    F_FULL_TEXT,                           /* optional: full-text index settings */
    F_VECTOR                               /* optional: vector collection settings */
};

/* -------------------------------------------------------- keys */

typedef struct key {
    uint8_t b[KEY_MAX];
    size_t n;
} key;

static cdb_status key_begin(key *k, unsigned kind, cdb_key_builder *kb)
{
    k->b[0] = (uint8_t)kind;
    cdb_key_init(kb, k->b + 1, sizeof k->b - 1);
    return CDB_OK;
}

static void key_end(key *k, const cdb_key_builder *kb)
{
    k->n = 1 + kb->len;
}

static cdb_status k_object(key *k, uint64_t id)
{
    cdb_key_builder kb;
    key_begin(k, KEY_OBJECT, &kb);
    cdb_key_u64(&kb, id);
    key_end(k, &kb);
    return kb.error;
}

static cdb_status k_name(key *k, unsigned kind, const char *name, size_t len)
{
    cdb_key_builder kb;
    key_begin(k, KEY_NAME, &kb);
    cdb_key_int(&kb, (int64_t)kind);
    cdb_key_string(&kb, name, len);
    key_end(k, &kb);
    return kb.error;
}

static cdb_status k_pair(key *k, unsigned kind, uint64_t a, uint64_t b)
{
    cdb_key_builder kb;
    key_begin(k, kind, &kb);
    cdb_key_u64(&kb, a);
    cdb_key_u64(&kb, b);
    key_end(k, &kb);
    return kb.error;
}

static cdb_status k_field_name(key *k, uint64_t store, const char *name, size_t len)
{
    cdb_key_builder kb;
    key_begin(k, KEY_FIELD_BY_NAME, &kb);
    cdb_key_u64(&kb, store);
    cdb_key_string(&kb, name, len);
    key_end(k, &kb);
    return kb.error;
}

/* -------------------------------------------------------- mirrored body */

static cdb_status ensure_created(cdb_catalog *cat)
{
    cdb_status s;
    unsigned flags = CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX;
    if (cat->open)
        return CDB_OK;
    if ((s = cdb_index_create(&cat->body, cat->pager, 0, flags, &cat->work)) != CDB_OK)
        return s;
    if ((s = cdb_index_create(&cat->mirror, cat->pager, 0, flags, &cat->work)) != CDB_OK)
        return s;
    cat->open = 1;
    return CDB_OK;
}

static cdb_status put_both(cdb_catalog *cat, const key *k, const void *v, size_t vlen)
{
    cdb_status s = cdb_index_put(&cat->body, k->b, k->n, v, vlen);
    if (s != CDB_OK)
        return s;
    return cdb_index_put(&cat->mirror, k->b, k->n, v, vlen);
}

static cdb_status delete_both(cdb_catalog *cat, const key *k)
{
    int found;
    cdb_status s = cdb_index_delete(&cat->body, k->b, k->n, &found);
    if (s != CDB_OK)
        return s;
    return cdb_index_delete(&cat->mirror, k->b, k->n, &found);
}

/* Reads from the primary body, falling back to the mirror (spec 3.5). */
static cdb_status get_either(cdb_catalog *cat, const key *k, void *v, size_t cap, size_t *vlen, int *found)
{
    cdb_status s;
    *found = 0;
    if (!cat->open)
        return CDB_OK;
    s = cdb_index_get(&cat->body, k->b, k->n, v, cap, vlen, found);
    if (s == CDB_E_CORRUPT)
        s = cdb_index_get(&cat->mirror, k->b, k->n, v, cap, vlen, found);
    return s;
}

/* -------------------------------------------------------- record pages */

static cdb_status write_record(cdb_catalog *cat, uint8_t ref[REF_SIZE], int is_new,
                               const void *payload, size_t len)
{
    uint64_t lids[2];
    cdb_page_header t;
    cdb_status s;
    int i;

    if (is_new) {
        for (i = 0; i < 2; i++)
            if ((s = cdb_pager_alloc(cat->pager, &lids[i])) != CDB_OK)
                return s;
        cdb_store_u64(ref, lids[0], CDB_ORDER_BIG);
        cdb_store_u64(ref + 8, lids[1], CDB_ORDER_BIG);
    } else {
        lids[0] = cdb_load_u64(ref, CDB_ORDER_BIG);
        lids[1] = cdb_load_u64(ref + 8, CDB_ORDER_BIG);
    }
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_CATALOG_RECORD;
    for (i = 0; i < 2; i++)
        if ((s = cdb_pager_write(cat->pager, lids[i], &t, payload, len)) != CDB_OK)
            return s;
    return CDB_OK;
}

/* Reads the record named by ref into cat->page; *payload points into it. */
static cdb_status read_record(cdb_catalog *cat, const uint8_t ref[REF_SIZE], const uint8_t **payload, size_t *len)
{
    cdb_page_header h;
    cdb_status s = CDB_E_CORRUPT;
    int i;
    for (i = 0; i < 2 && s != CDB_OK; i++) {
        s = cdb_pager_read(cat->pager, cdb_load_u64(ref + 8u * (unsigned)i, CDB_ORDER_BIG), cat->page, &h);
        if (s == CDB_OK && h.type != CDB_PAGE_TYPE_CATALOG_RECORD)
            s = CDB_E_CORRUPT;
    }
    if (s != CDB_OK)
        return s;
    *payload = cat->page + cdb_page_header_size(cat->pager->fmt);
    *len = h.payload_len;
    return CDB_OK;
}

/* -------------------------------------------------------- store records */

static cdb_status encode_store(cdb_catalog *cat, const cdb_store_info *in, size_t *len)
{
    cdb_value_writer w;
    uint8_t *buf = cat->page;
    size_t cap = cdb_page_capacity(cat->pager->fmt, cat->pager->page_size);
    cdb_value_writer_init(&w, buf, cap, cat->pager->order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, F_KIND);           cdb_value_write_int(&w, CDB_OBJ_STORE);
    cdb_value_field(&w, F_ID);             cdb_value_write_int(&w, (int64_t)in->id);
    cdb_value_field(&w, F_NAME);           cdb_value_write_string(&w, in->name, in->name_len);
    cdb_value_field(&w, F_MODEL);          cdb_value_write_int(&w, in->model);
    cdb_value_field(&w, F_DOMAIN);         cdb_value_write_int(&w, (int64_t)in->domain_id);
    cdb_value_field(&w, F_PAGE_LOG2);      cdb_value_write_int(&w, in->page_log2);
    cdb_value_field(&w, F_COMPRESSION);    cdb_value_write_int(&w, in->compression);
    cdb_value_field(&w, F_SCHEMA_VERSION); cdb_value_write_int(&w, in->schema_version);
    cdb_value_field(&w, F_NEXT_FIELD);     cdb_value_write_int(&w, (int64_t)in->next_field_id);
    cdb_value_field(&w, F_CREATED);        cdb_value_write_int(&w, (int64_t)in->created_commit);
    cdb_value_field(&w, F_FLAGS);          cdb_value_write_int(&w, in->flags);
    cdb_value_field(&w, F_HEAD_ROOT);      cdb_value_write_int(&w, (int64_t)in->head_root);
    cdb_value_field(&w, F_REV_ROOT);       cdb_value_write_int(&w, (int64_t)in->rev_root);
    cdb_value_field(&w, F_NEXT_ENTRY);     cdb_value_write_int(&w, (int64_t)in->next_entry_id);
    if (in->full_text_len) {
        cdb_value_field(&w, F_FULL_TEXT);
        cdb_value_write_encoded(&w, in->full_text, in->full_text_len);
    }
    if (in->vector_len) {
        cdb_value_field(&w, F_VECTOR);
        cdb_value_write_encoded(&w, in->vector, in->vector_len);
    }
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

static cdb_status field_int(const cdb_value *obj, cdb_byte_order o, uint64_t f, int64_t *out)
{
    cdb_value v;
    int found;
    cdb_status s = cdb_value_object_get(obj, o, f, &v, &found);
    if (s != CDB_OK)
        return s;
    if (!found || v.type != CDB_TYPE_INT)
        return CDB_E_CORRUPT;
    *out = v.as.i64;
    return CDB_OK;
}

static cdb_status decode_store(cdb_catalog *cat, const uint8_t *p, size_t len, cdb_store_info *out)
{
    cdb_byte_order o = cat->pager->order;
    cdb_value obj, name;
    int64_t v[11];
    int found, i;
    cdb_status s;

    if (cdb_value_validate(p, len, o, CDB_DECIMAL_COMPACT, 0) != CDB_OK
        || cdb_value_read(p, len, o, &obj) != CDB_OK || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if ((s = field_int(&obj, o, F_KIND, &v[F_KIND - 1])) != CDB_OK)
        return s;
    if (v[F_KIND - 1] != CDB_OBJ_STORE)
        return CDB_E_NOT_FOUND; /* another kind of object: not a store */
    for (i = 0; i < 11; i++) {
        if (i + 1 == F_NAME)
            continue;
        if ((s = field_int(&obj, o, (uint64_t)i + 1u, &v[i])) != CDB_OK)
            return s;
    }
    if ((s = cdb_value_object_get(&obj, o, F_NAME, &name, &found)) != CDB_OK)
        return s;
    if (!found || name.type != CDB_TYPE_STRING || name.payload_len > CDB_CATALOG_NAME_MAX)
        return CDB_E_CORRUPT;
    memset(out, 0, sizeof *out);
    out->id = (uint64_t)v[F_ID - 1];
    memcpy(out->name, name.payload, name.payload_len);
    out->name_len = name.payload_len;
    out->model = (uint32_t)v[F_MODEL - 1];
    out->domain_id = (uint64_t)v[F_DOMAIN - 1];
    out->page_log2 = (uint32_t)v[F_PAGE_LOG2 - 1];
    out->compression = (uint32_t)v[F_COMPRESSION - 1];
    out->schema_version = (uint32_t)v[F_SCHEMA_VERSION - 1];
    out->next_field_id = (uint64_t)v[F_NEXT_FIELD - 1];
    out->created_commit = (uint64_t)v[F_CREATED - 1];
    out->flags = (uint32_t)v[F_FLAGS - 1];
    {
        /* Optional fields: records written before they existed lack them. */
        uint64_t f;
        for (f = F_HEAD_ROOT; f <= F_NEXT_ENTRY; f++) {
            cdb_value fv;
            if ((s = cdb_value_object_get(&obj, o, f, &fv, &found)) != CDB_OK)
                return s;
            if (!found)
                continue;
            if (fv.type != CDB_TYPE_INT || fv.as.i64 < 0)
                return CDB_E_CORRUPT;
            if (f == F_HEAD_ROOT)
                out->head_root = (uint64_t)fv.as.i64;
            else if (f == F_REV_ROOT)
                out->rev_root = (uint64_t)fv.as.i64;
            else
                out->next_entry_id = (uint64_t)fv.as.i64;
        }
        if ((s = cdb_value_object_get(&obj, o, F_FULL_TEXT, &name, &found)) != CDB_OK)
            return s;
        if (found) {
            if (name.type != CDB_TYPE_OBJECT || name.encoded_len > CDB_CATALOG_FULL_TEXT_MAX)
                return CDB_E_CORRUPT;
            memcpy(out->full_text, name.start, name.encoded_len);
            out->full_text_len = name.encoded_len;
        }
        if ((s = cdb_value_object_get(&obj, o, F_VECTOR, &name, &found)) != CDB_OK)
            return s;
        if (found) {
            if (name.type != CDB_TYPE_OBJECT || name.encoded_len > CDB_CATALOG_SETTINGS_MAX)
                return CDB_E_CORRUPT;
            memcpy(out->vector, name.start, name.encoded_len);
            out->vector_len = name.encoded_len;
        }
    }
    return CDB_OK;
}

static cdb_status load_store(cdb_catalog *cat, uint64_t id, cdb_store_info *info, uint8_t ref[REF_SIZE])
{
    const uint8_t *p;
    size_t len, vlen;
    int found;
    key k;
    cdb_status s;

    if ((s = k_object(&k, id)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, ref, REF_SIZE, &vlen, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if (vlen != REF_SIZE)
        return CDB_E_CORRUPT;
    if ((s = read_record(cat, ref, &p, &len)) != CDB_OK)
        return s;
    if ((s = decode_store(cat, p, len, info)) != CDB_OK)
        return s;
    return info->id == id ? CDB_OK : CDB_E_CORRUPT;
}

static cdb_status save_store(cdb_catalog *cat, const cdb_store_info *info, uint8_t ref[REF_SIZE], int is_new)
{
    size_t len;
    cdb_status s = encode_store(cat, info, &len);
    if (s != CDB_OK)
        return s;
    return write_record(cat, ref, is_new, cat->page, len);
}

/* Every change ends here: record the roots and commit (spec 3.2 step 3). */
static cdb_status finish(cdb_catalog *cat, uint64_t next_id)
{
    cdb_catroot r = cat->pager->catroot;
    if (next_id != r.next_object_id) {
        /* The counter is also kept in the body: if the newest root slot is
         * damaged and an older one is used, the body still has the counter,
         * so no ID is ever handed out twice (spec 4.12). */
        uint8_t v[8];
        key k;
        cdb_status s;
        k.b[0] = (uint8_t)KEY_NEXT_ID;
        k.n = 1;
        cdb_store_u64(v, next_id, CDB_ORDER_BIG);
        if ((s = put_both(cat, &k, v, sizeof v)) != CDB_OK)
            return s;
    }
    r.body_root = cat->body.root;
    r.mirror_root = cat->mirror.root;
    r.next_object_id = next_id;
    cdb_pager_set_catroot(cat->pager, &r);
    return cdb_pager_commit(cat->pager);
}

static cdb_status check_name(const cdb_catalog *cat, const char *name, size_t len)
{
    uint8_t probe[8 + 2u * CDB_CATALOG_NAME_MAX + 16];
    cdb_value_writer w;
    if (name == NULL || len == 0 || len > CDB_CATALOG_NAME_MAX)
        return CDB_E_INVALID_ARG;
    /* Reuse the value writer's UTF-8 check. */
    cdb_value_writer_init(&w, probe, sizeof probe, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_string(&w, name, len) != CDB_OK)
        return CDB_E_INVALID_ARG;
    /* The longest key built from a name must fit in the index (spec 14.8.7). */
    if (cat->open && 2u * len + 16u > cat->body.max_key)
        return CDB_E_INVALID_ARG;
    return CDB_OK;
}

/* -------------------------------------------------------- public */

cdb_status cdb_catalog_open(cdb_catalog *cat, cdb_pager *p, const cdb_index_work *work, uint8_t *page)
{
    cdb_status s;
    if (cat == NULL || p == NULL || work == NULL || page == NULL)
        return CDB_E_INVALID_ARG;
    memset(cat, 0, sizeof *cat);
    cat->pager = p;
    cat->work = *work;
    cat->page = page;
    if (!p->cat_exists || p->catroot.body_root == 0)
        return CDB_OK; /* no catalog yet: created by the first change */
    /* One copy damaged: the other serves for both (spec 3.5). */
    s = cdb_index_open(&cat->body, p, p->catroot.body_root, work);
    if (s == CDB_E_CORRUPT) {
        if (cdb_index_open(&cat->body, p, p->catroot.mirror_root, work) != CDB_OK)
            return CDB_E_CORRUPT;
        cat->mirror = cat->body;
        s = CDB_OK;
    } else if (s == CDB_OK) {
        s = cdb_index_open(&cat->mirror, p, p->catroot.mirror_root, work);
        if (s == CDB_E_CORRUPT) {
            cat->mirror = cat->body;
            s = CDB_OK;
        }
    }
    if (s != CDB_OK)
        return s;
    cat->open = 1;
    {
        uint8_t v[8];
        size_t vlen;
        int found;
        key k;
        k.b[0] = (uint8_t)KEY_NEXT_ID;
        k.n = 1;
        if ((s = get_either(cat, &k, v, sizeof v, &vlen, &found)) != CDB_OK)
            return s;
        if (found && vlen == 8 && cdb_load_u64(v, CDB_ORDER_BIG) > p->catroot.next_object_id)
            p->catroot.next_object_id = cdb_load_u64(v, CDB_ORDER_BIG);
    }
    return CDB_OK;
}

cdb_status cdb_catalog_find_store(cdb_catalog *cat, const char *name, size_t len, uint64_t *id, int *found)
{
    uint8_t v[8];
    size_t vlen;
    key k;
    cdb_status s;
    if ((s = k_name(&k, CDB_OBJ_STORE, name, len)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, v, sizeof v, &vlen, found)) != CDB_OK)
        return s;
    if (*found) {
        if (vlen != 8)
            return CDB_E_CORRUPT;
        *id = cdb_load_u64(v, CDB_ORDER_BIG);
    }
    return CDB_OK;
}

uint64_t cdb_catalog_next_store_id(const cdb_catalog *cat)
{
    return cat->pager->catroot.next_object_id ? cat->pager->catroot.next_object_id : 1; /* 0 is the catalog */
}

cdb_status cdb_catalog_create_store(cdb_catalog *cat, const char *name, size_t len, uint32_t model,
                                    uint64_t *id)
{
    return cdb_catalog_create_store_ex(cat, name, len, model, NULL, 0, id);
}

cdb_status cdb_catalog_create_store_ex(cdb_catalog *cat, const char *name, size_t len, uint32_t model,
                                       const void *vector, size_t vector_len, uint64_t *id)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE], idv[8];
    uint64_t existing, next;
    int found;
    key k;
    cdb_status s;

    if (model < CDB_MODEL_RELATIONAL || model > CDB_MODEL_LINK || (vector == NULL && vector_len)
        || vector_len > CDB_CATALOG_SETTINGS_MAX)
        return CDB_E_INVALID_ARG;
    /* Names starting with '$' are the engine's own; only the link store has one. */
    if (name != NULL && len > 0 && (name[0] == '$') != (model == CDB_MODEL_LINK))
        return CDB_E_INVALID_ARG;
    if ((s = ensure_created(cat)) != CDB_OK)
        return s;
    if ((s = check_name(cat, name, len)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_find_store(cat, name, len, &existing, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;

    next = cdb_catalog_next_store_id(cat);
    memset(&info, 0, sizeof info);
    info.id = next;
    memcpy(info.name, name, len);
    info.name_len = len;
    info.model = model;
    info.page_log2 = cat->pager->hs.record.default_page_log2;
    info.next_field_id = 1;
    info.created_commit = cat->pager->hs.record.committed_seq + 1;
    if (vector_len) {
        memcpy(info.vector, vector, vector_len);
        info.vector_len = vector_len;
    }
    if ((s = save_store(cat, &info, ref, 1)) != CDB_OK)
        return s;
    if ((s = k_object(&k, info.id)) != CDB_OK || (s = put_both(cat, &k, ref, REF_SIZE)) != CDB_OK)
        return s;
    cdb_store_u64(idv, info.id, CDB_ORDER_BIG);
    if ((s = k_name(&k, CDB_OBJ_STORE, name, len)) != CDB_OK || (s = put_both(cat, &k, idv, sizeof idv)) != CDB_OK)
        return s;
    if ((s = finish(cat, next + 1)) != CDB_OK)
        return s;
    *id = info.id;
    return CDB_OK;
}

cdb_status cdb_catalog_get_store(cdb_catalog *cat, uint64_t id, cdb_store_info *info)
{
    uint8_t ref[REF_SIZE];
    return load_store(cat, id, info, ref);
}

cdb_status cdb_catalog_rename_store(cdb_catalog *cat, uint64_t id, const char *name, size_t len)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE], idv[8];
    uint64_t other;
    int found;
    key k;
    cdb_status s;

    if ((s = check_name(cat, name, len)) != CDB_OK)
        return s;
    if ((s = load_store(cat, id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_catalog_find_store(cat, name, len, &other, &found)) != CDB_OK)
        return s;
    if (found)
        return other == id ? CDB_OK : CDB_E_EXISTS;
    if ((s = k_name(&k, CDB_OBJ_STORE, info.name, info.name_len)) != CDB_OK || (s = delete_both(cat, &k)) != CDB_OK)
        return s;
    cdb_store_u64(idv, id, CDB_ORDER_BIG);
    if ((s = k_name(&k, CDB_OBJ_STORE, name, len)) != CDB_OK || (s = put_both(cat, &k, idv, sizeof idv)) != CDB_OK)
        return s;
    memcpy(info.name, name, len);
    info.name_len = len;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    return finish(cat, cat->pager->catroot.next_object_id);
}

cdb_status cdb_catalog_drop_store(cdb_catalog *cat, uint64_t id)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE];
    key k;
    cdb_status s;

    if ((s = load_store(cat, id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if ((s = k_name(&k, CDB_OBJ_STORE, info.name, info.name_len)) != CDB_OK || (s = delete_both(cat, &k)) != CDB_OK)
        return s;
    info.flags |= CDB_STORE_DROPPED; /* the record stays: IDs are never reused */
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    return finish(cat, cat->pager->catroot.next_object_id);
}

cdb_status cdb_catalog_stage_pool(cdb_catalog *cat, uint64_t store_id, uint64_t head_root,
                                  uint64_t rev_root, uint64_t next_entry_id)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE];
    cdb_status s;
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.head_root == head_root && info.rev_root == rev_root && info.next_entry_id == next_entry_id)
        return CDB_OK;
    info.head_root = head_root;
    info.rev_root = rev_root;
    info.next_entry_id = next_entry_id;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    cdb_pager_set_catroot(cat->pager, &cat->pager->catroot);
    return CDB_OK;
}

cdb_status cdb_catalog_stage_next_entry(cdb_catalog *cat, uint64_t store_id, uint64_t next_entry_id)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE];
    cdb_status s;
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.next_entry_id == next_entry_id)
        return CDB_OK;
    info.next_entry_id = next_entry_id;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    cdb_pager_set_catroot(cat->pager, &cat->pager->catroot);
    return CDB_OK;
}

cdb_status cdb_catalog_set_vector(cdb_catalog *cat, uint64_t store_id, const void *settings, size_t len)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE];
    cdb_status s;
    if (settings == NULL || len == 0 || len > CDB_CATALOG_SETTINGS_MAX)
        return CDB_E_INVALID_ARG;
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    memcpy(info.vector, settings, len);
    info.vector_len = len;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    return finish(cat, cat->pager->catroot.next_object_id);
}

cdb_status cdb_catalog_set_full_text(cdb_catalog *cat, uint64_t store_id, const void *settings, size_t len)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE];
    cdb_status s;
    if ((settings == NULL && len) || len > CDB_CATALOG_FULL_TEXT_MAX)
        return CDB_E_INVALID_ARG;
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    if (len)
        memcpy(info.full_text, settings, len);
    info.full_text_len = len;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    return finish(cat, cat->pager->catroot.next_object_id);
}

cdb_status cdb_catalog_add_schema(cdb_catalog *cat, uint64_t store_id, const void *schema, size_t len,
                                  uint32_t *version)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE], sref[REF_SIZE];
    key k;
    cdb_status s;

    if ((schema == NULL && len) || len > cdb_page_capacity(cat->pager->fmt, cat->pager->page_size))
        return CDB_E_INVALID_ARG;
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    info.schema_version++;
    if ((s = write_record(cat, sref, 1, schema, len)) != CDB_OK)
        return s;
    if ((s = k_pair(&k, KEY_SCHEMA, store_id, info.schema_version)) != CDB_OK
        || (s = put_both(cat, &k, sref, REF_SIZE)) != CDB_OK)
        return s;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    if ((s = finish(cat, cat->pager->catroot.next_object_id)) != CDB_OK)
        return s;
    *version = info.schema_version;
    return CDB_OK;
}

cdb_status cdb_catalog_get_schema(cdb_catalog *cat, uint64_t store_id, uint32_t version,
                                  void *buf, size_t cap, size_t *len)
{
    uint8_t sref[REF_SIZE];
    const uint8_t *p;
    size_t vlen;
    int found;
    key k;
    cdb_status s;

    if ((s = k_pair(&k, KEY_SCHEMA, store_id, version)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, sref, sizeof sref, &vlen, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if (vlen != REF_SIZE)
        return CDB_E_CORRUPT;
    if ((s = read_record(cat, sref, &p, len)) != CDB_OK)
        return s;
    if (*len > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memcpy(buf, p, *len);
    return CDB_OK;
}

cdb_status cdb_catalog_field_id(cdb_catalog *cat, uint64_t store_id, const char *name, size_t len,
                                uint64_t *field_id)
{
    cdb_store_info info;
    uint8_t ref[REF_SIZE], v[8];
    size_t vlen;
    int found;
    key k;
    cdb_status s;

    if ((s = check_name(cat, name, len)) != CDB_OK)
        return s;
    if (cat->open && len > cat->body.max_value)
        return CDB_E_INVALID_ARG;
    if ((s = k_field_name(&k, store_id, name, len)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, v, sizeof v, &vlen, &found)) != CDB_OK)
        return s;
    if (found) {
        if (vlen != 8)
            return CDB_E_CORRUPT;
        *field_id = cdb_load_u64(v, CDB_ORDER_BIG);
        return CDB_OK;
    }
    if ((s = load_store(cat, store_id, &info, ref)) != CDB_OK)
        return s;
    if (info.flags & CDB_STORE_DROPPED)
        return CDB_E_NOT_FOUND;
    *field_id = info.next_field_id++;
    cdb_store_u64(v, *field_id, CDB_ORDER_BIG);
    if ((s = put_both(cat, &k, v, sizeof v)) != CDB_OK)
        return s;
    if ((s = k_pair(&k, KEY_FIELD_BY_ID, store_id, *field_id)) != CDB_OK
        || (s = put_both(cat, &k, name, len)) != CDB_OK)
        return s;
    if ((s = save_store(cat, &info, ref, 0)) != CDB_OK)
        return s;
    return finish(cat, cat->pager->catroot.next_object_id);
}

cdb_status cdb_catalog_field_name(cdb_catalog *cat, uint64_t store_id, uint64_t field_id,
                                  char *name, size_t cap, size_t *len)
{
    int found;
    key k;
    cdb_status s;
    if ((s = k_pair(&k, KEY_FIELD_BY_ID, store_id, field_id)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, name, cap, len, &found)) != CDB_OK)
        return s;
    return found ? CDB_OK : CDB_E_NOT_FOUND;
}

/* -------------------------------------------------------- link types */

/* Link-type record (spec 14.13): kind, ID, name, then these fields. */
enum { LT_CARDINALITY = 4, LT_ENFORCED, LT_ON_DELETE };

cdb_status cdb_catalog_find_link_type(cdb_catalog *cat, const char *name, size_t len, uint64_t *id, int *found)
{
    uint8_t v[8];
    size_t vlen;
    key k;
    cdb_status s;
    if ((s = k_name(&k, CDB_OBJ_LINK_TYPE, name, len)) != CDB_OK)
        return s;
    if ((s = get_either(cat, &k, v, sizeof v, &vlen, found)) != CDB_OK)
        return s;
    if (*found) {
        if (vlen != 8)
            return CDB_E_CORRUPT;
        *id = cdb_load_u64(v, CDB_ORDER_BIG);
    }
    return CDB_OK;
}

cdb_status cdb_catalog_create_link_type(cdb_catalog *cat, const cdb_link_type_info *in, uint64_t *id)
{
    uint8_t ref[REF_SIZE], idv[8];
    uint64_t existing, next;
    size_t len;
    int found;
    cdb_value_writer w;
    key k;
    cdb_status s;

    if ((s = ensure_created(cat)) != CDB_OK)
        return s;
    if ((s = check_name(cat, in->name, in->name_len)) != CDB_OK)
        return s;
    if ((s = cdb_catalog_find_link_type(cat, in->name, in->name_len, &existing, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;
    next = cdb_catalog_next_store_id(cat);
    cdb_value_writer_init(&w, cat->page, cdb_page_capacity(cat->pager->fmt, cat->pager->page_size),
                          cat->pager->order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, F_KIND);         cdb_value_write_int(&w, CDB_OBJ_LINK_TYPE);
    cdb_value_field(&w, F_ID);           cdb_value_write_int(&w, (int64_t)next);
    cdb_value_field(&w, F_NAME);         cdb_value_write_string(&w, in->name, in->name_len);
    cdb_value_field(&w, LT_CARDINALITY); cdb_value_write_int(&w, in->cardinality);
    cdb_value_field(&w, LT_ENFORCED);    cdb_value_write_int(&w, in->enforced);
    cdb_value_field(&w, LT_ON_DELETE);   cdb_value_write_int(&w, in->on_delete);
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error;
    len = cdb_value_writer_len(&w);
    if ((s = write_record(cat, ref, 1, cat->page, len)) != CDB_OK)
        return s;
    if ((s = k_object(&k, next)) != CDB_OK || (s = put_both(cat, &k, ref, REF_SIZE)) != CDB_OK)
        return s;
    cdb_store_u64(idv, next, CDB_ORDER_BIG);
    if ((s = k_name(&k, CDB_OBJ_LINK_TYPE, in->name, in->name_len)) != CDB_OK
        || (s = put_both(cat, &k, idv, sizeof idv)) != CDB_OK)
        return s;
    if ((s = finish(cat, next + 1)) != CDB_OK)
        return s;
    *id = next;
    return CDB_OK;
}

cdb_status cdb_catalog_get_link_type(cdb_catalog *cat, uint64_t id, cdb_link_type_info *out)
{
    uint8_t ref[REF_SIZE];
    const uint8_t *p;
    size_t len, vlen;
    int found;
    cdb_byte_order o = cat->pager->order;
    cdb_value obj, name;
    int64_t v[6];
    unsigned f;
    key k;
    cdb_status s;

    if ((s = k_object(&k, id)) != CDB_OK || (s = get_either(cat, &k, ref, REF_SIZE, &vlen, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if (vlen != REF_SIZE)
        return CDB_E_CORRUPT;
    if ((s = read_record(cat, ref, &p, &len)) != CDB_OK)
        return s;
    if (cdb_value_validate(p, len, o, CDB_DECIMAL_COMPACT, 0) != CDB_OK || cdb_value_read(p, len, o, &obj) != CDB_OK
        || obj.type != CDB_TYPE_OBJECT)
        return CDB_E_CORRUPT;
    if ((s = field_int(&obj, o, F_KIND, &v[0])) != CDB_OK)
        return s;
    if (v[0] != CDB_OBJ_LINK_TYPE)
        return CDB_E_NOT_FOUND;
    for (f = F_ID; f <= LT_ON_DELETE; f++)
        if (f != F_NAME && (s = field_int(&obj, o, f, &v[f - 1u])) != CDB_OK)
            return s;
    if ((s = cdb_value_object_get(&obj, o, F_NAME, &name, &found)) != CDB_OK)
        return s;
    if (!found || name.type != CDB_TYPE_STRING || name.payload_len > CDB_CATALOG_NAME_MAX || (uint64_t)v[1] != id
        || v[3] < 0 || v[3] > 255 || v[4] < 0 || v[4] > 1 || v[5] < 0 || v[5] > 255)
        return CDB_E_CORRUPT;
    memset(out, 0, sizeof *out);
    out->id = id;
    memcpy(out->name, name.payload, name.payload_len);
    out->name_len = name.payload_len;
    out->cardinality = (uint8_t)v[3];
    out->enforced = (uint8_t)v[4];
    out->on_delete = (uint8_t)v[5];
    return CDB_OK;
}
