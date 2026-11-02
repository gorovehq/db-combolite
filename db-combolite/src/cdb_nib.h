/* Nibblets: small records packed together in shared pages (spec 16, 14.18).
 *
 * A record up to a quarter of the page size can live in a nibblet page
 * instead of a page of its own. Nibblet pages are shared by every store of
 * an encryption domain (only the public domain, 0, exists so far). Each page
 * has a slot directory at the front and records packed from the end; a
 * record's address is (page, slot), and slots never change while a record
 * lives, even when the page is compacted.
 *
 * Every record carries its owner's store ID, its kind and the key its
 * owner's index finds it under (the back-reference). That lets merging
 * move a record and tell the owner its new address without knowing the
 * record's format.
 *
 * Free space is tracked in one ordered index whose root is in the catalog
 * root slot (spec 14.18.3), keyed by (domain, free bytes, page). A record
 * goes to the fullest page it fits in. cdb_nib_merge, run at reconciliation,
 * moves the records of pages under half full into others and frees the
 * emptied pages; a record whose owner is gone (a dropped store) is dropped.
 *
 * Addresses: in memory an address is one uint64_t. A plain page ID is a
 * record on its own page; a nibblet has CDB_NIB_ADDR_FLAG set, with the page
 * in bits 16..62 and the slot in bits 0..15. On disk they take 8 bytes (a
 * page) or 10 bytes (page then slot), big-endian (cdb_nib_addr_store).
 *
 * Nibblets are always on. Their two working pages are the last two pages of
 * cdb_db_config.doc_buffers (cdb_doc_buffers_size), which every database
 * using the built-in apply supplies. */
#ifndef CDB_NIB_H
#define CDB_NIB_H

#include "cdb_db.h"

#define CDB_NIB_ADDR_FLAG (UINT64_C(1) << 63)
#define CDB_NIB_KIND_REVISION 1u   /* a document or table revision (owner key: entry, revision) */
#define CDB_NIB_KIND_TS_BLOCK 2u   /* a time-series block (owner key: series, first timestamp) */
#define CDB_NIB_KIND_COLCHUNK 3u   /* a column chunk (owner key: group's last entry, field ID) */
#define CDB_NIB_KIND_OVERFLOW_TAIL 4u /* last partly filled page of an overflow chain (spec 16.5) */
#define CDB_NIB_KEY_MAX 32u        /* longest owner key */
#define CDB_NIB_MERGE_BUDGET 16u   /* pages merged per reconciliation */

int cdb_nib_is(uint64_t addr);
uint64_t cdb_nib_page(uint64_t addr);
unsigned cdb_nib_slot(uint64_t addr);

/* The address as stored in an owner's index value: 8 or 10 bytes. */
size_t cdb_nib_addr_store(uint8_t *out, uint64_t addr);
cdb_status cdb_nib_addr_load(const uint8_t *in, size_t len, uint64_t *addr);

/* Whether a record of len data bytes would be a nibblet: nibblets are on
 * and it is at most a quarter page. */
int cdb_nib_fits(const cdb_db *db, size_t len);

/* Places a record and returns its address. */
cdb_status cdb_nib_put(cdb_db *db, uint64_t store, unsigned kind, const uint8_t *okey, size_t oklen,
                       const void *data, size_t len, uint64_t *addr);
/* Copies a record's data; CDB_E_BUFFER_TOO_SMALL with *len set. */
cdb_status cdb_nib_get(cdb_db *db, uint64_t addr, void *out, size_t cap, size_t *len);
/* Removes a record; a page left empty is freed. */
cdb_status cdb_nib_free(cdb_db *db, uint64_t addr);
/* At reconciliation: merges up to budget under-half-full pages. */
cdb_status cdb_nib_merge(cdb_db *db, unsigned budget);

/* Sweeps a nibblet page for records belonging to dropped stores.
 * Removes them, updates free space in the index, and frees the page if empty. */
cdb_status cdb_nib_sweep_page(cdb_db *db, uint64_t page, uint64_t *nibblets_freed, int *page_freed);

#endif
