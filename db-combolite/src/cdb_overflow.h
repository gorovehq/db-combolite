/* Overflow chains (spec 14.15): a value too long for the page that refers to
 * it continues in a chain of overflow pages (type 0x0007, flagged as overflow
 * continuations). Each page holds the logical ID of the next page (0 at the
 * end), at the file's address width, followed by the next part of the value.
 * Every page but the last is full. The owner records the first page's ID and
 * the value's total length.
 *
 * Chains are copy-on-write like every other logical page, so a chain written
 * inside an uncommitted change disappears with it. */
#ifndef CDB_OVERFLOW_H
#define CDB_OVERFLOW_H

#include <stddef.h>
#include <stdint.h>

#include "cdb_pager.h"

struct cdb_db;

#define CDB_OVERFLOW_OWNER_PREV_PAGE 0u
#define CDB_OVERFLOW_OWNER_DOC_REV 1u
#define CDB_OVERFLOW_OWNER_COL_CHUNK 2u

/* Value bytes one overflow page holds. */
size_t cdb_overflow_room(const cdb_pager *p);

/* Pages a chain of len bytes takes. */
uint64_t cdb_overflow_pages(const cdb_pager *p, size_t len);

/* Writes len bytes (len > 0) as a new chain owned by store; *first gets
 * its first page. scratch holds one page payload (cdb_page_capacity bytes).
 * The data is copied; it must not lie inside scratch. */
cdb_status cdb_overflow_write(cdb_pager *p, uint64_t store, const void *data, size_t len, uint8_t *scratch,
                              uint64_t *first);

/* Extended writer: if db != NULL and the tail fits in a nibblet, the final
 * fragment is stored as a nibblet of kind 4 (spec 16.5). */
cdb_status cdb_overflow_write_db(struct cdb_db *db, cdb_pager *p, uint64_t store, const void *data, size_t len,
                                 uint8_t *scratch, const uint8_t *okey, size_t oklen, uint64_t *first);

/* Reads the chain at first, which must hold exactly len bytes owned by
 * store, into out. page is a page_size buffer. out must have room for len
 * bytes. CDB_E_CORRUPT if any page does not match what the chain needs. */
cdb_status cdb_overflow_read(cdb_pager *p, uint64_t first, uint64_t store, void *out, size_t len, uint8_t *page);

/* Extended reader: resolves tail nibblets if present when db != NULL. */
cdb_status cdb_overflow_read_db(struct cdb_db *db, cdb_pager *p, uint64_t first, uint64_t store, void *out,
                                size_t len, uint8_t *page);

/* Releases every page of the chain at first, which holds len bytes owned by
 * store. page is a page_size buffer. */
cdb_status cdb_overflow_free(cdb_pager *p, uint64_t first, uint64_t store, size_t len, uint8_t *page);

/* Extended free: releases any tail nibblet when db != NULL. */
cdb_status cdb_overflow_free_db(struct cdb_db *db, cdb_pager *p, uint64_t first, uint64_t store, size_t len,
                                uint8_t *page);

/* Relocates an overflow tail nibblet during reconciliation merge. */
cdb_status cdb_overflow_nib_moved(struct cdb_db *db, uint64_t store, const uint8_t *okey, size_t oklen,
                                  uint64_t old_addr, uint64_t addr);

#endif
