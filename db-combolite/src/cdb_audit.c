/* System audit series and hash-chaining (spec 10.7, 10.6, 12.7). */
#include <string.h>

#include "cdb_audit.h"
#include "cdb_catroot.h"
#include "cdb_db.h"
#include "cdb_endian.h"
#include "cdb_index.h"
#include "combolite/cdb_crypto.h"

#define AUDIT_OFF_TYPE        0u
#define AUDIT_OFF_PRINCIPAL   2u
#define AUDIT_OFF_STORE       6u
#define AUDIT_OFF_ENTITY      14u
#define AUDIT_OFF_PREV_HASH   22u
#define AUDIT_OFF_HASH        54u
#define AUDIT_OFF_PAYLOAD_LEN 86u
#define AUDIT_OFF_PAYLOAD     90u
#define AUDIT_HDR_LEN         90u

#define AUDIT_MAX_PAYLOAD 3900u

static void make_audit_key(uint8_t out[16], int64_t ts, uint64_t seq)
{
    cdb_store_u64(out, (uint64_t)ts ^ (UINT64_C(1) << 63), CDB_ORDER_BIG);
    cdb_store_u64(out + 8, seq, CDB_ORDER_BIG);
}

static void decode_audit_key(const uint8_t in[16], int64_t *ts, uint64_t *seq)
{
    *ts = (int64_t)(cdb_load_u64(in, CDB_ORDER_BIG) ^ (UINT64_C(1) << 63));
    *seq = cdb_load_u64(in + 8, CDB_ORDER_BIG);
}

static cdb_status open_audit_index(cdb_db *db, cdb_index *ix, int create, int *have)
{
    cdb_status s;
    *have = 0;
    if (db->pager.catroot.audit_root != 0) {
        if ((s = cdb_index_open(ix, &db->pager, db->pager.catroot.audit_root, &db->cfg.catalog_work)) != CDB_OK)
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
        r.audit_root = ix->root;
        cdb_pager_set_catroot(&db->pager, &r);
    }
    *have = 1;
    return CDB_OK;
}

static cdb_status compute_audit_hash(const uint8_t key[16], uint16_t event_type, uint32_t principal_id,
                                     uint64_t store_id, uint64_t entity_id, const uint8_t prev_hash[32],
                                     const void *payload, uint32_t payload_len, uint8_t out_hash[32])
{
    cdb_crypto_hash_ctx h;
    uint8_t hdr[24];
    uint8_t plen_buf[4];
    cdb_status s;

    if ((s = cdb_crypto_hash_init(&h)) != CDB_OK)
        return s;

    /* 1. Key (16 bytes: sign-flipped timestamp + sequence) */
    if ((s = cdb_crypto_hash_update(&h, key, 16)) != CDB_OK) {
        cdb_crypto_hash_free(&h);
        return s;
    }

    /* 2. Metadata (22 bytes: type(2), principal(4), store(8), entity(8)) */
    cdb_store_u16(hdr, event_type, CDB_ORDER_BIG);
    cdb_store_u32(hdr + 2, principal_id, CDB_ORDER_BIG);
    cdb_store_u64(hdr + 6, store_id, CDB_ORDER_BIG);
    cdb_store_u64(hdr + 14, entity_id, CDB_ORDER_BIG);
    if ((s = cdb_crypto_hash_update(&h, hdr, 22)) != CDB_OK) {
        cdb_crypto_hash_free(&h);
        return s;
    }

    /* 3. prev_hash (32 bytes) */
    if ((s = cdb_crypto_hash_update(&h, prev_hash, 32)) != CDB_OK) {
        cdb_crypto_hash_free(&h);
        return s;
    }

    /* 4. payload_len (4 bytes) */
    cdb_store_u32(plen_buf, payload_len, CDB_ORDER_BIG);
    if ((s = cdb_crypto_hash_update(&h, plen_buf, 4)) != CDB_OK) {
        cdb_crypto_hash_free(&h);
        return s;
    }

    /* 5. payload (payload_len bytes) */
    if (payload_len > 0 && payload != NULL) {
        if ((s = cdb_crypto_hash_update(&h, payload, payload_len)) != CDB_OK) {
            cdb_crypto_hash_free(&h);
            return s;
        }
    }

    s = cdb_crypto_hash_final(&h, out_hash);
    cdb_crypto_hash_free(&h);
    return s;
}

static cdb_status audit_init(cdb_db *db)
{
    cdb_index ix;
    cdb_index_cursor cur;
    uint8_t k[16], v[4096];
    size_t klen, vlen;
    int done, have;
    cdb_status s;

    if (db->audit_initialized)
        return CDB_OK;

    db->audit_initialized = 1;
    memset(db->last_audit_hash, 0, sizeof(db->last_audit_hash));
    db->audit_seq = 0;

    if (db->pager.catroot.audit_root == 0)
        return CDB_OK;

    if ((s = open_audit_index(db, &ix, 0, &have)) != CDB_OK || !have)
        return s;

    if (db->cfg.catalog_page == NULL)
        return CDB_OK;

    if ((s = cdb_index_seek(&cur, &ix, db->cfg.catalog_page, NULL, 0)) != CDB_OK)
        return s;

    for (;;) {
        if ((s = cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK || done)
            break;
        if (klen == 16 && vlen >= AUDIT_HDR_LEN) {
            db->audit_seq = cdb_load_u64(k + 8, CDB_ORDER_BIG);
            memcpy(db->last_audit_hash, v + AUDIT_OFF_HASH, 32);
        }
    }
    return CDB_OK;
}

cdb_status cdb_audit_log(cdb_db *db, uint16_t event_type, uint32_t principal_id,
                         uint64_t store_id, uint64_t entity_id,
                         const void *payload, size_t payload_len)
{
    cdb_index ix;
    uint8_t k[16];
    uint8_t vbuf[4096];
    size_t vlen;
    int64_t now_ts = 0;
    int have_clk = 0, have;
    cdb_status s;

    if (db == NULL)
        return CDB_E_INVALID_ARG;
    if (payload_len > AUDIT_MAX_PAYLOAD)
        return CDB_E_BUFFER_TOO_SMALL;

    if ((s = audit_init(db)) != CDB_OK)
        return s;

    s = cdb_db_now(db, &now_ts, &have_clk);
    if (s != CDB_OK || !have_clk)
        now_ts = (int64_t)(db->audit_seq + 1u);

    db->audit_seq++;
    make_audit_key(k, now_ts, db->audit_seq);

    vlen = AUDIT_HDR_LEN + payload_len;
    cdb_store_u16(vbuf + AUDIT_OFF_TYPE, event_type, CDB_ORDER_BIG);
    cdb_store_u32(vbuf + AUDIT_OFF_PRINCIPAL, principal_id, CDB_ORDER_BIG);
    cdb_store_u64(vbuf + AUDIT_OFF_STORE, store_id, CDB_ORDER_BIG);
    cdb_store_u64(vbuf + AUDIT_OFF_ENTITY, entity_id, CDB_ORDER_BIG);
    memcpy(vbuf + AUDIT_OFF_PREV_HASH, db->last_audit_hash, 32);

    if ((s = compute_audit_hash(k, event_type, principal_id, store_id, entity_id,
                                db->last_audit_hash, payload, (uint32_t)payload_len,
                                vbuf + AUDIT_OFF_HASH)) != CDB_OK)
        return s;

    cdb_store_u32(vbuf + AUDIT_OFF_PAYLOAD_LEN, (uint32_t)payload_len, CDB_ORDER_BIG);
    if (payload_len > 0 && payload != NULL)
        memcpy(vbuf + AUDIT_OFF_PAYLOAD, payload, payload_len);

    if ((s = open_audit_index(db, &ix, 1, &have)) != CDB_OK)
        return s;

    if ((s = cdb_index_put(&ix, k, sizeof k, vbuf, vlen)) != CDB_OK)
        return s;

    if (ix.root != db->pager.catroot.audit_root) {
        cdb_catroot r = db->pager.catroot;
        r.audit_root = ix.root;
        cdb_pager_set_catroot(&db->pager, &r);
    }

    memcpy(db->last_audit_hash, vbuf + AUDIT_OFF_HASH, 32);
    return CDB_OK;
}

cdb_status cdb_audit_verify(cdb_db *db, int64_t from_ts, int64_t to_ts, int *valid)
{
    cdb_index ix;
    cdb_index_cursor cur;
    uint8_t k[16], seek_k[16], v[4096];
    size_t klen, vlen;
    int done, have, has_prev = 0;
    uint8_t expected_prev[32];
    cdb_status s;

    if (db == NULL || valid == NULL)
        return CDB_E_INVALID_ARG;

    *valid = 1;
    if ((s = audit_init(db)) != CDB_OK)
        return s;

    if (db->pager.catroot.audit_root == 0)
        return CDB_OK;

    if ((s = open_audit_index(db, &ix, 0, &have)) != CDB_OK || !have)
        return s;

    if (db->cfg.catalog_page == NULL)
        return CDB_OK;

    make_audit_key(seek_k, from_ts, 0);
    if ((s = cdb_index_seek(&cur, &ix, db->cfg.catalog_page, seek_k, sizeof seek_k)) != CDB_OK)
        return s;

    for (;;) {
        int64_t ts;
        uint64_t seq;
        uint16_t type;
        uint32_t principal, plen;
        uint64_t store, entity;
        uint8_t calc_hash[32];

        if ((s = cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            break;

        if (klen != 16 || vlen < AUDIT_HDR_LEN) {
            *valid = 0;
            return CDB_OK;
        }

        decode_audit_key(k, &ts, &seq);
        (void)seq;
        if (ts >= to_ts)
            break;

        type = cdb_load_u16(v + AUDIT_OFF_TYPE, CDB_ORDER_BIG);
        principal = cdb_load_u32(v + AUDIT_OFF_PRINCIPAL, CDB_ORDER_BIG);
        store = cdb_load_u64(v + AUDIT_OFF_STORE, CDB_ORDER_BIG);
        entity = cdb_load_u64(v + AUDIT_OFF_ENTITY, CDB_ORDER_BIG);
        plen = cdb_load_u32(v + AUDIT_OFF_PAYLOAD_LEN, CDB_ORDER_BIG);

        if (vlen != (size_t)(AUDIT_HDR_LEN + plen)) {
            *valid = 0;
            return CDB_OK;
        }

        /* Verify chain continuity */
        if (has_prev) {
            if (memcmp(v + AUDIT_OFF_PREV_HASH, expected_prev, 32) != 0) {
                *valid = 0;
                return CDB_OK;
            }
        }

        /* Verify self-hash */
        if ((s = compute_audit_hash(k, type, principal, store, entity,
                                    v + AUDIT_OFF_PREV_HASH,
                                    plen > 0 ? v + AUDIT_OFF_PAYLOAD : NULL,
                                    plen, calc_hash)) != CDB_OK)
            return s;

        if (memcmp(calc_hash, v + AUDIT_OFF_HASH, 32) != 0) {
            *valid = 0;
            return CDB_OK;
        }

        memcpy(expected_prev, v + AUDIT_OFF_HASH, 32);
        has_prev = 1;
    }

    *valid = 1;
    return CDB_OK;
}

cdb_status cdb_audit_query(cdb_db *db, int64_t from_ts, int64_t to_ts,
                           uint8_t *payload_buf, size_t payload_cap,
                           cdb_audit_event *events, size_t max_events, size_t *count)
{
    cdb_index ix;
    cdb_index_cursor cur;
    uint8_t k[16], seek_k[16], v[4096];
    size_t klen, vlen, payload_used = 0;
    int done, have;
    cdb_status s;

    if (db == NULL || count == NULL)
        return CDB_E_INVALID_ARG;

    *count = 0;
    if ((s = audit_init(db)) != CDB_OK)
        return s;

    if (db->pager.catroot.audit_root == 0)
        return CDB_OK;

    if ((s = open_audit_index(db, &ix, 0, &have)) != CDB_OK || !have)
        return s;

    if (db->cfg.catalog_page == NULL)
        return CDB_OK;

    make_audit_key(seek_k, from_ts, 0);
    if ((s = cdb_index_seek(&cur, &ix, db->cfg.catalog_page, seek_k, sizeof seek_k)) != CDB_OK)
        return s;

    for (;;) {
        int64_t ts;
        uint64_t seq;
        uint32_t plen;
        cdb_audit_event *ev;

        if ((s = cdb_index_next(&cur, k, sizeof k, &klen, v, sizeof v, &vlen, &done)) != CDB_OK)
            return s;
        if (done)
            break;

        if (klen != 16 || vlen < AUDIT_HDR_LEN)
            return CDB_E_CORRUPT;

        decode_audit_key(k, &ts, &seq);
        if (ts >= to_ts)
            break;

        if (*count >= max_events)
            break;

        ev = &events[*count];
        ev->ts = ts;
        ev->seq = seq;
        ev->event_type = cdb_load_u16(v + AUDIT_OFF_TYPE, CDB_ORDER_BIG);
        ev->principal_id = cdb_load_u32(v + AUDIT_OFF_PRINCIPAL, CDB_ORDER_BIG);
        ev->store_id = cdb_load_u64(v + AUDIT_OFF_STORE, CDB_ORDER_BIG);
        ev->entity_id = cdb_load_u64(v + AUDIT_OFF_ENTITY, CDB_ORDER_BIG);
        memcpy(ev->prev_hash, v + AUDIT_OFF_PREV_HASH, 32);
        memcpy(ev->hash, v + AUDIT_OFF_HASH, 32);

        plen = cdb_load_u32(v + AUDIT_OFF_PAYLOAD_LEN, CDB_ORDER_BIG);
        ev->payload_len = plen;

        if (payload_buf != NULL && plen > 0 && payload_used + plen <= payload_cap) {
            memcpy(payload_buf + payload_used, v + AUDIT_OFF_PAYLOAD, plen);
            ev->payload = payload_buf + payload_used;
            payload_used += plen;
        } else {
            ev->payload = NULL;
        }

        (*count)++;
    }

    return CDB_OK;
}
