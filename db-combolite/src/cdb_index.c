/* Ordered index: B+tree over pager pages (spec 14.8). */
#include <string.h>

#include "cdb_endian.h"
#include "cdb_index.h"
#include "cdb_key.h"
#include "cdb_varint.h"

/* Node payload (spec 14.8.2): level, flags, key count, prefix length, offset
 * of the entry heap; then the shared prefix, the slot directory (2-byte entry
 * offsets in key order), free space, and the entries packed at the end. */
#define NODE_HDR 8u
#define CHILD_BYTES 8u /* children are held as 8 big-endian bytes while expanded */

/* -------------------------------------------------------- sizing */

static size_t capacity_of(const cdb_pager *p)
{
    return cdb_page_capacity(p->fmt, p->page_size);
}

static size_t max_entries(size_t cap)
{
    return cap / 3u + 2u; /* the smallest entry is three bytes */
}

static size_t heap_size(size_t cap)
{
    return 2u * cap + max_entries(cap) * CDB_INDEX_MAX_PREFIX;
}

static size_t max_item(size_t cap)
{
    return (cap - NODE_HDR) / 8u - 4u; /* any four entries fit in one node */
}

size_t cdb_index_entries_needed(size_t page_size)
{
    return max_entries(page_size);
}

size_t cdb_index_work_size(size_t page_size)
{
    /* node image, read page, heap, separators */
    return page_size + page_size + heap_size(page_size) + CDB_INDEX_MAX_SPLIT * max_item(page_size);
}

/* -------------------------------------------------------- work memory */

typedef struct node {
    unsigned level;
    size_t count;
    size_t heap_used;
} node;

static uint8_t *w_image(cdb_index *ix) { return ix->w.bytes; }
static uint8_t *w_page(cdb_index *ix) { return ix->w.bytes + ix->pager->page_size; }
static uint8_t *w_heap(cdb_index *ix) { return ix->w.bytes + 2u * ix->pager->page_size; }
static uint8_t *w_seps(cdb_index *ix)
{
    return w_heap(ix) + heap_size(capacity_of(ix->pager));
}

static uint8_t *ent_key(cdb_index *ix, const cdb_index_entry *e) { return w_heap(ix) + e->off; }

/* Drops an entry's key but keeps its value: the value sits right after the key. */
static void clear_key(cdb_index_entry *e)
{
    e->off += e->klen;
    e->klen = 0;
}
static uint8_t *ent_val(cdb_index *ix, const cdb_index_entry *e) { return w_heap(ix) + e->off + e->klen; }

static cdb_status heap_add(cdb_index *ix, node *n, const uint8_t *k1, size_t l1, const uint8_t *k2, size_t l2,
                           const uint8_t *v, size_t vl, cdb_index_entry *e)
{
    size_t need = l1 + l2 + vl;
    if (need > heap_size(capacity_of(ix->pager)) - n->heap_used)
        return CDB_E_BUFFER_TOO_SMALL;
    e->off = (uint32_t)n->heap_used;
    e->klen = (uint32_t)(l1 + l2);
    e->vlen = (uint32_t)vl;
    if (l1)
        memcpy(w_heap(ix) + n->heap_used, k1, l1);
    if (l2)
        memcpy(w_heap(ix) + n->heap_used + l1, k2, l2);
    if (vl)
        memmove(w_heap(ix) + n->heap_used + l1 + l2, v, vl);
    n->heap_used += need;
    return CDB_OK;
}

/* -------------------------------------------------------- node pages */

typedef struct node_view {
    const uint8_t *pl;
    size_t cap;
    unsigned level;
    unsigned flags;
    size_t count;
    size_t prefix_len;
} node_view;

static cdb_status view_node(const cdb_index *ix, const uint8_t *pl, node_view *v)
{
    size_t heap;
    v->pl = pl;
    v->cap = capacity_of(ix->pager);
    v->level = pl[0];
    v->flags = pl[1];
    v->count = cdb_load_u16(pl + 2, ix->pager->order);
    v->prefix_len = cdb_load_u16(pl + 4, ix->pager->order);
    heap = cdb_load_u16(pl + 6, ix->pager->order);
    if (v->level >= CDB_INDEX_MAX_DEPTH || v->prefix_len > CDB_INDEX_MAX_PREFIX
        || NODE_HDR + v->prefix_len + 2u * v->count > heap || heap > v->cap)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

/* Decodes entry i: its key suffix and its value (or child) bytes. */
static cdb_status view_entry(const cdb_index *ix, const node_view *v, size_t i,
                             const uint8_t **suffix, size_t *slen, const uint8_t **val, size_t *vlen)
{
    size_t off = cdb_load_u16(v->pl + NODE_HDR + v->prefix_len + 2u * i, ix->pager->order);
    const uint8_t *p;
    size_t avail, n;
    uint64_t len;

    if (off >= v->cap)
        return CDB_E_CORRUPT;
    p = v->pl + off;
    avail = v->cap - off;
    if (cdb_varint_decode(p, avail, &len, &n) != CDB_OK || len > avail - n)
        return CDB_E_CORRUPT;
    p += n;
    avail -= n;
    *suffix = p;
    *slen = (size_t)len;
    p += len;
    avail -= (size_t)len;
    if (v->level == 0) {
        if (cdb_varint_decode(p, avail, &len, &n) != CDB_OK || len > avail - n)
            return CDB_E_CORRUPT;
        *val = p + n;
        *vlen = (size_t)len;
    } else {
        if (avail < ix->pager->addr_width)
            return CDB_E_CORRUPT;
        *val = p;
        *vlen = ix->pager->addr_width;
    }
    return CDB_OK;
}

static uint64_t child_of(const cdb_index *ix, const uint8_t *val)
{
    return ix->pager->addr_width == 8 ? cdb_load_u64(val, ix->pager->order) : cdb_load_u32(val, ix->pager->order);
}

static cdb_status read_node(cdb_index *ix, uint64_t lid, uint8_t *page, node_view *v)
{
    cdb_page_header h;
    cdb_status s = cdb_pager_read(ix->pager, lid, page, &h);
    if (s != CDB_OK)
        return s == CDB_E_NOT_FOUND ? CDB_E_CORRUPT : s;
    if (h.type != CDB_PAGE_TYPE_INDEX_NODE)
        return CDB_E_CORRUPT;
    return view_node(ix, page + cdb_page_header_size(ix->pager->fmt), v);
}

/* Loads node lid into the work entries, with full keys. Interior children
 * become 8 big-endian bytes. */
static cdb_status expand(cdb_index *ix, uint64_t lid, node *n)
{
    node_view v;
    size_t i;
    cdb_status s = read_node(ix, lid, w_page(ix), &v);
    if (s != CDB_OK)
        return s;
    if (v.count > ix->w.entries_cap)
        return CDB_E_CORRUPT;
    n->level = v.level;
    n->count = 0;
    n->heap_used = 0;
    for (i = 0; i < v.count; i++) {
        const uint8_t *suffix, *val;
        size_t slen, vlen;
        uint8_t child[CHILD_BYTES];
        if ((s = view_entry(ix, &v, i, &suffix, &slen, &val, &vlen)) != CDB_OK)
            return s;
        if (v.level > 0) {
            cdb_store_u64(child, child_of(ix, val), CDB_ORDER_BIG);
            val = child;
            vlen = CHILD_BYTES;
        }
        if ((s = heap_add(ix, n, v.pl + NODE_HDR, v.prefix_len, suffix, slen, val, vlen,
                          &ix->w.entries[n->count])) != CDB_OK)
            return s;
        n->count++;
    }
    return CDB_OK;
}

/* Index of the first entry >= key in [0, count). */
static size_t lower_bound(cdb_index *ix, const node *n, const uint8_t *key, size_t klen)
{
    size_t lo = 0, hi = n->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const cdb_index_entry *e = &ix->w.entries[mid];
        if (cdb_key_compare(ent_key(ix, e), e->klen, key, klen) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Interior: the child whose range holds key (last entry with key <= key). */
static size_t child_slot(cdb_index *ix, const node *n, const uint8_t *key, size_t klen)
{
    size_t lb = lower_bound(ix, n, key, klen);
    if (lb < n->count) {
        const cdb_index_entry *e = &ix->w.entries[lb];
        if (cdb_key_compare(ent_key(ix, e), e->klen, key, klen) == 0)
            return lb;
    }
    return lb == 0 ? 0 : lb - 1;
}

static size_t common_prefix(cdb_index *ix, unsigned level, size_t a, size_t b)
{
    const cdb_index_entry *first = &ix->w.entries[a], *last = &ix->w.entries[b - 1];
    size_t n = 0, max;
    if (level != 0 || !(ix->flags & CDB_INDEX_FLAG_PREFIX) || b - a < 2)
        return 0;
    max = first->klen < last->klen ? first->klen : last->klen;
    if (max > CDB_INDEX_MAX_PREFIX)
        max = CDB_INDEX_MAX_PREFIX;
    while (n < max && ent_key(ix, first)[n] == ent_key(ix, last)[n])
        n++;
    return n;
}

static size_t entry_bytes(cdb_index *ix, unsigned level, const cdb_index_entry *e, size_t prefix)
{
    size_t slen = e->klen - prefix;
    size_t size = cdb_varint_size(slen) + slen;
    if (level == 0)
        size += cdb_varint_size(e->vlen) + e->vlen;
    else
        size += ix->pager->addr_width;
    return size;
}

static size_t piece_size(cdb_index *ix, unsigned level, size_t a, size_t b)
{
    size_t prefix = common_prefix(ix, level, a, b), size = NODE_HDR + prefix, i;
    for (i = a; i < b; i++)
        size += 2u + entry_bytes(ix, level, &ix->w.entries[i], prefix);
    return size;
}

/* Builds the node image for entries [a, b) and writes it to lid. */
static cdb_status write_piece(cdb_index *ix, uint64_t lid, unsigned level, size_t a, size_t b)
{
    size_t cap = capacity_of(ix->pager);
    size_t prefix = common_prefix(ix, level, a, b);
    uint8_t *pl = w_image(ix);
    size_t top = cap, i;
    cdb_page_header t;
    cdb_byte_order o = ix->pager->order;

    if (piece_size(ix, level, a, b) > cap)
        return CDB_E_OVERFLOW;
    memset(pl, 0, cap);
    pl[0] = (uint8_t)level;
    pl[1] = ix->flags;
    cdb_store_u16(pl + 2, (uint16_t)(b - a), o);
    cdb_store_u16(pl + 4, (uint16_t)prefix, o);
    if (prefix)
        memcpy(pl + NODE_HDR, ent_key(ix, &ix->w.entries[a]), prefix);
    for (i = a; i < b; i++) {
        const cdb_index_entry *e = &ix->w.entries[i];
        size_t size = entry_bytes(ix, level, e, prefix), slen = e->klen - prefix;
        uint8_t *p;
        top -= size;
        p = pl + top;
        p += cdb_varint_encode(slen, p);
        memcpy(p, ent_key(ix, e) + prefix, slen);
        p += slen;
        if (level == 0) {
            p += cdb_varint_encode(e->vlen, p);
            if (e->vlen)
                memcpy(p, ent_val(ix, e), e->vlen);
        } else {
            uint64_t child = cdb_load_u64(ent_val(ix, e), CDB_ORDER_BIG);
            if (ix->pager->addr_width == 8)
                cdb_store_u64(p, child, o);
            else
                cdb_store_u32(p, (uint32_t)child, o);
        }
        cdb_store_u16(pl + NODE_HDR + prefix + 2u * (i - a), (uint16_t)top, o);
    }
    cdb_store_u16(pl + 6, (uint16_t)top, o);

    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_INDEX_NODE;
    t.store_id = ix->store_id;
    return cdb_pager_write(ix->pager, lid, &t, pl, cap);
}

/* -------------------------------------------------------- writes */

typedef struct split_out {
    size_t count;                         /* new right-hand nodes */
    uint64_t lid[CDB_INDEX_MAX_SPLIT];
    size_t sep_len[CDB_INDEX_MAX_SPLIT];  /* separators live in the seps area */
} split_out;

static uint8_t *sep_at(cdb_index *ix, size_t i)
{
    return w_seps(ix) + i * ix->max_key;
}

/* Writes the expanded node to lid, splitting it into several nodes if it no
 * longer fits. The new right-hand nodes and their first keys come back in
 * out, for the parent to record. */
static cdb_status write_node(cdb_index *ix, uint64_t lid, const node *n, split_out *out)
{
    size_t cap = capacity_of(ix->pager), target = cap * 3u / 4u;
    size_t starts[CDB_INDEX_MAX_SPLIT + 1], pieces = 0, i, acc = NODE_HDR;
    cdb_status s;

    out->count = 0;
    if (n->count == 0 || piece_size(ix, n->level, 0, n->count) <= cap)
        return write_piece(ix, lid, n->level, 0, n->count);

    /* Greedy cut into pieces of about three quarters of a node. Sizes are
     * counted without a prefix, so every piece is sure to fit. */
    starts[pieces++] = 0;
    for (i = 0; i < n->count; i++) {
        size_t e = 2u + entry_bytes(ix, n->level, &ix->w.entries[i], 0);
        if (acc + e > target && i > starts[pieces - 1]) {
            if (pieces == CDB_INDEX_MAX_SPLIT)
                return CDB_E_OVERFLOW;
            starts[pieces++] = i;
            acc = NODE_HDR;
        }
        acc += e;
    }
    starts[pieces] = n->count;

    /* Separators: the first key of every piece after the first. In an
     * interior node, that entry then covers everything from its key up. */
    for (i = 1; i < pieces; i++) {
        cdb_index_entry *first = &ix->w.entries[starts[i]];
        memcpy(sep_at(ix, i - 1), ent_key(ix, first), first->klen);
        out->sep_len[i - 1] = first->klen;
        if (n->level > 0)
            clear_key(first); /* the lowest child of a node has no lower bound */
    }
    if ((s = write_piece(ix, lid, n->level, starts[0], starts[1])) != CDB_OK)
        return s;
    for (i = 1; i < pieces; i++) {
        if ((s = cdb_pager_alloc(ix->pager, &out->lid[i - 1])) != CDB_OK)
            return s;
        if ((s = write_piece(ix, out->lid[i - 1], n->level, starts[i], starts[i + 1])) != CDB_OK)
            return s;
    }
    out->count = pieces - 1;
    return CDB_OK;
}

/* The root keeps its logical ID: when it splits, its pieces move to new
 * nodes and the root becomes their parent, one level up. */
static cdb_status split_root(cdb_index *ix, unsigned level, const split_out *out)
{
    uint64_t first;
    uint8_t child[CHILD_BYTES];
    node n;
    node_view v;
    size_t i;
    cdb_status s;

    /* The root's own (first) piece moves to a new node. */
    if ((s = cdb_pager_alloc(ix->pager, &first)) != CDB_OK)
        return s;
    if ((s = read_node(ix, ix->root, w_page(ix), &v)) != CDB_OK)
        return s;
    {
        cdb_page_header t;
        memset(&t, 0, sizeof t);
        t.type = CDB_PAGE_TYPE_INDEX_NODE;
        t.store_id = ix->store_id;
        memcpy(w_image(ix), v.pl, v.cap);
        if ((s = cdb_pager_write(ix->pager, first, &t, w_image(ix), v.cap)) != CDB_OK)
            return s;
    }
    n.level = level + 1;
    n.count = 0;
    n.heap_used = 0;
    cdb_store_u64(child, first, CDB_ORDER_BIG);
    if ((s = heap_add(ix, &n, NULL, 0, NULL, 0, child, CHILD_BYTES, &ix->w.entries[n.count++])) != CDB_OK)
        return s;
    for (i = 0; i < out->count; i++) {
        cdb_store_u64(child, out->lid[i], CDB_ORDER_BIG);
        if ((s = heap_add(ix, &n, sep_at(ix, i), out->sep_len[i], NULL, 0, child, CHILD_BYTES,
                          &ix->w.entries[n.count++])) != CDB_OK)
            return s;
    }
    return write_piece(ix, ix->root, n.level, 0, n.count);
}

typedef struct path {
    unsigned depth;
    uint64_t lid[CDB_INDEX_MAX_DEPTH];
    size_t slot[CDB_INDEX_MAX_DEPTH]; /* child taken at each interior level */
} path;

/* Descends to the leaf for key; on return the leaf is expanded in work. */
static cdb_status descend(cdb_index *ix, const uint8_t *key, size_t klen, path *pt, node *leaf)
{
    uint64_t lid = ix->root;
    cdb_status s;

    pt->depth = 0;
    for (;;) {
        if (pt->depth >= CDB_INDEX_MAX_DEPTH)
            return CDB_E_CORRUPT;
        if ((s = expand(ix, lid, leaf)) != CDB_OK)
            return s;
        pt->lid[pt->depth] = lid;
        if (leaf->level == 0) {
            pt->depth++;
            return CDB_OK;
        }
        if (leaf->count == 0)
            return CDB_E_CORRUPT;
        pt->slot[pt->depth] = child_slot(ix, leaf, key, klen);
        lid = cdb_load_u64(ent_val(ix, &ix->w.entries[pt->slot[pt->depth]]), CDB_ORDER_BIG);
        pt->depth++;
    }
}

/* Records a child's split in its ancestors, splitting them in turn. */
static cdb_status propagate(cdb_index *ix, const path *pt, unsigned at, split_out *out)
{
    while (out->count > 0) {
        node n;
        size_t i, pos;
        uint8_t child[CHILD_BYTES];
        cdb_status s;
        split_out up;

        if (at == 0) /* the root split */
            return split_root(ix, (unsigned)(pt->depth - 1u), out);
        at--;
        if ((s = expand(ix, pt->lid[at], &n)) != CDB_OK)
            return s;
        pos = pt->slot[at] + 1;
        if (n.count + out->count > ix->w.entries_cap)
            return CDB_E_BUFFER_TOO_SMALL;
        memmove(&ix->w.entries[pos + out->count], &ix->w.entries[pos], (n.count - pos) * sizeof(cdb_index_entry));
        for (i = 0; i < out->count; i++) {
            cdb_store_u64(child, out->lid[i], CDB_ORDER_BIG);
            if ((s = heap_add(ix, &n, sep_at(ix, i), out->sep_len[i], NULL, 0, child, CHILD_BYTES,
                              &ix->w.entries[pos + i])) != CDB_OK)
                return s;
        }
        n.count += out->count;
        if ((s = write_node(ix, pt->lid[at], &n, &up)) != CDB_OK)
            return s;
        if (at == 0 && up.count > 0)
            return split_root(ix, n.level, &up);
        *out = up;
    }
    return CDB_OK;
}

cdb_status cdb_index_put(cdb_index *ix, const uint8_t *key, size_t klen, const void *val, size_t vlen)
{
    path pt;
    node leaf;
    split_out out;
    size_t pos;
    cdb_status s;

    if (klen > ix->max_key || vlen > ix->max_value || (key == NULL && klen) || (val == NULL && vlen))
        return CDB_E_INVALID_ARG;
    if ((s = descend(ix, key, klen, &pt, &leaf)) != CDB_OK)
        return s;
    pos = lower_bound(ix, &leaf, key, klen);
    if (pos < leaf.count && cdb_key_compare(ent_key(ix, &ix->w.entries[pos]), ix->w.entries[pos].klen, key, klen) == 0) {
        if ((s = heap_add(ix, &leaf, key, klen, NULL, 0, (const uint8_t *)val, vlen, &ix->w.entries[pos])) != CDB_OK)
            return s;
    } else {
        if (leaf.count >= ix->w.entries_cap)
            return CDB_E_BUFFER_TOO_SMALL;
        memmove(&ix->w.entries[pos + 1], &ix->w.entries[pos], (leaf.count - pos) * sizeof(cdb_index_entry));
        if ((s = heap_add(ix, &leaf, key, klen, NULL, 0, (const uint8_t *)val, vlen, &ix->w.entries[pos])) != CDB_OK)
            return s;
        leaf.count++;
    }
    if ((s = write_node(ix, pt.lid[pt.depth - 1], &leaf, &out)) != CDB_OK)
        return s;
    if (out.count == 0)
        return CDB_OK;
    if (pt.depth == 1)
        return split_root(ix, 0, &out);
    return propagate(ix, &pt, (unsigned)(pt.depth - 1u), &out);
}

cdb_status cdb_index_get(cdb_index *ix, const uint8_t *key, size_t klen,
                         void *val, size_t vcap, size_t *vlen, int *found)
{
    path pt;
    node leaf;
    size_t pos;
    cdb_status s;

    *found = 0;
    if ((s = descend(ix, key, klen, &pt, &leaf)) != CDB_OK)
        return s;
    pos = lower_bound(ix, &leaf, key, klen);
    if (pos < leaf.count) {
        const cdb_index_entry *e = &ix->w.entries[pos];
        if (cdb_key_compare(ent_key(ix, e), e->klen, key, klen) == 0) {
            *found = 1;
            *vlen = e->vlen;
            if (e->vlen > vcap)
                return CDB_E_BUFFER_TOO_SMALL;
            if (e->vlen)
                memcpy(val, ent_val(ix, e), e->vlen);
        }
    }
    return CDB_OK;
}

/* While the root is an interior node with one child, pull that child up
 * into the root, so the tree never keeps a chain of single children. */
static cdb_status collapse_root(cdb_index *ix)
{
    for (;;) {
        node n;
        uint64_t only;
        cdb_status s;
        if ((s = expand(ix, ix->root, &n)) != CDB_OK)
            return s;
        if (n.level == 0 || n.count != 1)
            return CDB_OK;
        only = cdb_load_u64(ent_val(ix, &ix->w.entries[0]), CDB_ORDER_BIG);
        if ((s = expand(ix, only, &n)) != CDB_OK)
            return s;
        if ((s = write_piece(ix, ix->root, n.level, 0, n.count)) != CDB_OK)
            return s;
        if ((s = cdb_pager_free(ix->pager, only)) != CDB_OK)
            return s;
    }
}

cdb_status cdb_index_delete(cdb_index *ix, const uint8_t *key, size_t klen, int *found)
{
    path pt;
    node n;
    size_t pos;
    unsigned at;
    cdb_status s;

    *found = 0;
    if ((s = descend(ix, key, klen, &pt, &n)) != CDB_OK)
        return s;
    pos = lower_bound(ix, &n, key, klen);
    if (pos >= n.count || cdb_key_compare(ent_key(ix, &ix->w.entries[pos]), ix->w.entries[pos].klen, key, klen) != 0)
        return CDB_OK;
    *found = 1;

    at = (unsigned)(pt.depth - 1u);
    for (;;) {
        split_out none;
        memmove(&ix->w.entries[pos], &ix->w.entries[pos + 1], (n.count - pos - 1) * sizeof(cdb_index_entry));
        n.count--;
        if (n.level > 0 && pos == 0 && n.count > 0)
            clear_key(&ix->w.entries[0]); /* the new lowest child has no lower bound */
        if (n.count > 0 || at == 0) {
            if (n.count == 0)
                n.level = 0; /* the whole index is empty again */
            if ((s = write_node(ix, pt.lid[at], &n, &none)) != CDB_OK)
                return s;
            break;
        }
        /* An empty non-root node goes away, along with its parent's entry. */
        if ((s = cdb_pager_free(ix->pager, pt.lid[at])) != CDB_OK)
            return s;
        at--;
        if ((s = expand(ix, pt.lid[at], &n)) != CDB_OK)
            return s;
        pos = pt.slot[at];
    }
    return collapse_root(ix);
}

/* -------------------------------------------------------- open/create */

static cdb_status setup(cdb_index *ix, cdb_pager *p, const cdb_index_work *w)
{
    size_t cap = capacity_of(p);
    if (w == NULL || w->bytes == NULL || w->entries == NULL || cap < 64u
        || w->bytes_size < cdb_index_work_size(p->page_size) || w->entries_cap < max_entries(cap))
        return CDB_E_INVALID_ARG;
    ix->pager = p;
    ix->w = *w;
    ix->max_key = max_item(cap);
    ix->max_value = max_item(cap);
    return CDB_OK;
}

cdb_status cdb_index_create(cdb_index *ix, cdb_pager *p, uint64_t store_id, unsigned flags,
                            const cdb_index_work *w)
{
    cdb_status s;
    memset(ix, 0, sizeof *ix);
    if ((s = setup(ix, p, w)) != CDB_OK)
        return s;
    ix->flags = (uint8_t)flags;
    ix->store_id = store_id;
    if ((s = cdb_pager_alloc(p, &ix->root)) != CDB_OK)
        return s;
    return write_piece(ix, ix->root, 0, 0, 0);
}

cdb_status cdb_index_open(cdb_index *ix, cdb_pager *p, uint64_t root, const cdb_index_work *w)
{
    node_view v;
    cdb_page_header h;
    cdb_status s;
    memset(ix, 0, sizeof *ix);
    if ((s = setup(ix, p, w)) != CDB_OK)
        return s;
    ix->root = root;
    if ((s = cdb_pager_read(p, root, w_page(ix), &h)) != CDB_OK)
        return s;
    if (h.type != CDB_PAGE_TYPE_INDEX_NODE)
        return CDB_E_CORRUPT;
    if ((s = view_node(ix, w_page(ix) + cdb_page_header_size(p->fmt), &v)) != CDB_OK)
        return s;
    ix->flags = (uint8_t)v.flags;
    ix->store_id = h.store_id;
    return CDB_OK;
}

/* -------------------------------------------------------- cursors */

/* Descends from the node at the cursor's current depth to a leaf, taking
 * the leftmost child each time. */
static cdb_status leftmost(cdb_index_cursor *c)
{
    for (;;) {
        node_view v;
        const uint8_t *suffix, *val;
        size_t slen, vlen;
        cdb_status s = read_node(c->ix, c->lid[c->depth - 1], c->page, &v);
        if (s != CDB_OK)
            return s;
        if (v.level == 0)
            return CDB_OK;
        if (v.count == 0 || c->depth >= CDB_INDEX_MAX_DEPTH)
            return CDB_E_CORRUPT;
        if ((s = view_entry(c->ix, &v, c->slot[c->depth - 1], &suffix, &slen, &val, &vlen)) != CDB_OK)
            return s;
        c->lid[c->depth] = child_of(c->ix, val);
        c->slot[c->depth] = 0;
        c->depth++;
    }
}

cdb_status cdb_index_seek(cdb_index_cursor *c, cdb_index *ix, uint8_t *page, const uint8_t *key, size_t klen)
{
    path pt;
    node leaf;
    unsigned i;
    cdb_status s;

    memset(c, 0, sizeof *c);
    c->ix = ix;
    c->page = page;
    if ((s = descend(ix, key, klen, &pt, &leaf)) != CDB_OK)
        return s;
    c->depth = pt.depth;
    for (i = 0; i < pt.depth; i++) {
        c->lid[i] = pt.lid[i];
        c->slot[i] = (uint32_t)pt.slot[i];
    }
    c->slot[pt.depth - 1] = (uint32_t)lower_bound(ix, &leaf, key, klen);
    return CDB_OK;
}

cdb_status cdb_index_next(cdb_index_cursor *c, uint8_t *key, size_t kcap, size_t *klen,
                          void *val, size_t vcap, size_t *vlen, int *done)
{
    for (;;) {
        node_view v;
        const uint8_t *suffix, *value;
        size_t slen, vl;
        cdb_status s;

        *done = 0;
        if (c->done || c->depth == 0) {
            *done = 1;
            return CDB_OK;
        }
        if ((s = read_node(c->ix, c->lid[c->depth - 1], c->page, &v)) != CDB_OK)
            return s;
        if (c->slot[c->depth - 1] < v.count) {
            if ((s = view_entry(c->ix, &v, c->slot[c->depth - 1], &suffix, &slen, &value, &vl)) != CDB_OK)
                return s;
            *klen = v.prefix_len + slen;
            *vlen = vl;
            if (*klen > kcap || vl > vcap)
                return CDB_E_BUFFER_TOO_SMALL;
            memcpy(key, v.pl + NODE_HDR, v.prefix_len);
            memcpy(key + v.prefix_len, suffix, slen);
            if (vl)
                memcpy(val, value, vl);
            c->slot[c->depth - 1]++;
            return CDB_OK;
        }
        /* This leaf is done: climb to the first ancestor with another child. */
        for (;;) {
            c->depth--;
            if (c->depth == 0) {
                c->done = 1;
                break;
            }
            if ((s = read_node(c->ix, c->lid[c->depth - 1], c->page, &v)) != CDB_OK)
                return s;
            if (++c->slot[c->depth - 1] < v.count) {
                if ((s = leftmost(c)) != CDB_OK)
                    return s;
                break;
            }
        }
    }
}
