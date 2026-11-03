#include "unity.h"
#include "cdb_trigger.h"
#include "cdb_db.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_trigger_register_and_unregister(void)
{
    cdb_trigger_registry reg;
    cdb_trigger_registry_init(&reg, NULL, 50);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_register(&reg, "trg_books", 9, 101, CDB_TRIGGER_AFTER_INSERT, "audit_fn", 8, 8));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_register(&reg, "trg_users", 9, 102, CDB_TRIGGER_AFTER_UPDATE, "notify_fn", 9, 5));

    /* Duplicate registration */
    TEST_ASSERT_EQUAL_INT(CDB_E_EXISTS, cdb_trigger_register(&reg, "trg_books", 9, 101, CDB_TRIGGER_AFTER_INSERT, "other_fn", 8, 8));

    /* Unregister */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_unregister(&reg, "trg_books", 9));
    TEST_ASSERT_EQUAL_INT(CDB_E_NOT_FOUND, cdb_trigger_unregister(&reg, "trg_books", 9));
    TEST_ASSERT_EQUAL_UINT(1, reg.n_triggers);
}

static void test_trigger_depth_limit_protection(void)
{
    cdb_trigger_registry reg;
    cdb_trigger_registry_init(&reg, NULL, 100);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_register(&reg, "deep_trg", 8, 201, CDB_TRIGGER_AFTER_INSERT, "proc_step", 9, 8));

    /* Depths within budget */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_dispatch(&reg, NULL, 201, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_dispatch(&reg, NULL, 201, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 7));

    /* Depth exceeding limit (8) fails with CDB_E_OUT_OF_BUDGET */
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, cdb_trigger_dispatch(&reg, NULL, 201, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 8));
    TEST_ASSERT_EQUAL_INT(CDB_E_OUT_OF_BUDGET, cdb_trigger_dispatch(&reg, NULL, 201, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 10));
}

static void test_trigger_cascade_warning(void)
{
    cdb_trigger_registry reg;
    cdb_trigger_registry_init(&reg, NULL, 5); /* threshold = 5 */

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_register(&reg, "casc_trg", 8, 301, CDB_TRIGGER_AFTER_INSERT, "log_fn", 6, 8));

    TEST_ASSERT_EQUAL_INT(0, reg.cascade_warning_issued);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_dispatch(&reg, NULL, 301, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 0));
    }
    TEST_ASSERT_EQUAL_INT(0, reg.cascade_warning_issued);

    /* 5th dispatch hits threshold */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_trigger_dispatch(&reg, NULL, 301, CDB_TRIGGER_AFTER_INSERT, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(1, reg.cascade_warning_issued);
}

static void dummy_maintenance_task(cdb_db *db, void *arg)
{
    (void)db;
    int *counter = (int *)arg;
    (*counter)++;
}

static void test_scheduled_maintenance_tick(void)
{
    cdb_trigger_registry reg;
    int task_runs = 0;
    cdb_trigger_registry_init(&reg, NULL, 100);

    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schedule_register(&reg, "maint_cleanup", 13, 1000, dummy_maintenance_task, &task_runs));

    /* First tick at t=500: interval is 1000, not yet elapsed */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schedule_tick(&reg, NULL, 500));
    TEST_ASSERT_EQUAL_INT(0, task_runs);

    /* Tick at t=1200: 1200 - 0 >= 1000, runs */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schedule_tick(&reg, NULL, 1200));
    TEST_ASSERT_EQUAL_INT(1, task_runs);

    /* Tick at t=1600: 1600 - 1200 = 400 < 1000, not elapsed */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schedule_tick(&reg, NULL, 1600));
    TEST_ASSERT_EQUAL_INT(1, task_runs);

    /* Tick at t=2300: 2300 - 1200 = 1100 >= 1000, runs again */
    TEST_ASSERT_EQUAL_INT(CDB_OK, cdb_schedule_tick(&reg, NULL, 2300));
    TEST_ASSERT_EQUAL_INT(2, task_runs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trigger_register_and_unregister);
    RUN_TEST(test_trigger_depth_limit_protection);
    RUN_TEST(test_trigger_cascade_warning);
    RUN_TEST(test_scheduled_maintenance_tick);
    return UNITY_END();
}
