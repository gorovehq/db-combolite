#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "cdb_endian.h"
#include "cdb_faultdev.h"
#include "cdb_index.h"
#include "cdb_key.h"
#include "cdb_overflow.h"

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
    static uint8_t val[8192]; /* room for overflow values too */
    cdb_index_cursor c;
    uint8_t start[32], key[64], prev[64];
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
        TEST_ASSERT_TRUE_MESSAGE(cursor_page[cdb_page_payload_offset(pager.fmt)] >= 2,
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
        TEST_ASSERT_EQUAL_UINT8(0, cursor_page[cdb_page_payload_offset(pager.fmt)]);
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
        pl = cursor_page + cdb_page_payload_offset(pager.fmt);
        TEST_ASSERT_TRUE(pl[1] & CDB_INDEX_FLAG_PREFIX);
        TEST_ASSERT_TRUE(cdb_load_u16(pl + 4, pager.order) >= 20); /* the shared prefix length */
    }
    TEST_ASSERT_EQUAL_size_t(10, scan(INT64_MIN, NULL, 0));
}

/* ---------------------------------------------------- overflow values */

#define BIG 6000u

/* The value of id at length len: a pattern that differs per id and byte. */
static void big_value(int64_t id, size_t len, uint8_t *out)
{
    size_t i;
    for (i = 0; i < len; i++)
        out[i] = (uint8_t)((uint64_t)id * 31u + i * 7u + (i >> 8));
}

static void put_big(int64_t id, size_t len)
{
    static uint8_t val[BIG];
    uint8_t key[32];
    big_value(id, len, val);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(&ix, key, key_of(id, key), val, len));
}

static void expect_big(int64_t id, size_t len)
{
    static uint8_t val[BIG], want[BIG];
    uint8_t key[32];
    size_t vl;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(&ix, key, key_of(id, key), val, sizeof val, &vl, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_size_t(len, vl);
    big_value(id, len, want);
    if (len)
        TEST_ASSERT_EQUAL_MEMORY(want, val, len);
}

/* Live overflow pages in the file. */
static uint64_t overflow_pages(void)
{
    static uint8_t page[PAGE];
    uint64_t lid, n = 0;
    for (lid = 1; lid < pager.next_lid; lid++) {
        cdb_anchor_entry e;
        cdb_page_header h;
        if (cdb_pager_lookup(&pager, lid, &e) != CDB_OK || e.phys == 0)
            continue;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&pager, lid, page, &h));
        n += h.type == CDB_PAGE_TYPE_OVERFLOW;
    }
    return n;
}

/* The chain pages the values of these lengths need. */
static uint64_t pages_for(const size_t *len, size_t n)
{
    uint64_t total = 0;
    size_t i;
    for (i = 0; i < n; i++)
        if (len[i] > ix.max_value)
            total += cdb_overflow_pages(&pager, len[i]);
    return total;
}

static void test_overflow_values(void)
{
    static size_t len[120];
    static uint8_t val[BIG];
    uint8_t key[64];
    size_t i, vl, kl;
    int found, done;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    /* Lengths around the inline limit, and up to many pages. */
    for (i = 0; i < 120; i++) {
        size_t pick[6] = { 0, ix.max_value, ix.max_value + 1u, cdb_overflow_room(&pager),
                           cdb_overflow_room(&pager) + 1u, BIG };
        len[i] = i < 6 ? pick[i] : (i * 397u) % BIG;
        put_big((int64_t)i, len[i]);
        if (i % 8 == 7) /* the pager holds new anchors in memory until a commit */
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    for (i = 0; i < 120; i++)
        expect_big((int64_t)i, len[i]);
    TEST_ASSERT_EQUAL_UINT64(pages_for(len, 120), overflow_pages());

    /* The cursor gives every value back, in key order. */
    {
        cdb_index_cursor c;
        int64_t id = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&c, &ix, cursor_page, NULL, 0));
        for (;;) {
            static uint8_t want[BIG];
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_next(&c, key, sizeof key, &kl, val, sizeof val, &vl, &done));
            if (done)
                break;
            TEST_ASSERT_EQUAL_size_t(len[id], vl);
            big_value(id, vl, want);
            if (vl)
                TEST_ASSERT_EQUAL_MEMORY(want, val, vl);
            id++;
        }
        TEST_ASSERT_EQUAL_INT64(120, id);
    }

    /* A buffer too small reports the length. */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_index_get(&ix, key, key_of(5, key), val, 100, &vl, &found));
    TEST_ASSERT_EQUAL_size_t(BIG, vl);
    {
        cdb_index_cursor c;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_seek(&c, &ix, cursor_page, key, key_of(5, key)));
        TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_index_next(&c, key, sizeof key, &kl, val, 100, &vl, &done));
        TEST_ASSERT_EQUAL_size_t(BIG, vl);
    }

    /* Replacing: big to small, small to big, big to big. Chains are freed. */
    for (i = 0; i < 120; i++) {
        len[i] = (len[i] * 7u + 1234u) % BIG;
        put_big((int64_t)i, len[i]);
        if (i % 8 == 7)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    for (i = 0; i < 120; i++)
        expect_big((int64_t)i, len[i]);
    TEST_ASSERT_EQUAL_UINT64(pages_for(len, 120), overflow_pages());

    /* It survives a commit and reopen. */
    {
        uint64_t root = ix.root;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&pager, &st, &cfg));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_open(&ix, &pager, root, &work));
        for (i = 0; i < 120; i++)
            expect_big((int64_t)i, len[i]);
    }

    /* Deleting releases every chain. */
    for (i = 0; i < 120; i++) {
        del_id((int64_t)i, 1);
        len[i] = 0;
        if (i % 8 == 7)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
    }
    TEST_ASSERT_EQUAL_UINT64(0, overflow_pages());
    TEST_ASSERT_EQUAL_size_t(0, scan(INT64_MIN, NULL, 0));
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

/* Crashes the workload at every mutating operation in every mode; recovery
 * must give a committed state (match returns its number, or -1), at least
 * the last one acknowledged, and the index must still take changes. */
static void sweep(void (*prepare)(void), cdb_status (*work_fn)(int *), int (*match)(void))
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 100, PAGE - 1 };
    uint64_t total, op, start;
    int acked, m;

    prepare();
    start = dev.mutating_ops;
    TEST_ASSERT_EQUAL_INT(CDB_OK, work_fn(&acked));
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
                prepare();
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, work_fn(&acked));
                cdb_faultdev_reboot(&dev);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&pager, &st, &cfg));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_open(&ix, &pager, base_root, &work));
                k = match();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "the index holds a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged commit was lost");
                put_id(999);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
                TEST_ASSERT_TRUE(has_id(999));
            }
        }
    }
}

static void test_crash_sweep(void)
{
    sweep(prepare_base, workload, matching_state);
}

/* Overflow values. Base (committed): ids 0..7. S1: ids 0..3 change length
 * (odd ones shrink inline, even ones grow), 4 and 5 go, 8..10 arrive.
 * S2: 6 shrinks inline, 11 arrives. SIZE_MAX: absent. */
#define OV_IDS 12

static size_t ov_len(int k, int id)
{
    if (k >= 2 && id == 6)
        return 40;
    if (k >= 2 && id == 11)
        return 2500;
    if (k >= 1 && id < 4)
        return id % 2 ? 20u : 2000u + (size_t)id * 100u;
    if (k >= 1 && (id == 4 || id == 5))
        return SIZE_MAX;
    if (k >= 1 && id >= 8 && id <= 10)
        return (size_t)id * 900u % 3000u + 600u;
    return id < 8 ? 300u + (size_t)id * 350u : SIZE_MAX;
}

static void prepare_ov(void)
{
    int id;
    init_device();
    create_pager();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_create(&ix, &pager, 7, CDB_INDEX_FLAG_UNIQUE, &work));
    for (id = 0; id < OV_IDS; id++)
        if (ov_len(0, id) != SIZE_MAX)
            put_big(id, ov_len(0, id));
    base_root = ix.root;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&pager));
}

static cdb_status workload_ov(int *acked)
{
    static uint8_t val[BIG];
    uint8_t key[32];
    int id, k, found;
    cdb_status s;
    *acked = 0;
    if ((s = cdb_pager_open(&pager, &st, &cfg)) != CDB_OK || (s = cdb_index_open(&ix, &pager, base_root, &work)) != CDB_OK)
        return s;
    for (k = 1; k <= 2; k++) {
        for (id = 0; id < OV_IDS; id++) {
            size_t was = ov_len(k - 1, id), now = ov_len(k, id);
            if (was == now)
                continue;
            if (now == SIZE_MAX) {
                s = cdb_index_delete(&ix, key, key_of(id, key), &found);
            } else {
                big_value(id, now, val);
                s = cdb_index_put(&ix, key, key_of(id, key), val, now);
            }
            if (s != CDB_OK)
                return s;
        }
        if ((s = cdb_pager_commit(&pager)) != CDB_OK)
            return s;
        *acked = k;
    }
    return CDB_OK;
}

static int matching_ov(void)
{
    static uint8_t val[BIG], want[BIG];
    uint64_t pages = overflow_pages();
    int k;
    for (k = 0; k < 3; k++) {
        uint64_t need = 0;
        size_t n = 0;
        int id, ok = 1;
        for (id = 0; id < OV_IDS && ok; id++) {
            uint8_t key[32];
            size_t vl, len = ov_len(k, id);
            int found;
            if (cdb_index_get(&ix, key, key_of(id, key), val, sizeof val, &vl, &found) != CDB_OK)
                return -1;
            if (len == SIZE_MAX) {
                ok = !found;
                continue;
            }
            big_value(id, len, want);
            ok = found && vl == len && memcmp(val, want, len) == 0;
            n++;
            if (len > ix.max_value)
                need += cdb_overflow_pages(&pager, len);
        }
        if (ok && n == scan(INT64_MIN, NULL, 0) && pages == need)
            return k;
    }
    return -1;
}

static void test_overflow_crash_sweep(void)
{
    sweep(prepare_ov, workload_ov, matching_ov);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_put_get_replace);
    RUN_TEST(test_many_keys_shuffled);
    RUN_TEST(test_survives_commit_and_reopen);
    RUN_TEST(test_largest_entries);
    RUN_TEST(test_prefix_compression);
    RUN_TEST(test_overflow_values);
    RUN_TEST(test_crash_sweep);
    RUN_TEST(test_overflow_crash_sweep);
    return UNITY_END();
}
