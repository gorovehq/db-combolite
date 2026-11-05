/* Coordinators (spec 5, 5.9, 14.5): the slot record, joining, the primary
 * role, liveness, batons, reader pins, overlays, rebasing and multi-process
 * reconciliation and recovery. Several coordinators run in this one thread
 * on one fault environment; its sleep hook lets the others beat while one
 * waits, as other processes would. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_coord.h"
#include "cdb_crc32c.h"
#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_table.h"

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
    uint8_t cat_work[24u * 1024u];
    cdb_index_entry cat_entries[512];
    uint8_t cat_page[PAGE];
    uint8_t doc_buffers[8u * PAGE];
    uint8_t doc_stage[2048];
    uint8_t table_buffers[4u * PAGE];
    cdb_db_config cfg;
    cdb_db db;
};
static struct proc p1, p2;

/* Value encoding helpers for table tests. */
typedef struct enc {
    uint8_t b[256];
    size_t n;
    cdb_value_writer w;
} enc;

static void obj_begin(enc *e)
{
    cdb_value_writer_init(&e->w, e->b, sizeof e->b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&e->w);
}

static void arr_begin(enc *e)
{
    cdb_value_writer_init(&e->w, e->b, sizeof e->b, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&e->w);
}

static void put_int(enc *e, uint64_t f, int64_t v)
{
    if (f)
        cdb_value_field(&e->w, f);
    cdb_value_write_int(&e->w, v);
}

static void put_str(enc *e, uint64_t f, const char *s)
{
    if (f)
        cdb_value_field(&e->w, f);
    cdb_value_write_string(&e->w, s, strlen(s));
}

static void obj_end(enc *e)
{
    cdb_value_end_object(&e->w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, e->w.error);
    e->n = cdb_value_writer_len(&e->w);
}

static void arr_end(enc *e)
{
    cdb_value_end_array(&e->w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, e->w.error);
    e->n = cdb_value_writer_len(&e->w);
}

static void field_of(const uint8_t *row, size_t len, uint64_t f, cdb_value *v, int *found)
{
    cdb_value obj;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(row, len, CDB_ORDER_LITTLE, &obj));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, CDB_ORDER_LITTLE, f, v, found));
}

static enc make_item(uint64_t f_id, uint64_t f_name, uint64_t f_code,
                     int64_t id, const char *name, const char *code)
{
    enc e;
    obj_begin(&e);
    put_int(&e, f_id, id);
    put_str(&e, f_name, name);
    if (code != NULL)
        put_str(&e, f_code, code);
    obj_end(&e);
    return e;
}

/* Entry N is logical page N; pages up to 64 are reserved on first use so
 * the engine's own pages (the marks index) land above them. REPLACE sets the
 * value; PATCH appends to it, so applying a change twice shows; DELETE frees
 * the page. */
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
    if (d->op == CDB_OP_DELETE)
        return cdb_pager_free(p, d->entry_id);
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

static cdb_status open_proc_max(struct proc *p, unsigned max_coords)
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
    p->cfg.coord.max_coords = max_coords;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memcpy(h.db_id, db_id, 16);
    return cdb_db_open(&p->db, &env, DIR "/db", &p->cfg, &h);
}

static cdb_status open_proc(struct proc *p)
{
    return open_proc_max(p, 0);
}

static cdb_status open_proc_table_max(struct proc *p, unsigned max_coords)
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
    p->cfg.catalog_work.bytes = p->cat_work;
    p->cfg.catalog_work.bytes_size = sizeof p->cat_work;
    p->cfg.catalog_work.entries = p->cat_entries;
    p->cfg.catalog_work.entries_cap = 512;
    p->cfg.catalog_page = p->cat_page;
    p->cfg.durability = CDB_DURABILITY_FULL;
    p->cfg.coord.max_coords = max_coords;
    p->cfg.doc_buffers = p->doc_buffers;
    p->cfg.doc_stage = p->doc_stage;
    p->cfg.doc_stage_size = sizeof p->doc_stage;
    p->cfg.table_buffers = p->table_buffers;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memcpy(h.db_id, db_id, 16);
    return cdb_db_open(&p->db, &env, DIR "/db", &p->cfg, &h);
}

static cdb_status open_proc_table(struct proc *p)
{
    return open_proc_table_max(p, 0);
}

static void tick_p1(void *arg)
{
    (void)arg;
    (void)cdb_db_tick(&p1.db);
}

static void tick_both(void *arg)
{
    (void)arg;
    if (p1.db.coord.joined)
        (void)cdb_db_tick(&p1.db);
    if (p2.db.coord.joined)
        (void)cdb_db_tick(&p2.db);
}

/* When max_coords is 1, a second opener finds every slot held and leaves. */
static void test_second_process_is_busy(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc_max(&p1, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_REPLACE, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    /* The first process keeps ticking while the second tries to open. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, open_proc_max(&p2, 1));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/coord/1")); /* it left again */

    /* The first one is untouched and closes normally; then the second opens. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 2, CDB_OP_REPLACE, "two", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc_max(&p2, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
}

/* Two processes open the same database: the second uses an overlay, and
 * reconciliation under the baton merges both journals in stamp order. */
static void test_two_processes_share_and_merge(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_TRUE(cdb_coord_baton_held(&p1.db.coord));
    TEST_ASSERT_TRUE(p1.db.fresh);

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(1, p2.db.coord.me.slot);
    TEST_ASSERT_FALSE(cdb_coord_baton_held(&p2.db.coord));
    TEST_ASSERT_FALSE(p2.db.fresh);
    TEST_ASSERT_TRUE(file_exists(DIR "/db.cdbwork/pages.1.cdb"));

    /* Interleaved commits: p1 at t, p2 at t+10ms, p1 at t+20ms. p2 writes
     * another entry: the same one would collide (spec 5.9.4). */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_PATCH, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 2, CDB_OP_PATCH, "b", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_PATCH, "c", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    /* Each process sees only its own unreconciled changes. */
    TEST_ASSERT_EQUAL_STRING("ac", value_of(&p1, 1));
    TEST_ASSERT_EQUAL_STRING("", value_of(&p1, 2));
    TEST_ASSERT_EQUAL_STRING("", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("b", value_of(&p2, 2));

    /* p2 reconciles: p1 yields its standing baton on tick, and both journals
     * merge into the archive. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("ac", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("b", value_of(&p2, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p1.db));
    TEST_ASSERT_EQUAL_STRING("ac", value_of(&p1, 1));
    TEST_ASSERT_EQUAL_STRING("b", value_of(&p1, 2));

    /* Closing p1 while p2 is still live keeps the working directory until p2
     * closes too. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_TRUE(file_exists(DIR "/db.cdbwork"));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/main.0.cdb"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork"));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* A transaction that was open while another process reconciled into the
 * archive is rejected: its view is older than the archive (spec 5.9.4). The
 * rejected process is rebased at once, so its retry builds on the new
 * archive (spec 5.9.1, 14.4.7 rule 2). */
static void test_open_tx_rejected_after_peer_reconcile(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    /* p2 opens a transaction before p1 reconciles, so it does not rebase yet. */
    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 1, CDB_OP_PATCH, "b", 1));

    /* p1 commits "a" and reconciles into the archive while p2's tx is open. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_PATCH, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));

    /* p2's commit is rejected, and p2 now reads the reconciled "a". */
    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));
    TEST_ASSERT_FALSE(p2.db.in_tx);
    TEST_ASSERT_EQUAL_STRING("a", value_of(&p2, 1));

    /* The retry commits "b" on top of "a" in p2's overlay. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 1, CDB_OP_PATCH, "b", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));
    TEST_ASSERT_EQUAL_STRING("ab", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("a", value_of(&p1, 1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p1.db));
    TEST_ASSERT_EQUAL_STRING("ab", value_of(&p1, 1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* A live reader's published pin keeps pages freed by later archive commits
 * from being overwritten until the reader advances its pin (spec 5.9.7). */
static void test_reader_pin_blocks_reuse(void)
{
    cdb_anchor_entry e1, e3;

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_REPLACE, "first", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 2, CDB_OP_REPLACE, "keep", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&p1.db.pager, 1, &e1));

    /* p2 opens and pins the archive commit where entry 1 lives at e1.phys. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT64(p1.db.pager.hs.record.committed_seq, p2.db.coord.me.pinned);

    /* p1 frees entry 1 and reconciles, then allocates entry 3 while p2 still
     * pins the old sequence (tick only p1 so p2 does not rebase yet). */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_DELETE, NULL, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 3, CDB_OP_REPLACE, "third", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&p1.db.pager, 3, &e3));
    TEST_ASSERT_TRUE(e3.phys != e1.phys); /* e1.phys is still pinned by p2 */
    TEST_ASSERT_EQUAL_STRING("first", value_of(&p2, 1));

    /* Once p2 ticks and rebases, its pin moves past the commit that freed e1. */
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p2.db));
    TEST_ASSERT_EQUAL_STRING("", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_STRING("third", value_of(&p2, 3));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* When one of two open processes crashes, the survivor's next reconciliation
 * merges the dead slot's journal and cleans up its files (spec 5.9.1,
 * 14.4.7 rule 9). */
static void test_live_peer_merges_crashed_peer(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_PATCH, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 2, CDB_OP_PATCH, "b", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    /* p2 crashes: close its handles without leaving or removing its files,
     * then let the dead timeout pass while p1 keeps ticking. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    cdb_storage_close(&p2.db.overlay_st);
    cdb_storage_close(&p2.db.journal_st);
    cdb_storage_close(&p2.db.archive_st);
    cdb_storage_close(&p2.db.coord.st);
    p2.db.coord.joined = 0;
    {
        int64_t step = SEC / 4, rem = 4 * (int64_t)SEC;
        while (rem > 0) {
            cdb_faultenv_advance(&fe, step);
            (void)cdb_db_tick(&p1.db);
            rem -= step;
        }
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_STRING("a", value_of(&p1, 1));
    TEST_ASSERT_EQUAL_STRING("b", value_of(&p1, 2));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/main.1.cdb"));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/pages.1.cdb"));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork/coord/1"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_FALSE(file_exists(DIR "/db.cdbwork"));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* A newcomer that claims a dead slot while another process is still live
 * adopts the dead slot's journal into its overlay (spec 5.9.1, 14.4.7
 * rule 9). */
static void test_take_over_dead_slot_while_peer_live(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(1, p2.db.coord.me.slot);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 1, CDB_OP_PATCH, "b", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    /* p2 crashes while p1 stays alive without reconciling. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    cdb_storage_close(&p2.db.overlay_st);
    cdb_storage_close(&p2.db.journal_st);
    cdb_storage_close(&p2.db.archive_st);
    cdb_storage_close(&p2.db.coord.st);
    p2.db.coord.joined = 0;

    /* A new process opens, waits slot 1's dead timeout out, claims slot 1,
     * and adopts main.1.cdb into its overlay. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(1, p2.db.coord.me.slot);
    TEST_ASSERT_EQUAL_STRING("b", value_of(&p2, 1));
    TEST_ASSERT_EQUAL_UINT64(1 + 1024, p2.db.next_tx);

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 1, CDB_OP_PATCH, "c", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p1.db));
    TEST_ASSERT_EQUAL_STRING("bc", value_of(&p1, 1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* A slot whose "journal in use" flag is set while its journal is missing
 * causes reconciliation to return CDB_E_CORRUPT unless loss is accepted
 * (spec 14.4.7 rule 7). */
static void test_missing_peer_journal_refused(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 1, CDB_OP_PATCH, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    /* Remove p2's journal while p2's slot still says it is in use. */
    cdb_storage_close(&p2.db.journal_st);
    memset(&p2.db.journal_st, 0, sizeof p2.db.journal_st);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->remove(env.ctx, DIR "/db.cdbwork/main.1.cdb"));

    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_db_reconcile(&p1.db));
    p1.db.cfg.flags |= CDB_DB_ACCEPT_LOSS;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_STRING("a", value_of(&p1, 1));

    /* Restore p2's journal so both close cleanly. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
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

/* -------------------------------------------------------- entry-ID blocks */

/* Claims follow append -> settle -> choose: a second coordinator skips the
 * block the first holds, and a holder's next claim skips both (spec 5.9.3). */
static void test_id_block_claims(void)
{
    static cdb_coord c1, c2;
    uint64_t first;
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&c1));
    beating[nbeating++] = &c1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_claim_block(&c1, 5, 1, &first));
    TEST_ASSERT_EQUAL_UINT64(1, first);
    TEST_ASSERT_EQUAL_INT(CDB_OK, join(&c2));
    beating[nbeating++] = &c2;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_claim_block(&c2, 5, 1, &first));
    TEST_ASSERT_EQUAL_UINT64(1 + CDB_COORD_ID_BLOCK, first);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_claim_block(&c2, 6, 1, &first));
    TEST_ASSERT_EQUAL_UINT64(1, first); /* another store's blocks do not overlap */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_claim_block(&c1, 5, 1, &first));
    TEST_ASSERT_EQUAL_UINT64(1 + 2 * CDB_COORD_ID_BLOCK, first);
    TEST_ASSERT_EQUAL_UINT(2, c1.me.nblocks);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_drop_block(&c1, 5, 1));
    TEST_ASSERT_EQUAL_UINT(1, c1.me.nblocks);
    TEST_ASSERT_EQUAL_UINT64(1 + 2 * CDB_COORD_ID_BLOCK, c1.me.blocks[0].first);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&c2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_leave(&c1));
}

/* Two processes inserting into one store get disjoint blocks. Reconciling
 * raises the catalog floor past both and drops the blocks from the slots,
 * while each process keeps handing out the rest of its block from memory;
 * unused IDs are never handed out again (spec 5.9.3). */
static void test_id_blocks_across_processes(void)
{
    cdb_store_info info;
    uint64_t store, n1 = 0, n2 = 0, id;
    int dirty = 0;

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&p1.db, "s", 1, CDB_MODEL_DOCUMENT, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_peek_entry(&p1.db, store, &n1, &id));
    TEST_ASSERT_EQUAL_UINT64(1, id);
    TEST_ASSERT_EQUAL_UINT64(1, cdb_db_take_entry(&p1.db, store, &n1, &dirty));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_peek_entry(&p2.db, store, &n2, &id));
    TEST_ASSERT_EQUAL_UINT64(1 + CDB_COORD_ID_BLOCK, id);
    TEST_ASSERT_EQUAL_UINT64(1 + CDB_COORD_ID_BLOCK, cdb_db_take_entry(&p2.db, store, &n2, &dirty));
    TEST_ASSERT_TRUE(dirty);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&p1.db.catalog, store, &info));
    TEST_ASSERT_EQUAL_UINT64(1 + 2 * CDB_COORD_ID_BLOCK, info.next_entry_id);
    TEST_ASSERT_EQUAL_UINT(0, p1.db.coord.me.nblocks);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_peek_entry(&p1.db, store, &n1, &id));
    TEST_ASSERT_EQUAL_UINT64(2, id); /* the rest of the block, from memory */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_tick(&p2.db)); /* rebases onto the new floor */
    TEST_ASSERT_EQUAL_UINT(0, p2.db.coord.me.nblocks);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_peek_entry(&p2.db, store, &n2, &id));
    TEST_ASSERT_EQUAL_UINT64(2 + CDB_COORD_ID_BLOCK, id);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);

    /* After both close, a new session starts above both blocks. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    n1 = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_peek_entry(&p1.db, store, &n1, &id));
    TEST_ASSERT_EQUAL_UINT64(1 + 2 * CDB_COORD_ID_BLOCK, id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
}

/* -------------------------------------------------------- collisions */

/* A write to an entry another process has committed but not yet reconciled
 * is the later write and is rejected; other entries commit (spec 5.9.4). */
static void test_collision_on_same_entry(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 5, CDB_OP_REPLACE, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 6, CDB_OP_REPLACE, "two", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 5, CDB_OP_REPLACE, "bad", 3));
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));
    TEST_ASSERT_EQUAL_STRING("", value_of(&p2, 6)); /* nothing of it was applied */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 6, CDB_OP_REPLACE, "two", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("one", value_of(&p2, 5));
    TEST_ASSERT_EQUAL_STRING("two", value_of(&p2, 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* Another process's collision scan honours rollback records (spec 14.4.9
 * rule 7): a change p1 rolled back does not collide with p2, a kept one
 * does, and a write-set key is never dropped. */
static void test_collision_skips_rolled_back(void)
{
    cdb_tx_mark m;

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 5, CDB_OP_REPLACE, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&p1.db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 6, CDB_OP_REPLACE, "gone", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 8, CDB_OP_REPLACE, "gone", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p1.db, 1, 0, "ukey", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&p1.db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 7, CDB_OP_REPLACE, "seven", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 6, CDB_OP_REPLACE, "six", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 8, CDB_OP_REPLACE, "eight", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 7, CDB_OP_REPLACE, "bad", 3));
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    /* Entry 8 meets p1's dropped change first; the key must still count. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 8, CDB_OP_REPLACE, "bad", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p2.db, 1, 0, "ukey", 4));
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("one", value_of(&p2, 5));
    TEST_ASSERT_EQUAL_STRING("six", value_of(&p2, 6));
    TEST_ASSERT_EQUAL_STRING("seven", value_of(&p2, 7));
    TEST_ASSERT_EQUAL_STRING("eight", value_of(&p2, 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* Write-set key records make two inserts of the same unique key collide
 * even though they write different entries (spec 5.9.4, 14.4.7 rule 3). */
static void test_collision_on_unique_key(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 10, CDB_OP_REPLACE, "r1", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p1.db, 1, 0, "k1", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

    cdb_faultenv_advance(&fe, 10000000);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 11, CDB_OP_REPLACE, "r2", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p2.db, 1, 0, "k1", 2));
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

    /* The same key in another index, or another key, does not collide. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 11, CDB_OP_REPLACE, "r2", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p2.db, 1, 1, "k1", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p2.db, 1, 0, "k2", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    /* Key records carry no change: merging both journals touches only the
     * two rows. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("r1", value_of(&p2, 10));
    TEST_ASSERT_EQUAL_STRING("r2", value_of(&p2, 11));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* Appends a prepare record with the given stamp for p's open transaction,
 * as its commit would after taking that stamp. */
static void prepare_with_stamp(struct proc *p, uint64_t stamp)
{
    uint8_t payload[CDB_PREPARE_PAYLOAD_SIZE];
    cdb_record r;
    cdb_store_u64(payload, stamp, p->db.pager.order);
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_PREPARE;
    r.tx_id = p->db.tx_id;
    r.payload = payload;
    r.payload_len = CDB_PREPARE_PAYLOAD_SIZE;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&p->db.journal, &r, p->db.cfg.record_buf,
                                                     p->db.cfg.record_buf_size));
}

static unsigned sleeps;

/* From the second sleep on (the first is p2's settle interval), p1 gives up
 * its prepared transaction. */
static void abort_p1_later(void *arg)
{
    (void)arg;
    if (++sleeps >= 2 && p1.db.in_tx)
        (void)cdb_tx_abort(&p1.db);
    tick_both(NULL);
}

/* An overlap with a prepared, uncommitted transaction: an earlier
 * (stamp, slot) wins at once; a later one is waited out until it ends, and
 * an abort lets the committer through (spec 5.9.4). */
static void test_collision_with_prepared(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_commit_ready(&p2.db.coord)); /* the joiner's first-commit wait */

    /* p1 prepared first: p2 is the later writer. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 7, CDB_OP_REPLACE, "p1", 2));
    prepare_with_stamp(&p1, 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 7, CDB_OP_REPLACE, "p2", 2));
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&p1.db));

    /* p1 prepared with a later stamp: p2 waits until p1 aborts, then commits. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 8, CDB_OP_REPLACE, "p1", 2));
    prepare_with_stamp(&p1, UINT64_MAX / 2u);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 8, CDB_OP_REPLACE, "p2", 2));
    sleeps = 0;
    cdb_faultenv_on_sleep(&fe, abort_p1_later, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));
    TEST_ASSERT_TRUE(sleeps >= 2); /* it waited past the settle interval */
    TEST_ASSERT_FALSE(p1.db.in_tx);
    TEST_ASSERT_EQUAL_STRING("p2", value_of(&p2, 8));

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

static void commit_prepared(struct proc *p, uint64_t stamp)
{
    uint8_t info[CDB_COMMIT_PAYLOAD_SIZE];
    cdb_commit_info ci;
    cdb_record r;
    memset(&ci, 0, sizeof ci);
    ci.timestamp = stamp;
    ci.delta_count = p->db.tx_deltas;
    ci.track_mask = 1u << CDB_COMPANION_MAIN_JOURNAL;
    cdb_commit_encode(&ci, info, p->db.pager.order);
    memset(&r, 0, sizeof r);
    r.kind = (uint8_t)CDB_RECORD_COMMIT;
    r.tx_id = p->db.tx_id;
    r.payload = info;
    r.payload_len = CDB_COMMIT_PAYLOAD_SIZE;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&p->db.journal, &r, p->db.cfg.record_buf,
                                                     p->db.cfg.record_buf_size));
    p->db.last_tx = p->db.tx_id & (((uint64_t)1 << 48) - 1u);
    p->db.in_tx = 0;
}

static void commit_p1_later(void *arg)
{
    (void)arg;
    if (++sleeps >= 2 && p1.db.in_tx)
        commit_prepared(&p1, UINT64_MAX / 2u);
    tick_both(NULL);
}

/* When p1 prepared with a later stamp commits (rather than aborts), p2 waits
 * out the preparation and then is rejected with CDB_E_COLLISION (spec 5.9.4). */
static void test_collision_with_prepared_commit(void)
{
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_commit_ready(&p2.db.coord));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p1.db, 1, 9, CDB_OP_REPLACE, "p1", 2));
    prepare_with_stamp(&p1, UINT64_MAX / 2u);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 9, CDB_OP_REPLACE, "p2", 2));
    sleeps = 0;
    cdb_faultenv_on_sleep(&fe, commit_p1_later, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));
    TEST_ASSERT_TRUE(sleeps >= 2);
    TEST_ASSERT_FALSE(p1.db.in_tx);

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("p1", value_of(&p2, 9));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* Rebasing copies unmerged write-set key records along with data records
 * to the rotated journal (spec 5.9.4, 14.4.7 rule 3). */
static void test_rebase_preserves_write_keys(void)
{
    cdb_record r;
    uint64_t next, off;
    int found_write_key = 0;

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    /* p2 writes an unmerged transaction containing data and write-set keys */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 15, CDB_OP_REPLACE, "val", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put_key(&p2.db, 1, 0, "ukey", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

    /* p2 crashes while p1 stays alive without reconciling. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    cdb_storage_close(&p2.db.overlay_st);
    cdb_storage_close(&p2.db.journal_st);
    cdb_storage_close(&p2.db.archive_st);
    cdb_storage_close(&p2.db.coord.st);
    p2.db.coord.joined = 0;

    /* A newcomer opens, claims dead slot 1, and adopts/rebases its journal */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(1, p2.db.coord.me.slot);

    /* Read through p2's journal to verify CDB_RECORD_WRITE_KEY was copied */
    off = CDB_JOURNAL_HEADER_SIZE;
    while (off < p2.db.journal.end) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&p2.db.journal, off, p2.db.cfg.record_buf,
                                                       p2.db.cfg.record_buf_size, &r, &next));
        if (r.kind == CDB_RECORD_WRITE_KEY) {
            uint32_t idx = cdb_load_u32(r.payload, p2.db.pager.order);
            uint32_t kh = cdb_load_u32(r.payload + 4, p2.db.pager.order);
            if (idx == 0 && kh == cdb_crc32c((const uint8_t *)"ukey", 4))
                found_write_key = 1;
        }
        off = next;
    }
    TEST_ASSERT_TRUE(found_write_key);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* Counts p's journal records of a kind; entry_id when entry is not 0. */
static unsigned journal_count(struct proc *p, uint8_t kind, uint64_t entry)
{
    cdb_record r;
    uint64_t next, off = CDB_JOURNAL_HEADER_SIZE;
    unsigned n = 0;
    while (off < p->db.journal.end) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&p->db.journal, off, p->db.cfg.record_buf,
                                                       p->db.cfg.record_buf_size, &r, &next));
        if (r.kind == kind && (entry == 0 || r.entry_id == entry))
            n++;
        off = next;
    }
    return n;
}

/* The rebase copy of a transaction with a rollback (spec 14.4.7 rule 2,
 * 14.4.9): the dropped record and the rollback record stay behind, the
 * commit's rollbacks flag is cleared, and the adopted changes are the kept
 * ones only. */
static void test_rebase_drops_rolled_back(void)
{
    cdb_tx_mark m;
    cdb_record r;
    uint64_t next, off;

    cdb_faultenv_on_sleep(&fe, tick_both, NULL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 15, CDB_OP_REPLACE, "keep", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_savepoint(&p2.db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 16, CDB_OP_REPLACE, "drop", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_rollback_to(&p2.db, &m));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&p2.db, 1, 17, CDB_OP_REPLACE, "after", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));
    TEST_ASSERT_EQUAL_UINT(1u, journal_count(&p2, CDB_RECORD_ROLLBACK, 0));
    TEST_ASSERT_EQUAL_UINT(1u, journal_count(&p2, CDB_RECORD_DATA, 16));

    /* p2 crashes while p1 stays alive without reconciling. */
    cdb_faultenv_on_sleep(&fe, tick_p1, NULL);
    cdb_storage_close(&p2.db.overlay_st);
    cdb_storage_close(&p2.db.journal_st);
    cdb_storage_close(&p2.db.archive_st);
    cdb_storage_close(&p2.db.coord.st);
    p2.db.coord.joined = 0;

    /* A newcomer claims dead slot 1 and rebases its journal. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
    TEST_ASSERT_EQUAL_UINT(1, p2.db.coord.me.slot);
    TEST_ASSERT_EQUAL_UINT(0u, journal_count(&p2, CDB_RECORD_ROLLBACK, 0));
    TEST_ASSERT_EQUAL_UINT(0u, journal_count(&p2, CDB_RECORD_DATA, 16));
    TEST_ASSERT_EQUAL_UINT(1u, journal_count(&p2, CDB_RECORD_DATA, 15));
    TEST_ASSERT_EQUAL_UINT(1u, journal_count(&p2, CDB_RECORD_DATA, 17));
    for (off = CDB_JOURNAL_HEADER_SIZE; off < p2.db.journal.end; off = next) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_read(&p2.db.journal, off, p2.db.cfg.record_buf,
                                                       p2.db.cfg.record_buf_size, &r, &next));
        if (r.kind == CDB_RECORD_COMMIT)
            TEST_ASSERT_FALSE(r.flags & CDB_RECORD_FLAG_ROLLBACKS);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));
    TEST_ASSERT_EQUAL_STRING("keep", value_of(&p2, 15));
    TEST_ASSERT_EQUAL_STRING("", value_of(&p2, 16));
    TEST_ASSERT_EQUAL_STRING("after", value_of(&p2, 17));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
}

/* End-to-end multi-process test driving cdb_table.c: table creation,
 * primary key and secondary unique index, concurrent inserts, collision
 * detection on primary key and unique column, and concurrent row replace. */
static void test_table_multi_process_e2e(void)
{
    cdb_table_column cols[3];
    unsigned pk = 0;
    cdb_table_def def;
    uint64_t table_id, f_id, f_name, f_code;
    uint32_t code_idx;
    uint64_t e1, e2, e3, e4, e5, e6;
    int found;

    init_env();
    cdb_faultenv_on_sleep(&fe, tick_both, NULL);

    /* Open p1 as table-enabled process and create table "items" */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc_table(&p1));

    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[1].name = "name";
    cols[1].name_len = 4;
    cols[1].type = CDB_TYPE_STRING;
    cols[2].name = "code";
    cols[2].name_len = 4;
    cols[2].type = CDB_TYPE_STRING;

    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 3;
    def.primary_key = &pk;
    def.npk = 1;
    def.keep = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&p1.db, "items", 5, &def, &table_id));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&p1.db, table_id, "id", 2, &f_id));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&p1.db, table_id, "name", 4, &f_name));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&p1.db, table_id, "code", 4, &f_code));

    /* Add a secondary unique index on "code" */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create_index(&p1.db, table_id, &f_code, 1, 1, &code_idx));

    /* Open p2 concurrently */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc_table(&p2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_coord_commit_ready(&p2.db.coord));

    /* Case 1: Concurrent disjoint inserts succeed and merge */
    {
        enc r1 = make_item(f_id, f_name, f_code, 1, "widget", "W001");
        enc r2 = make_item(f_id, f_name, f_code, 2, "gadget", "G002");

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p1.db, table_id, r1.b, r1.n, &e1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

        cdb_faultenv_advance(&fe, 10000000);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p2.db, table_id, r2.b, r2.n, &e2));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p2.db));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));

        {
            enc k1, k2;
            uint64_t fe1, fe2;
            arr_begin(&k1);
            put_int(&k1, 0, 1);
            arr_end(&k1);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&p2.db, table_id, k1.b, k1.n, &fe1, &found));
            TEST_ASSERT_EQUAL_INT(1, found);
            TEST_ASSERT_EQUAL_UINT64(e1, fe1);

            arr_begin(&k2);
            put_int(&k2, 0, 2);
            arr_end(&k2);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&p2.db, table_id, k2.b, k2.n, &fe2, &found));
            TEST_ASSERT_EQUAL_INT(1, found);
            TEST_ASSERT_EQUAL_UINT64(e2, fe2);
        }
    }

    /* Case 2: Concurrent insert with duplicate Primary Key collides */
    {
        enc r_p1 = make_item(f_id, f_name, f_code, 10, "item10_p1", "C10_A");
        enc r_p2 = make_item(f_id, f_name, f_code, 10, "item10_p2", "C10_B");

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p1.db, table_id, r_p1.b, r_p1.n, &e3));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

        cdb_faultenv_advance(&fe, 10000000);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p2.db, table_id, r_p2.b, r_p2.n, &e4));
        TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));

        {
            enc k;
            uint64_t fe3;
            uint8_t out[PAGE];
            size_t rlen;
            cdb_value v;
            arr_begin(&k);
            put_int(&k, 0, 10);
            arr_end(&k);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&p2.db, table_id, k.b, k.n, &fe3, &found));
            TEST_ASSERT_EQUAL_INT(1, found);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&p2.db, table_id, fe3, out, sizeof out, &rlen));
            field_of(out, rlen, f_name, &v, &found);
            TEST_ASSERT_EQUAL_INT(1, found);
            TEST_ASSERT_EQUAL_STRING_LEN("item10_p1", (const char *)v.payload, v.payload_len);
        }
    }

    /* Case 3: Concurrent insert with duplicate unique secondary key collides */
    {
        enc r_p1 = make_item(f_id, f_name, f_code, 20, "item20", "UNIQUE_CODE_X");
        enc r_p2 = make_item(f_id, f_name, f_code, 21, "item21", "UNIQUE_CODE_X");

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p1.db, table_id, r_p1.b, r_p1.n, &e5));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

        cdb_faultenv_advance(&fe, 10000000);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&p2.db, table_id, r_p2.b, r_p2.n, &e6));
        TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));

        {
            enc k20, k21;
            uint64_t fe5, fe6;
            arr_begin(&k20);
            put_int(&k20, 0, 20);
            arr_end(&k20);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&p2.db, table_id, k20.b, k20.n, &fe5, &found));
            TEST_ASSERT_EQUAL_INT(1, found);

            arr_begin(&k21);
            put_int(&k21, 0, 21);
            arr_end(&k21);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&p2.db, table_id, k21.b, k21.n, &fe6, &found));
            TEST_ASSERT_EQUAL_INT(0, found);
        }
    }

    /* Case 4: Concurrent replace on same row collides */
    {
        enc rep_p1 = make_item(f_id, f_name, f_code, 1, "widget_updated_p1", "W001");
        enc rep_p2 = make_item(f_id, f_name, f_code, 1, "widget_updated_p2", "W001");

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p1.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&p1.db, table_id, e1, rep_p1.b, rep_p1.n));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&p1.db));

        cdb_faultenv_advance(&fe, 10000000);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&p2.db, CDB_DURABILITY_DEFAULT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&p2.db, table_id, e1, rep_p2.b, rep_p2.n));
        TEST_ASSERT_EQUAL_INT(CDB_E_COLLISION, cdb_tx_commit(&p2.db));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p1.db));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&p2.db));

        {
            uint8_t out[PAGE];
            size_t rlen;
            cdb_value v;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&p2.db, table_id, e1, out, sizeof out, &rlen));
            field_of(out, rlen, f_name, &v, &found);
            TEST_ASSERT_EQUAL_INT(1, found);
            TEST_ASSERT_EQUAL_STRING_LEN("widget_updated_p1", (const char *)v.payload, v.payload_len);
        }
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    cdb_faultenv_on_sleep(&fe, NULL, NULL);
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

enum jtx_kind {
    JTX_COMMITTED = 0,
    JTX_ABORTED = 1,
    JTX_INFLIGHT = 2
};

struct jtx_ext {
    uint64_t counter;
    uint64_t stamp;
    uint64_t entry;
    const char *text;
    uint32_t key_index;
    const char *key_str;
    int prepare;
    enum jtx_kind kind;
};

static void plant_journal_ext(unsigned slot, uint64_t unsealed, const struct jtx_ext *t, unsigned n)
{
    char path[64];
    uint8_t buf[256], info[CDB_COMMIT_PAYLOAD_SIZE], prep[CDB_PREPARE_PAYLOAD_SIZE];
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
        cdb_record r;
        uint8_t payload[64];
        size_t len = strlen(t[i].text);
        uint64_t tx_id = ((uint64_t)slot << 48) | t[i].counter;

        payload[0] = CDB_OP_PATCH;
        memcpy(payload + 1, t[i].text, len);
        memset(&r, 0, sizeof r);
        r.kind = CDB_RECORD_DATA;
        r.tx_id = tx_id;
        r.store_id = 1;
        r.entry_id = t[i].entry;
        r.payload = payload;
        r.payload_len = (uint32_t)(len + 1);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));

        if (t[i].key_str != NULL) {
            uint8_t kpayload[CDB_WRITE_KEY_PAYLOAD_SIZE];
            uint32_t h = cdb_crc32c((const uint8_t *)t[i].key_str, strlen(t[i].key_str));
            cdb_store_u32(kpayload, t[i].key_index, CDB_ORDER_LITTLE);
            cdb_store_u32(kpayload + 4, h, CDB_ORDER_LITTLE);
            memset(&r, 0, sizeof r);
            r.kind = (uint8_t)CDB_RECORD_WRITE_KEY;
            r.tx_id = tx_id;
            r.store_id = 1;
            r.entry_id = 0;
            r.payload = kpayload;
            r.payload_len = CDB_WRITE_KEY_PAYLOAD_SIZE;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
        }

        if (t[i].prepare) {
            cdb_store_u64(prep, t[i].stamp, CDB_ORDER_LITTLE);
            memset(&r, 0, sizeof r);
            r.kind = (uint8_t)CDB_RECORD_PREPARE;
            r.tx_id = tx_id;
            r.payload = prep;
            r.payload_len = CDB_PREPARE_PAYLOAD_SIZE;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
        }

        if (t[i].kind == JTX_COMMITTED) {
            cdb_commit_info ci;
            memset(&ci, 0, sizeof ci);
            ci.timestamp = t[i].stamp;
            ci.delta_count = 1;
            cdb_commit_encode(&ci, info, CDB_ORDER_LITTLE);
            memset(&r, 0, sizeof r);
            r.kind = CDB_RECORD_COMMIT;
            r.tx_id = tx_id;
            r.payload = info;
            r.payload_len = CDB_COMMIT_PAYLOAD_SIZE;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
        } else if (t[i].kind == JTX_ABORTED) {
            memset(&r, 0, sizeof r);
            r.kind = (uint8_t)CDB_RECORD_ABORT;
            r.tx_id = tx_id;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_journal_append(&j, &r, buf, sizeof buf));
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    cdb_storage_close(&st);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, DIR "/db.cdbwork"));
}

static const struct jtx_ext slot0_ext_txs[] = {
    { 1, 10, 1, "a", 0, "k1", 1, JTX_COMMITTED },
    { 2, 20, 1, "bad1", 1, "k2", 1, JTX_ABORTED },
    { 3, 25, 2, "bad2", 0, "k3", 1, JTX_INFLIGHT },
    { 4, 40, 1, "c", 2, "k4", 1, JTX_COMMITTED }
};

static const struct jtx_ext slot1_ext_txs[] = {
    { 1, 15, 1, "b", 0, "ky1", 1, JTX_COMMITTED },
    { 2, 30, 2, "bad3", 1, "ky2", 1, JTX_ABORTED },
    { 3, 35, 2, "d", 0, "ky3", 1, JTX_COMMITTED }
};

static uint64_t two_crashed_journals_ext(void)
{
    uint64_t seq;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p1.db));
    seq = p1.db.pager.hs.record.committed_seq;
    plant_journal_ext(0, seq, slot0_ext_txs, sizeof slot0_ext_txs / sizeof slot0_ext_txs[0]);
    plant_journal_ext(1, seq, slot1_ext_txs, sizeof slot1_ext_txs / sizeof slot1_ext_txs[0]);
    return seq;
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

/* Recovery crash sweep where journals contain prepare, write-key, abort,
 * and in-flight records: replaying interleaves committed transactions by
 * stamp while cleanly ignoring prepared/aborted/uncommitted records under
 * any crash mode (spec 5.9.4, 14.4.7). */
static void test_recovery_crash_sweep_prepare_write_key(void)
{
    static const cdb_crash_mode modes[] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                            CDB_CRASH_REORDER };
    uint64_t ops, op;
    unsigned m;

    two_crashed_journals_ext();
    {
        uint64_t before = fe.clock.ops;
        TEST_ASSERT_EQUAL_INT(CDB_OK, open_proc(&p2));
        ops = fe.clock.ops - before;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&p2.db));
    }
    for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (op = 1; op <= ops; op++) {
            init_env();
            two_crashed_journals_ext();
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
    RUN_TEST(test_two_processes_share_and_merge);
    RUN_TEST(test_open_tx_rejected_after_peer_reconcile);
    RUN_TEST(test_reader_pin_blocks_reuse);
    RUN_TEST(test_live_peer_merges_crashed_peer);
    RUN_TEST(test_take_over_dead_slot_while_peer_live);
    RUN_TEST(test_missing_peer_journal_refused);
    RUN_TEST(test_crashed_process_recovered);
    RUN_TEST(test_id_block_claims);
    RUN_TEST(test_id_blocks_across_processes);
    RUN_TEST(test_collision_on_same_entry);
    RUN_TEST(test_collision_on_unique_key);
    RUN_TEST(test_collision_skips_rolled_back);
    RUN_TEST(test_collision_with_prepared);
    RUN_TEST(test_collision_with_prepared_commit);
    RUN_TEST(test_rebase_preserves_write_keys);
    RUN_TEST(test_rebase_drops_rolled_back);
    RUN_TEST(test_table_multi_process_e2e);
    RUN_TEST(test_idle_process_loses_slot);
    RUN_TEST(test_recovery_merges_slots_by_stamp);
    RUN_TEST(test_journal_from_newer_archive_set_aside);
    RUN_TEST(test_recovery_crash_sweep);
    RUN_TEST(test_recovery_crash_sweep_prepare_write_key);
    RUN_TEST(test_counters_survive_crash);
    RUN_TEST(test_host_hooks);
    return UNITY_END();
}
