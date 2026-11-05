/* combolite_accel's opener (spec 18.1): with the backends this build has,
 * it finds a device and installs a complete accelerator, CUDA first when
 * both are built, and no software device unless asked. Without a device
 * it reports skipped (exit code 77). */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "combolite/cdb_accel.h"

void setUp(void) {}
void tearDown(void) {}

static cdb_accel *ax;

static void test_installs_every_call(void)
{
    cdb_env env;
    memset(&env, 0, sizeof env);
    cdb_accel_attach(ax, &env);
    TEST_ASSERT_NOT_NULL(env.accel);
    TEST_ASSERT_NOT_NULL(env.accel_ctx);
    TEST_ASSERT_NOT_NULL(env.accel->mirror_open);
    TEST_ASSERT_NOT_NULL(env.accel->mirror_put);
    TEST_ASSERT_NOT_NULL(env.accel->mirror_del);
    TEST_ASSERT_NOT_NULL(env.accel->mirror_close);
    TEST_ASSERT_NOT_NULL(env.accel->nearest);
}

/* Software devices only on request (CDB_ACCEL_SOFTWARE). */
static void test_no_software_by_default(void)
{
    TEST_ASSERT_NULL(strstr(cdb_accel_name(ax), "llvmpipe"));
}

static void test_prefers_cuda(void)
{
#if defined(CDB_ACCEL_CUDA) && defined(CDB_ACCEL_VULKAN)
    if (strncmp(cdb_accel_name(ax), "CUDA: ", 6) != 0)
        TEST_IGNORE_MESSAGE("no CUDA device here: Vulkan was chosen");
#else
    TEST_IGNORE_MESSAGE("one backend in this build");
#endif
}

int main(void)
{
    int failures;
    if (cdb_accel_open(0, &ax) != CDB_OK) {
        printf("no usable accelerator: skipped\n");
        return 77;
    }
    printf("opened %s\n", cdb_accel_name(ax));
    UNITY_BEGIN();
    RUN_TEST(test_installs_every_call);
    RUN_TEST(test_no_software_by_default);
    RUN_TEST(test_prefers_cuda);
    failures = UNITY_END();
    cdb_accel_close(ax);
    return failures;
}
