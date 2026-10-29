/* Pager: copy-on-write anchor map, append allocation, sequence reservation
 * and the free-space list (spec 1.7, 3.2, 3.5, 12.1, 14.7). */
#include <string.h>

#include "cdb_endian.h"
#include "cdb_pager.h"

/* Anchor-map page payload: an 8-byte node header (level, flags), then for
 * the root a 32-byte area (next logical ID, free-space chain roots), then
 * the entries. */
#define NODE_HDR 8u
#define ROOT_AREA 32u
#define NODE_FLAG_ROOT 0x01u
#define MAX_DEPTH 8u

/* Free-space page payload: count, reserved, next page, then extents. */
#define FREE_HDR 16u
#define MAX_CHAIN 1000000u

#define LOCATION0_SLOTS_BYTES 1024u /* two 512-byte catalog root slots (spec 14.3.1) */

static uint64_t round_up(uint64_t v, uint64_t unit)
{
    return (v + unit - 1) / unit * unit;
}

/* -------------------------------------------------------- small helpers */

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

static size_t payload_off(const cdb_pager *p)
{
    return cdb_page_header_size(p->fmt);
}

static size_t entry_size(const cdb_pager *p)
{
    return p->addr_width + 4u;
}

static int node_is_root(const cdb_pager *p, unsigned level, uint64_t number)
{
    return level == p->depth - 1 && number == 0;
}

static size_t entries_off(const cdb_pager *p, int is_root)
{
    return payload_off(p) + NODE_HDR + (is_root ? ROOT_AREA : 0u);
}

/* Number of logical IDs a tree of the given depth can address. */
static uint64_t tree_capacity(const cdb_pager *p, unsigned depth)
{
    uint64_t cap;
    unsigned i;
    if (depth == 1)
        return p->root_leaf_fan;
    cap = (uint64_t)p->root_inner_fan * p->leaf_fan;
    for (i = 2; i < depth; i++) {
        if (cap > UINT64_MAX / p->inner_fan)
            return UINT64_MAX;
        cap *= p->inner_fan;
    }
    return cap;
}

static uint64_t phys_offset(const cdb_pager *p, uint64_t phys)
{
    return phys * (uint64_t)p->page_size;
}

/* -------------------------------------------------------- sequences */

/* Hands out the next page sequence number, first making a new reservation
 * block durable whenever the current one is used up (spec 12.1). */
static cdb_status next_seq(cdb_pager *p, uint64_t *seq)
{
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

/* Physical page for a new write: the lowest free page whose freeing commit is
 * older than the latest commit, else the append point. Pages freed by the
 * latest commit stay untouched: the previous header slot's state may still
 * reference them (spec 1.9, 3.5). */
static uint64_t alloc_phys(cdb_pager *p)
{
    uint64_t committed = p->hs.record.committed_seq;
    size_t i;
    for (i = 0; i < p->free_len; i++) {
        cdb_free_extent *x = &p->cfg.free_list[i];
        if (x->freed_seq < committed) {
            uint64_t phys = x->start;
            x->start++;
            if (--x->len == 0) {
                memmove(x, x + 1, (p->free_len - i - 1) * sizeof *x);
                p->free_len--;
            }
            return phys;
        }
    }
    return p->append_point++;
}

/* Seals the image in buf with a fresh sequence number and writes it at phys. */
static cdb_status write_page_at(cdb_pager *p, uint8_t *buf, cdb_page_header *h, uint64_t phys)
{
    cdb_status s;
    if ((s = next_seq(p, &h->write_seq)) != CDB_OK)
        return s;
    if ((s = cdb_page_seal(buf, p->page_size, p->fmt, p->order, p->checksum_alg, h)) != CDB_OK)
        return s;
    return cdb_storage_program(&p->st, phys_offset(p, phys), buf, p->page_size);
}

static cdb_status append_page(cdb_pager *p, uint8_t *buf, cdb_page_header *h, uint64_t *phys)
{
    *phys = alloc_phys(p);
    return write_page_at(p, buf, h, *phys);
}

static cdb_status read_page(cdb_pager *p, uint64_t phys, uint8_t *buf, uint16_t type, cdb_page_header *h)
{
    cdb_status s;
    if (phys < p->data_start)
        return CDB_E_CORRUPT;
    if ((s = cdb_storage_read(&p->st, phys_offset(p, phys), buf, p->page_size)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if ((s = cdb_page_open(buf, p->page_size, p->fmt, p->order, p->checksum_alg, h)) != CDB_OK)
        return s;
    return h->type == type ? CDB_OK : CDB_E_CORRUPT;
}

/* -------------------------------------------------------- free list */

/* Records [start, start+len) as freed by the next commit. The list stays
 * sorted by start. Touching extents merge only when they were freed by the
 * same commit, so no page is ever held back longer than its own freeing
 * commit requires. */
static cdb_status free_push(cdb_pager *p, uint64_t start, uint64_t len)
{
    uint64_t freed = p->hs.record.committed_seq + 1;
    cdb_free_extent *list = p->cfg.free_list;
    size_t i = 0;

    if (start == 0 || len == 0)
        return CDB_OK;
    while (i < p->free_len && list[i].start < start)
        i++;
    if ((i > 0 && list[i - 1].start + list[i - 1].len > start)
        || (i < p->free_len && start + len > list[i].start))
        return CDB_E_CORRUPT; /* freeing space that is already free */
    if (i > 0 && list[i - 1].freed_seq == freed && list[i - 1].start + list[i - 1].len == start) {
        list[i - 1].len += len;
        if (i < p->free_len && list[i].freed_seq == freed && list[i - 1].start + list[i - 1].len == list[i].start) {
            list[i - 1].len += list[i].len;
            memmove(&list[i], &list[i + 1], (p->free_len - i - 1) * sizeof *list);
            p->free_len--;
        }
        return CDB_OK;
    }
    if (i < p->free_len && list[i].freed_seq == freed && start + len == list[i].start) {
        list[i].start = start;
        list[i].len += len;
        return CDB_OK;
    }
    if (p->free_len >= p->cfg.free_cap)
        return CDB_E_BUFFER_TOO_SMALL;
    memmove(&list[i + 1], &list[i], (p->free_len - i) * sizeof *list);
    list[i].start = start;
    list[i].len = len;
    list[i].freed_seq = freed;
    p->free_len++;
    return CDB_OK;
}

/* Extents freed before the latest commit are reusable for good: mark them 0
 * ("reusable") and merge touching extents that share a sequence. */
static void free_normalize(cdb_pager *p)
{
    cdb_free_extent *list = p->cfg.free_list;
    size_t i, out = 0;
    for (i = 0; i < p->free_len; i++) {
        cdb_free_extent x = list[i];
        if (x.freed_seq < p->hs.record.committed_seq)
            x.freed_seq = 0;
        if (out > 0 && list[out - 1].freed_seq == x.freed_seq
            && list[out - 1].start + list[out - 1].len == x.start)
            list[out - 1].len += x.len;
        else
            list[out++] = x;
    }
    p->free_len = out;
}

static size_t free_entry_size(const cdb_pager *p)
{
    return 2u * p->addr_width + 8u;
}

static size_t free_per_page(const cdb_pager *p)
{
    return (cdb_page_capacity(p->fmt, p->page_size) - FREE_HDR) / free_entry_size(p);
}

/* Loads the chain rooted at root into the in-memory list. */
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
        phys = cdb_load_u64(pl + 8, p->order);
    }
    return CDB_OK;
}

/* The pages of the previous chain are freed by this commit. A damaged chain
 * stops the walk; its remaining pages are simply not reclaimed. */
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
 * Spare pages are written as empty links and freed by the next commit. */
static cdb_status write_free_chains(cdb_pager *p)
{
    size_t per = free_per_page(p);
    size_t n = (p->free_len + per - 1) / per;
    size_t copy, i;

    if (n > CDB_PAGER_CHAIN_MAX)
        return CDB_E_BUFFER_TOO_SMALL; /* compact to shorten the list */
    for (i = 0; i < 2 * n; i++)
        p->chain_pages[i] = alloc_phys(p);
    p->free_root = 0;
    p->free_mirror = 0;
    for (copy = 0; copy < 2 && n > 0; copy++) {
        const uint64_t *pages = p->chain_pages + copy * n;
        for (i = 0; i < n; i++) {
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

static void init_empty_node(const cdb_pager *p, cdb_pager_node *n)
{
    int root = node_is_root(p, n->level, n->number);
    memset(n->primary, 0, p->page_size);
    n->primary[payload_off(p)] = (uint8_t)n->level;
    n->primary[payload_off(p) + 1] = root ? (uint8_t)NODE_FLAG_ROOT : 0u;
    memcpy(n->mirror, n->primary, p->page_size);
}

static int node_page_ok(const cdb_pager *p, const uint8_t *img, unsigned level, int root)
{
    const uint8_t *nh = img + payload_off(p);
    return nh[0] == level && (nh[1] & NODE_FLAG_ROOT) == (root ? NODE_FLAG_ROOT : 0u);
}

/* Loads both copies of a node. A damaged copy is replaced by the other. */
static cdb_status load_node(cdb_pager *p, cdb_pager_node *n)
{
    int root = node_is_root(p, n->level, n->number);
    cdb_page_header h;
    int ok_p = 0, ok_m = 0;

    if (n->phys_primary && read_page(p, n->phys_primary, n->primary, CDB_PAGE_TYPE_ANCHOR_MAP, &h) == CDB_OK)
        ok_p = node_page_ok(p, n->primary, n->level, root);
    if (n->level == 0 && ok_p) {
        memcpy(n->mirror, n->primary, p->page_size); /* leaf copies hold the same entries */
        return CDB_OK;
    }
    if (n->phys_mirror && read_page(p, n->phys_mirror, n->mirror, CDB_PAGE_TYPE_ANCHOR_MAP, &h) == CDB_OK)
        ok_m = node_page_ok(p, n->mirror, n->level, root);
    if (!ok_p && !ok_m)
        return CDB_E_CORRUPT;
    if (!ok_p)
        memcpy(n->primary, n->mirror, p->page_size);
    else if (!ok_m)
        memcpy(n->mirror, n->primary, p->page_size);
    return CDB_OK;
}

static cdb_status get_node(cdb_pager *p, unsigned level, uint64_t number, cdb_pager_node **out)
{
    cdb_pager_node *n = find_node(p, level, number);
    uint64_t phys_p, phys_m;
    cdb_status s;

    if (n == NULL) {
        if (level == p->depth - 1) {
            phys_p = p->root_primary;
            phys_m = p->root_mirror;
        } else {
            cdb_pager_node *parent;
            size_t at;
            if ((s = get_node(p, level + 1, number / p->inner_fan, &parent)) != CDB_OK)
                return s;
            at = entries_off(p, node_is_root(p, level + 1, number / p->inner_fan))
               + (size_t)(number % p->inner_fan) * p->addr_width;
            phys_p = load_addr(p, parent->primary + at);
            phys_m = load_addr(p, parent->mirror + at);
        }
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

/* Marks a node and every ancestor dirty. Dirty nodes are never evicted. */
static cdb_status mark_dirty(cdb_pager *p, cdb_pager_node *n)
{
    unsigned level = n->level;
    uint64_t number = n->number;
    cdb_status s;

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

static uint8_t *entry_at(const cdb_pager *p, uint8_t *img, uint64_t leaf, size_t slot)
{
    return img + entries_off(p, node_is_root(p, 0, leaf)) + slot * entry_size(p);
}

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

static cdb_status leaf_for(cdb_pager *p, uint64_t lid, cdb_pager_node **leaf_node, size_t *slot)
{
    uint64_t leaf;
    if (lid == 0 || lid >= p->next_lid)
        return CDB_E_NOT_FOUND;
    entry_locate(p, lid, &leaf, slot);
    return get_node(p, 0, leaf, leaf_node);
}

static cdb_status entry_get(cdb_pager *p, uint64_t lid, cdb_pager_node **leaf, size_t *slot, cdb_anchor_entry *e)
{
    cdb_status s = leaf_for(p, lid, leaf, slot);
    if (s != CDB_OK)
        return s;
    entry_decode(p, entry_at(p, (*leaf)->primary, (*leaf)->number, *slot), e);
    return CDB_OK;
}

static cdb_status entry_set(cdb_pager *p, cdb_pager_node *leaf, size_t slot, const cdb_anchor_entry *e)
{
    entry_encode(p, entry_at(p, leaf->primary, leaf->number, slot), e);
    entry_encode(p, entry_at(p, leaf->mirror, leaf->number, slot), e);
    return mark_dirty(p, leaf);
}

/* Adds a level: the old root becomes child 0 of a new, empty root. */
static cdb_status grow(cdb_pager *p)
{
    cdb_pager_node *old_root, *new_root;
    size_t from, to, len;
    cdb_status s;

    if (p->depth >= MAX_DEPTH)
        return CDB_E_OVERFLOW;
    if ((s = get_node(p, p->depth - 1, 0, &old_root)) != CDB_OK)
        return s;
    old_root->dirty = 1; /* pin it while the new root is set up */
    from = entries_off(p, 1);
    to = entries_off(p, 0);
    len = p->page_size - cdb_page_trailer_size(p->fmt) - from;
    memmove(old_root->primary + to, old_root->primary + from, len);
    memmove(old_root->mirror + to, old_root->mirror + from, len);
    memset(old_root->primary + to + len, 0, from - to);
    memset(old_root->mirror + to + len, 0, from - to);
    old_root->primary[payload_off(p) + 1] = 0;
    old_root->mirror[payload_off(p) + 1] = 0;

    p->depth++;
    if ((s = take_slot(p, &new_root)) != CDB_OK)
        return s;
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
    p->root_primary = 0;
    p->root_mirror = 0;
    p->dirty = 1;
    return CDB_OK;
}

/* -------------------------------------------------------- open/create */

static cdb_status setup(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg)
{
    const cdb_header *r = &p->hs.record;
    size_t cap, per_node;
    uint64_t loc0;

    p->st = *st;
    p->cfg = *cfg;
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

    p->leaf_fan = (cap - NODE_HDR) / entry_size(p);
    p->inner_fan = (cap - NODE_HDR) / p->addr_width;
    p->root_leaf_fan = (cap - NODE_HDR - ROOT_AREA) / entry_size(p);
    p->root_inner_fan = (cap - NODE_HDR - ROOT_AREA) / p->addr_width;

    /* Header slots, then location 0 (the catalog root slots), then data. */
    loc0 = round_up(CDB_HEADER_SLOTS * CDB_HEADER_SLOT_SIZE, p->page_size);
    p->data_start = round_up(loc0 + LOCATION0_SLOTS_BYTES, p->page_size) / p->page_size;

    per_node = 2u * p->page_size;
    if (cfg->arena == NULL || cfg->nodes == NULL || cfg->free_list == NULL
        || cfg->arena_size < per_node + 2u * p->page_size)
        return CDB_E_INVALID_ARG;
    p->node_count = (cfg->arena_size - 2u * p->page_size) / per_node;
    if (p->node_count > cfg->node_cap)
        p->node_count = cfg->node_cap;
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
    return CDB_OK;
}

cdb_status cdb_pager_open(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg)
{
    const cdb_header *r;
    cdb_status s;

    if (p == NULL || st == NULL || cfg == NULL)
        return CDB_E_INVALID_ARG;
    memset(p, 0, sizeof *p);
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
    p->root_primary = r->anchor_root;
    p->root_mirror = r->anchor_mirror_root;
    p->depth = 1;
    p->next_lid = 1;

    if (p->root_primary != 0 || p->root_mirror != 0) {
        cdb_pager_node *root;
        const uint8_t *area;
        cdb_page_header h;
        uint64_t phys = p->root_primary;
        /* The root's level gives the depth. Peek at it through either copy. */
        if (read_page(p, phys, p->scratch, CDB_PAGE_TYPE_ANCHOR_MAP, &h) != CDB_OK) {
            phys = p->root_mirror;
            if ((s = read_page(p, phys, p->scratch, CDB_PAGE_TYPE_ANCHOR_MAP, &h)) != CDB_OK)
                return s;
        }
        p->depth = (unsigned)p->scratch[payload_off(p)] + 1u;
        if (p->depth > MAX_DEPTH)
            return CDB_E_CORRUPT;
        if ((s = get_node(p, p->depth - 1, 0, &root)) != CDB_OK)
            return s;
        area = root->primary + payload_off(p) + NODE_HDR;
        p->next_lid = cdb_load_u64(area, p->order);
        p->free_root = cdb_load_u64(area + 8, p->order);
        p->free_mirror = cdb_load_u64(area + 16, p->order);
        if (p->next_lid == 0)
            return CDB_E_CORRUPT;
    }
    if (load_free_chain(p, p->free_root) != CDB_OK) {
        if ((s = load_free_chain(p, p->free_mirror)) != CDB_OK)
            return s;
    }
    return CDB_OK;
}

cdb_status cdb_pager_create(cdb_pager *p, const cdb_storage *st, const cdb_pager_config *cfg,
                            const cdb_header *h)
{
    cdb_header rec = *h;
    cdb_header_state hs;
    uint64_t page, loc0;
    cdb_status s;

    if (p == NULL || st == NULL || cfg == NULL || h == NULL)
        return CDB_E_INVALID_ARG;
    if (rec.default_page_log2 < 9 || rec.default_page_log2 > 16)
        return CDB_E_INVALID_ARG;
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

cdb_status cdb_pager_alloc(cdb_pager *p, uint64_t *lid)
{
    cdb_pager_node *root;
    cdb_status s;

    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (p->fmt == CDB_PAGE_FMT_COMPACT && p->next_lid > UINT16_MAX)
        return CDB_E_OVERFLOW;
    while (p->next_lid >= tree_capacity(p, p->depth))
        if ((s = grow(p)) != CDB_OK)
            return s;
    if ((s = get_node(p, p->depth - 1, 0, &root)) != CDB_OK)
        return s;
    *lid = p->next_lid++;
    return mark_dirty(p, root); /* the root records the next logical ID */
}

cdb_status cdb_pager_write(cdb_pager *p, uint64_t lid, const cdb_page_header *tmpl,
                           const void *payload, size_t len)
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
    leaf->dirty = 1; /* pin the leaf while the page is written */

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

cdb_status cdb_pager_read(cdb_pager *p, uint64_t lid, uint8_t *page, cdb_page_header *h)
{
    cdb_pager_node *leaf;
    cdb_anchor_entry e;
    size_t slot;
    cdb_status s;

    if ((s = entry_get(p, lid, &leaf, &slot, &e)) != CDB_OK)
        return s;
    if (e.state != CDB_ANCHOR_LIVE)
        return CDB_E_NOT_FOUND;
    if ((s = cdb_storage_read(&p->st, phys_offset(p, e.phys), page, p->page_size)) != CDB_OK)
        return s == CDB_E_TRUNCATED ? CDB_E_CORRUPT : s;
    if ((s = cdb_page_open(page, p->page_size, p->fmt, p->order, p->checksum_alg, h)) != CDB_OK)
        return s;
    return h->logical_id == lid ? CDB_OK : CDB_E_CORRUPT;
}

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
    if (e.state != CDB_ANCHOR_LIVE)
        return CDB_E_NOT_FOUND;
    leaf->dirty = 1;
    if ((s = free_push(p, e.phys, 1)) != CDB_OK)
        return s;
    e.phys = 0;
    e.state = CDB_ANCHOR_RELEASED;
    return entry_set(p, leaf, slot, &e);
}

cdb_status cdb_pager_lookup(cdb_pager *p, uint64_t lid, cdb_anchor_entry *out)
{
    cdb_pager_node *leaf;
    size_t slot;
    return entry_get(p, lid, &leaf, &slot, out);
}

/* Writes one dirty node's two copies at the given pages, then points its
 * parent at them. */
static cdb_status flush_node(cdb_pager *p, cdb_pager_node *n, uint64_t new_p, uint64_t new_m)
{
    int root = node_is_root(p, n->level, n->number);
    cdb_page_header h;
    cdb_status s;

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

    /* Step 1 barrier: data pages written since the last commit are durable. */
    if ((s = cdb_storage_sync(&p->st)) != CDB_OK)
        return s;

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
    root_p = alloc_phys(p);
    root_m = alloc_phys(p);
    free_normalize(p);
    if ((s = write_free_chains(p)) != CDB_OK)
        return s;
    if ((s = flush_node(p, root, root_p, root_m)) != CDB_OK)
        return s;
    if ((s = cdb_storage_sync(&p->st)) != CDB_OK)
        return s;

    /* Step 4: the header commit. */
    rec = p->hs.record;
    rec.anchor_root = p->root_primary;
    rec.anchor_mirror_root = p->root_mirror;
    rec.append_point = p->append_point;
    rec.page_seq_hwm = p->seq_hwm;
    if ((s = cdb_header_commit(&p->st, &p->hs, &rec)) != CDB_OK)
        return s;
    p->dirty = 0;
    return CDB_OK;
}

/* -------------------------------------------------------- compaction */

/* Lowest page that may be reused now, or 0 if none. */
static uint64_t lowest_reusable(const cdb_pager *p)
{
    size_t i;
    for (i = 0; i < p->free_len; i++)
        if (p->cfg.free_list[i].freed_seq < p->hs.record.committed_seq)
            return p->cfg.free_list[i].start;
    return 0;
}

cdb_status cdb_pager_reclaim_tail(cdb_pager *p, uint64_t *reclaimed)
{
    uint64_t committed = p->hs.record.committed_seq;
    uint64_t end = p->append_point, size;
    cdb_status s;

    *reclaimed = 0;
    if (p->read_only)
        return CDB_E_READ_ONLY;
    if (p->dirty)
        return CDB_E_STATE; /* commit first */
    /* The free tail: extents that reach the append point and may be reused. */
    while (p->free_len > 0) {
        cdb_free_extent *last = &p->cfg.free_list[p->free_len - 1];
        if (last->start + last->len != end || last->freed_seq >= committed)
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
        return cdb_storage_sync(&p->st);
    }
    return CDB_OK;
}

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
    uint64_t count = p->depth == 1 ? 1 : (p->next_lid + p->leaf_fan - 1) / p->leaf_fan;
    unsigned level;
    cdb_status s;

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
    if (p->dirty)
        return CDB_E_STATE; /* commit first */

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

    /* The file could end at `target` if every reusable page were filled.
     * Copy live pages at or beyond it down into holes below it
     * (spec 3.4 step 1). Pages already below it stay where they are, so
     * compacting a compact file moves nothing. */
    {
        uint64_t reusable = 0;
        size_t i;
        for (i = 0; i < p->free_len; i++)
            if (p->cfg.free_list[i].freed_seq < p->hs.record.committed_seq)
                reusable += p->cfg.free_list[i].len;
        target = p->append_point - reusable;
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
        if ((s = cdb_pager_read(p, lid, p->page2, &h)) != CDB_OK)
            return s;
        if ((s = cdb_pager_write(p, lid, &h, p->page2 + payload_off(p), h.payload_len)) != CDB_OK)
            return s;
        (*moved)++;
    }
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
    return cdb_pager_reclaim_tail(p, reclaimed);
}
