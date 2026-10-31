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
    cdb_db_config cfg;
    cdb_db db;
};
static struct proc p1, p2;

static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    cdb_page_header t;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    while (p->next_lid <= d->entry_id)
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
            return s;
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    return cdb_pager_write(p, d->entry_id, &t, d->data, d->len);
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
    RUN_TEST(test_dead_slot_taken_over);
    RUN_TEST(test_lobby_race_lost);
    RUN_TEST(test_full);
    RUN_TEST(test_join_crash_sweep);
    RUN_TEST(test_second_process_is_busy);
    RUN_TEST(test_crashed_process_recovered);
    RUN_TEST(test_idle_process_loses_slot);
    RUN_TEST(test_host_hooks);
    return UNITY_END();
}
