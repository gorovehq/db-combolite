/* Unit tests for CMBQL expression evaluation engine and cross-model field resolution. */
#include "unity.h"
#include "cdb_eval.h"
#include "cdb_query.h"
#include "combolite/cdb_value.h"

#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

static size_t make_int_literal(uint8_t *buf, size_t cap, int64_t val)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, val));
    return cdb_value_writer_len(&w);
}

static size_t make_f64_literal(uint8_t *buf, size_t cap, double val)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_f64(&w, val));
    return cdb_value_writer_len(&w);
}

static size_t make_null_literal(uint8_t *buf, size_t cap)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_null(&w));
    return cdb_value_writer_len(&w);
}

static size_t make_str_literal(uint8_t *buf, size_t cap, const char *s, size_t len)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&w, s, len));
    return cdb_value_writer_len(&w);
}

static void test_eval_literals(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    uint8_t vbuf[32];
    size_t vlen;
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* Write literal int 42 */
    vlen = make_int_literal(vbuf, sizeof(vbuf), 42);
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, vbuf, vlen));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
    TEST_ASSERT_EQUAL_INT64(42, res.as.i64);

    /* Write literal string "hello" */
    vlen = make_str_literal(vbuf, sizeof(vbuf), "hello", 5);
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, vbuf, vlen));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, res.type);
    TEST_ASSERT_EQUAL_UINT64(5, res.payload_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp("hello", res.payload, 5));
}

static void test_eval_params(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[128];
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;
    cdb_eval_param params[2];

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* Setup params: $0 = 100, $threshold = 50 */
    memset(params, 0, sizeof(params));
    params[0].index = 0;
    cdb_eval_make_int(&params[0].val, 100);

    params[1].name = "threshold";
    params[1].name_len = 9;
    cdb_eval_make_int(&params[1].val, 50);

    cdb_eval_ctx_set_params(&ctx, params, 2);

    /* Query indexed param $0 */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_param_index(&w, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
    TEST_ASSERT_EQUAL_INT64(100, res.as.i64);

    /* Query named param $threshold */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_param_named(&w, "threshold", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
    TEST_ASSERT_EQUAL_INT64(50, res.as.i64);

    /* Query unbound param */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_param_named(&w, "missing", 7));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
}

static void test_eval_fields_and_pseudo_fields(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    uint8_t doc_buf[256];
    cdb_value_writer vw;
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* Set pseudo fields */
    cdb_eval_ctx_set_entry_id(&ctx, 12345);
    cdb_eval_ctx_set_score(&ctx, 0.95);
    cdb_eval_ctx_set_timestamp(&ctx, 1600000000LL, CDB_TS_SECONDS);

    /* Test $entry_id */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "$entry_id", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
    TEST_ASSERT_EQUAL_INT64(12345, res.as.i64);

    /* Test $score */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "$score", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_F64, res.type);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.95, (float)res.as.f64);

    /* Build a document object with fields:
     * field 1: int 500 (price)
     * field 2: object (address) -> field 3: string "Kyoto" (city) */
    cdb_value_writer_init(&vw, doc_buf, sizeof(doc_buf), CDB_ORDER_BIG, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&vw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&vw, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&vw, 500));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&vw, 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_begin_object(&vw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_field(&vw, 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_string(&vw, "Kyoto", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_object(&vw));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_end_object(&vw));

    cdb_eval_ctx_set_record(&ctx, doc_buf, cdb_value_writer_len(&vw));

    /* Test numeric path "1" (without catalog) */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "1", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
    TEST_ASSERT_EQUAL_INT64(500, res.as.i64);

    /* Test nested path "2\x1F" "3" (object 2 -> field 3) */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "2\x1f" "3", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, res.type);
    TEST_ASSERT_EQUAL_UINT64(5, res.payload_len);
    TEST_ASSERT_EQUAL_INT(0, memcmp("Kyoto", res.payload, 5));

    /* Missing field "99" -> yields NULL */
    cdb_query_writer_init(&w, wire, sizeof(wire));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "99", 2));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
    TEST_ASSERT_EQUAL_INT(CDB_TYPE_NULL, res.type);
}

static void test_eval_tristate_and_comparisons(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;
    int matched = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* 10 < 20 -> TRUE */
    {
        uint8_t v1[16], v2[16];
        size_t l1 = make_int_literal(v1, sizeof(v1), 10);
        size_t l2 = make_int_literal(v2, sizeof(v2), 20);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_LT));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(1, res.as.boolean);

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_predicate(&ctx, wire, cdb_query_writer_len(&w), &matched));
        TEST_ASSERT_EQUAL_INT(1, matched);
    }

    /* Coercion: 15 (INT) == 15.0 (F64) -> TRUE */
    {
        uint8_t v1[16], v2[16];
        size_t l1 = make_int_literal(v1, sizeof(v1), 15);
        size_t l2 = make_f64_literal(v2, sizeof(v2), 15.0);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_EQ));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(1, res.as.boolean);
    }

    /* Tri-state: NULL == 10 -> NULL (unknown, falsy in filter) */
    {
        uint8_t v1[16], v2[16];
        size_t l1 = make_null_literal(v1, sizeof(v1));
        size_t l2 = make_int_literal(v2, sizeof(v2), 10);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_EQ));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_NULL, res.type);

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_predicate(&ctx, wire, cdb_query_writer_len(&w), &matched));
        TEST_ASSERT_EQUAL_INT(0, matched); /* NULL rejected by filter predicate */
    }

    /* IS NULL on NULL -> TRUE */
    {
        uint8_t v1[16];
        size_t l1 = make_null_literal(v1, sizeof(v1));

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_unary(&w, CDB_UNARY_IS_NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(1, res.as.boolean);
    }

    /* IS NOT NULL on NULL -> FALSE */
    {
        uint8_t v1[16];
        size_t l1 = make_null_literal(v1, sizeof(v1));

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_unary(&w, CDB_UNARY_IS_NOT_NULL));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(0, res.as.boolean);
    }
}

static void test_eval_arithmetic_and_scratch(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    uint8_t scratch[128];
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);
    cdb_eval_ctx_set_scratch(&ctx, scratch, sizeof(scratch));

    /* 20 + 30 = 50 */
    {
        uint8_t v1[16], v2[16];
        size_t l1 = make_int_literal(v1, sizeof(v1), 20);
        size_t l2 = make_int_literal(v2, sizeof(v2), 30);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_ADD));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_INT, res.type);
        TEST_ASSERT_EQUAL_INT64(50, res.as.i64);
    }

    /* Divide by zero: 10 / 0 -> NULL */
    {
        uint8_t v1[16], v2[16];
        size_t l1 = make_int_literal(v1, sizeof(v1), 10);
        size_t l2 = make_int_literal(v2, sizeof(v2), 0);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_DIV));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_NULL, res.type);
    }

    /* String concatenation: "Hello, " + "World!" -> "Hello, World!" */
    {
        uint8_t v1[32], v2[32];
        size_t l1 = make_str_literal(v1, sizeof(v1), "Hello, ", 7);
        size_t l2 = make_str_literal(v2, sizeof(v2), "World!", 6);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_ADD));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_STRING, res.type);
        TEST_ASSERT_EQUAL_UINT64(13, res.payload_len);
        TEST_ASSERT_EQUAL_INT(0, memcmp("Hello, World!", res.payload, 13));
    }
}

static void test_eval_like_and_contains(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[256];
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    /* LIKE: "database.sqlite" LIKE "%.sqlite" -> TRUE */
    {
        uint8_t v1[32], v2[32];
        size_t l1 = make_str_literal(v1, sizeof(v1), "database.sqlite", 15);
        size_t l2 = make_str_literal(v2, sizeof(v2), "%.sqlite", 8);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_LIKE));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(1, res.as.boolean);
    }

    /* CONTAINS: "combolite query engine" CONTAINS "query" -> TRUE */
    {
        uint8_t v1[32], v2[32];
        size_t l1 = make_str_literal(v1, sizeof(v1), "combolite query engine", 22);
        size_t l2 = make_str_literal(v2, sizeof(v2), "query", 5);

        cdb_query_writer_init(&w, wire, sizeof(wire));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_CONTAINS));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v1, l1));
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, v2, l2));

        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
        TEST_ASSERT_EQUAL_INT(CDB_TYPE_BOOL, res.type);
        TEST_ASSERT_EQUAL_INT(1, res.as.boolean);
    }
}

static void test_eval_budget_and_depth(void)
{
    cdb_eval_ctx ctx;
    uint8_t wire[64];
    uint8_t vbuf[16];
    size_t vlen;
    cdb_query_writer w;
    cdb_value res;
    size_t consumed = 0;

    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);

    vlen = make_int_literal(vbuf, sizeof(vbuf), 1);

    cdb_query_writer_init(&w, wire, sizeof(wire));
    cdb_query_write_expr_literal(&w, vbuf, vlen);

    /* Budget exhaustion */
    cdb_eval_ctx_set_budget(&ctx, 0);
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));

    /* Depth limit */
    cdb_eval_ctx_init(&ctx, CDB_ORDER_BIG, NULL, 0);
    ctx.max_depth = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_DEPTH, cdb_eval_expr_wire(&ctx, wire, cdb_query_writer_len(&w), &res, &consumed));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_eval_literals);
    RUN_TEST(test_eval_params);
    RUN_TEST(test_eval_fields_and_pseudo_fields);
    RUN_TEST(test_eval_tristate_and_comparisons);
    RUN_TEST(test_eval_arithmetic_and_scratch);
    RUN_TEST(test_eval_like_and_contains);
    RUN_TEST(test_eval_budget_and_depth);
    return UNITY_END();
}
