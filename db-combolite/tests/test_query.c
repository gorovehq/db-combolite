/* Tests for structured query wire format & operator tree (CMBQL core). */
#include "unity.h"
#include "cdb_query.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Encodes a simple int64 as a cdb_value literal. */
static size_t make_int_literal(uint8_t *buf, size_t cap, int64_t val)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_value_write_int(&w, val));
    return cdb_value_writer_len(&w);
}

/* A basic Scan("orders") query. */
static void test_query_header_and_scan(void)
{
    uint8_t wire[128];
    cdb_query_writer w;
    size_t root_off = 0;
    const uint8_t *p;
    cdb_query_node node;

    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "orders", 6));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    TEST_ASSERT_EQUAL_size_t(CDB_QUERY_HEADER_SIZE, root_off);

    p = wire + root_off;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, wire + cdb_query_writer_len(&w), &node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_SCAN, node.tag);
    TEST_ASSERT_EQUAL_size_t(6, node.as.scan.name_len);
    TEST_ASSERT_EQUAL_STRING_LEN("orders", node.as.scan.name, 6);
}

/* Filter and Project with expressions:
 * Project(Filter(Scan("users"), age >= 18), passthrough=0, [name: Field("profile.name"), adult: 1]) */
static void test_query_filter_and_project(void)
{
    uint8_t wire[512], lit_int[32], lit_one[32];
    size_t lit_int_len, lit_one_len, root_off;
    cdb_query_writer w;
    const uint8_t *p, *end;
    cdb_query_node proj_node, filt_node, scan_node;
    cdb_query_expr bin_expr, fld_expr, lit_expr;

    lit_int_len = make_int_literal(lit_int, sizeof lit_int, 18);
    lit_one_len = make_int_literal(lit_one, sizeof lit_one, 1);

    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));

    /* Root: Project (passthrough = 0, 2 outputs) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project(&w, 0, 2));

    /* Project Input: Filter */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_filter(&w));
    /* Filter Input: Scan("users") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "users", 5));
    /* Filter Predicate: Binary(GE, Field("age"), Literal(18)) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_GE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "age", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, lit_int, lit_int_len));

    /* Project Output 0: "user_name" = Field("profile\x1fname") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project_output(&w, "user_name", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "profile\x1f" "name", 12));

    /* Project Output 1: "is_adult" = Literal(1) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_project_output(&w, "is_adult", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_literal(&w, lit_one, lit_one_len));

    /* Validate full plan */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    end = wire + cdb_query_writer_len(&w);

    /* Read root Project */
    p = wire + root_off;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &proj_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_PROJECT, proj_node.tag);
    TEST_ASSERT_EQUAL_INT(0, proj_node.as.project.passthrough);
    TEST_ASSERT_EQUAL_size_t(2, proj_node.as.project.count);

    /* Read Filter child */
    p = proj_node.as.project.input_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &filt_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_FILTER, filt_node.tag);

    /* Read Scan child */
    p = filt_node.as.filter.input_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &scan_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_SCAN, scan_node.tag);
    TEST_ASSERT_EQUAL_STRING_LEN("users", scan_node.as.scan.name, 5);

    /* Read Filter Predicate */
    p = filt_node.as.filter.pred_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_expr(&p, end, &bin_expr));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_EXPR_BINARY, bin_expr.tag);
    TEST_ASSERT_EQUAL_UINT8(CDB_BIN_GE, bin_expr.as.binary.op);

    p = bin_expr.as.binary.left_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_expr(&p, end, &fld_expr));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_EXPR_FIELD, fld_expr.tag);
    TEST_ASSERT_EQUAL_STRING_LEN("age", fld_expr.as.field.path, 3);

    p = bin_expr.as.binary.right_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_expr(&p, end, &lit_expr));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_EXPR_LITERAL, lit_expr.tag);
}

/* Multi-model pipeline: HybridRank (TextSearch + VectorSearch) -> Join -> Limit */
static void test_query_hybrid_and_join(void)
{
    uint8_t wire[1024];
    cdb_query_writer w;
    size_t root_off;
    const uint8_t *p, *end;
    cdb_query_node lim_node, join_node, rrf_node;

    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));

    /* Limit(count=10, offset=0) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_limit(&w, 10, 0));

    /* Join(Inner) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_join(&w, CDB_JOIN_INNER));

    /* Join Left: HybridRank(RRF, 2 branches) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_hybrid_rank(&w, CDB_FUSE_RRF, 2));

    /* Branch 0: TextSearch(Scan("passages"), ALL, limit=50, "adventure", 1 field: "body") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_text_search(&w, CDB_QUERY_FTS_ALL, 50, "adventure", 9, 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "passages", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_field_name(&w, "body", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_branch_weight(&w, 1000)); /* 1.0 weight */

    /* Branch 1: VectorSearch(Scan("passage_vec"), field="$vector", near=Param(0), k=50, cosine) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_vector_search(&w, "$vector", 7, 50, CDB_QUERY_METRIC_COSINE));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "passage_vec", 11));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_param_index(&w, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_branch_weight(&w, 1500)); /* 1.5 weight */

    /* Join Right: Scan("catalog") */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "catalog", 7));

    /* Join Condition: Binary(EQ, Field("passages\x1f" "book_id"), Field("catalog\x1f" "id")) */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_EQ));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "passages\x1f" "book_id", 16));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "catalog\x1f" "id", 10));

    /* Validate full plan */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    end = wire + cdb_query_writer_len(&w);

    /* Read Limit */
    p = wire + root_off;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &lim_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_LIMIT, lim_node.tag);
    TEST_ASSERT_EQUAL_UINT64(10, lim_node.as.limit.count);
    TEST_ASSERT_EQUAL_UINT64(0, lim_node.as.limit.offset);

    /* Read Join */
    p = lim_node.as.limit.input_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &join_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_JOIN, join_node.tag);
    TEST_ASSERT_EQUAL_UINT8(CDB_JOIN_INNER, join_node.as.join.kind);

    /* Read Left HybridRank */
    p = join_node.as.join.left_wire;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &rrf_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_HYBRID_RANK, rrf_node.tag);
    TEST_ASSERT_EQUAL_size_t(2, rrf_node.as.hybrid_rank.count);
}

/* Time-series (Window, AsofJoin), Graph Traverse, and Document Unnest */
static void test_query_ts_graph_and_unnest(void)
{
    uint8_t wire[512];
    cdb_query_writer w;
    size_t root_off;
    const uint8_t *p, *end;
    cdb_query_node win_node, asof_node, trav_node, unnest_node;

    /* 1. Window query */
    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_window(&w, CDB_WINDOW_TUMBLING, "ts", 2, 60000000000ULL, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "sensor_metrics", 14));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    p = wire + root_off;
    end = wire + cdb_query_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &win_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_WINDOW, win_node.tag);
    TEST_ASSERT_EQUAL_UINT8(CDB_WINDOW_TUMBLING, win_node.as.window.type);
    TEST_ASSERT_EQUAL_UINT64(60000000000ULL, win_node.as.window.size_ns);

    /* 2. Traverse query: Traverse(Scan("users"), "follows", FWD, 1, 3) */
    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_traverse(&w, "follows", 7, CDB_TRAVERSE_FWD, 1, 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "users", 5));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    p = wire + root_off;
    end = wire + cdb_query_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &trav_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_TRAVERSE, trav_node.tag);
    TEST_ASSERT_EQUAL_STRING_LEN("follows", trav_node.as.traverse.link_type, 7);
    TEST_ASSERT_EQUAL_UINT64(1, trav_node.as.traverse.min_hops);
    TEST_ASSERT_EQUAL_UINT64(3, trav_node.as.traverse.max_hops);

    /* 3. AsofJoin query */
    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_asof_join(&w, CDB_ASOF_BACKWARD, 1000000000ULL));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "trades", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "quotes", 6));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "trades\x1f" "time", 11));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "quotes\x1f" "time", 11));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_binary(&w, CDB_BIN_EQ));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "trades\x1f" "sym", 10));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "quotes\x1f" "sym", 10));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    p = wire + root_off;
    end = wire + cdb_query_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &asof_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_ASOF_JOIN, asof_node.tag);
    TEST_ASSERT_EQUAL_UINT8(CDB_ASOF_BACKWARD, asof_node.as.asof_join.kind);
    TEST_ASSERT_EQUAL_UINT64(1000000000ULL, asof_node.as.asof_join.tolerance_ns);

    /* 4. Unnest query */
    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_unnest(&w, "item", 4, "idx", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "posts", 5));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_expr_field(&w, "tags", 4));

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_validate(wire, cdb_query_writer_len(&w), &root_off));
    p = wire + root_off;
    end = wire + cdb_query_writer_len(&w);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_read_node(&p, end, &unnest_node));
    TEST_ASSERT_EQUAL_UINT8(CDB_QUERY_NODE_UNNEST, unnest_node.tag);
    TEST_ASSERT_EQUAL_STRING_LEN("item", unnest_node.as.unnest.item_alias, 4);
    TEST_ASSERT_EQUAL_STRING_LEN("idx", unnest_node.as.unnest.index_alias, 3);
}

/* Malformed wire bytes, bad magic, truncated buffers, trailing garbage */
static void test_query_validation_errors(void)
{
    uint8_t wire[64];
    cdb_query_writer w;
    size_t root_off;

    /* NULL wire or too short */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(NULL, 10, &root_off));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(wire, 5, &root_off));

    /* Bad magic */
    memset(wire, 0, sizeof wire);
    wire[0] = 'X';
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(wire, 10, &root_off));

    /* Valid header, but wrong version */
    wire[0] = 'C'; wire[1] = 'M'; wire[2] = 'B'; wire[3] = 'Q';
    wire[4] = 99; /* bad version */
    wire[5] = 0;
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, cdb_query_validate(wire, 10, &root_off));

    /* Valid header, but non-zero flags */
    wire[4] = CDB_QUERY_VERSION_1;
    wire[5] = 0x80;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(wire, 10, &root_off));

    /* Valid header, but truncated node */
    cdb_query_writer_init(&w, wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_writer_begin(&w));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_query_write_scan(&w, "test", 4));
    /* Truncate last byte */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(wire, cdb_query_writer_len(&w) - 1, &root_off));

    /* Trailing garbage after valid node */
    wire[cdb_query_writer_len(&w)] = 0xFF;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_query_validate(wire, cdb_query_writer_len(&w) + 1, &root_off));

    /* Buffer too small for writer */
    cdb_query_writer_init(&w, wire, 4);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_query_writer_begin(&w));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_query_header_and_scan);
    RUN_TEST(test_query_filter_and_project);
    RUN_TEST(test_query_hybrid_and_join);
    RUN_TEST(test_query_ts_graph_and_unnest);
    RUN_TEST(test_query_validation_errors);
    return UNITY_END();
}
