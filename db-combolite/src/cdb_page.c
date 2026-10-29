/* Common page header and trailer (spec 13, 14.2). */
#include <string.h>

#include "cdb_crc32c.h"
#include "cdb_endian.h"
#include "cdb_page.h"

#define MARKER_0 0xC0u
#define MARKER_1 0xDBu
#define COMPACT_MAX_PAGE 1024u

size_t cdb_page_header_size(cdb_page_format fmt)
{
    switch (fmt) {
    case CDB_PAGE_FMT_FULL64: return 56;
    case CDB_PAGE_FMT_FULL32: return 40;
    case CDB_PAGE_FMT_COMPACT: return 28;
    }
    return 0;
}

/* Compact pages that carry only a checksum use an 8-byte trailer (spec 13.4). */
size_t cdb_page_trailer_size(cdb_page_format fmt)
{
    return fmt == CDB_PAGE_FMT_COMPACT ? 8 : 16;
}

size_t cdb_page_capacity(cdb_page_format fmt, size_t page_size)
{
    size_t overhead = cdb_page_header_size(fmt) + cdb_page_trailer_size(fmt);
    if (overhead == 0 || page_size <= overhead)
        return 0;
    if (fmt == CDB_PAGE_FMT_COMPACT && page_size > COMPACT_MAX_PAGE)
        return 0;
    return page_size - overhead;
}

static int fits(uint64_t v, unsigned bytes)
{
    return bytes >= 8 || v < (UINT64_C(1) << (bytes * 8u));
}

static cdb_status check_args(size_t page_size, cdb_page_format fmt, cdb_byte_order order,
                             unsigned checksum_alg)
{
    if (cdb_page_capacity(fmt, page_size) == 0)
        return CDB_E_INVALID_ARG;
    if (order != CDB_ORDER_LITTLE && order != CDB_ORDER_BIG)
        return CDB_E_INVALID_ARG;
    if (checksum_alg != CDB_CHECKSUM_CRC32C)
        return CDB_E_UNSUPPORTED;
    return CDB_OK;
}

static void store_id(uint8_t *p, uint64_t v, unsigned width, cdb_byte_order o)
{
    if (width == 8)
        cdb_store_u64(p, v, o);
    else if (width == 4)
        cdb_store_u32(p, (uint32_t)v, o);
    else
        cdb_store_u16(p, (uint16_t)v, o);
}

static uint64_t load_id(const uint8_t *p, unsigned width, cdb_byte_order o)
{
    if (width == 8)
        return cdb_load_u64(p, o);
    if (width == 4)
        return cdb_load_u32(p, o);
    return cdb_load_u16(p, o);
}

cdb_status cdb_page_seal(uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, const cdb_page_header *h)
{
    size_t hsize = cdb_page_header_size(fmt);
    size_t tsize = cdb_page_trailer_size(fmt);
    cdb_status s = check_args(page_size, fmt, order, checksum_alg);
    uint8_t *trailer;

    if (s != CDB_OK)
        return s;
    if (h->payload_len > cdb_page_capacity(fmt, page_size))
        return CDB_E_OVERFLOW;

    memset(page, 0, hsize);
    page[0] = MARKER_0;
    page[1] = MARKER_1;
    cdb_store_u16(page + 2, h->type, order);
    page[4] = h->layout_version;
    page[5] = h->flags;
    page[6] = h->compression;

    if (fmt == CDB_PAGE_FMT_COMPACT) {
        /* 14.2.3: narrow fields, but the write sequence keeps all 8 bytes. */
        if (h->extent_pos > UINT8_MAX || !fits(h->key_version, 2) || !fits(h->payload_len, 2)
            || !fits(h->logical_id, 2) || !fits(h->store_id, 2) || !fits(h->domain_id, 2))
            return CDB_E_OVERFLOW;
        page[7] = (uint8_t)h->extent_pos;
        cdb_store_u64(page + 8, h->write_seq, order);
        cdb_store_u16(page + 16, (uint16_t)h->key_version, order);
        cdb_store_u16(page + 18, (uint16_t)h->payload_len, order);
        store_id(page + 20, h->logical_id, 2, order);
        store_id(page + 22, h->store_id, 2, order);
        store_id(page + 24, h->domain_id, 2, order);
    } else {
        unsigned w = fmt == CDB_PAGE_FMT_FULL64 ? 8u : 4u;
        size_t ids = 24;
        if (!fits(h->logical_id, w) || !fits(h->store_id, w) || !fits(h->domain_id, w))
            return CDB_E_OVERFLOW;
        cdb_store_u32(page + 8, h->key_version, order);
        cdb_store_u32(page + 12, h->payload_len, order);
        cdb_store_u64(page + 16, h->write_seq, order);
        store_id(page + ids, h->logical_id, w, order);
        store_id(page + ids + w, h->store_id, w, order);
        store_id(page + ids + 2 * w, h->domain_id, w, order);
        cdb_store_u16(page + ids + 3 * w, h->extent_pos, order);
    }

    trailer = page + page_size - tsize;
    memset(trailer, 0, tsize);
    cdb_store_u32(trailer, cdb_crc32c(page, page_size - tsize), order);
    return CDB_OK;
}

cdb_status cdb_page_open(const uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, cdb_page_header *h)
{
    size_t hsize = cdb_page_header_size(fmt);
    size_t tsize = cdb_page_trailer_size(fmt);
    cdb_status s = check_args(page_size, fmt, order, checksum_alg);
    const uint8_t *trailer;

    if (s != CDB_OK)
        return s;
    trailer = page + page_size - tsize;
    if (page[0] != MARKER_0 || page[1] != MARKER_1)
        return CDB_E_CORRUPT;
    if (cdb_load_u32(trailer, order) != cdb_crc32c(page, page_size - tsize))
        return CDB_E_CORRUPT;
    {
        size_t i;
        for (i = 4; i < tsize; i++) /* checksum padding must be zero */
            if (trailer[i])
                return CDB_E_MALFORMED;
    }

    memset(h, 0, sizeof *h);
    h->type = cdb_load_u16(page + 2, order);
    h->layout_version = page[4];
    h->flags = page[5];
    h->compression = page[6];
    if (fmt == CDB_PAGE_FMT_COMPACT) {
        h->extent_pos = page[7];
        h->write_seq = cdb_load_u64(page + 8, order);
        h->key_version = cdb_load_u16(page + 16, order);
        h->payload_len = cdb_load_u16(page + 18, order);
        h->logical_id = load_id(page + 20, 2, order);
        h->store_id = load_id(page + 22, 2, order);
        h->domain_id = load_id(page + 24, 2, order);
        if (page[26] || page[27])
            return CDB_E_MALFORMED;
    } else {
        unsigned w = fmt == CDB_PAGE_FMT_FULL64 ? 8u : 4u;
        size_t ids = 24, i;
        if (page[7])
            return CDB_E_MALFORMED;
        h->key_version = cdb_load_u32(page + 8, order);
        h->payload_len = cdb_load_u32(page + 12, order);
        h->write_seq = cdb_load_u64(page + 16, order);
        h->logical_id = load_id(page + ids, w, order);
        h->store_id = load_id(page + ids + w, w, order);
        h->domain_id = load_id(page + ids + 2 * w, w, order);
        h->extent_pos = cdb_load_u16(page + ids + 3 * w, order);
        for (i = ids + 3 * w + 2; i < hsize; i++)
            if (page[i])
                return CDB_E_MALFORMED;
    }
    if (h->payload_len > cdb_page_capacity(fmt, page_size))
        return CDB_E_MALFORMED;
    return CDB_OK;
}
