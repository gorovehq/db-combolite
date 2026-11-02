/* Column-oriented tables: write path and pending rows index (M18 Layer 2).
 *
 * Covers: table creation, inserts into pending index, row retrieval via
 * cdb_table_get, primary key lookup via cdb_table_find, unique constraint
 * collision rejection, partial update, full replace, deletion with tombstone,
 * chunk_cap overflow rejection, transaction rollback, and recovery replay. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_endian.h"
#include "cdb_faultenv.h"
#include "cdb_key.h"
#include "cdb_page.h"
#include "cdb_rowgroup.h"
#include "cdb_table.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
#define ROWS 64u
#define CAP 4096u
#define ORDER CDB_ORDER_LITTLE

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static uint8_t record_buf[2u * PAGE];
static uint8_t cat_work[24u * 1024u];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t doc_stage[4096];
static uint8_t table_buffers[4u * PAGE];
static uint8_t col_work[4u * 1024u * 1024u];
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
    cfg.doc_stage = doc_stage;
    cfg.doc_stage_size = sizeof doc_stage;
    cfg.table_buffers = table_buffers;
    cfg.col_work = col_work;
    cfg.col_work_size = sizeof col_work;
    cfg.durability = CDB_DURABILITY_FULL;
    TEST_ASSERT_TRUE(cdb_rg_work_size(PAGE, ROWS, CAP, 1) <= sizeof col_work);
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = ORDER;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x7A, sizeof h.db_id);
    return cdb_db_open(&db, &env, "c", &cfg, &h);
}

static void reopen(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }
static void abort_tx(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_abort(&db)); }

/* ------------------------------------------------------------ value helpers */

typedef struct enc {
    uint8_t b[512];
    size_t n;
    cdb_value_writer w;
} enc;

static void obj_begin(enc *e)
{
    cdb_value_writer_init(&e->w, e->b, sizeof e->b, ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&e->w);
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

static void put_bool(enc *e, uint64_t f, int b)
{
    if (f)
        cdb_value_field(&e->w, f);
    cdb_value_write_bool(&e->w, b);
}

static void put_f64(enc *e, uint64_t f, double v)
{
    if (f)
        cdb_value_field(&e->w, f);
    cdb_value_write_f64(&e->w, v);
}

static void obj_end(enc *e)
{
    cdb_value_end_object(&e->w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, e->w.error);
    e->n = cdb_value_writer_len(&e->w);
}

static void arr_begin(enc *e)
{
    cdb_value_writer_init(&e->w, e->b, sizeof e->b, ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&e->w);
}

static void arr_end(enc *e)
{
    cdb_value_end_array(&e->w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, e->w.error);
    e->n = cdb_value_writer_len(&e->w);
}

/* ------------------------------------------------------------ test table */

/* metrics: id int PK, name string, active bool, score f64 */
static uint64_t T, F_ID, F_NAME, F_ACTIVE, F_SCORE;

static void make_row(enc *e, int64_t id, const char *name, int active, double score)
{
    obj_begin(e);
    put_int(e, F_ID, id);
    put_str(e, F_NAME, name);
    put_bool(e, F_ACTIVE, active);
    put_f64(e, F_SCORE, score);
    obj_end(e);
}

static void make_coltable(void)
{
    cdb_table_column cols[4];
    unsigned pk[1];
    cdb_table_def def;

    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[0].not_null = 1;

    cols[1].name = "name";
    cols[1].name_len = 4;
    cols[1].type = CDB_TYPE_STRING;

    cols[2].name = "active";
    cols[2].name_len = 6;
    cols[2].type = CDB_TYPE_BOOL;

    cols[3].name = "score";
    cols[3].name_len = 5;
    cols[3].type = CDB_TYPE_F64;

    pk[0] = 0; /* cols[0] is primary key */

    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 4;
    def.primary_key = pk;
    def.npk = 1;
    def.layout = CDB_TABLE_LAYOUT_COLUMN;
    def.group_rows = ROWS;
    def.chunk_cap = CAP;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "metrics", 7, &def, &T));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "id", 2, &F_ID));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "name", 4, &F_NAME));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "active", 6, &F_ACTIVE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "score", 5, &F_SCORE));
}

/* ------------------------------------------------------------ tests */

static void test_coltable_insert_and_get(void)
{
    uint64_t e1, e2;
    enc row;
    uint8_t out[256];
    size_t out_len;
    cdb_value v, fld;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 1001);
    put_str(&row, F_NAME, "alpha");
    put_bool(&row, F_ACTIVE, 1);
    put_f64(&row, F_SCORE, 98.5);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    TEST_ASSERT_TRUE(e1 > 0);

    obj_begin(&row);
    put_int(&row, F_ID, 1002);
    put_str(&row, F_NAME, "beta");
    put_bool(&row, F_ACTIVE, 0);
    put_f64(&row, F_SCORE, 72.0);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e2));
    TEST_ASSERT_TRUE(e2 > e1);
    commit();

    /* Read back committed rows from pending index */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_OBJECT, v.type);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(1001, fld.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_NAME, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING_LEN("alpha", fld.payload, fld.payload_len);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e2, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(1002, fld.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_primary_key_find(void)
{
    uint64_t e1, found_entry;
    enc row, pk_arr;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 42);
    put_str(&row, F_NAME, "answer");
    put_bool(&row, F_ACTIVE, 1);
    put_f64(&row, F_SCORE, 100.0);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    commit();

    /* Find by PK [42] */
    arr_begin(&pk_arr);
    put_int(&pk_arr, 0, 42);
    arr_end(&pk_arr);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, pk_arr.b, pk_arr.n, &found_entry, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(e1, found_entry);

    /* Find non-existent PK [999] */
    arr_begin(&pk_arr);
    put_int(&pk_arr, 0, 999);
    arr_end(&pk_arr);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, pk_arr.b, pk_arr.n, &found_entry, &found));
    TEST_ASSERT_FALSE(found);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_unique_collision(void)
{
    uint64_t e1, e2;
    enc row;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 500);
    put_str(&row, F_NAME, "first");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));

    /* Duplicate PK in same transaction */
    obj_begin(&row);
    put_int(&row, F_ID, 500);
    put_str(&row, F_NAME, "clash");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_insert(&db, T, row.b, row.n, &e2));
    commit();

    /* Duplicate PK in subsequent transaction */
    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 500);
    put_str(&row, F_NAME, "clash2");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_insert(&db, T, row.b, row.n, &e2));
    abort_tx();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_update_and_replace(void)
{
    uint64_t e1;
    enc row;
    uint8_t out[256];
    size_t out_len;
    cdb_value v, fld;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 200);
    put_str(&row, F_NAME, "original");
    put_bool(&row, F_ACTIVE, 1);
    put_f64(&row, F_SCORE, 50.0);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    commit();

    /* Partial update: update score only */
    begin();
    obj_begin(&row);
    put_f64(&row, F_SCORE, 95.5);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, e1, row.b, row.n));
    commit();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_SCORE, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_DOUBLE(95.5, fld.as.f64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_NAME, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING_LEN("original", fld.payload, fld.payload_len);

    /* Full replace: replaces all fields */
    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 200);
    put_str(&row, F_NAME, "replaced");
    put_bool(&row, F_ACTIVE, 0);
    put_f64(&row, F_SCORE, 10.0);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&db, T, e1, row.b, row.n));
    commit();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_NAME, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING_LEN("replaced", fld.payload, fld.payload_len);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ACTIVE, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(0, fld.as.boolean);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_delete(void)
{
    uint64_t e1, found_entry;
    enc row, pk_arr;
    uint8_t out[256];
    size_t out_len;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 777);
    put_str(&row, F_NAME, "to_delete");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    commit();

    /* Delete the row */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, e1));
    commit();

    /* Getting the deleted row returns NOT_FOUND */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));

    /* Finding by PK returns not found */
    arr_begin(&pk_arr);
    put_int(&pk_arr, 0, 777);
    arr_end(&pk_arr);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, pk_arr.b, pk_arr.n, &found_entry, &found));
    TEST_ASSERT_FALSE(found);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_chunk_cap_overflow(void)
{
    uint64_t e1;
    char big_string[5000];
    uint8_t buf_row[6000];
    cdb_value_writer w;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    memset(big_string, 'X', sizeof big_string);
    cdb_value_writer_init(&w, buf_row, sizeof buf_row, ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, F_ID);
    cdb_value_write_int(&w, 9999);
    cdb_value_field(&w, F_NAME);
    cdb_value_write_string(&w, big_string, sizeof big_string);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);

    begin();
    /* CAP is 4096; row > 5000 bytes must be rejected with OVERFLOW */
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, cdb_table_insert(&db, T, buf_row, cdb_value_writer_len(&w), &e1));
    abort_tx();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_transaction_abort(void)
{
    uint64_t e1, found_entry;
    enc row, pk_arr;
    uint8_t out[256];
    size_t out_len;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 303);
    put_str(&row, F_NAME, "aborted");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    abort_tx();

    /* Row must not exist */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));

    arr_begin(&pk_arr);
    put_int(&pk_arr, 0, 303);
    arr_end(&pk_arr);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, pk_arr.b, pk_arr.n, &found_entry, &found));
    TEST_ASSERT_FALSE(found);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_recovery(void)
{
    uint64_t e1;
    enc row, pk_arr;
    uint8_t out[256];
    size_t out_len;
    cdb_value v, fld;
    uint64_t found_entry;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    obj_begin(&row);
    put_int(&row, F_ID, 888);
    put_str(&row, F_NAME, "persistent");
    put_bool(&row, F_ACTIVE, 1);
    put_f64(&row, F_SCORE, 3.14159);
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    commit();

    /* Reopen database to trigger recovery replay */
    reopen();

    /* Verify row survives recovery */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(888, fld.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_NAME, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING_LEN("persistent", fld.payload, fld.payload_len);

    /* Verify primary key index survives recovery */
    arr_begin(&pk_arr);
    put_int(&pk_arr, 0, 888);
    arr_end(&pk_arr);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, pk_arr.b, pk_arr.n, &found_entry, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(e1, found_entry);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_reconciliation_fold(void)
{
    uint64_t e1, e2, e3;
    enc row;
    uint8_t out[256];
    size_t out_len;
    cdb_value v, fld;
    int found;
    cdb_rg g;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    make_row(&row, 101, "item1", 1, 10.5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    make_row(&row, 102, "item2", 0, 20.5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e2));
    make_row(&row, 103, "item3", 1, 30.5);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e3));
    commit();

    /* Initialize rowgroup reader: before flush, t->groups has no row groups */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_rowgroups(&db, T, &g));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_find(&g, e1, &found));
    TEST_ASSERT_EQUAL_INT(0, found);

    /* Flush / reconcile pending rows into row groups */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, T));

    /* After fold, t->groups now has the row group */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_find(&g, e1, &found));
    TEST_ASSERT_EQUAL_INT(1, found);
    TEST_ASSERT_EQUAL_UINT64(e1, g.hdr->first);
    TEST_ASSERT_EQUAL_UINT64(e3, g.hdr->last);
    TEST_ASSERT_EQUAL_UINT32(3, g.hdr->rows);

    /* Verify all rows can still be read back */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(101, fld.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e2, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(102, fld.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e3, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(103, fld.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_group_split(void)
{
    cdb_table_column cols[2];
    cdb_table_def def;
    uint64_t store;
    enc row;
    uint32_t i;
    uint64_t entries[40];
    cdb_rg g;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    /* Create table with small group_rows = 16 to trigger split */
    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[0].not_null = 1;
    cols[1].name = "val";
    cols[1].name_len = 3;
    cols[1].type = CDB_TYPE_INT;

    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 2;
    def.layout = CDB_TABLE_LAYOUT_COLUMN;
    def.group_rows = 16;
    def.chunk_cap = CAP;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "split_tab", 9, &def, &store));

    begin();
    for (i = 0; i < 40; i++) {
        obj_begin(&row);
        put_int(&row, 1, (int64_t)(i + 1));
        put_int(&row, 2, (int64_t)((i + 1) * 10));
        obj_end(&row);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, store, row.b, row.n, &entries[i]));
    }
    commit();

    /* Fold pending rows */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, store));

    /* Verify row groups in t->groups: 40 rows with group_rows = 16 should be split into 3 groups (16 + 16 + 8) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_rowgroups(&db, store, &g));

    /* Group 1: entries 0..15 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_find(&g, entries[0], &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(entries[0], g.hdr->first);
    TEST_ASSERT_EQUAL_UINT64(entries[15], g.hdr->last);
    TEST_ASSERT_EQUAL_UINT32(16, g.hdr->rows);

    /* Group 2: entries 16..31 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_find(&g, entries[16], &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(entries[16], g.hdr->first);
    TEST_ASSERT_EQUAL_UINT64(entries[31], g.hdr->last);
    TEST_ASSERT_EQUAL_UINT32(16, g.hdr->rows);

    /* Group 3: entries 32..39 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_find(&g, entries[32], &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(entries[32], g.hdr->first);
    TEST_ASSERT_EQUAL_UINT64(entries[39], g.hdr->last);
    TEST_ASSERT_EQUAL_UINT32(8, g.hdr->rows);

    /* Verify all 40 rows are retrievable */
    for (i = 0; i < 40; i++) {
        uint8_t out[128];
        size_t out_len;
        cdb_value v, fld;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, store, entries[i], out, sizeof out, &out_len));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, 1, &fld, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_INT64(i + 1, fld.as.i64);
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_update_delete_in_group(void)
{
    uint64_t e1, e2, e3;
    enc row;
    uint8_t out[256];
    size_t out_len;
    cdb_value v, fld;
    int found;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    make_row(&row, 201, "first", 1, 1.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    make_row(&row, 202, "second", 0, 2.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e2));
    make_row(&row, 203, "third", 1, 3.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e3));
    commit();

    /* Fold into row group */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, T));

    /* Now update e1 and delete e2 */
    begin();
    obj_begin(&row);
    put_str(&row, F_NAME, "first-updated");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, e1, row.b, row.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, e2));
    commit();

    /* Fold changes back into row group */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, T));

    /* Check e1 has updated name and original id */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e1, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(201, fld.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_NAME, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING_LEN("first-updated", fld.payload, fld.payload_len);

    /* Check e2 is deleted */
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, T, e2, out, sizeof out, &out_len));

    /* Check e3 is untouched */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e3, out, sizeof out, &out_len));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(out, out_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&v, ORDER, F_ID, &fld, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT64(203, fld.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_scan_entry_order(void)
{
    uint64_t e[5], scanned[10], e5;
    enc row;
    cdb_table_cursor cur;
    size_t n;
    int i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    for (i = 0; i < 5; i++) {
        make_row(&row, 300 + i, "item", 1, (double)i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e[i]));
    }
    commit();

    /* 1. Scan before fold (all rows pending) */
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(5, n);
    for (i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_UINT64(e[i], scanned[i]);

    /* 2. Fold into row groups */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, T));

    /* 3. Scan after fold (all rows in group) */
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(5, n);
    for (i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_UINT64(e[i], scanned[i]);

    /* 4. Partial scan: 2 rows then 3 rows with cursor */
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, scanned, 2, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(e[0], scanned[0]);
    TEST_ASSERT_EQUAL_UINT64(e[1], scanned[1]);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT64(e[2], scanned[0]);
    TEST_ASSERT_EQUAL_UINT64(e[3], scanned[1]);
    TEST_ASSERT_EQUAL_UINT64(e[4], scanned[2]);

    /* 5. Delete e[1] and add e[5], scan without folding (mixed group + pending + tombstone) */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, e[1]));
    make_row(&row, 305, "item5", 1, 5.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e5));
    commit();

    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(5, n);
    TEST_ASSERT_EQUAL_UINT64(e[0], scanned[0]);
    TEST_ASSERT_EQUAL_UINT64(e[2], scanned[1]);
    TEST_ASSERT_EQUAL_UINT64(e[3], scanned[2]);
    TEST_ASSERT_EQUAL_UINT64(e[4], scanned[3]);
    TEST_ASSERT_EQUAL_UINT64(e5, scanned[4]);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_scan_index(void)
{
    uint64_t e[4], scanned[10];
    enc row, lo, hi;
    cdb_table_cursor cur;
    size_t n;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    make_row(&row, 10, "ten", 1, 10.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e[0]));
    make_row(&row, 20, "twenty", 1, 20.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e[1]));
    make_row(&row, 30, "thirty", 1, 30.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e[2]));
    make_row(&row, 40, "forty", 1, 40.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e[3]));
    commit();

    /* Scan index 0 (primary key) from 20 to 30 */
    arr_begin(&lo);
    put_int(&lo, 0, 20);
    arr_end(&lo);

    arr_begin(&hi);
    put_int(&hi, 0, 30);
    arr_end(&hi);

    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, 0, lo.b, lo.n, hi.b, hi.n, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(e[1], scanned[0]);
    TEST_ASSERT_EQUAL_UINT64(e[2], scanned[1]);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_create_secondary_index_populated(void)
{
    uint64_t e1, e2, e3, sec_idx;
    enc row;
    cdb_table_cursor cur;
    uint64_t scanned[10];
    size_t n;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_coltable();

    begin();
    make_row(&row, 1, "cherry", 1, 3.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e1));
    make_row(&row, 2, "apple", 1, 1.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e2));
    make_row(&row, 3, "banana", 1, 2.0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, row.b, row.n, &e3));
    commit();

    /* Fold rows into groups */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_col_fold_pending(&db, T));

    /* Create secondary index on name (F_NAME) over populated table */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create_index(&db, T, &F_NAME, 1, 1, (uint32_t *)&sec_idx));

    /* Scan index on name (should be apple -> banana -> cherry) */
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, (uint32_t)sec_idx, NULL, 0, NULL, 0, &cur, scanned, 10, &n));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT64(e2, scanned[0]); /* apple */
    TEST_ASSERT_EQUAL_UINT64(e3, scanned[1]); /* banana */
    TEST_ASSERT_EQUAL_UINT64(e1, scanned[2]); /* cherry */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_coltable_history_side_store(void)
{
    cdb_table_column cols[2];
    cdb_table_def def;
    uint64_t store, e1;
    enc row;
    uint8_t out[256];
    size_t out_len;
    cdb_doc_info info;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());

    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[0].not_null = 1;
    cols[1].name = "msg";
    cols[1].name_len = 3;
    cols[1].type = CDB_TYPE_STRING;

    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 2;
    def.layout = CDB_TABLE_LAYOUT_COLUMN;
    def.keep = 0; /* history on! (spec 9.6, 14.10.2) */
    def.group_rows = ROWS;
    def.chunk_cap = CAP;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "hist_tab", 8, &def, &store));

    /* Insert row revision 1 */
    begin();
    obj_begin(&row);
    put_int(&row, 1, 501);
    put_str(&row, 2, "rev1");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, store, row.b, row.n, &e1));
    commit();

    /* Update row revision 2 */
    begin();
    obj_begin(&row);
    put_str(&row, 2, "rev2");
    obj_end(&row);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, store, e1, row.b, row.n));
    commit();

    /* Verify historical side store kept both revisions */
    db.via_table = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, store, e1, 1, out, sizeof out, &out_len, NULL, 0, NULL, &info));
    TEST_ASSERT_EQUAL_UINT64(1, info.revision);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_get(&db, store, e1, 2, out, sizeof out, &out_len, NULL, 0, NULL, &info));
    TEST_ASSERT_EQUAL_UINT64(2, info.revision);
    db.via_table = 0;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_coltable_insert_and_get);
    RUN_TEST(test_coltable_primary_key_find);
    RUN_TEST(test_coltable_unique_collision);
    RUN_TEST(test_coltable_update_and_replace);
    RUN_TEST(test_coltable_delete);
    RUN_TEST(test_coltable_chunk_cap_overflow);
    RUN_TEST(test_coltable_transaction_abort);
    RUN_TEST(test_coltable_recovery);
    RUN_TEST(test_coltable_reconciliation_fold);
    RUN_TEST(test_coltable_group_split);
    RUN_TEST(test_coltable_update_delete_in_group);
    RUN_TEST(test_coltable_scan_entry_order);
    RUN_TEST(test_coltable_scan_index);
    RUN_TEST(test_coltable_create_secondary_index_populated);
    RUN_TEST(test_coltable_history_side_store);
    return UNITY_END();
}
