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
#include "cdb_catroot.h"
#include "cdb_header.h"
#include "cdb_page.h"

/* Anchor states (spec 14.7.2). */
#define CDB_ANCHOR_UNUSED 0u
#define CDB_ANCHOR_LIVE 1u
#define CDB_ANCHOR_RESERVED 2u
#define CDB_ANCHOR_RELEASED 3u
/* Only in memory, never written: the page is in the overlay (spec 5.9.1). */
#define CDB_ANCHOR_PRIVATE 4u

/* Most pages one free-space chain may use; a longer list needs compaction. */
#define CDB_PAGER_CHAIN_MAX 128u

/* Page sequence numbers are reserved durably in blocks of this size (spec 12.1). */
#define CDB_SEQ_RESERVE_BLOCK 1024u

/* One anchor-map leaf entry, decoded: where logical page lid lives now
 * (spec 14.7.2). */
typedef struct cdb_anchor_entry {
    uint64_t phys;       /* physical page number; 0 = none */
    uint8_t size_log2;   /* page size as a power of two */
    uint8_t state;       /* one of CDB_ANCHOR_* */
    uint16_t extent_len; /* pages in the extent, or 0 for a single page (always 0 for now) */
} cdb_anchor_entry;

/* A run of free physical pages, as kept on the free-space list
 * (spec 14.7.4). A freed_seq of 0 means "reusable now". */
typedef struct cdb_free_extent {
    uint64_t start;     /* physical page number */
    uint64_t len;       /* in pages */
    uint64_t freed_seq; /* committed sequence of the commit that freed it */
} cdb_free_extent;

/* One cached anchor-map node. Each node has a primary copy and a mirror copy
 * (spec 3.5). Leaf copies hold the same entries; interior copies point into
 * their own tree. */
typedef struct cdb_pager_node {
    int in_use;          /* this cache slot holds a node */
    int dirty;           /* changed since the last commit; never evicted */
    unsigned level;      /* 0 = leaf; the root is at depth - 1 */
    uint64_t number;     /* position from the left within its level */
    uint8_t *primary;    /* page image, page_size bytes */
    uint8_t *mirror;     /* page image, page_size bytes */
    uint64_t phys_primary; /* where the primary copy is on disk; 0 = not yet written */
    uint64_t phys_mirror;  /* where the mirror copy is on disk; 0 = not yet written */
    uint64_t lru;        /* last-use stamp, for choosing which clean node to evict */
} cdb_pager_node;

/* Memory the caller lends the pager for its whole life. The pager keeps the
 * pointers and never frees them. The number of usable node slots is the
 * smaller of node_cap and what the arena holds; it must be at least 10. */
typedef struct cdb_pager_config {
    uint8_t *arena;              /* 2 * page_size bytes per node, plus two scratch pages */
    size_t arena_size;
    cdb_pager_node *nodes;       /* node_cap cache slots */
    size_t node_cap;
    cdb_free_extent *free_list;  /* in-memory free-space list, free_cap entries */
    size_t free_cap;
} cdb_pager_config;

/* An open pager. All fields are internal; other modules use the functions
 * below. Not safe to share between threads. */
typedef struct cdb_pager {
    cdb_storage st;
    cdb_header_state hs;  /* file header; hs.record is the committed record */
    cdb_pager_config cfg;
    size_t node_count;    /* usable node cache slots */
    uint8_t *scratch;     /* one page of working space, used by many internal steps */
    uint8_t *page2;       /* second scratch page, used by compaction */
    uint64_t chain_pages[2 * CDB_PAGER_CHAIN_MAX]; /* pages reserved for the two free-space chains */
    size_t page_size;
    cdb_page_format fmt;
    cdb_byte_order order;
    unsigned checksum_alg;
    unsigned addr_width;  /* 4 or 8 bytes */
    uint64_t data_start;  /* first physical page after the header and location 0 */
    uint64_t append_point; /* first physical page past the end of the used file */
    uint64_t next_seq;    /* next page sequence number to hand out */
    uint64_t seq_hwm;     /* sequences below this are durably reserved (spec 12.1) */
    uint64_t seq_at_commit;  /* next_seq after the last commit or open */
    uint64_t next_lid;    /* next logical page ID to hand out; IDs start at 1 */
    unsigned depth;       /* levels in the anchor map, 1 to 8 */
    uint64_t root_primary; /* physical pages of the anchor-map root; 0 = not written yet */
    uint64_t root_mirror;
    uint64_t free_root;   /* free-space chains as of the last commit */
    uint64_t free_mirror;
    size_t free_len;      /* extents in cfg.free_list */
    size_t leaf_fan, inner_fan, root_leaf_fan, root_inner_fan; /* entries per node kind */
    uint64_t lru_clock;
    int read_only;        /* the file has a feature this build can only read */
    int dirty;            /* anything to commit */
    /* Catalog root slots at location 0 (spec 14.3). */
    uint64_t loc0;        /* byte offset of location 0 */
    cdb_catroot catroot;  /* the root in force; zero if no catalog yet */
    int cat_exists;       /* a catalog root has been committed */
    int cat_dirty;        /* catroot changed; the next commit writes a slot */
    unsigned cat_next_slot; /* catalog root slot (0 or 1) the next write goes to */
    /* Sharing the archive with other processes (spec 5.9). Kept by
     * cdb_pager_reload; cleared by open and create. */
    int private_mode;     /* writes go to the overlay; commits are refused */
    cdb_storage overlay;  /* the overlay file, while private_mode */
    uint64_t ov_next;     /* next overlay page number; 0 means "none" */
    uint64_t reuse_below; /* space freed at this sequence or later is not reused; 0: no limit */
    cdb_status (*guard)(void *ctx); /* checked before every archive write; NULL: none */
    void *guard_ctx;
} cdb_pager;

/* Creates a new database on empty storage. h supplies the file settings:
 * byte order, page size (default_page_log2, used for every page), address
 * width (CDB_LAYOUT_ADDR64), checksum algorithm and database ID.
 * The storage is copied by value; cfg's buffers must outlive the pager.
 * CDB_E_INVALID_ARG for a page size outside 512 B..64 KiB or buffers too
 * small. The new file is open on return, as after cdb_pager_open. */
cdb_status cdb_pager_create(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg,
                            const cdb_header *h);

/* Opens an existing database. Same ownership rules as cdb_pager_create.
 * CDB_E_CORRUPT when the anchor map root, the free-space list (both chains)
 * or both catalog root slots are damaged; CDB_E_UNSUPPORTED for files using
 * features this build does not handle. There is no close function: the
 * pager holds no resources of its own. */
cdb_status cdb_pager_open(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg);

/* Allocates a new logical page ID. IDs are never reused (spec 4.12).
 * CDB_E_OVERFLOW when the ID space of the file's format is exhausted. */
cdb_status cdb_pager_alloc(cdb_pager *p, uint64_t *lid);

/* Writes a new version of logical page lid: seals it with a fresh sequence
 * number and appends it. tmpl supplies type, store and domain.
 * The first write after a commit goes to a new physical page (copy-on-write);
 * later writes before the next commit overwrite that page in place.
 * The payload (len bytes, at most one page's capacity) is copied. */
cdb_status cdb_pager_write(cdb_pager *p, uint64_t lid, const cdb_page_header *tmpl,
                           const void *payload, size_t len);

/* Reads and verifies logical page lid into page (page_size bytes). The
 * payload starts at page + cdb_page_header_size(p->fmt).
 * CDB_E_NOT_FOUND when lid has no live page, CDB_E_CORRUPT when the page
 * fails its checksum. */
cdb_status cdb_pager_read(cdb_pager *p, uint64_t lid, uint8_t *page, cdb_page_header *h);

/* Releases logical page lid. Its ID is not reused. */
cdb_status cdb_pager_free(cdb_pager *p, uint64_t lid);

/* Returns lid's anchor entry as it stands now (committed or not). */
cdb_status cdb_pager_lookup(cdb_pager *p, uint64_t lid, cdb_anchor_entry *e);

/* Replaces the catalog root; written by the next commit (spec 3.2 step 3). */
void cdb_pager_set_catroot(cdb_pager *p, const cdb_catroot *r);

/* Commits every change since the last commit (spec 3.2 steps 1 to 4).
 * Does nothing when there is nothing to commit. If it fails before the
 * header write, the file still holds the previous commit.
 * CDB_E_BUFFER_TOO_SMALL when the free-space list needs more than
 * CDB_PAGER_CHAIN_MAX pages per chain, or the caller's free_list is full. */
cdb_status cdb_pager_commit(cdb_pager *p);

/* Space reuse and compaction (spec 1.3, 3.4, 3.7).
 *
 * A freed page is reused only once its freeing commit is older than the
 * latest commit: the other header slot may still hold the previous state,
 * which can reference pages the latest commit freed. New writes always take
 * the lowest reusable page before growing the file. */

/* Releases the free tail of the file: lowers the append point past every
 * reusable extent that reaches it, commits, then truncates the storage. Also
 * trims pages a crash left past the append point. Requires no uncommitted
 * changes. *reclaimed is the number of pages released. */
cdb_status cdb_pager_reclaim_tail(cdb_pager *p, uint64_t *reclaimed);

/* Makes all freed space reusable (with a fence commit if needed), moves up to
 * max_moves live pages down into reusable holes, commits twice (so the
 * vacated pages become reusable), then reclaims the tail. */
cdb_status cdb_pager_compact(cdb_pager *p, uint64_t max_moves, uint64_t *moved, uint64_t *reclaimed);

/* Sharing the archive (spec 5.9). Only the baton holder writes the archive
 * file; any other process keeps its changes private. */

/* With overlay non-NULL, later writes go to that file as pages in state
 * CDB_ANCHOR_PRIVATE, freed pages are not tracked, and commit, compaction
 * and tail reclaim return CDB_E_STATE. NULL writes to the archive again.
 * CDB_E_STATE with uncommitted changes: switch after a commit or a reload. */
cdb_status cdb_pager_set_private(cdb_pager *p, const cdb_storage *overlay);

/* Drops every uncommitted change, private or not, and loads the committed
 * state the archive holds now, which another process may have moved on.
 * Keeps the private mode, overlay, reuse limit and guard; the overlay starts
 * over from its first page. */
cdb_status cdb_pager_reload(cdb_pager *p);

/* Space freed by the commit with sequence f is reused only when f is below
 * seq as well as below the committed sequence: seq is the lowest committed
 * sequence another process is reading (spec 5.9.7). 0 lifts the limit. */
void cdb_pager_set_reuse_below(cdb_pager *p, uint64_t seq);

/* guard(ctx) runs before every write to the archive file, the header
 * included; an error from it stops the write and is returned (the lease
 * margin of spec 5.9.8). NULL removes it. */
void cdb_pager_set_guard(cdb_pager *p, cdb_status (*guard)(void *ctx), void *ctx);

#endif
