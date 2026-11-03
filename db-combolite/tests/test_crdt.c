#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "combolite/cdb_crdt.h"
#include "cdb_db.h"
#include "cdb_crdt.h"
#include "cdb_faultenv.h"

#define FCAP (640u * 1024u)
#define PAGE_LOG2 9u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define STORE 1u
#define KV_PAGES 128u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 4096u];
static cdb_faultenv fe;
static cdb_env env;

/* Caller memory for Peer 1 */
static uint8_t arena1[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes1[NODES];
static cdb_free_extent free_list1[4096];
static uint8_t record_buf1[1024];
static uint8_t cat_work1[16384];
static cdb_index_entry cat_entries1[512];
static uint8_t cat_page1[PAGE];
static cdb_db_config cfg1;
static cdb_db db1;

/* Caller memory for Peer 2 */
static uint8_t arena2[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes2[NODES];
static cdb_free_extent free_list2[4096];
static uint8_t record_buf2[1024];
static uint8_t cat_work2[16384];
static cdb_index_entry cat_entries2[512];
static uint8_t cat_page2[PAGE];
static cdb_db_config cfg2;
static cdb_db db2;

static cdb_status kv_apply(void *ctx, cdb_pager *p, const cdb_delta *d)
{
    cdb_page_header t;
    uint64_t lid;
    cdb_status s;
    (void)ctx;
    TEST_ASSERT_TRUE(d->entry_id < KV_PAGES);
    if (d->op == CDB_OP_DELETE) {
        s = cdb_pager_free(p, d->entry_id);
        return s == CDB_E_NOT_FOUND ? CDB_OK : s;
    }
    while (p->next_lid < KV_PAGES) {
        if ((s = cdb_pager_alloc(p, &lid)) != CDB_OK)
            return s;
    }
    memset(&t, 0, sizeof t);
    t.type = CDB_PAGE_TYPE_DATA_TEST;
    t.store_id = d->store_id;
    return cdb_pager_write(p, d->entry_id, &t, d->data, d->len);
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
        h.db_id[i] = (uint8_t)(0x40 + i);
    return h;
}

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 4096;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

void setUp(void)
{
    init_env();

    memset(&cfg1, 0, sizeof cfg1);
    cfg1.pager.arena = arena1;
    cfg1.pager.arena_size = sizeof arena1;
    cfg1.pager.nodes = nodes1;
    cfg1.pager.node_cap = NODES;
    cfg1.pager.free_list = free_list1;
    cfg1.pager.free_cap = sizeof free_list1 / sizeof free_list1[0];
    cfg1.record_buf = record_buf1;
    cfg1.record_buf_size = sizeof record_buf1;
    cfg1.apply = kv_apply;
    cfg1.catalog_work.bytes = cat_work1;
    cfg1.catalog_work.bytes_size = sizeof cat_work1;
    cfg1.catalog_work.entries = cat_entries1;
    cfg1.catalog_work.entries_cap = 512;
    cfg1.catalog_page = cat_page1;
    cfg1.durability = CDB_DURABILITY_FULL;

    memset(&cfg2, 0, sizeof cfg2);
    cfg2.pager.arena = arena2;
    cfg2.pager.arena_size = sizeof arena2;
    cfg2.pager.nodes = nodes2;
    cfg2.pager.node_cap = NODES;
    cfg2.pager.free_list = free_list2;
    cfg2.pager.free_cap = sizeof free_list2 / sizeof free_list2[0];
    cfg2.record_buf = record_buf2;
    cfg2.record_buf_size = sizeof record_buf2;
    cfg2.apply = kv_apply;
    cfg2.catalog_work.bytes = cat_work2;
    cfg2.catalog_work.bytes_size = sizeof cat_work2;
    cfg2.catalog_work.entries = cat_entries2;
    cfg2.catalog_work.entries_cap = 512;
    cfg2.catalog_page = cat_page2;
    cfg2.durability = CDB_DURABILITY_FULL;
}

void tearDown(void)
{
}

/* ------------------------------------------------------------ 1. HLC Tests */

static void test_hlc_total_order_and_ticks(void)
{
    cdb_hlc a = {1000u, 0u, 1u};
    cdb_hlc b = {1000u, 0u, 2u};
    cdb_hlc c = {1000u, 1u, 1u};
    cdb_hlc d = {1001u, 0u, 1u};
    cdb_hlc clock = {0u, 0u, 42u};

    /* Comparison total ordering */
    TEST_ASSERT_TRUE(cdb_hlc_compare(a, a) == 0);
    TEST_ASSERT_TRUE(cdb_hlc_compare(a, b) < 0);
    TEST_ASSERT_TRUE(cdb_hlc_compare(b, a) > 0);
    TEST_ASSERT_TRUE(cdb_hlc_compare(a, c) < 0);
    TEST_ASSERT_TRUE(cdb_hlc_compare(c, d) < 0);
    TEST_ASSERT_TRUE(cdb_hlc_compare(d, c) > 0);

    /* Local tick advances */
    cdb_hlc_tick(&clock, 500u);
    TEST_ASSERT_EQUAL_UINT64(500u, clock.physical);
    TEST_ASSERT_EQUAL_UINT32(0u, clock.logical);
    TEST_ASSERT_EQUAL_UINT32(42u, clock.peer_id);

    /* Same wall time increments logical sequence */
    cdb_hlc_tick(&clock, 500u);
    TEST_ASSERT_EQUAL_UINT64(500u, clock.physical);
    TEST_ASSERT_EQUAL_UINT32(1u, clock.logical);

    /* New wall time resets logical sequence */
    cdb_hlc_tick(&clock, 600u);
    TEST_ASSERT_EQUAL_UINT64(600u, clock.physical);
    TEST_ASSERT_EQUAL_UINT32(0u, clock.logical);

    /* Remote clock update with higher physical time */
    {
        cdb_hlc remote = {700u, 5u, 99u};
        cdb_hlc_update(&clock, remote, 650u);
        TEST_ASSERT_EQUAL_UINT64(700u, clock.physical);
        TEST_ASSERT_EQUAL_UINT32(6u, clock.logical);
    }

    /* Remote clock update with concurrent equal physical time */
    {
        cdb_hlc remote = {700u, 10u, 99u};
        cdb_hlc_update(&clock, remote, 700u);
        TEST_ASSERT_EQUAL_UINT64(700u, clock.physical);
        TEST_ASSERT_EQUAL_UINT32(11u, clock.logical);
    }
}

/* ------------------------------------------------------------ 2. Version Vector Tests */

static void test_version_vector_dominance_and_concurrency(void)
{
    cdb_crdt_vector va, vb, vc;
    cdb_hlc h1 = {100u, 0u, 1u};
    cdb_hlc h2 = {100u, 0u, 2u};
    cdb_crdt_peer_entry out;

    memset(&va, 0, sizeof va);
    memset(&vb, 0, sizeof vb);
    memset(&vc, 0, sizeof vc);

    cdb_crdt_vector_set(&va, 1u, 5u, h1);
    cdb_crdt_vector_set(&va, 2u, 10u, h2);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_vector_get(&va, 1u, &out));
    TEST_ASSERT_EQUAL_UINT64(5u, out.counter);
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_crdt_vector_get(&va, 99u, &out));

    /* Identical vectors are equal */
    cdb_crdt_vector_set(&vb, 1u, 5u, h1);
    cdb_crdt_vector_set(&vb, 2u, 10u, h2);
    TEST_ASSERT_EQUAL_INT(CDB_CRDT_CMP_EQUAL, cdb_crdt_vector_compare(&va, &vb));

    /* vb strictly ahead: vb dominates va, va dominated by vb */
    cdb_crdt_vector_set(&vb, 1u, 6u, h1);
    TEST_ASSERT_EQUAL_INT(CDB_CRDT_CMP_DOMINATED, cdb_crdt_vector_compare(&va, &vb));
    TEST_ASSERT_EQUAL_INT(CDB_CRDT_CMP_DOMINATES, cdb_crdt_vector_compare(&vb, &va));

    /* vc concurrent: peer 1 higher, peer 2 lower */
    cdb_crdt_vector_set(&vc, 1u, 10u, h1);
    cdb_crdt_vector_set(&vc, 2u, 2u, h2);
    TEST_ASSERT_EQUAL_INT(CDB_CRDT_CMP_CONCURRENT, cdb_crdt_vector_compare(&va, &vc));

    /* Merge takes maximum counters */
    cdb_crdt_vector_merge(&va, &vc);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_vector_get(&va, 1u, &out));
    TEST_ASSERT_EQUAL_UINT64(10u, out.counter);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_vector_get(&va, 2u, &out));
    TEST_ASSERT_EQUAL_UINT64(10u, out.counter);
}

/* ------------------------------------------------------------ 3. PN-Counter Tests */

static void test_pn_counter_convergence(void)
{
    cdb_pn_counter c1, c2, m1, m2;

    cdb_pn_counter_init(&c1);
    cdb_pn_counter_init(&c2);

    /* Replica 1 operations */
    cdb_pn_counter_inc(&c1, 0u, 100u);
    cdb_pn_counter_dec(&c1, 0u, 25u);
    TEST_ASSERT_EQUAL_INT64(75, cdb_pn_counter_val(&c1));

    /* Replica 2 operations */
    cdb_pn_counter_inc(&c2, 1u, 50u);
    cdb_pn_counter_dec(&c2, 1u, 10u);
    TEST_ASSERT_EQUAL_INT64(40, cdb_pn_counter_val(&c2));

    /* Commutative merge: merge(c1, c2) == merge(c2, c1) */
    cdb_pn_counter_merge(&m1, &c1, &c2);
    cdb_pn_counter_merge(&m2, &c2, &c1);

    TEST_ASSERT_EQUAL_INT64(115, cdb_pn_counter_val(&m1));
    TEST_ASSERT_EQUAL_INT64(115, cdb_pn_counter_val(&m2));
    TEST_ASSERT_EQUAL_MEMORY(&m1, &m2, sizeof m1);
}

/* ------------------------------------------------------------ 4. LWW Document Field Merge */

static void test_lww_document_field_merge(void)
{
    const uint8_t val_alice[] = "Alice";
    const uint8_t val_alicia[] = "Alicia";
    const uint8_t val_active[] = "Active";
    const uint8_t val_engineer[] = "Engineer";

    cdb_crdt_field doc_a[2];
    cdb_crdt_field doc_b[3];
    cdb_crdt_field merged[8];
    size_t out_count = 0;

    /* Doc A: field 101="Alice" @ t=10, field 102="Active" @ t=10 */
    doc_a[0].field_id = 101u;
    doc_a[0].op = CDB_CRDT_FIELD_SET;
    doc_a[0].hlc = (cdb_hlc){10u, 0u, 1u};
    doc_a[0].val = val_alice;
    doc_a[0].val_len = sizeof val_alice;

    doc_a[1].field_id = 102u;
    doc_a[1].op = CDB_CRDT_FIELD_SET;
    doc_a[1].hlc = (cdb_hlc){10u, 0u, 1u};
    doc_a[1].val = val_active;
    doc_a[1].val_len = sizeof val_active;

    /* Doc B: field 101="Alicia" @ t=12 (wins), field 102 tombstoned @ t=15 (wins), field 103="Engineer" @ t=11 */
    doc_b[0].field_id = 101u;
    doc_b[0].op = CDB_CRDT_FIELD_SET;
    doc_b[0].hlc = (cdb_hlc){12u, 0u, 2u};
    doc_b[0].val = val_alicia;
    doc_b[0].val_len = sizeof val_alicia;

    doc_b[1].field_id = 102u;
    doc_b[1].op = CDB_CRDT_FIELD_TOMBSTONE;
    doc_b[1].hlc = (cdb_hlc){15u, 0u, 2u};
    doc_b[1].val = NULL;
    doc_b[1].val_len = 0u;

    doc_b[2].field_id = 103u;
    doc_b[2].op = CDB_CRDT_FIELD_SET;
    doc_b[2].hlc = (cdb_hlc){11u, 0u, 2u};
    doc_b[2].val = val_engineer;
    doc_b[2].val_len = sizeof val_engineer;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_lww_merge(doc_a, 2, doc_b, 3, merged, 8, &out_count));
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)out_count);

    /* Verify field 101 winner is Alicia */
    TEST_ASSERT_EQUAL_UINT64(101u, merged[0].field_id);
    TEST_ASSERT_EQUAL_STRING("Alicia", (const char *)merged[0].val);

    /* Verify field 102 winner is tombstone */
    TEST_ASSERT_EQUAL_UINT64(102u, merged[1].field_id);
    TEST_ASSERT_EQUAL_UINT8(CDB_CRDT_FIELD_TOMBSTONE, merged[1].op);

    /* Verify field 103 is present */
    TEST_ASSERT_EQUAL_UINT64(103u, merged[2].field_id);
    TEST_ASSERT_EQUAL_STRING("Engineer", (const char *)merged[2].val);
}

/* ------------------------------------------------------------ 5. Graph Link AWORSet */

static void test_graph_link_aworset(void)
{
    cdb_crdt_link_token add_early = {1, 10, 20, 10, 30, {100u, 0u, 1u}, 0};
    cdb_crdt_link_token remove_late = {1, 10, 20, 10, 30, {200u, 0u, 2u}, 1};
    cdb_crdt_link_token add_newer = {1, 10, 20, 10, 30, {300u, 0u, 3u}, 0};
    cdb_crdt_link_token del_same = {1, 10, 20, 10, 30, {300u, 0u, 3u}, 1};

    /* Later remove tombstones earlier addition */
    TEST_ASSERT_EQUAL_INT(0, cdb_crdt_link_resolve(add_early, remove_late));

    /* Concurrent or later addition wins over prior removal */
    TEST_ASSERT_EQUAL_INT(1, cdb_crdt_link_resolve(add_newer, remove_late));

    /* Add-Wins tie break on identical timestamp */
    TEST_ASSERT_EQUAL_INT(1, cdb_crdt_link_resolve(add_newer, del_same));
}

/* ------------------------------------------------------------ 6. Handshake & Slice Wire Codec */

static void test_replication_handshake_and_slice_wire_codec(void)
{
    uint8_t buf[512];
    size_t len = 0;
    cdb_crdt_vector vin, vout;
    uint32_t peer_id_out = 0;
    cdb_crdt_delta_rec sin[2], sout[4];
    size_t count_out = 0;

    /* Handshake encode / decode */
    memset(&vin, 0, sizeof vin);
    cdb_crdt_vector_set(&vin, 10u, 42u, (cdb_hlc){100u, 1u, 10u});
    cdb_crdt_vector_set(&vin, 20u, 84u, (cdb_hlc){200u, 2u, 20u});

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_handshake_encode(77u, &vin, buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_handshake_decode(buf, len, &peer_id_out, &vout));
    TEST_ASSERT_EQUAL_UINT32(77u, peer_id_out);
    TEST_ASSERT_EQUAL_INT(CDB_CRDT_CMP_EQUAL, cdb_crdt_vector_compare(&vin, &vout));

    /* Error handling */
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_crdt_handshake_encode(77u, &vin, buf, 10u, &len));
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_crdt_handshake_decode(buf, 8u, &peer_id_out, &vout));

    buf[0] = 0; /* corrupt magic */
    TEST_ASSERT_EQUAL_INT(CDB_E_MALFORMED, cdb_crdt_handshake_decode(buf, len, &peer_id_out, &vout));

    /* Slice encode / decode */
    sin[0].store_id = 1u;
    sin[0].entry_id = 10u;
    sin[0].op = CDB_OP_INSERT;
    sin[0].hlc = (cdb_hlc){100u, 0u, 1u};
    sin[0].data = (const uint8_t *)"hello";
    sin[0].len = 5u;

    sin[1].store_id = 1u;
    sin[1].entry_id = 11u;
    sin[1].op = CDB_OP_REPLACE;
    sin[1].hlc = (cdb_hlc){105u, 1u, 1u};
    sin[1].data = (const uint8_t *)"world";
    sin[1].len = 5u;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_slice_encode(1u, sin, 2, buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_crdt_slice_decode(buf, len, &peer_id_out, sout, 4, &count_out));
    TEST_ASSERT_EQUAL_UINT32(1u, peer_id_out);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)count_out);
    TEST_ASSERT_EQUAL_UINT64(10u, sout[0].entry_id);
    TEST_ASSERT_EQUAL_UINT8(CDB_OP_INSERT, sout[0].op);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", sout[0].data, 5);
    TEST_ASSERT_EQUAL_UINT64(11u, sout[1].entry_id);
    TEST_ASSERT_EQUAL_UINT8(CDB_OP_REPLACE, sout[1].op);
    TEST_ASSERT_EQUAL_STRING_LEN("world", sout[1].data, 5);
}

/* ------------------------------------------------------------ 7. Multi-Peer Replication Sync */

static void test_db_multi_peer_replication_sync(void)
{
    cdb_header h = settings();
    uint8_t slice[1024];
    size_t slice_len = 0;
    size_t applied = 0;
    cdb_hlc hlc1;
    cdb_crdt_vector v1, v2;
    uint8_t page_buf[PAGE];
    cdb_page_header ph;

    /* Open Peer 1 and Peer 2 databases */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db1, &env, "peer1", &cfg1, &h));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db2, &env, "peer2", &cfg2, &h));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_set_peer_id(&db1, 1u));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_set_peer_id(&db2, 2u));

    /* Peer 1 writes a transaction */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db1, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db1, STORE, 5u, CDB_OP_INSERT, "Peer1Data", 9u));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db1));

    /* Peer 1's HLC has ticked */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_get_hlc(&db1, &hlc1));
    TEST_ASSERT_EQUAL_UINT32(1u, hlc1.peer_id);
    TEST_ASSERT_TRUE(hlc1.physical > 0 || hlc1.logical > 0);

    /* Peer 2 asks Peer 1 for delta slice since Peer 2's known vector for Peer 1 (initially 0) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_get_crdt_vector(&db2, &v2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_crdt_export_slice(&db1, &v2, slice, sizeof slice, &slice_len));
    TEST_ASSERT_TRUE(slice_len > 0);

    /* Peer 2 imports delta slice */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_crdt_import_slice(&db2, slice, slice_len, &applied));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)applied);

    /* Verify Peer 2 now has the data on page 5 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db2.pager, 5u, page_buf, &ph));
    TEST_ASSERT_EQUAL_STRING_LEN("Peer1Data", page_buf + sizeof(cdb_page_header), 9);

    /* Peer 2 now commits its own change on page 6 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db2, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_put(&db2, STORE, 6u, CDB_OP_INSERT, "Peer2Data", 9u));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db2));

    /* Peer 1 syncs from Peer 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_get_crdt_vector(&db1, &v1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_crdt_export_slice(&db2, &v1, slice, sizeof slice, &slice_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_crdt_import_slice(&db1, slice, slice_len, &applied));
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)applied);

    /* Verify Peer 1 now has page 6 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db1.pager, 6u, page_buf, &ph));
    TEST_ASSERT_EQUAL_STRING_LEN("Peer2Data", page_buf + sizeof(cdb_page_header), 9);

    /* Re-importing already seen deltas applies 0 records (idempotence) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_crdt_import_slice(&db1, slice, slice_len, &applied));
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)applied);

    /* Clean close */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hlc_total_order_and_ticks);
    RUN_TEST(test_version_vector_dominance_and_concurrency);
    RUN_TEST(test_pn_counter_convergence);
    RUN_TEST(test_lww_document_field_merge);
    RUN_TEST(test_graph_link_aworset);
    RUN_TEST(test_replication_handshake_and_slice_wire_codec);
    RUN_TEST(test_db_multi_peer_replication_sync);
    return UNITY_END();
}
