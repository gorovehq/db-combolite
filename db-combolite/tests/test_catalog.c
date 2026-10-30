#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "cdb_catalog.h"
#include "cdb_faultdev.h"

#define CAP (4u * 1024u * 1024u)
#define PAGE_LOG2 12u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define WORK (96u * 1024u)
#define ENTRIES 1500u

static uint8_t current[CAP], durable[CAP], log_data[CAP];
static cdb_fault_record log_records[8192];
static cdb_faultdev dev;
static cdb_storage st;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static cdb_pager_config cfg;
static uint8_t work_bytes[WORK];
static cdb_index_entry work_entries[ENTRIES];
static cdb_index_work work;
static uint8_t cat_page[PAGE];

static cdb_pager pager;
static cdb_catalog cat;

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

static void create_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x77, sizeof h.db_id);
    init_device();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_create(&pager, &st, &cfg, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_open(&cat, &pager, &work, cat_page));
}

static cdb_status reopen(void)
{
    cdb_status s = cdb_pager_open(&pager, &st, &cfg);
    if (s != CDB_OK)
        return s;
    return cdb_catalog_open(&cat, &pager, &work, cat_page);
}

void setUp(void)
{
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
    create_db();
}

void tearDown(void) {}

static uint64_t create(const char *name, uint32_t model)
{
    uint64_t id = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_create_store(&cat, name, strlen(name), model, &id));
    return id;
}

static int find(const char *name, uint64_t *id)
{
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_find_store(&cat, name, strlen(name), id, &found));
    return found;
}

/* ------------------------------------------------------------ basics */

static void test_empty_catalog(void)
{
    cdb_store_info info;
    uint64_t id;
    TEST_ASSERT_FALSE(find("anything", &id));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_catalog_get_store(&cat, 1, &info));
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_FALSE(pager.cat_exists);
}

static void test_create_find_get_and_reopen(void)
{
    cdb_store_info info;
    uint64_t a = create("decisions", CDB_MODEL_DOCUMENT), b = create("owners", CDB_MODEL_RELATIONAL), id;
    TEST_ASSERT_EQUAL_UINT64(1, a); /* 0 is the catalog itself */
    TEST_ASSERT_EQUAL_UINT64(2, b);
    TEST_ASSERT_TRUE(find("decisions", &id));
    TEST_ASSERT_EQUAL_UINT64(a, id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&cat, b, &info));
    TEST_ASSERT_EQUAL_STRING("owners", info.name);
    TEST_ASSERT_EQUAL_UINT32(CDB_MODEL_RELATIONAL, info.model);
    TEST_ASSERT_EQUAL_UINT32(PAGE_LOG2, info.page_log2);

    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_TRUE(pager.cat_exists);
    TEST_ASSERT_TRUE(find("owners", &id));
    TEST_ASSERT_EQUAL_UINT64(b, id);
    TEST_ASSERT_EQUAL_UINT64(3, pager.catroot.next_object_id);
}

static void test_rejects_bad_input(void)
{
    uint64_t id;
    create("taken", CDB_MODEL_VECTOR);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_catalog_create_store(&cat, "taken", 5, CDB_MODEL_VECTOR, &id));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_create_store(&cat, "x", 1, 99, &id));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_create_store(&cat, "", 0, CDB_MODEL_VECTOR, &id));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_create_store(&cat, "\xC0\x80", 2, CDB_MODEL_VECTOR, &id));
}

static void test_rename_keeps_id(void)
{
    uint64_t a = create("alpha", CDB_MODEL_DOCUMENT), id;
    create("beta", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_rename_store(&cat, a, "gamma", 5));
    TEST_ASSERT_FALSE(find("alpha", &id));
    TEST_ASSERT_TRUE(find("gamma", &id));
    TEST_ASSERT_EQUAL_UINT64(a, id);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_catalog_rename_store(&cat, a, "beta", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_TRUE(find("gamma", &id));
}

/* '$' names stay the engine's: no store takes one, and the link store
 * keeps its own. */
static void test_rename_keeps_reserved_names(void)
{
    uint64_t a = create("alpha", CDB_MODEL_DOCUMENT), links = create("$links", CDB_MODEL_LINK), id;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_rename_store(&cat, a, "$mine", 5));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_rename_store(&cat, links, "plain", 5));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_catalog_rename_store(&cat, links, "$other", 6));
    TEST_ASSERT_TRUE(find("alpha", &id));
    TEST_ASSERT_TRUE(find("$links", &id));
    TEST_ASSERT_EQUAL_UINT64(links, id);
}

static void test_drop_frees_name_but_not_id(void)
{
    cdb_store_info info;
    uint64_t a = create("temp", CDB_MODEL_TIME_SERIES), b, id;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_drop_store(&cat, a));
    TEST_ASSERT_FALSE(find("temp", &id));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&cat, a, &info));
    TEST_ASSERT_TRUE(info.flags & CDB_STORE_DROPPED);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_catalog_drop_store(&cat, a));
    b = create("temp", CDB_MODEL_TIME_SERIES);
    TEST_ASSERT_TRUE(b > a); /* spec 4.12 */
}

static void test_schema_history(void)
{
    char buf[64];
    size_t len;
    uint32_t v1, v2;
    cdb_store_info info;
    uint64_t a = create("pool", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_add_schema(&cat, a, "schema one", 10, &v1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_add_schema(&cat, a, "schema two", 10, &v2));
    TEST_ASSERT_EQUAL_UINT32(1, v1);
    TEST_ASSERT_EQUAL_UINT32(2, v2);
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&cat, a, &info));
    TEST_ASSERT_EQUAL_UINT32(2, info.schema_version);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_schema(&cat, a, 1, buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_MEMORY("schema one", buf, len); /* history is kept (spec 4.11) */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_catalog_get_schema(&cat, a, 3, buf, sizeof buf, &len));
}

static void test_field_dictionary(void)
{
    char name[64];
    size_t len;
    uint64_t f1, f2, again, a = create("rows", CDB_MODEL_RELATIONAL), b = create("other", CDB_MODEL_RELATIONAL);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&cat, a, "status", 6, &f1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&cat, a, "owner", 5, &f2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&cat, a, "status", 6, &again));
    TEST_ASSERT_EQUAL_UINT64(1, f1);
    TEST_ASSERT_EQUAL_UINT64(2, f2);
    TEST_ASSERT_EQUAL_UINT64(f1, again);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&cat, b, "owner", 5, &again));
    TEST_ASSERT_EQUAL_UINT64(1, again); /* dictionaries are per store */
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_name(&cat, a, 2, name, sizeof name, &len));
    TEST_ASSERT_EQUAL_MEMORY("owner", name, len);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_catalog_field_name(&cat, a, 3, name, sizeof name, &len));
}

/* ------------------------------------------------------- redundancy */

static void test_first_catalog_commit_writes_both_slots(void)
{
    cdb_catroot r;
    uint8_t img[CDB_CATROOT_SLOT_SIZE];
    int i;
    create("one", CDB_MODEL_DOCUMENT);
    for (i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, pager.loc0 + (uint64_t)i * 512u, img, sizeof img));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catroot_decode(img, &r, CDB_ORDER_LITTLE));
    }
}

/* The body is addressed by logical IDs and covered by the committed header,
 * so losing the newest root slot loses nothing, and the ID counter, kept in
 * the body too, never goes backwards. */
static void test_damaged_newest_slot_falls_back(void)
{
    uint64_t id, third;
    unsigned newest;
    create("first", CDB_MODEL_DOCUMENT);
    create("second", CDB_MODEL_DOCUMENT);
    newest = (pager.cat_next_slot + 1u) % 2u;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, pager.loc0 + newest * 512u + 40u, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_TRUE(find("first", &id));
    TEST_ASSERT_TRUE(find("second", &id));
    TEST_ASSERT_EQUAL_UINT64(2, id);
    third = create("third", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_UINT64(3, third); /* not 2 again */
}

static void test_both_slots_damaged(void)
{
    create("first", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, pager.loc0 + 40u, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, pager.loc0 + 552u, 2));
    TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, reopen());
}

static void test_mirror_serves_damaged_body(void)
{
    cdb_anchor_entry e;
    uint64_t id;
    create("survivor", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&pager, pager.catroot.body_root, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, e.phys * PAGE + 200u, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_TRUE(find("survivor", &id));
}

static void test_body_serves_damaged_mirror(void)
{
    cdb_anchor_entry e;
    uint64_t id;
    create("survivor", CDB_MODEL_DOCUMENT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_lookup(&pager, pager.catroot.mirror_root, &e));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, e.phys * PAGE + 200u, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
    TEST_ASSERT_TRUE(find("survivor", &id));
}

/* ------------------------------------------------------- crash sweep */

/* S0: alpha. S1: + beta. S2: alpha renamed gamma. S3: beta schema v1.
 * S4: beta field "title" = 1. S5: gamma dropped. */
static cdb_status workload(int *acked)
{
    uint64_t beta, f;
    uint32_t v;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(reopen());
    TRY(cdb_catalog_create_store(&cat, "beta", 4, CDB_MODEL_RELATIONAL, &beta));
    *acked = 1;
    TRY(cdb_catalog_rename_store(&cat, 1, "gamma", 5));
    *acked = 2;
    TRY(cdb_catalog_add_schema(&cat, beta, "v1", 2, &v));
    *acked = 3;
    TRY(cdb_catalog_field_id(&cat, beta, "title", 5, &f));
    *acked = 4;
    TRY(cdb_catalog_drop_store(&cat, 1));
    *acked = 5;
#undef TRY
    return CDB_OK;
}

static int observed_state(void)
{
    cdb_store_info info;
    char buf[16];
    size_t len;
    uint64_t id;
    int has_alpha = find("alpha", &id), has_gamma = find("gamma", &id), has_beta = find("beta", &id);
    int schema, field, dropped;

    if (!has_beta)
        return has_alpha && !has_gamma ? 0 : -1;
    TEST_ASSERT_EQUAL_UINT64(2, id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&cat, 2, &info));
    schema = info.schema_version == 1;
    field = cdb_catalog_field_name(&cat, 2, 1, buf, sizeof buf, &len) == CDB_OK;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_get_store(&cat, 1, &info));
    dropped = (info.flags & CDB_STORE_DROPPED) != 0;
    if (has_alpha && !has_gamma && !schema && !field && !dropped)
        return 1;
    if (has_alpha)
        return -1;
    if (dropped)
        return !has_gamma && schema && field ? 5 : -1;
    if (!has_gamma)
        return -1;
    if (field)
        return schema ? 4 : -1;
    return schema ? 3 : 2;
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    const size_t tears[3] = { 0, 300, PAGE - 1 };
    uint64_t total, op, start, id;
    int acked, m;

    create_db();
    create("alpha", CDB_MODEL_DOCUMENT);
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
                create_db();
                create("alpha", CDB_MODEL_DOCUMENT);
                cdb_faultdev_arm(&dev, op, modes[m], modes[m] == CDB_CRASH_KEEP_PREFIX ? 1 : 0, tears[t]);
                TEST_ASSERT_NOT_EQUAL_INT(CDB_OK, workload(&acked));
                cdb_faultdev_reboot(&dev);
                TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
                k = observed_state();
                TEST_ASSERT_TRUE_MESSAGE(k >= 0, "the catalog holds a state that was never committed");
                TEST_ASSERT_TRUE_MESSAGE(k >= acked, "an acknowledged catalog change was lost");
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_create_store(&cat, "after", 5, CDB_MODEL_VECTOR, &id));
                TEST_ASSERT_TRUE(id >= 2u + (k >= 1)); /* no ID reused after recovery */
                TEST_ASSERT_EQUAL_INT(CDB_OK, reopen());
                TEST_ASSERT_TRUE(find("after", &id));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_catalog);
    RUN_TEST(test_create_find_get_and_reopen);
    RUN_TEST(test_rejects_bad_input);
    RUN_TEST(test_rename_keeps_id);
    RUN_TEST(test_rename_keeps_reserved_names);
    RUN_TEST(test_drop_frees_name_but_not_id);
    RUN_TEST(test_schema_history);
    RUN_TEST(test_field_dictionary);
    RUN_TEST(test_first_catalog_commit_writes_both_slots);
    RUN_TEST(test_damaged_newest_slot_falls_back);
    RUN_TEST(test_both_slots_damaged);
    RUN_TEST(test_mirror_serves_damaged_body);
    RUN_TEST(test_body_serves_damaged_mirror);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
