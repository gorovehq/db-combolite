#include "unity.h"
#include "cdb_compiler.h"
#include "cdb_query.h"
#include "cdb_proc.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_compile_declarative_sql(void) {
    const char *q = "SELECT title, score FROM Scan(\"books\") WHERE score >= 0.8 SORT BY score DESC LIMIT 10;";
    uint8_t wire[2048];
    size_t wire_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_query(q, strlen(q), NULL, wire, sizeof(wire), &wire_len, &err);
    TEST_ASSERT_EQUAL_MESSAGE(CDB_OK, s, err.msg);
    TEST_ASSERT_TRUE(wire_len > CDB_QUERY_HEADER_SIZE);

    /* Validate structural integrity */
    size_t root_off = 0;
    TEST_ASSERT_EQUAL(CDB_OK, cdb_query_validate(wire, wire_len, &root_off));
}

static void test_compile_pipeline_flux(void) {
    const char *q = "Scan(\"telemetry\") | filter(temp > 85.0 AND voltage > 3.3) | window(field: \"ts\", size: 60000) | sort(temp DESC) | limit(25);";
    uint8_t wire[2048];
    size_t wire_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_query(q, strlen(q), NULL, wire, sizeof(wire), &wire_len, &err);
    TEST_ASSERT_EQUAL_MESSAGE(CDB_OK, s, err.msg);
    TEST_ASSERT_TRUE(wire_len > CDB_QUERY_HEADER_SIZE);

    size_t root_off = 0;
    TEST_ASSERT_EQUAL(CDB_OK, cdb_query_validate(wire, wire_len, &root_off));
}

static void test_compile_dotted_resolution(void) {
    /* Quoted dotted identifier preserves literal dot (Tier 1) */
    const char *q1 = "Scan(\"telemetry\") | filter(\"sensor.temp.celsius\" > 50);";
    uint8_t wire1[1024];
    size_t len1 = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    TEST_ASSERT_EQUAL(CDB_OK, cdb_compile_query(q1, strlen(q1), NULL, wire1, sizeof(wire1), &len1, &err));

    /* Search for literal dot in wire1 */
    const char *needle1 = "sensor.temp.celsius";
    int found_dot = 0;
    for (size_t i = 0; i + strlen(needle1) <= len1; ++i) {
        if (memcmp(&wire1[i], needle1, strlen(needle1)) == 0) {
            found_dot = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_dot);

    /* Unquoted dotted identifier compiles to 0x1F Unit Separator (Tier 4 schemaless fallback) */
    const char *q2 = "Scan(\"profiles\") | filter(user.address.city == \"Tokyo\");";
    uint8_t wire2[1024];
    size_t len2 = 0;
    TEST_ASSERT_EQUAL(CDB_OK, cdb_compile_query(q2, strlen(q2), NULL, wire2, sizeof(wire2), &len2, &err));

    const char *needle2 = "user\x1F" "address\x1F" "city";
    int found_sep = 0;
    for (size_t i = 0; i + strlen(needle2) <= len2; ++i) {
        if (memcmp(&wire2[i], needle2, strlen(needle2)) == 0) {
            found_sep = 1;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_sep);
}

static void test_compile_math_and_dsp_calls(void) {
    const char *q = "Scan(\"signals\") | filter(sin(phase) > 0.5 AND haversine(lat1, lon1, lat2, lon2) < 500.0);";
    uint8_t wire[2048];
    size_t wire_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_query(q, strlen(q), NULL, wire, sizeof(wire), &wire_len, &err);
    TEST_ASSERT_EQUAL_MESSAGE(CDB_OK, s, err.msg);

    size_t root_off = 0;
    TEST_ASSERT_EQUAL(CDB_OK, cdb_query_validate(wire, wire_len, &root_off));
}

static void test_compile_stored_procedure(void) {
    const char *script =
        "PROCEDURE test_proc($init_val INT)\n"
        "BEGIN\n"
        "    LET $total = 42;\n"
        "    LET $status = \"ready\";\n"
        "    RETURN $total;\n"
        "END;";

    uint8_t bc[1024];
    size_t bc_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_proc(script, strlen(script), NULL, bc, sizeof(bc), &bc_len, &err);
    TEST_ASSERT_EQUAL_MESSAGE(CDB_OK, s, err.msg);
    TEST_ASSERT_TRUE(bc_len >= CDB_PROC_HEADER_SIZE);

    /* Verify execution in VM */
    cdb_proc_vm vm;
    cdb_proc_vm_init(&vm, NULL, CDB_ORDER_LITTLE, NULL, 0, 1000);
    cdb_value ret;
    memset(&ret, 0, sizeof(ret));
    TEST_ASSERT_EQUAL(CDB_OK, cdb_proc_exec(&vm, bc, bc_len, &ret));
    TEST_ASSERT_EQUAL(CDB_TYPE_INT, ret.type);
    TEST_ASSERT_EQUAL_INT64(42, ret.as.i64);
}

static void test_compile_error_reporting(void) {
    const char *bad_sql = "SELECT title FROM WHERE score > 0;";
    uint8_t wire[512];
    size_t wire_len = 0;
    cdb_compile_error err;
    memset(&err, 0, sizeof(err));

    cdb_status s = cdb_compile_query(bad_sql, strlen(bad_sql), NULL, wire, sizeof(wire), &wire_len, &err);
    TEST_ASSERT_EQUAL(CDB_E_INVALID_ARG, s);
    TEST_ASSERT_TRUE(err.line > 0);
    TEST_ASSERT_TRUE(strlen(err.msg) > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_compile_declarative_sql);
    RUN_TEST(test_compile_pipeline_flux);
    RUN_TEST(test_compile_dotted_resolution);
    RUN_TEST(test_compile_math_and_dsp_calls);
    RUN_TEST(test_compile_stored_procedure);
    RUN_TEST(test_compile_error_reporting);
    return UNITY_END();
}
