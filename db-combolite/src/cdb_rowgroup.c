/* Row groups (cdb_rowgroup.h; spec 9.12, 14.10.2). */
#include <string.h>

#include "cdb_endian.h"
#include "cdb_key.h"
#include "cdb_nib.h"
#include "cdb_overflow.h"
#include "cdb_rowgroup.h"
#include "cdb_varint.h"

#define DM CDB_DECIMAL_COMPACT
#define BIG CDB_ORDER_BIG
#define ALIGN 16u
#define HDR_FIXED 18u
#define REF_MAX (8u + 1u + 1u + CDB_RG_ADDR_MAX + 4u + 4u + 1u + CDB_RG_STAT_MAX + 1u + CDB_RG_STAT_MAX)
#define HDR_MAX (HDR_FIXED + (1u + CDB_TABLE_MAX_COLUMNS) * REF_MAX)
#define ROW_EXTRA (32u + CDB_TABLE_MAX_COLUMNS * 18u) /* an object's bytes past its values */
#define ID_ITEM_MAX (1u + CDB_VARINT_MAX_LEN)          /* an entry ID as an int value */
#define KEY_SMALL 24u                                  /* key of a fixed-size value */

/* -------------------------------------------------------- sizes */

static size_t up(size_t n)
{
    return (n + ALIGN - 1u) / ALIGN * ALIGN;
}

static size_t bm_bytes(uint32_t rows)
{
    return ((size_t)rows + 7u) / 8u;
}

static size_t chunk_cap_for(uint32_t rows, size_t cap)
{
    return 2u + bm_bytes(rows) + cdb_codec_bound(cap);
}

size_t cdb_rg_work_size(size_t page_size, uint32_t rows, size_t cap, int with_codec)
{
    size_t n = up(sizeof(cdb_rg_header)) + up(HDR_MAX) + up((size_t)rows * 8u) + up(bm_bytes(rows)) + up(cap)
             + up(cdb_colenc_scratch_words(rows) * 8u) + up(cap + ROW_EXTRA) + up(chunk_cap_for(rows, cap))
             + up(page_size);
    if (with_codec)
        n += cdb_codec_work_size(CDB_CODEC_LZMA2, cap);
    return n + ALIGN; /* the area's start may need aligning */
}

static int has_alloc(const cdb_db *db)
{
    return db->env.ops != NULL && db->env.ops->alloc != NULL && db->env.ops->free != NULL;
}

cdb_status cdb_rg_init(cdb_rg *g, cdb_db *db, uint64_t store, cdb_index *groups, uint32_t rows, size_t cap,
                       uint8_t *work, size_t work_size)
{
    size_t page = db->pager.page_size, skip, end;
    uint8_t *p;
    int alloc = has_alloc(db);

    /* The entry-ID chunk must always fit: every row's ID is at most
     * ID_ITEM_MAX bytes as a value. */
    if (rows == 0 || rows > CDB_RG_ROWS_MAX || cap < (size_t)rows * ID_ITEM_MAX || work == NULL)
        return CDB_E_INVALID_ARG;
    skip = (ALIGN - (size_t)((uintptr_t)work % ALIGN)) % ALIGN;
    if (work_size < cdb_rg_work_size(page, rows, cap, !alloc))
        return CDB_E_BUFFER_TOO_SMALL;
    memset(g, 0, sizeof *g);
    g->db = db;
    g->store = store;
    g->groups = groups;
    g->rows = rows;
    g->cap = cap;
    p = work + skip;
    g->hdr = (cdb_rg_header *)(void *)p;
    p += up(sizeof(cdb_rg_header));
    g->hbytes = p;
    p += up(HDR_MAX);
    g->entries = (uint64_t *)(void *)p;
    p += up((size_t)rows * 8u);
    g->bitmap = p;
    p += up(bm_bytes(rows));
    g->vals = p;
    p += up(cap);
    g->scratch = (uint64_t *)(void *)p;
    g->scratch_words = cdb_colenc_scratch_words(rows);
    p += up(g->scratch_words * 8u);
    g->enc = p;
    g->enc_cap = cap + ROW_EXTRA;
    p += up(g->enc_cap);
    g->chunk = p;
    g->chunk_cap = chunk_cap_for(rows, cap);
    p += up(g->chunk_cap);
    g->page = p;
    p += up(page);
    end = (size_t)(p - work);
    g->codec.env = alloc ? &db->env : NULL;
    g->codec.work = alloc ? NULL : p;
    g->codec.work_size = alloc ? 0 : work_size - end;
    return CDB_OK;
}

/* -------------------------------------------------------- tags and values */

/* Column tags: a bool column is named by the false tag. */
static int col_tag_ok(uint8_t tag)
{
    return tag == CDB_TAG_FALSE || (tag >= CDB_TAG_INT && tag <= CDB_TAG_REF)
        || (tag >= CDB_TAG_STRING && tag <= CDB_TAG_VECTOR);
}

/* Types whose values are ordered (they have keys, cdb_key.h). */
static int orderable(uint8_t tag)
{
    return tag == CDB_TAG_FALSE || (tag >= CDB_TAG_INT && tag <= CDB_TAG_REF) || tag == CDB_TAG_STRING
        || tag == CDB_TAG_BINARY;
}

static int of_column(uint8_t tag, const cdb_value *v)
{
    return tag == CDB_TAG_FALSE ? v->type == CDB_TYPE_BOOL : v->tag == tag;
}

static cdb_status small_key(const cdb_value *v, uint8_t *k, size_t *klen)
{
    cdb_key_builder kb;
    cdb_key_init(&kb, k, KEY_SMALL);
    switch (v->type) {
    case CDB_TYPE_BOOL: cdb_key_bool(&kb, v->as.boolean); break;
    case CDB_TYPE_INT: cdb_key_int(&kb, v->as.i64); break;
    case CDB_TYPE_F64: cdb_key_f64(&kb, v->as.f64); break;
    case CDB_TYPE_F32: cdb_key_f64(&kb, (double)v->as.f32); break;
    case CDB_TYPE_TIMESTAMP: cdb_key_timestamp(&kb, v->as.ts.ticks); break;
    case CDB_TYPE_REF: cdb_key_ref(&kb, v->as.ref.store_id, v->as.ref.entry_id); break;
    default: return CDB_E_INVALID_ARG;
    }
    *klen = kb.len;
    return kb.error;
}

/* Orders two values of one orderable column the way their index keys
 * would (strings and binaries: bytes, shorter first on a tie). */
static cdb_status compare(cdb_byte_order o, const uint8_t *a, size_t al, const uint8_t *b, size_t bl, int *out)
{
    cdb_value va, vb;
    uint8_t ka[KEY_SMALL], kb[KEY_SMALL];
    size_t kal, kbl;
    cdb_status s;
    if ((s = cdb_value_read(a, al, o, &va)) != CDB_OK || (s = cdb_value_read(b, bl, o, &vb)) != CDB_OK)
        return s;
    if (va.type == CDB_TYPE_STRING || va.type == CDB_TYPE_BINARY) {
        size_t n = va.payload_len < vb.payload_len ? va.payload_len : vb.payload_len;
        int c = n ? memcmp(va.payload, vb.payload, n) : 0;
        *out = c != 0 ? c : (va.payload_len < vb.payload_len ? -1 : va.payload_len > vb.payload_len);
        return CDB_OK;
    }
    if ((s = small_key(&va, ka, &kal)) != CDB_OK || (s = small_key(&vb, kb, &kbl)) != CDB_OK)
        return s;
    *out = cdb_key_compare(ka, kal, kb, kbl);
    return CDB_OK;
}

/* -------------------------------------------------------- header */

static size_t gkey(uint8_t *k, uint64_t last)
{
    cdb_key_builder kb;
    cdb_key_init(&kb, k, 8);
    cdb_key_u64(&kb, last);
    return kb.len;
}

static size_t ref_put(uint8_t *p, const cdb_rg_ref *r)
{
    size_t n, a;
    cdb_store_u64(p, r->field_id, BIG);
    p[8] = r->tag;
    a = cdb_nib_addr_store(p + 10, r->addr);
    p[9] = (uint8_t)a;
    n = 10u + a;
    cdb_store_u32(p + n, r->stored, BIG);
    cdb_store_u32(p + n + 4u, r->nulls, BIG);
    n += 8u;
    p[n++] = r->min_len;
    memcpy(p + n, r->min, r->min_len);
    n += r->min_len;
    p[n++] = r->max_len;
    memcpy(p + n, r->max, r->max_len);
    return n + r->max_len;
}

static size_t hdr_put(uint8_t *p, const cdb_rg_header *h)
{
    size_t n = HDR_FIXED;
    unsigned i;
    cdb_store_u64(p, h->first, BIG);
    cdb_store_u32(p + 8, h->rows, BIG);
    cdb_store_u32(p + 12, h->schema, BIG);
    cdb_store_u16(p + 16, (uint16_t)h->ncols, BIG);
    n += ref_put(p + n, &h->ids);
    for (i = 0; i < h->ncols; i++)
        n += ref_put(p + n, &h->cols[i]);
    return n;
}

static cdb_status ref_get(const uint8_t *p, size_t len, size_t *pos, cdb_rg_ref *r)
{
    size_t n = *pos, a;
    cdb_status s;
    if (len - n < 10u)
        return CDB_E_CORRUPT;
    r->field_id = cdb_load_u64(p + n, BIG);
    r->tag = p[n + 8u];
    a = p[n + 9u];
    n += 10u;
    if (len - n < a + 9u)
        return CDB_E_CORRUPT;
    if ((s = cdb_nib_addr_load(p + n, a, &r->addr)) != CDB_OK)
        return s;
    n += a;
    r->stored = cdb_load_u32(p + n, BIG);
    r->nulls = cdb_load_u32(p + n + 4u, BIG);
    n += 8u;
    r->min_len = p[n++];
    if (r->min_len > CDB_RG_STAT_MAX || len - n < (size_t)r->min_len + 1u)
        return CDB_E_CORRUPT;
    memcpy(r->min, p + n, r->min_len);
    n += r->min_len;
    r->max_len = p[n++];
    if (r->max_len > CDB_RG_STAT_MAX || len - n < r->max_len)
        return CDB_E_CORRUPT;
    memcpy(r->max, p + n, r->max_len);
    *pos = n + r->max_len;
    return CDB_OK;
}

/* A statistic must be a canonical value of the column's type. */
static cdb_status stat_ok(cdb_byte_order o, uint8_t tag, const uint8_t *v, size_t len)
{
    cdb_value val;
    if (cdb_value_validate(v, len, o, DM, 0) != CDB_OK || cdb_value_read(v, len, o, &val) != CDB_OK
        || val.encoded_len != len || !of_column(tag, &val))
        return CDB_E_CORRUPT;
    return CDB_OK;
}

static cdb_status hdr_get(cdb_rg *g, const uint8_t *p, size_t len, uint64_t last, cdb_rg_header *h)
{
    cdb_byte_order o = g->db->pager.order;
    size_t pos = HDR_FIXED;
    unsigned i;
    cdb_status s;
    if (len < HDR_FIXED)
        return CDB_E_CORRUPT;
    h->first = cdb_load_u64(p, BIG);
    h->last = last;
    h->rows = cdb_load_u32(p + 8, BIG);
    h->schema = cdb_load_u32(p + 12, BIG);
    h->ncols = cdb_load_u16(p + 16, BIG);
    if (h->rows == 0 || h->rows > CDB_RG_ROWS_MAX || h->first > last || last - h->first < h->rows - 1u
        || h->ncols > CDB_TABLE_MAX_COLUMNS)
        return CDB_E_CORRUPT;
    if ((s = ref_get(p, len, &pos, &h->ids)) != CDB_OK)
        return s;
    if (h->ids.field_id != 0 || h->ids.tag != CDB_TAG_INT || h->ids.nulls != 0 || h->ids.min_len
        || h->ids.max_len || h->ids.stored < 2u)
        return CDB_E_CORRUPT;
    for (i = 0; i < h->ncols; i++) {
        cdb_rg_ref *r = &h->cols[i];
        int c;
        if ((s = ref_get(p, len, &pos, r)) != CDB_OK)
            return s;
        if (r->field_id == 0 || (i > 0 && r->field_id <= h->cols[i - 1u].field_id) || !col_tag_ok(r->tag)
            || r->nulls > h->rows || r->stored < 2u)
            return CDB_E_CORRUPT;
        /* Statistics come in pairs, only for ordered types with values. */
        if ((r->min_len == 0) != (r->max_len == 0) || (r->min_len && (!orderable(r->tag) || r->nulls == h->rows)))
            return CDB_E_CORRUPT;
        if (r->min_len) {
            if ((s = stat_ok(o, r->tag, r->min, r->min_len)) != CDB_OK
                || (s = stat_ok(o, r->tag, r->max, r->max_len)) != CDB_OK
                || (s = compare(o, r->min, r->min_len, r->max, r->max_len, &c)) != CDB_OK)
                return s;
            if (c > 0)
                return CDB_E_CORRUPT;
        }
    }
    return pos == len ? CDB_OK : CDB_E_CORRUPT;
}

static cdb_status hdr_store(cdb_rg *g)
{
    uint8_t k[8];
    size_t n = hdr_put(g->hbytes, g->hdr);
    return cdb_index_put(g->groups, k, gkey(k, g->hdr->last), g->hbytes, n);
}

cdb_status cdb_rg_load(cdb_rg *g, uint64_t last, int *found)
{
    uint8_t k[8];
    size_t vlen;
    cdb_status s;
    if ((s = cdb_index_get(g->groups, k, gkey(k, last), g->hbytes, HDR_MAX, &vlen, found)) != CDB_OK)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
    if (!*found)
        return CDB_OK;
    return hdr_get(g, g->hbytes, vlen, last, g->hdr);
}

cdb_status cdb_rg_find(cdb_rg *g, uint64_t entry, int *found)
{
    cdb_index_cursor c;
    uint8_t k[16];
    size_t klen, vlen;
    int done;
    cdb_status s;
    *found = 0;
    if ((s = cdb_index_seek(&c, g->groups, g->page, k, gkey(k, entry))) != CDB_OK
        || (s = cdb_index_next(&c, k, sizeof k, &klen, g->hbytes, HDR_MAX, &vlen, &done)) != CDB_OK)
        return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
    if (done)
        return CDB_OK;
    if (klen != 8)
        return CDB_E_CORRUPT;
    if ((s = hdr_get(g, g->hbytes, vlen, cdb_load_u64(k, BIG), g->hdr)) != CDB_OK)
        return s;
    *found = 1;
    return CDB_OK;
}

/* -------------------------------------------------------- chunks */

static void okey_of(uint8_t *k, uint64_t last, uint64_t field)
{
    cdb_store_u64(k, last, BIG);
    cdb_store_u64(k + 8, field, BIG);
}

static cdb_status put_chunk(cdb_rg *g, uint64_t last, cdb_rg_ref *r, size_t clen)
{
    uint8_t ok[16];
    if (clen > UINT32_MAX)
        return CDB_E_OVERFLOW;
    r->stored = (uint32_t)clen;
    if (cdb_nib_fits(g->db, clen)) {
        okey_of(ok, last, r->field_id);
        return cdb_nib_put(g->db, g->store, CDB_NIB_KIND_COLCHUNK, ok, sizeof ok, g->chunk, clen, &r->addr);
    }
    {
        uint8_t tokey[17];
        tokey[0] = CDB_OVERFLOW_OWNER_COL_CHUNK;
        okey_of(tokey + 1, last, r->field_id);
        return cdb_overflow_write_db(g->db, &g->db->pager, g->store, g->chunk, clen, g->page, tokey, sizeof tokey, &r->addr);
    }
}

static cdb_status free_chunk(cdb_rg *g, const cdb_rg_ref *r)
{
    if (cdb_nib_is(r->addr))
        return cdb_nib_free(g->db, r->addr);
    return cdb_overflow_free_db(g->db, &g->db->pager, r->addr, g->store, r->stored, g->page);
}

/* Encodes the vl bytes of present values in g->vals, with g->bitmap when
 * there are nulls, into a chunk in g->chunk. */
static cdb_status make_chunk(cdb_rg *g, uint8_t tag, uint32_t rows, uint32_t nulls, size_t vl, size_t *clen)
{
    size_t bm = nulls ? bm_bytes(rows) : 0u, elen, plen;
    unsigned e;
    cdb_status s;
    if ((s = cdb_colenc_encode(tag, g->db->pager.order, DM, g->vals, vl, (size_t)(rows - nulls), g->scratch,
                               g->scratch_words, g->enc, g->enc_cap, &e, &elen)) != CDB_OK)
        return s;
    g->chunk[0] = (uint8_t)e;
    g->chunk[1] = (uint8_t)CDB_CODEC_NONE;
    memcpy(g->chunk + 2, g->bitmap, bm);
    /* Column chunks are always tried (spec 15.4). */
    if (elen > 0 && cdb_codec_available(CDB_CODEC_LZMA2)
        && cdb_codec_worth_trying(elen, g->db->pager.page_size, 1)) {
        if ((s = cdb_codec_compress(&g->codec, CDB_CODEC_LZMA2, g->enc, elen, g->chunk + 2 + bm,
                                    g->chunk_cap - 2u - bm, &plen)) != CDB_OK)
            return s;
        if (cdb_codec_keeps(elen, plen)) {
            g->chunk[1] = (uint8_t)CDB_CODEC_LZMA2;
            *clen = 2u + bm + plen;
            return CDB_OK;
        }
    }
    memcpy(g->chunk + 2 + bm, g->enc, elen);
    *clen = 2u + bm + elen;
    return CDB_OK;
}

/* Reads a chunk into g->chunk and checks its frame; *data and *dlen are its
 * encoded values (in g->chunk, or decompressed into g->enc). */
static cdb_status load_chunk(cdb_rg *g, const cdb_rg_ref *r, uint32_t rows, unsigned *enc, const uint8_t **data,
                             size_t *dlen)
{
    size_t bm = r->nulls ? bm_bytes(rows) : 0u, len, plain, i;
    uint32_t count = 0;
    const uint8_t *pl;
    cdb_status s;
    if (r->stored > g->chunk_cap || r->stored < 2u + bm)
        return CDB_E_CORRUPT;
    /* A chunk is a nibblet exactly when it fits one (spec 16.1). */
    if (cdb_nib_is(r->addr) != cdb_nib_fits(g->db, r->stored))
        return CDB_E_NOT_CANONICAL;
    if (cdb_nib_is(r->addr)) {
        if ((s = cdb_nib_get(g->db, r->addr, g->chunk, g->chunk_cap, &len)) != CDB_OK)
            return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
        if (len != r->stored)
            return CDB_E_CORRUPT;
    } else if ((s = cdb_overflow_read_db(g->db, &g->db->pager, r->addr, g->store, g->chunk, r->stored, g->page)) != CDB_OK) {
        return s;
    }
    if (g->chunk[0] > CDB_COLENC_BITPACK)
        return CDB_E_UNSUPPORTED;
    *enc = g->chunk[0];
    for (i = 0; i < bm; i++) {
        uint8_t b = g->chunk[2u + i];
        while (b) {
            count += b & 1u;
            b = (uint8_t)(b >> 1);
        }
    }
    if (count != r->nulls || (bm && (rows % 8u) && (g->chunk[1u + bm] & (0xFFu >> (rows % 8u)))))
        return CDB_E_CORRUPT;
    pl = g->chunk + 2u + bm;
    len = r->stored - 2u - bm;
    if (g->chunk[1] == CDB_CODEC_NONE) {
        *data = pl;
        *dlen = len;
        return CDB_OK;
    }
    if (g->chunk[1] != CDB_CODEC_LZMA2)
        return CDB_E_UNSUPPORTED;
    if (!cdb_codec_available(CDB_CODEC_LZMA2))
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_codec_length(pl, len, &plain)) != CDB_OK)
        return s;
    if (plain > g->cap || plain == 0)
        return CDB_E_CORRUPT;
    if (!cdb_codec_keeps(plain, len))
        return CDB_E_NOT_CANONICAL;
    if ((s = cdb_codec_decompress(&g->codec, CDB_CODEC_LZMA2, pl, len, g->enc, g->enc_cap, dlen)) != CDB_OK)
        return s;
    *data = g->enc;
    return CDB_OK;
}

/* -------------------------------------------------------- columns */

cdb_status cdb_rg_column_open(cdb_rg *g, int i, cdb_rg_column *c)
{
    const cdb_rg_ref *r;
    const uint8_t *data;
    size_t dlen;
    unsigned enc;
    cdb_status s;
    if (i >= (int)g->hdr->ncols)
        return CDB_E_INVALID_ARG;
    r = i < 0 ? &g->hdr->ids : &g->hdr->cols[i];
    memset(c, 0, sizeof *c);
    c->g = g;
    c->ref = r;
    if ((s = load_chunk(g, r, g->hdr->rows, &enc, &data, &dlen)) != CDB_OK)
        return s;
    c->bitmap = r->nulls ? g->chunk + 2 : NULL;
    return cdb_colenc_reader_init(&c->r, r->tag, g->db->pager.order, DM, enc, data, dlen,
                                  (size_t)(g->hdr->rows - r->nulls), g->scratch, g->scratch_words);
}

cdb_status cdb_rg_column_next(cdb_rg_column *c, uint8_t *dst, size_t dcap, size_t *vlen, int *present,
                              int *done)
{
    const uint8_t *item;
    size_t il;
    int d;
    cdb_status s;
    *present = 0;
    *vlen = 0;
    if (c->row == c->g->hdr->rows) {
        *done = 1;
        return cdb_colenc_reader_finish(&c->r);
    }
    *done = 0;
    if (c->bitmap != NULL && (c->bitmap[c->row / 8u] & (0x80u >> (c->row % 8u)))) {
        c->row++;
        return CDB_OK;
    }
    if ((s = cdb_colenc_next(&c->r, &item, &il, &d)) != CDB_OK)
        return s;
    if (d)
        return CDB_E_CORRUPT;
    if (c->ref->tag == CDB_TAG_FALSE) {
        if (dcap < 1u)
            return CDB_E_BUFFER_TOO_SMALL;
        dst[0] = item[0] ? (uint8_t)CDB_TAG_TRUE : (uint8_t)CDB_TAG_FALSE;
        *vlen = 1;
    } else {
        if (dcap < il + 1u)
            return CDB_E_BUFFER_TOO_SMALL;
        dst[0] = c->ref->tag;
        memcpy(dst + 1, item, il);
        *vlen = il + 1u;
    }
    *present = 1;
    c->row++;
    return CDB_OK;
}

cdb_status cdb_rg_load_entries(cdb_rg *g)
{
    cdb_rg_column c;
    uint8_t v[ID_ITEM_MAX];
    size_t vl;
    uint32_t i = 0;
    int present, done;
    cdb_status s;
    if ((s = cdb_rg_column_open(g, -1, &c)) != CDB_OK)
        return s;
    for (;;) {
        cdb_value val;
        if ((s = cdb_rg_column_next(&c, v, sizeof v, &vl, &present, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        if (!present || cdb_value_read(v, vl, g->db->pager.order, &val) != CDB_OK || val.as.i64 < 0
            || (i > 0 && (uint64_t)val.as.i64 <= g->entries[i - 1u]))
            return CDB_E_CORRUPT;
        g->entries[i++] = (uint64_t)val.as.i64;
    }
    if (g->entries[0] != g->hdr->first || g->entries[g->hdr->rows - 1u] != g->hdr->last)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

/* -------------------------------------------------------- building */

static void free_built(cdb_rg *g, int ids, unsigned ncols)
{
    unsigned i;
    if (ids)
        (void)free_chunk(g, &g->hdr->ids);
    for (i = 0; i < ncols; i++)
        (void)free_chunk(g, &g->hdr->cols[i]);
}

/* Reads the n entry IDs into g->entries and, as int values, g->vals. */
static cdb_status gather_ids(cdb_rg *g, const cdb_rg_source *src, uint32_t n, size_t *vl)
{
    size_t used = 0;
    uint32_t i;
    cdb_status s;
    if ((s = src->rewind(src->ctx)) != CDB_OK)
        return s;
    for (i = 0; i < n; i++) {
        cdb_value_writer w; /* one root value per writer */
        const uint8_t *v;
        size_t l;
        uint64_t e;
        if ((s = src->next(src->ctx, 0, &e, &v, &l)) != CDB_OK)
            return s;
        if (e > (uint64_t)INT64_MAX || (i > 0 && e <= g->entries[i - 1u]))
            return CDB_E_INVALID_ARG;
        g->entries[i] = e;
        cdb_value_writer_init(&w, g->vals + used, g->cap - used, g->db->pager.order, DM);
        if ((s = cdb_value_write_int(&w, (int64_t)e)) != CDB_OK)
            return s;
        used += cdb_value_writer_len(&w);
    }
    *vl = used;
    return CDB_OK;
}

/* Reads one column's values into g->vals (present ones) and g->bitmap (the
 * nulls), with its null count and statistics in r. */
static cdb_status gather(cdb_rg *g, const cdb_rg_source *src, uint32_t n, cdb_rg_ref *r, size_t *vl_out)
{
    cdb_byte_order o = g->db->pager.order;
    size_t vl = 0, mino = 0, minl = 0, maxo = 0, maxl = 0;
    uint32_t i;
    int have = 0, c;
    cdb_status s;
    memset(g->bitmap, 0, bm_bytes(n));
    r->nulls = 0;
    if ((s = src->rewind(src->ctx)) != CDB_OK)
        return s;
    for (i = 0; i < n; i++) {
        const uint8_t *v;
        size_t l;
        uint64_t e;
        cdb_value val;
        if ((s = src->next(src->ctx, r->field_id, &e, &v, &l)) != CDB_OK)
            return s;
        if (e != g->entries[i])
            return CDB_E_INVALID_ARG;
        if (v == NULL) {
            g->bitmap[i / 8u] = (uint8_t)(g->bitmap[i / 8u] | (0x80u >> (i % 8u)));
            r->nulls++;
            continue;
        }
        if (cdb_value_read(v, l, o, &val) != CDB_OK || val.encoded_len != l || !of_column(r->tag, &val))
            return CDB_E_INVALID_ARG;
        if (l > g->cap - vl)
            return CDB_E_OVERFLOW;
        memcpy(g->vals + vl, v, l);
        if (orderable(r->tag)) {
            if (!have) {
                mino = maxo = vl;
                minl = maxl = l;
                have = 1;
            } else {
                if ((s = compare(o, g->vals + vl, l, g->vals + mino, minl, &c)) != CDB_OK)
                    return s;
                if (c < 0) {
                    mino = vl;
                    minl = l;
                }
                if ((s = compare(o, g->vals + vl, l, g->vals + maxo, maxl, &c)) != CDB_OK)
                    return s;
                if (c > 0) {
                    maxo = vl;
                    maxl = l;
                }
            }
        }
        vl += l;
    }
    r->min_len = r->max_len = 0;
    if (have && minl <= CDB_RG_STAT_MAX && maxl <= CDB_RG_STAT_MAX) {
        memcpy(r->min, g->vals + mino, minl);
        memcpy(r->max, g->vals + maxo, maxl);
        r->min_len = (uint8_t)minl;
        r->max_len = (uint8_t)maxl;
    }
    *vl_out = vl;
    return CDB_OK;
}

cdb_status cdb_rg_write(cdb_rg *g, const cdb_rg_source *src, uint32_t n, uint32_t schema, const cdb_rg_col *cols,
                        unsigned ncols)
{
    cdb_rg_header *h = g->hdr;
    size_t vl, clen;
    unsigned i, built = 0;
    int found;
    uint64_t first, last;
    cdb_status s;

    if (src == NULL || n == 0 || n > g->rows || ncols > CDB_TABLE_MAX_COLUMNS || (ncols > 0 && cols == NULL))
        return CDB_E_INVALID_ARG;
    for (i = 0; i < ncols; i++)
        if (cols[i].field_id == 0 || (i > 0 && cols[i].field_id <= cols[i - 1u].field_id)
            || !col_tag_ok(cols[i].tag == CDB_TAG_TRUE ? (uint8_t)CDB_TAG_FALSE : cols[i].tag))
            return CDB_E_INVALID_ARG;
    if ((s = gather_ids(g, src, n, &vl)) != CDB_OK)
        return s;
    first = g->entries[0];
    last = g->entries[n - 1u];
    /* Groups never overlap. */
    if ((s = cdb_rg_find(g, first, &found)) != CDB_OK)
        return s;
    if (found && h->first <= last)
        return CDB_E_EXISTS;
    memset(h, 0, sizeof *h);
    h->first = first;
    h->last = last;
    h->rows = n;
    h->schema = schema;
    h->ncols = ncols;
    h->ids.tag = CDB_TAG_INT;
    if ((s = make_chunk(g, CDB_TAG_INT, n, 0, vl, &clen)) != CDB_OK
        || (s = put_chunk(g, last, &h->ids, clen)) != CDB_OK)
        return s;
    for (i = 0; i < ncols; i++) {
        cdb_rg_ref *r = &h->cols[i];
        r->field_id = cols[i].field_id;
        r->tag = cols[i].tag == CDB_TAG_TRUE ? (uint8_t)CDB_TAG_FALSE : cols[i].tag;
        if ((s = gather(g, src, n, r, &vl)) != CDB_OK || (s = make_chunk(g, r->tag, n, r->nulls, vl, &clen)) != CDB_OK
            || (s = put_chunk(g, last, r, clen)) != CDB_OK) {
            free_built(g, 1, built);
            return s;
        }
        built++;
    }
    if ((s = hdr_store(g)) != CDB_OK)
        free_built(g, 1, built);
    return s;
}

/* -------------------------------------------------------- reading */

cdb_status cdb_rg_get_row(cdb_rg *g, uint64_t entry, void *out, size_t cap, size_t *len, int *found)
{
    const cdb_rg_header *h = g->hdr;
    uint64_t fid[CDB_TABLE_MAX_COLUMNS];
    size_t off[CDB_TABLE_MAX_COLUMNS], vlen[CDB_TABLE_MAX_COLUMNS], used = 0;
    unsigned i, n = 0;
    uint32_t lo, hi, p;
    cdb_value_writer w;
    cdb_status s;

    *len = 0;
    if ((s = cdb_rg_find(g, entry, found)) != CDB_OK || !*found)
        return s;
    if (h->first > entry) {
        *found = 0;
        return CDB_OK;
    }
    if ((s = cdb_rg_load_entries(g)) != CDB_OK)
        return s;
    lo = 0;
    hi = h->rows;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (g->entries[mid] < entry)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == h->rows || g->entries[lo] != entry) {
        *found = 0;
        return CDB_OK;
    }
    p = lo;
    /* Each present value lands side by side in g->vals; the object is then
     * built in g->enc, which no column uses any more. */
    for (i = 0; i < h->ncols; i++) {
        cdb_rg_column c;
        uint32_t r;
        size_t l = 0;
        int present = 0, done;
        if (h->cols[i].nulls == h->rows)
            continue;
        if ((s = cdb_rg_column_open(g, (int)i, &c)) != CDB_OK)
            return s;
        for (r = 0; r <= p; r++) {
            if ((s = cdb_rg_column_next(&c, g->vals + used, g->cap - used, &l, &present, &done)) != CDB_OK)
                return s == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : s;
            if (done)
                return CDB_E_CORRUPT;
        }
        if (present) {
            fid[n] = h->cols[i].field_id;
            off[n] = used;
            vlen[n++] = l;
            used += l;
        }
    }
    cdb_value_writer_init(&w, g->enc, g->enc_cap, g->db->pager.order, DM);
    cdb_value_begin_object(&w);
    for (i = 0; i < n; i++) {
        cdb_value_field(&w, fid[i]);
        cdb_value_write_encoded(&w, g->vals + off[i], vlen[i]);
    }
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error == CDB_E_BUFFER_TOO_SMALL ? CDB_E_CORRUPT : w.error;
    *len = cdb_value_writer_len(&w);
    if (*len > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memcpy(out, g->enc, *len);
    return CDB_OK;
}

/* -------------------------------------------------------- removal and moves */

cdb_status cdb_rg_drop(cdb_rg *g, uint64_t last)
{
    uint8_t k[8];
    unsigned i;
    int found;
    cdb_status s;
    if ((s = cdb_rg_load(g, last, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if ((s = free_chunk(g, &g->hdr->ids)) != CDB_OK)
        return s;
    for (i = 0; i < g->hdr->ncols; i++)
        if ((s = free_chunk(g, &g->hdr->cols[i])) != CDB_OK)
            return s;
    return cdb_index_delete(g->groups, k, gkey(k, last), &found);
}

cdb_status cdb_rg_nib_moved(cdb_rg *g, const uint8_t *okey, size_t oklen, uint64_t old_addr, uint64_t addr)
{
    uint64_t last, field;
    cdb_rg_ref *r = NULL;
    unsigned i;
    int found;
    cdb_status s;
    if (oklen != 16)
        return CDB_E_CORRUPT;
    last = cdb_load_u64(okey, BIG);
    field = cdb_load_u64(okey + 8, BIG);
    if ((s = cdb_rg_load(g, last, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    if (field == 0)
        r = &g->hdr->ids;
    for (i = 0; r == NULL && i < g->hdr->ncols; i++)
        if (g->hdr->cols[i].field_id == field)
            r = &g->hdr->cols[i];
    if (r == NULL || r->addr != old_addr)
        return CDB_E_NOT_FOUND;
    r->addr = addr;
    return hdr_store(g);
}
