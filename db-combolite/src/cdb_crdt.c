#include <string.h>

#include "combolite/cdb_crdt.h"
#include "cdb_crdt.h"
#include "cdb_endian.h"

/* ------------------------------------------------------------ HLC */

int cdb_hlc_compare(cdb_hlc a, cdb_hlc b)
{
    if (a.physical < b.physical)
        return -1;
    if (a.physical > b.physical)
        return 1;
    if (a.logical < b.logical)
        return -1;
    if (a.logical > b.logical)
        return 1;
    if (a.peer_id < b.peer_id)
        return -1;
    if (a.peer_id > b.peer_id)
        return 1;
    return 0;
}

void cdb_hlc_tick(cdb_hlc *clock, uint64_t wall_clock_now)
{
    if (clock == NULL)
        return;
    if (wall_clock_now > clock->physical) {
        clock->physical = wall_clock_now;
        clock->logical = 0;
    } else {
        clock->logical++;
    }
}

void cdb_hlc_update(cdb_hlc *clock, cdb_hlc remote, uint64_t wall_clock_now)
{
    uint64_t pt;
    if (clock == NULL)
        return;
    pt = clock->physical;
    if (remote.physical > pt)
        pt = remote.physical;
    if (wall_clock_now > pt)
        pt = wall_clock_now;

    if (pt == clock->physical && pt == remote.physical) {
        uint32_t max_log = clock->logical > remote.logical ? clock->logical : remote.logical;
        clock->logical = max_log + 1;
    } else if (pt == clock->physical) {
        clock->logical++;
    } else if (pt == remote.physical) {
        clock->physical = pt;
        clock->logical = remote.logical + 1;
    } else {
        clock->physical = pt;
        clock->logical = 0;
    }
}

/* ------------------------------------------------------------ Version Vector */

void cdb_crdt_vector_set(cdb_crdt_vector *v, uint32_t peer_id, uint64_t counter, cdb_hlc hlc)
{
    uint32_t i;
    if (v == NULL)
        return;
    for (i = 0; i < v->count; i++) {
        if (v->peers[i].peer_id == peer_id) {
            v->peers[i].counter = counter;
            v->peers[i].hlc = hlc;
            return;
        }
    }
    if (v->count < CDB_CRDT_MAX_PEERS) {
        v->peers[v->count].peer_id = peer_id;
        v->peers[v->count].counter = counter;
        v->peers[v->count].hlc = hlc;
        v->count++;
    }
}

cdb_status cdb_crdt_vector_get(const cdb_crdt_vector *v, uint32_t peer_id, cdb_crdt_peer_entry *out)
{
    uint32_t i;
    if (v == NULL || out == NULL)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < v->count; i++) {
        if (v->peers[i].peer_id == peer_id) {
            *out = v->peers[i];
            return CDB_OK;
        }
    }
    return CDB_E_NOT_FOUND;
}

int cdb_crdt_vector_compare(const cdb_crdt_vector *a, const cdb_crdt_vector *b)
{
    int a_has_greater = 0;
    int b_has_greater = 0;
    uint32_t i, j;

    if (a == NULL || b == NULL)
        return CDB_CRDT_CMP_EQUAL;

    /* Check entries in a vs b */
    for (i = 0; i < a->count; i++) {
        uint32_t pid = a->peers[i].peer_id;
        int found = 0;
        for (j = 0; j < b->count; j++) {
            if (b->peers[j].peer_id == pid) {
                found = 1;
                if (a->peers[i].counter > b->peers[j].counter)
                    a_has_greater = 1;
                else if (b->peers[j].counter > a->peers[i].counter)
                    b_has_greater = 1;
                break;
            }
        }
        if (!found && a->peers[i].counter > 0)
            a_has_greater = 1;
    }

    /* Check entries in b that might not be in a */
    for (j = 0; j < b->count; j++) {
        uint32_t pid = b->peers[j].peer_id;
        int found = 0;
        for (i = 0; i < a->count; i++) {
            if (a->peers[i].peer_id == pid) {
                found = 1;
                break;
            }
        }
        if (!found && b->peers[j].counter > 0)
            b_has_greater = 1;
    }

    if (a_has_greater && b_has_greater)
        return CDB_CRDT_CMP_CONCURRENT;
    if (a_has_greater)
        return CDB_CRDT_CMP_DOMINATES;
    if (b_has_greater)
        return CDB_CRDT_CMP_DOMINATED;
    return CDB_CRDT_CMP_EQUAL;
}

void cdb_crdt_vector_merge(cdb_crdt_vector *dest, const cdb_crdt_vector *src)
{
    uint32_t i, j;
    if (dest == NULL || src == NULL)
        return;
    for (i = 0; i < src->count; i++) {
        int found = 0;
        for (j = 0; j < dest->count; j++) {
            if (dest->peers[j].peer_id == src->peers[i].peer_id) {
                found = 1;
                if (src->peers[i].counter > dest->peers[j].counter)
                    dest->peers[j].counter = src->peers[i].counter;
                if (cdb_hlc_compare(src->peers[i].hlc, dest->peers[j].hlc) > 0)
                    dest->peers[j].hlc = src->peers[i].hlc;
                break;
            }
        }
        if (!found && dest->count < CDB_CRDT_MAX_PEERS) {
            dest->peers[dest->count++] = src->peers[i];
        }
    }
}

/* ------------------------------------------------------------ PN-Counter */

void cdb_pn_counter_init(cdb_pn_counter *c)
{
    if (c != NULL)
        memset(c, 0, sizeof *c);
}

void cdb_pn_counter_inc(cdb_pn_counter *c, uint32_t peer_idx, uint64_t amount)
{
    if (c != NULL && peer_idx < CDB_CRDT_MAX_PEERS)
        c->p[peer_idx] += amount;
}

void cdb_pn_counter_dec(cdb_pn_counter *c, uint32_t peer_idx, uint64_t amount)
{
    if (c != NULL && peer_idx < CDB_CRDT_MAX_PEERS)
        c->n[peer_idx] += amount;
}

int64_t cdb_pn_counter_val(const cdb_pn_counter *c)
{
    uint64_t p_sum = 0, n_sum = 0;
    uint32_t i;
    if (c == NULL)
        return 0;
    for (i = 0; i < CDB_CRDT_MAX_PEERS; i++) {
        p_sum += c->p[i];
        n_sum += c->n[i];
    }
    return (int64_t)p_sum - (int64_t)n_sum;
}

void cdb_pn_counter_merge(cdb_pn_counter *dest, const cdb_pn_counter *a, const cdb_pn_counter *b)
{
    uint32_t i;
    if (dest == NULL || a == NULL || b == NULL)
        return;
    for (i = 0; i < CDB_CRDT_MAX_PEERS; i++) {
        dest->p[i] = a->p[i] > b->p[i] ? a->p[i] : b->p[i];
        dest->n[i] = a->n[i] > b->n[i] ? a->n[i] : b->n[i];
    }
}

/* ------------------------------------------------------------ LWW Merge */

cdb_status cdb_crdt_lww_merge(const cdb_crdt_field *fields_a, size_t count_a,
                              const cdb_crdt_field *fields_b, size_t count_b,
                              cdb_crdt_field *out_fields, size_t cap_out, size_t *count_out)
{
    size_t i, j, n = 0;

    if (count_out == NULL)
        return CDB_E_INVALID_ARG;
    *count_out = 0;

    /* First pass: add winning entries from fields_a */
    for (i = 0; i < count_a; i++) {
        uint64_t fid = fields_a[i].field_id;
        const cdb_crdt_field *winner = &fields_a[i];
        for (j = 0; j < count_b; j++) {
            if (fields_b[j].field_id == fid) {
                if (cdb_hlc_compare(fields_b[j].hlc, fields_a[i].hlc) > 0)
                    winner = &fields_b[j];
                break;
            }
        }
        if (n >= cap_out)
            return CDB_E_BUFFER_TOO_SMALL;
        out_fields[n++] = *winner;
    }

    /* Second pass: add entries from fields_b that were not in fields_a */
    for (j = 0; j < count_b; j++) {
        uint64_t fid = fields_b[j].field_id;
        int seen_in_a = 0;
        for (i = 0; i < count_a; i++) {
            if (fields_a[i].field_id == fid) {
                seen_in_a = 1;
                break;
            }
        }
        if (!seen_in_a) {
            if (n >= cap_out)
                return CDB_E_BUFFER_TOO_SMALL;
            out_fields[n++] = fields_b[j];
        }
    }

    *count_out = n;
    return CDB_OK;
}

/* ------------------------------------------------------------ Graph Links */

int cdb_crdt_link_resolve(cdb_crdt_link_token a, cdb_crdt_link_token b)
{
    if (!a.tombstoned && !b.tombstoned)
        return 1;
    if (a.tombstoned && b.tombstoned)
        return 0;

    /* One is alive and one is tombstoned. Add-Wins semantics:
     * If the addition HLC is greater than or equal to the deletion HLC, add wins.
     * Otherwise removal observed the addition and removal wins. */
    if (!a.tombstoned && b.tombstoned)
        return cdb_hlc_compare(a.hlc, b.hlc) >= 0 ? 1 : 0;
    else
        return cdb_hlc_compare(b.hlc, a.hlc) >= 0 ? 1 : 0;
}

/* ------------------------------------------------------------ Time Series */

int cdb_crdt_ts_same_point(cdb_crdt_ts_point a, cdb_crdt_ts_point b)
{
    return a.series_id == b.series_id && a.ts == b.ts && a.peer_id == b.peer_id;
}

/* ------------------------------------------------------------ Wire Protocol */

cdb_status cdb_crdt_handshake_encode(uint32_t peer_id, const cdb_crdt_vector *v,
                                     uint8_t *buf, size_t cap, size_t *len)
{
    size_t needed, off;
    uint32_t i;

    if (v == NULL || buf == NULL || len == NULL)
        return CDB_E_INVALID_ARG;

    needed = 4u + 4u + 2u + (size_t)v->count * (4u + 8u + 8u + 4u + 4u);
    *len = needed;
    if (cap < needed)
        return CDB_E_BUFFER_TOO_SMALL;

    cdb_store_u32(buf, CDB_CRDT_WIRE_MAGIC, CDB_ORDER_BIG);
    cdb_store_u32(buf + 4, peer_id, CDB_ORDER_BIG);
    cdb_store_u16(buf + 8, (uint16_t)v->count, CDB_ORDER_BIG);
    off = 10u;

    for (i = 0; i < v->count; i++) {
        cdb_store_u32(buf + off, v->peers[i].peer_id, CDB_ORDER_BIG);
        cdb_store_u64(buf + off + 4, v->peers[i].counter, CDB_ORDER_BIG);
        cdb_store_u64(buf + off + 12, v->peers[i].hlc.physical, CDB_ORDER_BIG);
        cdb_store_u32(buf + off + 20, v->peers[i].hlc.logical, CDB_ORDER_BIG);
        cdb_store_u32(buf + off + 24, v->peers[i].hlc.peer_id, CDB_ORDER_BIG);
        off += 28u;
    }

    return CDB_OK;
}

cdb_status cdb_crdt_handshake_decode(const uint8_t *buf, size_t len,
                                     uint32_t *peer_id, cdb_crdt_vector *v)
{
    uint32_t magic;
    uint16_t count;
    size_t off, i;

    if (buf == NULL || peer_id == NULL || v == NULL)
        return CDB_E_INVALID_ARG;
    if (len < 10u)
        return CDB_E_TRUNCATED;

    magic = cdb_load_u32(buf, CDB_ORDER_BIG);
    if (magic != CDB_CRDT_WIRE_MAGIC)
        return CDB_E_MALFORMED;

    *peer_id = cdb_load_u32(buf + 4, CDB_ORDER_BIG);
    count = cdb_load_u16(buf + 8, CDB_ORDER_BIG);
    if (count > CDB_CRDT_MAX_PEERS)
        return CDB_E_OVERFLOW;
    if (len < 10u + (size_t)count * 28u)
        return CDB_E_TRUNCATED;

    memset(v, 0, sizeof *v);
    v->count = count;
    off = 10u;

    for (i = 0; i < count; i++) {
        v->peers[i].peer_id = cdb_load_u32(buf + off, CDB_ORDER_BIG);
        v->peers[i].counter = cdb_load_u64(buf + off + 4, CDB_ORDER_BIG);
        v->peers[i].hlc.physical = cdb_load_u64(buf + off + 12, CDB_ORDER_BIG);
        v->peers[i].hlc.logical = cdb_load_u32(buf + off + 20, CDB_ORDER_BIG);
        v->peers[i].hlc.peer_id = cdb_load_u32(buf + off + 24, CDB_ORDER_BIG);
        off += 28u;
    }

    return CDB_OK;
}

cdb_status cdb_crdt_slice_encode(uint32_t sender_peer_id, const cdb_crdt_delta_rec *recs, size_t count,
                                 uint8_t *buf, size_t cap, size_t *len)
{
    size_t needed = 4u + 4u + 4u;
    size_t off, i;

    if (len == NULL)
        return CDB_E_INVALID_ARG;
    if (count > 0 && recs == NULL)
        return CDB_E_INVALID_ARG;

    for (i = 0; i < count; i++) {
        needed += 8u + 8u + 1u + 8u + 4u + 4u + 4u + recs[i].len;
    }
    *len = needed;
    if (buf == NULL || cap < needed)
        return CDB_E_BUFFER_TOO_SMALL;

    cdb_store_u32(buf, CDB_CRDT_WIRE_MAGIC, CDB_ORDER_BIG);
    cdb_store_u32(buf + 4, sender_peer_id, CDB_ORDER_BIG);
    cdb_store_u32(buf + 8, (uint32_t)count, CDB_ORDER_BIG);
    off = 12u;

    for (i = 0; i < count; i++) {
        cdb_store_u64(buf + off, recs[i].store_id, CDB_ORDER_BIG);
        cdb_store_u64(buf + off + 8, recs[i].entry_id, CDB_ORDER_BIG);
        buf[off + 16] = recs[i].op;
        cdb_store_u64(buf + off + 17, recs[i].hlc.physical, CDB_ORDER_BIG);
        cdb_store_u32(buf + off + 25, recs[i].hlc.logical, CDB_ORDER_BIG);
        cdb_store_u32(buf + off + 29, recs[i].hlc.peer_id, CDB_ORDER_BIG);
        cdb_store_u32(buf + off + 33, (uint32_t)recs[i].len, CDB_ORDER_BIG);
        off += 37u;
        if (recs[i].len > 0) {
            memcpy(buf + off, recs[i].data, recs[i].len);
            off += recs[i].len;
        }
    }

    return CDB_OK;
}

cdb_status cdb_crdt_slice_decode(const uint8_t *buf, size_t len,
                                 uint32_t *sender_peer_id,
                                 cdb_crdt_delta_rec *recs, size_t cap, size_t *count)
{
    uint32_t magic, nrecs;
    size_t off, i;

    if (buf == NULL || sender_peer_id == NULL || count == NULL)
        return CDB_E_INVALID_ARG;
    if (len < 12u)
        return CDB_E_TRUNCATED;

    magic = cdb_load_u32(buf, CDB_ORDER_BIG);
    if (magic != CDB_CRDT_WIRE_MAGIC)
        return CDB_E_MALFORMED;

    *sender_peer_id = cdb_load_u32(buf + 4, CDB_ORDER_BIG);
    nrecs = cdb_load_u32(buf + 8, CDB_ORDER_BIG);
    *count = nrecs;

    if (recs == NULL && cap < nrecs)
        return CDB_E_BUFFER_TOO_SMALL;

    off = 12u;
    for (i = 0; i < nrecs; i++) {
        uint32_t dlen;
        if (off + 37u > len)
            return CDB_E_TRUNCATED;

        dlen = cdb_load_u32(buf + off + 33, CDB_ORDER_BIG);
        if (off + 37u + dlen > len)
            return CDB_E_TRUNCATED;

        if (i < cap && recs != NULL) {
            recs[i].store_id = cdb_load_u64(buf + off, CDB_ORDER_BIG);
            recs[i].entry_id = cdb_load_u64(buf + off + 8, CDB_ORDER_BIG);
            recs[i].op = buf[off + 16];
            recs[i].hlc.physical = cdb_load_u64(buf + off + 17, CDB_ORDER_BIG);
            recs[i].hlc.logical = cdb_load_u32(buf + off + 25, CDB_ORDER_BIG);
            recs[i].hlc.peer_id = cdb_load_u32(buf + off + 29, CDB_ORDER_BIG);
            recs[i].len = dlen;
            recs[i].data = buf + off + 37u;
        }
        off += 37u + dlen;
    }

    if (cap < nrecs)
        return CDB_E_BUFFER_TOO_SMALL;

    return CDB_OK;
}
