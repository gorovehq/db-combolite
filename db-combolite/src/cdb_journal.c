/* Delta journal (spec 14.4). */
#include <string.h>

#include "cdb_crc32c.h"
#include "cdb_endian.h"
#include "cdb_journal.h"

static const uint8_t journal_magic[8] = { 0x89, 0x43, 0x44, 0x42, 0x43, 0x4D, 0x50, 0x0A };

#define RECORD_MARKER_0 0xC0u
#define RECORD_MARKER_1 0xDEu
#define JOURNAL_EPOCH 1u

cdb_status cdb_journal_create(cdb_journal *j, const cdb_storage *st, cdb_byte_order order,
                              const cdb_journal_header *hdr)
{
    uint8_t h[CDB_JOURNAL_HEADER_SIZE];
    cdb_status s;

    memset(h, 0, sizeof h);
    memcpy(h, journal_magic, sizeof journal_magic);
    h[8] = (uint8_t)order;
    h[9] = (uint8_t)JOURNAL_EPOCH;
    cdb_store_u16(h + 10, hdr->type_id, order);
    cdb_store_u16(h + 12, hdr->slot, order);
    memcpy(h + 16, hdr->db_id, 16);
    cdb_store_u64(h + 32, hdr->unsealed_seq, order);
    cdb_store_u64(h + 40, hdr->first_seq, order);
    cdb_store_u64(h + 48, hdr->domain_id, order);
    cdb_store_u32(h + 60, cdb_crc32c(h, 60), order);
    if ((s = cdb_storage_program(st, 0, h, sizeof h)) != CDB_OK)
        return s;
    memset(j, 0, sizeof *j);
    j->st = *st;
    j->order = order;
    j->hdr = *hdr;
    j->end = CDB_JOURNAL_HEADER_SIZE;
    j->next_seq = hdr->first_seq;
    return CDB_OK;
}

cdb_status cdb_journal_open(cdb_journal *j, const cdb_storage *st, cdb_byte_order order)
{
    uint8_t h[CDB_JOURNAL_HEADER_SIZE];
    cdb_status s;
    size_t i;

    if ((s = cdb_storage_read(st, 0, h, sizeof h)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if (memcmp(h, journal_magic, sizeof journal_magic) != 0 || h[8] != (uint8_t)order)
        return CDB_E_CORRUPT;
    if (cdb_load_u32(h + 60, order) != cdb_crc32c(h, 60))
        return CDB_E_CORRUPT;
    if (h[9] != JOURNAL_EPOCH)
        return CDB_E_UNSUPPORTED;
    for (i = 14; i < 16; i++)
        if (h[i])
            return CDB_E_MALFORMED;
    memset(j, 0, sizeof *j);
    j->st = *st;
    j->order = order;
    j->hdr.type_id = cdb_load_u16(h + 10, order);
    j->hdr.slot = cdb_load_u16(h + 12, order);
    memcpy(j->hdr.db_id, h + 16, 16);
    j->hdr.unsealed_seq = cdb_load_u64(h + 32, order);
    j->hdr.first_seq = cdb_load_u64(h + 40, order);
    j->hdr.domain_id = cdb_load_u64(h + 48, order);
    j->end = CDB_JOURNAL_HEADER_SIZE;
    j->next_seq = j->hdr.first_seq;
    return CDB_OK;
}

static size_t record_size(uint32_t payload_len)
{
    return CDB_RECORD_HEADER_SIZE + (size_t)payload_len + CDB_RECORD_TRAILER_SIZE;
}

cdb_status cdb_journal_append(cdb_journal *j, cdb_record *r, uint8_t *buf, size_t buf_size)
{
    size_t size = record_size(r->payload_len);
    uint8_t *p = buf;
    cdb_status s;

    if (size > buf_size)
        return CDB_E_BUFFER_TOO_SMALL;
    r->seq = j->next_seq;
    memset(p, 0, CDB_RECORD_HEADER_SIZE);
    p[0] = RECORD_MARKER_0;
    p[1] = RECORD_MARKER_1;
    p[2] = r->kind;
    p[3] = r->flags;
    cdb_store_u32(p + 4, r->payload_len, j->order);
    cdb_store_u64(p + 8, r->seq, j->order);
    cdb_store_u64(p + 16, r->tx_id, j->order);
    cdb_store_u64(p + 24, r->store_id, j->order);
    cdb_store_u64(p + 32, r->entry_id, j->order);
    cdb_store_u32(p + 40, r->key_version, j->order);
    if (r->payload_len) /* the payload may already sit in place inside buf */
        memmove(p + CDB_RECORD_HEADER_SIZE, r->payload, r->payload_len);
    memset(p + size - CDB_RECORD_TRAILER_SIZE, 0, CDB_RECORD_TRAILER_SIZE);
    cdb_store_u32(p + size - CDB_RECORD_TRAILER_SIZE,
                  cdb_crc32c(p, size - CDB_RECORD_TRAILER_SIZE), j->order);
    if ((s = cdb_storage_program(&j->st, j->end, p, size)) != CDB_OK)
        return s;
    j->end += size;
    j->next_seq++;
    return CDB_OK;
}

cdb_status cdb_journal_read(cdb_journal *j, uint64_t offset, uint8_t *buf, size_t buf_size,
                            cdb_record *r, uint64_t *next)
{
    uint32_t payload_len;
    size_t size, i;
    cdb_status s;

    if (buf_size < CDB_RECORD_HEADER_SIZE + CDB_RECORD_TRAILER_SIZE)
        return CDB_E_BUFFER_TOO_SMALL;
    if ((s = cdb_storage_read(&j->st, offset, buf, CDB_RECORD_HEADER_SIZE)) != CDB_OK)
        return s;
    if (buf[0] != RECORD_MARKER_0 || buf[1] != RECORD_MARKER_1)
        return CDB_E_CORRUPT;
    payload_len = cdb_load_u32(buf + 4, j->order);
    size = record_size(payload_len);
    if (size > buf_size)
        return CDB_E_CORRUPT; /* larger than any record we would write */
    if ((s = cdb_storage_read(&j->st, offset + CDB_RECORD_HEADER_SIZE, buf + CDB_RECORD_HEADER_SIZE,
                              size - CDB_RECORD_HEADER_SIZE)) != CDB_OK)
        return s;
    if (cdb_load_u32(buf + size - CDB_RECORD_TRAILER_SIZE, j->order)
        != cdb_crc32c(buf, size - CDB_RECORD_TRAILER_SIZE))
        return CDB_E_CORRUPT;
    for (i = size - CDB_RECORD_TRAILER_SIZE + 4; i < size; i++)
        if (buf[i])
            return CDB_E_CORRUPT;
    memset(r, 0, sizeof *r);
    r->kind = buf[2];
    r->flags = buf[3];
    r->payload_len = payload_len;
    r->seq = cdb_load_u64(buf + 8, j->order);
    r->tx_id = cdb_load_u64(buf + 16, j->order);
    r->store_id = cdb_load_u64(buf + 24, j->order);
    r->entry_id = cdb_load_u64(buf + 32, j->order);
    r->key_version = cdb_load_u32(buf + 40, j->order);
    r->payload = buf + CDB_RECORD_HEADER_SIZE;
    *next = offset + size;
    return CDB_OK;
}

void cdb_commit_encode(const cdb_commit_info *c, uint8_t out[CDB_COMMIT_PAYLOAD_SIZE], cdb_byte_order order)
{
    cdb_store_u64(out, c->commit_number, order);
    cdb_store_u64(out + 8, c->timestamp, order);
    cdb_store_u64(out + 16, c->principal, order);
    cdb_store_u32(out + 24, c->delta_count, order);
    cdb_store_u32(out + 28, c->track_mask, order);
    cdb_store_u16(out + 32, 0, order); /* unsigned (spec 12.5) */
}

cdb_status cdb_commit_decode(const cdb_record *r, cdb_commit_info *c, cdb_byte_order order)
{
    uint16_t sig_len;
    if (r->kind != CDB_RECORD_COMMIT || r->payload_len < CDB_COMMIT_PAYLOAD_SIZE)
        return CDB_E_MALFORMED;
    c->commit_number = cdb_load_u64(r->payload, order);
    c->timestamp = cdb_load_u64(r->payload + 8, order);
    c->principal = cdb_load_u64(r->payload + 16, order);
    c->delta_count = cdb_load_u32(r->payload + 24, order);
    c->track_mask = cdb_load_u32(r->payload + 28, order);
    sig_len = cdb_load_u16(r->payload + 32, order);
    if ((uint32_t)CDB_COMMIT_PAYLOAD_SIZE + sig_len != r->payload_len)
        return CDB_E_MALFORMED;
    return CDB_OK;
}
