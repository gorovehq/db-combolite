/* Order-preserving key encoding (spec 14.8.4).
 *
 * Index keys are byte strings whose memcmp order is the natural order of the
 * values they encode. Each component starts with a type-rank byte, so values
 * of different types sort predictably (null < booleans < integers < floats <
 * timestamps < strings < binary < references). Composite keys are the
 * concatenation of their components. Strings use binary (UTF-8 byte) order;
 * language-aware collation comes later (spec 14.8.5). */
#ifndef CDB_KEY_H
#define CDB_KEY_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

#define CDB_KEY_RANK_NULL 0x10u
#define CDB_KEY_RANK_FALSE 0x20u
#define CDB_KEY_RANK_TRUE 0x21u
#define CDB_KEY_RANK_INT 0x30u
#define CDB_KEY_RANK_FLOAT 0x38u
#define CDB_KEY_RANK_TIMESTAMP 0x40u
#define CDB_KEY_RANK_STRING 0x50u
#define CDB_KEY_RANK_BINARY 0x58u
#define CDB_KEY_RANK_REF 0x60u

/* Appends components to a caller buffer. Errors are sticky. */
typedef struct cdb_key_builder {
    uint8_t *buf;
    size_t cap;
    size_t len;
    cdb_status error;
} cdb_key_builder;

void cdb_key_init(cdb_key_builder *k, uint8_t *buf, size_t cap);
cdb_status cdb_key_null(cdb_key_builder *k);
cdb_status cdb_key_bool(cdb_key_builder *k, int v);
cdb_status cdb_key_int(cdb_key_builder *k, int64_t v);
/* All NaNs encode as one value that sorts after +infinity; -0 encodes as +0. */
cdb_status cdb_key_f64(cdb_key_builder *k, double v);
cdb_status cdb_key_timestamp(cdb_key_builder *k, int64_t ticks);
/* Zero bytes are escaped (00 -> 00 FF) and the component ends with 00 00,
 * so a string sorts before any longer string it prefixes. */
cdb_status cdb_key_string(cdb_key_builder *k, const char *s, size_t len);
cdb_status cdb_key_binary(cdb_key_builder *k, const void *data, size_t len);
cdb_status cdb_key_ref(cdb_key_builder *k, uint64_t store_id, uint64_t entry_id);
/* Raw big-endian 64-bit suffix, used to make non-unique index keys unique by
 * appending the entry ID (spec 14.8.3). */
cdb_status cdb_key_u64(cdb_key_builder *k, uint64_t v);

/* memcmp order with the shorter key first on a tie. */
int cdb_key_compare(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);

#endif
