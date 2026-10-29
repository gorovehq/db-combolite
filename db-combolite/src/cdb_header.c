/* File header, epoch 1 (spec 1.9, 14.1). */
#include <string.h>

#include "cdb_crc32c.h"
#include "cdb_endian.h"
#include "cdb_header.h"

static const uint8_t header_magic[8] = { 0x89, 0x43, 0x44, 0x42, 0x0D, 0x0A, 0x1A, 0x0A };

#define BYTE_ORDER_CHECK UINT32_C(0x01020304)

/* Field offsets (spec 14.1.4). */
enum {
    OFF_MAGIC = 0, OFF_ORDER = 8, OFF_EPOCH = 9, OFF_ROLE = 10, OFF_LAYOUT = 11,
    OFF_ORDER_CHECK = 12, OFF_DB_ID = 16, OFF_COMMITTED_SEQ = 32, OFF_WRITE_COUNTER = 40,
    OFF_PAGE_SEQ_HWM = 48, OFF_ANCHOR_ROOT = 56, OFF_APPEND_POINT = 64,
    OFF_MIN_PAGE = 72, OFF_MAX_PAGE = 73, OFF_PAGE_MODE = 74, OFF_DEFAULT_PAGE = 75,
    OFF_CHECKSUM_ALG = 76, OFF_AEAD_ALG = 77, OFF_RING_SIZE = 78,
    OFF_COMPAT = 80, OFF_ROCOMPAT = 88, OFF_INCOMPAT = 96,
    OFF_APP_ID = 104, OFF_CREATOR_VER = 108, OFF_WRITER_VER = 112, OFF_RESERVED_A = 116,
    OFF_CREATED = 120, OFF_KEY_REF = 128, OFF_COMPANIONS = 256, OFF_ANCHORS = 400,
    OFF_COMMIT_HWM = 464, OFF_ANCHOR_MIRROR = 472, OFF_CONTENT_HASH_ALG = 480,
    OFF_RESERVED_B = 481, OFF_TAMPER_TAG = 492, OFF_CRC = 508
};

#define COMPANION_SIZE 12u
#define ANCHOR_SIZE 16u
#define RESERVED_A_LEN 4u
#define RESERVED_B_LEN 11u
#define MAX_PAGE_LOG2 30u

static int all_zero(const uint8_t *p, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (p[i])
            return 0;
    return 1;
}

cdb_status cdb_header_encode(const cdb_header *h, uint8_t out[CDB_HEADER_SLOT_SIZE])
{
    cdb_byte_order o = h->order;
    unsigned i;

    if (o != CDB_ORDER_LITTLE && o != CDB_ORDER_BIG)
        return CDB_E_INVALID_ARG;
    memset(out, 0, CDB_HEADER_SLOT_SIZE);
    memcpy(out + OFF_MAGIC, header_magic, sizeof header_magic);
    out[OFF_ORDER] = (uint8_t)o;
    out[OFF_EPOCH] = (uint8_t)CDB_HEADER_EPOCH;
    out[OFF_ROLE] = h->role;
    out[OFF_LAYOUT] = h->layout_flags;
    cdb_store_u32(out + OFF_ORDER_CHECK, BYTE_ORDER_CHECK, o);
    memcpy(out + OFF_DB_ID, h->db_id, sizeof h->db_id);
    cdb_store_u64(out + OFF_COMMITTED_SEQ, h->committed_seq, o);
    cdb_store_u64(out + OFF_WRITE_COUNTER, h->write_counter, o);
    cdb_store_u64(out + OFF_PAGE_SEQ_HWM, h->page_seq_hwm, o);
    cdb_store_u64(out + OFF_ANCHOR_ROOT, h->anchor_root, o);
    cdb_store_u64(out + OFF_APPEND_POINT, h->append_point, o);
    out[OFF_MIN_PAGE] = h->min_page_log2;
    out[OFF_MAX_PAGE] = h->max_page_log2;
    out[OFF_PAGE_MODE] = h->page_mode;
    out[OFF_DEFAULT_PAGE] = h->default_page_log2;
    out[OFF_CHECKSUM_ALG] = h->checksum_alg;
    out[OFF_AEAD_ALG] = h->aead_alg;
    cdb_store_u16(out + OFF_RING_SIZE, h->ring_size, o);
    cdb_store_u64(out + OFF_COMPAT, h->compat_flags, o);
    cdb_store_u64(out + OFF_ROCOMPAT, h->rocompat_flags, o);
    cdb_store_u64(out + OFF_INCOMPAT, h->incompat_flags, o);
    cdb_store_u32(out + OFF_APP_ID, h->app_id, o);
    cdb_store_u32(out + OFF_CREATOR_VER, h->creator_version, o);
    cdb_store_u32(out + OFF_WRITER_VER, h->writer_version, o);
    cdb_store_u64(out + OFF_CREATED, h->created_us, o);
    memcpy(out + OFF_KEY_REF, h->key_ref, CDB_HEADER_KEYREF_LEN);
    for (i = 0; i < CDB_HEADER_COMPANION_SLOTS; i++) {
        uint8_t *p = out + OFF_COMPANIONS + i * COMPANION_SIZE;
        const cdb_companion_slot *c = &h->companions[i];
        cdb_store_u16(p, c->type_id, o);
        p[2] = c->compat_class;
        p[3] = c->state;
        cdb_store_u64(p + 4, c->watermark, o);
    }
    for (i = 0; i < CDB_HEADER_REGION_ANCHORS; i++) {
        uint8_t *p = out + OFF_ANCHORS + i * ANCHOR_SIZE;
        const cdb_region_anchor *a = &h->anchors[i];
        p[0] = a->feature_index;
        cdb_store_u32(p + 4, a->size_hint, o);
        cdb_store_u64(p + 8, a->anchor, o);
    }
    cdb_store_u64(out + OFF_COMMIT_HWM, h->commit_hwm, o);
    cdb_store_u64(out + OFF_ANCHOR_MIRROR, h->anchor_mirror_root, o);
    out[OFF_CONTENT_HASH_ALG] = h->content_hash_alg;
    memcpy(out + OFF_TAMPER_TAG, h->tamper_tag, sizeof h->tamper_tag);
    cdb_store_u32(out + OFF_CRC, cdb_crc32c(out, OFF_CRC), o);
    return CDB_OK;
}

static int key_ref_padding_ok(const uint8_t *k)
{
    size_t i;
    for (i = 0; i < CDB_HEADER_KEYREF_LEN; i++)
        if (k[i] == 0)
            return all_zero(k + i, CDB_HEADER_KEYREF_LEN - i);
    return 1;
}

cdb_status cdb_header_decode(const uint8_t in[CDB_HEADER_SLOT_SIZE], cdb_header *h)
{
    cdb_byte_order o;
    unsigned i;

    if (memcmp(in + OFF_MAGIC, header_magic, sizeof header_magic) != 0)
        return CDB_E_MALFORMED;
    if (in[OFF_ORDER] != (uint8_t)CDB_ORDER_LITTLE && in[OFF_ORDER] != (uint8_t)CDB_ORDER_BIG)
        return CDB_E_MALFORMED;
    o = (cdb_byte_order)in[OFF_ORDER];
    if (cdb_load_u32(in + OFF_CRC, o) != cdb_crc32c(in, OFF_CRC))
        return CDB_E_CORRUPT;
    if (cdb_load_u32(in + OFF_ORDER_CHECK, o) != BYTE_ORDER_CHECK)
        return CDB_E_MALFORMED;
    if (in[OFF_EPOCH] != CDB_HEADER_EPOCH)
        return CDB_E_UNSUPPORTED;

    memset(h, 0, sizeof *h);
    h->order = o;
    h->epoch = in[OFF_EPOCH];
    h->role = in[OFF_ROLE];
    h->layout_flags = in[OFF_LAYOUT];
    memcpy(h->db_id, in + OFF_DB_ID, sizeof h->db_id);
    h->committed_seq = cdb_load_u64(in + OFF_COMMITTED_SEQ, o);
    h->write_counter = cdb_load_u64(in + OFF_WRITE_COUNTER, o);
    h->page_seq_hwm = cdb_load_u64(in + OFF_PAGE_SEQ_HWM, o);
    h->anchor_root = cdb_load_u64(in + OFF_ANCHOR_ROOT, o);
    h->append_point = cdb_load_u64(in + OFF_APPEND_POINT, o);
    h->min_page_log2 = in[OFF_MIN_PAGE];
    h->max_page_log2 = in[OFF_MAX_PAGE];
    h->page_mode = in[OFF_PAGE_MODE];
    h->default_page_log2 = in[OFF_DEFAULT_PAGE];
    h->checksum_alg = in[OFF_CHECKSUM_ALG];
    h->aead_alg = in[OFF_AEAD_ALG];
    h->ring_size = cdb_load_u16(in + OFF_RING_SIZE, o);
    h->compat_flags = cdb_load_u64(in + OFF_COMPAT, o);
    h->rocompat_flags = cdb_load_u64(in + OFF_ROCOMPAT, o);
    h->incompat_flags = cdb_load_u64(in + OFF_INCOMPAT, o);
    h->app_id = cdb_load_u32(in + OFF_APP_ID, o);
    h->creator_version = cdb_load_u32(in + OFF_CREATOR_VER, o);
    h->writer_version = cdb_load_u32(in + OFF_WRITER_VER, o);
    h->created_us = cdb_load_u64(in + OFF_CREATED, o);
    memcpy(h->key_ref, in + OFF_KEY_REF, CDB_HEADER_KEYREF_LEN);
    for (i = 0; i < CDB_HEADER_COMPANION_SLOTS; i++) {
        const uint8_t *p = in + OFF_COMPANIONS + i * COMPANION_SIZE;
        cdb_companion_slot *c = &h->companions[i];
        c->type_id = cdb_load_u16(p, o);
        c->compat_class = p[2];
        c->state = p[3];
        c->watermark = cdb_load_u64(p + 4, o);
        if (c->compat_class > 2 || (c->state & ~0x03u))
            return CDB_E_MALFORMED;
        if (!(c->state & 0x01u) && !all_zero(p, COMPANION_SIZE))
            return CDB_E_MALFORMED; /* an unprovisioned slot must be empty */
    }
    for (i = 0; i < CDB_HEADER_REGION_ANCHORS; i++) {
        const uint8_t *p = in + OFF_ANCHORS + i * ANCHOR_SIZE;
        cdb_region_anchor *a = &h->anchors[i];
        if (!all_zero(p + 1, 3))
            return CDB_E_MALFORMED;
        a->feature_index = p[0];
        a->size_hint = cdb_load_u32(p + 4, o);
        a->anchor = cdb_load_u64(p + 8, o);
    }
    h->commit_hwm = cdb_load_u64(in + OFF_COMMIT_HWM, o);
    h->anchor_mirror_root = cdb_load_u64(in + OFF_ANCHOR_MIRROR, o);
    h->content_hash_alg = in[OFF_CONTENT_HASH_ALG];
    memcpy(h->tamper_tag, in + OFF_TAMPER_TAG, sizeof h->tamper_tag);

    if (h->role > CDB_ROLE_COMMITTED || (h->layout_flags & ~CDB_LAYOUT_KNOWN))
        return CDB_E_MALFORMED;
    if (!all_zero(in + OFF_RESERVED_A, RESERVED_A_LEN) || !all_zero(in + OFF_RESERVED_B, RESERVED_B_LEN))
        return CDB_E_MALFORMED;
    if (h->committed_seq == 0 || h->ring_size < CDB_HEADER_SLOTS || h->page_mode > 1)
        return CDB_E_MALFORMED;
    if (h->min_page_log2 > h->default_page_log2 || h->default_page_log2 > h->max_page_log2
        || h->max_page_log2 > MAX_PAGE_LOG2)
        return CDB_E_MALFORMED;
    if (!key_ref_padding_ok(h->key_ref))
        return CDB_E_MALFORMED;
    return CDB_OK;
}

cdb_status cdb_header_check_features(const cdb_header *h, int *read_only)
{
    *read_only = 0;
    if (h->incompat_flags & ~CDB_FEATURES_INCOMPAT_KNOWN)
        return CDB_E_UNSUPPORTED;
    if (h->rocompat_flags & ~CDB_FEATURES_ROCOMPAT_KNOWN)
        *read_only = 1;
    return CDB_OK;
}

static uint64_t slot_offset(unsigned slot)
{
    return (uint64_t)slot * CDB_HEADER_SLOT_SIZE;
}

static cdb_status write_slot(const cdb_storage *st, unsigned slot, const cdb_header *h)
{
    uint8_t img[CDB_HEADER_SLOT_SIZE];
    cdb_status s = cdb_header_encode(h, img);
    if (s != CDB_OK)
        return s;
    if ((s = cdb_storage_program(st, slot_offset(slot), img, sizeof img)) != CDB_OK)
        return s;
    return cdb_storage_sync(st);
}

cdb_status cdb_header_create(const cdb_storage *st, const cdb_header *h, cdb_header_state *hs)
{
    cdb_header rec = *h;
    cdb_status s;

    rec.epoch = (uint8_t)CDB_HEADER_EPOCH;
    rec.committed_seq = 1;
    rec.ring_size = (uint16_t)CDB_HEADER_SLOTS;
    rec.role = CDB_ROLE_COMMITTED;
    rec.write_counter = 1;
    if ((s = write_slot(st, 0, &rec)) != CDB_OK)
        return s;
    rec.role = CDB_ROLE_WORKING;
    rec.write_counter = 2;
    if ((s = write_slot(st, 1, &rec)) != CDB_OK)
        return s;

    memset(hs, 0, sizeof *hs);
    rec.role = CDB_ROLE_COMMITTED;
    rec.write_counter = 1;
    hs->record = rec;
    hs->record_slot = 0;
    hs->working_slot = 0; /* slot 1 holds the newest write, so slot 0 is next */
    hs->write_counter = 2;
    return CDB_OK;
}

/* Is a a better committed record than b (spec 1.9)? */
static int record_better(const cdb_header *a, const cdb_header *b)
{
    if (a->committed_seq != b->committed_seq)
        return a->committed_seq > b->committed_seq;
    if (a->role != b->role)
        return a->role == CDB_ROLE_COMMITTED;
    return a->write_counter > b->write_counter;
}

cdb_status cdb_header_open(const cdb_storage *st, cdb_header_state *hs)
{
    cdb_header slots[CDB_HEADER_SLOTS];
    int valid[CDB_HEADER_SLOTS];
    unsigned i, best = 0, newest = 0, missing = 0;
    int any = 0, unsupported = 0;
    cdb_status s;

    for (i = 0; i < CDB_HEADER_SLOTS; i++) {
        uint8_t img[CDB_HEADER_SLOT_SIZE];
        valid[i] = 0;
        s = cdb_storage_read(st, slot_offset(i), img, sizeof img);
        if (s == CDB_E_TRUNCATED) {
            missing++; /* never written, or cut short by a crash */
            continue;
        }
        if (s != CDB_OK)
            return s;
        s = cdb_header_decode(img, &slots[i]);
        if (s != CDB_OK) {
            unsupported |= s == CDB_E_UNSUPPORTED;
            continue;
        }
        valid[i] = 1;
        if (!any || record_better(&slots[i], &slots[best]))
            best = i;
        if (!any || slots[i].write_counter > slots[newest].write_counter)
            newest = i;
        any = 1;
    }
    if (!any) {
        if (unsupported)
            return CDB_E_UNSUPPORTED;
        return missing == CDB_HEADER_SLOTS ? CDB_E_NOT_FOUND : CDB_E_CORRUPT;
    }
    for (i = 0; i < CDB_HEADER_SLOTS; i++)
        if (valid[i] && memcmp(slots[i].db_id, slots[best].db_id, sizeof slots[i].db_id) != 0)
            valid[i] = 0; /* a stray slot from another database is ignored */

    memset(hs, 0, sizeof *hs);
    hs->record = slots[best];
    hs->record_slot = best;
    /* A later checkpoint may have raised the reservation marks: take the
     * maximum across every valid slot (spec 12.1). */
    for (i = 0; i < CDB_HEADER_SLOTS; i++) {
        if (!valid[i])
            continue;
        if (slots[i].page_seq_hwm > hs->record.page_seq_hwm)
            hs->record.page_seq_hwm = slots[i].page_seq_hwm;
        if (slots[i].commit_hwm > hs->record.commit_hwm)
            hs->record.commit_hwm = slots[i].commit_hwm;
        if (slots[i].write_counter > hs->write_counter)
            hs->write_counter = slots[i].write_counter;
    }
    if (valid[newest] == 0)
        newest = best;
    hs->working_slot = (newest + 1) % CDB_HEADER_SLOTS;
    return cdb_header_check_features(&hs->record, &hs->read_only);
}

static cdb_status write_next(const cdb_storage *st, cdb_header_state *hs, cdb_header *rec)
{
    cdb_status s;

    if (hs->read_only)
        return CDB_E_READ_ONLY;
    rec->epoch = (uint8_t)CDB_HEADER_EPOCH;
    rec->ring_size = (uint16_t)CDB_HEADER_SLOTS;
    rec->write_counter = hs->write_counter + 1;
    if (rec->page_seq_hwm < hs->record.page_seq_hwm)
        rec->page_seq_hwm = hs->record.page_seq_hwm;
    if (rec->commit_hwm < hs->record.commit_hwm)
        rec->commit_hwm = hs->record.commit_hwm;
    if ((s = write_slot(st, hs->working_slot, rec)) != CDB_OK)
        return s;
    hs->write_counter = rec->write_counter;
    hs->working_slot = (hs->working_slot + 1) % CDB_HEADER_SLOTS;
    return CDB_OK;
}

cdb_status cdb_header_commit(const cdb_storage *st, cdb_header_state *hs, const cdb_header *h)
{
    cdb_header rec = *h;
    unsigned slot = hs->working_slot;
    cdb_status s;

    memcpy(rec.db_id, hs->record.db_id, sizeof rec.db_id);
    rec.order = hs->record.order;
    rec.committed_seq = hs->record.committed_seq + 1;
    rec.role = CDB_ROLE_COMMITTED;
    if ((s = write_next(st, hs, &rec)) != CDB_OK)
        return s;
    hs->record = rec;
    hs->record_slot = slot;
    return CDB_OK;
}

cdb_status cdb_header_checkpoint(const cdb_storage *st, cdb_header_state *hs,
                                 uint64_t page_seq_hwm, uint64_t commit_hwm)
{
    cdb_header rec = hs->record;
    cdb_status s;

    rec.role = CDB_ROLE_WORKING;
    rec.page_seq_hwm = page_seq_hwm;
    rec.commit_hwm = commit_hwm;
    if ((s = write_next(st, hs, &rec)) != CDB_OK)
        return s;
    hs->record.page_seq_hwm = rec.page_seq_hwm;
    hs->record.commit_hwm = rec.commit_hwm;
    return CDB_OK;
}
