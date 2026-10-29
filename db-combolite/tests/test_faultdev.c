#include <string.h>

#include "unity.h"
#include "cdb_faultdev.h"

#define CAP 4096

static uint8_t current[CAP], durable[CAP], log_data[CAP];
static cdb_fault_record log_records[64];
static cdb_faultdev dev;
static cdb_storage st;

static void init_device(unsigned flags, uint32_t erase_block)
{
    cdb_faultdev_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.current = current;
    cfg.durable = durable;
    cfg.capacity = CAP;
    cfg.log = log_records;
    cfg.log_cap = 64;
    cfg.log_data = log_data;
    cfg.log_data_cap = CAP;
    cfg.geometry.erase_block_size = erase_block;
    cfg.geometry.program_unit = 1;
    cfg.geometry.capacity = CAP;
    cfg.flags = flags;
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_init(&dev, &cfg, &st));
}

void setUp(void)
{
    init_device(0, 0);
}

void tearDown(void) {}

static void read_expect(uint64_t offset, const char *expected, size_t len)
{
    uint8_t out[64];
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, offset, out, len));
    TEST_ASSERT_EQUAL_MEMORY(expected, out, len);
}

static void test_synced_data_survives_crash(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "durable", 7));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "LOSTLOS", 7));
    cdb_faultdev_crash_now(&dev, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_read(&st, 0, NULL, 0));
    cdb_faultdev_reboot(&dev);
    read_expect(0, "durable", 7);
}

static void test_clean_reboot_keeps_everything(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "kept", 4));
    cdb_faultdev_reboot(&dev);
    read_expect(0, "kept", 4);
}

static void test_armed_crash_strikes_nth_op(void)
{
    uint64_t size;
    cdb_faultdev_arm(&dev, 3, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "a", 1));   /* op 1 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));                 /* op 2 */
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_program(&st, 1, "b", 1)); /* op 3: crash */
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_sync(&st));               /* dead after crash */
    cdb_faultdev_reboot(&dev);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_size(&st, &size));
    TEST_ASSERT_EQUAL_UINT64(1, size);
    read_expect(0, "a", 1);
}

static void test_crash_during_sync_loses_unsynced(void)
{
    cdb_faultdev_arm(&dev, 2, CDB_CRASH_DROP_UNSYNCED, 0, 0);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "x", 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_sync(&st));
    cdb_faultdev_reboot(&dev);
    {
        uint64_t size;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_size(&st, &size));
        TEST_ASSERT_EQUAL_UINT64(0, size);
    }
}

static void test_keep_prefix(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 3, "two", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 6, "six", 3));
    cdb_faultdev_crash_now(&dev, CDB_CRASH_KEEP_PREFIX, 2, 0);
    cdb_faultdev_reboot(&dev);
    read_expect(0, "onetwo", 6);
    {
        uint8_t out[3];
        TEST_ASSERT_EQUAL_INT(CDB_E_TRUNCATED, cdb_storage_read(&st, 6, out, 3));
    }
}

static void test_torn_write(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "AAAAAAAA", 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    /* The in-flight write is struck by the crash and torn after 3 bytes. */
    cdb_faultdev_arm(&dev, 1, CDB_CRASH_TORN, 0, 3);
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_program(&st, 0, "BBBBBBBB", 8));
    cdb_faultdev_reboot(&dev);
    read_expect(0, "BBBAAAAA", 8);
}

static void test_reorder_keeps_only_latest(void)
{
    uint8_t out[3];
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "one", 3));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 3, "two", 3));
    cdb_faultdev_crash_now(&dev, CDB_CRASH_REORDER, 0, 0);
    cdb_faultdev_reboot(&dev);
    read_expect(3, "two", 3);
    {
        uint64_t size;
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_size(&st, &size));
        TEST_ASSERT_EQUAL_UINT64(6, size);
    }
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_read(&st, 0, out, 3));
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]); /* the earlier write never landed */
}

static void test_flash_rules(void)
{
    const uint8_t zero = 0x00, ff = 0xFF;
    init_device(CDB_FAULTDEV_FLASH, 512);
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 10, &zero, 1));
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_storage_program(&st, 10, &ff, 1)); /* needs erase */
    TEST_ASSERT_EQUAL_INT(CDB_E_INVALID_ARG, cdb_storage_erase(&st, 10, 512));      /* unaligned */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_erase(&st, 0, 512));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 10, &ff, 1));
}

static void test_bit_flip(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "A", 1)); /* 0x41 */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_faultdev_flip_bit(&dev, 0, 1));
    read_expect(0, "C", 1); /* 0x43 */
}

static void test_counts_ops_for_crash_enumeration(void)
{
    /* A harness first counts a workload's mutating ops, then crashes at each one. */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 0, "a", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_program(&st, 1, "b", 1));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_storage_sync(&st));
    TEST_ASSERT_EQUAL_UINT64(3, dev.mutating_ops);
}

static void test_device_full(void)
{
    TEST_ASSERT_EQUAL_INT(CDB_E_IO, cdb_storage_program(&st, CAP - 1, "ab", 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_synced_data_survives_crash);
    RUN_TEST(test_clean_reboot_keeps_everything);
    RUN_TEST(test_armed_crash_strikes_nth_op);
    RUN_TEST(test_crash_during_sync_loses_unsynced);
    RUN_TEST(test_keep_prefix);
    RUN_TEST(test_torn_write);
    RUN_TEST(test_reorder_keeps_only_latest);
    RUN_TEST(test_flash_rules);
    RUN_TEST(test_bit_flip);
    RUN_TEST(test_counts_ops_for_crash_enumeration);
    RUN_TEST(test_device_full);
    return UNITY_END();
}
