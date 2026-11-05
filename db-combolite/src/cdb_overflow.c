/* Overflow chains (spec 14.15).
 *
 * Page payload: [next page's logical ID, address width][value bytes].
 * The chain carries no length of its own: the owner stores the total, and
 * since every page but the last is full, the size of each part follows from
 * it. Readers use that to check every page they visit.
 *
 * Spec 16.5 (Kind 4): The last partly filled page of an overflow chain
 * is packed into a shared nibblet when it fits (<= page_size / 4). */
#include <string.h>

#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_nib.h"
#include "cdb_overflow.h"
#include "cdb_table.h"

/* The next-page link, at the file's address width and byte order. */
static uint64_t load_lid(const cdb_pager *p, const uint8_t *b)
{
    return p->addr_width == 8 ? cdb_load_u64(b, p->order) : cdb_load_u32(b, p->order);
}

static void store_lid(const cdb_pager *p, uint8_t *b, uint64_t lid)
{
    if (p->addr_width == 8)
        cdb_store_u64(b, lid, p->order);
    else
        cdb_store_u32(b, (uint32_t)lid, p->order);
}

size_t cdb_overflow_room(const cdb_pager *p)
{
    return cdb_page_capacity(p->fmt, p->page_size) - p->addr_width;
}

uint64_t cdb_overflow_pages(const cdb_pager *p, size_t len)
{
    size_t room = cdb_overflow_room(p);
    return ((uint64_t)len + room - 1u) / room;
}

cdb_status cdb_overflow_write_db(cdb_db *db, cdb_pager *p, uint64_t store, const void *data, size_t len,
                                 uint8_t *scratch, const uint8_t *okey, size_t oklen, uint64_t *first)
{
    const uint8_t *src = (const uint8_t *)data;
    size_t room;
    uint64_t lid, next = 0;
    cdb_page_header t;
    cdb_status s;
    size_t tail;
    int tail_is_nibblet = 0;

    if (p == NULL && db != NULL)
        p = &db->pager;
    if (len == 0 || data == NULL || p == NULL || first == NULL)
        return CDB_E_INVALID_ARG;

    room = cdb_overflow_room(p);
    tail = len % room;
    if (db != NULL && p->addr_width == 8 && tail > 0 && cdb_nib_fits(db, tail))
        tail_is_nibblet = 1;

    /* If the entire value fits in a tail nibblet */
    if (len < room && tail_is_nibblet)
        return cdb_nib_put(db, store, CDB_NIB_KIND_OVERFLOW_TAIL, okey, oklen, data, len, first);

    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_OVERFLOW;
    t.flags = CDB_PAGE_FLAG_OVERFLOW;
    t.store_id = store;
    if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
        return s;
    *first = lid;

    while (len > 0) {
        size_t part = len < room ? len : room;
        next = 0;
        if (len > part) {
            size_t rem = len - part;
            if (tail_is_nibblet && rem == tail) {
                uint8_t tokey[9];
                uint64_t tail_addr = 0;
                tokey[0] = CDB_OVERFLOW_OWNER_PREV_PAGE;
                cdb_store_u64(tokey + 1, lid, CDB_ORDER_BIG);
                s = cdb_nib_put(db, store, CDB_NIB_KIND_OVERFLOW_TAIL, tokey, sizeof tokey, src + part, tail, &tail_addr);
                if (s != CDB_OK)
                    return s;
                next = tail_addr;
            } else if ((s = cdb_pager_alloc(p, &next)) != CDB_OK) {
                return s;
            }
        }
        store_lid(p, scratch, next);
        memcpy(scratch + p->addr_width, src, part);
        if ((s = cdb_pager_write(p, lid, &t, scratch, (uint32_t)(p->addr_width + part))) != CDB_OK)
            return s;
        src += part;
        len -= part;
        if (tail_is_nibblet && len == tail)
            break;
        lid = next;
    }
    return CDB_OK;
}

cdb_status cdb_overflow_write(cdb_pager *p, uint64_t store, const void *data, size_t len, uint8_t *scratch,
                              uint64_t *first)
{
    return cdb_overflow_write_db(NULL, p, store, data, len, scratch, NULL, 0, first);
}

/* Reads one page of a chain and checks it is what the chain needs next.
 * part is the number of value bytes this page must hold; last says whether
 * it must end the chain (next ID 0) or continue it (next ID not 0). Any
 * mismatch in type, flag, store, length or link is CDB_E_CORRUPT. */
static cdb_status chain_page(cdb_pager *p, uint64_t lid, uint64_t store, size_t part, int last, uint8_t *page,
                             uint64_t *next, const uint8_t **body)
{
    cdb_page_header h;
    const uint8_t *pl;
    cdb_status s = cdb_pager_read(p, lid, page, &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    if (h.type != CDB_PAGE_TYPE_OVERFLOW || !(h.flags & CDB_PAGE_FLAG_OVERFLOW) || h.store_id != store
        || h.payload_len != p->addr_width + part)
        return CDB_E_CORRUPT;
    pl = page + cdb_page_payload_offset(p->fmt);
    *next = load_lid(p, pl);
    if (last ? *next != 0 : *next == 0)
        return CDB_E_CORRUPT;
    *body = pl + p->addr_width;
    return CDB_OK;
}

cdb_status cdb_overflow_read_db(cdb_db *db, cdb_pager *p, uint64_t first, uint64_t store, void *out, size_t len,
                                uint8_t *page)
{
    uint8_t *dst = (uint8_t *)out;
    size_t room;
    uint64_t lid = first;
    cdb_status s;

    if (p == NULL && db != NULL)
        p = &db->pager;
    if (len == 0 || first == 0 || p == NULL || out == NULL)
        return CDB_E_CORRUPT;
    room = cdb_overflow_room(p);

    while (len > 0) {
        if (cdb_nib_is(lid)) {
            size_t actual_len = 0;
            if (db == NULL)
                return CDB_E_CORRUPT;
            s = cdb_nib_get(db, lid, dst, len, &actual_len);
            if (s != CDB_OK)
                return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
            if (actual_len != len)
                return CDB_E_CORRUPT;
            return CDB_OK;
        }
        {
            size_t part = len < room ? len : room;
            const uint8_t *body;
            uint64_t next;
            if ((s = chain_page(p, lid, store, part, len == part, page, &next, &body)) != CDB_OK)
                return s;
            memcpy(dst, body, part);
            dst += part;
            len -= part;
            lid = next;
        }
    }
    return CDB_OK;
}

cdb_status cdb_overflow_read(cdb_pager *p, uint64_t first, uint64_t store, void *out, size_t len, uint8_t *page)
{
    return cdb_overflow_read_db(NULL, p, first, store, out, len, page);
}

cdb_status cdb_overflow_free_db(cdb_db *db, cdb_pager *p, uint64_t first, uint64_t store, size_t len,
                                uint8_t *page)
{
    size_t room;
    uint64_t lid = first;
    cdb_status s;

    if (p == NULL && db != NULL)
        p = &db->pager;
    if (len == 0 || first == 0 || p == NULL)
        return CDB_E_CORRUPT;
    room = cdb_overflow_room(p);

    while (len > 0) {
        if (cdb_nib_is(lid)) {
            if (db == NULL)
                return CDB_E_CORRUPT;
            return cdb_nib_free(db, lid);
        }
        {
            size_t part = len < room ? len : room;
            const uint8_t *body;
            uint64_t next;
            if ((s = chain_page(p, lid, store, part, len == part, page, &next, &body)) != CDB_OK)
                return s;
            if ((s = cdb_pager_free(p, lid)) != CDB_OK)
                return s;
            len -= part;
            lid = next;
        }
    }
    return CDB_OK;
}

cdb_status cdb_overflow_free(cdb_pager *p, uint64_t first, uint64_t store, size_t len, uint8_t *page)
{
    return cdb_overflow_free_db(NULL, p, first, store, len, page);
}

cdb_status cdb_overflow_nib_moved(cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen,
                                  uint64_t old_addr, uint64_t addr)
{
    if (db == NULL || okey == NULL || oklen < 1)
        return CDB_E_CORRUPT;

    switch (okey[0]) {
    case CDB_OVERFLOW_OWNER_PREV_PAGE: {
        uint64_t prev_lid;
        cdb_page_header h;
        uint8_t *pbuf = db->pager.page2;
        uint8_t *pl;
        cdb_status s;
        if (oklen != 9)
            return CDB_E_CORRUPT;
        prev_lid = cdb_load_u64(okey + 1, CDB_ORDER_BIG);
        if ((s = cdb_pager_read(&db->pager, prev_lid, pbuf, &h)) != CDB_OK)
            return s == CDB_E_NOT_FOUND ? CDB_E_NOT_FOUND : s;
        if (h.type != CDB_PAGE_TYPE_OVERFLOW || !(h.flags & CDB_PAGE_FLAG_OVERFLOW) || h.store_id != store)
            return CDB_E_NOT_FOUND;
        pl = pbuf + cdb_page_payload_offset(db->pager.fmt);
        if (h.payload_len < db->pager.addr_width)
            return CDB_E_CORRUPT;
        if (load_lid(&db->pager, pl) != old_addr)
            return CDB_E_NOT_FOUND;
        store_lid(&db->pager, pl, addr);
        return cdb_pager_write(&db->pager, prev_lid, &h, pl, h.payload_len);
    }
    case CDB_OVERFLOW_OWNER_DOC_REV:
        return cdb_doc_overflow_tail_moved(db, store, okey + 1, oklen - 1, old_addr, addr);
    case CDB_OVERFLOW_OWNER_COL_CHUNK:
        return cdb_table_nib_moved(db, store, okey + 1, oklen - 1, old_addr, addr);
    default:
        return CDB_E_CORRUPT;
    }
}
