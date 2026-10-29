#include <string.h>

#include "unity.h"
#include "cdb_faultenv.h"

#define FCAP 4096u

static uint8_t arena[CDB_FAULTENV_MAX_FILES * 3u * FCAP];
static cdb_fault_record records[CDB_FAULTENV_MAX_FILES * 64u];
static cdb_faultenv fe;
static cdb_env env;

void setUp(void)
{
    cdb_faultenv_config c;
    c.arena = arena;
    c.file_capacity = FCAP;
    c.records = records;
    c.records_per_file = 64;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultenv_init(&fe, &c, &env));
}

void tearDown(void) {}

static int exists(const char *path)
{
    int e = -1;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->exists(env.ctx, path, &e));
    return e;
}

static void test_create_needs_dir_sync_to_survive(void)
{
    cdb_storage st;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "a", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "x", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st)); /* content durable, entry not */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_FALSE(exists("a"));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "a", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "."));
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_TRUE(exists("a"));
}

static void test_files_vanish_with_undurable_dir(void)
{
    cdb_storage st;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->mkdir(env.ctx, "w"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "w/j", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "w")); /* entry durable in w ... */
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);     /* ... but w itself is not */
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_FALSE(exists("w"));
    TEST_ASSERT_FALSE(exists("w/j"));
}

static void test_remove_and_rename_roll_back(void)
{
    cdb_storage st;
    uint8_t b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "old", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "1", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "tmp", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "2", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "."));

    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->rename(env.ctx, "tmp", "old")); /* replaces old */
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "old", 0, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, 0, &b, 1));
    TEST_ASSERT_EQUAL_HEX8('2', b);
    cdb_faultenv_crash_now(&fe, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_TRUE(exists("tmp")); /* the rename was not durable */
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "old", 0, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, 0, &b, 1));
    TEST_ASSERT_EQUAL_HEX8('1', b);
}

static void test_shared_clock_crashes_every_file(void)
{
    cdb_storage a, b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "a", CDB_ENV_CREATE, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "b", CDB_ENV_CREATE, &b));
    cdb_faultenv_arm(&fe, 2, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&a, 0, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_program(&b, 0, "b", 1)); /* op 2 strikes on b */
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_sync(&a));             /* a is dead too */
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, env.ops->sync_dir(env.ctx, "."));
}

static void test_clean_reboot_keeps_everything(void)
{
    cdb_storage st;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->open(env.ctx, "a", CDB_ENV_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "k", 1));
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_TRUE(exists("a"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_needs_dir_sync_to_survive);
    RUN_TEST(test_files_vanish_with_undurable_dir);
    RUN_TEST(test_remove_and_rename_roll_back);
    RUN_TEST(test_shared_clock_crashes_every_file);
    RUN_TEST(test_clean_reboot_keeps_everything);
    return UNITY_END();
}
