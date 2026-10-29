#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "cdb_endian.h"
#include "cdb_faultdev.h"
#include "cdb_index.h"
#include "cdb_key.h"

#define CAP (2u * 1024u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define WORK 16384u
#define ENTRIES 512u

static uint8_t current[CAP], durable[CAP], log_data[CAP];
static cdb_fault_record log_records[16384];
static cdb_faultdev dev;
static cdb_storage st;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static cdb_pager_config cfg;
static uint8_t work_bytes[WORK];
static cdb_index_entry work_entries[ENTRIES];
static cdb_index_work work;
static uint8_t cursor_page[PAGE];

static cdb_pager pager;
static cdb_index ix;

static void init_device(void)
{
    cdb_faultdev_config fc;
    memset(&fc, 0, sizeof fc);
    fc.current = current;
    fc.durable = durable;
    fc.capacity = CAP;
    fc.log = log_records;
    fc.log_cap = sizeof log_records / sizeof log_records[0];
    fc.log_data = log_data;
    fc.log_data_cap = CAP;
    fc.geometry.program_unit = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_init(&dev, &fc, &st));
}

static void create_pager(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x42, sizeof h.db_id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_create(&pager, &st, &cfg, &h));
}

void setUp(void)
{
    init_device();
    cfg.arena = arena;
    cfg.arena_size = sizeof arena;
    cfg.nodes = nodes;
    cfg.node_cap = NODES;
    cfg.free_list = free_list;
    cfg.free_cap = sizeof free_list / sizeof free_list[0];
    work.bytes = work_bytes;
    work.bytes_size = sizeof work_bytes;
    work.entries = work_entries;
    work.entries_cap = ENTRIES;
    TEST_ASSERT_TRUE(cdb_index_work_size(PAGE) <= WORK);
    TEST_ASSERT_TRUE(cdb_index_entries_needed(PAGE) <= ENTRIES);
    create_pager();
}

void tearDown(void) {}

/* Keys are integers; values are "v<id>". */
static size_t key_of(int64_t id, uint8_t *out)
{
    cdb_key_builder k;
    cdb_key_init(&k, out, 32);
    cdb_key_int(&k, id);
    return k.len;
}

static void put_id(int64_t id)
{
    uint8_t key[32];
    char val[24];
    size_t kl = key_of(id, key);
    int n = snprintf(val, sizeof val, "v%lld", (long long)id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(&ix, key, kl, val, (size_t)n));
}

static int has_id(int64_t id)
{
    uint8_t key[32];
    char val[32], expect[24];
    size_t vl, kl = key_of(id, key);
    int found, n;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(&ix, key, kl, val, sizeof val, &vl, &found));
    if (!found)
        return 0;
    n = snprintf(expect, sizeof expect, "v%lld", (long long)id);
    TEST_ASSERT_EQUAL_size_t((size_t)n, vl);
    TEST_ASSERT_EQUAL_MEMORY(expect, val, vl);
    return 1;
}

static void del_id(int64_t id, int expect_found)
{
    uint8_t key[32];
    int found;
    size_t kl = key_of(id, key);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_delete(&ix, key, kl, &found));
    TEST_ASSERT_EQUAL_INT(expect_found, found);
}

/* Scans from `from` and checks the keys come back strictly ascending.
 * Returns the number of entries and fills ids (if not NULL). */
static size_t scan(int64_t from, int64_t *ids, size_t cap)
{
    cdb_index_cursor c;
    uint8_t start[32], key[64], prev[64], val[64];
    size_t sl = key_of(from, start), kl, vl, prev_len = 0, n = 0;
    int done;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&c, &ix, cursor_page, start, sl));
    for (;;) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&c, key, sizeof key, &kl, val, sizeof val, &vl, &done));
        if (done)
            break;
        if (n > 0)
            TEST_ASSERT_EQUAL_INT(-1, cdb_key_compare(prev, prev_len, key, kl));
        memcpy(prev, key, kl);
        prev_len = kl;
        if (ids && n < cap) {
            /* decode: rank byte then big-endian with the sign bit flipped */
            uint64_t u = 0;
            size_t i;
            for (i = 1; i < 9; i++)
                u = (u << 8) | key[i];
            ids[n] = (int64_t)(u ^ UINT64_C(0x8000000000000000));
        }
        n++;
    }
    return n;
}

/* Deterministic shuffle of 0..n-1. */
static void shuffled(int64_t *out, size_t n, uint32_t seed)
{
    size_t i;
    for (i = 0; i < n; i++)
        out[i] = (int64_t)i;
    for (i = n; i-- > 1;) {
        size_t j;
        int64_t t;
        seed = seed * 1103515245u + 12345u;
        j = (seed >> 8) % (i + 1);
        t = out[i];
        out[i] = out[j];
        out[j] = t;
    }
}

/* ------------------------------------------------------------ basics */

static void test_put_get_replace(void)
{
    uint8_t key[32];
    char val[16];
    size_t kl, vl;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    put_id(42);
    TEST_ASSERT_TRUE(has_id(42));
    TEST_ASSERT_FALSE(has_id(41));
    kl = key_of(42, key);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(&ix, key, kl, "new", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(&ix, key, kl, val, sizeof val, &vl, &found));
    TEST_ASSERT_EQUAL_MEMORY("new", val, 3);
    TEST_ASSERT_EQUAL_size_t(1, scan(INT64_MIN, NULL, 0));
    del_id(41, 0);
    del_id(42, 1);
    TEST_ASSERT_EQUAL_size_t(0, scan(INT64_MIN, NULL, 0));
}

static void test_many_keys_shuffled(void)
{
    static int64_t order[2000], seen[2000];
    size_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    shuffled(order, 2000, 1);
    for (i = 0; i < 2000; i++) {
        put_id(order[i]);
        if (i % 250 == 249) /* commits let freed pages be reused */
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    for (i = 0; i < 2000; i++)
        TEST_ASSERT_TRUE(has_id((int64_t)i));
    TEST_ASSERT_FALSE(has_id(2000));
    {
        cdb_page_header h;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&pager, ix.root, cursor_page, &h));
        TEST_ASSERT_TRUE_MESSAGE(cursor_page[cdb_page_header_size(pager.fmt)] >= 2,
                                 "expected a tree at least three levels deep");
    }
    TEST_ASSERT_EQUAL_size_t(2000, scan(INT64_MIN, seen, 2000));
    for (i = 0; i < 2000; i++)
        TEST_ASSERT_EQUAL_INT64((int64_t)i, seen[i]);
    /* Range scan from the middle. */
    TEST_ASSERT_EQUAL_size_t(500, scan(1500, seen, 2000));
    TEST_ASSERT_EQUAL_INT64(1500, seen[0]);

    /* Delete the odd ones in another order, then everything. */
    shuffled(order, 2000, 2);
    for (i = 0; i < 2000; i++) {
        if (order[i] % 2)
            del_id(order[i], 1);
        if (i % 250 == 249)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    TEST_ASSERT_EQUAL_size_t(1000, scan(INT64_MIN, seen, 2000));
    for (i = 0; i < 1000; i++)
        TEST_ASSERT_EQUAL_INT64((int64_t)(2 * i), seen[i]);
    for (i = 0; i < 2000; i++) {
        del_id(order[i], order[i] % 2 == 0);
        if (i % 250 == 249)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    TEST_ASSERT_EQUAL_size_t(0, scan(INT64_MIN, NULL, 0));
    /* Everything collapsed back into an empty root leaf. */
    {
        cdb_page_header h;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&pager, ix.root, cursor_page, &h));
        TEST_ASSERT_EQUAL_UINT8(0, cursor_page[cdb_page_header_size(pager.fmt)]);
    }
}

static void test_survives_commit_and_reopen(void)
{
    uint64_t root;
    size_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    for (i = 0; i < 700; i++)
        put_id((int64_t)i * 3);
    root = ix.root;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&pager, &st, &cfg));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_open(&ix, &pager, root, &work));
    TEST_ASSERT_EQUAL_UINT64(root, ix.root); /* the root's logical ID never moves */
    for (i = 0; i < 700; i++)
        TEST_ASSERT_TRUE(has_id((int64_t)i * 3));
    TEST_ASSERT_FALSE(has_id(1));
}

static void test_largest_entries(void)
{
    uint8_t key[256], val[256];
    size_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    memset(val, 'v', sizeof val);
    for (i = 0; i < 300; i++) {
        memset(key, 'k', ix.max_key);
        key[ix.max_key - 2] = (uint8_t)(i >> 8);
        key[ix.max_key - 1] = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(&ix, key, ix.max_key, val, ix.max_value));
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_index_put(&ix, key, ix.max_key + 1, val, 1));
    TEST_ASSERT_EQUAL_size_t(300, scan(INT64_MIN, NULL, 0));
}

static void test_prefix_compression(void)
{
    char name[64];
    uint8_t key[64];
    cdb_key_builder k;
    size_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7,
                                                   CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX, &work));
    for (i = 0; i < 10; i++) {
        int n = snprintf(name, sizeof name, "governance/architecture/ADR-%04u", (unsigned)i);
        cdb_key_init(&k, key, sizeof key);
        cdb_key_string(&k, name, (size_t)n);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(&ix, key, k.len, "x", 1));
    }
    {
        cdb_page_header h;
        const uint8_t *pl;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&pager, ix.root, cursor_page, &h));
        pl = cursor_page + cdb_page_header_size(pager.fmt);
        TEST_ASSERT_TRUE(pl[1] & CDB_INDEX_FLAG_PREFIX);
        TEST_ASSERT_TRUE(cdb_load_u16(pl + 4, pager.order) >= 20); /* the shared prefix length */
    }
    TEST_ASSERT_EQUAL_size_t(10, scan(INT64_MIN, NULL, 0));
}

/* ------------------------------------------------------- crash sweep */

/* Base (committed): ids 0..199. S1: + 200..299, - even ids < 100. S2: + 300..349. */
static int in_state(int k, int64_t id)
{
    if (id < 0 || id >= 350)
        return 0;
    if (k == 0)
        return id < 200;
    if (id < 100 && id % 2 == 0)
        return 0;
    if (k == 1)
        return id < 300;
    return 1;
}

static uint64_t base_root;

static void prepare_base(void)
{
    size_t i;
    init_device();
    create_pager();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE | CDB_INDEX_FLAG_PREFIX, &work));
    for (i = 0; i < 200; i++)
        put_id((int64_t)i);
    base_root = ix.root;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
}

static cdb_status workload(int *acked)
{
    uint8_t key[32];
    char val[24];
    int64_t i;
    int found;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(cdb_pager_open(&pager, &st, &cfg));
    TRY(cdb_index_open(&ix, &pager, base_root, &work));
    for (i = 200; i < 300; i++) {
        int n = snprintf(val, sizeof val, "v%lld", (long long)i);
        TRY(cdb_index_put(&ix, key, key_of(i, key), val, (size_t)n));
    }
    for (i = 0; i < 100; i += 2)
        TRY(cdb_index_delete(&ix, key, key_of(i, key), &found));
    TRY(cdb_pager_commit(&pager));
    *acked = 1;
    for (i = 300; i < 350; i++) {
        int n = snprintf(val, sizeof val, "v%lld", (long long)i);
        TRY(cdb_index_put(&ix, key, key_of(i, key), val, (size_t)n));
    }
    TRY(cdb_pager_commit(&pager));
    *acked = 2;
#undef TRY
    return CDB_OK;
}

static int matching_state(void)
{
    static int64_t ids[400];
    size_t n = scan(INT64_MIN, ids, 400), i;
    int k;
    for (k = 0; k < 3; k++) {
        size_t expect = 0;
        int64_t id;
        int ok = 1;
        for (id = 0; id < 350; id++)
            expect += (size_t)in_state(k, id);
        if (n != expect)
            continue;
        for (i = 0; i < n && ok; i++)
            ok = in_state(k, ids[i]) && has_id(ids[i]);
        if (ok)
            return k;
    }
    return -1;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 100, PAGE - 1 };
    uint64_t total, op, start;
    int acked, m;

    prepare_base();
    start = dev.mutating_ops;
    TEST_ASSERT_EQUAL_INT(CDB_OK, workload(&acked));
    total = dev.mutating_ops - start;
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }
    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 3 : 1;
            for (t = 0; t < nt; t++) {
                int k;
                prepare_base();
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultdev_reboot(&dev);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&pager, &st, &cfg));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_open(&ix, &pager, base_root, &work));
                k = matching_state();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "the index holds a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                put_id(999);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
                TEST_ASSERT_TRUE(has_id(999));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_put_get_replace);
    RUN_TEST(test_many_keys_shuffled);
    RUN_TEST(test_survives_commit_and_reopen);
    RUN_TEST(test_largest_entries);
    RUN_TEST(test_prefix_compression);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
