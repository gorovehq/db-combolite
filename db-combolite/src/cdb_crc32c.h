/* CRC-32C (Castagnoli), used for header and page checksums (spec 2.9, 14.1.9). */
#ifndef CDB_CRC32C_H
#define CDB_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/* Continue a CRC over more data. Start with crc = 0. Uses the CPU's
 * CRC-32C instruction when there is one. */
uint32_t cdb_crc32c_update(uint32_t crc, const void *data, size_t len);

/* The portable path alone, and whether cdb_crc32c_update uses the CPU's
 * instruction on this machine; for tests. */
uint32_t cdb_crc32c_sw(uint32_t crc, const void *data, size_t len);
int cdb_crc32c_hw(void);

/* CRC-32C of one buffer. */
static inline uint32_t cdb_crc32c(const void *data, size_t len)
{
    return cdb_crc32c_update(0, data, len);
}

#endif
