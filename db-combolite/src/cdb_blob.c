/* Streaming API & Content-Addressed Blob Storage (spec 8.4, 14.15).
 *
 * Overflow chains of type 0x0007, content-addressed with SHA-256 or HMAC-SHA-256. */
#include <string.h>

#include "combolite/cdb_blob.h"
#include "cdb_endian.h"
#include "cdb_overflow.h"
#include "cdb_page.h"
#include "cdb_pager.h"

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

cdb_status cdb_blob_write_begin(cdb_pager *p, uint64_t store_id, uint8_t *scratch,
                               const uint8_t *enclave_key, cdb_blob_writer *w)
{
    cdb_status s;
    if (p == NULL || scratch == NULL || w == NULL)
        return CDB_E_INVALID_ARG;

    memset(w, 0, sizeof *w);
    w->pager = p;
    w->store_id = store_id;
    w->scratch = scratch;
    w->page_room = cdb_overflow_room(p);
    if (w->page_room == 0)
        return CDB_E_INVALID_ARG;

    if (enclave_key != NULL) {
        w->is_enclave = 1;
        s = cdb_crypto_hash_init_hmac(&w->hash_ctx, enclave_key);
    } else {
        s = cdb_crypto_hash_init(&w->hash_ctx);
    }
    if (s != CDB_OK)
        return s;

    w->is_open = 1;
    return CDB_OK;
}

cdb_status cdb_blob_write_chunk(cdb_blob_writer *w, const void *data, size_t len)
{
    const uint8_t *src = (const uint8_t *)data;
    cdb_status s;

    if (w == NULL || !w->is_open)
        return CDB_E_STATE;
    if (len == 0)
        return CDB_OK;
    if (data == NULL)
        return CDB_E_INVALID_ARG;

    if ((s = cdb_crypto_hash_update(&w->hash_ctx, data, len)) != CDB_OK)
        return s;

    if (w->first_page == 0) {
        if ((s = cdb_pager_alloc(w->pager, &w->first_page)) != CDB_OK)
            return s;
        w->curr_page = w->first_page;
        w->page_count = 1;
    }

    while (len > 0) {
        size_t avail = w->page_room - w->curr_page_fill;
        if (avail == 0) {
            uint64_t next = 0;
            cdb_page_header t;
            if ((s = cdb_pager_alloc(w->pager, &next)) != CDB_OK)
                return s;

            store_lid(w->pager, w->scratch, next);
            memset(&t, 0, sizeof t);
            t.type = CDB_PAGE_TYPE_OVERFLOW;
            t.flags = CDB_PAGE_FLAG_OVERFLOW;
            t.store_id = w->store_id;

            s = cdb_pager_write(w->pager, w->curr_page, &t, w->scratch,
                                (uint32_t)(w->pager->addr_width + w->curr_page_fill));
            if (s != CDB_OK)
                return s;

            w->curr_page = next;
            w->curr_page_fill = 0;
            w->page_count++;
            avail = w->page_room;
        }

        size_t take = len < avail ? len : avail;
        memcpy(w->scratch + w->pager->addr_width + w->curr_page_fill, src, take);
        w->curr_page_fill += take;
        w->total_bytes += take;
        src += take;
        len -= take;
    }

    return CDB_OK;
}

cdb_status cdb_blob_write_end(cdb_blob_writer *w, cdb_blob_ref *out_ref)
{
    cdb_status s;

    if (w == NULL || !w->is_open || out_ref == NULL)
        return CDB_E_INVALID_ARG;

    memset(out_ref, 0, sizeof *out_ref);

    if (w->total_bytes == 0) {
        out_ref->first_page = 0;
        out_ref->page_count = 0;
        out_ref->total_len = 0;
        if (w->first_page != 0) {
            cdb_pager_free(w->pager, w->first_page);
            w->first_page = 0;
            w->curr_page = 0;
        }
    } else {
        cdb_page_header t;
        store_lid(w->pager, w->scratch, 0); /* 0 marks the end of the chain */
        memset(&t, 0, sizeof t);
        t.type = CDB_PAGE_TYPE_OVERFLOW;
        t.flags = CDB_PAGE_FLAG_OVERFLOW;
        t.store_id = w->store_id;

        s = cdb_pager_write(w->pager, w->curr_page, &t, w->scratch,
                            (uint32_t)(w->pager->addr_width + w->curr_page_fill));
        if (s != CDB_OK)
            return s;

        out_ref->first_page = w->first_page;
        out_ref->page_count = w->page_count;
        out_ref->total_len = w->total_bytes;
    }

    if (w->is_enclave)
        out_ref->flags |= 0x02u;

    s = cdb_crypto_hash_final(&w->hash_ctx, out_ref->hash);
    cdb_crypto_hash_free(&w->hash_ctx);
    w->is_open = 0;
    return s;
}

cdb_status cdb_blob_write_abort(cdb_blob_writer *w)
{
    if (w == NULL || !w->is_open)
        return CDB_OK;

    if (w->first_page != 0) {
        uint64_t lid = w->first_page;
        while (lid != 0 && lid != w->curr_page) {
            cdb_page_header h;
            uint64_t next = 0;
            cdb_status s = cdb_pager_read(w->pager, lid, w->scratch, &h);
            if (s == CDB_OK) {
                const uint8_t *pl = w->scratch + cdb_page_header_size(w->pager->fmt);
                next = load_lid(w->pager, pl);
            }
            cdb_pager_free(w->pager, lid);
            lid = next;
        }
        if (w->curr_page != 0)
            cdb_pager_free(w->pager, w->curr_page);
    }

    cdb_crypto_hash_free(&w->hash_ctx);
    w->is_open = 0;
    return CDB_OK;
}

cdb_status cdb_blob_read_begin(cdb_pager *p, uint64_t store_id, const cdb_blob_ref *ref,
                              uint8_t *page_buf, int verify_hash, const uint8_t *enclave_key,
                              cdb_blob_reader *r)
{
    cdb_status s;
    if (p == NULL || ref == NULL || page_buf == NULL || r == NULL)
        return CDB_E_INVALID_ARG;

    memset(r, 0, sizeof *r);
    r->pager = p;
    r->store_id = store_id;
    r->ref = *ref;
    r->page_buf = page_buf;
    r->verify_hash = verify_hash;
    r->bytes_remaining = ref->total_len;
    r->curr_page = ref->first_page;

    if (verify_hash) {
        if (enclave_key != NULL)
            s = cdb_crypto_hash_init_hmac(&r->hash_ctx, enclave_key);
        else
            s = cdb_crypto_hash_init(&r->hash_ctx);
        if (s != CDB_OK)
            return s;
    }

    if (ref->total_len == 0) {
        if (verify_hash) {
            uint8_t h[CDB_CRYPTO_HASH_SIZE];
            r->verify_hash = 0;
            if ((s = cdb_crypto_hash_final(&r->hash_ctx, h)) != CDB_OK)
                return s;
            if (memcmp(h, ref->hash, CDB_CRYPTO_HASH_SIZE) != 0)
                return CDB_E_CORRUPT;
        }
        r->is_open = 1;
        return CDB_OK;
    }

    if (ref->first_page == 0)
        return CDB_E_CORRUPT;

    /* Read first page of the chain */
    cdb_page_header h;
    s = cdb_pager_read(p, r->curr_page, page_buf, &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    if (h.type != CDB_PAGE_TYPE_OVERFLOW || !(h.flags & CDB_PAGE_FLAG_OVERFLOW) || h.store_id != store_id)
        return CDB_E_CORRUPT;
    if (h.payload_len < p->addr_width)
        return CDB_E_CORRUPT;

    const uint8_t *pl = page_buf + cdb_page_header_size(p->fmt);
    r->next_page = load_lid(p, pl);
    r->curr_page_bytes = h.payload_len - p->addr_width;
    r->curr_offset_in_page = 0;
    r->is_open = 1;
    return CDB_OK;
}

cdb_status cdb_blob_read_chunk(cdb_blob_reader *r, void *dest, size_t cap, size_t *out_len)
{
    uint8_t *dst = (uint8_t *)dest;
    cdb_status s;

    if (r == NULL || !r->is_open || out_len == NULL)
        return CDB_E_INVALID_ARG;
    if (cap > 0 && dest == NULL)
        return CDB_E_INVALID_ARG;

    *out_len = 0;
    if (r->bytes_remaining == 0 || cap == 0) {
        if (r->bytes_remaining == 0 && r->verify_hash) {
            uint8_t h[CDB_CRYPTO_HASH_SIZE];
            r->verify_hash = 0;
            if ((s = cdb_crypto_hash_final(&r->hash_ctx, h)) != CDB_OK)
                return s;
            if (memcmp(h, r->ref.hash, CDB_CRYPTO_HASH_SIZE) != 0)
                return CDB_E_CORRUPT;
        }
        return CDB_OK;
    }

    while (cap > 0 && r->bytes_remaining > 0) {
        if (r->curr_offset_in_page == r->curr_page_bytes) {
            if (r->next_page == 0)
                return CDB_E_CORRUPT; /* truncated chain */

            r->curr_page = r->next_page;
            cdb_page_header h;
            s = cdb_pager_read(r->pager, r->curr_page, r->page_buf, &h);
            if (s != CDB_OK)
                return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
            if (h.type != CDB_PAGE_TYPE_OVERFLOW || !(h.flags & CDB_PAGE_FLAG_OVERFLOW) || h.store_id != r->store_id)
                return CDB_E_CORRUPT;
            if (h.payload_len < r->pager->addr_width)
                return CDB_E_CORRUPT;

            const uint8_t *pl = r->page_buf + cdb_page_header_size(r->pager->fmt);
            r->next_page = load_lid(r->pager, pl);
            r->curr_page_bytes = h.payload_len - r->pager->addr_width;
            r->curr_offset_in_page = 0;
        }

        size_t avail = r->curr_page_bytes - r->curr_offset_in_page;
        size_t take = cap < avail ? cap : avail;
        if ((uint64_t)take > r->bytes_remaining)
            take = (size_t)r->bytes_remaining;

        const uint8_t *pl = r->page_buf + cdb_page_header_size(r->pager->fmt) + r->pager->addr_width;
        memcpy(dst, pl + r->curr_offset_in_page, take);

        if (r->verify_hash) {
            if ((s = cdb_crypto_hash_update(&r->hash_ctx, dst, take)) != CDB_OK)
                return s;
        }

        r->curr_offset_in_page += take;
        r->bytes_remaining -= take;
        *out_len += take;
        dst += take;
        cap -= take;
    }

    if (r->bytes_remaining == 0 && r->verify_hash) {
        uint8_t h[CDB_CRYPTO_HASH_SIZE];
        r->verify_hash = 0;
        if ((s = cdb_crypto_hash_final(&r->hash_ctx, h)) != CDB_OK)
            return s;
        if (memcmp(h, r->ref.hash, CDB_CRYPTO_HASH_SIZE) != 0)
            return CDB_E_CORRUPT;
    }

    return CDB_OK;
}

cdb_status cdb_blob_read_end(cdb_blob_reader *r)
{
    if (r == NULL || !r->is_open)
        return CDB_OK;

    cdb_crypto_hash_free(&r->hash_ctx);
    r->is_open = 0;
    return CDB_OK;
}

cdb_status cdb_blob_free(cdb_pager *p, uint64_t store_id, const cdb_blob_ref *ref, uint8_t *page_buf)
{
    if (p == NULL || ref == NULL || page_buf == NULL)
        return CDB_E_INVALID_ARG;
    if (ref->first_page == 0 || ref->total_len == 0)
        return CDB_OK;

    return cdb_overflow_free(p, ref->first_page, store_id, (size_t)ref->total_len, page_buf);
}
