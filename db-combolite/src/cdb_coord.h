/* Coordinator: the in-process stand-in for a database server (spec 5,
 * 5.9, 14.5).
 *
 * Every process that opens a database runs one coordinator. Coordinators
 * share nothing but files in the working directory's coord/ folder:
 *   coord/<slot>            a slot file, written only by its owner: two
 *                           512-byte halves, A and B, written in turn
 *   coord/join.<session>    a lobby file, written only by the joiner whose
 *                           session identity names it, while it claims a slot
 * Claims (a slot, the primary role) follow append -> settle -> choose
 * (spec 5.5): write the request, wait the settle interval, and let the same
 * rule pick the winner everywhere: earliest request time, then the lower
 * priority value, then the lower session identity.
 *
 * A coordinator is alive while its heartbeat counter keeps changing. Each
 * observer measures that on its own monotonic clock, so no two processes'
 * clocks have to agree for liveness; a heartbeat already older than the dead
 * timeout on the wall clock counts as dead at once. There are no threads:
 * cdb_coord_beat writes a heartbeat when one is due, and callers call it from
 * their own entry points (spec 5.9.6). */
#ifndef CDB_COORD_H
#define CDB_COORD_H

#include "combolite/cdb_env.h"
#include "cdb_endian.h"

#define CDB_COORD_HALF 512u          /* bytes per slot half (spec 14.5.2) */
#define CDB_COORD_MAX 64u            /* most coordinators this build tracks */
#define CDB_COORD_MAX_BLOCKS 21u     /* held entry-ID blocks per slot (spec 5.9.3) */
#define CDB_COORD_MAX_LOBBY 16u      /* lobby files looked at per scan */
#define CDB_COORD_DIR 172u           /* longest coord/ folder path, with its zero */
#define CDB_COORD_PATH 220u          /* the folder plus "/" and the longest name */

/* Roles (offset 12). */
#define CDB_COORD_FREE 0u
#define CDB_COORD_SUB 1u
#define CDB_COORD_PRIMARY 2u

/* State flags (offset 13). */
#define CDB_COORD_ALIVE 0x01u
#define CDB_COORD_BATON_PENDING 0x02u
#define CDB_COORD_JOURNAL 0x04u

/* Request kinds (offset 88). */
#define CDB_COORD_REQ_NONE 0u
#define CDB_COORD_REQ_BATON 1u
#define CDB_COORD_REQ_PRIMARY 2u
#define CDB_COORD_REQ_PRIORITY 3u
#define CDB_COORD_REQ_JOIN 4u        /* in a lobby file: the slot and priority wanted */
#define CDB_COORD_REQ_ID_BLOCK 5u

/* One slot half, decoded (spec 14.5.2). Times are in microseconds of the
 * primary's clock. */
typedef struct cdb_coord_rec {
    uint16_t slot;
    uint8_t role;
    uint8_t flags;
    uint16_t priority;           /* 1 is the highest */
    uint8_t db_id[16];
    uint8_t session[16];
    uint64_t principal;
    uint64_t beat;               /* heartbeat counter */
    uint64_t beat_time;
    uint64_t seq_hwm;            /* delta-sequence high-water mark */
    uint64_t tx_hwm;             /* transaction-counter high-water mark */
    uint8_t req_kind;
    uint16_t heat;
    uint64_t req_time;
    uint64_t x_total;
    uint64_t cts;
    uint64_t x;
    uint64_t issued;
    uint64_t clock;              /* published by the primary */
    uint64_t pinned;             /* archive committed sequence its readers use */
    uint8_t nblocks;
    struct cdb_coord_block {
        uint64_t store;
        uint64_t first;
    } blocks[CDB_COORD_MAX_BLOCKS];
} cdb_coord_rec;

/* Encodes r into one 512-byte half. */
void cdb_coord_encode(const cdb_coord_rec *r, cdb_byte_order order, uint8_t *out);
/* Decodes and checks one half. CDB_E_CORRUPT for a wrong magic, byte order
 * or CRC; CDB_E_UNSUPPORTED for another epoch; CDB_E_MALFORMED for nonzero
 * reserved bytes, an unknown role or too many blocks. */
cdb_status cdb_coord_decode(const uint8_t *in, cdb_byte_order order, cdb_coord_rec *r);

/* Timing and capacity. Zero fields take the provisional defaults of spec
 * 5.9.6; they move to the device-class table once measured. */
typedef struct cdb_coord_config {
    unsigned max_coords;         /* 0: 64; at most CDB_COORD_MAX */
    int64_t beat_ns;             /* heartbeat interval; 0: 1 s */
    unsigned dead_after;         /* heartbeat intervals before a coordinator is dead; 0: 3 */
    int64_t settle_ns;           /* settle interval (spec 5.5); 0: 2 ms */
    int64_t baton_wait_ns;       /* longest wait for the baton; 0: the holder's service
                                    window plus the dead timeout (spec 5.9.8) */
} cdb_coord_config;

/* The parts of another coordinator's record that the choices here use. */
typedef struct cdb_coord_brief {
    uint16_t slot;
    uint8_t role;
    uint8_t req_kind;
    uint16_t priority;
    uint8_t session[16];
    uint64_t req_time;
    uint64_t clock;
    uint64_t pinned;
    uint64_t cts;
    uint64_t issued;             /* nonzero while it holds the baton */
    uint64_t x, x_total;
} cdb_coord_brief;

/* What one observer knows about another coordinator: when its counter last
 * changed, on the observer's monotonic clock. */
typedef struct cdb_coord_peer {
    int seen;
    int confirmed;               /* its counter changed while watched */
    int live;
    uint64_t beat;
    int64_t changed_at;
    int64_t moved_at;            /* when its baton or progress last changed */
    cdb_coord_brief b;
} cdb_coord_peer;

typedef struct cdb_coord_lobby {
    char name[48];
    int live;
    cdb_coord_brief b;
} cdb_coord_lobby;

typedef struct cdb_coord {
    cdb_env env;
    cdb_coord_config cfg;
    cdb_byte_order order;
    char dir[CDB_COORD_DIR];     /* the coord/ folder */
    char path[CDB_COORD_PATH];   /* this coordinator's slot file */
    cdb_storage st;
    int joined;
    unsigned next_half;          /* the half the next write goes to */
    cdb_coord_rec me;
    int have_mono;
    int64_t offset;              /* primary clock = monotonic clock + offset */
    int64_t last_beat;           /* monotonic time of the last heartbeat written */
    int64_t joined_at;
    cdb_coord_peer peers[CDB_COORD_MAX];
    cdb_coord_lobby lobby[CDB_COORD_MAX_LOBBY];
    unsigned nlobby;
    unsigned live_others;        /* other live coordinators at the last scan */
    int primary;                 /* slot of the live primary at the last scan; -1 none */
    uint64_t max_cts;            /* highest CTS any other slot file shows, live or not */
    unsigned journals_others;    /* other slot files whose journal is in use, live or not */
    uint64_t min_pin;            /* lowest sequence a live other publishes (spec 5.9.7); 0 none */
    int64_t scanned_at;          /* monotonic time of the last scan */
    int64_t req_at;              /* monotonic time of this coordinator's baton request */
} cdb_coord;

/* Joins the coordinators of the database in dir (created if missing; its
 * parent must exist). Claims a slot and a priority value through the lobby
 * (spec 5.9.5), then the primary role if no live coordinator holds it
 * (spec 5.2). May wait: the settle interval, and, for any other coordinator
 * that looks alive but has not been watched yet, until its heartbeat moves
 * or the dead timeout passes.
 * CDB_E_BUSY when every slot is held by a live coordinator. */
cdb_status cdb_coord_join(cdb_coord *c, const cdb_env *env, const char *dir, const uint8_t db_id[16],
                          cdb_byte_order order, const cdb_coord_config *cfg);

/* Writes a heartbeat if one is due (force: now), refreshes what is known
 * about the others, and elects a new primary if the old one died.
 * CDB_E_BUSY if another coordinator has taken this one's slot, which
 * happens after this process was silent past the dead timeout. */
cdb_status cdb_coord_beat(cdb_coord *c, int force);

/* Raises the slot's transaction-counter high-water mark to hwm and makes it
 * durable (spec 14.4.3): counters below it may then be used. */
cdb_status cdb_coord_reserve_tx(cdb_coord *c, uint64_t hwm);

/* The baton (spec 5.3, 5.4, 5.9.8): the right to write the archive. A slot
 * holds it while its issue time is nonzero.
 *
 * cdb_coord_baton_take requests it, declaring x_total units of work, and
 * waits: it sleeps through the environment and heartbeats, until the baton
 * is granted or the holder times out. *granted is 0 when the wait cap
 * passed; the request is then withdrawn. With no other live coordinator it
 * is granted at once, with no settle. Holding it already is a grant. */
cdb_status cdb_coord_baton_take(cdb_coord *c, uint64_t x_total, int *granted);

/* One attempt, without waiting; leaves the request in place on a miss. */
cdb_status cdb_coord_baton_try(cdb_coord *c, uint64_t x_total, int *granted);

/* Publishes progress x of the work declared; written with the next
 * heartbeat. */
void cdb_coord_baton_progress(cdb_coord *c, uint64_t x);

/* Gives the baton up and withdraws any request. */
cdb_status cdb_coord_baton_release(cdb_coord *c);

/* Nonzero while this coordinator holds the baton. */
int cdb_coord_baton_held(const cdb_coord *c);

/* Nonzero if another live coordinator was requesting the baton at the last
 * scan. */
int cdb_coord_baton_wanted(const cdb_coord *c);

/* Before an archive commit: scans, and CDB_E_BUSY if this coordinator no
 * longer holds the highest CTS (spec 5.9.8, fencing). */
cdb_status cdb_coord_baton_fence(cdb_coord *c);

/* Before every archive write: CDB_E_BUSY once this coordinator has gone
 * half the dead timeout without a heartbeat (the lease margin), else writes
 * a heartbeat if one is due. */
cdb_status cdb_coord_guard(cdb_coord *c);

/* Sets or clears the slot's "journal in use" flag and makes it durable
 * (spec 14.4.7 rule 7). */
cdb_status cdb_coord_set_journal(cdb_coord *c, int in_use);

/* Publishes the archive committed sequence this process reads (spec 5.9.7). */
cdb_status cdb_coord_publish_pin(cdb_coord *c, uint64_t seq);

/* Reads slot s's file: *valid is 0 when there is none, or none of this
 * database. */
cdb_status cdb_coord_read_slot(cdb_coord *c, unsigned s, cdb_coord_rec *r, int *valid);

/* Removes slot s's file, once a merge has taken in everything it stood for.
 * CDB_E_STATE for this coordinator's own slot or a live one. */
cdb_status cdb_coord_remove_slot(cdb_coord *c, unsigned s);

/* Leaves cleanly: removes the slot file. */
cdb_status cdb_coord_leave(cdb_coord *c);

/* The primary's clock now, in nanoseconds (spec 14.5.3). */
int64_t cdb_coord_clock(cdb_coord *c);

/* The choose rule of spec 5.5: nonzero if a's request beats b's. */
int cdb_coord_beats(const cdb_coord_brief *a, const cdb_coord_brief *b);

#endif
