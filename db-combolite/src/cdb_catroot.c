/* Catalog root slot, epoch 1 (spec 14.3.2).
 *
 * A 512-byte record with a start magic, an end magic and a CRC32C over
 * bytes 0..507. The two magics let a torn write be recognised (a slot cut
 * short keeps an old or missing end magic), and let a recovery tool find
 * slots by scanning. Logical IDs are always 8 bytes here, whatever the
 * file's address width. */
#include <string.h>

#include "cdb_catroot.h"
#include "cdb_crc32c.h"
#include "cdb_endian.h"

/* "\x89CDBCAT\n", and the same bytes reversed. */
static const uint8_t start_magic[8] = { 0x89, 0x43, 0x44, 0x42, 0x43, 0x41, 0x54, 0x0A };
static const uint8_t end_magic[8] = { 0x0A, 0x54, 0x41, 0x43, 0x42, 0x44, 0x43, 0x89 };

#define EPOCH 1u
#define OFF_RESERVED 96u
#define RESERVED_LEN 388u
#define OFF_END_MAGIC 484u
#define OFF_TAG 492u   /* tamper-evident tag: left zero, not checked yet */
#define OFF_CRC 508u

/* Writes every field in the file's byte order; reserved bytes and the tag
 * are zero. */
void cdb_catroot_encode(const cdb_catroot *r, uint8_t out[CDB_CATROOT_SLOT_SIZE], cdb_byte_order o)
{
    memset(out, 0, CDB_CATROOT_SLOT_SIZE);
    memcpy(out, start_magic, sizeof start_magic);
    out[8] = (uint8_t)EPOCH;
    out[9] = r->slot_index;
    cdb_store_u32(out + 12, CDB_CATROOT_FORMAT, o);
    cdb_store_u64(out + 16, r->commit_seq, o);
    cdb_store_u64(out + 24, r->write_counter, o);
    cdb_store_u64(out + 32, r->body_root, o);
    cdb_store_u64(out + 40, r->mirror_root, o);
    cdb_store_u64(out + 48, r->enclave_dir_root, o);
    cdb_store_u64(out + 56, r->schema_history_root, o);
    cdb_store_u64(out + 64, r->quick_query_root, o);
    cdb_store_u64(out + 72, r->dedup_root, o);
    cdb_store_u64(out + 80, r->audit_root, o);
    cdb_store_u64(out + 88, r->next_object_id, o);
    memcpy(out + OFF_END_MAGIC, end_magic, sizeof end_magic);
    cdb_store_u32(out + OFF_CRC, cdb_crc32c(out, OFF_CRC), o);
}

/* Checks in this order: start magic (absent: never written), CRC and end
 * magic (damaged), epoch and format (a newer layout: CDB_E_UNSUPPORTED),
 * then that reserved bytes are zero (CDB_E_MALFORMED). */
cdb_status cdb_catroot_decode(const uint8_t in[CDB_CATROOT_SLOT_SIZE], cdb_catroot *r, cdb_byte_order o)
{
    size_t i;
    if (memcmp(in, start_magic, sizeof start_magic) != 0)
        return CDB_E_NOT_FOUND;
    if (cdb_load_u32(in + OFF_CRC, o) != cdb_crc32c(in, OFF_CRC)
        || memcmp(in + OFF_END_MAGIC, end_magic, sizeof end_magic) != 0)
        return CDB_E_CORRUPT;
    if (in[8] != EPOCH || cdb_load_u32(in + 12, o) != CDB_CATROOT_FORMAT)
        return CDB_E_UNSUPPORTED;
    if (in[10] || in[11])
        return CDB_E_MALFORMED;
    for (i = OFF_RESERVED; i < OFF_RESERVED + RESERVED_LEN; i++)
        if (in[i])
            return CDB_E_MALFORMED;
    memset(r, 0, sizeof *r);
    r->slot_index = in[9];
    r->commit_seq = cdb_load_u64(in + 16, o);
    r->write_counter = cdb_load_u64(in + 24, o);
    r->body_root = cdb_load_u64(in + 32, o);
    r->mirror_root = cdb_load_u64(in + 40, o);
    r->enclave_dir_root = cdb_load_u64(in + 48, o);
    r->schema_history_root = cdb_load_u64(in + 56, o);
    r->quick_query_root = cdb_load_u64(in + 64, o);
    r->dedup_root = cdb_load_u64(in + 72, o);
    r->audit_root = cdb_load_u64(in + 80, o);
    r->next_object_id = cdb_load_u64(in + 88, o);
    return CDB_OK;
}
