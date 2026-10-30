/* Overflow chains (spec 14.15). */
#include <string.h>

#include "cdb_endian.h"
#include "cdb_overflow.h"

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

cdb_status cdb_overflow_write(cdb_pager *p, uint64_t store, const void *data, size_t len, uint8_t *scratch,
                              uint64_t *first)
{
    const uint8_t *src = (const uint8_t *)data;
    size_t room = cdb_overflow_room(p);
    uint64_t lid, next = 0;
    cdb_page_header t;
    cdb_status s;

    if (len == 0 || data == NULL)
        return CDB_E_INVALID_ARG;
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
        if (len > part && (s = cdb_pager_alloc(p, &next)) != CDB_OK)
            return s;
        store_lid(p, scratch, next);
        memcpy(scratch + p->addr_width, src, part);
        if ((s = cdb_pager_write(p, lid, &t, scratch, p->addr_width + part)) != CDB_OK)
            return s;
        src += part;
        len -= part;
        lid = next;
    }
    return CDB_OK;
}

/* Reads one page of a chain and checks it is what the chain needs next. */
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
    pl = page + cdb_page_header_size(p->fmt);
    *next = load_lid(p, pl);
    if (last ? *next != 0 : *next == 0)
        return CDB_E_CORRUPT;
    *body = pl + p->addr_width;
    return CDB_OK;
}

cdb_status cdb_overflow_read(cdb_pager *p, uint64_t first, uint64_t store, void *out, size_t len, uint8_t *page)
{
    uint8_t *dst = (uint8_t *)out;
    size_t room = cdb_overflow_room(p);
    uint64_t lid = first;
    cdb_status s;
    if (len == 0 || first == 0)
        return CDB_E_CORRUPT;
    while (len > 0) {
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
    return CDB_OK;
}

cdb_status cdb_overflow_free(cdb_pager *p, uint64_t first, uint64_t store, size_t len, uint8_t *page)
{
    size_t room = cdb_overflow_room(p);
    uint64_t lid = first;
    cdb_status s;
    if (len == 0 || first == 0)
        return CDB_E_CORRUPT;
    while (len > 0) {
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
    return CDB_OK;
}
