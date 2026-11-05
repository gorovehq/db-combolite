/* The executor against a real database (spec 11.10): Scan reads table rows
 * and documents, so Filter, Project, Sort and Aggregate see their fields.
 * test_exec.c covers the operators on mock rows. */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_compiler.h"
#include "cdb_doc.h"
#include "cdb_exec.h"
#include "cdb_faultenv.h"
#include "cdb_table.h"

#define FCAP (1024u * 1024u)
#define PAGE_LOG2 10u
#define PAGE (1u << PAGE_LOG2)
#define NODES 16u
/* Bodies up to 12 KB, past the 4-page (4 KB) record buffer a scan starts
 * with, so a large one shows the profiles apart (spec 17.3) */
#define DOC_MAX 12288u
#define DBUF (DOC_MAX + PAGE)

static uint8_t env_arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record env_records[CDB_FAULTENV_MAX_FILES * 8192u];
static cdb_faultenv fe;
static cdb_env env;

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[4096];
static uint8_t record_buf[DOC_MAX + 2u * PAGE];
static uint8_t cat_work[24u * 1024u];
static cdb_index_entry cat_entries[512];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[3u * DBUF + 5u * PAGE];
static uint8_t doc_stage[4096];
static uint8_t idx_stage[16u * 1024u];
static uint8_t table_buffers[64u * 1024u];
static cdb_db_config cfg;
static cdb_db db;

/* Operators, the scan's record buffer, and Sort's rows and pool */
static uint8_t scratch[64u * 1024u];

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
    cdb_header h;

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
    cfg.doc_max = DOC_MAX;
    cfg.durability = CDB_DURABILITY_FULL;
    TEST_ASSERT_EQUAL_size_t(cdb_doc_buffers_size(PAGE, DOC_MAX), sizeof doc_buffers);
    TEST_ASSERT_TRUE(cdb_table_buffers_size(PAGE, DOC_MAX) <= sizeof table_buffers);

    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x5C, sizeof h.db_id);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_open(&db, &env, "live", &cfg, &h));
}

void tearDown(void)
{
    cdb_db_close(&db);
}

static void begin(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL)); }
static void commit(void) { TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db)); }

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

static void obj_end(enc *e)
{
    cdb_value_end_object(&e->w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, e->w.error);
    e->n = cdb_value_writer_len(&e->w);
}

/* ------------------------------------------------------------ fixtures */

/* books: id int (primary key), title string, year int */
static uint64_t T, F_ID, F_TITLE, F_YEAR;

static void make_books(void)
{
    static const struct { int64_t id; const char *title; int64_t year; } rows[] = {
        {1342, "Pride and Prejudice", 1813},
        {84, "Frankenstein", 1818},
        {11, "Alice in Wonderland", 1865},
        {1661, "Sherlock Holmes", 1892},
        {98, "A Tale of Two Cities", 1859},
    };
    cdb_table_column cols[3];
    unsigned pk = 0;
    cdb_table_def def;
    uint64_t entry, frank = 0;
    size_t i;

    memset(cols, 0, sizeof cols);
    cols[0].name = "id";
    cols[0].name_len = 2;
    cols[0].type = CDB_TYPE_INT;
    cols[1].name = "title";
    cols[1].name_len = 5;
    cols[1].type = CDB_TYPE_STRING;
    cols[2].name = "year";
    cols[2].name_len = 4;
    cols[2].type = CDB_TYPE_INT;
    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 3;
    def.primary_key = &pk;
    def.npk = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "books", 5, &def, &T));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "id", 2, &F_ID));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "title", 5, &F_TITLE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "year", 4, &F_YEAR));

    begin();
    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        enc e;
        obj_begin(&e);
        cdb_value_field(&e.w, F_ID);
        cdb_value_write_int(&e.w, rows[i].id);
        cdb_value_field(&e.w, F_TITLE);
        cdb_value_write_string(&e.w, rows[i].title, strlen(rows[i].title));
        cdb_value_field(&e.w, F_YEAR);
        cdb_value_write_int(&e.w, rows[i].year);
        obj_end(&e);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, T, e.b, e.n, &entry));
        if (rows[i].id == 84)
            frank = entry;
    }
    commit();

    /* A gap in the entry IDs: the scan must skip it, not emit an empty row */
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_delete(&db, T, frank));
    commit();
}

/* passages: a document pool with named fields */
static uint64_t P;

static void make_passages(void)
{
    static const struct { int64_t ebook; int64_t seq; const char *text; } docs[] = {
        {1342, 0, "It is a truth universally acknowledged"},
        {1342, 1, "Mr. Bennet was so odd a mixture"},
        {11, 0, "Alice was beginning to get very tired"},
    };
    uint64_t f_ebook, f_seq, f_text, entry;
    size_t i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "passages", 8, CDB_MODEL_DOCUMENT, &P));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&db.catalog, P, "ebook", 5, &f_ebook));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&db.catalog, P, "seq", 3, &f_seq));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&db.catalog, P, "text", 4, &f_text));

    begin();
    for (i = 0; i < sizeof docs / sizeof docs[0]; i++) {
        enc e;
        obj_begin(&e);
        cdb_value_field(&e.w, f_ebook);
        cdb_value_write_int(&e.w, docs[i].ebook);
        cdb_value_field(&e.w, f_seq);
        cdb_value_write_int(&e.w, docs[i].seq);
        cdb_value_field(&e.w, f_text);
        cdb_value_write_string(&e.w, docs[i].text, strlen(docs[i].text));
        obj_end(&e);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, P, NULL, 0, e.b, e.n, &entry));
    }
    commit();
}

/* ------------------------------------------------------------ helpers */

static uint8_t wire[2048];
static cdb_exec_cursor cur;

static void open_text(const char *q)
{
    size_t len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CDB_OK, cdb_compile_query(q, strlen(q), &db.catalog, wire, sizeof wire, &len, &err),
                                  err.msg != NULL ? err.msg : q);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, &db, wire, len, NULL, 0, scratch, sizeof scratch));
}

/* The named field of a row: an output field, or else from its record */
static cdb_value field(cdb_exec_row *row, const char *name)
{
    cdb_value v;
    int found = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_row_get_field(row, &cur.eval_ctx, name, strlen(name), &v, &found));
    TEST_ASSERT_TRUE_MESSAGE(found, name);
    return v;
}

static void expect_str(cdb_exec_row *row, const char *name, const char *want)
{
    cdb_value v = field(row, name);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, v.type);
    TEST_ASSERT_EQUAL_size_t(strlen(want), v.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(want, v.payload, v.payload_len);
}

static int next(cdb_exec_row *row)
{
    int has = 0;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, row, &has));
    return has;
}

/* ------------------------------------------------------------ tests */

/* A bare scan reads every live row, with its fields, and skips the deleted one */
static void test_scan_reads_table_rows(void)
{
    cdb_exec_row row;
    size_t n = 0;

    make_books();
    open_text("SELECT * FROM Scan(\"books\")");
    while (next(&row)) {
        TEST_ASSERT_NOT_NULL(row.record_data);
        TEST_ASSERT_TRUE(field(&row, "id").as.i64 != 84);
        n++;
    }
    TEST_ASSERT_EQUAL_size_t(4, n);
    cdb_exec_close(&cur);
}

static void test_filter_on_table_field(void)
{
    cdb_exec_row row;

    make_books();
    open_text("SELECT * FROM Scan(\"books\") WHERE id = 1342");
    TEST_ASSERT_TRUE(next(&row));
    expect_str(&row, "title", "Pride and Prejudice");
    TEST_ASSERT_EQUAL_INT64(1813, field(&row, "year").as.i64);
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);

    /* No match is no rows */
    open_text("SELECT * FROM Scan(\"books\") WHERE id = 99999");
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);
}

static void test_project_table_columns(void)
{
    cdb_exec_row row;

    make_books();
    open_text("SELECT title FROM Scan(\"books\") WHERE year > 1860");
    TEST_ASSERT_TRUE(next(&row));
    TEST_ASSERT_EQUAL_size_t(1, row.n_fields);
    expect_str(&row, "title", "Alice in Wonderland");
    TEST_ASSERT_TRUE(next(&row));
    expect_str(&row, "title", "Sherlock Holmes");
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);
}

/* Sort buffers rows, so each must keep its own record, not the last one read */
static void test_sort_keeps_each_record(void)
{
    static const char *const want[] = {"Sherlock Holmes", "Alice in Wonderland", "A Tale of Two Cities",
                                       "Pride and Prejudice"};
    cdb_exec_row row;
    size_t i;

    make_books();
    open_text("SELECT title, year FROM Scan(\"books\") ORDER BY year DESC");
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(next(&row));
        expect_str(&row, "title", want[i]);
    }
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);

    open_text("SELECT title FROM Scan(\"books\") ORDER BY title LIMIT 2");
    TEST_ASSERT_TRUE(next(&row));
    expect_str(&row, "title", "A Tale of Two Cities");
    TEST_ASSERT_TRUE(next(&row));
    expect_str(&row, "title", "Alice in Wonderland");
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);
}

/* Aggregate holds group keys and MIN/MAX across rows */
static void test_aggregate_over_documents(void)
{
    cdb_query_writer qw;
    cdb_exec_row row;
    cdb_value v;

    make_passages();
    /* Aggregate(Scan("passages"), groups=[ebook], aggs=[(n, COUNT, *), (first, MIN, text)]) */
    cdb_query_writer_init(&qw, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate(&qw, 1, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "passages", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "ebook", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate_output(&qw, "n", 1, CDB_AGG_COUNT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "*", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate_output(&qw, "first", 5, CDB_AGG_MIN));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "text", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, &db, wire, cdb_query_writer_len(&qw), NULL, 0, scratch,
                                                sizeof scratch));

    TEST_ASSERT_TRUE(next(&row));
    TEST_ASSERT_EQUAL_INT64(1342, field(&row, "ebook").as.i64);
    TEST_ASSERT_EQUAL_INT64(2, field(&row, "n").as.i64);
    expect_str(&row, "first", "It is a truth universally acknowledged");
    TEST_ASSERT_TRUE(next(&row));
    TEST_ASSERT_EQUAL_INT64(11, field(&row, "ebook").as.i64);
    v = field(&row, "n");
    TEST_ASSERT_EQUAL_INT64(1, v.as.i64);
    expect_str(&row, "first", "Alice was beginning to get very tired");
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);
}

static void test_filter_on_document_field(void)
{
    cdb_exec_row row;

    make_passages();
    open_text("SELECT text FROM Scan(\"passages\") WHERE ebook = 1342 AND seq = 1");
    TEST_ASSERT_TRUE(next(&row));
    expect_str(&row, "text", "Mr. Bennet was so odd a mixture");
    TEST_ASSERT_FALSE(next(&row));
    cdb_exec_close(&cur);
}

/* A store that does not exist fails the query instead of matching nothing */
static void test_unknown_store_is_an_error(void)
{
    size_t len = 0;
    cdb_compile_error err;

    make_books();
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_compile_query("SELECT * FROM Scan(\"bokos\")", 27, &db.catalog, wire,
                                                    sizeof wire, &len, &err));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_exec_open(&cur, &db, wire, len, NULL, 0, scratch, sizeof scratch));
}

/* ------------------------------------------------------------ profiles (spec 17) */

/* 600 documents {n}: n is 0..599 in a shuffled order */
#define MANY 600u
static uint64_t F_N;

static void make_many(void)
{
    uint64_t store, entry;
    unsigned i;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "nums", 4, CDB_MODEL_DOCUMENT, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&db.catalog, store, "n", 1, &F_N));
    /* 20 to a commit: the fixture's pager holds few dirty nodes */
    for (i = 0; i < MANY; i++) {
        enc e;
        if (i % 20 == 0)
            begin();
        obj_begin(&e);
        cdb_value_field(&e.w, F_N);
        cdb_value_write_int(&e.w, (int64_t)((i * 7919u) % MANY));
        obj_end(&e);
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, store, NULL, 0, e.b, e.n, &entry));
        if (i % 20 == 19) {
            commit();
            /* Fold the journal in: the fault environment's files are 1 MB */
            if (i % 200 == 199)
                TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
        }
    }
}

/* Runs a query to its end: the rows seen and the status that ended it */
static cdb_status drain(const char *q, int64_t *firsts, size_t nfirst, size_t *rows)
{
    cdb_exec_row row;
    cdb_status s;
    int has = 0;

    *rows = 0;
    open_text(q);
    while ((s = cdb_exec_next(&cur, &row, &has)) == CDB_OK && has) {
        if (*rows < nfirst)
            firsts[*rows] = field(&row, "n").as.i64;
        (*rows)++;
    }
    cdb_exec_close(&cur);
    return s;
}

/* Sort holds more than 256 rows on the host profile; mcu stops at its limit */
static void test_sort_past_256_rows(void)
{
    int64_t first[3];
    size_t rows;
    cdb_status s;

    make_many();
    s = drain("SELECT n FROM Scan(\"nums\") ORDER BY n", first, 3, &rows);
#ifdef CDB_PROFILE_HOST
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    TEST_ASSERT_EQUAL_size_t(MANY, rows);
    TEST_ASSERT_EQUAL_INT64(0, first[0]);
    TEST_ASSERT_EQUAL_INT64(2, first[2]);
#else
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, s);
#endif
}

/* ORDER BY ... LIMIT keeps only the top N on the host profile */
static void test_top_n(void)
{
    int64_t first[3];
    size_t rows;
    cdb_status s;

    make_many();
    s = drain("SELECT n FROM Scan(\"nums\") ORDER BY n DESC LIMIT 3", first, 3, &rows);
#ifdef CDB_PROFILE_HOST
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    TEST_ASSERT_EQUAL_size_t(3, rows);
    TEST_ASSERT_EQUAL_INT64(599, first[0]);
    TEST_ASSERT_EQUAL_INT64(598, first[1]);
    TEST_ASSERT_EQUAL_INT64(597, first[2]);
#else
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, s);
#endif
}

/* A record past the scan's first buffer: read whole on the host profile */
static void test_large_record(void)
{
    static char text[10000];
    static uint8_t body[10100];
    cdb_value_writer w;
    cdb_exec_row row;
    uint64_t store, f_text, entry;
    cdb_status s;
    int has = 0;

    memset(text, 'a', sizeof text);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_create_store(&db, "big", 3, CDB_MODEL_DOCUMENT, &store));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_catalog_field_id(&db.catalog, store, "text", 4, &f_text));
    cdb_value_writer_init(&w, body, sizeof body, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, f_text);
    cdb_value_write_string(&w, text, sizeof text);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_doc_insert(&db, store, NULL, 0, body, cdb_value_writer_len(&w), &entry));
    commit();

    open_text("SELECT text FROM Scan(\"big\")");
    s = cdb_exec_next(&cur, &row, &has);
#ifdef CDB_PROFILE_HOST
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    TEST_ASSERT_TRUE(has);
    TEST_ASSERT_EQUAL_size_t(sizeof text, field(&row, "text").payload_len);
#else
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, s);
#endif
    cdb_exec_close(&cur);
}

/* The same for a table row: cdb_table_get must say how large the row is */
static void test_large_table_row(void)
{
    static char text[10000];
    static uint8_t body[10100];
    cdb_table_column col;
    cdb_table_def def;
    cdb_value_writer w;
    cdb_exec_row row;
    uint64_t table, f_body, entry;
    cdb_status s;
    int has = 0;

    memset(text, 'b', sizeof text);
    memset(&col, 0, sizeof col);
    col.name = "body";
    col.name_len = 4;
    col.type = CDB_TYPE_STRING;
    memset(&def, 0, sizeof def);
    def.columns = &col;
    def.ncolumns = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_create(&db, "whole", 5, &def, &table));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, table, "body", 4, &f_body));
    cdb_value_writer_init(&w, body, sizeof body, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, f_body);
    cdb_value_write_string(&w, text, sizeof text);
    cdb_value_end_object(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, w.error);
    begin();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_insert(&db, table, body, cdb_value_writer_len(&w), &entry));
    commit();

    open_text("SELECT body FROM Scan(\"whole\")");
    s = cdb_exec_next(&cur, &row, &has);
#ifdef CDB_PROFILE_HOST
    TEST_ASSERT_EQUAL_INT(CDB_OK, s);
    TEST_ASSERT_TRUE(has);
    TEST_ASSERT_EQUAL_size_t(sizeof text, field(&row, "body").payload_len);
#else
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, s);
#endif
    cdb_exec_close(&cur);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sort_past_256_rows);
    RUN_TEST(test_top_n);
    RUN_TEST(test_large_record);
    RUN_TEST(test_large_table_row);
    RUN_TEST(test_unknown_store_is_an_error);
    RUN_TEST(test_scan_reads_table_rows);
    RUN_TEST(test_filter_on_table_field);
    RUN_TEST(test_project_table_columns);
    RUN_TEST(test_sort_keeps_each_record);
    RUN_TEST(test_aggregate_over_documents);
    RUN_TEST(test_filter_on_document_field);
    return UNITY_END();
}
