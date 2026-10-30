/* Open-store slots (cdb_slots.h). */
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

static cdb_slot *slot_at(cdb_slot_chunk *c, size_t elem, unsigned i)
{
    return (cdb_slot *)(void *)((unsigned char *)(void *)c + HDR + (size_t)i * elem);
}

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
 * allocator. NULL: there is no memory for one. */
static cdb_slot_chunk *grow(cdb_db *db, cdb_slots *s, unsigned n)
{
    cdb_slot_chunk **pp, *c = NULL;
    unsigned m;

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
