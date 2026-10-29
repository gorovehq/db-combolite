#include <string.h>

#include "unity.h"
#include "cdb_schema.h"

#define O CDB_ORDER_LITTLE
#define T(t) CDB_SCHEMA_TYPE(CDB_TYPE_##t)

static uint8_t sb[1024], db_[1024];
static cdb_value_writer sw, dw;

void setUp(void)
{
    cdb_value_writer_init(&sw, sb, sizeof sb, O, CDB_DECIMAL_COMPACT);
    cdb_value_writer_init(&dw, db_, sizeof db_, O, CDB_DECIMAL_COMPACT);
}

void tearDown(void) {}

/* ------------------------------------------------------------ builders */

/* open < 0 leaves the open flag out. */
static void schema_begin(cdb_value_writer *w, int open)
{
    cdb_value_begin_object(w);
    if (open >= 0) {
        cdb_value_field(w, CDB_SCHEMA_OPEN);
        cdb_value_write_bool(w, open);
    }
    cdb_value_field(w, CDB_SCHEMA_FIELDS);
    cdb_value_begin_array(w);
}

static void schema_end(cdb_value_writer *w)
{
    cdb_value_end_array(w);
    cdb_value_end_object(w);
}

/* Starts a rule; the caller may add fields above 4, then rule_end. */
static void rule_begin(cdb_value_writer *w, uint64_t field, uint64_t types, int required, int64_t max_len)
{
    cdb_value_begin_object(w);
    cdb_value_field(w, CDB_RULE_FIELD);
    cdb_value_write_int(w, (int64_t)field);
    if (types) {
        cdb_value_field(w, CDB_RULE_TYPES);
        cdb_value_write_int(w, (int64_t)types);
    }
    if (required) {
        cdb_value_field(w, CDB_RULE_REQUIRED);
        cdb_value_write_bool(w, 1);
    }
    if (max_len >= 0) {
        cdb_value_field(w, CDB_RULE_MAX_LEN);
        cdb_value_write_int(w, max_len);
    }
}

static void rule_end(cdb_value_writer *w)
{
    cdb_value_end_object(w);
}

static void rule(cdb_value_writer *w, uint64_t field, uint64_t types, int required, int64_t max_len)
{
    rule_begin(w, field, types, required, max_len);
    rule_end(w);
}

static size_t done_schema(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, sw.error);
    return cdb_value_writer_len(&sw);
}

static cdb_status check(int *trivial)
{
    return cdb_schema_check(sb, done_schema(), O, trivial);
}

static cdb_status validate(void)
{
    int trivial;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schema_check(sb, done_schema(), O, &trivial));
    TEST_ASSERT_EQUAL_INT(CDB_OK, dw.error);
    return cdb_schema_validate(sb, cdb_value_writer_len(&sw), db_, cdb_value_writer_len(&dw), O);
}

static void str(cdb_value_writer *w, uint64_t field, const char *s)
{
    cdb_value_field(w, field);
    cdb_value_write_string(w, s, strlen(s));
}

/* ------------------------------------------------------------ check */

static void test_trivial_schemas(void)
{
    int trivial = 0;
    cdb_value_begin_object(&sw);
    cdb_value_end_object(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, check(&trivial));
    TEST_ASSERT_TRUE(trivial);

    setUp();
    schema_begin(&sw, 1);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, check(&trivial));
    TEST_ASSERT_TRUE(trivial);

    setUp();
    schema_begin(&sw, 0); /* closed, no rules: only {} conforms */
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, check(&trivial));
    TEST_ASSERT_FALSE(trivial);

    setUp();
    schema_begin(&sw, -1);
    rule(&sw, 1, 0, 0, -1);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, check(&trivial));
    TEST_ASSERT_FALSE(trivial);
}

static void test_check_rejects_malformed(void)
{
    const uint8_t not_object[] = { CDB_TAG_INT, 2 };
    int trivial;

    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_schema_check(not_object, sizeof not_object, O, &trivial));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_schema_check(NULL, 0, O, &trivial));

    /* open must be a bool */
    cdb_value_begin_object(&sw);
    cdb_value_field(&sw, CDB_SCHEMA_OPEN);
    cdb_value_write_int(&sw, 1);
    cdb_value_end_object(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));

    /* a rule needs its field ID */
    setUp();
    schema_begin(&sw, -1);
    cdb_value_begin_object(&sw);
    cdb_value_field(&sw, CDB_RULE_TYPES);
    cdb_value_write_int(&sw, (int64_t)T(INT));
    cdb_value_end_object(&sw);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));

    /* rules in strictly ascending order */
    setUp();
    schema_begin(&sw, -1);
    rule(&sw, 2, 0, 0, -1);
    rule(&sw, 1, 0, 0, -1);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));
    setUp();
    schema_begin(&sw, -1);
    rule(&sw, 1, 0, 0, -1);
    rule(&sw, 1, 0, 0, -1);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));

    /* type bits beyond the known types */
    setUp();
    schema_begin(&sw, -1);
    rule(&sw, 1, CDB_SCHEMA_TYPES_ALL + 1u, 0, -1);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));

    /* negative max length */
    setUp();
    schema_begin(&sw, -1);
    rule(&sw, 1, 0, 0, -1);
    schema_end(&sw);
    setUp();
    schema_begin(&sw, -1);
    cdb_value_begin_object(&sw);
    cdb_value_field(&sw, CDB_RULE_FIELD);
    cdb_value_write_int(&sw, 1);
    cdb_value_field(&sw, CDB_RULE_MAX_LEN);
    cdb_value_write_int(&sw, -3);
    cdb_value_end_object(&sw);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));

    /* a bad nested schema is found */
    setUp();
    schema_begin(&sw, -1);
    rule_begin(&sw, 1, T(OBJECT), 0, -1);
    cdb_value_field(&sw, CDB_RULE_OBJECT);
    cdb_value_begin_object(&sw);
    cdb_value_field(&sw, CDB_SCHEMA_OPEN);
    cdb_value_write_int(&sw, 0);
    cdb_value_end_object(&sw);
    rule_end(&sw);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, check(&trivial));
}

static void test_check_refuses_unknown_fields(void)
{
    int trivial;
    cdb_value_begin_object(&sw);
    cdb_value_field(&sw, 9);
    cdb_value_write_int(&sw, 1);
    cdb_value_end_object(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, check(&trivial));

    setUp();
    schema_begin(&sw, -1);
    rule_begin(&sw, 1, 0, 0, -1);
    cdb_value_field(&sw, 40);
    cdb_value_write_int(&sw, 1);
    rule_end(&sw);
    schema_end(&sw);
    TEST_ASSERT_EQUAL_INT(CDB_E_UNSUPPORTED, check(&trivial));
}

/* ------------------------------------------------------------ validate */

/* { 1: string, required, at most 8 bytes;  2: int or null;  4: bool } */
static void basic_schema(int open)
{
    schema_begin(&sw, open);
    rule(&sw, 1, T(STRING), 1, 8);
    rule(&sw, 2, T(INT) | T(NULL), 0, -1);
    rule(&sw, 4, T(BOOL), 0, -1);
    schema_end(&sw);
}

static void test_required_types_and_lengths(void)
{
    basic_schema(1);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "title");
    cdb_value_field(&dw, 2);
    cdb_value_write_null(&dw);
    cdb_value_field(&dw, 4);
    cdb_value_write_bool(&dw, 0);
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, validate());

    setUp();
    basic_schema(1);
    cdb_value_begin_object(&dw); /* field 1 missing */
    cdb_value_field(&dw, 2);
    cdb_value_write_int(&dw, 3);
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    basic_schema(1);
    cdb_value_begin_object(&dw); /* an empty document lacks field 1 too */
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    basic_schema(1);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "t");
    str(&dw, 2, "not an int");
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    basic_schema(1);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "nine char");
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    basic_schema(1);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "eight ch"); /* exactly the limit */
    cdb_value_field(&dw, 4);
    cdb_value_write_bool(&dw, 1);
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, validate());

    setUp();
    basic_schema(1);
    {
        const uint8_t not_object[] = { CDB_TAG_INT, 2 };
        int trivial;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schema_check(sb, done_schema(), O, &trivial));
        TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, cdb_schema_validate(sb, done_schema(), not_object, sizeof not_object, O));
    }
}

static void test_open_and_closed(void)
{
    basic_schema(1);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "t");
    str(&dw, 3, "unlisted"); /* between rules */
    str(&dw, 9, "unlisted"); /* after every rule */
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, validate());

    setUp();
    basic_schema(0);
    cdb_value_begin_object(&dw);
    str(&dw, 1, "t");
    str(&dw, 3, "unlisted");
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    basic_schema(0);
    cdb_value_begin_object(&dw);
    str(&dw, 0, "before every rule");
    str(&dw, 1, "t");
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());
}

/* { 3: object { 1: string, required }, 5: array of at most 2 objects { 1: int, required } } */
static void nested_schema(void)
{
    schema_begin(&sw, -1);
    rule_begin(&sw, 3, T(OBJECT), 0, -1);
    cdb_value_field(&sw, CDB_RULE_OBJECT);
    schema_begin(&sw, 0);
    rule(&sw, 1, T(STRING), 1, -1);
    schema_end(&sw);
    rule_end(&sw);
    rule_begin(&sw, 5, T(ARRAY), 0, 2);
    cdb_value_field(&sw, CDB_RULE_ELEM_TYPES);
    cdb_value_write_int(&sw, (int64_t)T(OBJECT));
    cdb_value_field(&sw, CDB_RULE_ELEM_OBJECT);
    schema_begin(&sw, -1);
    rule(&sw, 1, T(INT), 1, -1);
    schema_end(&sw);
    rule_end(&sw);
    schema_end(&sw);
}

static void owner(cdb_value_writer *w, const char *name)
{
    cdb_value_field(w, 3);
    cdb_value_begin_object(w);
    if (name)
        str(w, 1, name);
    cdb_value_end_object(w);
}

static void items(cdb_value_writer *w, int n, int bad)
{
    int i;
    cdb_value_field(w, 5);
    cdb_value_begin_array(w);
    for (i = 0; i < n; i++) {
        cdb_value_begin_object(w);
        if (bad && i == n - 1)
            str(w, 1, "x");
        else {
            cdb_value_field(w, 1);
            cdb_value_write_int(w, i);
        }
        cdb_value_end_object(w);
    }
    cdb_value_end_array(w);
}

static void test_nested_objects_and_arrays(void)
{
    nested_schema();
    cdb_value_begin_object(&dw);
    owner(&dw, "casey");
    items(&dw, 2, 0);
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_OK, validate());

    setUp();
    nested_schema();
    cdb_value_begin_object(&dw);
    owner(&dw, NULL); /* the nested object lacks its required field */
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    nested_schema();
    cdb_value_begin_object(&dw);
    items(&dw, 3, 0); /* too many elements */
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    nested_schema();
    cdb_value_begin_object(&dw);
    items(&dw, 2, 1); /* an element breaks its schema */
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());

    setUp();
    nested_schema();
    cdb_value_begin_object(&dw);
    cdb_value_field(&dw, 5);
    cdb_value_begin_array(&dw);
    cdb_value_write_int(&dw, 1); /* an element of a type not allowed */
    cdb_value_end_array(&dw);
    cdb_value_end_object(&dw);
    TEST_ASSERT_EQUAL_INT(CDB_E_SCHEMA, validate());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trivial_schemas);
    RUN_TEST(test_check_rejects_malformed);
    RUN_TEST(test_check_refuses_unknown_fields);
    RUN_TEST(test_required_types_and_lengths);
    RUN_TEST(test_open_and_closed);
    RUN_TEST(test_nested_objects_and_arrays);
    return UNITY_END();
}
