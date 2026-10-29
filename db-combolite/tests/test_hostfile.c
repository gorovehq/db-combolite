#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "combolite/cdb_storage.h"

static char path[512];

void setUp(void)
{
    snprintf(path, sizeof path, "%s/hostfile_%ld.cdb", CDB_TEST_TMPDIR, (long)getpid());
    unlink(path);
}

void tearDown(void)
{
    unlink(path);
}

static void test_open_missing_without_create(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_hostfile_open(&hf, path, 0, &st));
}

static void test_write_read_and_size(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    uint8_t out[16];
    uint64_t size;

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_hostfile_open(&hf, path, CDB_HOSTFILE_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 1024, "combolite", 9));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_size(&st, &size));
    TEST_ASSERT_EQUAL_UINT64(1033, size);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, 1024, out, 9));
    TEST_ASSERT_EQUAL_MEMORY("combolite", out, 9);
    TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_storage_read(&st, 1030, out, 16));
    cdb_storage_close(&st);
}

static void test_new_file_is_private(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    struct stat sb;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_hostfile_open(&hf, path, CDB_HOSTFILE_CREATE, &st));
    cdb_storage_close(&st);
    TEST_ASSERT_EQUAL_INT(0, stat(path, &sb));
    TEST_ASSERT_EQUAL_INT(0, (int)(sb.st_mode & (S_IRWXG | S_IRWXO)));
}

static void test_data_persists_across_reopen(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    uint8_t out[4];

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_hostfile_open(&hf, path, CDB_HOSTFILE_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "abcd", 4));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    cdb_storage_close(&st);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_hostfile_open(&hf, path, CDB_HOSTFILE_READONLY, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, 0, out, 4));
    TEST_ASSERT_EQUAL_MEMORY("abcd", out, 4);
    TEST_ASSERT_EQUAL_INT(CDB_E_READ_ONLY, cdb_storage_program(&st, 0, "x", 1));
    cdb_storage_close(&st);
}

static void test_geometry(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    cdb_storage_geometry g;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_hostfile_open(&hf, path, CDB_HOSTFILE_CREATE, &st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_geometry_of(&st, &g));
    TEST_ASSERT_EQUAL_UINT32(0, g.erase_block_size);
    TEST_ASSERT_EQUAL_UINT32(1, g.program_unit);
    TEST_ASSERT_EQUAL_UINT64(0, g.capacity);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_erase(&st, 0, 4096)); /* no-op on host files */
    cdb_storage_close(&st);
}

static void test_bad_flag_combination(void)
{
    cdb_hostfile hf;
    cdb_storage st;
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG,
                          cdb_hostfile_open(&hf, path, CDB_HOSTFILE_CREATE | CDB_HOSTFILE_READONLY, &st));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_missing_without_create);
    RUN_TEST(test_write_read_and_size);
    RUN_TEST(test_new_file_is_private);
    RUN_TEST(test_data_persists_across_reopen);
    RUN_TEST(test_geometry);
    RUN_TEST(test_bad_flag_combination);
    return UNITY_END();
}
