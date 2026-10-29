#include <math.h>
#include <string.h>

#include "unity.h"
#include "cdb_key.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct enc { uint8_t b[64]; size_t n; } enc;

static enc k_int(int64_t v) { enc e; cdb_key_builder k; cdb_key_init(&k, e.b, sizeof e.b); cdb_key_int(&k, v); e.n = k.len; return e; }
static enc k_f64(double v) { enc e; cdb_key_builder k; cdb_key_init(&k, e.b, sizeof e.b); cdb_key_f64(&k, v); e.n = k.len; return e; }
static enc k_str(const char *s, size_t n) { enc e; cdb_key_builder k; cdb_key_init(&k, e.b, sizeof e.b); cdb_key_string(&k, s, n); e.n = k.len; return e; }

static int cmp(const enc *a, const enc *b) { return cdb_key_compare(a->b, a->n, b->b, b->n); }

static void test_integers_sort_numerically(void)
{
    const int64_t v[] = { INT64_MIN, -1000000, -256, -1, 0, 1, 255, 256, 65536, INT64_MAX };
    size_t i;
    for (i = 0; i + 1 < sizeof v / sizeof v[0]; i++) {
        enc a = k_int(v[i]), b = k_int(v[i + 1]);
        TEST_ASSERT_EQUAL_INT(-1, cmp(&a, &b));
    }
}

static void test_floats_sort_numerically(void)
{
    const double v[] = { -INFINITY, -1e300, -2.5, -1e-300, 0.0, 1e-300, 1.0, 2.5, 1e300, INFINITY, NAN };
    size_t i;
    for (i = 0; i + 1 < sizeof v / sizeof v[0]; i++) {
        enc a = k_f64(v[i]), b = k_f64(v[i + 1]);
        TEST_ASSERT_EQUAL_INT(-1, cmp(&a, &b));
    }
    {
        enc nz = k_f64(-0.0), pz = k_f64(0.0);
        TEST_ASSERT_EQUAL_INT(0, cmp(&nz, &pz));
    }
}

static void test_strings_sort_bytewise_with_prefixes_first(void)
{
    enc a = k_str("", 0), b = k_str("\0", 1), c = k_str("a", 1), d = k_str("a\0", 2),
        e = k_str("ab", 2), f = k_str("b", 1);
    TEST_ASSERT_EQUAL_INT(-1, cmp(&a, &b));
    TEST_ASSERT_EQUAL_INT(-1, cmp(&b, &c));
    TEST_ASSERT_EQUAL_INT(-1, cmp(&c, &d));
    TEST_ASSERT_EQUAL_INT(-1, cmp(&d, &e));
    TEST_ASSERT_EQUAL_INT(-1, cmp(&e, &f));
}

static void test_composite_keys(void)
{
    uint8_t x[64], y[64], z[64];
    cdb_key_builder a, b, c;
    cdb_key_init(&a, x, sizeof x);
    cdb_key_init(&b, y, sizeof y);
    cdb_key_init(&c, z, sizeof z);
    cdb_key_string(&a, "ADR", 3); cdb_key_int(&a, 5);
    cdb_key_string(&b, "ADR", 3); cdb_key_int(&b, 6);
    cdb_key_string(&c, "ADRS", 4); cdb_key_int(&c, 0);
    TEST_ASSERT_EQUAL_INT(-1, cdb_key_compare(x, a.len, y, b.len));
    TEST_ASSERT_EQUAL_INT(-1, cdb_key_compare(y, b.len, z, c.len));
}

static void test_type_ranks(void)
{
    uint8_t buf[9][32];
    size_t len[9];
    cdb_key_builder k;
    int i;
#define ONE(i, call) cdb_key_init(&k, buf[i], 32); call; len[i] = k.len
    ONE(0, cdb_key_null(&k));
    ONE(1, cdb_key_bool(&k, 0));
    ONE(2, cdb_key_bool(&k, 1));
    ONE(3, cdb_key_int(&k, INT64_MAX));
    ONE(4, cdb_key_f64(&k, -INFINITY));
    ONE(5, cdb_key_timestamp(&k, INT64_MIN));
    ONE(6, cdb_key_string(&k, "", 0));
    ONE(7, cdb_key_binary(&k, "", 0));
    ONE(8, cdb_key_ref(&k, 0, 0));
#undef ONE
    for (i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_INT(-1, cdb_key_compare(buf[i], len[i], buf[i + 1], len[i + 1]));
}

static void test_buffer_too_small_is_sticky(void)
{
    uint8_t b[4];
    cdb_key_builder k;
    cdb_key_init(&k, b, sizeof b);
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_key_int(&k, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUFFER_TOO_SMALL, cdb_key_null(&k));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_integers_sort_numerically);
    RUN_TEST(test_floats_sort_numerically);
    RUN_TEST(test_strings_sort_bytewise_with_prefixes_first);
    RUN_TEST(test_composite_keys);
    RUN_TEST(test_type_ranks);
    RUN_TEST(test_buffer_too_small_is_sticky);
    return UNITY_END();
}
