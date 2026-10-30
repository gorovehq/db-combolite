/* Nibblets (cdb_nib.h; spec 16, 14.18). */
#include <string.h>

#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_nib.h"
#include "cdb_page.h"
#include "cdb_table.h"
#include "cdb_ts.h"

/* Page payload (spec 14.18.1): domain (8), slot count (2), lowest record
 * offset (2), free bytes (2), then the slot directory, 4 bytes a slot:
 * record offset (2) and length (2), length 0 for a free slot. Records:
 * owner store (8), kind (1), owner key length (1), owner key, data. */
#define P_DOMAIN 0u
#define P_COUNT 8u
#define P_LOW 10u
#define P_FREE 12u
#define P_DIR 14u
#define SLOT_SIZE 4u
#define REC_HDR 10u
#define PAGE_MASK ((UINT64_C(1) << 47) - 1u)

/* ------------------------------------------------------------ addresses */

int cdb_nib_is(uint64_t addr)
{
    return (addr & CDB_NIB_ADDR_FLAG) != 0;
}

uint64_t cdb_nib_page(uint64_t addr)
{
    return cdb_nib_is(addr) ? (addr >> 16) & PAGE_MASK : addr;
}

unsigned cdb_nib_slot(uint64_t addr)
{
    return (unsigned)(addr & 0xFFFFu);
}

static uint64_t make_addr(uint64_t page, unsigned slot)
{
    return CDB_NIB_ADDR_FLAG | (page << 16) | slot;
}

size_t cdb_nib_addr_store(uint8_t *out, uint64_t addr)
{
    cdb_store_u64(out, cdb_nib_page(addr), CDB_ORDER_BIG);
    if (!cdb_nib_is(addr))
        return 8;
    cdb_store_u16(out + 8, (uint16_t)cdb_nib_slot(addr), CDB_ORDER_BIG);
    return 10;
}

cdb_status cdb_nib_addr_load(const uint8_t *in, size_t len, uint64_t *addr)
{
    uint64_t page;
    if (len != 8 && len != 10)
        return CDB_E_CORRUPT;
    page = cdb_load_u64(in, CDB_ORDER_BIG);
    if (len == 8) {
        *addr = page;
        return CDB_OK;
    }
    if (page > PAGE_MASK)
        return CDB_E_CORRUPT;
    *addr = make_addr(page, cdb_load_u16(in + 8, CDB_ORDER_BIG));
    return CDB_OK;
}

/* ------------------------------------------------------------ pages */

static size_t cap_of(const cdb_db *db)
{
    return cdb_page_capacity(db->pager.fmt, db->pager.page_size);
}

int cdb_nib_fits(const cdb_db *db, size_t len)
{
    return db->cfg.doc_buffers != NULL && len <= db->pager.page_size / 4u;
}

/* Nibblet page buffer i (0 or 1): the last two pages of the document
 * buffers (cdb_doc_buffers_size), and its payload. */
static uint8_t *page_buf(cdb_db *db, unsigned i)
{
    return db->cfg.doc_buffers + 3u * cdb_doc_buffer_size(db->pager.page_size, db->cfg.doc_max)
         + (size_t)(3u + i) * db->pager.page_size;
}

static uint8_t *payload(cdb_db *db, unsigned i)
{
    return page_buf(db, i) + cdb_page_header_size(db->pager.fmt);
}

static unsigned get16(const uint8_t *p, size_t off, cdb_byte_order o)
{
    return cdb_load_u16(p + off, o);
}

static void put16(uint8_t *p, size_t off, unsigned v, cdb_byte_order o)
{
    cdb_store_u16(p + off, (uint16_t)v, o);
}

/* Reads nibblet page `page` into buffer i and checks its header fields. */
static cdb_status read_page(cdb_db *db, uint64_t page, unsigned i)
{
    cdb_page_header h;
    cdb_byte_order o = db->pager.order;
    uint8_t *p;
    size_t cap = cap_of(db);
    unsigned count, low;
    cdb_status s;
    if (db->cfg.doc_buffers == NULL)
        return CDB_E_UNSUPPORTED;
    if ((s = cdb_pager_read(&db->pager, page, page_buf(db, i), &h)) != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    p = payload(db, i);
    if (h.type != CDB_PAGE_TYPE_NIBBLET || h.payload_len != cap)
        return CDB_E_CORRUPT;
    count = get16(p, P_COUNT, o);
    low = get16(p, P_LOW, o);
    if (P_DIR + (size_t)count * SLOT_SIZE > low || low > cap || get16(p, P_FREE, o) > cap)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

static cdb_status write_page(cdb_db *db, uint64_t page, unsigned i)
{
    cdb_page_header t;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_NIBBLET;
    return cdb_pager_write(&db->pager, page, &t, payload(db, i), cap_of(db));
}

static void init_page(cdb_db *db, unsigned i, uint64_t domain)
{
    uint8_t *p = payload(db, i);
    cdb_byte_order o = db->pager.order;
    size_t cap = cap_of(db);
    memset(p, 0, cap);
    cdb_store_u64(p + P_DOMAIN, domain, o);
    put16(p, P_COUNT, 0, o);
    put16(p, P_LOW, (unsigned)cap, o);
    put16(p, P_FREE, (unsigned)(cap - P_DIR), o);
}

/* Slot i's record (offset and length); length 0 for a free slot. */
static void slot_get(const uint8_t *p, unsigned i, cdb_byte_order o, unsigned *off, unsigned *len)
{
    *off = cdb_load_u16(p + P_DIR + (size_t)i * SLOT_SIZE, o);
    *len = cdb_load_u16(p + P_DIR + (size_t)i * SLOT_SIZE + 2u, o);
}

static void slot_set(uint8_t *p, unsigned i, cdb_byte_order o, unsigned off, unsigned len)
{
    put16(p, P_DIR + (size_t)i * SLOT_SIZE, off, o);
    put16(p, P_DIR + (size_t)i * SLOT_SIZE + 2u, len, o);
}

/* Closes the gaps between records: moves every record as far toward the
 * end of the page as it goes, highest offset first, so each move goes up
 * and never overwrites a record not yet moved. Slots keep their numbers. */
static void compact(cdb_db *db, uint8_t *p)
{
    cdb_byte_order o = db->pager.order;
    unsigned count = get16(p, P_COUNT, o), i;
    size_t top = cap_of(db);
    unsigned last = 0x10000u; /* above any offset */
    for (;;) {
        unsigned best = count, best_off = 0;
        for (i = 0; i < count; i++) {
            unsigned off, len;
            slot_get(p, i, o, &off, &len);
            if (len && off < last && (best == count || off > best_off)) {
                best = i;
                best_off = off;
            }
        }
        if (best == count)
            break;
        {
            unsigned off, len;
            slot_get(p, best, o, &off, &len);
            top -= len;
            memmove(p + top, p + off, len);
            slot_set(p, best, o, (unsigned)top, len);
            last = off;
        }
    }
    put16(p, P_LOW, (unsigned)top, o);
}

/* Puts a record of rlen bytes, already assembled in rec, into the page in
 * payload p. The caller has checked the page's free bytes cover rlen plus a
 * new slot. Returns the slot. */
static unsigned place(cdb_db *db, uint8_t *p, const uint8_t *rec, size_t rlen)
{
    cdb_byte_order o = db->pager.order;
    unsigned count = get16(p, P_COUNT, o), slot = count, i, low;
    size_t dir_end, need;
    for (i = 0; i < count; i++) {
        unsigned off, len;
        slot_get(p, i, o, &off, &len);
        if (len == 0) {
            slot = i;
            break;
        }
    }
    need = rlen + (slot == count ? SLOT_SIZE : 0u);
    dir_end = P_DIR + (size_t)(slot == count ? count + 1u : count) * SLOT_SIZE;
    if (get16(p, P_LOW, o) < dir_end || get16(p, P_LOW, o) - dir_end < rlen)
        compact(db, p);
    low = get16(p, P_LOW, o) - (unsigned)rlen;
    memcpy(p + low, rec, rlen);
    if (slot == count)
        put16(p, P_COUNT, count + 1u, o);
    slot_set(p, slot, o, low, (unsigned)rlen);
    put16(p, P_LOW, low, o);
    put16(p, P_FREE, get16(p, P_FREE, o) - (unsigned)need, o);
    return slot;
}

/* Removes slot i's record; trailing free slots are dropped. Returns the
 * number of records left. */
static unsigned unplace(cdb_db *db, uint8_t *p, unsigned slot)
{
    cdb_byte_order o = db->pager.order;
    unsigned count = get16(p, P_COUNT, o), off, len, live = 0, i;
    unsigned freed;
    slot_get(p, slot, o, &off, &len);
    freed = len;
    slot_set(p, slot, o, 0, 0);
    while (count > 0) {
        slot_get(p, count - 1u, o, &off, &len);
        if (len)
            break;
        count--;
        freed += SLOT_SIZE;
    }
    put16(p, P_COUNT, count, o);
    put16(p, P_FREE, get16(p, P_FREE, o) + freed, o);
    for (i = 0; i < count; i++) {
        slot_get(p, i, o, &off, &len);
        live += len != 0;
    }
    return live;
}

/* ------------------------------------------------------------ free-space index */

static size_t fkey(uint8_t *k, uint64_t domain, unsigned free_bytes, uint64_t page)
{
    cdb_store_u64(k, domain, CDB_ORDER_BIG);
    cdb_store_u16(k + 8, (uint16_t)free_bytes, CDB_ORDER_BIG);
    cdb_store_u64(k + 10, page, CDB_ORDER_BIG);
    return 18;
}

/* Opens the free-space index, creating it (and recording its root in the
 * catalog root slot) on first use when create is set. *have 0: none yet. */
static cdb_status open_index(cdb_db *db, cdb_index *ix, int create, int *have)
{
    cdb_status s;
    *have = 0;
    if (db->pager.catroot.nib_root != 0) {
        if ((s = cdb_index_open(ix, &db->pager, db->pager.catroot.nib_root, &db->cfg.catalog_work)) != CDB_OK)
            return s;
        *have = 1;
        return CDB_OK;
    }
    if (!create)
        return CDB_OK;
    if ((s = cdb_index_create(ix, &db->pager, 0, CDB_INDEX_FLAG_UNIQUE, &db->cfg.catalog_work)) != CDB_OK)
        return s;
    {
        cdb_catroot r = db->pager.catroot;
        r.nib_root = ix->root;
        cdb_pager_set_catroot(&db->pager, &r);
    }
    *have = 1;
    return CDB_OK;
}

/* Moves page's key from free bytes `old` (none if old_present is 0) to
 * `now` (none if the page is gone). */
static cdb_status rekey(cdb_index *ix, uint64_t page, int old_present, unsigned old, int now_present, unsigned now)
{
    uint8_t k[18];
    int found;
    cdb_status s;
    if (old_present && (s = cdb_index_delete(ix, k, fkey(k, 0, old, page), &found)) != CDB_OK)
        return s;
    if (now_present)
        return cdb_index_put(ix, k, fkey(k, 0, now, page), NULL, 0);
    return CDB_OK;
}

/* ------------------------------------------------------------ records */

cdb_status cdb_nib_put(cdb_db *db, uint64_t store, unsigned kind, const uint8_t *okey, size_t oklen,
                       const void *data, size_t len, uint64_t *addr)
{
    uint8_t k[18];
    size_t rlen = REC_HDR + oklen + len, need = rlen + SLOT_SIZE;
    cdb_byte_order o = db->pager.order;
    cdb_index ix;
    cdb_index_cursor c;
    uint64_t page = 0;
    unsigned old_free = 0, slot;
    int have, found_page = 0, done;
    size_t klen, vlen;
    uint8_t *p;
    cdb_status s;

    if (!cdb_nib_fits(db, len) || oklen > CDB_NIB_KEY_MAX || need > cap_of(db) - P_DIR)
        return CDB_E_INVALID_ARG;
    if ((s = open_index(db, &ix, 1, &have)) != CDB_OK)
        return s;
    /* Best fit: the smallest free space of the domain that holds the record
     * and a new slot. */
    if ((s = cdb_index_seek(&c, &ix, page_buf(db, 1), k, fkey(k, 0, (unsigned)need, 0))) != CDB_OK
        || (s = cdb_index_next(&c, k, sizeof k, &klen, NULL, 0, &vlen, &done)) != CDB_OK)
        return s;
    if (!done && klen == 18 && cdb_load_u64(k, CDB_ORDER_BIG) == 0) {
        page = cdb_load_u64(k + 10, CDB_ORDER_BIG);
        old_free = cdb_load_u16(k + 8, CDB_ORDER_BIG);
        found_page = 1;
    }
    if (found_page) {
        if ((s = read_page(db, page, 0)) != CDB_OK)
            return s;
        if (get16(payload(db, 0), P_FREE, o) != old_free)
            return CDB_E_CORRUPT; /* the index and the page disagree */
    } else {
        if ((s = cdb_pager_alloc(&db->pager, &page)) != CDB_OK)
            return s;
        if (page > PAGE_MASK)
            return CDB_E_OVERFLOW;
        init_page(db, 0, 0);
    }
    p = payload(db, 0);
    /* The record is assembled in the second buffer (the cursor is done
     * with it), then place() copies it into position. */
    {
        uint8_t *rec = page_buf(db, 1);
        cdb_store_u64(rec, store, CDB_ORDER_BIG);
        rec[8] = (uint8_t)kind;
        rec[9] = (uint8_t)oklen;
        memcpy(rec + REC_HDR, okey, oklen);
        memcpy(rec + REC_HDR + oklen, data, len);
        slot = place(db, p, rec, rlen);
    }
    if ((s = write_page(db, page, 0)) != CDB_OK)
        return s;
    if ((s = rekey(&ix, page, found_page, old_free, 1, get16(p, P_FREE, o))) != CDB_OK)
        return s;
    *addr = make_addr(page, slot);
    return CDB_OK;
}

/* The record in slot `slot` of the page in buffer i: header fields and
 * where its data is. CDB_E_CORRUPT for a free or malformed slot. */
static cdb_status record_at(cdb_db *db, unsigned i, unsigned slot, uint64_t *store, unsigned *kind,
                            const uint8_t **okey, size_t *oklen, const uint8_t **data, size_t *len)
{
    const uint8_t *p = payload(db, i), *r;
    cdb_byte_order o = db->pager.order;
    unsigned off, rlen;
    if (slot >= get16(p, P_COUNT, o))
        return CDB_E_CORRUPT;
    slot_get(p, slot, o, &off, &rlen);
    if (rlen < REC_HDR || off < get16(p, P_LOW, o) || (size_t)off + rlen > cap_of(db))
        return CDB_E_CORRUPT;
    r = p + off;
    *store = cdb_load_u64(r, CDB_ORDER_BIG);
    *kind = r[8];
    *oklen = r[9];
    if (REC_HDR + *oklen > rlen)
        return CDB_E_CORRUPT;
    *okey = r + REC_HDR;
    *data = r + REC_HDR + *oklen;
    *len = rlen - REC_HDR - *oklen;
    return CDB_OK;
}

cdb_status cdb_nib_get(cdb_db *db, uint64_t addr, void *out, size_t cap, size_t *len)
{
    const uint8_t *okey, *data;
    size_t oklen;
    uint64_t store;
    unsigned kind;
    cdb_status s;
    if (!cdb_nib_is(addr))
        return CDB_E_INVALID_ARG;
    if ((s = read_page(db, cdb_nib_page(addr), 0)) != CDB_OK
        || (s = record_at(db, 0, cdb_nib_slot(addr), &store, &kind, &okey, &oklen, &data, len)) != CDB_OK)
        return s;
    if (*len > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memcpy(out, data, *len);
    return CDB_OK;
}

cdb_status cdb_nib_free(cdb_db *db, uint64_t addr)
{
    uint64_t page = cdb_nib_page(addr);
    cdb_byte_order o = db->pager.order;
    cdb_index ix;
    unsigned old_free, live;
    int have;
    cdb_status s;
    if (!cdb_nib_is(addr))
        return CDB_E_INVALID_ARG;
    if ((s = read_page(db, page, 0)) != CDB_OK)
        return s;
    {
        const uint8_t *okey, *data;
        size_t oklen, len;
        uint64_t store;
        unsigned kind;
        if ((s = record_at(db, 0, cdb_nib_slot(addr), &store, &kind, &okey, &oklen, &data, &len)) != CDB_OK)
            return s;
    }
    old_free = get16(payload(db, 0), P_FREE, o);
    live = unplace(db, payload(db, 0), cdb_nib_slot(addr));
    if ((s = open_index(db, &ix, 0, &have)) != CDB_OK)
        return s;
    if (!have)
        return CDB_E_CORRUPT; /* a nibblet exists, so the index must */
    if (live == 0) {
        if ((s = rekey(&ix, page, 1, old_free, 0, 0)) != CDB_OK)
            return s;
        return cdb_pager_free(&db->pager, page);
    }
    if ((s = write_page(db, page, 0)) != CDB_OK)
        return s;
    return rekey(&ix, page, 1, old_free, 1, get16(payload(db, 0), P_FREE, o));
}

/* ------------------------------------------------------------ merging */

/* Tells a record's owner it moved. CDB_E_NOT_FOUND: the owner no longer
 * refers to it (a dropped store, say), so the record is not kept. */
static cdb_status moved(cdb_db *db, uint64_t store, unsigned kind, const uint8_t *okey, size_t oklen,
                        uint64_t addr)
{
    switch (kind) {
    case CDB_NIB_KIND_REVISION:
        return cdb_doc_nib_moved(db, store, okey, oklen, addr);
    case CDB_NIB_KIND_TS_BLOCK:
        return cdb_ts_nib_moved(db, store, okey, oklen, addr);
    case CDB_NIB_KIND_COLCHUNK:
        return cdb_table_nib_moved(db, store, okey, oklen, addr);
    default:
        return CDB_E_CORRUPT;
    }
}

/* Moves every record of `src` (buffer 1) into `dst` (buffer 0), which the
 * caller has checked has room, then frees src. */
static cdb_status move_page(cdb_db *db, cdb_index *ix, uint64_t src, unsigned src_free, uint64_t dst,
                            unsigned dst_free)
{
    cdb_byte_order o = db->pager.order;
    uint8_t *sp, *dp;
    unsigned count, i;
    cdb_status s;
    if ((s = read_page(db, dst, 0)) != CDB_OK || (s = read_page(db, src, 1)) != CDB_OK)
        return s;
    sp = payload(db, 1);
    dp = payload(db, 0);
    count = get16(sp, P_COUNT, o);
    for (i = 0; i < count; i++) {
        unsigned off, rlen, slot;
        slot_get(sp, i, o, &off, &rlen);
        if (rlen == 0)
            continue;
        {
            const uint8_t *okey, *data;
            size_t oklen, len;
            uint64_t store;
            unsigned kind;
            if ((s = record_at(db, 1, i, &store, &kind, &okey, &oklen, &data, &len)) != CDB_OK)
                return s;
            slot = place(db, dp, sp + off, rlen);
            s = moved(db, store, kind, okey, oklen, make_addr(dst, slot));
            if (s == CDB_E_NOT_FOUND)
                (void)unplace(db, dp, slot); /* its owner is gone */
            else if (s != CDB_OK)
                return s;
        }
    }
    if ((s = write_page(db, dst, 0)) != CDB_OK || (s = rekey(ix, src, 1, src_free, 0, 0)) != CDB_OK
        || (s = rekey(ix, dst, 1, dst_free, 1, get16(dp, P_FREE, o))) != CDB_OK)
        return s;
    return cdb_pager_free(&db->pager, src);
}

cdb_status cdb_nib_merge(cdb_db *db, unsigned budget)
{
    uint64_t pages[2u * CDB_NIB_MERGE_BUDGET];
    unsigned frees[2u * CDB_NIB_MERGE_BUDGET];
    size_t half = (cap_of(db) - P_DIR) / 2u, n = 0, i, j;
    cdb_index ix;
    cdb_index_cursor c;
    uint8_t k[18];
    int have, done;
    cdb_status s;

    if (db->cfg.doc_buffers == NULL)
        return CDB_OK;
    if ((s = open_index(db, &ix, 0, &have)) != CDB_OK || !have)
        return s;
    if (budget > CDB_NIB_MERGE_BUDGET)
        budget = CDB_NIB_MERGE_BUDGET;
    /* The pages under half full, fullest first. */
    if ((s = cdb_index_seek(&c, &ix, page_buf(db, 0), k, fkey(k, 0, (unsigned)half, 0))) != CDB_OK)
        return s;
    while (n < 2u * budget) {
        size_t klen, vlen;
        if ((s = cdb_index_next(&c, k, sizeof k, &klen, NULL, 0, &vlen, &done)) != CDB_OK)
            return s;
        if (done || klen != 18 || cdb_load_u64(k, CDB_ORDER_BIG) != 0)
            break;
        frees[n] = cdb_load_u16(k + 8, CDB_ORDER_BIG);
        pages[n++] = cdb_load_u64(k + 10, CDB_ORDER_BIG);
    }
    /* The emptiest pages move into the fullest ones that hold them: a page
     * of f free bytes holds (capacity - f) bytes of records and slots. */
    for (i = n; i-- > 0 && budget > 0;) {
        size_t used = (cap_of(db) - P_DIR) - frees[i];
        if (pages[i] == 0)
            continue;
        for (j = 0; j < i; j++) {
            if (pages[j] == 0 || frees[j] < used)
                continue;
            if ((s = move_page(db, &ix, pages[i], frees[i], pages[j], frees[j])) != CDB_OK)
                return s;
            frees[j] = get16(payload(db, 0), P_FREE, db->pager.order);
            pages[i] = 0;
            budget--;
            break;
        }
    }
    return CDB_OK;
}
