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
#define CDB_PAGE_TYPE_DATA_TEST 0x7F00u /* reserved for tests */

/* Page header flags (spec 13, 14.2.6), in the order section 13 lists them. */
#define CDB_PAGE_FLAG_COMPRESSED     0x01u
#define CDB_PAGE_FLAG_EXTENT         0x02u
#define CDB_PAGE_FLAG_OVERFLOW       0x04u /* an overflow continuation */
#define CDB_PAGE_FLAG_TAMPER_EVIDENT 0x08u /* authenticated with keyed integrity tag */
#define CDB_PAGE_FLAG_DANGER_MODE    0x10u

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
} cdb_page_header;

size_t cdb_page_header_size(cdb_page_format fmt);
size_t cdb_page_trailer_size(cdb_page_format fmt);

/* Bytes available for payload in a page of page_size bytes. */
size_t cdb_page_capacity(cdb_page_format fmt, size_t page_size);

/* Writes h at the start of page and seals the trailer.
 * When domain_id > 0, encrypts the payload in place using aead_alg and key.
 * When domain_id == 0 and CDB_PAGE_FLAG_TAMPER_EVIDENT is set, seals trailer with HMAC-SHA-256.
 * Otherwise writes CRC32C. */
cdb_status cdb_page_seal_crypto(uint8_t *page, size_t page_size, cdb_page_format fmt,
                               cdb_byte_order order, unsigned checksum_alg,
                               unsigned aead_alg, const uint8_t *key,
                               const cdb_page_header *h);

/* Verifies the marker and trailer, decodes the header, and decrypts the payload in place
 * if domain_id > 0 using aead_alg and key. CDB_E_CORRUPT on verification failure. */
cdb_status cdb_page_open_crypto(uint8_t *page, size_t page_size, cdb_page_format fmt,
                               cdb_byte_order order, unsigned checksum_alg,
                               unsigned aead_alg, const uint8_t *key,
                               cdb_page_header *h);

/* Writes h at the start of page and the checksum trailer at its end.
 * The payload must already be in place. Fields too wide for the format
 * return CDB_E_OVERFLOW. Unused payload bytes should be zero. */
cdb_status cdb_page_seal(uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, const cdb_page_header *h);

/* Verifies the marker and the trailer, then decodes the header.
 * CDB_E_CORRUPT when the checksum fails. */
cdb_status cdb_page_open(const uint8_t *page, size_t page_size, cdb_page_format fmt,
                         cdb_byte_order order, unsigned checksum_alg, cdb_page_header *h);

#endif
