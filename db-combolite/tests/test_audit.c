#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "cdb_audit.h"
#include "cdb_catroot.h"
#include "cdb_db.h"
#include "cdb_faultdev.h"
#include "cdb_faultenv.h"
#include "cdb_index.h"
#include "cdb_page.h"
#include "cdb_ts.h"
#include "combolite/cdb_crypto.h"
#include "combolite/cdb_value.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 9u /* 512-byte pages */
#define PAGE (1u << PAGE_LOG2)
#define NODES 32u

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[8192];
static uint8_t record_buf[2048];
static uint8_t cat_work[16384];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t ts_buffers[3u * PAGE];
static uint8_t ts_work[(CDB_TS_BLOCK_MAX_POINTS + 1u) * sizeof(cdb_ts_point)];
static cdb_db_config cfg;
static cdb_db db;

static void init_env(void)
{
    cdb_faultenv_config c;
    c.arena = env_arena;
    c.file_capacity = FCAP;
    c.records = env_records;
    c.records_per_file = 8192;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

void setUp(void)
{
    init_env();
    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof arena;
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof free_list / sizeof free_list[0];
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof record_buf;
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof cat_work;
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 512;
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_buffers;
    cfg.ts_buffers = ts_buffers;
    cfg.ts_work = ts_work;
    cfg.ts_work_size = sizeof ts_work;
    cfg.durability = CDB_DURABILITY_FULL;
}

void tearDown(void)
{
    (void)cdb_db_close(&db);
}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x7E, sizeof h.db_id);
    return cdb_db_open(&db, &env, "test_audit_db", &cfg, &h);
}

static void test_audit_hash_chain(void)
{
    int valid = 0;
    cdb_audit_event events[8];
    uint8_t payload_buf[1024];
    size_t count = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    /* Empty audit series is valid */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(1, valid);

    /* Log 5 diverse audit events */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_BATON, 0, 0, 0, "baton handoff", 13));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_SCHEMA, 1, 10, 1, "schema create", 13));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_REVISION, 42, 10, 100, "rev 1", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_KEY_OP, 0, 0, 2, "key rotated", 11));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_CUSTOM, 99, 0, 3, "health check", 12));

    /* Verify chain validity */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(1, valid);

    /* Query logged events */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_query(&db, INT64_MIN, INT64_MAX, payload_buf, sizeof payload_buf, events, 8, &count));
    TEST_ASSERT_EQUAL_size_t(5, count);

    /* Verify event contents and hash continuity */
    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_BATON, events[0].event_type);
    TEST_ASSERT_EQUAL_UINT32(13, events[0].payload_len);
    TEST_ASSERT_EQUAL_STRING_LEN("baton handoff", (const char *)events[0].payload, 13);

    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_SCHEMA, events[1].event_type);
    TEST_ASSERT_EQUAL_UINT32(1, events[1].principal_id);
    TEST_ASSERT_EQUAL_UINT64(10, events[1].store_id);
    /* Hash chain check: event 1's prev_hash matches event 0's hash */
    TEST_ASSERT_EQUAL_MEMORY(events[0].hash, events[1].prev_hash, 32);

    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_REVISION, events[2].event_type);
    TEST_ASSERT_EQUAL_UINT32(42, events[2].principal_id);
    TEST_ASSERT_EQUAL_UINT64(100, events[2].entity_id);
    TEST_ASSERT_EQUAL_MEMORY(events[1].hash, events[2].prev_hash, 32);

    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_KEY_OP, events[3].event_type);
    TEST_ASSERT_EQUAL_MEMORY(events[2].hash, events[3].prev_hash, 32);

    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_CUSTOM, events[4].event_type);
    TEST_ASSERT_EQUAL_MEMORY(events[3].hash, events[4].prev_hash, 32);

    /* Close and reopen database */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    /* Chain remains valid across database reopen */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(1, valid);

    /* Append an event after reopen; verify continuity continues */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_CUSTOM, 100, 0, 4, "reopen event", 12));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(1, valid);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_query(&db, INT64_MIN, INT64_MAX, payload_buf, sizeof payload_buf, events, 8, &count));
    TEST_ASSERT_EQUAL_size_t(6, count);
    TEST_ASSERT_EQUAL_MEMORY(events[4].hash, events[5].prev_hash, 32);
}

static void test_audit_tamper_detection(void)
{
    int valid = 0;
    uint8_t pg[PAGE];
    cdb_page_header ph;
    size_t hdr_sz, payload_cap;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_BATON, 0, 0, 0, "entry 1", 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_SCHEMA, 1, 10, 1, "entry 2", 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_log(&db, CDB_AUDIT_REVISION, 2, 10, 2, "entry 3", 7));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(1, valid);

    /* Deliberately tamper with the audit index root page bytes */
    TEST_ASSERT_TRUE(db.pager.catroot.audit_root != 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_read(&db.pager, db.pager.catroot.audit_root, pg, &ph));
    hdr_sz = cdb_page_payload_offset(db.pager.fmt);
    payload_cap = cdb_page_capacity(db.pager.fmt, db.pager.page_size);

    /* Flip bits in the entry region at the end of the node */
    pg[hdr_sz + payload_cap - 20] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_pager_write(&db.pager, db.pager.catroot.audit_root, &ph, pg + hdr_sz, payload_cap));

    /* Verify now detects tampering and marks valid = 0 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_verify(&db, INT64_MIN, INT64_MAX, &valid));
    TEST_ASSERT_EQUAL_INT(0, valid);
}

static void test_audit_collision_logging(void)
{
    uint64_t store, series;
    int created;
    cdb_ts_def def;
    cdb_ts_in pin;
    uint8_t enc[128];
    cdb_value_writer w;
    cdb_audit_event events[4];
    uint8_t payload_buf[256];
    size_t count = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "metrics", 7, CDB_TS_LATE_ACCEPT, 0, &store));

    memset(&def, 0, sizeof def);
    def.precision = CDB_TS_MILLIS;
    def.window = 60000;
    def.nfields = 1;
    def.fields[0].id = 1;
    def.fields[0].type = CDB_TYPE_INT;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "temperature", 11, NULL, 0, &def, &series, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Append first point: ts = 1000, value = 42 */
    cdb_value_writer_init(&w, enc, sizeof enc, db.pager.order, CDB_DECIMAL_UNBOUNDED);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, 42);
    cdb_value_end_object(&w);

    pin.ts = 1000;
    pin.use_clock = 0;
    pin.values = enc;
    pin.len = cdb_value_writer_len(&w);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, series, &pin, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Append colliding point: ts = 1000, value = 99 (different value) */
    cdb_value_writer_init(&w, enc, sizeof enc, db.pager.order, CDB_DECIMAL_UNBOUNDED);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_int(&w, 99);
    cdb_value_end_object(&w);

    pin.ts = 1000;
    pin.values = enc;
    pin.len = cdb_value_writer_len(&w);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, series, &pin, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Collision must have been written to the system audit series with the replaced value */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_audit_query(&db, INT64_MIN, INT64_MAX, payload_buf, sizeof payload_buf, events, 4, &count));
    TEST_ASSERT_EQUAL_size_t(1, count);
    TEST_ASSERT_EQUAL_UINT16(CDB_AUDIT_COLLISION, events[0].event_type);
    TEST_ASSERT_EQUAL_UINT64(store, events[0].store_id);
    TEST_ASSERT_EQUAL_UINT64(series, events[0].entity_id);
    TEST_ASSERT_TRUE(events[0].payload_len > 0);

    /* Decode replaced point from payload and verify value 42 was recorded */
    {
        cdb_value obj, v;
        int found = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(events[0].payload, events[0].payload_len, db.pager.order, &obj));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 1, &v, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_INT64(42, v.as.i64);
    }
}

static void test_audit_series_protection(void)
{
    int64_t dropped = 0;
    cdb_ts_downsample_spec spec;
    size_t gen = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    /* Attempting retention or drop on the audit series fails with CDB_E_SECURITY */
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_ts_set_retention(&db, CDB_AUDIT_STORE_ID, 1, 1000));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_ts_apply_retention(&db, CDB_AUDIT_STORE_ID, 1, 1000, &dropped));
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_ts_drop_before(&db, CDB_AUDIT_STORE_ID, 1, 1000));

    memset(&spec, 0, sizeof spec);
    spec.src_store = CDB_AUDIT_STORE_ID;
    spec.dst_store = 100;
    spec.bucket_step = 60;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_ts_downsample_run(&db, &spec, 0, 1000, &gen));

    spec.src_store = 100;
    spec.dst_store = CDB_AUDIT_STORE_ID;
    TEST_ASSERT_EQUAL_INT(CDB_E_SECURITY, cdb_ts_downsample_run(&db, &spec, 0, 1000, &gen));
}

static void test_ts_retention(void)
{
    uint64_t store, series;
    int created;
    cdb_ts_def def;
    int64_t ret = 0, dropped = 0;
    cdb_ts_row rows[16];
    uint8_t buf[1024];
    size_t n = 0;
    int64_t next = 0;
    cdb_ts_in pin;
    uint8_t enc[128];
    cdb_value_writer w;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));

    memset(&def, 0, sizeof def);
    def.precision = CDB_TS_MILLIS;
    def.window = 60000;
    def.nfields = 1;
    def.fields[0].id = 1;
    def.fields[0].type = CDB_TYPE_INT;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "s", 1, NULL, 0, &def, &series, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Default retention is 0 (keep everything) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_get_retention(&db, store, series, &ret));
    TEST_ASSERT_EQUAL_INT64(0, ret);

    /* Set retention to 5000 ticks */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_set_retention(&db, store, series, 5000));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_get_retention(&db, store, series, &ret));
    TEST_ASSERT_EQUAL_INT64(5000, ret);

    /* Append points at 1000, 2000, 3000, 8000, 9000 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    for (int64_t t = 1000; t <= 9000; t += (t == 3000 ? 5000 : 1000)) {
        cdb_value_writer_init(&w, enc, sizeof enc, db.pager.order, CDB_DECIMAL_UNBOUNDED);
        cdb_value_begin_object(&w);
        cdb_value_field(&w, 1);
        cdb_value_write_int(&w, t * 10);
        cdb_value_end_object(&w);

        pin.ts = t;
        pin.use_clock = 0;
        pin.values = enc;
        pin.len = cdb_value_writer_len(&w);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, series, &pin, 1));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Apply retention with now = 9000: cutoff = 9000 - 5000 = 4000 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_apply_retention(&db, store, series, 9000, &dropped));
    TEST_ASSERT_EQUAL_INT64(4000, dropped);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Range query: points 1000, 2000, 3000 must be dropped; 8000 and 9000 remain */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, series, 0, 10000, buf, sizeof buf, rows, 16, &n, &next));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_INT64(8000, rows[0].ts);
    TEST_ASSERT_EQUAL_INT64(9000, rows[1].ts);

    /* Setting retention to 0 keeps everything */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_set_retention(&db, store, series, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_get_retention(&db, store, series, &ret));
    TEST_ASSERT_EQUAL_INT64(0, ret);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_apply_retention(&db, store, series, 15000, &dropped));
    TEST_ASSERT_EQUAL_INT64(INT64_MIN, dropped);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
}

static void test_ts_downsampling(void)
{
    uint64_t store, src_s, dst_s;
    int created;
    cdb_ts_def src_def, dst_def;
    cdb_ts_downsample_spec spec;
    size_t gen = 0;
    cdb_ts_in pin;
    uint8_t enc[128], buf[1024];
    cdb_value_writer w;
    cdb_ts_row rows[16];
    size_t n = 0;
    int64_t next = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));

    /* Source series: field 1 = INT, field 2 = F64 */
    memset(&src_def, 0, sizeof src_def);
    src_def.precision = CDB_TS_MILLIS;
    src_def.window = 60000;
    src_def.nfields = 2;
    src_def.fields[0].id = 1;
    src_def.fields[0].type = CDB_TYPE_INT;
    src_def.fields[1].id = 2;
    src_def.fields[1].type = CDB_TYPE_F64;

    /* Destination series: field 1 = INT (will hold AVG), field 2 = F64 (will hold SUM) */
    memset(&dst_def, 0, sizeof dst_def);
    dst_def.precision = CDB_TS_MILLIS;
    dst_def.window = 60000;
    dst_def.nfields = 2;
    dst_def.fields[0].id = 1;
    dst_def.fields[0].type = CDB_TYPE_INT;
    dst_def.fields[1].id = 2;
    dst_def.fields[1].type = CDB_TYPE_F64;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "raw", 3, NULL, 0, &src_def, &src_s, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "summary", 7, NULL, 0, &dst_def, &dst_s, &created));

    /* Append points:
     * Bucket 0..100:
     *   ts = 10: f1 = 10, f2 = 1.5
     *   ts = 20: f1 = 20, f2 = 2.5
     *   ts = 30: f1 = 30, f2 = 3.5
     *   -> avg(f1) = 20, sum(f2) = 7.5
     * Bucket 100..200:
     *   ts = 110: f1 = 100, f2 = 10.0
     *   ts = 120: f1 = 200, f2 = 20.0
     *   -> avg(f1) = 150, sum(f2) = 30.0
     */
    {
        int64_t timestamps[5] = { 10, 20, 30, 110, 120 };
        int64_t i_vals[5] = { 10, 20, 30, 100, 200 };
        double f_vals[5] = { 1.5, 2.5, 3.5, 10.0, 20.0 };

        for (size_t i = 0; i < 5; i++) {
            cdb_value_writer_init(&w, enc, sizeof enc, db.pager.order, CDB_DECIMAL_UNBOUNDED);
            cdb_value_begin_object(&w);
            cdb_value_field(&w, 1);
            cdb_value_write_int(&w, i_vals[i]);
            cdb_value_field(&w, 2);
            cdb_value_write_f64(&w, f_vals[i]);
            cdb_value_end_object(&w);

            pin.ts = timestamps[i];
            pin.use_clock = 0;
            pin.values = enc;
            pin.len = cdb_value_writer_len(&w);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, src_s, &pin, 1));
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Setup downsampling spec: bucket_step = 100 */
    memset(&spec, 0, sizeof spec);
    spec.src_store = store;
    spec.src_series = src_s;
    spec.dst_store = store;
    spec.dst_series = dst_s;
    spec.bucket_step = 100;
    spec.field_aggs[0] = CDB_TS_AGG_AVG;
    spec.field_aggs[1] = CDB_TS_AGG_SUM;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_downsample_run(&db, &spec, 0, 200, &gen));
    TEST_ASSERT_EQUAL_size_t(2, gen);

    /* Query downsampled series */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, dst_s, 0, 200, buf, sizeof buf, rows, 16, &n, &next));
    TEST_ASSERT_EQUAL_size_t(2, n);

    /* Point 1 at bucket ts = 0 */
    TEST_ASSERT_EQUAL_INT64(0, rows[0].ts);
    {
        cdb_value obj, v1, v2;
        int found1 = 0, found2 = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf + rows[0].off, rows[0].len, db.pager.order, &obj));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 1, &v1, &found1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 2, &v2, &found2));
        TEST_ASSERT_TRUE(found1);
        TEST_ASSERT_TRUE(found2);
        TEST_ASSERT_EQUAL_INT64(20, v1.as.i64); /* avg(10, 20, 30) = 20 */
        TEST_ASSERT_EQUAL_DOUBLE(7.5, v2.as.f64); /* sum(1.5, 2.5, 3.5) = 7.5 */
    }

    /* Point 2 at bucket ts = 100 */
    TEST_ASSERT_EQUAL_INT64(100, rows[1].ts);
    {
        cdb_value obj, v1, v2;
        int found1 = 0, found2 = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf + rows[1].off, rows[1].len, db.pager.order, &obj));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 1, &v1, &found1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 2, &v2, &found2));
        TEST_ASSERT_TRUE(found1);
        TEST_ASSERT_TRUE(found2);
        TEST_ASSERT_EQUAL_INT64(150, v1.as.i64); /* avg(100, 200) = 150 */
        TEST_ASSERT_EQUAL_DOUBLE(30.0, v2.as.f64); /* sum(10.0, 20.0) = 30.0 */
    }
}

static void test_ts_downsampling_more_aggs(void)
{
    uint64_t store, src_s, dst_s;
    int created;
    cdb_ts_def src_def, dst_def;
    cdb_ts_downsample_spec spec;
    size_t gen = 0;
    cdb_ts_in pin;
    uint8_t enc[128], buf[1024];
    cdb_value_writer w;
    cdb_ts_row rows[16];
    size_t n = 0;
    int64_t next = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));

    /* Source series: field 1 = INT, field 2 = INT, field 3 = INT, field 4 = INT */
    memset(&src_def, 0, sizeof src_def);
    src_def.precision = CDB_TS_MILLIS;
    src_def.window = 60000;
    src_def.nfields = 4;
    src_def.fields[0].id = 1;
    src_def.fields[0].type = CDB_TYPE_INT;
    src_def.fields[1].id = 2;
    src_def.fields[1].type = CDB_TYPE_INT;
    src_def.fields[2].id = 3;
    src_def.fields[2].type = CDB_TYPE_INT;
    src_def.fields[3].id = 4;
    src_def.fields[3].type = CDB_TYPE_INT;

    /* Dst series: f1 = MIN, f2 = MAX, f3 = COUNT, f4 = FIRST */
    dst_def = src_def;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "raw_aggs", 8, NULL, 0, &src_def, &src_s, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "dst_aggs", 8, NULL, 0, &dst_def, &dst_s, &created));

    /* In bucket 0..100:
     * ts=10: f1=50, f2=50, f3=50, f4=50
     * ts=20: f1=10, f2=90, f3=10, f4=90
     * ts=30: f1=80, f2=20, f3=80, f4=20
     */
    {
        int64_t ts[3] = { 10, 20, 30 };
        int64_t v[3][4] = {
            { 50, 50, 50, 50 },
            { 10, 90, 10, 90 },
            { 80, 20, 80, 20 }
        };

        for (size_t i = 0; i < 3; i++) {
            cdb_value_writer_init(&w, enc, sizeof enc, db.pager.order, CDB_DECIMAL_UNBOUNDED);
            cdb_value_begin_object(&w);
            for (uint64_t f = 1; f <= 4; f++) {
                cdb_value_field(&w, f);
                cdb_value_write_int(&w, v[i][f - 1]);
            }
            cdb_value_end_object(&w);

            pin.ts = ts[i];
            pin.use_clock = 0;
            pin.values = enc;
            pin.len = cdb_value_writer_len(&w);
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_append(&db, store, src_s, &pin, 1));
        }
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    memset(&spec, 0, sizeof spec);
    spec.src_store = store;
    spec.src_series = src_s;
    spec.dst_store = store;
    spec.dst_series = dst_s;
    spec.bucket_step = 100;
    spec.field_aggs[0] = CDB_TS_AGG_MIN;
    spec.field_aggs[1] = CDB_TS_AGG_MAX;
    spec.field_aggs[2] = CDB_TS_AGG_COUNT;
    spec.field_aggs[3] = CDB_TS_AGG_FIRST;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_downsample_run(&db, &spec, 0, 100, &gen));
    TEST_ASSERT_EQUAL_size_t(1, gen);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_range(&db, store, dst_s, 0, 100, buf, sizeof buf, rows, 16, &n, &next));
    TEST_ASSERT_EQUAL_size_t(1, n);

    {
        cdb_value obj, v1, v2, v3, v4;
        int found;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(buf + rows[0].off, rows[0].len, db.pager.order, &obj));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 1, &v1, &found));
        TEST_ASSERT_EQUAL_INT64(10, v1.as.i64); /* min(50, 10, 80) = 10 */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 2, &v2, &found));
        TEST_ASSERT_EQUAL_INT64(90, v2.as.i64); /* max(50, 90, 20) = 90 */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 3, &v3, &found));
        TEST_ASSERT_EQUAL_INT64(3, v3.as.i64);  /* count = 3 */
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, db.pager.order, 4, &v4, &found));
        TEST_ASSERT_EQUAL_INT64(50, v4.as.i64); /* first = 50 */
    }
}

static void test_ts_retention_edge_cases(void)
{
    uint64_t store, series;
    int created;
    cdb_ts_def def;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_create(&db, "m", 1, CDB_TS_LATE_ACCEPT, 0, &store));

    memset(&def, 0, sizeof def);
    def.precision = CDB_TS_MILLIS;
    def.window = 60000;
    def.nfields = 1;
    def.fields[0].id = 1;
    def.fields[0].type = CDB_TYPE_INT;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_ts_series(&db, store, "s", 1, NULL, 0, &def, &series, &created));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));

    /* Negative max_age_ticks is rejected */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_ts_set_retention(&db, store, series, -1));

    /* Non-existent series fails with CDB_E_NOT_FOUND */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_ts_set_retention(&db, store, 99999, 1000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_audit_hash_chain);
    RUN_TEST(test_audit_tamper_detection);
    RUN_TEST(test_audit_collision_logging);
    RUN_TEST(test_audit_series_protection);
    RUN_TEST(test_ts_retention);
    RUN_TEST(test_ts_retention_edge_cases);
    RUN_TEST(test_ts_downsampling);
    RUN_TEST(test_ts_downsampling_more_aggs);
    return UNITY_END();
}
