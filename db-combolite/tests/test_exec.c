/* Tests for streaming query plan executor & cursors (CMBQL core).
 * Spec 11.5, 11.10; Spec 14.19.
 */
#include "unity.h"
#include "cdb_exec.h"
#include "cdb_query.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Helper: write integer literal value */
static size_t make_int_lit(uint8_t *buf, size_t cap, int64_t val)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, val));
    return cdb_value_writer_len(&w);
}

static void make_str_val(cdb_value *out, const char *str, size_t len)
{
    memset(out, 0, sizeof(*out));
    out->type = CDB_TYPE_STRING;
    out->tag = CDB_TAG_STRING;
    out->payload = (const uint8_t *)str;
    out->payload_len = len;
}

/* 1. Test Scan -> Filter -> Project */
static void test_exec_scan_filter_project(void)
{
    uint8_t wire[512], scratch[16384];
    uint8_t lit_eighteen[16], lit_two[16];
    size_t lit_eighteen_len, lit_two_len;
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row mock_users[3];
    cdb_exec_row out_row;
    int has_row = 0;

    lit_eighteen_len = make_int_lit(lit_eighteen, sizeof(lit_eighteen), 18);
    lit_two_len = make_int_lit(lit_two, sizeof(lit_two), 2);

    /* Construct Query:
     * Project(Filter(Scan("users"), age >= 18), passthrough=0, [name: Field("name"), double_age: age * 2]) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));

    /* Root: Project (passthrough = 0, 2 outputs) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project(&qw, 0, 2));

    /* Project Child: Filter */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_filter(&qw));

    /* Filter Child: Scan("users") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "users", 5));

    /* Filter Predicate: age >= 18 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&qw, CDB_BIN_GE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "age", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&qw, lit_eighteen, lit_eighteen_len));

    /* Project output 1: "user_name" = Field("name") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project_output(&qw, "user_name", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "name", 4));

    /* Project output 2: "double_age" = age * 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project_output(&qw, "double_age", 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&qw, CDB_BIN_MUL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "age", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&qw, lit_two, lit_two_len));

    /* Setup mock user rows */
    memset(mock_users, 0, sizeof(mock_users));

    /* User 1: Alice, 25 */
    mock_users[0].entry_id = 1;
    mock_users[0].n_fields = 2;
    mock_users[0].fields[0].name = "name"; mock_users[0].fields[0].name_len = 4;
    make_str_val(&mock_users[0].fields[0].val, "Alice", 5);
    mock_users[0].fields[1].name = "age"; mock_users[0].fields[1].name_len = 3;
    cdb_eval_make_int(&mock_users[0].fields[1].val, 25);

    /* User 2: Bob, 16 (filtered out!) */
    mock_users[1].entry_id = 2;
    mock_users[1].n_fields = 2;
    mock_users[1].fields[0].name = "name"; mock_users[1].fields[0].name_len = 4;
    make_str_val(&mock_users[1].fields[0].val, "Bob", 3);
    mock_users[1].fields[1].name = "age"; mock_users[1].fields[1].name_len = 3;
    cdb_eval_make_int(&mock_users[1].fields[1].val, 16);

    /* User 3: Charlie, 30 */
    mock_users[2].entry_id = 3;
    mock_users[2].n_fields = 2;
    mock_users[2].fields[0].name = "name"; mock_users[2].fields[0].name_len = 4;
    make_str_val(&mock_users[2].fields[0].val, "Charlie", 7);
    mock_users[2].fields[1].name = "age"; mock_users[2].fields[1].name_len = 3;
    cdb_eval_make_int(&mock_users[2].fields[1].val, 30);

    /* Open cursor */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));

    /* Bind mock scan */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "users", 5, mock_users, 3));

    /* Fetch Row 1: Alice (age 25 * 2 = 50) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_size_t(2, out_row.n_fields);
    TEST_ASSERT_EQUAL_STRING_LEN("user_name", out_row.fields[0].name, 9);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, out_row.fields[0].val.type);
    TEST_ASSERT_EQUAL_STRING_LEN("Alice", (const char *)out_row.fields[0].val.payload, 5);
    TEST_ASSERT_EQUAL_STRING_LEN("double_age", out_row.fields[1].name, 10);
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, out_row.fields[1].val.type);
    TEST_ASSERT_EQUAL_INT64(50, out_row.fields[1].val.as.i64);

    /* Fetch Row 2: Charlie (age 30 * 2 = 60) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("Charlie", (const char *)out_row.fields[0].val.payload, 7);
    TEST_ASSERT_EQUAL_INT64(60, out_row.fields[1].val.as.i64);

    /* Fetch Row 3: EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 2. Test Limit with offset */
static void test_exec_limit_and_offset(void)
{
    uint8_t wire[256], scratch[2048];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row items[4];
    cdb_exec_row out_row;
    int has_row = 0;
    size_t i;

    for (i = 0; i < 4; i++) {
        memset(&items[i], 0, sizeof(items[i]));
        items[i].entry_id = i + 1;
        items[i].n_fields = 1;
        items[i].fields[0].name = "id"; items[i].fields[0].name_len = 2;
        cdb_eval_make_int(&items[i].fields[0].val, (int64_t)(i + 1));
    }

    /* Limit(count=2, offset=1, Scan("items")) -> should yield items with id 2 and 3 */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_limit(&qw, 2, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "items", 5));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "items", 5, items, 4));

    /* Item 2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(2, out_row.fields[0].val.as.i64);

    /* Item 3 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(3, out_row.fields[0].val.as.i64);

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 3. Test Sort ASC / DESC */
static void test_exec_sort(void)
{
    /* Four rows of about 2 KB each, in the half of the arena the slots get */
    static uint8_t scratch[32768];
    uint8_t wire[256];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row scores[4];
    cdb_exec_row out_row;
    int has_row = 0;

    memset(scores, 0, sizeof(scores));
    scores[0].n_fields = 1; scores[0].fields[0].name = "score"; scores[0].fields[0].name_len = 5;
    cdb_eval_make_int(&scores[0].fields[0].val, 10);

    scores[1].n_fields = 1; scores[1].fields[0].name = "score"; scores[1].fields[0].name_len = 5;
    cdb_eval_make_int(&scores[1].fields[0].val, 50);

    scores[2].n_fields = 1; scores[2].fields[0].name = "score"; scores[2].fields[0].name_len = 5;
    cdb_eval_make_null(&scores[2].fields[0].val);

    scores[3].n_fields = 1; scores[3].fields[0].name = "score"; scores[3].fields[0].name_len = 5;
    cdb_eval_make_int(&scores[3].fields[0].val, 30);

    /* Sort(Scan("scores"), keys=[(score DESC, nulls LAST)]) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_sort(&qw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "scores", 6));

    /* Key 0 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "score", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_sort_key_suffix(&qw, CDB_SORT_DESC, CDB_NULLS_LAST));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "scores", 6, scores, 4));

    /* 50 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(50, out_row.fields[0].val.as.i64);

    /* 30 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(30, out_row.fields[0].val.as.i64);

    /* 10 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(10, out_row.fields[0].val.as.i64);

    /* NULL */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_TRUE(cdb_eval_is_null(&out_row.fields[0].val));

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 4. Test Aggregate: Group By region, SUM(amount), COUNT(*) */
static void test_exec_aggregate(void)
{
    /* Two groups of about 5 KB each, in the half of the arena the slots get */
    static uint8_t scratch[32768];
    uint8_t wire[512];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row sales[4];
    cdb_exec_row out_row;
    int has_row = 0;

    memset(sales, 0, sizeof(sales));

    /* North: 100 */
    sales[0].n_fields = 2;
    sales[0].fields[0].name = "region"; sales[0].fields[0].name_len = 6;
    make_str_val(&sales[0].fields[0].val, "North", 5);
    sales[0].fields[1].name = "amount"; sales[0].fields[1].name_len = 6;
    cdb_eval_make_int(&sales[0].fields[1].val, 100);

    /* South: 50 */
    sales[1].n_fields = 2;
    sales[1].fields[0].name = "region"; sales[1].fields[0].name_len = 6;
    make_str_val(&sales[1].fields[0].val, "South", 5);
    sales[1].fields[1].name = "amount"; sales[1].fields[1].name_len = 6;
    cdb_eval_make_int(&sales[1].fields[1].val, 50);

    /* North: 200 */
    sales[2].n_fields = 2;
    sales[2].fields[0].name = "region"; sales[2].fields[0].name_len = 6;
    make_str_val(&sales[2].fields[0].val, "North", 5);
    sales[2].fields[1].name = "amount"; sales[2].fields[1].name_len = 6;
    cdb_eval_make_int(&sales[2].fields[1].val, 200);

    /* South: 75 */
    sales[3].n_fields = 2;
    sales[3].fields[0].name = "region"; sales[3].fields[0].name_len = 6;
    make_str_val(&sales[3].fields[0].val, "South", 5);
    sales[3].fields[1].name = "amount"; sales[3].fields[1].name_len = 6;
    cdb_eval_make_int(&sales[3].fields[1].val, 75);

    /* Aggregate(Scan("sales"), groups=[region], aggs=[(total, SUM, amount), (cnt, COUNT, *)]) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate(&qw, 1, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "sales", 5));

    /* Group 0: region */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "region", 6));

    /* Agg 0: "total" = SUM(amount) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate_output(&qw, "total", 5, CDB_AGG_SUM));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "amount", 6));

    /* Agg 1: "cnt" = COUNT(*) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_aggregate_output(&qw, "cnt", 3, CDB_AGG_COUNT));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "*", 1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "sales", 5, sales, 4));

    /* Group 1: North (total=300, cnt=2) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("North", (const char *)out_row.fields[0].val.payload, 5);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, (float)out_row.fields[1].val.as.f64);
    TEST_ASSERT_EQUAL_INT64(2, out_row.fields[2].val.as.i64);

    /* Group 2: South (total=125, cnt=2) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("South", (const char *)out_row.fields[0].val.payload, 5);
    TEST_ASSERT_EQUAL_FLOAT(125.0f, (float)out_row.fields[1].val.as.f64);
    TEST_ASSERT_EQUAL_INT64(2, out_row.fields[2].val.as.i64);

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 5. Test Join: Left Outer Join */
static void test_exec_left_outer_join(void)
{
    uint8_t wire[512], scratch[16384];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row orders[2], customers[1];
    cdb_exec_row out_row;
    int has_row = 0;

    memset(orders, 0, sizeof(orders));
    /* Order 101, cust_id=1 */
    orders[0].n_fields = 2;
    orders[0].fields[0].name = "order_id"; orders[0].fields[0].name_len = 8;
    cdb_eval_make_int(&orders[0].fields[0].val, 101);
    orders[0].fields[1].name = "cust_id"; orders[0].fields[1].name_len = 7;
    cdb_eval_make_int(&orders[0].fields[1].val, 1);

    /* Order 102, cust_id=99 (unmatched) */
    orders[1].n_fields = 2;
    orders[1].fields[0].name = "order_id"; orders[1].fields[0].name_len = 8;
    cdb_eval_make_int(&orders[1].fields[0].val, 102);
    orders[1].fields[1].name = "cust_id"; orders[1].fields[1].name_len = 7;
    cdb_eval_make_int(&orders[1].fields[1].val, 99);

    memset(customers, 0, sizeof(customers));
    customers[0].n_fields = 2;
    customers[0].fields[0].name = "id"; customers[0].fields[0].name_len = 2;
    cdb_eval_make_int(&customers[0].fields[0].val, 1);
    customers[0].fields[1].name = "name"; customers[0].fields[1].name_len = 4;
    make_str_val(&customers[0].fields[1].val, "Acme", 4);

    /* Join(Scan("orders"), Scan("customers"), LeftOuter, (cust_id == id)) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_join(&qw, CDB_JOIN_LEFT_OUTER));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "orders", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "customers", 9));

    /* Join Condition: cust_id == id */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&qw, CDB_BIN_EQ));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "cust_id", 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "id", 2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "orders", 6, orders, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "customers", 9, customers, 1));

    /* Row 1: Order 101 matched with Acme */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(101, out_row.fields[0].val.as.i64);
    TEST_ASSERT_TRUE(out_row.has_right);
    TEST_ASSERT_EQUAL_size_t(4, out_row.n_fields);
    TEST_ASSERT_EQUAL_STRING_LEN("Acme", (const char *)out_row.fields[3].val.payload, 4);

    /* Row 2: Order 102 left-outer with NULL customer */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_INT64(102, out_row.fields[0].val.as.i64);
    TEST_ASSERT_TRUE(out_row.has_right);
    TEST_ASSERT_EQUAL_size_t(2, out_row.n_fields);

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 6. Test Union All */
static void test_exec_union_all(void)
{
    uint8_t wire[256], scratch[2048];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row rows_a[1], rows_b[1];
    cdb_exec_row out_row;
    int has_row = 0;

    memset(rows_a, 0, sizeof(rows_a));
    rows_a[0].entry_id = 10;
    memset(rows_b, 0, sizeof(rows_b));
    rows_b[0].entry_id = 20;

    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_union(&qw, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "source_a", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "source_b", 8));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "source_a", 8, rows_a, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "source_b", 8, rows_b, 1));

    /* Row from A */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_UINT64(10, out_row.entry_id);

    /* Row from B */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_UINT64(20, out_row.entry_id);

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 7. Test Window (Tumbling time window) */
static void test_exec_window(void)
{
    uint8_t wire[256], scratch[16384];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row tick;
    cdb_exec_row out_row;
    int has_row = 0;

    memset(&tick, 0, sizeof(tick));
    tick.entry_id = 1;
    tick.timestamp = 1500000000LL; /* 1.5 seconds */
    tick.n_fields = 1;
    tick.fields[0].name = "ts"; tick.fields[0].name_len = 2;
    cdb_eval_make_int(&tick.fields[0].val, 1500000000LL);

    /* Window(Scan("ticks"), type=Tumbling, time_field="ts", size_ns=1000000000, step_ns=0) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_window(&qw, CDB_WINDOW_TUMBLING, "ts", 2, 1000000000ULL, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "ticks", 5));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "ticks", 5, &tick, 1));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_size_t(3, out_row.n_fields);
    TEST_ASSERT_EQUAL_STRING_LEN("$window_start", out_row.fields[1].name, 13);
    TEST_ASSERT_EQUAL_INT64(1000000000LL, out_row.fields[1].val.as.i64);
    TEST_ASSERT_EQUAL_STRING_LEN("$window_end", out_row.fields[2].name, 11);
    TEST_ASSERT_EQUAL_INT64(2000000000LL, out_row.fields[2].val.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 8. Test Asof Join */
static void test_exec_asof_join(void)
{
    uint8_t wire[512], scratch[16384];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row trades[1], quotes[3];
    cdb_exec_row out_row;
    int has_row = 0;

    memset(trades, 0, sizeof(trades));
    trades[0].entry_id = 1;
    trades[0].timestamp = 1500;
    trades[0].n_fields = 1;
    trades[0].fields[0].name = "price"; trades[0].fields[0].name_len = 5;
    cdb_eval_make_int(&trades[0].fields[0].val, 100);

    memset(quotes, 0, sizeof(quotes));
    quotes[0].entry_id = 10; quotes[0].timestamp = 1000; quotes[0].n_fields = 1;
    quotes[0].fields[0].name = "bid"; quotes[0].fields[0].name_len = 3;
    cdb_eval_make_int(&quotes[0].fields[0].val, 98);

    quotes[1].entry_id = 20; quotes[1].timestamp = 1400; quotes[1].n_fields = 1;
    quotes[1].fields[0].name = "bid"; quotes[1].fields[0].name_len = 3;
    cdb_eval_make_int(&quotes[1].fields[0].val, 99); /* Closest preceding! */

    quotes[2].entry_id = 30; quotes[2].timestamp = 1600; quotes[2].n_fields = 1;
    quotes[2].fields[0].name = "bid"; quotes[2].fields[0].name_len = 3;
    cdb_eval_make_int(&quotes[2].fields[0].val, 101);

    /* AsofJoin(Scan("trades"), Scan("quotes"), Backward, tolerance=500, time=timestamp) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_asof_join(&qw, CDB_ASOF_BACKWARD, 500));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "trades", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "quotes", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "$timestamp", 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "$timestamp", 10));
    /* Empty condition = match all */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&qw, (const uint8_t *)"\x02", 1)); /* TRUE */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "trades", 6, trades, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "quotes", 6, quotes, 3));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_TRUE(out_row.has_right);
    TEST_ASSERT_EQUAL_UINT64(20, out_row.right_entry_id); /* Matched quote at 1400 */
    TEST_ASSERT_EQUAL_size_t(2, out_row.n_fields);
    TEST_ASSERT_EQUAL_STRING_LEN("bid", out_row.fields[1].name, 3);
    TEST_ASSERT_EQUAL_INT64(99, out_row.fields[1].val.as.i64);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 9. Test Unnest array elements into discrete stream rows */
static void test_exec_unnest(void)
{
    uint8_t wire[256], scratch[16384], arr_buf[64];
    cdb_query_writer qw;
    cdb_value_writer vw;
    cdb_exec_cursor cur;
    cdb_exec_row doc;
    cdb_exec_row out_row;
    int has_row = 0;

    cdb_value_writer_init(&vw, arr_buf, sizeof(arr_buf), CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_array(&vw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&vw, "sdr", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&vw, "radio", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&vw, "dsp", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_array(&vw));

    memset(&doc, 0, sizeof(doc));
    doc.entry_id = 42;
    doc.n_fields = 1;
    doc.fields[0].name = "tags"; doc.fields[0].name_len = 4;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_read(arr_buf, cdb_value_writer_len(&vw), CDB_ORDER_LITTLE, &doc.fields[0].val));

    /* Unnest(Scan("docs"), item_alias="tag", index_alias="idx", array_expr=Field("tags")) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_unnest(&qw, "tag", 3, "idx", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "docs", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&qw, "tags", 4));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "docs", 4, &doc, 1));

    /* Item 0: sdr, idx=0 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("sdr", (const char *)out_row.fields[1].val.payload, 3);
    TEST_ASSERT_EQUAL_INT64(0, out_row.fields[2].val.as.i64);

    /* Item 1: radio, idx=1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("radio", (const char *)out_row.fields[1].val.payload, 5);
    TEST_ASSERT_EQUAL_INT64(1, out_row.fields[2].val.as.i64);

    /* Item 2: dsp, idx=2 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_STRING_LEN("dsp", (const char *)out_row.fields[1].val.payload, 3);
    TEST_ASSERT_EQUAL_INT64(2, out_row.fields[2].val.as.i64);

    /* EOF */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_FALSE(has_row);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

/* 10. Test HybridRank (Reciprocal Rank Fusion) */
static void test_exec_hybrid_rank(void)
{
    uint8_t wire[256], scratch[16384];
    cdb_query_writer qw;
    cdb_exec_cursor cur;
    cdb_exec_row branch_a[2], branch_b[2];
    cdb_exec_row out_row;
    int has_row = 0;

    /* Branch A: text search hits */
    memset(branch_a, 0, sizeof(branch_a));
    branch_a[0].store_id = 1; branch_a[0].entry_id = 100;
    branch_a[1].store_id = 1; branch_a[1].entry_id = 200;

    /* Branch B: vector search hits */
    memset(branch_b, 0, sizeof(branch_b));
    branch_b[0].store_id = 1; branch_b[0].entry_id = 200; /* In both branches! */
    branch_b[1].store_id = 1; branch_b[1].entry_id = 300;

    /* HybridRank(RRF, [Scan("branch_a") weight=1000, Scan("branch_b") weight=1000]) */
    cdb_query_writer_init(&qw, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&qw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_hybrid_rank(&qw, CDB_FUSE_RRF, 2));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "branch_a", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_branch_weight(&qw, 1000));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&qw, "branch_b", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_branch_weight(&qw, 1000));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_open(&cur, NULL, wire, cdb_query_writer_len(&qw),
                                                NULL, 0, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "branch_a", 8, branch_a, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_set_mock_scan(&cur, "branch_b", 8, branch_b, 2));

    /* Entry 200 should be rank 1 because it appeared in both branches */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_next(&cur, &out_row, &has_row));
    TEST_ASSERT_TRUE(has_row);
    TEST_ASSERT_EQUAL_UINT64(200, out_row.entry_id);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_exec_close(&cur));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exec_scan_filter_project);
    RUN_TEST(test_exec_limit_and_offset);
    RUN_TEST(test_exec_sort);
    RUN_TEST(test_exec_aggregate);
    RUN_TEST(test_exec_left_outer_join);
    RUN_TEST(test_exec_union_all);
    RUN_TEST(test_exec_window);
    RUN_TEST(test_exec_asof_join);
    RUN_TEST(test_exec_unnest);
    RUN_TEST(test_exec_hybrid_rank);
    return UNITY_END();
}
