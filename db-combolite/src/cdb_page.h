/* Common page header and trailer (spec 13, 14.2).
 *
 * A page is [header][payload][trailer]. The trailer holds the integrity tag
 * over everything before it. In this milestone the tag is a CRC32C checksum;
 * AEAD tags arrive with the crypto layer (spec 12.1, 14.2.5). */
#ifndef CDB_PAGE_H
#define CDB_PAGE_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

typedef enum cdb_page_format {
    CDB_PAGE_FMT_FULL64,  /* 56-byte header, 64-bit IDs (spec 14.2.1) */
    CDB_PAGE_FMT_FULL32,  /* 40-byte header, 32-bit IDs (spec 14.2.2) */
    CDB_PAGE_FMT_COMPACT  /* 28-byte header, 16-bit IDs, pages <= 1 KiB (spec 14.2.3) */
} cdb_page_format;

/* Checksum algorithm IDs, file header offset 76 (spec 2.9). 0 is invalid. */
#define CDB_CHECKSUM_CRC32C 1u

/* Page type IDs (spec 2.6). */
#define CDB_PAGE_TYPE_ANCHOR_MAP 0x0001u
#define CDB_PAGE_TYPE_FREE_SPACE 0x0002u
#define CDB_PAGE_TYPE_INDEX_NODE 0x0003u
#define CDB_PAGE_TYPE_CATALOG_RECORD 0x0004u
#define CDB_PAGE_TYPE_DOCUMENT 0x0005u
#define CDB_PAGE_TYPE_VECTOR 0x0006u
#define CDB_PAGE_TYPE_OVERFLOW 0x0007u
#define CDB_PAGE_TYPE_TS_BLOCK 0x0008u
#define CDB_PAGE_TYPE_NIBBLET 0x0009u
#define CDB_PAGE_TYPE_ENCLAVE_ROOT 0x000Au
#define CDB_PAGE_TYPE_HELD_LOG 0x000Bu
#define CDB_PAGE_TYPE_DATA_TEST 0x7F00u /* reserved for tests */

/* Page header flags (spec 13, 14.2.6), in the order section 13 lists them. */
#define CDB_PAGE_FLAG_COMPRESSED     0x01u
#define CDB_PAGE_FLAG_EXTENT         0x02u
#define CDB_PAGE_FLAG_OVERFLOW       0x04u /* an overflow continuation */
#define CDB_PAGE_FLAG_TAMPER_EVIDENT 0x08u /* authenticated with keyed integrity tag */
#define CDB_PAGE_FLAG_DANGER_MODE    0x10u

/* Session salt index of an overlay page: the salt in the overlay file's
 * header (spec 14.2.8). */
#define CDB_SALT_INDEX_FILE 0xFFFFu

/* A decoded page header. payload_len counts only the page type's payload,
 * not the store ID slot in front of it (spec 14.2.7); the encoding adds the
 * slot. store_id is the owning store, also on an encrypted page, whose
 * header field holds 0 and whose slot alone, inside the encryption, names
 * the store. */
typedef struct cdb_page_header {
    uint16_t type;
    uint8_t layout_version;
    uint8_t flags;
    uint8_t compression;
    uint32_t key_version;
    uint32_t payload_len;
    uint64_t write_seq;
    uint64_t logical_id;
    uint64_t store_id;
    uint64_t domain_id;
    uint16_t extent_pos;
    uint16_t salt_index; /* 0 on a page that is not encrypted (spec 14.2.8) */
} cdb_page_header;

size_t cdb_page_header_size(cdb_page_format fmt);
size_t cdb_page_trailer_size(cdb_page_format fmt);

/* The store ID slot: the format's ID width, 8, 4 or 2 bytes (spec 14.2.7). */
size_t cdb_page_slot_size(cdb_page_format fmt);

/* Where a page type's payload starts: after the header and the store ID
 * slot. Every page layout's offsets count from here (spec 14.2.7). */
size_t cdb_page_payload_offset(cdb_page_format fmt);

/* Bytes available for a page type's payload in a page of page_size bytes:
 * the room after the store ID slot, the same in every domain. */
size_t cdb_page_capacity(cdb_page_format fmt, size_t page_size);

/* Decodes the header without verifying the page, so a caller can find the
 * key to open it with. h->store_id is the header's field, 0 on an encrypted
 * page. CDB_E_CORRUPT without the marker, CDB_E_MALFORMED for bad fields. */
cdb_status cdb_page_peek(const uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, cdb_page_header *h);

/* Writes h at the start of page, the store ID slot after it, and seals the
 * trailer. When domain_id > 0, encrypts the slot and payload in place using
 * aead_alg and key, and the header's store field holds 0 (spec 14.2.7).
 * When domain_id == 0 and CDB_PAGE_FLAG_TAMPER_EVIDENT is set, seals trailer with HMAC-SHA-256.
 * Otherwise writes CRC32C. */
cdb_status cdb_page_seal_crypto(uint8_t *page, size_t page_size, cdb_page_format fmt,
                               cdb_byte_order order, unsigned checksum_alg,
                               unsigned aead_alg, const uint8_t *key,
                               const cdb_page_header *h);

/* Verifies the marker and trailer, decodes the header, and decrypts the slot and payload
 * in place if domain_id > 0 using aead_alg and key. h->store_id is taken from the slot.
 * CDB_E_CORRUPT on verification failure or a slot that disagrees with the header. */
cdb_status cdb_page_open_crypto(uint8_t *page, size_t page_size, cdb_page_format fmt,
                               cdb_byte_order order, unsigned checksum_alg,
                               unsigned aead_alg, const uint8_t *key,
                               cdb_page_header *h);

/* Writes h at the start of page, the store ID slot, and the checksum trailer
 * at its end. The payload must already be in place, at
 * cdb_page_payload_offset. Fields too wide for the format
 * return CDB_E_OVERFLOW. Unused payload bytes should be zero. */
cdb_status cdb_page_seal(uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, const cdb_page_header *h);

/* Verifies the marker and the trailer, then decodes the header.
 * CDB_E_CORRUPT when the checksum fails. */
cdb_status cdb_page_open(const uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, cdb_page_header *h);

#endif
