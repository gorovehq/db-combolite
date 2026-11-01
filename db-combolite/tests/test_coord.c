/* Coordinators (spec 5, 5.9, 14.5): the slot record, joining, the primary
 * role, liveness, and a second process refused while one has the database
 * open. Several coordinators run in this one thread on one fault
 * environment; its sleep hook lets the others beat while one waits, as other
 * processes would. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "cdb_coord.h"
#include "cdb_crc32c.h"
#include "cdb_db.h"
#include "cdb_faultenv.h"

#define FCAP (256u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define DIR "w"
#define SEC 1000000000

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 2048u];
static cdb_faultenv fe;
static cdb_env env;

static const uint8_t db_id[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

/* The coordinators the sleep hook keeps beating. */
static cdb_coord *beating[4];
static unsigned nbeating;

static void beat_others(void *arg)
{
    unsigned i;
    (void)arg;
    for (i = 0; i < nbeating; i++)
        if (beating[i]->joined)
            (void)cdb_coord_beat(beating[i], 0);
}

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 2048;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mkdir(env.ctx, DIR));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "."));
    nbeating = 0;
    cdb_faultenv_on_sleep(&fe, beat_others, NULL);
}

void setUp(void)
{
    init_env();
}

void tearDown(void) {}

static cdb_status join(cdb_coord *c)
{
    return cdb_coord_join(c, &env, DIR "/coord", db_id, CDB_ORDER_LITTLE, NULL);
}

/* Lets time pass in steps, every listed coordinator beating as it goes. */
static void run_for(int64_t ns)
{
    int64_t step = SEC / 4;
    while (ns > 0) {
        cdb_faultenv_advance(&fe, step);
        beat_others(NULL);
        ns -= step;
    }
}

static int file_exists(const char *path)
{
    int e = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->exists(env.ctx, path, &e));
    return e;
}

/* -------------------------------------------------------- the record */

static cdb_coord_rec sample(void)
{
    cdb_coord_rec r;
    memset(&r, 0, sizeof r);
    r.slot = 7;
    r.role = CDB_COORD_PRIMARY;
    r.flags = CDB_COORD_ALIVE | CDB_COORD_JOURNAL;
    r.priority = 3;
    memcpy(r.db_id, db_id, 16);
    memset(r.session, 0xAB, 16);
    r.principal = 99;
    r.beat = 12345;
    r.beat_time = 777;
    r.seq_hwm = 1u << 20;
    r.tx_hwm = 4096;
    r.req_kind = CDB_COORD_REQ_BATON;
    r.heat = 2;
    r.req_time = 888;
    r.x_total = 1000;
    r.cts = 41;
    r.x = 500;
    r.issued = 880;
    r.clock = 999;
    r.pinned = 17;
    r.nblocks = 2;
    r.blocks[0].store = 5;
    r.blocks[0].first = 1024;
    r.blocks[1].store = 6;
    r.blocks[1].first = 2048;
    return r;
}

static void test_record_round_trip(void)
{
    cdb_coord_rec r = sample(), back;
    uint8_t h[CDB_COORD_HALF];
    cdb_coord_encode(&r, CDB_ORDER_BIG, h);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_decode(h, CDB_ORDER_BIG, &back));
    TEST_ASSERT_EQUAL_MEMORY(&r, &back, sizeof r);
    /* The other byte order is refused, not misread. */
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));
}

/* Rewrites the CRC after a deliberate change, so only the change is judged. */
static void reseal(uint8_t *h)
{
    cdb_store_u32(h + 508, cdb_crc32c(h, 508), CDB_ORDER_LITTLE);
}

static void test_record_rejects(void)
{
    cdb_coord_rec r = sample(), back;
    uint8_t h[CDB_COORD_HALF];

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[100] ^= 1u;
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[92] = 1; /* reserved */
    reseal(h);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[160 + 16 * 2] = 1; /* past the last held block */
    reseal(h);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[152] = CDB_COORD_MAX_BLOCKS + 1u;
    reseal(h);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[12] = 3; /* no such role */
    reseal(h);
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));

    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    h[9] = 2; /* another epoch */
    reseal(h);
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_coord_decode(h, CDB_ORDER_LITTLE, &back));
}

static void test_choose_rule(void)
{
    cdb_coord_brief a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.req_time = 5;
    b.req_time = 6;
    a.priority = 9;
    b.priority = 1;
    TEST_ASSERT_TRUE(cdb_coord_beats(&a, &b)); /* earliest first */
    TEST_ASSERT_FALSE(cdb_coord_beats(&b, &a));
    b.req_time = 5;
    TEST_ASSERT_TRUE(cdb_coord_beats(&b, &a)); /* then the lower priority value */
    b.priority = 9;
    a.session[0] = 1;
    b.session[0] = 2;
    TEST_ASSERT_TRUE(cdb_coord_beats(&a, &b)); /* then the session */
    TEST_ASSERT_FALSE(cdb_coord_beats(&a, &a));
}

/* -------------------------------------------------------- joining */

static void test_alone(void)
{
    cdb_coord a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    TEST_ASSERT_EQUAL_UINT(0, a.me.slot);
    TEST_ASSERT_EQUAL_UINT(1, a.me.priority);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, a.me.role);
    TEST_ASSERT_EQUAL_UINT(0, a.live_others);
    TEST_ASSERT_TRUE(file_exists(DIR "/coord/0"));
    /* Beats are rate-limited: nothing is written before one is due. */
    {
        uint64_t before = a.me.beat;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_beat(&a, 0));
        TEST_ASSERT_EQUAL_UINT64(before, a.me.beat);
        cdb_faultenv_advance(&fe, SEC);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_beat(&a, 0));
        TEST_ASSERT_TRUE(a.me.beat > before);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_FALSE(file_exists(DIR "/coord/0"));
}

static void test_two_then_primary_dies(void)
{
    static cdb_coord a, b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&b));
    beating[nbeating++] = &b;
    TEST_ASSERT_EQUAL_UINT(1, b.me.slot);
    TEST_ASSERT_EQUAL_UINT(2, b.me.priority);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_SUB, b.me.role);
    TEST_ASSERT_EQUAL_UINT(1, b.live_others);
    TEST_ASSERT_EQUAL_INT(0, b.primary);

    /* Both keep beating: the roles stay. */
    run_for(10 * (int64_t)SEC);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, a.me.role);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_SUB, b.me.role);
    /* The subordinate's clock never runs behind the primary's. */
    TEST_ASSERT_TRUE(cdb_coord_clock(&b) >= (int64_t)(b.peers[0].b.clock * 1000u));

    /* The primary goes silent; once it is dead the subordinate takes over. */
    beating[0] = &b;
    nbeating = 1;
    run_for(4 * (int64_t)SEC);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, b.me.role);
    TEST_ASSERT_EQUAL_UINT(0, b.live_others);

    /* The old primary wakes up: its slot is still its own, it sees the new
     * primary and steps down. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_beat(&a, 0));
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_SUB, a.me.role);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* -------------------------------------------------------- the baton */

/* Alone, the baton is granted at once, with no settle wait; each new baton
 * takes the next CTS. */
static void test_baton_alone(void)
{
    cdb_coord a;
    int64_t t0;
    int granted;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    t0 = fe.mono_ns;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 10, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_EQUAL_INT64(t0, fe.mono_ns);
    TEST_ASSERT_EQUAL_UINT64(1, a.me.cts);
    TEST_ASSERT_TRUE(cdb_coord_baton_held(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_fence(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_guard(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 10, &granted)); /* held: a grant */
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_release(&a));
    TEST_ASSERT_FALSE(cdb_coord_baton_held(&a));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_guard(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 10, &granted));
    TEST_ASSERT_EQUAL_UINT64(2, a.me.cts);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
}

/* A request waits while another holds the baton; the holder sees that it
 * is wanted, and its release lets the request through with the next CTS.
 * The old holder's fence then fails. */
static void test_baton_passes_on_release(void)
{
    static cdb_coord a, b;
    int granted;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&b));
    beating[nbeating++] = &b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_try(&a, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_try(&b, 0, &granted));
    TEST_ASSERT_FALSE(granted);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_REQ_BATON, b.me.req_kind);
    run_for(2 * (int64_t)SEC);
    TEST_ASSERT_TRUE(cdb_coord_baton_wanted(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_try(&b, 0, &granted));
    TEST_ASSERT_FALSE(granted); /* a is alive and within its window */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_release(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_try(&b, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_EQUAL_UINT64(a.me.cts + 1u, b.me.cts);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_fence(&b));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_baton_fence(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* A holder that goes silent loses the baton once it is dead; when it wakes,
 * its guard and its fence both refuse further writes. */
static void test_baton_dead_holder(void)
{
    static cdb_coord a, b;
    int64_t t0;
    int granted;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&b));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    beating[0] = &b; /* a goes silent */
    t0 = fe.mono_ns;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&b, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_TRUE(fe.mono_ns - t0 >= 3 * (int64_t)SEC);
    TEST_ASSERT_TRUE(b.me.cts > a.me.cts);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_guard(&a));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_baton_fence(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* A holder that stays alive but neither progresses nor releases loses the
 * baton after its service window. The new holder waits one more heartbeat
 * interval, by which time the old holder's guard has seen the higher CTS. */
static void test_baton_window_passes(void)
{
    static cdb_coord a, b;
    int64_t t0;
    int granted;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&b));
    beating[nbeating++] = &b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_guard(&a));
    t0 = fe.mono_ns;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&b, 0, &granted));
    TEST_ASSERT_TRUE(granted);
    TEST_ASSERT_TRUE(fe.mono_ns - t0 >= 4 * (int64_t)SEC); /* window, then one interval */
    TEST_ASSERT_TRUE(a.max_cts > a.me.cts);               /* a's own beats have seen it */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_guard(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* A wait longer than the configured cap gives up and withdraws the
 * request. */
static void test_baton_wait_cap(void)
{
    static cdb_coord a, b;
    cdb_coord_config cfg;
    int granted;
    memset(&cfg, 0, sizeof cfg);
    cfg.baton_wait_ns = SEC / 2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_join(&b, &env, DIR "/coord", db_id, CDB_ORDER_LITTLE, &cfg));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&a, 0, &granted));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_baton_take(&b, 0, &granted));
    TEST_ASSERT_FALSE(granted);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_REQ_NONE, b.me.req_kind);
    TEST_ASSERT_TRUE(cdb_coord_baton_held(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* A coordinator that stopped without leaving (a crash) holds its slot only
 * until it has been watched for the dead timeout; the next joiner takes the
 * slot over, counters included, and the silent one learns it lost it. */
static void test_dead_slot_taken_over(void)
{
    static cdb_coord a, c;
    uint64_t a_beat;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
    a.me.tx_hwm = 5000; /* as if it had reserved counters */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_beat(&a, 1));
    a_beat = a.me.beat;
    {
        int64_t t0 = fe.mono_ns;
        TEST_ASSERT_EQUAL_INT(CDB_OK, join(&c));
        TEST_ASSERT_TRUE(fe.mono_ns - t0 >= 3 * (int64_t)SEC); /* it waited the dead timeout out */
    }
    TEST_ASSERT_EQUAL_UINT(0, c.me.slot);
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, c.me.role);
    TEST_ASSERT_TRUE(c.me.beat > a_beat);
    TEST_ASSERT_EQUAL_UINT64(5000, c.me.tx_hwm);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_beat(&a, 0));
    TEST_ASSERT_FALSE(a.joined);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&c));
}

/* Another joiner's earlier lobby request for slot 0 wins it. */
static void test_lobby_race_lost(void)
{
    cdb_coord_rec r;
    uint8_t h[CDB_COORD_HALF];
    cdb_storage st;
    cdb_coord b;

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mkdir(env.ctx, DIR "/coord"));
    memset(&r, 0, sizeof r);
    memcpy(r.db_id, db_id, 16);
    memset(r.session, 0x11, 16);
    r.slot = 0;
    r.priority = 7;
    r.req_kind = CDB_COORD_REQ_JOIN;
    r.req_time = 1; /* long before anyone else's */
    r.beat = 1;
    cdb_coord_encode(&r, CDB_ORDER_LITTLE, h);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, DIR "/coord/join.11111111111111111111111111111111",
                                                CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, h, sizeof h));
    cdb_storage_close(&st);

    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&b));
    TEST_ASSERT_EQUAL_UINT(1, b.me.slot);
    TEST_ASSERT_EQUAL_UINT(1, b.me.priority); /* the priority did not clash */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* Every slot held by a live coordinator: busy. */
static void test_full(void)
{
    static cdb_coord a, b, c;
    cdb_coord_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.max_coords = 2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_join(&a, &env, DIR "/coord", db_id, CDB_ORDER_LITTLE, &cfg));
    beating[nbeating++] = &a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_join(&b, &env, DIR "/coord", db_id, CDB_ORDER_LITTLE, &cfg));
    beating[nbeating++] = &b;
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_coord_join(&c, &env, DIR "/coord", db_id, CDB_ORDER_LITTLE, &cfg));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&b));
}

/* Crash at every operation of a join, in every mode: the next join always
 * succeeds, in slot 0, and as primary. */
static void test_join_crash_sweep(void)
{
    static const cdb_crash_mode modes[] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                            CDB_CRASH_REORDER };
    uint64_t ops, op;
    unsigned m;
    cdb_coord a;

    {
        uint64_t before = fe.clock.ops;
        TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
        ops = fe.clock.ops - before;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
    }
    TEST_ASSERT_TRUE(ops > 3);
    for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (op = 1; op <= ops; op++) {
            init_env();
            cdb_faultenv_arm(&fe, op, modes[m], 1, 100);
            (void)join(&a);
            cdb_faultenv_crash_now(&fe, modes[m], 1, 100);
            cdb_faultenv_reboot(&fe);
            TEST_ASSERT_EQUAL_INT(CDB_OK, join(&a));
            TEST_ASSERT_EQUAL_UINT(0, a.me.slot);
            TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, a.me.role);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&a));
        }
    }
}

/* -------------------------------------------------------- databases */

/* One process's database buffers. */
struct proc {
    uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
    cdb_pager_node nodes[NODES];
    cdb_free_extent free_list[1024];
    uint8_t record_buf[1024];
    uint8_t cat_work[16384];
    cdb_index_entry cat_entries[512];
    uint8_t cat_page[PAGE];
    cdb_db_config cfg;
    cdb_db db;
};
static struct proc p1, p2;

/* Entry N is logical page N; pages up to 64 are reserved on first use so
 * the engine's own pages (the marks index) land above them. REPLACE sets the
 * value; PATCH appends to it, so applying a change twice shows. */
static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    uint8_t cur[PAGE], val[PAGE];
    cdb_page_header t;
    size_t len = 0;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    TEST_ASSERT_TRUE(d->entry_id < 64);
    while (p->next_lid < 64)
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
            return s;
    if (d->op == CDB_OP_PATCH) {
        s = cdb_pager_read(p, d->entry_id, cur, &t);
        if (s == CDB_OK) {
            len = t.payload_len;
            memcpy(val, cur + cdb_page_header_size(p->fmt), len);
        } else if (s != CDB_E_NOT_FOUND) {
            return s;
        }
    }
    TEST_ASSERT_TRUE(len + d->len < 64);
    memcpy(val + len, d->data, d->len);
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    return cdb_pager_write(p, d->entry_id, &t, val, len + d->len);
}

/* The value of entry e in p's pages, as a string; "" when absent. */
static const char *value_of(struct proc *p, uint64_t e)
{
    static char out[PAGE];
    uint8_t buf[PAGE];
    cdb_page_header h;
    cdb_status s = cdb_pager_read(&p->db.pager, e, buf, &h);
    out[0] = '\0';
    if (s == CDB_E_NOT_FOUND)
        return out;
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    memcpy(out, buf + cdb_page_header_size(p->db.pager.fmt), h.payload_len);
    out[h.payload_len] = '\0';
    return out;
}

static cdb_status open_proc(struct proc *p)
{
    cdb_header h;
    memset(&p->cfg, 0, sizeof p->cfg);
    p->cfg.pager.arena = p->arena;
    p->cfg.pager.arena_size = sizeof p->arena;
    p->cfg.pager.nodes = p->nodes;
    p->cfg.pager.node_cap = NODES;
    p->cfg.pager.free_list = p->free_list;
    p->cfg.pager.free_cap = sizeof p->free_list / sizeof p->free_list[0];
    p->cfg.record_buf = p->record_buf;
    p->cfg.record_buf_size = sizeof p->record_buf;
    p->cfg.apply = kv_apply;
    p->cfg.catalog_work.bytes = p->cat_work;
    p->cfg.catalog_work.bytes_size = sizeof p->cat_work;
    p->cfg.catalog_work.entries = p->cat_entries;
    p->cfg.catalog_work.entries_cap = 512;
    p->cfg.catalog_page = p->cat_page;
    p->cfg.durability = CDB_DURABILITY_FULL;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memcpy(h.db_id, db_id, 16);
    return cdb_db_open(&p->db, &env, DIR "/db", &p->cfg, &h);
}

static void tick_p1(void *arg)
{
    (void)arg;
    (void)cdb_db_tick(&p1.db);
}

static void test_second_process_is_busy(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_REPLACE, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    /* The first process keeps ticking while the second tries to open. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, open_proc(&p2));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/coord/1")); /* it left again */

    /* The first one is untouched and closes normally; then the second opens. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 2, CDB_OP_REPLACE, "two", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* A process that crashed with the database open: the next open waits the
 * dead timeout out, then recovers its journal as before. */
static void test_crashed_process_recovered(void)
{
    cdb_page_header h;
    uint8_t buf[PAGE];
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 3, CDB_OP_REPLACE, "three", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&p2.db.pager, 3, buf, &h));
    TEST_ASSERT_EQUAL_UINT(5, h.payload_len);
    TEST_ASSERT_EQUAL_MEMORY("three", buf + cdb_page_header_size(p2.db.pager.fmt), 5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* -------------------------------------------------------- journals per slot */

/* One transaction of a hand-written journal: append text to an entry. */
struct jtx {
    uint64_t counter;
    uint64_t stamp;
    uint64_t entry;
    const char *text;
};

/* Writes main.<slot>.cdb as a crashed session of that slot would have left
 * it (spec 14.4.7): committed transactions, commit numbers 0. */
static void plant_journal(unsigned slot, uint64_t unsealed, const struct jtx *t, unsigned n)
{
    char path[64];
    uint8_t buf[256], info[CDB_COMMIT_PAYLOAD_SIZE];
    cdb_journal_header jh;
    cdb_journal j;
    cdb_storage st;
    unsigned i;

    snprintf(path, sizeof path, DIR "/db.cdbwork/main.%u.cdb", slot);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mkdir(env.ctx, DIR "/db.cdbwork"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, DIR));
    memset(&jh, 0, sizeof jh);
    jh.type_id = CDB_COMPANION_MAIN_JOURNAL;
    jh.slot = (uint16_t)slot;
    memcpy(jh.db_id, db_id, 16);
    jh.unsealed_seq = unsealed;
    jh.first_seq = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, path, CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_truncate(&st, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_create(&j, &st, CDB_ORDER_LITTLE, &jh));
    for (i = 0; i < n; i++) {
        cdb_commit_info ci;
        cdb_record r;
        uint8_t payload[64];
        size_t len = strlen(t[i].text);
        payload[0] = CDB_OP_PATCH;
        memcpy(payload + 1, t[i].text, len);
        memset(&r, 0, sizeof r);
        r.kind = CDB_RECORD_DATA;
        r.tx_id = ((uint64_t)slot << 48) | t[i].counter;
        r.store_id = 1;
        r.entry_id = t[i].entry;
        r.payload = payload;
        r.payload_len = (uint32_t)(len + 1);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
        memset(&ci, 0, sizeof ci);
        ci.timestamp = t[i].stamp;
        ci.delta_count = 1;
        cdb_commit_encode(&ci, info, CDB_ORDER_LITTLE);
        memset(&r, 0, sizeof r);
        r.kind = CDB_RECORD_COMMIT;
        r.tx_id = ((uint64_t)slot << 48) | t[i].counter;
        r.payload = info;
        r.payload_len = CDB_COMMIT_PAYLOAD_SIZE;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    cdb_storage_close(&st);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, DIR "/db.cdbwork"));
}

/* Two slots' journals, interleaved by stamp. */
static const struct jtx slot0_txs[] = { { 1, 10, 1, "a" }, { 2, 30, 1, "c" } };
static const struct jtx slot1_txs[] = { { 1, 20, 1, "b" }, { 2, 40, 2, "d" } };

/* A database at rest, then two crashed sessions' journals beside it.
 * Returns the archive's committed sequence they were started from. */
static uint64_t two_crashed_journals(void)
{
    uint64_t seq;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    seq = p1.db.pager.hs.record.committed_seq;
    plant_journal(0, seq, slot0_txs, 2);
    plant_journal(1, seq, slot1_txs, 2);
    return seq;
}

static void test_recovery_merges_slots_by_stamp(void)
{
    uint64_t seq = two_crashed_journals();
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_STRING("abc", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("d", value_of(&p2, 2));
    TEST_ASSERT_EQUAL_UINT64(4, p2.db.applied);
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/main.1.cdb"));
    /* This session took slot 0: its counters go on above the slot's mark. */
    TEST_ASSERT_EQUAL_UINT(0, p2.db.coord.me.slot);
    TEST_ASSERT_EQUAL_UINT64(2, p2.db.my_mark);
    TEST_ASSERT_EQUAL_UINT64(3, p2.db.next_tx);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));

    /* The same journals again, as a crash between the archive commit and
     * their removal would leave them: the marks say they are in, so nothing
     * is applied twice. */
    plant_journal(0, seq, slot0_txs, 2);
    plant_journal(1, seq, slot1_txs, 2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_STRING("abc", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("d", value_of(&p2, 2));
    TEST_ASSERT_EQUAL_UINT64(4, p2.db.applied);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));

    /* Only what is above a mark is new. */
    {
        static const struct jtx more[] = { { 2, 30, 1, "c" }, { 3, 50, 1, "e" } };
        plant_journal(1, seq, more, 2);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_STRING("abce", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_UINT64(5, p2.db.applied);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* A journal started from an archive state newer than the archive (an older
 * copy of the archive was put back) is set aside, never applied. */
static void test_journal_from_newer_archive_set_aside(void)
{
    uint64_t seq = two_crashed_journals();
    plant_journal(1, seq + 5, slot1_txs, 2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_STRING("ac", value_of(&p2, 1));
    TEST_ASSERT_TRUE(file_exists(DIR "/db.cdbwork/main.1.cdb.stray"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* Crash at every operation of the recovering open, in every mode: the next
 * open always ends with each transaction applied exactly once. */
static void test_recovery_crash_sweep(void)
{
    static const cdb_crash_mode modes[] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                            CDB_CRASH_REORDER };
    uint64_t ops, op;
    unsigned m;

    two_crashed_journals();
    {
        uint64_t before = fe.clock.ops;
        TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
        ops = fe.clock.ops - before;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    }
    for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (op = 1; op <= ops; op++) {
            init_env();
            two_crashed_journals();
            cdb_faultenv_arm(&fe, op, modes[m], 1, 100);
            (void)open_proc(&p2);
            cdb_faultenv_crash_now(&fe, modes[m], 1, 100);
            cdb_faultenv_reboot(&fe);
            TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
            TEST_ASSERT_EQUAL_STRING("abc", value_of(&p2, 1));
            TEST_ASSERT_EQUAL_STRING("d", value_of(&p2, 2));
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
        }
    }
}

/* Transaction counters are reserved in the slot before use, so after a
 * crash the slot's next owner starts above anything the old one used. */
static void test_counters_survive_crash(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_REPLACE, "x", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_UINT64(1 + 1024, p1.db.coord.me.tx_hwm);
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(0, p2.db.coord.me.slot);
    TEST_ASSERT_EQUAL_UINT64(1 + 1024, p2.db.next_tx);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* An application idle past the dead timeout loses its slot to a newcomer
 * and is told so; its database refuses further work. */
static void test_idle_process_loses_slot(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2)); /* p1 never beats: it is taken for dead */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, cdb_db_tick(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* -------------------------------------------------------- host environment */

static int count_name(void *arg, const char *name)
{
    (void)name;
    ++*(unsigned *)arg;
    return 0;
}

/* The host's list, mono, random and sleep, and a join and leave on a real
 * directory. */
static void test_host_hooks(void)
{
    cdb_hostenv he;
    cdb_env h;
    char dir[160], path[200];
    uint8_t r1[16], r2[16];
    int64_t t0, t1;
    unsigned n = 0;
    cdb_coord c;
    cdb_storage st;

    cdb_hostenv_init(&he, &h);
    snprintf(dir, sizeof dir, "%s/coord_%ld", CDB_TEST_TMPDIR, (long)getpid());
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->mkdir(h.ctx, dir));
    snprintf(path, sizeof path, "%s/a", dir);
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->open(h.ctx, path, CDB_ENV_CREATE, &st));
    cdb_storage_close(&st);
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->list(h.ctx, dir, count_name, &n));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->remove(h.ctx, path));

    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->mono(h.ctx, &t0));
    h.ops->sleep(h.ctx, 2000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->mono(h.ctx, &t1));
    TEST_ASSERT_TRUE(t1 - t0 >= 2000000);

    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->random(h.ctx, r1, sizeof r1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->random(h.ctx, r2, sizeof r2));
    TEST_ASSERT_TRUE(memcmp(r1, r2, sizeof r1) != 0);

    snprintf(path, sizeof path, "%s/coord", dir);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_join(&c, &h, path, db_id, CDB_ORDER_LITTLE, NULL));
    TEST_ASSERT_EQUAL_UINT(CDB_COORD_PRIMARY, c.me.role);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&c));
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->rmdir(h.ctx, path));
    TEST_ASSERT_EQUAL_INT(CDB_OK, h.ops->rmdir(h.ctx, dir));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_record_round_trip);
    RUN_TEST(test_record_rejects);
    RUN_TEST(test_choose_rule);
    RUN_TEST(test_alone);
    RUN_TEST(test_two_then_primary_dies);
    RUN_TEST(test_baton_alone);
    RUN_TEST(test_baton_passes_on_release);
    RUN_TEST(test_baton_dead_holder);
    RUN_TEST(test_baton_window_passes);
    RUN_TEST(test_baton_wait_cap);
    RUN_TEST(test_dead_slot_taken_over);
    RUN_TEST(test_lobby_race_lost);
    RUN_TEST(test_full);
    RUN_TEST(test_join_crash_sweep);
    RUN_TEST(test_second_process_is_busy);
    RUN_TEST(test_crashed_process_recovered);
    RUN_TEST(test_idle_process_loses_slot);
    RUN_TEST(test_recovery_merges_slots_by_stamp);
    RUN_TEST(test_journal_from_newer_archive_set_aside);
    RUN_TEST(test_recovery_crash_sweep);
    RUN_TEST(test_counters_survive_crash);
    RUN_TEST(test_host_hooks);
    return UNITY_END();
}
