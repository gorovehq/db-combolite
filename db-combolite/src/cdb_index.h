/* Ordered index: a B+tree over pager pages (spec 9.5, 14.8).
 *
 * Nodes are logical pages, so rewriting a node never disturbs its parent:
 * the pager's anchor map gives copy-on-write underneath (spec 1.7). The root
 * keeps its logical ID for the life of the index; owners store only that.
 * Leaves have no sibling links; cursors walk through the parents.
 *
 * Keys are byte strings compared with memcmp (build them with cdb_key).
 * Every stored key is unique: a non-unique index appends the entry ID to
 * each key (spec 14.8.3). put replaces the value of an existing key.
 *
 * A value longer than cdb_index_max_value() is kept in an overflow chain
 * (spec 14.15); the leaf holds its length and the chain's first page. Keys
 * are always inline. */
#ifndef CDB_INDEX_H
#define CDB_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "cdb_pager.h"

#define CDB_INDEX_MAX_DEPTH 16u
#define CDB_INDEX_MAX_PREFIX 32u   /* longest shared prefix a node stores */
#define CDB_INDEX_MAX_SPLIT 16u    /* most pieces one node can split into */

/* Node flags (spec 14.8.2). */
#define CDB_INDEX_FLAG_UNIQUE 0x01u
#define CDB_INDEX_FLAG_PREFIX 0x02u

typedef struct cdb_index_entry {
    uint32_t off;  /* into the work heap: key bytes, then value bytes */
    uint32_t klen;
    uint32_t vlen; /* value bytes in the heap */
    uint32_t tlen; /* the value's length; above the inline limit, the heap holds
                      the first page of its overflow chain (8 bytes, big-endian) */
} cdb_index_entry;

/* Working memory for node edits. cdb_index_work_size() and
 * cdb_index_entries_needed() give the sizes for a page size. */
typedef struct cdb_index_work {
    uint8_t *bytes;
    size_t bytes_size;
    cdb_index_entry *entries;
    size_t entries_cap;
} cdb_index_work;

typedef struct cdb_index {
    cdb_pager *pager;
    uint64_t root;      /* logical page ID of the root, fixed for life */
    uint8_t flags;      /* CDB_INDEX_FLAG_* for nodes this index writes */
    uint64_t store_id;  /* owner, recorded in node page headers */
    cdb_index_work w;
    size_t max_key;
    size_t max_value;   /* longest value kept inline */
} cdb_index;

size_t cdb_index_work_size(size_t page_size);
size_t cdb_index_entries_needed(size_t page_size);

/* The longest value an index over this pager's pages keeps inline in a leaf.
 * Longer values (up to UINT32_MAX bytes) go to an overflow chain. */
size_t cdb_index_max_value(const cdb_pager *p);

/* Creates an empty index: allocates and writes its root leaf. */
cdb_status cdb_index_create(cdb_index *ix, cdb_pager *p, uint64_t store_id, unsigned flags,
                            const cdb_index_work *w);
/* Opens an index whose root is the given logical page. */
cdb_status cdb_index_open(cdb_index *ix, cdb_pager *p, uint64_t root, const cdb_index_work *w);

cdb_status cdb_index_put(cdb_index *ix, const uint8_t *key, size_t klen, const void *val, size_t vlen);
/* Copies the value into val (vcap bytes). *found is 0 when the key is absent.
 * A value longer than vcap gives CDB_E_BUFFER_TOO_SMALL, with *vlen set. */
cdb_status cdb_index_get(cdb_index *ix, const uint8_t *key, size_t klen,
                         void *val, size_t vcap, size_t *vlen, int *found);
cdb_status cdb_index_delete(cdb_index *ix, const uint8_t *key, size_t klen, int *found);

/* Range cursor. Invalidated by any change to the index. */
typedef struct cdb_index_cursor {
    cdb_index *ix;
    unsigned depth;
    uint64_t lid[CDB_INDEX_MAX_DEPTH];
    uint32_t slot[CDB_INDEX_MAX_DEPTH];
    uint8_t *page;     /* caller buffer, page_size bytes */
    int done;
} cdb_index_cursor;

/* Positions the cursor at the first key >= key (klen 0: the first key). */
cdb_status cdb_index_seek(cdb_index_cursor *c, cdb_index *ix, uint8_t *page,
                          const uint8_t *key, size_t klen);
/* Copies the current entry out and advances. *done is 1 when exhausted. */
cdb_status cdb_index_next(cdb_index_cursor *c, uint8_t *key, size_t kcap, size_t *klen,
                          void *val, size_t vcap, size_t *vlen, int *done);

#endif
