/* Row groups of column-oriented tables (spec 9.12, 14.10.2): creating a
 * column-oriented table, building groups, reading rows back, statistics,
 * nibblet and overflow-chain chunks, compression, the byte cap, dropping,
 * nibblet merges that move chunks, and corrupt headers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_value.h"
#include "cdb_endian.h"
#include "cdb_faultenv.h"
#include "cdb_key.h"
#include "cdb_nib.h"
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
    /* The area covers the codec too, as for a platform without an
     * allocator. */
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

/* ------------------------------------------------------------ the table */

/* metrics: a int not-null, s string, b bool, x f64; column-oriented. */
static uint64_t T, F_A, F_S, F_B, F_X;
static cdb_rg G;
static cdb_rg_col COLS[4];

static cdb_status make_table_ex(unsigned layout, uint32_t rows, uint32_t cap, uint64_t *store)
{
    cdb_table_column cols[4];
    cdb_table_def def;
    memset(cols, 0, sizeof cols);
    cols[0].name = "a";
    cols[0].name_len = 1;
    cols[0].type = CDB_TYPE_INT;
    cols[0].not_null = 1;
    cols[1].name = "s";
    cols[1].name_len = 1;
    cols[1].type = CDB_TYPE_STRING;
    cols[2].name = "b";
    cols[2].name_len = 1;
    cols[2].type = CDB_TYPE_BOOL;
    cols[3].name = "x";
    cols[3].name_len = 1;
    cols[3].type = CDB_TYPE_F64;
    memset(&def, 0, sizeof def);
    def.columns = cols;
    def.ncolumns = 4;
    def.layout = layout;
    def.group_rows = rows;
    def.chunk_cap = cap;
    return cdb_table_create(&db, layout ? "metrics" : "plain", layout ? 7u : 5u, &def, store);
}

static void make_table(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, ROWS, CAP, &T));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "a", 1, &F_A));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "s", 1, &F_S));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "b", 1, &F_B));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_column_id(&db, T, "x", 1, &F_X));
    COLS[0].field_id = F_A;
    COLS[0].tag = CDB_TAG_INT;
    COLS[1].field_id = F_S;
    COLS[1].tag = CDB_TAG_STRING;
    COLS[2].field_id = F_B;
    COLS[2].tag = CDB_TAG_FALSE;
    COLS[3].field_id = F_X;
    COLS[3].tag = CDB_TAG_F64;
    TEST_ASSERT_TRUE(F_A < F_S && F_S < F_B && F_B < F_X);
}

static void groups(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_table_rowgroups(&db, T, &G));
}

/* ------------------------------------------------------------ rows */

/* Rows of a group being built: each an encoded object of its present
 * columns, side by side in one arena. */
#define MAXROWS 256u
static uint8_t row_arena[256u * 1024u];
static size_t row_used;
static uint64_t ent[MAXROWS];
static size_t roff[MAXROWS], rlen[MAXROWS];
static uint32_t nrows;

typedef struct rowsrc {
    uint32_t first, n, i;
} rowsrc;

static void rows_reset(void)
{
    row_used = 0;
    nrows = 0;
}

static cdb_value_writer W;

static void row_begin(uint64_t e)
{
    ent[nrows] = e;
    roff[nrows] = row_used;
    cdb_value_writer_init(&W, row_arena + row_used, sizeof row_arena - row_used, ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&W);
}

static void row_end(void)
{
    cdb_value_end_object(&W);
    TEST_ASSERT_EQUAL_INT(CDB_OK, W.error);
    rlen[nrows] = cdb_value_writer_len(&W);
    row_used += rlen[nrows];
    nrows++;
}

/* The standard row of entry e: a, s (null when i % 5 == 0), b, x (null
 * when i % 4 == 1). */
static void std_row(uint32_t i, uint64_t e)
{
    char s[32];
    row_begin(e);
    cdb_value_field(&W, F_A);
    cdb_value_write_int(&W, (int64_t)i * 7 - 100);
    if (i % 5u != 0) {
        snprintf(s, sizeof s, "name-%u", (unsigned)(i % 9u));
        cdb_value_field(&W, F_S);
        cdb_value_write_string(&W, s, strlen(s));
    }
    cdb_value_field(&W, F_B);
    cdb_value_write_bool(&W, i % 3u == 0);
    if (i % 4u != 1) {
        cdb_value_field(&W, F_X);
        cdb_value_write_f64(&W, (double)i * 0.5);
    }
    row_end();
}

static cdb_status src_rewind(void *ctx)
{
    ((rowsrc *)ctx)->i = 0;
    return CDB_OK;
}

static cdb_status src_next(void *ctx, uint64_t field, uint64_t *e, const uint8_t **v, size_t *l)
{
    rowsrc *s = (rowsrc *)ctx;
    uint32_t r = s->first + s->i++;
    cdb_value obj, val;
    int found;
    *e = ent[r];
    *v = NULL;
    *l = 0;
    if (field == 0)
        return CDB_OK;
    if (cdb_value_read(row_arena + roff[r], rlen[r], ORDER, &obj) != CDB_OK
        || cdb_value_object_get(&obj, ORDER, field, &val, &found) != CDB_OK)
        return CDB_E_CORRUPT;
    if (found && val.type != CDB_TYPE_NULL) {
        *v = val.start;
        *l = val.encoded_len;
    }
    return CDB_OK;
}

static cdb_status write_rows(uint32_t first, uint32_t n, const cdb_rg_col *cols, unsigned ncols)
{
    rowsrc s;
    cdb_rg_source src;
    s.first = first;
    s.n = n;
    s.i = 0;
    src.ctx = &s;
    src.rewind = src_rewind;
    src.next = src_next;
    return cdb_rg_write(&G, &src, n, 1, cols, ncols);
}

static uint8_t got[8192];

/* Every row of [first, first+n) reads back exactly as written. */
static void check_rows(uint32_t first, uint32_t n)
{
    uint32_t r;
    for (r = first; r < first + n; r++) {
        size_t len;
        int found;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_get_row(&G, ent[r], got, sizeof got, &len, &found));
        TEST_ASSERT_TRUE(found);
        TEST_ASSERT_EQUAL_size_t(rlen[r], len);
        TEST_ASSERT_EQUAL_MEMORY(row_arena + roff[r], got, len);
    }
}

static void absent(uint64_t e)
{
    size_t len;
    int found = 1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_get_row(&G, e, got, sizeof got, &len, &found));
    TEST_ASSERT_FALSE(found);
}

/* ------------------------------------------------------------ tests */

static void test_create(void)
{
    uint64_t plain;
    uint8_t row[16];
    size_t len;
    uint64_t e;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table_ex(CDB_TABLE_LAYOUT_ROW, 0, 0, &plain));
    reopen();
    groups();
    TEST_ASSERT_EQUAL_UINT32(ROWS, G.rows);
    TEST_ASSERT_EQUAL_size_t(CAP, G.cap);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_table_rowgroups(&db, plain, &G));
    /* Row calls are not built for column-oriented tables yet. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_begin(&db, CDB_DURABILITY_FULL));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_table_insert(&db, T, row, 0, &e));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_table_delete(&db, T, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_tx_commit(&db));
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_table_get(&db, T, 1, row, sizeof row, &len));
    {
        uint32_t ix;
        TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_table_create_index(&db, T, &F_A, 1, 0, &ix));
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_create_refused(void)
{
    uint64_t s;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    /* The cap must hold the entry-ID chunk: rows x 11 bytes. Nothing else
     * bounds it from below; in particular not doc_max or the page size. */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, ROWS, ROWS * 11u - 1u, &s));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, CDB_RG_ROWS_MAX + 1u,
                                                           UINT32_MAX, &s));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, make_table_ex(7, ROWS, CAP, &s));
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, ROWS, ROWS * 11u, &s));
    TEST_ASSERT_TRUE(ROWS * 11u < PAGE);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    /* No work area, or one too small. */
    cfg.col_work = NULL;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, ROWS, CAP, &s));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
    cfg.col_work = col_work;
    cfg.col_work_size = 1024;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, make_table_ex(CDB_TABLE_LAYOUT_COLUMN, ROWS, CAP, &s));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_roundtrip(void)
{
    uint32_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 40; i++)
        std_row(i, 10u + 2u * i); /* entries 10, 12, ..., 88 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 40, COLS, 4));
    check_rows(0, 40);
    absent(9);
    absent(11);
    absent(89);
    absent(1000);
    /* The header as built. */
    {
        int found;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, 88, &found));
        TEST_ASSERT_TRUE(found);
    }
    TEST_ASSERT_EQUAL_UINT64(10, G.hdr->first);
    TEST_ASSERT_EQUAL_UINT64(88, G.hdr->last);
    TEST_ASSERT_EQUAL_UINT32(40, G.hdr->rows);
    TEST_ASSERT_EQUAL_UINT32(1, G.hdr->schema);
    TEST_ASSERT_EQUAL_UINT(4, G.hdr->ncols);
    TEST_ASSERT_EQUAL_UINT32(0, G.hdr->cols[0].nulls);
    TEST_ASSERT_EQUAL_UINT32(8, G.hdr->cols[1].nulls);  /* i % 5 == 0 */
    TEST_ASSERT_EQUAL_UINT32(10, G.hdr->cols[3].nulls); /* i % 4 == 1 */
    /* Durable across a reconciliation and a reopen. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    reopen();
    groups();
    check_rows(0, 40);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_stats(void)
{
    cdb_value v;
    uint32_t i;
    int found;
    char longs[64];
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 20; i++)
        std_row(i, 100u + i);
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 20, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, 119, &found));
    TEST_ASSERT_TRUE(found);
    /* a: -100 .. 33 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(G.hdr->cols[0].min, G.hdr->cols[0].min_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT64(-100, v.as.i64);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(G.hdr->cols[0].max, G.hdr->cols[0].max_len, ORDER, &v));
    TEST_ASSERT_EQUAL_INT64(33, v.as.i64);
    /* s: "name-0" .. "name-8" (i % 9 for i not a multiple of 5; 9 and 18
     * give 0) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(G.hdr->cols[1].min, G.hdr->cols[1].min_len, ORDER, &v));
    TEST_ASSERT_EQUAL_MEMORY("name-0", v.payload, 6);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(G.hdr->cols[1].max, G.hdr->cols[1].max_len, ORDER, &v));
    TEST_ASSERT_EQUAL_MEMORY("name-8", v.payload, 6);
    /* b: false .. true */
    TEST_ASSERT_EQUAL_UINT8(CDB_TAG_FALSE, G.hdr->cols[2].min[0]);
    TEST_ASSERT_EQUAL_UINT8(CDB_TAG_TRUE, G.hdr->cols[2].max[0]);

    /* A column with only nulls has no statistics; one whose largest value
     * passes CDB_RG_STAT_MAX has none either. */
    rows_reset();
    memset(longs, 'z', sizeof longs);
    for (i = 0; i < 6; i++) {
        row_begin(200u + i);
        cdb_value_field(&W, F_A);
        cdb_value_write_int(&W, (int64_t)i);
        cdb_value_field(&W, F_S);
        cdb_value_write_string(&W, longs, i == 3 ? sizeof longs : 4u);
        row_end();
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 6, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, 205, &found));
    TEST_ASSERT_EQUAL_UINT8(0, G.hdr->cols[1].min_len);
    TEST_ASSERT_EQUAL_UINT8(0, G.hdr->cols[1].max_len);
    TEST_ASSERT_EQUAL_UINT32(6, G.hdr->cols[2].nulls);
    TEST_ASSERT_EQUAL_UINT8(0, G.hdr->cols[2].min_len);
    check_rows(0, 6);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Chunks past a quarter page go to an overflow chain; compressible ones are
 * kept as LZMA2 when that saves an eighth, incompressible ones plain. */
static void test_big_chunks(void)
{
    cdb_rg_column c;
    uint64_t rnd = 99;
    uint32_t i;
    int found;
    char s[64];
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 60; i++) {
        row_begin(1000u + i);
        /* Random 62-bit ints: bit-packed at about 62 bits each, about 470
         * bytes that no codec can shrink by an eighth. */
        rnd = rnd * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        cdb_value_field(&W, F_A);
        cdb_value_write_int(&W, (int64_t)(rnd >> 2));
        /* Distinct, so no dictionary helps; but highly repetitive. */
        snprintf(s, sizeof s, "row-%04u-the-quick-brown-fox-jumps", (unsigned)i);
        cdb_value_field(&W, F_S);
        cdb_value_write_string(&W, s, strlen(s));
        row_end();
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 60, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, 1059, &found));
    TEST_ASSERT_FALSE(cdb_nib_is(G.hdr->cols[0].addr));
    TEST_ASSERT_TRUE(G.hdr->cols[0].stored > PAGE / 4u);
    TEST_ASSERT_TRUE(cdb_nib_is(G.hdr->cols[2].addr)); /* all null: tiny */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_column_open(&G, 0, &c));
    TEST_ASSERT_EQUAL_UINT8(CDB_CODEC_NONE, G.chunk[1]);
    TEST_ASSERT_EQUAL_UINT8(CDB_COLENC_BITPACK, G.chunk[0]);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_column_open(&G, 1, &c));
    if (cdb_codec_available(CDB_CODEC_LZMA2))
        TEST_ASSERT_EQUAL_UINT8(CDB_CODEC_LZMA2, G.chunk[1]);
    else
        TEST_ASSERT_EQUAL_UINT8(CDB_CODEC_NONE, G.chunk[1]);
    check_rows(0, 60);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    reopen();
    groups();
    check_rows(0, 60);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A column past the cap refuses the group and leaves nothing behind; the
 * same rows fit as two groups. */
static void test_cap(void)
{
    static char big[1000];
    uint32_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    memset(big, 'q', sizeof big);
    for (i = 0; i < 6; i++) {
        row_begin(50u + i);
        cdb_value_field(&W, F_A);
        cdb_value_write_int(&W, (int64_t)i);
        big[0] = (char)('a' + i);
        cdb_value_field(&W, F_S);
        cdb_value_write_string(&W, big, sizeof big);
        row_end();
    }
    TEST_ASSERT_EQUAL_INT(CDB_E_OVERFLOW, write_rows(0, 6, COLS, 4));
    absent(50);
    absent(55);
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 3, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(3, 3, COLS, 4));
    check_rows(0, 6);
    /* Overlapping an existing group is refused. */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, write_rows(2, 2, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_bad_source(void)
{
    uint32_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 4; i++)
        std_row(i, 5u - i); /* descending */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, write_rows(0, 4, COLS, 4));
    rows_reset();
    row_begin(1);
    cdb_value_field(&W, F_A);
    cdb_value_write_string(&W, "x", 1); /* a string in an int column */
    row_end();
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, write_rows(0, 1, COLS, 4));
    absent(1);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, write_rows(0, 0, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, write_rows(0, ROWS + 1u, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

static void test_drop(void)
{
    uint32_t i;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 30; i++)
        std_row(i, 1u + i);
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 10, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(10, 10, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(20, 10, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_drop(&G, 20));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_rg_drop(&G, 20));
    check_rows(0, 10);
    check_rows(20, 10);
    for (i = 11; i <= 20; i++)
        absent(i);
    /* The range is free again. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(10, 10, COLS, 4));
    check_rows(0, 30);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    reopen();
    groups();
    check_rows(0, 30);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Many small groups share nibblet pages; dropping most leaves pages under
 * half full, and reconciliation merges them, moving chunks: the surviving
 * groups' headers follow. */
static void test_merge_moves_chunks(void)
{
    uint64_t before[8][5], moved = 0;
    uint32_t i, g;
    int found;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 160; i++)
        std_row(i, 1u + i);
    for (g = 0; g < 40; g++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(g * 4u, 4, COLS, 4));
    /* Keep every fifth group. */
    for (g = 0; g < 40; g++)
        if (g % 5u != 0)
            TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_drop(&G, (uint64_t)g * 4u + 4u));
    for (g = 0; g < 8; g++) {
        unsigned c;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, (uint64_t)g * 20u + 4u, &found));
        TEST_ASSERT_TRUE(found);
        before[g][0] = G.hdr->ids.addr;
        for (c = 0; c < 4; c++)
            before[g][c + 1u] = G.hdr->cols[c].addr;
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    groups();
    for (g = 0; g < 8; g++) {
        unsigned c;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_rg_load(&G, (uint64_t)g * 20u + 4u, &found));
        moved += G.hdr->ids.addr != before[g][0];
        for (c = 0; c < 4; c++)
            moved += G.hdr->cols[c].addr != before[g][c + 1u];
        check_rows(g * 20u, 4);
    }
    TEST_ASSERT_TRUE(moved > 0);
    reopen();
    groups();
    for (g = 0; g < 8; g++)
        check_rows(g * 20u, 4);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* A dropped table's chunks are orphans: merging discards them. */
static void test_dropped_table(void)
{
    uint64_t keep;
    uint32_t i, g;
    cdb_rg K;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    TEST_ASSERT_EQUAL_INT(CDB_OK, make_table_ex(CDB_TABLE_LAYOUT_ROW, 0, 0, &keep));
    groups();
    rows_reset();
    for (i = 0; i < 80; i++)
        std_row(i, 1u + i);
    for (g = 0; g < 20; g++)
        TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(g * 4u, 4, COLS, 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_drop_store(&db, T));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_table_rowgroups(&db, T, &K));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_reconcile(&db));
    reopen();
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

/* Headers that break a rule are refused as corrupt. */
static void test_corrupt_header(void)
{
    uint8_t k[8], v[2048], bad[2048];
    size_t vlen;
    uint32_t i;
    int found;
    cdb_key_builder kb;
    TEST_ASSERT_EQUAL_INT(CDB_OK, open_db());
    make_table();
    groups();
    rows_reset();
    for (i = 0; i < 10; i++)
        std_row(i, 1u + i);
    TEST_ASSERT_EQUAL_INT(CDB_OK, write_rows(0, 10, COLS, 4));
    cdb_key_init(&kb, k, sizeof k);
    cdb_key_u64(&kb, 10);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_get(G.groups, k, 8, v, sizeof v, &vlen, &found));
    TEST_ASSERT_TRUE(found);
#define MUTATE(stmt)                                                                     \
    do {                                                                                 \
        memcpy(bad, v, vlen);                                                            \
        stmt;                                                                            \
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(G.groups, k, 8, bad, vlen));         \
        TEST_ASSERT_EQUAL_INT(CDB_E_CORRUPT, cdb_rg_load(&G, 10, &found));               \
    } while (0)
    MUTATE(cdb_store_u32(bad + 8, 0, CDB_ORDER_BIG));    /* no rows */
    MUTATE(cdb_store_u32(bad + 8, 11, CDB_ORDER_BIG));   /* more rows than the range */
    MUTATE(cdb_store_u64(bad, 11, CDB_ORDER_BIG));       /* first after last */
    MUTATE(cdb_store_u16(bad + 16, 5, CDB_ORDER_BIG));   /* a column too many */
    MUTATE(bad[18 + 7] = 1);                             /* entry-ID chunk's field ID */
    {
        /* Swap the field IDs of the first two columns: out of order. */
        size_t p = 18, q;
        p += 8u + 1u + 1u + v[18 + 9] + 4u + 4u + 1u + 1u; /* past the entry-ID chunk */
        q = p + 8u + 1u + 1u + v[p + 9] + 4u + 4u;
        q += 1u + v[q];
        q += 1u + v[q];
        MUTATE(cdb_store_u64(bad + p, F_S, CDB_ORDER_BIG); cdb_store_u64(bad + q, F_A, CDB_ORDER_BIG));
    }
    /* Back as it was: readable again. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_index_put(G.groups, k, 8, v, vlen));
    check_rows(0, 10);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_db_close(&db));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create);
    RUN_TEST(test_create_refused);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_stats);
    RUN_TEST(test_big_chunks);
    RUN_TEST(test_cap);
    RUN_TEST(test_bad_source);
    RUN_TEST(test_drop);
    RUN_TEST(test_merge_moves_chunks);
    RUN_TEST(test_dropped_table);
    RUN_TEST(test_corrupt_header);
    return UNITY_END();
}
