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

/* The lock hook (spec 5.8): exclusive conflicts with anything, shared with
 * shared does not, closing releases, and taking a lock creates the file. */
static void test_locks_conflict_and_release(void)
{
    cdb_lock *a, *b, *c;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 1, &a));
    TEST_ASSERT_TRUE(exists("l"));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, env.ops->lock_open(env.ctx, "l", 1, &b));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, env.ops->lock_open(env.ctx, "l", 0, &b));
    env.ops->lock_close(env.ctx, a);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 0, &b));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 0, &c));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, env.ops->lock_open(env.ctx, "l", 1, &a));
    TEST_ASSERT_EQUAL_UINT(2, cdb_faultenv_held_locks(&fe));
    env.ops->lock_close(env.ctx, b);
    env.ops->lock_close(env.ctx, c);
    TEST_ASSERT_EQUAL_UINT(0, cdb_faultenv_held_locks(&fe));
}

/* A lock belongs to the file: a rename keeps it, and a file created at the
 * path of a removed one is not locked. */
static void test_lock_follows_the_file(void)
{
    cdb_lock *a, *b;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 1, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->rename(env.ctx, "l", "m"));
    TEST_ASSERT_EQUAL_INT(CDB_E_BUSY, env.ops->lock_open(env.ctx, "m", 0, &b));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->remove(env.ctx, "m"));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "m", 1, &b));
    env.ops->lock_close(env.ctx, b);
    env.ops->lock_close(env.ctx, a);
}

/* A reboot ends every process, so every lock is released; taking one is no
 * crash point, but creating its file is. */
static void test_reboot_releases_locks(void)
{
    cdb_lock *a;
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 1, &a));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->sync_dir(env.ctx, "."));
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_EQUAL_UINT(0, cdb_faultenv_held_locks(&fe));
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 1, &a));
    env.ops->lock_close(env.ctx, a);

    cdb_faultenv_arm(&fe, 1, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, env.ops->lock_open(env.ctx, "l", 1, &a)); /* exists: no op */
    env.ops->lock_close(env.ctx, a);
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, env.ops->lock_open(env.ctx, "n", 1, &a)); /* the create */
    cdb_faultenv_reboot(&fe);
    TEST_ASSERT_FALSE(exists("n"));
}

static void test_without_locks(void)
{
    cdb_env e = env;
    cdb_faultenv_without_locks(&e);
    TEST_ASSERT_NULL(e.ops->lock_open);
    TEST_ASSERT_NULL(e.ops->lock_close);
    TEST_ASSERT_TRUE(e.ops->open == env.ops->open);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_needs_dir_sync_to_survive);
    RUN_TEST(test_files_vanish_with_undurable_dir);
    RUN_TEST(test_remove_and_rename_roll_back);
    RUN_TEST(test_shared_clock_crashes_every_file);
    RUN_TEST(test_clean_reboot_keeps_everything);
    RUN_TEST(test_locks_conflict_and_release);
    RUN_TEST(test_lock_follows_the_file);
    RUN_TEST(test_reboot_releases_locks);
    RUN_TEST(test_without_locks);
    return UNITY_END();
}
