/* Pager: copy-on-write anchor map, append allocation, sequence reservation
 * and the free-space list (spec 1.7, 3.2, 3.5, 12.1, 14.7).
 *
 * How it fits together, for a first-time reader:
 *
 * - The rest of the engine never sees physical file positions. It works with
 *   "logical page IDs" (lids), small integers handed out by cdb_pager_alloc.
 * - The anchor map is a tree of pages that translates a lid into the
 *   physical page number where that page currently lives. Leaves hold one
 *   entry per lid; interior nodes hold the physical numbers of their children.
 * - Nothing that the last commit can still see is ever overwritten. A changed
 *   page is written to a new physical page (copy-on-write), and the old one
 *   is put on the free-space list. Only when the file header is committed
 *   does the new state become the one a reader or a recovery will see. A crash
 *   before that leaves the previous committed state intact.
 * - Every anchor-map node and the free-space list are stored twice (a
 *   "primary" and a "mirror" copy) so one damaged page cannot lose them.
 * - Anchor-map nodes are cached in memory, in a fixed set of slots supplied by
 *   the caller (no heap). A node changed since the last commit is "dirty" and
 *   must stay in the cache until the commit writes it out.
 *
 * Physical page numbers count whole pages from the start of the file. Page 0
 * and the pages holding the header slots and location 0 are never data pages,
 * so a physical number of 0 is used to mean "none". */
#include <string.h>

#include "combolite/cdb_crypto.h"
#include "cdb_endian.h"
#include "cdb_pager.h"

/* Anchor-map page payload: an 8-byte node header (level, flags), then for
 * the root a 32-byte area (next logical ID, free-space chain roots), then
 * the entries. Layout in spec 14.7.6. */
#define NODE_HDR 8u
#define ROOT_AREA 32u
#define NODE_FLAG_ROOT 0x01u
#define MAX_DEPTH 8u   /* most levels the anchor map may have; deeper trees are treated as corrupt */

/* Free-space page payload: count, reserved, next page, then extents
 * (spec 14.7.7). */
#define FREE_HDR 16u
#define MAX_CHAIN 1000000u /* loop guard: a chain longer than this is assumed to be a cycle */

#define LOCATION0_SLOTS_BYTES 1024u /* two 512-byte catalog root slots (spec 14.3.1) */

static uint64_t round_up(uint64_t v, uint64_t unit)
{
    return (v + unit - 1) / unit * unit;
}

/* -------------------------------------------------------- small helpers */

/* Physical addresses on disk are 8 bytes in a 64-bit file and 4 bytes in a
 * 32-bit or compact file (spec 14.7.2, 14.7.3). These two helpers hide that
 * difference and apply the file's byte order. */
static void store_addr(const cdb_pager *p, uint8_t *at, uint64_t v)
{
    if (p->addr_width == 8)
        cdb_store_u64(at, v, p->order);
    else
        cdb_store_u32(at, (uint32_t)v, p->order);
}

static uint64_t load_addr(const cdb_pager *p, const uint8_t *at)
{
    return p->addr_width == 8 ? cdb_load_u64(at, p->order) : cdb_load_u32(at, p->order);
}

/* Byte offset of the payload inside a page: the payload starts after the
 * common page header and the store ID slot (spec 14.2.7). */
static size_t payload_off(const cdb_pager *p)
{
    return cdb_page_payload_offset(p->fmt);
}

/* Size of one anchor-map leaf entry: the physical address, then page size
 * (1 byte), state (1 byte) and extent length (2 bytes) (spec 14.7.2). */
static size_t entry_size(const cdb_pager *p)
{
    return p->addr_width + 4u;
}

/* Nodes are named by (level, number): level 0 is the leaves, and number is
 * the node's position from the left within its level. The root is the only
 * node on the top level, so it is always number 0 there. */
static int node_is_root(const cdb_pager *p, unsigned level, uint64_t number)
{
    return level == p->depth - 1 && number == 0;
}

/* Byte offset, within a node's page image, of its first entry. The root has
 * the extra 32-byte area (next lid and free-list roots) before its entries. */
static size_t entries_off(const cdb_pager *p, int is_root)
{
    return payload_off(p) + NODE_HDR + (is_root ? ROOT_AREA : 0u);
}

/* Number of logical IDs a tree of the given depth can address. */
static uint64_t tree_capacity(const cdb_pager *p, unsigned depth)
{
    uint64_t cap;
    unsigned i;
    /* A one-level tree is a single root that is also a leaf. */
    if (depth == 1)
        return p->root_leaf_fan;
    /* Otherwise: the root's children, times each middle level's fan-out,
     * times the entries per leaf. Saturate instead of overflowing. */
    cap = (uint64_t)p->root_inner_fan * p->leaf_fan;
    for (i = 2; i < depth; i++) {
        if (cap > UINT64_MAX / p->inner_fan)
            return UINT64_MAX;
        cap *= p->inner_fan;
    }
    return cap;
}

/* Byte offset in the file of physical page number phys. */
static uint64_t phys_offset(const cdb_pager *p, uint64_t phys)
{
    return phys * (uint64_t)p->page_size;
}

/* -------------------------------------------------------- sequences */

/* Hands out the next page sequence number, first making a new reservation
 * block durable whenever the current one is used up (spec 12.1).
 *
 * Why: every page write carries a sequence number that must be unique across
 * the whole file, forever (it will feed the encryption nonce, and it is how
 * the pager tells new pages from committed ones). Recording the high-water
 * mark in the header *before* using numbers below it means that after a
 * crash, numbering resumes above anything that may already be on disk. */
static cdb_status next_seq(cdb_pager *p, uint64_t *seq)
{
    /* Block used up: durably raise the mark by one block first. */
    if (p->next_seq >= p->seq_hwm) {
        uint64_t hwm = p->next_seq + CDB_SEQ_RESERVE_BLOCK;
        cdb_status s = cdb_header_checkpoint(&p->st, &p->hs, hwm, p->hs.record.commit_hwm);
        if (s != CDB_OK)
            return s;
        p->seq_hwm = hwm;
    }
    *seq = p->next_seq++;
    return CDB_OK;
}

/* Whether space freed by the commit with sequence freed may be written now.
 * Pages freed by the latest commit stay untouched: the previous header
 * slot's state may still reference them (spec 1.9, 3.5). Nor is space reused
 * that a state another process still reads may reference (spec 5.9.7). */
static int reusable(const cdb_pager *p, uint64_t freed)
{
    return freed < p->hs.record.committed_seq && (p->reuse_below == 0 || freed < p->reuse_below);
}

/* Physical page for a new write: the lowest reusable free page, else the
 * append point. */
static uint64_t alloc_phys(cdb_pager *p)
{
    size_t i;
    /* The list is sorted by start, so the first eligible extent holds the
     * lowest reusable page. Take its first page and shrink it (or drop it
     * when it becomes empty). */
    for (i = 0; i < p->free_len; i++) {
        cdb_free_extent *x = &p->cfg.free_list[i];
        if (reusable(p, x->freed_seq)) {
            uint64_t phys = x->start;
            x->start++;
            if (--x->len == 0) {
                memmove(x, x + 1, (p->free_len - i - 1) * sizeof *x);
                p->free_len--;
            }
            return phys;
        }
    }
    /* Nothing reusable: grow the file by one page. */
    return p->append_point++;
}

void cdb_pager_set_crypto(cdb_pager *p, unsigned aead_alg, const uint8_t *master_key,
                          const uint8_t *integrity_key,
                          cdb_pager_key_fn domain_key_fn, void *domain_key_ctx)
{
    if (p == NULL)
        return;
    p->aead_alg = (uint8_t)aead_alg;
    if (master_key != NULL) {
        memcpy(p->master_key, master_key, 32);
        p->has_master_key = 1;
    } else {
        p->has_master_key = 0;
    }
    if (integrity_key != NULL) {
        memcpy(p->integrity_key, integrity_key, 32);
        p->has_integrity_key = 1;
    } else {
        p->has_integrity_key = 0;
    }
    p->domain_key_fn = domain_key_fn;
    p->domain_key_ctx = domain_key_ctx;
}

void cdb_pager_set_session(cdb_pager *p, uint16_t slot,
                           cdb_status (*salt_assign)(void *ctx, uint16_t *index),
                           cdb_status (*precommit)(void *ctx, uint64_t commit_seq, int salt_used),
                           void *ctx)
{
    p->sess.slot = slot;
    p->sess.salt_assign = salt_assign;
    p->sess.precommit = precommit;
    p->sess.ctx = ctx;
    if (p->sess.ov_seq == 0)
        p->sess.ov_seq = 1;
}

/* The nonce track of a page (spec 14.2.5): the overlay's is 0xFFFE and the
 * slot, the archive's 0xFFFFFFFF. */
static uint32_t page_track(const cdb_pager *p, int overlay)
{
    return overlay ? (0xFFFEu << 16) | (uint32_t)p->sess.slot : CDB_TRACK_ARCHIVE_PAGE;
}

/* Seals buf with h. An encrypted page names its salt (spec 14.2.8): an
 * overlay page the overlay's own, an archive page the session's index in
 * the salt table, assigned at the first such page since the last commit. */
static cdb_status pager_seal_page(cdb_pager *p, uint8_t *buf, cdb_page_header *h, int overlay)
{
    const uint8_t *key = NULL;
    uint8_t key_buf[32];
    unsigned aead_alg = CDB_AEAD_NONE;
    cdb_status s;

    h->salt_index = 0;
    if (h->domain_id > 0 && p->aead_alg != CDB_AEAD_NONE) {
        if (overlay) {
            h->salt_index = CDB_SALT_INDEX_FILE;
        } else {
            if (p->salt_index == 0) {
                if (p->sess.salt_assign == NULL)
                    return CDB_E_SECURITY;
                if ((s = p->sess.salt_assign(p->sess.ctx, &p->salt_index)) != CDB_OK)
                    return s;
                if (p->salt_index == 0 || p->salt_index == CDB_SALT_INDEX_FILE)
                    return CDB_E_STATE;
            }
            h->salt_index = p->salt_index;
            p->salt_used = 1;
        }
        if (p->domain_key_fn == NULL)
            return CDB_E_SECURITY;
        if ((s = p->domain_key_fn(p->domain_key_ctx, h->domain_id, h->key_version, h->salt_index, key_buf)) != CDB_OK)
            return s;
        key = key_buf;
        aead_alg = p->aead_alg;
    } else if ((p->hs.record.layout_flags & CDB_LAYOUT_GOVERNANCE) || (h->flags & CDB_PAGE_FLAG_TAMPER_EVIDENT)) {
        if (!p->has_integrity_key)
            return CDB_E_SECURITY;
        h->flags |= CDB_PAGE_FLAG_TAMPER_EVIDENT;
        key = p->integrity_key;
    }

    s = cdb_page_seal_crypto(buf, p->page_size, p->fmt, p->order, p->checksum_alg,
                             aead_alg, key, page_track(p, overlay), h);
    cdb_crypto_wipe(key_buf, sizeof key_buf);
    return s;
}

/* Verifies the page in buf and decrypts it when it is encrypted. The
 * header, read without verifying it first, names the domain and salt whose
 * key opens the page; only an overlay page names the overlay's salt. */
static cdb_status pager_open_page(cdb_pager *p, uint8_t *buf, cdb_page_header *h, int overlay)
{
    const uint8_t *key = NULL;
    uint8_t key_buf[32];
    unsigned aead_alg = CDB_AEAD_NONE;
    cdb_status s;

    if ((s = cdb_page_peek(buf, p->page_size, p->fmt, p->order, h)) != CDB_OK)
        return s;
    if (h->domain_id > 0 && p->aead_alg != CDB_AEAD_NONE) {
        if (h->salt_index == 0 || (h->salt_index == CDB_SALT_INDEX_FILE) != (overlay != 0))
            return CDB_E_MALFORMED;
        if (p->domain_key_fn == NULL)
            return CDB_E_SECURITY;
        if ((s = p->domain_key_fn(p->domain_key_ctx, h->domain_id, h->key_version, h->salt_index, key_buf)) != CDB_OK)
            return s;
        key = key_buf;
        aead_alg = p->aead_alg;
    } else if (h->flags & CDB_PAGE_FLAG_TAMPER_EVIDENT) {
        if (!p->has_integrity_key)
            return CDB_E_SECURITY;
        key = p->integrity_key;
    }

    s = cdb_page_open_crypto(buf, p->page_size, p->fmt, p->order, p->checksum_alg,
                             aead_alg, key, page_track(p, overlay), h);
    cdb_crypto_wipe(key_buf, sizeof key_buf);
    return s;
}

/* Seals the image in buf with a fresh sequence number and writes it at phys.
 * buf must be a full page image with the payload already in place; the
 * header and checksum trailer are filled in here. h->write_seq is set. */
static cdb_status write_page_at(cdb_pager *p, uint8_t *buf, cdb_page_header *h, uint64_t phys)
{
    cdb_status s;
    if (p->guard != NULL && (s = p->guard(p->guard_ctx)) != CDB_OK)
        return s;
    if ((s = next_seq(p, &h->write_seq)) != CDB_OK)
        return s;
    if ((s = pager_seal_page(p, buf, h, 0)) != CDB_OK)
        return s;
    return cdb_storage_program(&p->st, phys_offset(p, phys), buf, p->page_size);
}

/* Seals buf and writes it at page phys of the overlay (spec 5.9.1). The
 * sequence number comes from the session's overlay counter, which no
 * rebase resets: the overlay's nonces never repeat under its salt (14.2.5). */
static cdb_status write_overlay(cdb_pager *p, uint8_t *buf, cdb_page_header *h, uint64_t phys)
{
    cdb_status s;
    if (p->sess.ov_seq == 0)
        p->sess.ov_seq = 1;
    h->write_seq = p->sess.ov_seq++;
    if ((s = pager_seal_page(p, buf, h, 1)) != CDB_OK)
        return s;
    return cdb_storage_program(&p->overlay, phys_offset(p, phys), buf, p->page_size);
}

/* Writes buf to a newly allocated physical page and reports where in *phys. */
static cdb_status append_page(cdb_pager *p, uint8_t *buf, cdb_page_header *h, uint64_t *phys)
{
    *phys = alloc_phys(p);
    return write_page_at(p, buf, h, *phys);
}

/* Reads physical page phys into buf (page_size bytes), verifies its checksum
 * and checks that it has the expected page type. A page in the reserved area
 * at the start of the file, a page past the end of the file, a bad checksum
 * or the wrong type all report CDB_E_CORRUPT: a structure pointed at it. */
static cdb_status read_page(cdb_pager *p, uint64_t phys, uint8_t *buf, uint16_t type, cdb_page_header *h)
{
    cdb_status s;
    if (phys < p->data_start)
        return CDB_E_CORRUPT;
    if ((s = cdb_storage_read(&p->st, phys_offset(p, phys), buf, p->page_size)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if ((s = pager_open_page(p, buf, h, 0)) != CDB_OK)
        return s;
    return h->type == type && h->payload_len == cdb_page_capacity(p->fmt, p->page_size) ? CDB_OK : CDB_E_CORRUPT;
}

/* -------------------------------------------------------- free list */

/* Records [start, start+len) as freed by the next commit. The list stays
 * sorted by start. Touching extents merge only when they were freed by the
 * same commit, so no page is ever held back longer than its own freeing
 * commit requires.
 *
 * The extent is tagged with the sequence the *next* commit will have, which
 * is the commit that makes the page unreferenced. Errors: CDB_E_CORRUPT if
 * the range overlaps space already on the list (a double free), and
 * CDB_E_BUFFER_TOO_SMALL if the caller's free_list array is full. */
static cdb_status free_push(cdb_pager *p, uint64_t start, uint64_t len)
{
    uint64_t freed = p->hs.record.committed_seq + 1;
    cdb_free_extent *list = p->cfg.free_list;
    size_t i = 0;

    /* Physical page 0 means "no page", so there is nothing to free. */
    if (start == 0 || len == 0)
        return CDB_OK;
    /* Find the insertion point: the first extent starting at or after start. */
    while (i < p->free_len && list[i].start < start)
        i++;
    if ((i > 0 && list[i - 1].start + list[i - 1].len > start)
        || (i < p->free_len && start + len > list[i].start))
        return CDB_E_CORRUPT; /* freeing space that is already free */
    /* Extend the extent just before, if it ends exactly where this starts and
     * was freed by the same commit; then try to absorb the one after too. */
    if (i > 0 && list[i - 1].freed_seq == freed && list[i - 1].start + list[i - 1].len == start) {
        list[i - 1].len += len;
        if (i < p->free_len && list[i].freed_seq == freed && list[i - 1].start + list[i - 1].len == list[i].start) {
            list[i - 1].len += list[i].len;
            memmove(&list[i], &list[i + 1], (p->free_len - i - 1) * sizeof *list);
            p->free_len--;
        }
        return CDB_OK;
    }
    /* Extend the extent just after backwards, under the same condition. */
    if (i < p->free_len && list[i].freed_seq == freed && start + len == list[i].start) {
        list[i].start = start;
        list[i].len += len;
        return CDB_OK;
    }
    /* No merge possible: insert a new extent at position i. */
    if (p->free_len >= p->cfg.free_cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memmove(&list[i + 1], &list[i], (p->free_len - i) * sizeof *list);
    list[i].start = start;
    list[i].len = len;
    list[i].freed_seq = freed;
    p->free_len++;
    return CDB_OK;
}

/* Extents that are reusable now are reusable for good: mark them 0
 * ("reusable") and merge touching extents that share a sequence
 * (spec 14.7.4). This keeps the list short. Compacts the list in place. */
static void free_normalize(cdb_pager *p)
{
    cdb_free_extent *list = p->cfg.free_list;
    size_t i, out = 0;
    for (i = 0; i < p->free_len; i++) {
        cdb_free_extent x = list[i];
        if (reusable(p, x.freed_seq))
            x.freed_seq = 0;
        if (out > 0 && list[out - 1].freed_seq == x.freed_seq
            && list[out - 1].start + list[out - 1].len == x.start)
            list[out - 1].len += x.len;
        else
            list[out++] = x;
    }
    p->free_len = out;
}

/* One stored extent: start and length (address width each), then the 8-byte
 * freeing sequence (spec 14.7.4). */
static size_t free_entry_size(const cdb_pager *p)
{
    return 2u * p->addr_width + 8u;
}

/* How many extents fit on one free-space page. */
static size_t free_per_page(const cdb_pager *p)
{
    return (cdb_page_capacity(p->fmt, p->page_size) - FREE_HDR) / free_entry_size(p);
}

/* Loads the chain rooted at root into the in-memory list. Each page is
 * verified; any bad page fails the whole load with CDB_E_CORRUPT, so the
 * caller can fall back to the mirror chain. root 0 means an empty list.
 * Uses p->scratch. */
static cdb_status load_free_chain(cdb_pager *p, uint64_t root)
{
    uint64_t phys = root;
    unsigned guard = 0;
    p->free_len = 0;
    while (phys != 0) {
        cdb_page_header h;
        const uint8_t *pl;
        uint32_t count, i;
        cdb_status s;
        if (++guard > MAX_CHAIN)
            return CDB_E_CORRUPT;
        if ((s = read_page(p, phys, p->scratch, CDB_PAGE_TYPE_FREE_SPACE, &h)) != CDB_OK)
            return s;
        pl = p->scratch + payload_off(p);
        count = cdb_load_u32(pl, p->order);
        if (count > free_per_page(p))
            return CDB_E_CORRUPT;
        for (i = 0; i < count; i++) {
            const uint8_t *e = pl + FREE_HDR + i * free_entry_size(p);
            if (p->free_len >= p->cfg.free_cap)
                return CDB_E_BUFFER_TOO_SMALL;
            p->cfg.free_list[p->free_len].start = load_addr(p, e);
            p->cfg.free_list[p->free_len].len = load_addr(p, e + p->addr_width);
            p->cfg.free_list[p->free_len].freed_seq = cdb_load_u64(e + 2u * p->addr_width, p->order);
            p->free_len++;
        }
        /* The next-page link is always 8 bytes, even in a 32-bit file. */
        phys = cdb_load_u64(pl + 8, p->order);
    }
    return CDB_OK;
}

/* The pages of the previous chain are freed by this commit. A damaged chain
 * stops the walk; its remaining pages are simply not reclaimed. Losing a few
 * pages of space is preferred to failing the commit. Uses p->scratch. */
static cdb_status free_old_chain(cdb_pager *p, uint64_t root)
{
    uint64_t phys = root;
    unsigned guard = 0;
    while (phys != 0 && ++guard <= MAX_CHAIN) {
        cdb_page_header h;
        uint64_t next;
        cdb_status s;
        if (read_page(p, phys, p->scratch, CDB_PAGE_TYPE_FREE_SPACE, &h) != CDB_OK)
            break;
        next = cdb_load_u64(p->scratch + payload_off(p) + 8, p->order);
        if ((s = free_push(p, phys, 1)) != CDB_OK)
            return s;
        phys = next;
    }
    return CDB_OK;
}

/* Writes the list as two chains, primary and mirror. All chain pages are
 * allocated first; taking them can only shorten the list, so it then fits.
 * Spare pages are written as empty links and freed by the next commit.
 * (spec 14.7.7, 14.7.8.) On success p->free_root and p->free_mirror name the
 * first page of each chain, or 0 when the list is empty. Uses p->scratch. */
static cdb_status write_free_chains(cdb_pager *p)
{
    size_t per = free_per_page(p);
    size_t n = (p->free_len + per - 1) / per; /* pages per chain, from the current length */
    size_t copy, i;

    if (n > CDB_PAGER_CHAIN_MAX)
        return CDB_E_BUFFER_TOO_SMALL; /* compact to shorten the list */
    /* Reserve every chain page up front (n for the primary, n for the
     * mirror). These allocations may consume extents, which is why the
     * extents are only written out afterwards. */
    for (i = 0; i < 2 * n; i++)
        p->chain_pages[i] = alloc_phys(p);
    p->free_root = 0;
    p->free_mirror = 0;
    /* Write the same list twice: copy 0 is the primary chain, copy 1 the
     * mirror. Each page links to the next page of its own chain. */
    for (copy = 0; copy < 2 && n > 0; copy++) {
        const uint64_t *pages = p->chain_pages + copy * n;
        for (i = 0; i < n; i++) {
            /* Slice of the list for page i; may be empty (a spare page). */
            size_t first = i * per;
            size_t count = first >= p->free_len ? 0 : (p->free_len - first < per ? p->free_len - first : per);
            uint8_t *pl = p->scratch + payload_off(p);
            cdb_page_header h;
            size_t k;
            cdb_status s;

            memset(p->scratch, 0, p->page_size);
            cdb_store_u32(pl, (uint32_t)count, p->order);
            cdb_store_u64(pl + 8, i + 1 < n ? pages[i + 1] : 0, p->order);
            for (k = 0; k < count; k++) {
                const cdb_free_extent *x = &p->cfg.free_list[first + k];
                uint8_t *e = pl + FREE_HDR + k * free_entry_size(p);
                store_addr(p, e, x->start);
                store_addr(p, e + p->addr_width, x->len);
                cdb_store_u64(e + 2u * p->addr_width, x->freed_seq, p->order);
            }
            memset(&h, 0, sizeof h);
            h.type = CDB_PAGE_TYPE_FREE_SPACE;
            h.payload_len = (uint32_t)cdb_page_capacity(p->fmt, p->page_size);
            if ((s = write_page_at(p, p->scratch, &h, pages[i])) != CDB_OK)
                return s;
        }
        if (copy == 0)
            p->free_root = pages[0];
        else
            p->free_mirror = pages[0];
    }
    return CDB_OK;
}

/* -------------------------------------------------------- node cache */

/* Returns the cached node (level, number), or NULL if it is not cached. */
static cdb_pager_node *find_node(cdb_pager *p, unsigned level, uint64_t number)
{
    size_t i;
    for (i = 0; i < p->node_count; i++) {
        cdb_pager_node *n = &p->cfg.nodes[i];
        if (n->in_use && n->level == level && n->number == number)
            return n;
    }
    return NULL;
}

/* Picks a cache slot for a node about to be loaded: an empty slot if there
 * is one, else the least recently used clean node, which is simply dropped
 * (its pages on disk are still current). Dirty nodes are never evicted
 * because their changes exist only in memory until the next commit. */
static cdb_status take_slot(cdb_pager *p, cdb_pager_node **out)
{
    cdb_pager_node *victim = NULL;
    size_t i;
    for (i = 0; i < p->node_count; i++) {
        cdb_pager_node *n = &p->cfg.nodes[i];
        if (!n->in_use) {
            victim = n;
            break;
        }
        if (!n->dirty && (victim == NULL || n->lru < victim->lru))
            victim = n;
    }
    if (victim == NULL)
        return CDB_E_BUFFER_TOO_SMALL; /* every cached node is dirty: commit first */
    victim->in_use = 0;
    victim->dirty = 0;
    *out = victim;
    return CDB_OK;
}

/* Fills both images of a node that has never been written: all entries zero
 * (state "unused"), with only the node header (level, root flag) set. */
static void init_empty_node(const cdb_pager *p, cdb_pager_node *n)
{
    int root = node_is_root(p, n->level, n->number);
    memset(n->primary, 0, p->page_size);
    n->primary[payload_off(p)] = (uint8_t)n->level;
    n->primary[payload_off(p) + 1] = root ? (uint8_t)NODE_FLAG_ROOT : 0u;
    memcpy(n->mirror, n->primary, p->page_size);
}

/* Whether a verified anchor-map page is the node we expected: same level,
 * and the root flag set exactly when it is the root. Guards against a
 * pointer that leads to a valid page of the wrong node. */
static int node_page_ok(const cdb_pager *p, const uint8_t *img, unsigned level, int root)
{
    const uint8_t *nh = img + payload_off(p);
    return nh[0] == level && (nh[1] & NODE_FLAG_ROOT) == (root ? NODE_FLAG_ROOT : 0u);
}

/* Loads both copies of a node. A damaged copy is replaced by the other
 * (spec 3.5, 14.7.8). CDB_E_CORRUPT only when neither copy is usable.
 * n->phys_primary and n->phys_mirror must already be set. */
static cdb_status load_node(cdb_pager *p, cdb_pager_node *n)
{
    int root = node_is_root(p, n->level, n->number);
    cdb_page_header h;
    int ok_p = 0, ok_m = 0;

    if (n->phys_primary && read_page(p, n->phys_primary, n->primary, CDB_PAGE_TYPE_ANCHOR_MAP, &h) == CDB_OK)
        ok_p = node_page_ok(p, n->primary, n->level, root);
    /* A good primary leaf is enough: the mirror leaf has the same entries,
     * so copy instead of paying for a second read. Interior nodes differ
     * between copies (each points into its own tree), so both are read. */
    if (n->level == 0 && ok_p) {
        memcpy(n->mirror, n->primary, p->page_size); /* leaf copies hold the same entries */
        return CDB_OK;
    }
    if (n->phys_mirror && read_page(p, n->phys_mirror, n->mirror, CDB_PAGE_TYPE_ANCHOR_MAP, &h) == CDB_OK)
        ok_m = node_page_ok(p, n->mirror, n->level, root);
    if (!ok_p && !ok_m)
        return CDB_E_CORRUPT;
    /* Repair the bad copy in memory from the good one. For an interior node
     * this means both images now point at the same set of children. */
    if (!ok_p)
        memcpy(n->primary, n->mirror, p->page_size);
    else if (!ok_m)
        memcpy(n->mirror, n->primary, p->page_size);
    return CDB_OK;
}

/* Returns node (level, number), loading it into the cache if needed, and
 * marks it most recently used. Loading a non-root node first loads its
 * parent (recursively, up to the root) to find the child's two physical
 * pages. A node whose pointers are both 0 has never been written and starts
 * out empty. The returned pointer stays valid until the slot is evicted,
 * which can happen on any later get_node call unless the node is dirty. */
static cdb_status get_node(cdb_pager *p, unsigned level, uint64_t number, cdb_pager_node **out)
{
    cdb_pager_node *n = find_node(p, level, number);
    uint64_t phys_p, phys_m;
    cdb_status s;

    if (n == NULL) {
        /* Where are this node's two copies? The root's come from the file
         * header; any other node's come from its parent's entry. */
        if (level == p->depth - 1) {
            phys_p = p->root_primary;
            phys_m = p->root_mirror;
        } else {
            cdb_pager_node *parent;
            size_t at;
            /* Node `number` is child (number % inner_fan) of parent
             * (number / inner_fan) on the level above. */
            if ((s = get_node(p, level + 1, number / p->inner_fan, &parent)) != CDB_OK)
                return s;
            at = entries_off(p, node_is_root(p, level + 1, number / p->inner_fan))
               + (size_t)(number % p->inner_fan) * p->addr_width;
            phys_p = load_addr(p, parent->primary + at);
            phys_m = load_addr(p, parent->mirror + at);
        }
        /* Copy the addresses out of the parent before taking a slot: taking
         * a slot may evict the (clean) parent. */
        if ((s = take_slot(p, &n)) != CDB_OK)
            return s;
        n->level = level;
        n->number = number;
        n->phys_primary = phys_p;
        n->phys_mirror = phys_m;
        if (phys_p == 0 && phys_m == 0) {
            init_empty_node(p, n);
        } else if ((s = load_node(p, n)) != CDB_OK) {
            return s;
        }
        n->in_use = 1;
    }
    n->lru = ++p->lru_clock;
    *out = n;
    return CDB_OK;
}

/* Makes sure n and every ancestor are in the cache at the same time, without
 * leaving clean nodes marked dirty if a load or a later step fails. Once
 * this succeeds, mark_dirty(p, n) finds every ancestor cached and cannot
 * fail. */
static cdb_status load_ancestors(cdb_pager *p, cdb_pager_node *n)
{
    cdb_pager_node *path[MAX_DEPTH];
    uint8_t was_dirty[MAX_DEPTH];
    unsigned count = 0, i;
    unsigned level = n->level;
    uint64_t number = n->number;
    cdb_status s = CDB_OK;

    path[count] = n;
    was_dirty[count] = (uint8_t)n->dirty;
    n->dirty = 1;
    count++;
    while (level + 1 < p->depth) {
        cdb_pager_node *parent;
        level++;
        number /= p->inner_fan;
        if ((s = get_node(p, level, number, &parent)) != CDB_OK)
            break;
        path[count] = parent;
        was_dirty[count] = (uint8_t)parent->dirty;
        parent->dirty = 1;
        count++;
    }
    for (i = 0; i < count; i++)
        path[i]->dirty = was_dirty[i];
    return s;
}

/* Marks a node and every ancestor dirty. Dirty nodes are never evicted.
 * Ancestors must be dirty too: rewriting a node moves it to new physical
 * pages, so its parent's pointer changes, and so on up to the root. The
 * commit relies on this (see flush_node). Also sets p->dirty. */
static cdb_status mark_dirty(cdb_pager *p, cdb_pager_node *n)
{
    unsigned level = n->level;
    uint64_t number = n->number;
    cdb_status s;

    if ((s = load_ancestors(p, n)) != CDB_OK)
        return s;
    n->dirty = 1;
    while (level + 1 < p->depth) {
        cdb_pager_node *parent;
        level++;
        number /= p->inner_fan;
        if ((s = get_node(p, level, number, &parent)) != CDB_OK)
            return s;
        parent->dirty = 1;
    }
    p->dirty = 1;
    return CDB_OK;
}

/* Which leaf node holds lid's entry, and at which slot in it. The anchor map
 * is indexed directly by lid (spec 14.7.1): no search is needed. */
static void entry_locate(const cdb_pager *p, uint64_t lid, uint64_t *leaf, size_t *slot)
{
    if (p->depth == 1) {
        *leaf = 0;
        *slot = (size_t)lid;
    } else {
        *leaf = lid / p->leaf_fan;
        *slot = (size_t)(lid % p->leaf_fan);
    }
}

/* Address of a slot's entry inside a leaf's page image. */
static uint8_t *entry_at(const cdb_pager *p, uint8_t *img, uint64_t leaf, size_t slot)
{
    return img + entries_off(p, node_is_root(p, 0, leaf)) + slot * entry_size(p);
}

/* Converts a leaf entry between its on-disk bytes (spec 14.7.2) and the
 * in-memory struct. */
static void entry_decode(const cdb_pager *p, const uint8_t *e, cdb_anchor_entry *out)
{
    out->phys = load_addr(p, e);
    out->size_log2 = e[p->addr_width];
    out->state = e[p->addr_width + 1];
    out->extent_len = cdb_load_u16(e + p->addr_width + 2, p->order);
}

static void entry_encode(const cdb_pager *p, uint8_t *e, const cdb_anchor_entry *in)
{
    store_addr(p, e, in->phys);
    e[p->addr_width] = in->size_log2;
    e[p->addr_width + 1] = in->state;
    cdb_store_u16(e + p->addr_width + 2, in->extent_len, p->order);
}

/* Loads the leaf that holds lid. CDB_E_NOT_FOUND for lid 0 (never issued)
 * and for any lid not yet allocated. */
static cdb_status leaf_for(cdb_pager *p, uint64_t lid, cdb_pager_node **leaf_node, size_t *slot)
{
    uint64_t leaf;
    if (lid == 0 || lid >= p->next_lid)
        return CDB_E_NOT_FOUND;
    entry_locate(p, lid, &leaf, slot);
    return get_node(p, 0, leaf, leaf_node);
}

/* Reads lid's anchor entry; also returns its leaf and slot so the caller can
 * update the entry with entry_set. */
static cdb_status entry_get(cdb_pager *p, uint64_t lid, cdb_pager_node **leaf, size_t *slot, cdb_anchor_entry *e)
{
    cdb_status s = leaf_for(p, lid, leaf, slot);
    if (s != CDB_OK)
        return s;
    entry_decode(p, entry_at(p, (*leaf)->primary, (*leaf)->number, *slot), e);
    return CDB_OK;
}

/* Writes an entry into both copies of the leaf (they always hold the same
 * entries) and marks the leaf and its ancestors dirty. Only memory changes
 * here; the next commit writes the nodes out. */
static cdb_status entry_set(cdb_pager *p, cdb_pager_node *leaf, size_t slot, const cdb_anchor_entry *e)
{
    entry_encode(p, entry_at(p, leaf->primary, leaf->number, slot), e);
    entry_encode(p, entry_at(p, leaf->mirror, leaf->number, slot), e);
    return mark_dirty(p, leaf);
}

/* Adds a level: the old root becomes child 0 of a new, empty root
 * (spec 14.7.6). Only the cache changes; the next commit writes both nodes.
 * CDB_E_OVERFLOW if the tree is already MAX_DEPTH levels deep. */
static cdb_status grow(cdb_pager *p)
{
    cdb_pager_node *old_root, *new_root;
    size_t from, to, len;
    int was_dirty;
    cdb_status s;

    if (p->depth >= MAX_DEPTH)
        return CDB_E_OVERFLOW;
    if ((s = get_node(p, p->depth - 1, 0, &old_root)) != CDB_OK)
        return s;
    was_dirty = old_root->dirty;
    old_root->dirty = 1; /* pin it while the new root's slot is taken */
    if ((s = take_slot(p, &new_root)) != CDB_OK) {
        old_root->dirty = was_dirty;
        return s;
    }
    /* The old root is about to become an ordinary node, which has no 32-byte
     * root area. Slide its entries down to where an ordinary node keeps
     * them, zero the freed bytes at the end, and clear its root flag. */
    from = entries_off(p, 1);
    to = entries_off(p, 0);
    len = p->page_size - cdb_page_trailer_size(p->fmt) - from;
    memmove(old_root->primary + to, old_root->primary + from, len);
    memmove(old_root->mirror + to, old_root->mirror + from, len);
    memset(old_root->primary + to + len, 0, from - to);
    memset(old_root->mirror + to + len, 0, from - to);
    old_root->primary[payload_off(p) + 1] = 0;
    old_root->mirror[payload_off(p) + 1] = 0;

    /* With the depth raised, the old root is node (depth - 2, 0), child 0 of
     * the new root. Set up the new root in the cache, pointing its first
     * child slot at the old root's current pages in each copy's tree. */
    p->depth++;
    new_root->level = p->depth - 1;
    new_root->number = 0;
    new_root->phys_primary = 0;
    new_root->phys_mirror = 0;
    init_empty_node(p, new_root);
    store_addr(p, new_root->primary + entries_off(p, 1), old_root->phys_primary);
    store_addr(p, new_root->mirror + entries_off(p, 1), old_root->phys_mirror);
    new_root->in_use = 1;
    new_root->dirty = 1;
    new_root->lru = ++p->lru_clock;
    /* The new root has no pages on disk yet; the commit will place it. */
    p->root_primary = 0;
    p->root_mirror = 0;
    p->dirty = 1;
    return CDB_OK;
}

/* -------------------------------------------------------- open/create */

/* Derives the pager's geometry from the committed header record (already in
 * p->hs) and carves the caller's arena into node images and scratch pages.
 * Errors: CDB_E_UNSUPPORTED for settings this build cannot handle yet
 * (several page sizes, a checksum other than CRC32C, a page too small to
 * hold the node and free-list headers); CDB_E_INVALID_ARG when the caller's
 * buffers are missing or too small for MAX_DEPTH + 2 cached nodes. */
static cdb_status setup(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg)
{
    const cdb_header *r = &p->hs.record;
    size_t cap, per_node;
    uint64_t loc0;

    p->st = *st;
    p->cfg = *cfg;
    /* Page format and address width follow from the header's layout flags
     * (spec 14.1.4, 14.2). */
    p->order = r->order;
    p->page_size = (size_t)1 << r->default_page_log2;
    p->checksum_alg = r->checksum_alg;
    if (r->layout_flags & CDB_LAYOUT_COMPACT_HEADER) {
        p->fmt = CDB_PAGE_FMT_COMPACT;
        p->addr_width = 4;
    } else if (r->layout_flags & CDB_LAYOUT_ADDR64) {
        p->fmt = CDB_PAGE_FMT_FULL64;
        p->addr_width = 8;
    } else {
        p->fmt = CDB_PAGE_FMT_FULL32;
        p->addr_width = 4;
    }
    if (r->min_page_log2 != r->default_page_log2 || r->max_page_log2 != r->default_page_log2
        || r->page_mode != 0)
        return CDB_E_UNSUPPORTED; /* per-store page sizes come later */
    if (r->checksum_alg != CDB_CHECKSUM_CRC32C)
        return CDB_E_UNSUPPORTED;
    cap = cdb_page_capacity(p->fmt, p->page_size);
    if (cap <= NODE_HDR + ROOT_AREA + FREE_HDR)
        return CDB_E_UNSUPPORTED;

    /* Fan-outs: how many entries fit in a leaf and in an interior node. The
     * root holds fewer because of its 32-byte area (spec 14.7.6). */
    p->leaf_fan = (cap - NODE_HDR) / entry_size(p);
    p->inner_fan = (cap - NODE_HDR) / p->addr_width;
    p->root_leaf_fan = (cap - NODE_HDR - ROOT_AREA) / entry_size(p);
    p->root_inner_fan = (cap - NODE_HDR - ROOT_AREA) / p->addr_width;

    /* Header slots, then location 0 (the catalog root slots), then data. */
    loc0 = round_up(CDB_HEADER_SLOTS * CDB_HEADER_SLOT_SIZE, p->page_size);
    p->loc0 = loc0;
    p->data_start = round_up(loc0 + LOCATION0_SLOTS_BYTES, p->page_size) / p->page_size;

    /* Arena layout: node_count pairs of (primary, mirror) page images, then
     * two scratch pages. Every node slot gets its fixed share. */
    per_node = 2u * p->page_size;
    if (cfg->arena == NULL || cfg->nodes == NULL || cfg->free_list == NULL
        || cfg->arena_size < per_node + 2u * p->page_size)
        return CDB_E_INVALID_ARG;
    p->node_count = (cfg->arena_size - 2u * p->page_size) / per_node;
    if (p->node_count > cfg->node_cap)
        p->node_count = cfg->node_cap;
    /* Enough slots for one full root-to-leaf path at maximum depth, plus
     * room for the node being loaded. */
    if (p->node_count < MAX_DEPTH + 2u)
        return CDB_E_INVALID_ARG;
    {
        size_t i;
        for (i = 0; i < p->node_count; i++) {
            memset(&p->cfg.nodes[i], 0, sizeof p->cfg.nodes[i]);
            p->cfg.nodes[i].primary = cfg->arena + i * per_node;
            p->cfg.nodes[i].mirror = cfg->arena + i * per_node + p->page_size;
        }
    }
    p->scratch = cfg->arena + p->node_count * per_node;
    p->page2 = p->scratch + p->page_size;

    /* Optional data page cache in caller memory. */
    p->cache_slots = NULL;
    p->cache_count = 0;
    p->cache_sets = 0;
    p->cache_clock = 0;
    if (cfg->page_cache != NULL && cfg->page_cache_size >= sizeof(cdb_page_cache_slot) + p->page_size) {
        size_t slot_bytes = sizeof(cdb_page_cache_slot) + p->page_size;
        size_t n_pages = cfg->page_cache_size / slot_bytes;
        if (n_pages > 0) {
            size_t i;
            uint8_t *page_mem = cfg->page_cache + n_pages * sizeof(cdb_page_cache_slot);
            p->cache_slots = (cdb_page_cache_slot *)(void *)cfg->page_cache;
            p->cache_count = n_pages;
            p->cache_sets = n_pages / 4u;
            if (p->cache_sets == 0)
                p->cache_sets = 1u;
            for (i = 0; i < n_pages; i++) {
                memset(&p->cache_slots[i], 0, sizeof p->cache_slots[i]);
                p->cache_slots[i].page = page_mem + i * p->page_size;
            }
        }
    }

    return CDB_OK;
}

/* Chooses the catalog root (spec 14.3.1): the newest valid slot whose commit
 * sequence is not above the header's. A slot written by a commit that never
 * completed is ignored (C-R4).
 *
 * Besides choosing the root, this works out which slot the next catalog
 * write goes to (the one not written most recently, judged by write
 * counter), and keeps the highest write counter seen so counters keep rising
 * even when the newest slot was ignored. */
static cdb_status load_catroot(cdb_pager *p)
{
    uint64_t committed = p->hs.record.committed_seq;
    uint64_t newest_counter = 0;
    unsigned i, newest = 1, corrupt = 0;
    int have = 0;

    for (i = 0; i < 2; i++) {
        /* A slot beyond the end of a short file, or one never written, is
         * simply absent. A slot that fails verification is counted. */
        uint8_t img[CDB_CATROOT_SLOT_SIZE];
        cdb_catroot r;
        cdb_status s = cdb_storage_read(&p->st, p->loc0 + i * CDB_CATROOT_SLOT_SIZE, img, sizeof img);
        if (s == CDB_E_TRUNCATED)
            continue;
        if (s != CDB_OK)
            return s;
        s = cdb_catroot_decode(img, &r, p->order);
        if (s == CDB_E_NOT_FOUND)
            continue;
        if (s != CDB_OK) {
            corrupt++;
            continue;
        }
        if (r.write_counter >= newest_counter) {
            newest_counter = r.write_counter;
            newest = i;
        }
        /* Written by a commit whose header never landed: not in force. */
        if (r.commit_seq > committed)
            continue;
        /* Prefer the higher commit sequence, then the higher write counter. */
        if (!have || r.commit_seq > p->catroot.commit_seq
            || (r.commit_seq == p->catroot.commit_seq && r.write_counter > p->catroot.write_counter)) {
            p->catroot = r;
            have = 1;
        }
    }
    /* Both slots damaged: the catalog is lost (spec 3.5). One damaged slot
     * with nothing else written is a first catalog commit cut short. */
    if (!have && corrupt == 2)
        return CDB_E_CORRUPT;
    p->cat_exists = have;
    if (!have)
        memset(&p->catroot, 0, sizeof p->catroot);
    p->cat_next_slot = (newest + 1u) % 2u;
    if (newest_counter > p->catroot.write_counter)
        p->catroot.write_counter = newest_counter;
    return CDB_OK;
}

/* The write counter is owned by the pager (it must keep rising across both
 * slots), so the caller's value is ignored and the current one kept. */
void cdb_pager_set_catroot(cdb_pager *p, const cdb_catroot *r)
{
    uint64_t counter = p->catroot.write_counter;
    p->catroot = *r;
    p->catroot.write_counter = counter;
    p->cat_dirty = 1;
    p->dirty = 1;
}

/* Writes the catalog root slot for the commit in progress (spec 3.2 step 3).
 * Normally one slot: the one not written most recently, so the other slot
 * still holds the root of the previous commit if this write is torn. The
 * very first catalog commit writes both slots so neither is left empty.
 * The slot is stamped with the sequence the header commit is about to get;
 * until that header lands, load_catroot ignores it. */
static cdb_status write_catroot(cdb_pager *p)
{
    uint8_t img[CDB_CATROOT_SLOT_SIZE];
    unsigned copies = p->cat_exists ? 1u : 2u, i;
    cdb_status s;

    p->catroot.commit_seq = p->hs.record.committed_seq + 1;
    for (i = 0; i < copies; i++) {
        p->catroot.write_counter++;
        p->catroot.slot_index = (uint8_t)p->cat_next_slot;
        cdb_catroot_encode(&p->catroot, img, p->order);
        if ((s = cdb_storage_program(&p->st, p->loc0 + p->cat_next_slot * CDB_CATROOT_SLOT_SIZE,
                                     img, sizeof img)) != CDB_OK)
            return s;
        p->cat_next_slot = (p->cat_next_slot + 1u) % 2u;
    }
    return CDB_OK;
}

/* Opens an existing file: reads the header, sets up geometry, finds the
 * anchor-map root and its depth, then loads the free-space list and the
 * catalog root. Nothing is written. */
cdb_status cdb_pager_open(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg)
{
    const cdb_header *r;
    cdb_status s;
    unsigned saved_alg = p ? p->aead_alg : 0;
    int saved_hmk = p ? p->has_master_key : 0;
    int saved_hik = p ? p->has_integrity_key : 0;
    uint8_t saved_mk[32];
    uint8_t saved_ik[32];
    cdb_pager_key_fn saved_fn = p ? p->domain_key_fn : NULL;
    void *saved_ctx = p ? p->domain_key_ctx : NULL;

    if (p == NULL || st == NULL || cfg == NULL)
        return CDB_E_INVALID_ARG;
    if (saved_hmk) memcpy(saved_mk, p->master_key, 32);
    if (saved_hik) memcpy(saved_ik, p->integrity_key, 32);
    memset(p, 0, sizeof *p);
    p->aead_alg = (uint8_t)saved_alg;
    p->has_master_key = saved_hmk;
    p->has_integrity_key = saved_hik;
    if (saved_hmk) memcpy(p->master_key, saved_mk, 32);
    if (saved_hik) memcpy(p->integrity_key, saved_ik, 32);
    p->domain_key_fn = saved_fn;
    p->domain_key_ctx = saved_ctx;
    if ((s = cdb_header_open(st, &p->hs)) != CDB_OK)
        return s;
    if ((s = setup(p, st, cfg)) != CDB_OK)
        return s;
    r = &p->hs.record;
    p->read_only = p->hs.read_only;
    p->append_point = r->append_point < p->data_start ? p->data_start : r->append_point;
    /* Resume numbering at the reservation mark: anything below it may have
     * been used before a crash (spec 12.1). */
    p->next_seq = r->page_seq_hwm;
    p->seq_hwm = r->page_seq_hwm;
    p->seq_at_commit = p->next_seq;
    p->root_primary = r->anchor_root;
    p->root_mirror = r->anchor_mirror_root;
    /* Defaults for a file with no anchor map yet: a single empty root, and
     * lid 0 is never issued. */
    p->depth = 1;
    p->next_lid = 1;

    /* An anchor map exists: read the next lid and the free-space chain
     * roots from the root's 32-byte area (spec 14.7.6). */
    if (p->root_primary != 0 || p->root_mirror != 0) {
        cdb_pager_node *root;
        const uint8_t *area;
        cdb_page_header h;
        uint64_t phys = p->root_primary;
        /* The root's level gives the depth. Peek at it through either copy. */
        if (read_page(p, phys, p->scratch, CDB_PAGE_TYPE_ANCHOR_MAP, &h) != CDB_OK
            || (p->scratch[payload_off(p) + 1] & NODE_FLAG_ROOT) == 0
            || (unsigned)p->scratch[payload_off(p)] >= MAX_DEPTH) {
            phys = p->root_mirror;
            if ((s = read_page(p, phys, p->scratch, CDB_PAGE_TYPE_ANCHOR_MAP, &h)) != CDB_OK)
                return s;
            if ((p->scratch[payload_off(p) + 1] & NODE_FLAG_ROOT) == 0
                || (unsigned)p->scratch[payload_off(p)] >= MAX_DEPTH)
                return CDB_E_CORRUPT;
        }
        p->depth = (unsigned)p->scratch[payload_off(p)] + 1u;
        if ((s = get_node(p, p->depth - 1, 0, &root)) != CDB_OK)
            return s;
        area = root->primary + payload_off(p) + NODE_HDR;
        p->next_lid = cdb_load_u64(area, p->order);
        p->free_root = cdb_load_u64(area + 8, p->order);
        p->free_mirror = cdb_load_u64(area + 16, p->order);
        if (p->next_lid == 0)
            return CDB_E_CORRUPT;
    }
    /* Free-space list: the primary chain, or the mirror if it is damaged. */
    if (load_free_chain(p, p->free_root) != CDB_OK) {
        if ((s = load_free_chain(p, p->free_mirror)) != CDB_OK)
            return s;
    }
    return load_catroot(p);
}

/* Writes a fresh header (with no anchor map and no catalog) and then opens
 * the new file the normal way. The caller's h is copied and adjusted; it is
 * not modified. */
cdb_status cdb_pager_create(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg,
                            const cdb_header *h)
{
    cdb_header rec = *h;
    cdb_header_state hs;
    uint64_t page, loc0;
    cdb_status s;

    if (p == NULL || st == NULL || cfg == NULL || h == NULL)
        return CDB_E_INVALID_ARG;
    /* Page sizes from 512 bytes to 64 KiB. */
    if (rec.default_page_log2 < 9 || rec.default_page_log2 > 16)
        return CDB_E_INVALID_ARG;
    /* One page size for every page in this milestone; data starts on the
     * first page after the header slots and location 0. */
    rec.min_page_log2 = rec.default_page_log2;
    rec.max_page_log2 = rec.default_page_log2;
    rec.page_mode = 0;
    if (rec.checksum_alg == 0)
        rec.checksum_alg = (uint8_t)CDB_CHECKSUM_CRC32C;
    page = (uint64_t)1 << rec.default_page_log2;
    loc0 = round_up(CDB_HEADER_SLOTS * CDB_HEADER_SLOT_SIZE, page);
    rec.append_point = round_up(loc0 + LOCATION0_SLOTS_BYTES, page) / page;
    rec.anchor_root = 0;
    rec.anchor_mirror_root = 0;
    rec.page_seq_hwm = 1; /* sequence 0 is never issued */
    if ((s = cdb_header_create(st, &rec, &hs)) != CDB_OK)
        return s;
    return cdb_pager_open(p, st, cfg);
}

/* -------------------------------------------------------- public ops */

/* Hands out the next lid. The lid has no page yet (its entry is "unused")
 * until cdb_pager_write is called for it. */
cdb_status cdb_pager_alloc(cdb_pager *p, uint64_t *lid)
{
    cdb_pager_node *root;
    cdb_status s;

    if (p->read_only)
        return CDB_E_READ_ONLY;
    /* The compact page header stores lids in 16 bits (spec 14.2.3). */
    if (p->fmt == CDB_PAGE_FMT_COMPACT && p->next_lid > UINT16_MAX)
        return CDB_E_OVERFLOW;
    /* Add levels until the tree can address the new lid. */
    while (p->next_lid >= tree_capacity(p, p->depth))
        if ((s = grow(p)) != CDB_OK)
            return s;
    if ((s = get_node(p, p->depth - 1, 0, &root)) != CDB_OK)
        return s;
    *lid = p->next_lid++;
    return mark_dirty(p, root); /* the root records the next logical ID */
}

/* Whether the page at phys was written since the last commit. Its sealed
 * sequence is then at or above the mark: sequences are never reused, so
 * every page of a committed state is below it. No committed state refers to
 * such a page, so it may be overwritten in place. A page that does not
 * verify is not trusted. The logical-ID check guards against a page that
 * was since reused for something else. Uses p->scratch. */
static int written_since_commit(cdb_pager *p, uint64_t phys, uint64_t lid)
{
    cdb_page_header h;
    if (phys < p->data_start || cdb_storage_read(&p->st, phys_offset(p, phys), p->scratch, p->page_size) != CDB_OK
        || pager_open_page(p, p->scratch, &h, 0) != CDB_OK)
        return 0;
    return h.write_seq >= p->seq_at_commit && h.logical_id == lid;
}

/* Writes lid. A page already written since the last commit is overwritten
 * in place (unless moving it is the point), so a page changed many times
 * between commits takes one physical page, not one per change.
 *
 * may_rewrite is 0 only for compaction, whose whole purpose is to move the
 * page to a new (lower) physical location. Errors: CDB_E_NOT_FOUND for a lid
 * never allocated or already freed; CDB_E_INVALID_ARG for a payload larger
 * than one page can hold. The payload is copied; the caller keeps its buffer.
 * Uses p->scratch. */
static cdb_status pager_write(cdb_pager *p, uint64_t lid, const cdb_page_header *tmpl, const void *payload,
                              size_t len, int may_rewrite)
{
    cdb_pager_node *leaf;
    cdb_anchor_entry e;
    cdb_page_header h;
    size_t slot;
    uint64_t phys;
    cdb_status s;

    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (len > cdb_page_capacity(p->fmt, p->page_size) || (payload == NULL && len != 0))
        return CDB_E_INVALID_ARG;
    if ((s = entry_get(p, lid, &leaf, &slot, &e)) != CDB_OK)
        return s;
    if (e.state == CDB_ANCHOR_RELEASED)
        return CDB_E_NOT_FOUND;
    /* Private mode (spec 5.9.1): the new version goes to the overlay, in
     * place if it is there already. The archive page is left alone and not
     * put on the free list, since this pager never commits. */
    if (p->private_mode) {
        memset(p->scratch, 0, p->page_size);
        if (len)
            memcpy(p->scratch + payload_off(p), payload, len);
        h = *tmpl;
        h.logical_id = lid;
        h.payload_len = (uint32_t)len;
        if (e.state == CDB_ANCHOR_PRIVATE)
            return write_overlay(p, p->scratch, &h, e.phys);
        if ((s = load_ancestors(p, leaf)) != CDB_OK)
            return s;
        phys = p->ov_next;
        if ((s = write_overlay(p, p->scratch, &h, phys)) != CDB_OK)
            return s;
        p->ov_next++;
        e.phys = phys;
        e.size_log2 = (uint8_t)p->hs.record.default_page_log2;
        e.state = CDB_ANCHOR_PRIVATE;
        e.extent_len = 0;
        return entry_set(p, leaf, slot, &e);
    }
    /* Case 1: the current page is already a private, uncommitted copy.
     * Overwrite it where it is; the anchor entry does not change. */
    if (may_rewrite && e.state == CDB_ANCHOR_LIVE && written_since_commit(p, e.phys, lid)) {
        memset(p->scratch, 0, p->page_size);
        if (len)
            memcpy(p->scratch + payload_off(p), payload, len);
        h = *tmpl;
        h.logical_id = lid;
        h.payload_len = (uint32_t)len;
        return write_page_at(p, p->scratch, &h, e.phys);
    }

    /* Case 2: copy-on-write. Write the new version to a fresh physical page,
     * put the old page (if any) on the free list, and point the anchor entry
     * at the new page. The old page stays intact for the committed state. */
    if ((s = load_ancestors(p, leaf)) != CDB_OK)
        return s;
    memset(p->scratch, 0, p->page_size);
    if (len)
        memcpy(p->scratch + payload_off(p), payload, len);
    h = *tmpl;
    h.logical_id = lid;
    h.payload_len = (uint32_t)len;
    if ((s = append_page(p, p->scratch, &h, &phys)) != CDB_OK)
        return s;
    if (e.state == CDB_ANCHOR_LIVE && (s = free_push(p, e.phys, 1)) != CDB_OK)
        return s;
    e.phys = phys;
    e.size_log2 = (uint8_t)p->hs.record.default_page_log2;
    e.state = CDB_ANCHOR_LIVE;
    e.extent_len = 0;
    return entry_set(p, leaf, slot, &e);
}

size_t cdb_pager_cache_size(size_t page_size, size_t n_pages)
{
    return n_pages * (sizeof(cdb_page_cache_slot) + page_size);
}

void cdb_pager_cache_wipe_domain(cdb_pager *p, uint64_t domain_id)
{
    size_t i;
    if (p == NULL || p->cache_slots == NULL)
        return;
    for (i = 0; i < p->cache_count; i++) {
        cdb_page_cache_slot *s = &p->cache_slots[i];
        if (s->lid != 0 && (domain_id == 0 || s->hdr.domain_id == domain_id)) {
            cdb_crypto_wipe(s->page, p->page_size);
            memset(&s->hdr, 0, sizeof s->hdr);
            s->lid = 0;
            s->phys = 0;
            s->state = 0;
            s->lru = 0;
        }
    }
}

static void cache_invalidate(cdb_pager *p, uint64_t lid)
{
    if (p->cache_slots != NULL && p->cache_count > 0) {
        size_t set_idx = (size_t)((lid * 2654435761ULL) % (uint64_t)p->cache_sets);
        size_t base = set_idx * 4u;
        size_t count = p->cache_sets == 1 ? p->cache_count : 4u;
        size_t i;
        for (i = 0; i < count; i++) {
            if (p->cache_slots[base + i].lid == lid) {
                cdb_crypto_wipe(p->cache_slots[base + i].page, p->page_size);
                memset(&p->cache_slots[base + i].hdr, 0, sizeof p->cache_slots[base + i].hdr);
                p->cache_slots[base + i].lid = 0;
                p->cache_slots[base + i].phys = 0;
                p->cache_slots[base + i].state = 0;
                p->cache_slots[base + i].lru = 0;
                break;
            }
        }
    }
}

cdb_status cdb_pager_write(cdb_pager *p, uint64_t lid, const cdb_page_header *tmpl,
                           const void *payload, size_t len)
{
    cdb_status s = pager_write(p, lid, tmpl, payload, len, 1);
    if (s == CDB_OK)
        cache_invalidate(p, lid);
    return s;
}

cdb_status cdb_pager_peek(cdb_pager *p, uint64_t lid, uint8_t *page, cdb_page_header *h, int *committed)
{
    cdb_pager_node *leaf;
    cdb_anchor_entry e;
    size_t slot;
    cdb_status s;

    if ((s = entry_get(p, lid, &leaf, &slot, &e)) != CDB_OK)
        return s;
    if (e.state != CDB_ANCHOR_LIVE && e.state != CDB_ANCHOR_PRIVATE)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_storage_read(e.state == CDB_ANCHOR_PRIVATE ? &p->overlay : &p->st, phys_offset(p, e.phys), page,
                              p->page_size)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if ((s = cdb_page_peek(page, p->page_size, p->fmt, p->order, h)) != CDB_OK)
        return s;
    if (committed != NULL)
        *committed = e.state == CDB_ANCHOR_LIVE && h->write_seq < p->seq_at_commit;
    return CDB_OK;
}

/* Reads the current version of lid, including changes not yet committed.
 * CDB_E_NOT_FOUND if the lid has no live page; CDB_E_CORRUPT if the page
 * fails verification or carries a different lid (a stale pointer). */
cdb_status cdb_pager_read(cdb_pager *p, uint64_t lid, uint8_t *page, cdb_page_header *h)
{
    cdb_pager_node *leaf;
    cdb_anchor_entry e;
    size_t slot;
    cdb_status s;

    if ((s = entry_get(p, lid, &leaf, &slot, &e)) != CDB_OK)
        return s;
    if (e.state != CDB_ANCHOR_LIVE && e.state != CDB_ANCHOR_PRIVATE)
        return CDB_E_NOT_FOUND;

    if (p->cache_slots != NULL && p->cache_count > 0) {
        size_t set_idx = (size_t)((lid * 2654435761ULL) % (uint64_t)p->cache_sets);
        size_t base = set_idx * 4u;
        size_t count = p->cache_sets == 1 ? p->cache_count : 4u;
        size_t i;
        for (i = 0; i < count; i++) {
            cdb_page_cache_slot *cs = &p->cache_slots[base + i];
            if (cs->lid == lid && cs->phys == e.phys && cs->state == e.state) {
                if (cs->hdr.domain_id > 0 && p->aead_alg != CDB_AEAD_NONE) {
                    uint8_t key_buf[32];
                    if (p->domain_key_fn == NULL
                        || p->domain_key_fn(p->domain_key_ctx, cs->hdr.domain_id, cs->hdr.key_version,
                                            cs->hdr.salt_index, key_buf) != CDB_OK) {
                        cdb_crypto_wipe(key_buf, sizeof key_buf);
                        return CDB_E_SECURITY;
                    }
                    cdb_crypto_wipe(key_buf, sizeof key_buf);
                } else if (cs->hdr.flags & CDB_PAGE_FLAG_TAMPER_EVIDENT) {
                    if (!p->has_integrity_key)
                        return CDB_E_SECURITY;
                }
                cs->lru = ++p->cache_clock;
                memcpy(page, cs->page, p->page_size);
                if (h != NULL)
                    *h = cs->hdr;
                return CDB_OK;
            }
        }
    }

    if ((s = cdb_storage_read(e.state == CDB_ANCHOR_PRIVATE ? &p->overlay : &p->st, phys_offset(p, e.phys), page,
                              p->page_size)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if ((s = pager_open_page(p, page, h, e.state == CDB_ANCHOR_PRIVATE)) != CDB_OK)
        return s;
    if (h->logical_id != lid)
        return CDB_E_CORRUPT;

    if (p->cache_slots != NULL && p->cache_count > 0) {
        size_t set_idx = (size_t)((lid * 2654435761ULL) % (uint64_t)p->cache_sets);
        size_t base = set_idx * 4u;
        size_t count = p->cache_sets == 1 ? p->cache_count : 4u;
        size_t best_i = 0;
        uint64_t min_lru = UINT64_MAX;
        size_t i;
        for (i = 0; i < count; i++) {
            cdb_page_cache_slot *cs = &p->cache_slots[base + i];
            if (cs->lid == 0) {
                best_i = i;
                break;
            }
            if (cs->lru < min_lru) {
                min_lru = cs->lru;
                best_i = i;
            }
        }
        cdb_page_cache_slot *victim = &p->cache_slots[base + best_i];
        victim->lid = lid;
        victim->phys = e.phys;
        victim->state = e.state;
        victim->hdr = *h;
        victim->lru = ++p->cache_clock;
        memcpy(victim->page, page, p->page_size);
    }
    return CDB_OK;
}

/* Marks lid released and puts its physical page on the free list. The page
 * itself is not touched: the committed state may still reference it until
 * the next commit lands. CDB_E_NOT_FOUND if lid has no live page. */
cdb_status cdb_pager_free(cdb_pager *p, uint64_t lid)
{
    cdb_pager_node *leaf;
    cdb_anchor_entry e;
    size_t slot;
    cdb_status s;

    if (p->read_only)
        return CDB_E_READ_ONLY;
    if ((s = entry_get(p, lid, &leaf, &slot, &e)) != CDB_OK)
        return s;
    if (e.state != CDB_ANCHOR_LIVE && e.state != CDB_ANCHOR_PRIVATE)
        return CDB_E_NOT_FOUND;
    if ((s = load_ancestors(p, leaf)) != CDB_OK)
        return s;
    if (!p->private_mode && (s = free_push(p, e.phys, 1)) != CDB_OK)
        return s;
    e.phys = 0;
    e.state = CDB_ANCHOR_RELEASED;
    cache_invalidate(p, lid);
    return entry_set(p, leaf, slot, &e);
}

/* Returns lid's raw anchor entry, whatever its state. */
cdb_status cdb_pager_lookup(cdb_pager *p, uint64_t lid, cdb_anchor_entry *out)
{
    cdb_pager_node *leaf;
    size_t slot;
    return entry_get(p, lid, &leaf, &slot, out);
}

/* Writes one dirty node's two copies at the given pages, then points its
 * parent at them. Nodes must be flushed from the leaves up, so that each
 * parent is still dirty (and so still unwritten) when its child's new
 * addresses are stored into it. The root's new pages go to p->root_primary
 * and p->root_mirror, which the header commit then records. */
static cdb_status flush_node(cdb_pager *p, cdb_pager_node *n, uint64_t new_p, uint64_t new_m)
{
    int root = node_is_root(p, n->level, n->number);
    cdb_page_header h;
    cdb_status s;

    /* The root carries the allocation counter and the free-list chain roots
     * (spec 14.7.6); refresh them in both copies. */
    if (root) {
        size_t i;
        for (i = 0; i < 2; i++) {
            uint8_t *area = (i ? n->mirror : n->primary) + payload_off(p) + NODE_HDR;
            cdb_store_u64(area, p->next_lid, p->order);
            cdb_store_u64(area + 8, p->free_root, p->order);
            cdb_store_u64(area + 16, p->free_mirror, p->order);
        }
    }
    if (n->level == 0) /* leaf copies hold identical entries */
        memcpy(n->mirror + payload_off(p), n->primary + payload_off(p),
               p->page_size - payload_off(p) - cdb_page_trailer_size(p->fmt));

    memset(&h, 0, sizeof h);
    h.type = CDB_PAGE_TYPE_ANCHOR_MAP;
    h.payload_len = (uint32_t)cdb_page_capacity(p->fmt, p->page_size);
    if ((s = write_page_at(p, n->primary, &h, new_p)) != CDB_OK)
        return s;
    if ((s = write_page_at(p, n->mirror, &h, new_m)) != CDB_OK)
        return s;
    n->phys_primary = new_p;
    n->phys_mirror = new_m;
    n->dirty = 0;

    /* Record the new locations one level up: in the pager for the root, or
     * in the parent's matching entry (primary into primary, mirror into
     * mirror) for any other node. */
    if (root) {
        p->root_primary = new_p;
        p->root_mirror = new_m;
    } else {
        cdb_pager_node *parent = find_node(p, n->level + 1, n->number / p->inner_fan);
        size_t at;
        if (parent == NULL || !parent->dirty)
            return CDB_E_STATE; /* ancestors of a dirty node are always dirty */
        at = entries_off(p, node_is_root(p, parent->level, parent->number))
           + (size_t)(n->number % p->inner_fan) * p->addr_width;
        store_addr(p, parent->primary + at, new_p);
        store_addr(p, parent->mirror + at, new_m);
    }
    return CDB_OK;
}

/* The commit (spec 3.2, 14.7.8). The ordering is what makes it crash safe:
 * every new page is made durable (barrier) before the header that points at
 * it is written, and the header write is the single step that switches the
 * file to the new state. A crash anywhere before that leaves the previous
 * header, and everything it references, untouched. */
cdb_status cdb_pager_commit(cdb_pager *p)
{
    cdb_pager_node *root;
    uint64_t root_p, root_m;
    cdb_header rec;
    unsigned level;
    size_t i;
    cdb_status s;

    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (!p->dirty)
        return CDB_OK;
    if (p->private_mode)
        return CDB_E_STATE; /* only the baton holder commits (spec 5.9.1) */
    /* The salt table (spec 14.2.8) changes in the commit whose pages use it. */
    if (p->sess.precommit != NULL
        && (s = p->sess.precommit(p->sess.ctx, p->hs.record.committed_seq + 1u, p->salt_used)) != CDB_OK)
        return s;

    /* Step 1 barrier: data pages written since the last commit are durable. */
    if ((s = cdb_storage_sync(&p->st)) != CDB_OK)
        return s;

    /* Space freed by commits before the latest one is now reusable. */
    free_normalize(p);

    /* Every commit rewrites the root, since the root records the chains. */
    if ((s = get_node(p, p->depth - 1, 0, &root)) != CDB_OK)
        return s;
    root->dirty = 1;

    /* Everything this commit replaces becomes free: the previous free-space
     * chains and the old copies of every node about to be rewritten. */
    if ((s = free_old_chain(p, p->free_root)) != CDB_OK)
        return s;
    if ((s = free_old_chain(p, p->free_mirror)) != CDB_OK)
        return s;
    for (i = 0; i < p->node_count; i++) {
        cdb_pager_node *n = &p->cfg.nodes[i];
        if (n->in_use && n->dirty) {
            if ((s = free_push(p, n->phys_primary, 1)) != CDB_OK)
                return s;
            if ((s = free_push(p, n->phys_mirror, 1)) != CDB_OK)
                return s;
        }
    }

    /* Step 2: the anchor map from the leaves up. The root's pages and the
     * chains are allocated last, so the chains record the final list. */
    for (level = 0; level + 1 < p->depth; level++) {
        for (i = 0; i < p->node_count; i++) {
            cdb_pager_node *n = &p->cfg.nodes[i];
            if (n->in_use && n->dirty && n->level == level) {
                uint64_t np = alloc_phys(p), nm = alloc_phys(p);
                if ((s = flush_node(p, n, np, nm)) != CDB_OK)
                    return s;
            }
        }
    }
    /* Steps 3 to 6 of spec 14.7.8: reserve the root's pages, write both
     * free-space chains, write the root (which records the chain roots),
     * then a barrier so all of it is durable before any header points to
     * it. */
    root_p = alloc_phys(p);
    root_m = alloc_phys(p);
    free_normalize(p);
    if ((s = write_free_chains(p)) != CDB_OK)
        return s;
    if ((s = flush_node(p, root, root_p, root_m)) != CDB_OK)
        return s;
    if ((s = cdb_storage_sync(&p->st)) != CDB_OK)
        return s;

    /* Step 3: the catalog root slot, when the catalog changed. */
    if (p->cat_dirty) {
        if ((s = write_catroot(p)) != CDB_OK)
            return s;
        if ((s = cdb_storage_sync(&p->st)) != CDB_OK)
            return s;
    }

    /* Step 4: the header commit. This is the switch-over point: once the
     * new header slot is durable, the new anchor map is the committed state.
     * cdb_header_commit raises committed_seq by one. */
    rec = p->hs.record;
    rec.anchor_root = p->root_primary;
    rec.anchor_mirror_root = p->root_mirror;
    rec.append_point = p->append_point;
    rec.page_seq_hwm = p->seq_hwm;
    if (p->guard != NULL && (s = p->guard(p->guard_ctx)) != CDB_OK)
        return s;
    if ((s = cdb_header_commit(&p->st, &p->hs, &rec)) != CDB_OK)
        return s;
    /* Pages written from now on are newer than this commit, so they may be
     * overwritten in place until the next one (see written_since_commit). */
    p->dirty = 0;
    p->seq_at_commit = p->next_seq;
    p->salt_index = 0; /* the table may change before the next commit */
    p->salt_used = 0;
    if (p->cat_dirty) {
        p->cat_dirty = 0;
        p->cat_exists = 1;
    }
    return CDB_OK;
}

/* -------------------------------------------------------- compaction */

/* Lowest page that may be reused now, or 0 if none. */
static uint64_t lowest_reusable(const cdb_pager *p)
{
    size_t i;
    for (i = 0; i < p->free_len; i++)
        if (reusable(p, p->cfg.free_list[i].freed_seq))
            return p->cfg.free_list[i].start;
    return 0;
}

/* See cdb_pager.h. The append point is lowered by a normal commit first, and
 * only after that commit is durable is the file truncated: a crash between
 * the two leaves a slightly longer file, which is harmless. */
cdb_status cdb_pager_reclaim_tail(cdb_pager *p, uint64_t *reclaimed)
{
    uint64_t end = p->append_point, size;
    cdb_status s;

    *reclaimed = 0;
    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (p->dirty || p->private_mode)
        return CDB_E_STATE; /* commit first; only the baton holder writes */
    /* The free tail: extents that reach the append point and may be reused. */
    while (p->free_len > 0) {
        cdb_free_extent *last = &p->cfg.free_list[p->free_len - 1];
        if (last->start + last->len != end || !reusable(p, last->freed_seq))
            break;
        end = last->start;
        p->free_len--;
    }
    if (end < p->append_point) {
        *reclaimed = p->append_point - end;
        p->append_point = end;
        p->dirty = 1;
        if ((s = cdb_pager_commit(p)) != CDB_OK)
            return s;
    }
    /* Neither header slot's state references anything past the append point,
     * including pages a crash left behind (spec 3.7). */
    if ((s = cdb_storage_size(&p->st, &size)) != CDB_OK)
        return s;
    if (size > phys_offset(p, p->append_point)) {
        s = cdb_storage_truncate(&p->st, phys_offset(p, p->append_point));
        if (s == CDB_E_UNSUPPORTED)
            return CDB_OK; /* this medium keeps its size */
        if (s != CDB_OK)
            return s;
        s = cdb_storage_sync(&p->st);
        if (s != CDB_OK)
            return s;
    }
    if (*reclaimed > 0)
        cdb_pager_cache_wipe_domain(p, 0);
    return CDB_OK;
}

/* Number of cache slots holding dirty (unevictable) nodes. */
static size_t dirty_nodes(const cdb_pager *p)
{
    size_t i, n = 0;
    for (i = 0; i < p->node_count; i++)
        if (p->cfg.nodes[i].in_use && p->cfg.nodes[i].dirty)
            n++;
    return n;
}

/* Marks every anchor-map node with a copy at or beyond target dirty, so the
 * next commit rewrites it into lower space. Commits along the way whenever
 * the node cache is nearly full of dirty nodes. */
static cdb_status relocate_nodes(cdb_pager *p, uint64_t target, uint64_t max_moves, uint64_t *moved)
{
    /* count = number of nodes on the current level; starts with the leaves. */
    uint64_t count = p->depth == 1 ? 1 : (p->next_lid + p->leaf_fan - 1) / p->leaf_fan;
    unsigned level;
    cdb_status s;

    /* Every level except the root, which is rewritten by every commit
     * anyway. */
    for (level = 0; level + 1 < p->depth; level++) {
        uint64_t number;
        for (number = 0; number < count && *moved < max_moves; number++) {
            cdb_pager_node *n;
            if ((s = get_node(p, level, number, &n)) != CDB_OK)
                return s;
            if (n->dirty || (n->phys_primary < target && n->phys_mirror < target))
                continue;
            if (n->phys_primary == 0 && n->phys_mirror == 0)
                continue; /* never written: nothing to move */
            if ((s = mark_dirty(p, n)) != CDB_OK)
                return s;
            (*moved)++;
            if (dirty_nodes(p) + p->depth + 2u >= p->node_count && (s = cdb_pager_commit(p)) != CDB_OK)
                return s;
        }
        count = (count + p->inner_fan - 1) / p->inner_fan;
    }
    return CDB_OK;
}

cdb_status cdb_pager_compact(cdb_pager *p, uint64_t max_moves, uint64_t *moved, uint64_t *reclaimed)
{
    uint64_t lid, target;
    cdb_status s;

    *moved = 0;
    *reclaimed = 0;
    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (p->dirty || p->private_mode)
        return CDB_E_STATE; /* commit first; only the baton holder writes */

    /* Space freed by the latest commit is not reusable yet. A fence commit
     * makes everything freed so far eligible. */
    {
        size_t i;
        for (i = 0; i < p->free_len; i++) {
            if (p->cfg.free_list[i].freed_seq >= p->hs.record.committed_seq) {
                p->dirty = 1;
                if ((s = cdb_pager_commit(p)) != CDB_OK)
                    return s;
                break;
            }
        }
    }

    /* Step 1: move live data pages.
     * The file could end at `target` if every reusable page were filled.
     * Copy live pages at or beyond it down into holes below it
     * (spec 3.4 step 1). Pages already below it stay where they are, so
     * compacting a compact file moves nothing. */
    {
        uint64_t free_pages = 0;
        size_t i;
        for (i = 0; i < p->free_len; i++)
            if (reusable(p, p->cfg.free_list[i].freed_seq))
                free_pages += p->cfg.free_list[i].len;
        target = p->append_point - free_pages;
    }
    for (lid = 1; lid < p->next_lid && *moved < max_moves; lid++) {
        cdb_anchor_entry e;
        cdb_page_header h;
        uint64_t hole = lowest_reusable(p);
        if (hole == 0 || hole >= target)
            break;
        if ((s = cdb_pager_lookup(p, lid, &e)) != CDB_OK)
            return s;
        if (e.state != CDB_ANCHOR_LIVE || e.phys < target)
            continue;
        /* Read into page2 (pager_write uses scratch) and rewrite with
         * may_rewrite = 0, which forces a new page; alloc_phys hands out the
         * lowest reusable hole. The header template is kept as read. */
        if ((s = cdb_pager_read(p, lid, p->page2, &h)) != CDB_OK)
            return s;
        if ((s = pager_write(p, lid, &h, p->page2 + payload_off(p), h.payload_len, 0)) != CDB_OK)
            return s;
        (*moved)++;
        if (dirty_nodes(p) + p->depth + 2u >= p->node_count && (s = cdb_pager_commit(p)) != CDB_OK)
            return s;
    }
    /* Anchor-map nodes living in the high region must move too. */
    if ((s = relocate_nodes(p, target, max_moves, moved)) != CDB_OK)
        return s;
    if (*moved > 0) {
        /* Steps 2 and 3: the anchors and the header. The old copies are freed
         * by this commit, so they become reusable one commit later. */
        p->dirty = 1;
        if ((s = cdb_pager_commit(p)) != CDB_OK)
            return s;
        p->dirty = 1;
        if ((s = cdb_pager_commit(p)) != CDB_OK)
            return s;
    }
    /* Step 4: release the freed tail. */
    s = cdb_pager_reclaim_tail(p, reclaimed);
    if (*moved > 0)
        cdb_pager_cache_wipe_domain(p, 0);
    return s;
}

/* -------------------------------------------------------- sharing */

cdb_status cdb_pager_set_private(cdb_pager *p, const cdb_storage *overlay)
{
    if (p->dirty)
        return CDB_E_STATE;
    cdb_pager_cache_wipe_domain(p, 0);
    if (overlay == NULL) {
        p->private_mode = 0;
        return CDB_OK;
    }
    p->overlay = *overlay;
    p->private_mode = 1;
    p->ov_next = 1;
    return CDB_OK;
}

/* Opening again is exactly "load the committed state"; the settings for
 * sharing and the session are the only things that outlive it. */
cdb_status cdb_pager_reload(cdb_pager *p)
{
    cdb_storage st = p->st, overlay = p->overlay;
    cdb_pager_config cfg = p->cfg;
    int private_mode = p->private_mode;
    uint64_t reuse_below = p->reuse_below;
    cdb_status (*guard)(void *) = p->guard;
    void *guard_ctx = p->guard_ctx;
    cdb_pager_session sess = p->sess;
    cdb_status s = cdb_pager_open(p, &st, &cfg);
    p->sess = sess;
    p->private_mode = private_mode;
    p->overlay = overlay;
    p->ov_next = 1;
    p->reuse_below = reuse_below;
    p->guard = guard;
    p->guard_ctx = guard_ctx;
    return s;
}

void cdb_pager_set_reuse_below(cdb_pager *p, uint64_t seq)
{
    p->reuse_below = seq;
}

void cdb_pager_set_guard(cdb_pager *p, cdb_status (*guard)(void *ctx), void *ctx)
{
    p->guard = guard;
    p->guard_ctx = ctx;
}
