/* Pager: logical pages over append-only storage (spec 1.7, 2, 3.2, 12.1, 14.7).
 *
 * Every logical page ID resolves through the anchor map, a multi-level
 * table updated copy-on-write. Changed pages are appended at the append
 * point; nothing referenced by the committed state is ever overwritten.
 * A commit writes the new anchor map (and its mirror copy) and the free-space
 * list, then commits the file header.
 *
 * This milestone supports one page size per file. All memory comes from the
 * caller through cdb_pager_config. */
#ifndef CDB_PAGER_H
#define CDB_PAGER_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_storage.h"
#include "cdb_header.h"
#include "cdb_page.h"

/* Anchor states (spec 14.7.2). */
#define CDB_ANCHOR_UNUSED 0u
#define CDB_ANCHOR_LIVE 1u
#define CDB_ANCHOR_RESERVED 2u
#define CDB_ANCHOR_RELEASED 3u

/* Page sequence numbers are reserved durably in blocks of this size (spec 12.1). */
#define CDB_SEQ_RESERVE_BLOCK 1024u

typedef struct cdb_anchor_entry {
    uint64_t phys;       /* physical page number; 0 = none */
    uint8_t size_log2;
    uint8_t state;
    uint16_t extent_len;
} cdb_anchor_entry;

typedef struct cdb_free_extent {
    uint64_t start;     /* physical page number */
    uint64_t len;       /* in pages */
    uint64_t freed_seq; /* committed sequence of the commit that freed it */
} cdb_free_extent;

/* One cached anchor-map node. Each node has a primary copy and a mirror copy
 * (spec 3.5). Leaf copies hold the same entries; interior copies point into
 * their own tree. */
typedef struct cdb_pager_node {
    int in_use;
    int dirty;
    unsigned level;
    uint64_t number;
    uint8_t *primary;    /* page image, page_size bytes */
    uint8_t *mirror;     /* page image, page_size bytes */
    uint64_t phys_primary;
    uint64_t phys_mirror;
    uint64_t lru;
} cdb_pager_node;

typedef struct cdb_pager_config {
    uint8_t *arena;              /* 2 * page_size bytes per node, plus one scratch page */
    size_t arena_size;
    cdb_pager_node *nodes;
    size_t node_cap;
    cdb_free_extent *free_list;
    size_t free_cap;
} cdb_pager_config;

typedef struct cdb_pager {
    cdb_storage st;
    cdb_header_state hs;
    cdb_pager_config cfg;
    size_t node_count;
    uint8_t *scratch;
    size_t page_size;
    cdb_page_format fmt;
    cdb_byte_order order;
    unsigned checksum_alg;
    unsigned addr_width;  /* 4 or 8 bytes */
    uint64_t data_start;  /* first physical page after the header and location 0 */
    uint64_t append_point;
    uint64_t next_seq;
    uint64_t seq_hwm;
    uint64_t next_lid;
    unsigned depth;
    uint64_t root_primary;
    uint64_t root_mirror;
    uint64_t free_root;   /* free-space chains as of the last commit */
    uint64_t free_mirror;
    size_t free_len;
    size_t leaf_fan, inner_fan, root_leaf_fan, root_inner_fan;
    uint64_t lru_clock;
    int read_only;
    int dirty;            /* anything to commit */
} cdb_pager;

/* Creates a new database on empty storage. h supplies the file settings:
 * byte order, page size (default_page_log2, used for every page), address
 * width (CDB_LAYOUT_ADDR64), checksum algorithm and database ID. */
cdb_status cdb_pager_create(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg,
                            const cdb_header *h);

cdb_status cdb_pager_open(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg);

/* Allocates a new logical page ID. IDs are never reused (spec 4.12). */
cdb_status cdb_pager_alloc(cdb_pager *p, uint64_t *lid);

/* Writes a new version of logical page lid: seals it with a fresh sequence
 * number and appends it. tmpl supplies type, store and domain. */
cdb_status cdb_pager_write(cdb_pager *p, uint64_t lid, const cdb_page_header *tmpl,
                           const void *payload, size_t len);

/* Reads and verifies logical page lid into page (page_size bytes). The
 * payload starts at page + cdb_page_header_size(p->fmt). */
cdb_status cdb_pager_read(cdb_pager *p, uint64_t lid, uint8_t *page, cdb_page_header *h);

/* Releases logical page lid. Its ID is not reused. */
cdb_status cdb_pager_free(cdb_pager *p, uint64_t lid);

cdb_status cdb_pager_lookup(cdb_pager *p, uint64_t lid, cdb_anchor_entry *e);

/* Commits every change since the last commit (spec 3.2 steps 1, 2 and 4). */
cdb_status cdb_pager_commit(cdb_pager *p);

#endif
