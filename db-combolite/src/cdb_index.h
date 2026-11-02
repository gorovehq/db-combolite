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

#define CDB_INDEX_MAX_DEPTH 16u    /* deeper trees are treated as corrupt */
#define CDB_INDEX_MAX_PREFIX 32u   /* longest shared prefix a node stores */
#define CDB_INDEX_MAX_SPLIT 16u    /* most pieces one node can split into */

/* Node flags (spec 14.8.2). */
#define CDB_INDEX_FLAG_UNIQUE 0x01u
#define CDB_INDEX_FLAG_PREFIX 0x02u

/* One entry of a node while it is expanded in work memory (internal; it is
 * public only so callers can size the entries array). */
typedef struct cdb_index_entry {
    uint32_t off;  /* into the work heap: key bytes, then value bytes */
    uint32_t klen; /* full key length (prefix included) */
    uint32_t vlen; /* value bytes in the heap */
    uint32_t tlen; /* the value's length; above the inline limit, the heap holds
                      the first page of its overflow chain (8 bytes, big-endian) */
} cdb_index_entry;

/* Working memory for node edits. cdb_index_work_size() and
 * cdb_index_entries_needed() give the sizes for a page size.
 * Owned by the caller and borrowed by the index for its whole life. Every
 * operation overwrites it, so two indexes may share one work area only if
 * they are never used at the same time. */
typedef struct cdb_index_work {
    uint8_t *bytes;            /* at least cdb_index_work_size(page_size) bytes */
    size_t bytes_size;
    cdb_index_entry *entries;  /* at least cdb_index_entries_needed(page_size) */
    size_t entries_cap;
} cdb_index_work;

/* An open index: a small handle. The tree itself lives in pager pages. */
typedef struct cdb_index {
    cdb_pager *pager;   /* borrowed; must outlive the index */
    uint64_t root;      /* logical page ID of the root, fixed for life */
    uint8_t flags;      /* CDB_INDEX_FLAG_* for nodes this index writes */
    uint64_t store_id;  /* owner, recorded in node page headers */
    cdb_index_work w;
    size_t max_key;     /* longest key accepted */
    size_t max_value;   /* longest value kept inline */
} cdb_index;

/* Sizes of the two work-memory buffers for indexes over pages of
 * page_size bytes. */
size_t cdb_index_work_size(size_t page_size);
size_t cdb_index_entries_needed(size_t page_size);

/* The longest value an index over this pager's pages keeps inline in a leaf.
 * Longer values (up to UINT32_MAX bytes) go to an overflow chain. */
size_t cdb_index_max_value(const cdb_pager *p);

/* Creates an empty index: allocates and writes its root leaf. The caller
 * should save ix->root; it is the only thing needed to open the index again.
 * flags are CDB_INDEX_FLAG_*; store_id tags every node page. The new root
 * becomes durable with the next pager commit. */
cdb_status cdb_index_create(cdb_index *ix, cdb_pager *p, uint64_t store_id, unsigned flags,
                            const cdb_index_work *w);
/* Opens an index whose root is the given logical page. CDB_E_CORRUPT if
 * that page is not an index node. */
cdb_status cdb_index_open(cdb_index *ix, cdb_pager *p, uint64_t root, const cdb_index_work *w);

/* Inserts key with value val, or replaces the value if the key exists.
 * Both buffers are copied. CDB_E_INVALID_ARG for a key longer than
 * ix->max_key or a value of 4 GiB or more; CDB_E_OVERFLOW if a node would
 * need more than CDB_INDEX_MAX_SPLIT pieces. */
cdb_status cdb_index_put(cdb_index *ix, const uint8_t *key, size_t klen, const void *val, size_t vlen);
/* Copies the value into val (vcap bytes). *found is 0 when the key is absent.
 * A value longer than vcap gives CDB_E_BUFFER_TOO_SMALL, with *vlen set. */
cdb_status cdb_index_get(cdb_index *ix, const uint8_t *key, size_t klen,
                         void *val, size_t vcap, size_t *vlen, int *found);
/* Removes key and releases its overflow chain, if any. *found is 0 (and
 * the status CDB_OK) when the key was not there. */
cdb_status cdb_index_delete(cdb_index *ix, const uint8_t *key, size_t klen, int *found);

/* Drops the entire index tree (all interior and leaf nodes, plus any
 * overflow chains) and marks root 0. */
cdb_status cdb_index_drop(cdb_index *ix);

/* Range cursor. Invalidated by any change to the index. It does not use the
 * index's work memory, so lookups (but not changes) may be made while a
 * cursor is open. */
typedef struct cdb_index_cursor {
    cdb_index *ix;
    unsigned depth;                       /* nodes on the current path; 0 = finished */
    uint64_t lid[CDB_INDEX_MAX_DEPTH];    /* the path: root first, current leaf last */
    uint32_t slot[CDB_INDEX_MAX_DEPTH];   /* in the leaf: the next entry to return */
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
