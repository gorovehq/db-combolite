/* Relational tables (spec 9): columns, defaults and nulls, constraints,
 * indexes and scans, schema changes, revision windows, and crash safety. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_faultenv.h"
#include "cdb_page.h"
#include "cdb_table.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u

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
static uint8_t idx_stage[16u * 1024u];
static uint8_t table_buffers[4u * PAGE];
static uint8_t page[PAGE];
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
    {
        const char *cap = getenv("CDB_TEST_MAX_OPEN"); /* the cap-1 runs (tests/CMakeLists.txt) */
        cfg.max_open = cap != NULL ? (unsigned)atoi(cap) : 0;
    }
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
    cfg.idx_stage = idx_stage;
    cfg.idx_stage_size = sizeof idx_stage;
    cfg.table_buffers = table_buffers;
    cfg.durability = CDB_DURABILITY_FULL;
    TEST_ASSERT_EQUAL_size_t(cdb_table_buffers_size(PAGE, 0), sizeof table_buffers);
}

void tearDown(void) {}

static cdb_status open_db(void)
{
    cdb_header h;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x7A, sizeof h.db_id);
    return cdb_db_open(&db, &env, "r", &cfg, &h);
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }

/* ------------------------------------------------------------ values */

/* An encoded object or array built field by field. */
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

static void put_null(enc *e, uint64_t f)
{
    cdb_value_field(&e->w, f);
    cdb_value_write_null(&e->w);
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

/* ------------------------------------------------------------ the table */

/* people: id int (primary key), name string not-null, score int default 7,
 * note string. */
static uint64_t T, F_ID, F_NAME, F_SCORE, F_NOTE;

static cdb_status make_table(uint32_t keep)
{
    static uint8_t seven[4];
    cdb_value_writer w;
    cdb_table_column cols[4];
    unsigned pk = 0;
    cdb_table_def def;
    cdb_status s;
    cdb_value_writer_init(&w, seven, sizeof seven, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_int(&w, 7);
    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[1].name = "name";
    cols[1].name_len = 4;
    cols[1].type = CDB_TYPE_STRING;
    cols[1].not_null = 1;
    cols[2].name = "score";
    cols[2].name_len = 5;
    cols[2].type = CDB_TYPE_INT;
    cols[2].default_value = seven;
    cols[2].default_len = cdb_value_writer_len(&w);
    cols[3].name = "note";
    cols[3].name_len = 4;
    cols[3].type = CDB_TYPE_STRING;
    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 4;
    def.primary_key = &pk;
    def.npk = 1;
    def.keep = keep;
    if ((s = cdb_table_create(&db, "people", 6, &def, &T)) != CDB_OK)
        return s;
    if ((s = cdb_table_column_id(&db, T, "id", 2, &F_ID)) != CDB_OK
        || (s = cdb_table_column_id(&db, T, "name", 4, &F_NAME)) != CDB_OK
        || (s = cdb_table_column_id(&db, T, "score", 5, &F_SCORE)) != CDB_OK)
        return s;
    return cdb_table_column_id(&db, T, "note", 4, &F_NOTE);
}

/* A row {id, name} plus score when score >= 0. */
static enc person(int64_t id, const char *name, int64_t score)
{
    enc e;
    obj_begin(&e);
    put_int(&e, F_ID, id);
    put_str(&e, F_NAME, name);
    if (score >= 0)
        put_int(&e, F_SCORE, score);
    obj_end(&e);
    return e;
}

static cdb_status insert(int64_t id, const char *name, int64_t score, uint64_t *entry)
{
    enc e = person(id, name, score);
    return cdb_table_insert(&db, T, e.b, e.n, entry);
}

/* A field of a row read back: *found 0 when it is null. */
static void field_of(const uint8_t *row, size_t len, uint64_t f, cdb_value *v, int *found)
{
    cdb_value obj;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(row, len, CDB_ORDER_LITTLE, &obj));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_object_get(&obj, CDB_ORDER_LITTLE, f, v, found));
}

static int64_t int_of(uint64_t entry, uint64_t f)
{
    uint8_t out[PAGE];
    size_t len;
    cdb_value v;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, entry, out, sizeof out, &len));
    field_of(out, len, f, &v, &found);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, v.type);
    return v.as.i64;
}

static int is_null(uint64_t entry, uint64_t f)
{
    uint8_t out[PAGE];
    size_t len;
    cdb_value v;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, entry, out, sizeof out, &len));
    field_of(out, len, f, &v, &found);
    return !found;
}

static int find(int64_t id, uint64_t *entry)
{
    enc k;
    int found;
    arr_begin(&k);
    put_int(&k, 0, id);
    arr_end(&k);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_find(&db, T, k.b, k.n, entry, &found));
    return found;
}

/* ------------------------------------------------------------ tests */

static void test_create_and_describe(void)
{
    cdb_table_column_info info[8];
    unsigned n;
    uint32_t version, keep;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, make_table(0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_describe(&db, T, info, 8, &n, &version, &keep));
    TEST_ASSERT_EQUAL_UINT(4, n);
    TEST_ASSERT_EQUAL_UINT32(1, version);
    TEST_ASSERT_EQUAL_UINT32(0, keep);
    TEST_ASSERT_EQUAL_UINT64(F_ID, info[0].field_id);
    TEST_ASSERT_TRUE(info[0].primary && info[0].not_null); /* primary keys are not-null */
    TEST_ASSERT_TRUE(info[1].not_null && !info[1].primary);
    TEST_ASSERT_TRUE(info[2].has_default && !info[2].not_null);
    /* Tables refuse the document calls. */
    {
        enc e = person(1, "x", -1);
        uint64_t entry;
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_doc_insert(&db, T, NULL, 0, e.b, e.n, &entry));
        commit();
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_rows_defaults_and_refusals(void)
{
    uint64_t a, b;
    enc e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1, "ada", -1, &a)); /* score defaults to 7 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2, "bob", 40, &b));
    /* Not-null, wrong type, unknown column, and not an object. */
    obj_begin(&e);
    put_int(&e, F_ID, 3);
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_table_insert(&db, T, e.b, e.n, &b));
    obj_begin(&e);
    put_str(&e, F_ID, "three");
    put_str(&e, F_NAME, "cy");
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_table_insert(&db, T, e.b, e.n, &b));
    obj_begin(&e);
    put_int(&e, F_ID, 3);
    put_str(&e, F_NAME, "cy");
    put_int(&e, 999, 1);
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_table_insert(&db, T, e.b, e.n, &b));
    arr_begin(&e);
    arr_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_table_insert(&db, T, e.b, e.n, &b));
    commit();
    TEST_ASSERT_EQUAL_INT64(7, int_of(a, F_SCORE));
    TEST_ASSERT_EQUAL_INT64(40, int_of(b, F_SCORE));
    TEST_ASSERT_TRUE(is_null(a, F_NOTE));
    /* A buffer too small for the row: *len is a size that will do. */
    {
        uint8_t out[PAGE];
        size_t need = 0, len = 0, len2 = 0;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, b, out, sizeof out, &len));
        TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_table_get(&db, T, b, out, 4, &need));
        TEST_ASSERT_TRUE(need >= len);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, b, out, need, &len2));
        TEST_ASSERT_EQUAL_size_t(len, len2);
    }
    /* update changes only the columns named; a null clears one. */
    begin();
    obj_begin(&e);
    put_null(&e, F_SCORE); /* fields in increasing order */
    put_str(&e, F_NOTE, "hello");
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, a, e.b, e.n));
    commit();
    TEST_ASSERT_TRUE(is_null(a, F_SCORE));
    TEST_ASSERT_FALSE(is_null(a, F_NOTE));
    TEST_ASSERT_EQUAL_INT64(1, int_of(a, F_ID));
    /* replace without score: null (defaults are for inserts). */
    begin();
    e = person(1, "ada", -1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&db, T, a, e.b, e.n));
    commit();
    TEST_ASSERT_TRUE(is_null(a, F_SCORE));
    TEST_ASSERT_TRUE(is_null(a, F_NOTE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_primary_key(void)
{
    uint64_t a, b, c, got;
    enc e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1, "ada", -1, &a));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, insert(1, "dup", -1, &b)); /* in this transaction */
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2, "bob", -1, &b));
    commit();
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, insert(2, "dup", -1, &c)); /* committed */
    /* Moving a key: delete the holder, then reuse it, in one transaction. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, b));
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2, "bee", -1, &c));
    /* Changing a row's own key to one that is taken, and to a free one. */
    e = person(1, "ada", -1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&db, T, a, e.b, e.n)); /* its own key */
    e = person(2, "ada", -1);
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_replace(&db, T, a, e.b, e.n));
    e = person(10, "ada", -1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_replace(&db, T, a, e.b, e.n));
    commit();
    TEST_ASSERT_TRUE(find(10, &got));
    TEST_ASSERT_EQUAL_UINT64(a, got);
    TEST_ASSERT_FALSE(find(1, &got));
    TEST_ASSERT_TRUE(find(2, &got));
    TEST_ASSERT_EQUAL_UINT64(c, got);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    /* The index survives a reopen. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_TRUE(find(10, &got));
    TEST_ASSERT_EQUAL_UINT64(a, got);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Scans in entry order and over an index, with paging and bounds. */
static void test_scans(void)
{
    uint64_t e[20], got[8];
    cdb_table_cursor cur;
    size_t n, total;
    enc lo, hi;
    unsigned i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    begin();
    for (i = 0; i < 20; i++) {
        char name[8];
        snprintf(name, sizeof name, "p%02u", i);
        TEST_ASSERT_EQUAL_INT(CDB_OK, insert(100 - (int64_t)i, name, -1, &e[i])); /* keys descend */
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, e[5]));
    commit();
    /* Entry order, three pages of up to 8. */
    memset(&cur, 0, sizeof cur);
    total = 0;
    do {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, got, 8, &n));
        for (i = 0; i < n; i++)
            TEST_ASSERT_TRUE(got[i] != e[5]);
        total += n;
    } while (n > 0);
    TEST_ASSERT_EQUAL_size_t(19, total);
    /* Primary key 85..90 inclusive, in key order. */
    arr_begin(&lo);
    put_int(&lo, 0, 85);
    arr_end(&lo);
    arr_begin(&hi);
    put_int(&hi, 0, 90);
    arr_end(&hi);
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, 0, lo.b, lo.n, hi.b, hi.n, &cur, got, 3, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT64(e[15], got[0]); /* key 85 */
    TEST_ASSERT_EQUAL_UINT64(e[14], got[1]);
    TEST_ASSERT_EQUAL_UINT64(e[13], got[2]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, 0, lo.b, lo.n, hi.b, hi.n, &cur, got, 8, &n));
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT64(e[10], got[2]); /* key 90 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, 0, lo.b, lo.n, hi.b, hi.n, &cur, got, 8, &n));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* All four schema changes, read through rows written before them. */
static void test_schema_changes(void)
{
    static uint8_t dflt[8];
    cdb_value_writer w;
    cdb_table_column c;
    uint64_t old, young, f_tag;
    uint8_t out[PAGE];
    size_t len;
    cdb_value v;
    int found;
    enc e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1, "ada", 5, &old));
    commit();
    /* Add a column with a default: the old row reads it as the default. */
    cdb_value_writer_init(&w, dflt, sizeof dflt, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_string(&w, "none", 4);
    memset(&c, 0, sizeof c);
    c.name = "tag";
    c.name_len = 3;
    c.type = CDB_TYPE_STRING;
    c.not_null = 1;
    c.default_value = dflt;
    c.default_len = cdb_value_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_add_column(&db, T, &c, &f_tag));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_add_column(&db, T, &c, &f_tag));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, old, out, sizeof out, &len));
    field_of(out, len, f_tag, &v, &found);
    TEST_ASSERT_TRUE(found && v.type == CDB_TYPE_STRING && v.payload_len == 4);
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2, "bob", 9, &young)); /* gets the default too */
    commit();
    /* Widen score to decimal: both rows convert. */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_table_widen_column(&db, T, F_NAME, CDB_TYPE_INT));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_table_widen_column(&db, T, F_SCORE, CDB_TYPE_DECIMAL)); /* it has a default */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_drop_column(&db, T, F_NOTE));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_drop_column(&db, T, F_NOTE));
    /* Rename keeps the ID; the old name is free. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_rename_column(&db, T, F_NAME, "full_name", 9));
    {
        uint64_t id;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "full_name", 9, &id));
        TEST_ASSERT_EQUAL_UINT64(F_NAME, id);
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_column_id(&db, T, "name", 4, &id));
    }
    /* The dropped column is refused in new rows, hidden in old ones. */
    begin();
    obj_begin(&e);
    put_str(&e, F_NOTE, "x");
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_table_update(&db, T, old, e.b, e.n));
    commit();
    TEST_ASSERT_EQUAL_INT64(5, int_of(old, F_SCORE));
    TEST_ASSERT_TRUE(is_null(old, F_NOTE));
    /* The primary key's column cannot be dropped while indexed. */
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_table_drop_column(&db, T, F_ID));
    /* Widening int to decimal: an old row's int reads as a decimal. */
    {
        cdb_table_column pc;
        uint64_t f_pts;
        memset(&pc, 0, sizeof pc);
        pc.name = "points";
        pc.name_len = 6;
        pc.type = CDB_TYPE_INT;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_add_column(&db, T, &pc, &f_pts));
        begin();
        obj_begin(&e);
        put_int(&e, f_pts, 1234);
        obj_end(&e);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, old, e.b, e.n));
        commit();
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_widen_column(&db, T, f_pts, CDB_TYPE_DECIMAL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, old, out, sizeof out, &len));
        field_of(out, len, f_pts, &v, &found);
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_DECIMAL, v.type);
        TEST_ASSERT_EQUAL_UINT64(0, v.as.decimal.scale);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT64(9, int_of(young, F_SCORE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A secondary unique index over existing rows; nulls never collide. */
static void test_secondary_index(void)
{
    uint64_t a, b, c, d, got[8];
    uint32_t ix, ix2;
    cdb_table_cursor cur;
    size_t n;
    enc e, k;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1, "ada", 3, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2, "bob", 3, &b)); /* same score */
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(3, "cy", 4, &c));
    commit();
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_create_index(&db, T, &F_SCORE, 1, 1, &ix)); /* not unique */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create_index(&db, T, &F_SCORE, 1, 0, &ix));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create_index(&db, T, &F_NOTE, 1, 1, &ix2));
    TEST_ASSERT_TRUE(ix != 0 && ix2 != ix);
    /* score = 3 finds both rows. */
    arr_begin(&k);
    put_int(&k, 0, 3);
    arr_end(&k);
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, ix, k.b, k.n, k.b, k.n, &cur, got, 8, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    /* note is unique, but null twice is fine. */
    begin();
    obj_begin(&e);
    put_str(&e, F_NOTE, "n1");
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, a, e.b, e.n));
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_table_update(&db, T, b, e.b, e.n));
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(4, "dee", -1, &d)); /* note null, like b and c */
    commit();
    /* The index follows a change: score 3 -> 8 for a. */
    begin();
    obj_begin(&e);
    put_int(&e, F_SCORE, 8);
    obj_end(&e);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, a, e.b, e.n));
    commit();
    memset(&cur, 0, sizeof cur);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, ix, k.b, k.n, k.b, k.n, &cur, got, 8, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT64(b, got[0]);
    TEST_ASSERT_EQUAL_INT(CDB_E_STATE, cdb_table_drop_column(&db, T, F_SCORE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_drop_index(&db, T, ix));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_drop_column(&db, T, F_SCORE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Large batch of rows in one transaction, verifying O(1) staging lookups
 * and duplicate detection within uncommitted staging records. */
static void test_staging_unique_batch(void)
{
    uint64_t e, entries[200], got[8];
    cdb_table_cursor cur;
    size_t n;
    enc e1;
    int i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));

    begin();
    for (i = 0; i < 200; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1000 + i, "batch", i, &entries[i]));
    }
    /* Duplicate key within the open transaction's staged rows is rejected. */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, insert(1050, "dupe", 999, &e));

    /* Update a staged row within the open transaction: triggers stage_reindex. */
    obj_begin(&e1);
    put_str(&e1, F_NAME, "updated");
    obj_end(&e1);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, entries[50], e1.b, e1.n));

    /* Staged row with unchanged unique key still rejects duplicates. */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, insert(1050, "dupe2", 999, &e));

    /* Delete a staged row within the open transaction. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, entries[100]));

    /* Deleted entry's key can now be re-inserted in the same transaction. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1100, "revived", 42, &e));

    commit();

    /* Verify committed state */
    /* Scan by PK 1050 (entries[50]) */
    {
        enc k;
        arr_begin(&k);
        put_int(&k, 0, 1050);
        arr_end(&k);
        memset(&cur, 0, sizeof cur);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_scan(&db, T, 0, k.b, k.n, k.b, k.n, &cur, got, 8, &n));
        TEST_ASSERT_EQUAL_size_t(1, n);
        TEST_ASSERT_EQUAL_UINT64(entries[50], got[0]);
    }
    /* Entry 100 was deleted, but key 1100 was re-inserted as e */
    {
        uint8_t row[256];
        size_t rlen;
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, T, entries[100], row, sizeof row, &rlen));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, e, row, sizeof row, &rlen));
    }

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Verifies that when idx_stage buffer is exhausted or absent, unique checks
 * gracefully fall back to doc_stage and journal scanning. */
static void test_staging_unique_overflow(void)
{
    uint64_t e, entries[20];
    int i;

    /* Test with tiny idx_stage forcing overflow */
    cfg.idx_stage_size = 32;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(0));

    begin();
    for (i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, insert(2000 + i, "overflow", i, &entries[i]));
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, insert(2005, "dupe", 999, &e));
    commit();

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* With keep 1, a row updated many times keeps one revision, so its pages
 * stay few; a delete removes the row. With history on they grow. */
static unsigned live_pages(void)
{
    uint64_t lid;
    unsigned n = 0;
    for (lid = 1; lid < db.pager.next_lid; lid++) {
        cdb_page_header h;
        if (cdb_pager_read(&db.pager, lid, page, &h) == CDB_OK)
            n++;
    }
    return n;
}

static unsigned churn(uint32_t keep)
{
    uint64_t a, got;
    unsigned i, pages;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table(keep));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, insert(1, "ada", 0, &a));
    commit();
    for (i = 1; i <= 120; i++) {
        enc e;
        obj_begin(&e);
        put_int(&e, F_SCORE, (int64_t)i);
        obj_end(&e);
        begin();
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_update(&db, T, a, e.b, e.n));
        commit();
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    pages = live_pages();
    TEST_ASSERT_EQUAL_INT64(120, int_of(a, F_SCORE));
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, a));
    commit();
    {
        uint8_t out[PAGE];
        size_t len;
        TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_get(&db, T, a, out, sizeof out, &len));
    }
    TEST_ASSERT_FALSE(find(1, &got));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    return pages;
}

static void test_revision_window(void)
{
    unsigned kept = churn(1), all;
    init_env();
    all = churn(0);
    TEST_ASSERT_TRUE(kept + 3u < all);
}

/* ------------------------------------------------------- crash sweep */

/* Steps: S1 three rows; S2 one updated, one deleted; S3 an index on score.
 * After a crash the table matches the last acknowledged step, and every
 * index agrees with the rows. */
static cdb_status workload(int *acked)
{
    uint64_t a, b, c;
    uint32_t ix;
    enc e;
    cdb_status s;
    *acked = 0;
#define TRY(call) do { if ((s = (call)) != CDB_OK) return s; } while (0)
    TRY(open_db());
    TRY(make_table(0));
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TRY(insert(1, "ada", 5, &a));
    TRY(insert(2, "bob", 6, &b));
    TRY(insert(3, "cy", 7, &c));
    TRY(cdb_tx_commit(&db));
    *acked = 1;
    TRY(cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    e = person(20, "ada", 9);
    TRY(cdb_table_replace(&db, T, a, e.b, e.n));
    TRY(cdb_table_delete(&db, T, b));
    TRY(cdb_tx_commit(&db));
    *acked = 2;
    TRY(cdb_table_create_index(&db, T, &F_SCORE, 1, 0, &ix));
    *acked = 3;
    TRY(cdb_db_close(&db));
#undef TRY
    return CDB_OK;
}

/* The rows by primary key, and the score index's entries (if it exists). */
static void check_state(int acked)
{
    uint64_t got, entries[8];
    cdb_table_cursor cur;
    size_t n;
    int have1 = find(1, &got), have2 = find(2, &got), have20 = find(20, &got), have3 = find(3, &got);
    if (acked >= 2) {
        TEST_ASSERT_TRUE(!have1 && !have2 && have20 && have3);
    } else if (acked == 1) {
        TEST_ASSERT_TRUE(have3);
        TEST_ASSERT_TRUE((have1 && have2 && !have20) || (!have1 && !have2 && have20));
    }
    /* Every row the primary key finds reads back with that key. */
    if (have20) {
        TEST_ASSERT_TRUE(find(20, &got));
        TEST_ASSERT_EQUAL_INT64(20, int_of(got, F_ID));
    }
    /* The index on score, when it exists, holds exactly the live rows. */
    memset(&cur, 0, sizeof cur);
    if (cdb_table_scan(&db, T, 1, NULL, 0, NULL, 0, &cur, entries, 8, &n) == CDB_OK) {
        size_t live = (size_t)(have1 + have2 + have20 + have3), i;
        TEST_ASSERT_EQUAL_size_t(live, n);
        for (i = 0; i < n; i++) {
            uint8_t out[PAGE];
            size_t len;
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_get(&db, T, entries[i], out, sizeof out, &len));
        }
    } else {
        TEST_ASSERT_TRUE(acked < 3);
    }
}

static void test_crash_sweep(void)
{
    const cdb_crash_mode modes[4] = { CDB_CRASH_DROP_UNSYNCED, CDB_CRASH_KEEP_PREFIX, CDB_CRASH_TORN,
                                      CDB_CRASH_REORDER };
    uint64_t total, op;
    int acked, m;
    init_env();
    TEST_ASSERT_EQUAL_INT(CDB_OK, workload(&acked));
    total = fe.clock.ops;
    {
        char msg[64];
        snprintf(msg, sizeof msg, "crash points swept: %u", (unsigned)total);
        TEST_MESSAGE(msg);
    }
    for (m = 0; m < 4; m++)
        for (op = 1; op <= total; op++) {
            init_env();
            cdb_faultenv_arm(&fe, op, modes[m], 1, 300);
            (void)workload(&acked);
            cdb_faultenv_reboot(&fe);
            if (open_db() != CDB_OK) {
                TEST_ASSERT_EQUAL_INT(0, acked);
                continue;
            }
            {
                int found;
                /* A crash before the table was fully created leaves none, or
                 * one without columns that refuses rows. */
                if (cdb_catalog_find_store(&db.catalog, "people", 6, &T, &found) == CDB_OK && found) {
                    uint32_t version;
                    unsigned n;
                    cdb_table_column_info info[8];
                    if (cdb_table_describe(&db, T, info, 8, &n, &version, NULL) == CDB_OK && version == 1) {
                        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "id", 2, &F_ID));
                        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "score", 5, &F_SCORE));
                        check_state(acked);
                    } else {
                        TEST_ASSERT_EQUAL_INT(0, acked);
                    }
                } else {
                    TEST_ASSERT_EQUAL_INT(0, acked);
                }
            }
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
        }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_and_describe);
    RUN_TEST(test_rows_defaults_and_refusals);
    RUN_TEST(test_primary_key);
    RUN_TEST(test_scans);
    RUN_TEST(test_schema_changes);
    RUN_TEST(test_secondary_index);
    RUN_TEST(test_staging_unique_batch);
    RUN_TEST(test_staging_unique_overflow);
    RUN_TEST(test_revision_window);
    RUN_TEST(test_crash_sweep);
    return UNITY_END();
}
