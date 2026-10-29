#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "cdb_faultdev.h"
#include "cdb_pager.h"

#define CAP (2u * 1024u * 1024u)
#define PAGE_LOG2 9u /* 512-byte pages keep the fan-out small, so trees grow quickly */
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

static uint8_t current[CAP], durable[CAP], log_data[CAP];
static cdb_fault_record log_records[8192];
static cdb_faultdev dev;
static cdb_storage st;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static cdb_pager_config cfg;
static uint8_t page[PAGE];

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

static cdb_header settings(void)
{
    cdb_header h;
    unsigned i;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    for (i = 0; i < 16; i++)
        h.db_id[i] = (uint8_t)(i * 17);
    return h;
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
}

void tearDown(void) {}

static cdb_page_header data_tmpl(void)
{
    cdb_page_header h;
    memset(&h, 0, sizeof h);
    h.type = CDB_PAGE_TYPE_DATA_TEST;
    h.store_id = 1;
    return h;
}

static void write_text(cdb_pager *p, uint64_t lid, const char *text)
{
    cdb_page_header t = data_tmpl();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_write(p, lid, &t, text, strlen(text)));
}

static void expect_text(cdb_pager *p, uint64_t lid, const char *text)
{
    cdb_page_header h;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(p, lid, page, &h));
    TEST_ASSERT_EQUAL_UINT32(strlen(text), h.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(text, page + cdb_page_header_size(p->fmt), strlen(text));
}

static void create(cdb_pager *p)
{
    cdb_header h = settings();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_create(p, &st, &cfg, &h));
}

/* ------------------------------------------------------------ basics */

static void test_create_and_open_empty(void)
{
    cdb_pager p;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_EQUAL_UINT(1, p.depth);
    TEST_ASSERT_EQUAL_UINT64(1, p.next_lid);
    TEST_ASSERT_EQUAL_UINT64(4, p.data_start); /* 1024 header bytes + 1024 location-0 bytes */
}

static void test_write_commit_reopen(void)
{
    cdb_pager p;
    uint64_t a, b;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &b));
    TEST_ASSERT_EQUAL_UINT64(1, a);
    TEST_ASSERT_EQUAL_UINT64(2, b);
    write_text(&p, a, "alpha");
    write_text(&p, b, "bravo");
    expect_text(&p, a, "alpha"); /* visible before commit */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, a, "alpha");
    expect_text(&p, b, "bravo");
    TEST_ASSERT_EQUAL_UINT64(3, p.next_lid);
}

static void test_uncommitted_changes_vanish_after_crash(void)
{
    cdb_pager p;
    uint64_t a, b;
    cdb_page_header h;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &a));
    write_text(&p, a, "committed");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    write_text(&p, a, "uncommitted");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &b));
    write_text(&p, b, "also lost");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st)); /* even if the data pages are durable */
    cdb_faultdev_crash_now(&dev, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultdev_reboot(&dev);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, a, "committed");
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(&p, b, page, &h));
}

static void test_tree_grows_to_three_levels(void)
{
    const uint64_t probes[] = { 1, 32, 33, 35, 36, 1799, 1800, 1999 };
    char text[32];
    cdb_pager p;
    cdb_anchor_entry e;
    cdb_page_header h;
    uint64_t lid = 0;
    size_t i;

    create(&p);
    for (i = 0; i < 2000; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    TEST_ASSERT_EQUAL_UINT64(2000, lid);
    TEST_ASSERT_EQUAL_UINT(3, p.depth);
    for (i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        snprintf(text, sizeof text, "page-%u", (unsigned)probes[i]);
        write_text(&p, probes[i], text);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_EQUAL_UINT(3, p.depth);
    TEST_ASSERT_EQUAL_UINT64(2001, p.next_lid);
    for (i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        snprintf(text, sizeof text, "page-%u", (unsigned)probes[i]);
        expect_text(&p, probes[i], text);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&p, 500, &e));
    TEST_ASSERT_EQUAL_UINT8(CDB_ANCHOR_UNUSED, e.state);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(&p, 500, page, &h));
}

static void test_growth_across_commits(void)
{
    cdb_pager p;
    uint64_t lid;
    size_t i;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, 1, "first");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p)); /* depth 1, committed */
    for (i = 0; i < 100; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, 90, "ninety");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p)); /* grew to depth 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_EQUAL_UINT(2, p.depth);
    expect_text(&p, 1, "first");
    expect_text(&p, 90, "ninety");
}

static void test_rewrite_frees_old_page(void)
{
    cdb_pager p;
    cdb_anchor_entry first;
    uint64_t lid;
    size_t i;
    int found = 0;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "v1");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&p, lid, &first));
    write_text(&p, lid, "v2");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, lid, "v2");
    for (i = 0; i < p.free_len; i++)
        if (free_list[i].start <= first.phys && first.phys < free_list[i].start + free_list[i].len)
            found = 1;
    TEST_ASSERT_TRUE(found);
}

static void test_free_and_ids_not_reused(void)
{
    cdb_pager p;
    cdb_page_header h;
    uint64_t a, b;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &a));
    write_text(&p, a, "gone soon");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_free(&p, a));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(&p, a, page, &h));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_write(&p, a, &h, "x", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(&p, a, page, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &b));
    TEST_ASSERT_TRUE(b > a);
}

static void test_mirror_repairs_damaged_anchor_page(void)
{
    cdb_pager p;
    uint64_t lid, root;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "survives");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    root = p.root_primary; /* depth 1: the root is the only leaf */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, root * PAGE + 100, 4));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, lid, "survives");

    /* With the mirror copy damaged as well, nothing is left to repair from. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, p.root_mirror * PAGE + 100, 4));
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_pager_open(&p, &st, &cfg));
}

static void test_damaged_data_page_is_reported(void)
{
    cdb_pager p;
    cdb_anchor_entry e;
    cdb_page_header h;
    uint64_t lid;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "payload");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&p, lid, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, e.phys * PAGE + 70, 0));
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_pager_read(&p, lid, page, &h));
}

static void test_sequences_increase_across_reopen(void)
{
    cdb_pager p;
    cdb_page_header h;
    uint64_t lid, first_seq;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "one");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&p, lid, page, &h));
    first_seq = h.write_seq;
    TEST_ASSERT_TRUE(first_seq >= 1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    write_text(&p, lid, "two");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&p, lid, page, &h));
    TEST_ASSERT_TRUE(h.write_seq > first_seq);
}

static void test_32bit_and_big_endian_file(void)
{
    cdb_pager p;
    cdb_header h = settings();
    uint64_t lid;
    h.layout_flags = 0; /* 32-bit addresses */
    h.order = CDB_ORDER_BIG;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_create(&p, &st, &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "narrow");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_EQUAL_INT(CDB_PAGE_FMT_FULL32, p.fmt);
    expect_text(&p, lid, "narrow");
}

/* ------------------------------------------------------- reuse and compaction */

static uint64_t file_pages(void)
{
    uint64_t size;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_size(&st, &size));
    return size / PAGE;
}

static void test_space_is_reused(void)
{
    cdb_pager p;
    uint64_t lid, after_warmup;
    char text[32];
    int i;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    for (i = 0; i < 10; i++) {
        snprintf(text, sizeof text, "v%d", i);
        write_text(&p, lid, text);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    }
    after_warmup = p.append_point;
    for (i = 10; i < 200; i++) {
        snprintf(text, sizeof text, "v%d", i);
        write_text(&p, lid, text);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    }
    /* 190 more commits, each writing seven pages, and the file did not grow. */
    TEST_ASSERT_EQUAL_UINT64(after_warmup, p.append_point);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, lid, "v199");
}

static void test_free_list_merges(void)
{
    cdb_pager p;
    uint64_t lid;
    int i;
    create(&p);
    for (i = 0; i < 40; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
        write_text(&p, lid, "x");
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    for (i = 1; i <= 40; i++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_free(&p, (uint64_t)i));
    /* 40 consecutive data pages collapse into one extent. */
    TEST_ASSERT_TRUE(p.free_len <= 4);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
}

static void make_holes(cdb_pager *p, unsigned pages, unsigned keep_every)
{
    char text[32];
    uint64_t lid;
    unsigned i;
    for (i = 0; i < pages; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(p, &lid));
        snprintf(text, sizeof text, "page-%u", (unsigned)lid);
        write_text(p, lid, text);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(p));
    for (i = 1; i <= pages; i++)
        if (i % keep_every != 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_free(p, i));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(p));
    p->dirty = 1; /* one more commit so the freed pages become reusable */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(p));
}

static void expect_survivors(cdb_pager *p, unsigned pages, unsigned keep_every)
{
    char text[32];
    cdb_page_header h;
    unsigned i;
    for (i = 1; i <= pages; i++) {
        if (i % keep_every == 0) {
            snprintf(text, sizeof text, "page-%u", i);
            expect_text(p, i, text);
        } else {
            TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(p, i, page, &h));
        }
    }
}

static void test_compaction_shrinks_file(void)
{
    cdb_pager p;
    uint64_t before, moved, reclaimed;
    create(&p);
    make_holes(&p, 300, 10);
    before = file_pages();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_compact(&p, 1000, &moved, &reclaimed));
    TEST_ASSERT_TRUE(moved > 0);
    TEST_ASSERT_TRUE(reclaimed > 0);
    TEST_ASSERT_TRUE_MESSAGE(file_pages() < before / 2, "compaction did not shrink the file");
    expect_survivors(&p, 300, 10);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_survivors(&p, 300, 10);
    /* Compacting again converges: within a few rounds nothing moves, and the
     * file never ends up larger than after the first compaction. */
    {
        uint64_t first = file_pages();
        int rounds = 0;
        do {
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_compact(&p, 1000, &moved, &reclaimed));
        } while (moved > 0 && ++rounds < 5);
        TEST_ASSERT_EQUAL_UINT64(0, moved);
        TEST_ASSERT_TRUE(file_pages() <= first);
    }
    expect_survivors(&p, 300, 10);
}

static void test_compaction_requires_committed_state(void)
{
    cdb_pager p;
    uint64_t lid, moved, reclaimed;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_pager_compact(&p, 10, &moved, &reclaimed));
}

static void test_reclaim_trims_crash_leftovers(void)
{
    cdb_pager p;
    uint64_t lid, committed_end, reclaimed;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, lid, "kept");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    committed_end = p.append_point;
    write_text(&p, lid, "uncommitted");
    write_text(&p, lid, "uncommitted again");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    cdb_faultdev_crash_now(&dev, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultdev_reboot(&dev);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    TEST_ASSERT_TRUE(file_pages() > committed_end);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_reclaim_tail(&p, &reclaimed));
    TEST_ASSERT_EQUAL_UINT64(committed_end, file_pages()); /* spec 3.7 reclaim-now */
    expect_text(&p, lid, "kept");
}

/* Reuse must never damage the state in the other header slot. Between a
 * commit and the next one, that slot holds the state before the latest
 * commit; if the newest slot rots, that state has to be readable in full,
 * even after new writes have reused space. */
static void test_previous_state_survives_reuse(void)
{
    cdb_pager p;
    uint64_t lid, extra;
    char text[32];
    int i;
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    for (i = 0; i < 30; i++) {
        snprintf(text, sizeof text, "gen-%d", i);
        write_text(&p, lid, text);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
    }
    /* New, uncommitted writes take reusable space. */
    for (i = 0; i < 12; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &extra));
        write_text(&p, extra, "uncommitted filler");
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, (uint64_t)p.hs.record_slot * 512u + 100u, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    expect_text(&p, lid, "gen-28"); /* the previous commit, intact */
}

/* Compaction sweep: crashing anywhere inside compaction never changes what
 * the database holds, and never leaves it unopenable. */
static void prepare_holes(void)
{
    cdb_pager p;
    init_device();
    create(&p);
    make_holes(&p, 120, 4);
}

static void test_compaction_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 100, PAGE - 1 };
    cdb_pager p;
    uint64_t total, op, moved, reclaimed, start;
    int m;

    prepare_holes();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
    start = dev.mutating_ops;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_compact(&p, 1000, &moved, &reclaimed));
    total = dev.mutating_ops - start;
    TEST_ASSERT_TRUE(moved > 0 && reclaimed > 0);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "compaction crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }

    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 3 : 1;
            for (t = 0; t < nt; t++) {
                prepare_holes();
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, cdb_pager_compact(&p, 1000, &moved, &reclaimed));
                cdb_faultdev_reboot(&dev);
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
                expect_survivors(&p, 120, 4);
                /* Finishing the job afterwards works. */
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_compact(&p, 1000, &moved, &reclaimed));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
                expect_survivors(&p, 120, 4);
            }
        }
    }
}

/* ------------------------------------------------------- crash sweep */

/* Base state S0 (commit seq 2): pages 1..3 hold "a1".."a3".
 * The swept session then commits S1 (seq 3): page 2 = "b2", new page 4 = "b4",
 * page 3 freed; and S2 (seq 4): page 1 = "c1". */
static void prepare_base(void)
{
    cdb_pager p;
    uint64_t lid;
    init_device();
    create(&p);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, 1, "a1");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, 2, "a2");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_alloc(&p, &lid));
    write_text(&p, 3, "a3");
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
}

static cdb_status session(cdb_pager *p, int *c1, int *c2, uint64_t *seq_used)
{
    cdb_page_header t = data_tmpl();
    uint64_t lid;
    cdb_status s;
    *c1 = *c2 = 0;
    *seq_used = 0;
    if ((s = cdb_pager_open(p, &st, &cfg)) != CDB_OK)
        return s;
#define STEP(call) do { s = (call); *seq_used = p->next_seq; if (s != CDB_OK) return s; } while (0)
    STEP(cdb_pager_write(p, 2, &t, "b2", 2));
    STEP(cdb_pager_alloc(p, &lid));
    STEP(cdb_pager_write(p, lid, &t, "b4", 2));
    STEP(cdb_pager_free(p, 3));
    STEP(cdb_pager_commit(p));
    *c1 = 1;
    STEP(cdb_pager_write(p, 1, &t, "c1", 2));
    STEP(cdb_pager_commit(p));
    *c2 = 1;
#undef STEP
    return CDB_OK;
}

static void verify_state(cdb_pager *p)
{
    cdb_page_header h;
    uint64_t seq = p->hs.record.committed_seq;
    TEST_ASSERT_TRUE(seq >= 2 && seq <= 4);
    if (seq == 2) {
        expect_text(p, 1, "a1");
        expect_text(p, 2, "a2");
        expect_text(p, 3, "a3");
        TEST_ASSERT_EQUAL_UINT64(4, p->next_lid);
    } else {
        expect_text(p, 1, seq == 4 ? "c1" : "a1");
        expect_text(p, 2, "b2");
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_pager_read(p, 3, page, &h));
        expect_text(p, 4, "b4");
        TEST_ASSERT_EQUAL_UINT64(5, p->next_lid);
    }
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 100, PAGE - 1 };
    cdb_pager p;
    uint64_t total, op, seq_used;
    int c1, c2, m;
    unsigned runs = 0;

    prepare_base();
    {
        uint64_t before = dev.mutating_ops;
        TEST_ASSERT_EQUAL_INT(CDB_OK, session(&p, &c1, &c2, &seq_used));
        total = dev.mutating_ops - before;
    }
    TEST_ASSERT_TRUE(total > 6);
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }

    for (op = 1; op <= total; op++) {
        for (m = 0; m < 4; m++) {
            size_t t, nt = modes[m] == CDB_CRASH_TORN ? 3 : 1;
            for (t = 0; t < nt; t++) {
                prepare_base();
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, session(&p, &c1, &c2, &seq_used));
                cdb_faultdev_reboot(&dev);

                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
                verify_state(&p);
                if (c1)
                    TEST_ASSERT_TRUE(p.hs.record.committed_seq >= 3);
                if (c2)
                    TEST_ASSERT_EQUAL_UINT64(4, p.hs.record.committed_seq);
                /* No sequence number used before the crash is ever issued again. */
                TEST_ASSERT_TRUE(p.next_seq >= seq_used);
                /* The database keeps working after recovery. */
                write_text(&p, 1, "after");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_commit(&p));
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_open(&p, &st, &cfg));
                expect_text(&p, 1, "after");
                runs++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)(total * 6u), runs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_open_empty);
    RUN_TEST(test_write_commit_reopen);
    RUN_TEST(test_uncommitted_changes_vanish_after_crash);
    RUN_TEST(test_tree_grows_to_three_levels);
    RUN_TEST(test_growth_across_commits);
    RUN_TEST(test_rewrite_frees_old_page);
    RUN_TEST(test_free_and_ids_not_reused);
    RUN_TEST(test_mirror_repairs_damaged_anchor_page);
    RUN_TEST(test_damaged_data_page_is_reported);
    RUN_TEST(test_sequences_increase_across_reopen);
    RUN_TEST(test_32bit_and_big_endian_file);
    RUN_TEST(test_space_is_reused);
    RUN_TEST(test_free_list_merges);
    RUN_TEST(test_compaction_shrinks_file);
    RUN_TEST(test_compaction_requires_committed_state);
    RUN_TEST(test_reclaim_trims_crash_leftovers);
    RUN_TEST(test_previous_state_survives_reuse);
    RUN_TEST(test_compaction_crash_sweep);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
