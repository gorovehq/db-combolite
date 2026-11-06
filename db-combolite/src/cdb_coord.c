/* Coordinator (cdb_coord.h; spec 5, 5.9, 14.5).
 *
 * Joining, step by step:
 *  1. Scan coord/ and wait until every coordinator that looks alive has
 *     either moved its heartbeat (alive) or stayed still for the dead
 *     timeout (dead).
 *  2. Pick the lowest slot no live coordinator holds and the lowest priority
 *     value no live coordinator uses, and write them as a request to this
 *     session's own lobby file.
 *  3. Settle, scan again, and give up the pick if a live coordinator now
 *     holds the slot, or a live lobby request for the same slot or priority
 *     wins the choose rule. Losers try again with the lost slot or priority
 *     excluded.
 *  4. The winner writes both halves of its slot file (taking over the
 *     counters of a dead previous owner), removes its lobby file, syncs the
 *     directory, and claims the primary role if no live coordinator has it. */
#include <stdio.h>
#include <string.h>

#include "cdb_coord.h"
#include "cdb_crc32c.h"

/* "\x89CDBCRD\n" (spec 14.5.2). */
static const uint8_t coord_magic[8] = { 0x89, 0x43, 0x44, 0x42, 0x43, 0x52, 0x44, 0x0A };

#define COORD_EPOCH 2u                /* lock liveness and status (spec 14.5.2) */
#define DEFAULT_BEAT_NS 1000000000    /* spec 5.9.6, provisional */
#define DEFAULT_DEAD_AFTER 3u
#define DEFAULT_SETTLE_NS 2000000
#define WALL_STALE_FACTOR 10          /* a heartbeat this many dead timeouts old on the wall
                                         clock is dead without watching; generous, so a wall
                                         clock jumping forward cannot kill a live process */
#define KNOWN_FLAGS (CDB_COORD_ALIVE | CDB_COORD_BATON_PENDING | CDB_COORD_JOURNAL | CDB_COORD_LOCKED \
                     | CDB_COORD_CLOSED)
#define LOBBY_PREFIX "join."
#define DIR_LOCK "lock"
#define PRIORITY_LIMIT (2u * CDB_COORD_MAX) /* priority values tried at join */
#define BATON_UNIT_NS 1000u           /* service window per declared unit of work (spec 5.9.8),
                                         provisional */
#define MAX_SHARE_NS 3600000000000ull /* the window's work share is capped at an hour */

/* -------------------------------------------------------- encoding */

void cdb_coord_encode(const cdb_coord_rec *r, cdb_byte_order order, uint8_t *h)
{
    unsigned i;
    memset(h, 0, CDB_COORD_HALF);
    memcpy(h, coord_magic, sizeof coord_magic);
    h[8] = (uint8_t)order;
    h[9] = (uint8_t)COORD_EPOCH;
    cdb_store_u16(h + 10, r->slot, order);
    h[12] = r->role;
    h[13] = r->flags;
    cdb_store_u16(h + 14, r->priority, order);
    memcpy(h + 16, r->db_id, 16);
    memcpy(h + 32, r->session, 16);
    cdb_store_u64(h + 48, r->principal, order);
    cdb_store_u64(h + 56, r->beat, order);
    cdb_store_u64(h + 64, r->beat_time, order);
    cdb_store_u64(h + 72, r->seq_hwm, order);
    cdb_store_u64(h + 80, r->tx_hwm, order);
    h[88] = r->req_kind;
    h[89] = r->status_op;
    cdb_store_u16(h + 90, r->heat, order);
    cdb_store_u32(h + 92, r->estimate_ms, order);
    cdb_store_u64(h + 96, r->req_time, order);
    cdb_store_u64(h + 104, r->x_total, order);
    cdb_store_u64(h + 112, r->cts, order);
    cdb_store_u64(h + 120, r->x, order);
    cdb_store_u64(h + 128, r->issued, order);
    cdb_store_u64(h + 136, r->clock, order);
    cdb_store_u64(h + 144, r->pinned, order);
    h[152] = r->nblocks;
    for (i = 0; i < r->nblocks && i < CDB_COORD_MAX_BLOCKS; i++) {
        cdb_store_u64(h + 160 + 16u * i, r->blocks[i].store, order);
        cdb_store_u64(h + 168 + 16u * i, r->blocks[i].first, order);
    }
    cdb_store_u64(h + 496, r->status_start, order);
    cdb_store_u32(h + 508, cdb_crc32c(h, 508), order);
}

/* Bytes [from, to) must be zero. */
static int zero(const uint8_t *h, unsigned from, unsigned to)
{
    for (; from < to; from++)
        if (h[from])
            return 0;
    return 1;
}

cdb_status cdb_coord_decode(const uint8_t *h, cdb_byte_order order, cdb_coord_rec *r)
{
    unsigned i;
    if (memcmp(h, coord_magic, sizeof coord_magic) != 0 || h[8] != (uint8_t)order)
        return CDB_E_CORRUPT;
    if (cdb_load_u32(h + 508, order) != cdb_crc32c(h, 508))
        return CDB_E_CORRUPT;
    if (h[9] != COORD_EPOCH)
        return CDB_E_UNSUPPORTED;
    memset(r, 0, sizeof *r);
    r->slot = cdb_load_u16(h + 10, order);
    r->role = h[12];
    r->flags = h[13];
    r->priority = cdb_load_u16(h + 14, order);
    memcpy(r->db_id, h + 16, 16);
    memcpy(r->session, h + 32, 16);
    r->principal = cdb_load_u64(h + 48, order);
    r->beat = cdb_load_u64(h + 56, order);
    r->beat_time = cdb_load_u64(h + 64, order);
    r->seq_hwm = cdb_load_u64(h + 72, order);
    r->tx_hwm = cdb_load_u64(h + 80, order);
    r->req_kind = h[88];
    r->status_op = h[89];
    r->heat = cdb_load_u16(h + 90, order);
    r->estimate_ms = cdb_load_u32(h + 92, order);
    r->req_time = cdb_load_u64(h + 96, order);
    r->x_total = cdb_load_u64(h + 104, order);
    r->cts = cdb_load_u64(h + 112, order);
    r->x = cdb_load_u64(h + 120, order);
    r->issued = cdb_load_u64(h + 128, order);
    r->clock = cdb_load_u64(h + 136, order);
    r->pinned = cdb_load_u64(h + 144, order);
    r->nblocks = h[152];
    r->status_start = cdb_load_u64(h + 496, order);
    if (r->role > CDB_COORD_PRIMARY || (r->flags & ~KNOWN_FLAGS) || r->req_kind > CDB_COORD_REQ_RECONCILE
        || r->nblocks > CDB_COORD_MAX_BLOCKS)
        return CDB_E_MALFORMED;
    if (!zero(h, 153, 160) || !zero(h, 160 + 16u * r->nblocks, 496) || !zero(h, 504, 508))
        return CDB_E_MALFORMED;
    for (i = 0; i < r->nblocks; i++) {
        r->blocks[i].store = cdb_load_u64(h + 160 + 16u * i, order);
        r->blocks[i].first = cdb_load_u64(h + 168 + 16u * i, order);
    }
    return CDB_OK;
}

int cdb_coord_beats(const cdb_coord_brief *a, const cdb_coord_brief *b)
{
    if (a->req_time != b->req_time)
        return a->req_time < b->req_time;
    if (a->priority != b->priority)
        return a->priority < b->priority;
    return memcmp(a->session, b->session, 16) < 0;
}

static void brief_of(const cdb_coord_rec *r, cdb_coord_brief *b)
{
    b->slot = r->slot;
    b->role = r->role;
    b->req_kind = r->req_kind;
    b->priority = r->priority;
    memcpy(b->session, r->session, 16);
    b->req_time = r->req_time;
    b->clock = r->clock;
    b->pinned = r->pinned;
    b->cts = r->cts;
    b->issued = r->issued;
    b->x = r->x;
    b->x_total = r->x_total;
}

/* -------------------------------------------------------- clocks */

/* The monotonic clock; without one, the wall clock; without either, 0. */
static int64_t mono_now(cdb_coord *c)
{
    int64_t ns;
    if (c->env.ops->mono != NULL && c->env.ops->mono(c->env.ctx, &ns) == CDB_OK)
        return ns;
    if (c->env.ops->now != NULL && c->env.ops->now(c->env.ctx, &ns) == CDB_OK)
        return ns;
    return 0;
}

static int wall_now(cdb_coord *c, int64_t *ns)
{
    return c->env.ops->now != NULL && c->env.ops->now(c->env.ctx, ns) == CDB_OK;
}

int64_t cdb_coord_clock(cdb_coord *c)
{
    return mono_now(c) + c->offset;
}

static uint64_t clock_us(cdb_coord *c)
{
    int64_t t = cdb_coord_clock(c);
    return t > 0 ? (uint64_t)(t / 1000) : 0u;
}

static int64_t dead_ns(const cdb_coord *c)
{
    return c->cfg.beat_ns * (int64_t)c->cfg.dead_after;
}

/* A heartbeat time far older than the dead timeout on the wall clock. */
static int wall_stale(cdb_coord *c, uint64_t beat_time_us)
{
    int64_t wall;
    uint64_t now_us;
    if (!wall_now(c, &wall) || wall <= 0)
        return 0;
    now_us = (uint64_t)(wall / 1000);
    return now_us > beat_time_us
           && now_us - beat_time_us > (uint64_t)(dead_ns(c) / 1000) * (uint64_t)WALL_STALE_FACTOR;
}

static void pause_for(cdb_coord *c, int64_t ns)
{
    if (c->env.ops->sleep != NULL)
        c->env.ops->sleep(c->env.ctx, ns);
}

/* -------------------------------------------------------- files */

/* Reads a slot or lobby file and keeps the valid half with the higher
 * heartbeat counter (spec 14.5.1). *valid is 0 for a missing file, a file of
 * another database, or one with no valid half. */
static cdb_status read_file(cdb_coord *c, const char *path, unsigned halves, cdb_coord_rec *out, int *valid)
{
    uint8_t h[CDB_COORD_HALF];
    cdb_coord_rec r;
    cdb_storage st;
    unsigned i;
    cdb_status s;

    *valid = 0;
    s = c->env.ops->open(c->env.ctx, path, CDB_ENV_READONLY, &st);
    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    if (s != CDB_OK)
        return s;
    for (i = 0; i < halves; i++) {
        if (cdb_storage_read(&st, (uint64_t)i * CDB_COORD_HALF, h, sizeof h) != CDB_OK)
            continue;
        if (cdb_coord_decode(h, c->order, &r) != CDB_OK || memcmp(r.db_id, c->me.db_id, 16) != 0)
            continue;
        if (!*valid || r.beat > out->beat) {
            *out = r;
            *valid = 1;
        }
    }
    cdb_storage_close(&st);
    return CDB_OK;
}

/* Writes this coordinator's record to the next half. Every write counts as
 * a heartbeat, so the counter always rises and the newest half wins. */
static cdb_status write_half(cdb_coord *c)
{
    uint8_t h[CDB_COORD_HALF];
    cdb_status s;
    c->me.beat++;
    c->me.beat_time = clock_us(c);
    if (c->me.role == CDB_COORD_PRIMARY)
        c->me.clock = c->me.beat_time;
    cdb_coord_encode(&c->me, c->order, h);
    if ((s = cdb_storage_program(&c->st, (uint64_t)c->next_half * CDB_COORD_HALF, h, sizeof h)) != CDB_OK)
        return s;
    c->next_half ^= 1u;
    return CDB_OK;
}

static void slot_path(const cdb_coord *c, unsigned slot, char *out)
{
    snprintf(out, CDB_COORD_PATH, "%s/%u", c->dir, slot);
}

static void lobby_path(const cdb_coord *c, const uint8_t *session, char *out)
{
    static const char hex[] = "0123456789abcdef";
    char name[2 * 16 + 1];
    unsigned i;
    for (i = 0; i < 16; i++) {
        name[2 * i] = hex[session[i] >> 4];
        name[2 * i + 1] = hex[session[i] & 15u];
    }
    name[32] = '\0';
    snprintf(out, CDB_COORD_PATH, "%s/" LOBBY_PREFIX "%s", c->dir, name);
}

/* -------------------------------------------------------- scanning */

struct names {
    unsigned max;
    uint64_t slots;                               /* bit s: coord/<s> exists */
    char lobby[CDB_COORD_MAX_LOBBY][48];
    unsigned nlobby;
};

static int on_name(void *arg, const char *name)
{
    struct names *n = (struct names *)arg;
    size_t len = strlen(name), i;
    if (len > 0 && len <= 2) {
        unsigned v = 0;
        for (i = 0; i < len; i++) {
            if (name[i] < '0' || name[i] > '9')
                return 0;
            v = v * 10u + (unsigned)(name[i] - '0');
        }
        if (v < n->max && (len == 1 || name[0] != '0'))
            n->slots |= (uint64_t)1 << v;
    } else if (len == sizeof LOBBY_PREFIX - 1 + 32 && strncmp(name, LOBBY_PREFIX, sizeof LOBBY_PREFIX - 1) == 0
               && n->nlobby < CDB_COORD_MAX_LOBBY) {
        memcpy(n->lobby[n->nlobby++], name, len + 1);
    }
    return 0;
}

/* Whether slot s's owner holds its lock (spec 5.8): an attempt that does
 * not wait, released at once when it succeeds. A slot this coordinator holds
 * itself (cdb_coord_hold_all) has no other owner. */
static cdb_status lock_held(cdb_coord *c, unsigned s, const char *path, int *held)
{
    cdb_lock *l;
    cdb_status st;
    if (c->held[s] != NULL) {
        *held = 0;
        return CDB_OK;
    }
    st = c->env.ops->lock_open(c->env.ctx, path, 1, &l);
    if (st == CDB_E_BUSY) {
        *held = 1;
        return CDB_OK;
    }
    if (st != CDB_OK)
        return st;
    c->env.ops->lock_close(c->env.ctx, l);
    *held = 0;
    return CDB_OK;
}

/* Reads every slot and lobby file and updates what is known about the
 * others: which are alive, which holds the primary role. A slot that says
 * it uses lock liveness is alive while its lock is held (spec 5.8). Any
 * other is alive while its counter has moved within the dead timeout, as
 * measured here; without a monotonic clock nothing can be measured and it
 * counts as dead. */
static cdb_status scan(cdb_coord *c)
{
    struct names n;
    cdb_coord_rec r;
    char path[CDB_COORD_PATH];
    int64_t now = mono_now(c);
    unsigned s, i;
    int valid;
    cdb_status st;

    memset(&n, 0, sizeof n);
    n.max = c->cfg.max_coords;
    if (c->env.ops->list != NULL && (st = c->env.ops->list(c->env.ctx, c->dir, on_name, &n)) != CDB_OK)
        return st;
    c->live_others = 0;
    c->primary = -1;
    c->max_cts = 0;
    c->journals_others = 0;
    c->min_pin = 0;
    c->scanned_at = now;
    for (s = 0; s < c->cfg.max_coords; s++) {
        cdb_coord_peer *p = &c->peers[s];
        if (!(n.slots & ((uint64_t)1 << s))) {
            p->seen = p->live = 0;
            continue;
        }
        slot_path(c, s, path);
        if ((st = read_file(c, path, 2, &r, &valid)) != CDB_OK)
            return st;
        /* Fencing counts every baton ever published, by the living or the
         * dead (spec 5.9.8). */
        if (valid && memcmp(r.session, c->me.session, 16) != 0) {
            if (r.cts > c->max_cts)
                c->max_cts = r.cts;
            if (r.flags & CDB_COORD_JOURNAL)
                c->journals_others++;
        }
        if (!valid || !(r.flags & CDB_COORD_ALIVE) || r.role == CDB_COORD_FREE
            || memcmp(r.session, c->me.session, 16) == 0) {
            p->seen = p->live = 0;
            continue;
        }
        if (c->use_locks && (r.flags & CDB_COORD_LOCKED)) {
            int held;
            if ((st = lock_held(c, s, path, &held)) != CDB_OK)
                return st;
            if (!p->seen || memcmp(p->b.session, r.session, 16) != 0 || p->b.cts != r.cts
                || p->b.issued != r.issued || p->b.x != r.x)
                p->moved_at = now;
            p->seen = p->confirmed = p->by_lock = 1;
            p->beat = r.beat;
            p->changed_at = now;
            brief_of(&r, &p->b);
            p->live = held;
        } else if (!p->seen || p->by_lock || memcmp(p->b.session, r.session, 16) != 0) {
            /* First sight: alive until watched for the dead timeout, unless
             * the wall clock already shows it long dead. */
            p->seen = 1;
            p->confirmed = 0;
            p->by_lock = 0;
            p->beat = r.beat;
            p->changed_at = wall_stale(c, r.beat_time) ? now - dead_ns(c) : now;
            p->moved_at = now;
        } else {
            if (p->beat != r.beat) {
                p->confirmed = 1;
                p->beat = r.beat;
                p->changed_at = now;
            }
            if (p->b.cts != r.cts || p->b.issued != r.issued || p->b.x != r.x)
                p->moved_at = now;
        }
        if (!p->by_lock) {
            brief_of(&r, &p->b);
            p->live = c->have_mono && now - p->changed_at < dead_ns(c);
        }
        if (p->live) {
            c->live_others++;
            if (r.pinned != 0 && (c->min_pin == 0 || r.pinned < c->min_pin))
                c->min_pin = r.pinned;
            if (r.role == CDB_COORD_PRIMARY && c->primary < 0)
                c->primary = (int)s;
        }
    }
    /* Lobby files live for one settle interval; one that is long stale on
     * the wall clock was left by a crashed joiner and is removed. */
    lobby_path(c, c->me.session, path);
    c->nlobby = 0;
    for (i = 0; i < n.nlobby; i++) {
        char lp[CDB_COORD_PATH];
        cdb_coord_lobby *l;
        snprintf(lp, sizeof lp, "%s/%s", c->dir, n.lobby[i]);
        if (strcmp(lp, path) == 0)
            continue;
        if ((st = read_file(c, lp, 1, &r, &valid)) != CDB_OK)
            return st;
        if (!valid || r.req_kind != CDB_COORD_REQ_JOIN)
            continue;
        if (wall_stale(c, r.beat_time)) {
            (void)c->env.ops->remove(c->env.ctx, lp);
            continue;
        }
        l = &c->lobby[c->nlobby++];
        memcpy(l->name, n.lobby[i], sizeof l->name);
        l->live = 1;
        brief_of(&r, &l->b);
    }
    return CDB_OK;
}

/* Scans until every coordinator that looks alive has been watched long
 * enough to be sure: its counter moved, or the dead timeout passed. */
static cdb_status settle_liveness(cdb_coord *c)
{
    cdb_status s;
    for (;;) {
        unsigned i;
        int unsure = 0;
        if ((s = scan(c)) != CDB_OK)
            return s;
        for (i = 0; i < c->cfg.max_coords; i++)
            if (c->peers[i].live && !c->peers[i].confirmed)
                unsure = 1;
        if (!unsure || !c->have_mono)
            return CDB_OK;
        pause_for(c, c->cfg.beat_ns / 4 > c->cfg.settle_ns ? c->cfg.beat_ns / 4 : c->cfg.settle_ns);
    }
}

/* A subordinate keeps its clock at or ahead of the primary's published
 * clock, so its stamps never run behind it (spec 14.5.3). */
static void follow_clock(cdb_coord *c)
{
    uint64_t pclock;
    int64_t t, mine;
    if (c->primary < 0)
        return;
    pclock = c->peers[c->primary].b.clock;
    if (pclock > (uint64_t)(INT64_MAX / 1000))
        return;
    t = (int64_t)(pclock * 1000u);
    mine = cdb_coord_clock(c);
    if (t > mine)
        c->offset += t - mine;
}

/* -------------------------------------------------------- roles */

/* Claims the primary role through append -> settle -> choose (spec 5.2,
 * 5.5). A live primary already holding it makes this one a subordinate. */
static cdb_status elect(cdb_coord *c)
{
    cdb_coord_brief mine;
    uint8_t kind = c->me.req_kind;  /* a baton request waiting meanwhile is kept */
    uint64_t at = c->me.req_time;
    unsigned i;
    int win = 1;
    cdb_status s;

    if ((s = scan(c)) != CDB_OK)
        return s;
    if (c->primary < 0) {
        c->me.req_kind = CDB_COORD_REQ_PRIMARY;
        c->me.req_time = clock_us(c);
        if ((s = write_half(c)) != CDB_OK)
            return s;
        pause_for(c, c->cfg.settle_ns);
        if ((s = scan(c)) != CDB_OK)
            return s;
        brief_of(&c->me, &mine);
        if (c->primary >= 0)
            win = 0;
        for (i = 0; i < c->cfg.max_coords && win; i++)
            if (c->peers[i].live && c->peers[i].b.req_kind == CDB_COORD_REQ_PRIMARY
                && cdb_coord_beats(&c->peers[i].b, &mine))
                win = 0;
    } else {
        win = 0;
    }
    c->me.role = (uint8_t)(win ? CDB_COORD_PRIMARY : CDB_COORD_SUB);
    c->me.req_kind = kind == CDB_COORD_REQ_BATON ? kind : (uint8_t)CDB_COORD_REQ_NONE;
    c->me.req_time = kind == CDB_COORD_REQ_BATON ? at : 0u;
    if (!win)
        follow_clock(c);
    return write_half(c);
}

/* Becomes the owner of slot s, carrying over the counters, held blocks and
 * journal flag of a dead previous owner (spec 5.9.2, 5.9.5): both halves are
 * rewritten so nothing of the old session can win the half comparison. With
 * the lock hook, the caller already holds the slot's lock; lobby is NULL. */
static cdb_status take_slot(cdb_coord *c, unsigned s, uint16_t priority, const char *lobby)
{
    cdb_coord_rec old;
    int valid;
    cdb_status st;

    slot_path(c, s, c->path);
    if ((st = read_file(c, c->path, 2, &old, &valid)) != CDB_OK)
        return st;
    c->me.slot = (uint16_t)s;
    c->me.priority = priority;
    c->me.role = CDB_COORD_SUB;
    c->me.flags = (uint8_t)(CDB_COORD_ALIVE | (c->use_locks ? CDB_COORD_LOCKED : 0u));
    c->me.req_kind = CDB_COORD_REQ_NONE;
    c->me.req_time = 0;
    if (valid) {
        c->me.beat = old.beat;
        c->me.seq_hwm = old.seq_hwm;
        c->me.tx_hwm = old.tx_hwm;
        c->me.cts = old.cts; /* its last baton; a dead session's baton is not held */
        c->me.flags = (uint8_t)(c->me.flags | (old.flags & CDB_COORD_JOURNAL));
        c->me.nblocks = old.nblocks;
        memcpy(c->me.blocks, old.blocks, sizeof old.blocks);
    }
    if ((st = c->env.ops->open(c->env.ctx, c->path, CDB_ENV_CREATE, &c->st)) != CDB_OK)
        return st;
    c->next_half = 0;
    if ((st = write_half(c)) != CDB_OK || (st = write_half(c)) != CDB_OK
        || (st = cdb_storage_sync(&c->st)) != CDB_OK) {
        cdb_storage_close(&c->st);
        return st;
    }
    c->joined = 1;
    /* The lobby file goes before the directory sync, so a crash never
     * leaves it durable behind a durable slot file. */
    if (lobby != NULL)
        (void)c->env.ops->remove(c->env.ctx, lobby);
    return c->env.ops->sync_dir(c->env.ctx, c->dir);
}

static void dir_lock_path(const cdb_coord *c, char *out)
{
    snprintf(out, CDB_COORD_PATH, "%s/" DIR_LOCK, c->dir);
}

static void unlock(cdb_coord *c, cdb_lock **l)
{
    if (*l != NULL) {
        c->env.ops->lock_close(c->env.ctx, *l);
        *l = NULL;
    }
}

/* Takes coord/lock shared, so no process removes the working directory while
 * this one joins (spec 5.9.5). A process removing it holds the lock
 * exclusively for a moment, and may have removed the folder: this waits it
 * out, recreating the folder, for at most the dead timeout. */
static cdb_status lock_dir_shared(cdb_coord *c, const char *parent)
{
    char path[CDB_COORD_PATH];
    int64_t start = mono_now(c);
    cdb_status s;
    dir_lock_path(c, path);
    for (;;) {
        s = c->env.ops->lock_open(c->env.ctx, path, 0, &c->dir_lock);
        if (s == CDB_OK)
            return CDB_OK;
        if (s != CDB_E_BUSY && s != CDB_E_NOT_FOUND)
            return s;
        if (!c->have_mono || mono_now(c) - start >= dead_ns(c))
            return CDB_E_BUSY;
        pause_for(c, c->cfg.settle_ns);
        if (s == CDB_E_NOT_FOUND) {
            if ((s = c->env.ops->mkdir(c->env.ctx, parent)) != CDB_OK
                || (s = c->env.ops->mkdir(c->env.ctx, c->dir)) != CDB_OK)
                return s;
        }
    }
}

/* With the lock hook: the lowest slot whose lock this coordinator gets, and
 * whose journal it can adopt, is its own; the system settles any race
 * between joiners (spec 5.9.5). */
static cdb_status join_by_lock(cdb_coord *c, cdb_coord_adoptable_fn adoptable, void *arg)
{
    char path[CDB_COORD_PATH];
    unsigned slot, p, i;
    cdb_status s;

    for (slot = 0; slot < c->cfg.max_coords; slot++) {
        cdb_coord_rec old;
        int valid;
        slot_path(c, slot, path);
        s = c->env.ops->lock_open(c->env.ctx, path, 1, &c->slot_lock);
        if (s == CDB_E_BUSY)
            continue;
        if (s != CDB_OK)
            return s;
        if (adoptable == NULL)
            break;
        if ((s = read_file(c, path, 2, &old, &valid)) != CDB_OK) {
            unlock(c, &c->slot_lock);
            return s;
        }
        if (!valid || !(old.flags & CDB_COORD_JOURNAL) || adoptable(arg, slot))
            break;
        unlock(c, &c->slot_lock); /* its journal waits for one that can */
    }
    if (slot == c->cfg.max_coords)
        return CDB_E_BUSY;
    for (p = 1; p <= PRIORITY_LIMIT; p++) {
        int used = 0;
        for (i = 0; i < c->cfg.max_coords && !used; i++)
            used = i != slot && c->peers[i].live && c->peers[i].b.priority == p;
        if (!used)
            break;
    }
    if (p > PRIORITY_LIMIT || (s = take_slot(c, slot, (uint16_t)p, NULL)) != CDB_OK) {
        unlock(c, &c->slot_lock);
        return p > PRIORITY_LIMIT ? CDB_E_BUSY : s;
    }
    return CDB_OK;
}

/* Writes the lobby request for slot s and priority p, settles, and checks
 * whether it won. */
static cdb_status try_lobby(cdb_coord *c, const char *lobby, unsigned s, uint16_t p, int *won, int *lost_slot)
{
    uint8_t h[CDB_COORD_HALF];
    cdb_coord_rec req = c->me;
    cdb_coord_brief mine;
    cdb_storage st;
    unsigned i;
    cdb_status r;

    req.slot = (uint16_t)s;
    req.priority = p;
    req.role = CDB_COORD_FREE;
    req.req_kind = CDB_COORD_REQ_JOIN;
    req.req_time = clock_us(c);
    req.beat = 1;
    req.beat_time = req.req_time;
    cdb_coord_encode(&req, c->order, h);
    if ((r = c->env.ops->open(c->env.ctx, lobby, CDB_ENV_CREATE, &st)) != CDB_OK)
        return r;
    r = cdb_storage_program(&st, 0, h, sizeof h);
    cdb_storage_close(&st);
    if (r != CDB_OK)
        return r;
    pause_for(c, c->cfg.settle_ns);
    if ((r = scan(c)) != CDB_OK)
        return r;
    brief_of(&req, &mine);
    *won = 1;
    *lost_slot = 0;
    if (c->peers[s].live) {
        *won = 0;
        *lost_slot = 1;
    }
    for (i = 0; i < c->cfg.max_coords; i++)
        if (c->peers[i].live && c->peers[i].b.priority == p)
            *won = 0;
    for (i = 0; i < c->nlobby; i++) {
        const cdb_coord_brief *o = &c->lobby[i].b;
        if ((o->slot == s || o->priority == p) && cdb_coord_beats(o, &mine)) {
            *won = 0;
            if (o->slot == s)
                *lost_slot = 1;
        }
    }
    return CDB_OK;
}

static void make_session(cdb_coord *c)
{
    int64_t a = mono_now(c), b = 0;
    unsigned i;
    if (c->env.ops->random != NULL && c->env.ops->random(c->env.ctx, c->me.session, 16) == CDB_OK)
        return;
    /* No random source: mix the clocks and this object's address. Unique
     * enough for one host; a micro-controller runs one process anyway. */
    (void)wall_now(c, &b);
    for (i = 0; i < 8; i++) {
        c->me.session[i] = (uint8_t)((uint64_t)a >> (8u * i));
        c->me.session[8 + i] = (uint8_t)(((uint64_t)b ^ (uint64_t)(size_t)c) >> (8u * i));
    }
}

cdb_status cdb_coord_join(cdb_coord *c, const cdb_env *env, const char *dir, const uint8_t db_id[16],
                          cdb_byte_order order, const cdb_coord_config *cfg)
{
    return cdb_coord_join_with(c, env, dir, db_id, order, cfg, NULL, NULL);
}

cdb_status cdb_coord_join_with(cdb_coord *c, const cdb_env *env, const char *dir, const uint8_t db_id[16],
                               cdb_byte_order order, const cdb_coord_config *cfg, cdb_coord_adoptable_fn adoptable,
                               void *arg)
{
    char lobby[CDB_COORD_PATH], parent[CDB_COORD_PATH];
    uint64_t lost_slots = 0;
    uint8_t lost_prio[PRIORITY_LIMIT + 1u];
    int64_t probe, wall;
    unsigned attempt;
    const char *slash;
    cdb_status s;
    int n;

    memset(c, 0, sizeof *c);
    c->env = *env;
    if (cfg != NULL)
        c->cfg = *cfg;
    if (c->cfg.max_coords == 0 || c->cfg.max_coords > CDB_COORD_MAX)
        c->cfg.max_coords = CDB_COORD_MAX;
    if (c->cfg.beat_ns <= 0)
        c->cfg.beat_ns = DEFAULT_BEAT_NS;
    if (c->cfg.dead_after == 0)
        c->cfg.dead_after = DEFAULT_DEAD_AFTER;
    if (c->cfg.settle_ns <= 0)
        c->cfg.settle_ns = DEFAULT_SETTLE_NS;
    c->order = order;
    c->primary = -1;
    memcpy(c->me.db_id, db_id, 16);
    n = snprintf(c->dir, sizeof c->dir, "%s", dir);
    if (n < 0 || (size_t)n >= sizeof c->dir)
        return CDB_E_INVALID_ARG;
    c->have_mono = env->ops->mono != NULL && env->ops->mono(env->ctx, &probe) == CDB_OK;
    c->use_locks = env->ops->lock_open != NULL && env->ops->lock_close != NULL;
    /* Stamps start on the wall clock where there is one (spec 14.4.4). */
    if (wall_now(c, &wall))
        c->offset = wall - mono_now(c);
    make_session(c);
    lobby_path(c, c->me.session, lobby);

    if ((s = env->ops->mkdir(env->ctx, c->dir)) != CDB_OK)
        return s;
    strcpy(parent, c->dir);
    if ((slash = strrchr(parent, '/')) != NULL) {
        parent[slash - parent] = '\0';
        if ((s = env->ops->sync_dir(env->ctx, parent)) != CDB_OK)
            return s;
    } else {
        strcpy(parent, ".");
    }
    if (c->use_locks && (s = lock_dir_shared(c, parent)) != CDB_OK)
        return s;
    if ((s = settle_liveness(c)) != CDB_OK) {
        unlock(c, &c->dir_lock);
        return s;
    }
    if (c->use_locks) {
        s = join_by_lock(c, adoptable, arg);
        unlock(c, &c->dir_lock); /* the slot's lock now keeps the directory */
        if (s != CDB_OK)
            return s;
        c->joined_at = c->last_beat = mono_now(c);
        s = elect(c);
        c->commit_ready = (c->live_others == 0);
        return s;
    }

    memset(lost_prio, 0, sizeof lost_prio);
    for (attempt = 0; attempt < 2u * CDB_COORD_MAX + 4u; attempt++) {
        unsigned slot, p;
        int won, lost_slot;
        for (slot = 0; slot < c->cfg.max_coords; slot++)
            if (!c->peers[slot].live && !(lost_slots & ((uint64_t)1 << slot)))
                break;
        if (slot == c->cfg.max_coords)
            return CDB_E_BUSY;
        for (p = 1; p <= PRIORITY_LIMIT; p++) {
            unsigned i;
            int used = lost_prio[p];
            for (i = 0; i < c->cfg.max_coords && !used; i++)
                used = c->peers[i].live && c->peers[i].b.priority == p;
            if (!used)
                break;
        }
        if (p > PRIORITY_LIMIT)
            return CDB_E_BUSY;
        if ((s = try_lobby(c, lobby, slot, (uint16_t)p, &won, &lost_slot)) != CDB_OK) {
            (void)env->ops->remove(env->ctx, lobby);
            return s;
        }
        if (won) {
            if ((s = take_slot(c, slot, (uint16_t)p, lobby)) != CDB_OK) {
                (void)env->ops->remove(env->ctx, lobby);
                return s;
            }
            c->joined_at = c->last_beat = mono_now(c);
            s = elect(c);
            c->commit_ready = (c->live_others == 0);
            return s;
        }
        (void)env->ops->remove(env->ctx, lobby);
        if (lost_slot)
            lost_slots |= (uint64_t)1 << slot;
        else
            lost_prio[p] = 1;
    }
    return CDB_E_BUSY;
}

cdb_status cdb_coord_beat(cdb_coord *c, int force)
{
    int64_t now = mono_now(c);
    cdb_status s;

    if (!c->joined)
        return CDB_E_STATE;
    if (!force && c->have_mono && now - c->last_beat < c->cfg.beat_ns)
        return CDB_OK;
    /* Silent past the dead timeout: in the fallback, another coordinator may
     * have taken this slot over (spec 5.9.6). Its lock keeps it otherwise. */
    if (!c->use_locks && now - c->last_beat >= dead_ns(c)) {
        cdb_coord_rec r;
        int valid;
        if ((s = read_file(c, c->path, 2, &r, &valid)) != CDB_OK)
            return s;
        if (!valid || memcmp(r.session, c->me.session, 16) != 0) {
            cdb_storage_close(&c->st);
            c->joined = 0;
            return CDB_E_BUSY;
        }
    }
    if ((s = write_half(c)) != CDB_OK)
        return s;
    c->last_beat = now;
    if ((s = scan(c)) != CDB_OK)
        return s;
    /* Two live primaries, after this one was silent and another was
     * elected: step down; the next election settles it. */
    if (c->me.role == CDB_COORD_PRIMARY && c->primary >= 0) {
        c->me.role = CDB_COORD_SUB;
        return write_half(c);
    }
    if (c->me.role != CDB_COORD_PRIMARY && c->primary < 0)
        return elect(c);
    follow_clock(c);
    return CDB_OK;
}

cdb_status cdb_coord_reserve_tx(cdb_coord *c, uint64_t hwm)
{
    cdb_status s;
    if (!c->joined)
        return CDB_E_STATE;
    if (hwm <= c->me.tx_hwm)
        return CDB_OK;
    c->me.tx_hwm = hwm;
    if ((s = write_half(c)) != CDB_OK)
        return s;
    return cdb_storage_sync(&c->st);
}

/* -------------------------------------------------------- baton */

/* The service window of a baton whose holder declared x_total units: the
 * dead timeout plus a share for the work (spec 5.9.8). */
static int64_t window_ns(const cdb_coord *c, uint64_t x_total)
{
    uint64_t share = x_total < MAX_SHARE_NS / BATON_UNIT_NS ? x_total * BATON_UNIT_NS : MAX_SHARE_NS;
    return dead_ns(c) + (int64_t)share;
}

/* Whether peer p holds the baton and has not timed out. A holder judged by
 * its lock never times out: it keeps the baton until it releases it or dies
 * (spec 5.9.8). Otherwise the window runs from its last change or from this
 * coordinator's request, whichever is later: a standing holder idle for an
 * hour has not timed out until someone has asked and waited. */
static int holds(const cdb_coord *c, const cdb_coord_peer *p, int64_t now)
{
    int64_t since;
    if (!p->live || p->b.issued == 0)
        return 0;
    if (p->by_lock)
        return 1;
    since = p->moved_at > c->req_at ? p->moved_at : c->req_at;
    return now - since < window_ns(c, p->b.x_total);
}

/* Withdraws a request that has not been granted. */
static cdb_status withdraw(cdb_coord *c)
{
    c->me.req_kind = CDB_COORD_REQ_NONE;
    c->me.req_time = 0;
    c->me.x_total = 0;
    return write_half(c);
}

/* Request, then grant (spec 5.9.8): granted when no live slot holds the
 * baton within its window and no live request is earlier by the rule of
 * spec 5.5. The winner publishes the next CTS, settles and checks again: a
 * higher CTS elsewhere, or the same CTS with an earlier request, means
 * another took it at the same moment, and this one backs off. Taking it from
 * a holder that is still alive but timed out waits one heartbeat interval
 * more, so that holder's guard sees the new CTS before anything is written
 * (cdb_coord_guard). */
cdb_status cdb_coord_baton_try(cdb_coord *c, uint64_t x_total, int *granted)
{
    cdb_coord_brief mine;
    uint64_t old_cts;
    int64_t now;
    int overrode = 0;
    unsigned i;
    cdb_status s;

    *granted = 0;
    if (!c->joined)
        return CDB_E_STATE;
    if (c->me.issued != 0) {
        *granted = 1;
        return CDB_OK;
    }
    if (c->me.req_kind != CDB_COORD_REQ_BATON) {
        c->me.req_kind = CDB_COORD_REQ_BATON;
        c->me.req_time = clock_us(c);
        c->me.x_total = x_total;
        c->me.x = 0;
        c->req_at = mono_now(c);
        if ((s = write_half(c)) != CDB_OK)
            return s;
    }
    if ((s = scan(c)) != CDB_OK)
        return s;
    now = mono_now(c);
    brief_of(&c->me, &mine);
    for (i = 0; i < c->cfg.max_coords; i++) {
        const cdb_coord_peer *p = &c->peers[i];
        if (holds(c, p, now))
            return CDB_OK;
        if (p->live && p->b.issued != 0)
            overrode = 1;
        if (p->live && p->b.req_kind == CDB_COORD_REQ_BATON && p->b.issued == 0 && cdb_coord_beats(&p->b, &mine))
            return CDB_OK;
    }
    old_cts = c->me.cts;
    c->me.cts = (c->max_cts > c->me.cts ? c->max_cts : c->me.cts) + 1u;
    c->me.issued = clock_us(c);
    if (c->me.issued == 0)
        c->me.issued = 1; /* no clock: still nonzero */
    c->me.x = 0;
    c->me.x_total = x_total;
    if ((s = write_half(c)) != CDB_OK)
        return s;
    if (c->live_others > 0) {
        int lost;
        pause_for(c, c->cfg.settle_ns);
        if ((s = scan(c)) != CDB_OK)
            return s;
        lost = c->max_cts > c->me.cts;
        for (i = 0; i < c->cfg.max_coords && !lost; i++) {
            const cdb_coord_peer *p = &c->peers[i];
            lost = p->live && p->b.issued != 0 && p->b.cts == c->me.cts && cdb_coord_beats(&p->b, &mine);
        }
        if (lost) {
            c->me.cts = old_cts;
            c->me.issued = 0;
            return write_half(c);
        }
        if (overrode)
            pause_for(c, c->cfg.beat_ns + c->cfg.settle_ns);
    }
    *granted = 1;
    return CDB_OK;
}

cdb_status cdb_coord_baton_take(cdb_coord *c, uint64_t x_total, int *granted)
{
    int64_t start = mono_now(c);
    cdb_status s;

    for (;;) {
        int64_t cap = c->cfg.baton_wait_ns;
        unsigned i;
        if ((s = cdb_coord_baton_try(c, x_total, granted)) != CDB_OK || *granted)
            return s;
        if (cap <= 0) {
            cap = window_ns(c, 0);
            for (i = 0; i < c->cfg.max_coords; i++)
                if (c->peers[i].live && c->peers[i].b.issued != 0 && window_ns(c, c->peers[i].b.x_total) > cap)
                    cap = window_ns(c, c->peers[i].b.x_total);
            cap += dead_ns(c);
        }
        if (!c->have_mono || mono_now(c) - start >= cap)
            return withdraw(c);
        pause_for(c, c->cfg.beat_ns / 4 > c->cfg.settle_ns ? c->cfg.beat_ns / 4 : c->cfg.settle_ns);
        if ((s = cdb_coord_beat(c, 0)) != CDB_OK)
            return s;
    }
}

void cdb_coord_baton_progress(cdb_coord *c, uint64_t x)
{
    c->me.x = x;
}

cdb_status cdb_coord_baton_release(cdb_coord *c)
{
    if (!c->joined)
        return CDB_OK;
    if (c->me.issued == 0 && c->me.req_kind != CDB_COORD_REQ_BATON)
        return CDB_OK;
    c->me.issued = 0;
    c->me.x = 0;
    return withdraw(c);
}

int cdb_coord_baton_held(const cdb_coord *c)
{
    return c->joined && c->me.issued != 0;
}

int cdb_coord_baton_wanted(const cdb_coord *c)
{
    unsigned i;
    for (i = 0; i < c->cfg.max_coords; i++)
        if (c->peers[i].live && c->peers[i].b.issued == 0
            && (c->peers[i].b.req_kind == CDB_COORD_REQ_BATON || c->peers[i].b.req_kind == CDB_COORD_REQ_RECONCILE))
            return 1;
    return 0;
}

cdb_status cdb_coord_request_reconcile(cdb_coord *c, int on)
{
    if (!c->joined)
        return CDB_E_STATE;
    if (on) {
        if (c->me.issued != 0 || c->me.req_kind == CDB_COORD_REQ_BATON || c->me.req_kind == CDB_COORD_REQ_RECONCILE)
            return CDB_OK;
        c->me.req_kind = CDB_COORD_REQ_RECONCILE;
        c->me.req_time = clock_us(c);
    } else {
        if (c->me.req_kind != CDB_COORD_REQ_RECONCILE)
            return CDB_OK;
        c->me.req_kind = CDB_COORD_REQ_NONE;
        c->me.req_time = 0;
    }
    return write_half(c);
}

cdb_status cdb_coord_baton_fence(cdb_coord *c)
{
    cdb_status s;
    if (!cdb_coord_baton_held(c))
        return CDB_E_BUSY;
    if ((s = scan(c)) != CDB_OK)
        return s;
    return c->max_cts > c->me.cts ? CDB_E_BUSY : CDB_OK;
}

/* The lease margin (spec 5.9.8, fallback). Silent for half the dead timeout,
 * this holder may already look dead to others: it writes a heartbeat and
 * goes on only if no other coordinator is alive to have taken over.
 * Otherwise it heartbeats when one is due, and a higher CTS seen by that
 * scan means the baton has passed on. With the lock hook nobody takes the
 * baton from a live holder, so there is no margin; without a monotonic clock
 * there are no timeouts, for this coordinator or anyone. */
cdb_status cdb_coord_guard(cdb_coord *c)
{
    int64_t now = mono_now(c);
    cdb_status s;
    if (!cdb_coord_baton_held(c))
        return CDB_E_BUSY;
    if (!c->have_mono)
        return CDB_OK;
    if (!c->use_locks && now - c->last_beat >= dead_ns(c) / 2) {
        if ((s = cdb_coord_beat(c, 1)) != CDB_OK)
            return s;
        if (c->live_others > 0)
            return CDB_E_BUSY;
    } else if (now - c->last_beat >= c->cfg.beat_ns && (s = cdb_coord_beat(c, 0)) != CDB_OK) {
        return s;
    }
    return c->max_cts > c->me.cts ? CDB_E_BUSY : CDB_OK;
}

cdb_status cdb_coord_set_journal(cdb_coord *c, int in_use)
{
    cdb_status s;
    if (!c->joined)
        return CDB_E_STATE;
    c->me.flags = (uint8_t)(in_use ? (c->me.flags | CDB_COORD_JOURNAL) : (c->me.flags & ~CDB_COORD_JOURNAL));
    if ((s = write_half(c)) != CDB_OK)
        return s;
    return cdb_storage_sync(&c->st);
}

cdb_status cdb_coord_publish_pin(cdb_coord *c, uint64_t seq)
{
    if (!c->joined)
        return CDB_E_STATE;
    if (c->me.pinned == seq)
        return CDB_OK;
    c->me.pinned = seq;
    return write_half(c);
}

cdb_status cdb_coord_claim_block(cdb_coord *c, uint64_t store, uint64_t floor, uint64_t *first)
{
    uint8_t old_kind;
    uint64_t old_time, cand;
    unsigned sl, i;
    cdb_status st;

    if (!c->joined)
        return CDB_E_STATE;
    if (c->me.nblocks >= CDB_COORD_MAX_BLOCKS)
        return CDB_E_OVERFLOW;
    if (floor == 0)
        floor = 1;
    old_kind = c->me.req_kind;
    old_time = c->me.req_time;

    for (;;) {
        cdb_coord_rec r;
        int valid, lost;

        if ((st = scan(c)) != CDB_OK)
            return st;
        cand = floor;
        for (;;) {
            int moved = 0;
            for (i = 0; i < c->me.nblocks; i++) {
                if (c->me.blocks[i].store == store
                    && cand < c->me.blocks[i].first + CDB_COORD_ID_BLOCK
                    && c->me.blocks[i].first < cand + CDB_COORD_ID_BLOCK) {
                    cand = c->me.blocks[i].first + CDB_COORD_ID_BLOCK;
                    moved = 1;
                }
            }
            for (sl = 0; sl < c->cfg.max_coords; sl++) {
                if (sl == c->me.slot)
                    continue;
                if ((st = cdb_coord_read_slot(c, sl, &r, &valid)) != CDB_OK)
                    return st;
                if (!valid)
                    continue;
                for (i = 0; i < r.nblocks; i++) {
                    if (r.blocks[i].store == store
                        && cand < r.blocks[i].first + CDB_COORD_ID_BLOCK
                        && r.blocks[i].first < cand + CDB_COORD_ID_BLOCK) {
                        cand = r.blocks[i].first + CDB_COORD_ID_BLOCK;
                        moved = 1;
                    }
                }
            }
            if (!moved)
                break;
        }

        c->me.blocks[c->me.nblocks].store = store;
        c->me.blocks[c->me.nblocks].first = cand;
        c->me.nblocks++;

        if (c->live_others == 0) {
            if ((st = write_half(c)) != CDB_OK) {
                c->me.nblocks--;
                return st;
            }
            *first = cand;
            return CDB_OK;
        }

        c->me.req_kind = CDB_COORD_REQ_ID_BLOCK;
        c->me.req_time = clock_us(c);
        if ((st = write_half(c)) != CDB_OK) {
            c->me.nblocks--;
            c->me.req_kind = old_kind == CDB_COORD_REQ_BATON ? old_kind : (uint8_t)CDB_COORD_REQ_NONE;
            c->me.req_time = old_kind == CDB_COORD_REQ_BATON ? old_time : 0u;
            return st;
        }
        pause_for(c, c->cfg.settle_ns);
        if ((st = scan(c)) != CDB_OK) {
            c->me.nblocks--;
            c->me.req_kind = old_kind == CDB_COORD_REQ_BATON ? old_kind : (uint8_t)CDB_COORD_REQ_NONE;
            c->me.req_time = old_kind == CDB_COORD_REQ_BATON ? old_time : 0u;
            return st;
        }

        lost = 0;
        for (sl = 0; sl < c->cfg.max_coords; sl++) {
            if (sl == c->me.slot)
                continue;
            if ((st = cdb_coord_read_slot(c, sl, &r, &valid)) != CDB_OK) {
                c->me.nblocks--;
                c->me.req_kind = old_kind == CDB_COORD_REQ_BATON ? old_kind : (uint8_t)CDB_COORD_REQ_NONE;
                c->me.req_time = old_kind == CDB_COORD_REQ_BATON ? old_time : 0u;
                return st;
            }
            if (!valid)
                continue;
            for (i = 0; i < r.nblocks; i++) {
                if (r.blocks[i].store != store
                    || cand >= r.blocks[i].first + CDB_COORD_ID_BLOCK
                    || r.blocks[i].first >= cand + CDB_COORD_ID_BLOCK)
                    continue;
                if (c->peers[sl].live && r.req_kind == CDB_COORD_REQ_ID_BLOCK && i + 1u == r.nblocks) {
                    cdb_coord_brief pb, mine;
                    brief_of(&r, &pb);
                    brief_of(&c->me, &mine);
                    if (!cdb_coord_beats(&pb, &mine))
                        continue;
                }
                lost = 1;
                if (r.blocks[i].first + CDB_COORD_ID_BLOCK > floor)
                    floor = r.blocks[i].first + CDB_COORD_ID_BLOCK;
            }
        }

        c->me.req_kind = old_kind == CDB_COORD_REQ_BATON ? old_kind : (uint8_t)CDB_COORD_REQ_NONE;
        c->me.req_time = old_kind == CDB_COORD_REQ_BATON ? old_time : 0u;
        if (!lost) {
            if ((st = write_half(c)) != CDB_OK)
                return st;
            *first = cand;
            return CDB_OK;
        }
        c->me.nblocks--;
        if ((st = write_half(c)) != CDB_OK)
            return st;
    }
}

cdb_status cdb_coord_drop_block(cdb_coord *c, uint64_t store, uint64_t first)
{
    unsigned i, j = 0;
    if (!c->joined)
        return CDB_E_STATE;
    for (i = 0; i < c->me.nblocks; i++) {
        if (c->me.blocks[i].store == store && c->me.blocks[i].first == first)
            continue;
        c->me.blocks[j++] = c->me.blocks[i];
    }
    if (j == c->me.nblocks)
        return CDB_OK;
    memset(&c->me.blocks[j], 0, (c->me.nblocks - j) * sizeof c->me.blocks[0]);
    c->me.nblocks = (uint8_t)j;
    return write_half(c);
}

cdb_status cdb_coord_commit_ready(cdb_coord *c)
{
    cdb_status s;
    if (!c->joined)
        return CDB_E_STATE;
    if (!c->have_mono)
        return CDB_OK;
    if (mono_now(c) - c->scanned_at >= c->cfg.beat_ns / 2) {
        if ((s = cdb_coord_beat(c, 1)) != CDB_OK)
            return s;
    }
    if (!c->commit_ready) {
        int64_t wait = c->cfg.beat_ns + c->cfg.settle_ns;
        while (mono_now(c) - c->joined_at < wait) {
            int64_t rem = wait - (mono_now(c) - c->joined_at);
            int64_t step = c->cfg.beat_ns / 2 > 0 && c->cfg.beat_ns / 2 < rem ? c->cfg.beat_ns / 2 : rem;
            pause_for(c, step);
            if ((s = cdb_coord_beat(c, 0)) != CDB_OK)
                return s;
        }
        if ((s = cdb_coord_beat(c, 1)) != CDB_OK)
            return s;
        c->commit_ready = 1;
    }
    return CDB_OK;
}

cdb_status cdb_coord_read_slot(cdb_coord *c, unsigned s, cdb_coord_rec *r, int *valid)
{
    char path[CDB_COORD_PATH];
    slot_path(c, s, path);
    return read_file(c, path, 2, r, valid);
}

void cdb_coord_release_all(cdb_coord *c)
{
    unsigned s;
    for (s = 0; s < CDB_COORD_MAX; s++)
        unlock(c, &c->held[s]);
    unlock(c, &c->dir_lock);
    c->holding_all = 0;
}

cdb_status cdb_coord_hold_all(cdb_coord *c, int dir, int *all)
{
    char path[CDB_COORD_PATH];
    struct names n;
    unsigned s;
    cdb_status st;

    *all = 0;
    if (!c->joined)
        return CDB_E_STATE;
    if (c->holding_all) {
        *all = 1;
        return CDB_OK;
    }
    if (!c->use_locks) {
        if ((st = scan(c)) != CDB_OK)
            return st;
        *all = c->holding_all = c->live_others == 0;
        return CDB_OK;
    }
    if (dir) {
        dir_lock_path(c, path);
        st = c->env.ops->lock_open(c->env.ctx, path, 1, &c->dir_lock);
        if (st == CDB_E_BUSY)
            return CDB_OK; /* a process is joining */
        if (st != CDB_OK)
            return st;
    }
    memset(&n, 0, sizeof n);
    n.max = c->cfg.max_coords;
    if (c->env.ops->list != NULL && (st = c->env.ops->list(c->env.ctx, c->dir, on_name, &n)) != CDB_OK) {
        cdb_coord_release_all(c);
        return st;
    }
    for (s = 0; s < c->cfg.max_coords; s++) {
        if (s == c->me.slot || !(n.slots & ((uint64_t)1 << s)) || c->held[s] != NULL)
            continue;
        slot_path(c, s, path);
        st = c->env.ops->lock_open(c->env.ctx, path, 1, &c->held[s]);
        if (st != CDB_OK) {
            cdb_coord_release_all(c);
            return st == CDB_E_BUSY ? CDB_OK : st;
        }
    }
    c->holding_all = 1;
    *all = 1;
    return scan(c); /* every other slot is now known dead */
}

cdb_status cdb_coord_remove_slot(cdb_coord *c, unsigned s)
{
    char path[CDB_COORD_PATH];
    cdb_status st;
    if (s >= c->cfg.max_coords || (c->joined && s == c->me.slot) || !c->holding_all
        || (c->use_locks && c->held[s] == NULL))
        return CDB_E_STATE;
    slot_path(c, s, path);
    if ((st = c->env.ops->remove(c->env.ctx, path)) != CDB_OK && st != CDB_E_NOT_FOUND)
        return st;
    return c->env.ops->sync_dir(c->env.ctx, c->dir);
}

/* The slot file goes while its lock is still held, so no joiner takes the
 * slot and then sees its file vanish. */
cdb_status cdb_coord_leave(cdb_coord *c)
{
    cdb_status s;
    if (!c->joined)
        return CDB_OK;
    c->joined = 0;
    cdb_storage_close(&c->st);
    if ((s = c->env.ops->remove(c->env.ctx, c->path)) != CDB_OK && s != CDB_E_NOT_FOUND)
        return s;
    s = c->env.ops->sync_dir(c->env.ctx, c->dir);
    unlock(c, &c->slot_lock);
    return s;
}

cdb_status cdb_coord_close_pending(cdb_coord *c)
{
    cdb_status s;
    if (!c->joined)
        return CDB_E_STATE;
    c->me.flags = (uint8_t)((c->me.flags & ~CDB_COORD_ALIVE) | CDB_COORD_CLOSED);
    c->me.role = CDB_COORD_SUB;
    c->me.req_kind = CDB_COORD_REQ_NONE;
    c->me.req_time = 0;
    c->me.issued = 0;
    c->me.status_op = CDB_COORD_OP_IDLE;
    c->me.estimate_ms = 0;
    c->me.status_start = 0;
    if ((s = write_half(c)) != CDB_OK || (s = cdb_storage_sync(&c->st)) != CDB_OK)
        return s;
    c->joined = 0;
    cdb_storage_close(&c->st);
    unlock(c, &c->slot_lock);
    return CDB_OK;
}

cdb_status cdb_coord_remove_dir_lock(cdb_coord *c)
{
    char path[CDB_COORD_PATH];
    cdb_status s;
    if (c->dir_lock == NULL || !c->holding_all)
        return CDB_E_STATE;
    dir_lock_path(c, path);
    if ((s = c->env.ops->remove(c->env.ctx, path)) != CDB_OK && s != CDB_E_NOT_FOUND)
        return s;
    return CDB_OK;
}

void cdb_coord_abandon(cdb_coord *c)
{
    if (c->joined)
        cdb_storage_close(&c->st);
    c->joined = 0;
    if (c->use_locks) {
        unlock(c, &c->slot_lock);
        cdb_coord_release_all(c);
    }
}
