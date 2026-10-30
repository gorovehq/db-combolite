/* Open-store slots (cdb_slots.h).
 *
 * Memory layout: a set is a linked list of chunks. Each chunk is a small
 * header (rounded up to cdb_slot_align so the slots after it are aligned)
 * followed by n slots of `elem` bytes each. Every slot begins with a
 * cdb_slot, so this code can read in_use, store_id and the use stamps of
 * any model's slot without knowing the rest of its struct.
 *
 * Chunks given back that came from caller memory cannot be returned to the
 * caller, so they wait on the set's `spare` list for reuse. Chunks from the
 * environment's allocator are freed. */
#include <string.h>

#include "cdb_db.h"
#include "cdb_slots.h"

struct cdb_slot_chunk {
    cdb_slot_chunk *next;
    unsigned n;     /* slots in it */
    int owned;      /* from the environment's allocator, rather than caller memory */
};

/* The header rounded up, so the slots after it are aligned. */
#define HDR ((sizeof(cdb_slot_chunk) + sizeof(cdb_slot_align) - 1u) / sizeof(cdb_slot_align) * sizeof(cdb_slot_align))

/* Slot i of chunk c. */
static cdb_slot *slot_at(cdb_slot_chunk *c, size_t elem, unsigned i)
{
    return (cdb_slot *)(void *)((unsigned char *)(void *)c + HDR + (size_t)i * elem);
}

/* Header plus slots, rounded up to the alignment unit so the next chunk
 * carved from caller memory starts aligned too. */
size_t cdb_slots_chunk_bytes(size_t elem, unsigned n)
{
    size_t bytes = HDR + (size_t)n * elem;
    return (bytes + sizeof(cdb_slot_align) - 1u) / sizeof(cdb_slot_align) * sizeof(cdb_slot_align);
}

void cdb_slots_init(cdb_slots *s, size_t elem, const cdb_slot_ops *ops)
{
    memset(s, 0, sizeof *s);
    s->elem = elem;
    s->ops = ops;
}

/* A linear search: sets are small (bounded by max_open, or by use). */
void *cdb_slots_find(const cdb_slots *s, uint64_t store)
{
    cdb_slot_chunk *c;
    unsigned i;
    for (c = s->chunks; c != NULL; c = c->next)
        for (i = 0; i < c->n; i++) {
            cdb_slot *t = slot_at(c, s->elem, i);
            if (t->in_use && t->store_id == store)
                return t;
        }
    return NULL;
}

/* Stamps the slot with the set's use clock (for least-recently-used
 * eviction and for shrinking) and with the open transaction and outermost
 * operation scope, if any, so busy() can protect it. */
void cdb_slots_touch(const cdb_db *db, cdb_slots *s, void *slot)
{
    cdb_slot *t = (cdb_slot *)slot;
    t->last_use = ++s->clock;
    t->tx_use = db->in_tx ? db->tx_id : 0;
    t->op_use = db->op_depth ? db->op_id : 0;
}

/* The open transaction or operation scope has used it. */
static int busy(const cdb_db *db, const cdb_slot *t)
{
    return (db->in_tx && t->tx_use == db->tx_id) || (db->op_depth && t->op_use == db->op_id);
}

static void zero_slots(cdb_slot_chunk *c, size_t elem)
{
    memset(slot_at(c, elem, 0), 0, (size_t)c->n * elem);
}

/* A new chunk of up to n slots: a spare one, then caller memory, then the
 * allocator. NULL: there is no memory for one.
 * Caller memory is handed out front to back and never reclaimed except
 * through the spare list. The allocator is used only when both hooks are
 * present. The new chunk's slots are zeroed and the chunk goes to the
 * front of the list. */
static cdb_slot_chunk *grow(cdb_db *db, cdb_slots *s, unsigned n)
{
    cdb_slot_chunk **pp, *c = NULL;
    unsigned m;

    /* A spare chunk no bigger than asked for (so the cap still holds). */
    for (pp = &s->spare; *pp != NULL; pp = &(*pp)->next)
        if ((*pp)->n <= n) {
            c = *pp;
            *pp = c->next;
            break;
        }
    /* Caller memory: as many of the n slots as still fit. */
    for (m = n; c == NULL && db->cfg.slot_mem != NULL && m > 0; m--) {
        size_t bytes = cdb_slots_chunk_bytes(s->elem, m);
        if (bytes <= db->cfg.slot_mem_size - db->slot_mem_used) {
            c = (cdb_slot_chunk *)(void *)((unsigned char *)db->cfg.slot_mem + db->slot_mem_used);
            db->slot_mem_used += bytes;
            c->n = m;
            c->owned = 0;
        }
    }
    if (c == NULL && db->env.ops->alloc != NULL && db->env.ops->free != NULL
        && (c = (cdb_slot_chunk *)db->env.ops->alloc(db->env.ctx, cdb_slots_chunk_bytes(s->elem, n))) != NULL) {
        c->n = n;
        c->owned = 1;
    }
    if (c == NULL)
        return NULL;
    zero_slots(c, s->elem);
    c->next = s->chunks;
    s->chunks = c;
    s->count += c->n;
    return c;
}

/* Three ways to get a slot, cheapest first: an unused one, a new chunk
 * (sized so the set never exceeds max_open), or evicting the least recently
 * used store that is neither busy nor pinned, after saving it. If the save
 * fails, nothing is evicted and its error is returned. */
cdb_status cdb_slots_claim(cdb_db *db, cdb_slots *s, void **out)
{
    unsigned cap = db->cfg.max_open, i;
    cdb_slot *victim = NULL;
    cdb_slot_chunk *c;
    cdb_status st;

    /* A free slot. */
    for (c = s->chunks; c != NULL; c = c->next)
        for (i = 0; i < c->n; i++) {
            cdb_slot *t = slot_at(c, s->elem, i);
            if (!t->in_use) {
                memset(t, 0, s->elem);
                *out = t;
                return CDB_OK;
            }
        }
    /* Room to grow. */
    if (cap == 0 || s->count < cap) {
        unsigned n = cap != 0 && cap - s->count < CDB_SLOTS_CHUNK ? cap - s->count : CDB_SLOTS_CHUNK;
        if ((c = grow(db, s, n)) != NULL) {
            *out = slot_at(c, s->elem, 0);
            return CDB_OK;
        }
    }
    /* The least recently used store that may be closed. */
    for (c = s->chunks; c != NULL; c = c->next)
        for (i = 0; i < c->n; i++) {
            cdb_slot *t = slot_at(c, s->elem, i);
            if (busy(db, t) || (s->ops->pinned != NULL && s->ops->pinned(t)))
                continue;
            if (victim == NULL || t->last_use < victim->last_use)
                victim = t;
        }
    if (victim == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    if ((st = s->ops->save(db, victim)) != CDB_OK)
        return st;
    memset(victim, 0, s->elem);
    *out = victim;
    return CDB_OK;
}

/* The first slot in use at or after the iterator's position. */
static void *open_from(cdb_slot_iter *it)
{
    for (; it->chunk != NULL; it->chunk = it->chunk->next, it->i = 0)
        for (; it->i < it->chunk->n; it->i++) {
            cdb_slot *t = slot_at(it->chunk, it->elem, it->i);
            if (t->in_use)
                return t;
        }
    return NULL;
}

void *cdb_slots_first(const cdb_slots *s, cdb_slot_iter *it)
{
    it->chunk = s->chunks;
    it->i = 0;
    it->elem = s->elem;
    return open_from(it);
}

void *cdb_slots_next(cdb_slot_iter *it)
{
    it->i++;
    return open_from(it);
}

/* Allocator chunks are freed; caller-memory chunks go to the spare list.
 * The caller has already unlinked c from the in-use list. */
static void give_back(cdb_db *db, cdb_slots *s, cdb_slot_chunk *c)
{
    s->count -= c->n;
    if (c->owned) {
        db->env.ops->free(db->env.ctx, c);
    } else {
        c->next = s->spare;
        s->spare = c;
    }
}

/* A chunk is idle when none of its open stores was used since the previous
 * shrink (last_use not above `floor`), none is busy, and none is pinned.
 * Its stores are simply forgotten: the caller saved their state first, so
 * they reopen from the catalog on their next use. */
void cdb_slots_shrink(cdb_db *db, cdb_slots *s)
{
    cdb_slot_chunk **pp = &s->chunks;
    while (*pp != NULL) {
        cdb_slot_chunk *c = *pp;
        unsigned i;
        int idle = 1;
        for (i = 0; i < c->n && idle; i++) {
            cdb_slot *t = slot_at(c, s->elem, i);
            if (t->in_use && (t->last_use > s->floor || busy(db, t) || (s->ops->pinned != NULL && s->ops->pinned(t))))
                idle = 0;
        }
        if (idle) {
            *pp = c->next;
            give_back(db, s, c);
        } else {
            pp = &c->next;
        }
    }
    s->floor = s->clock;
}

void cdb_slots_release(cdb_db *db, cdb_slots *s)
{
    while (s->chunks != NULL) {
        cdb_slot_chunk *c = s->chunks;
        s->chunks = c->next;
        give_back(db, s, c);
    }
    s->spare = NULL; /* caller memory: nothing to give back */
}
