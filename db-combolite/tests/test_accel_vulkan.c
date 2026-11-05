/* The Vulkan accelerator on real devices (spec 18.11), with the tests of
 * accel_device.h, on every usable Vulkan device in turn: a discrete GPU,
 * an integrated one and a software device (llvmpipe) each round
 * differently. Software devices are opened with CDB_ACCEL_SOFTWARE, after
 * checking that the default open turns them down. Without a hardware
 * device the test reports skipped (exit code 77), not passed, though a
 * software device's failures still fail it.
 * CDB_TEST_VULKAN_DEVICE=<n> limits the run to device n. */
#include "accel_device.h"

#include "combolite/cdb_accel_vulkan.h"

int main(void)
{
    const char *only = getenv("CDB_TEST_VULKAN_DEVICE");
    int n = cdb_accel_vulkan_devices(), i, hardware = 0, failures;
    UNITY_BEGIN();
    for (i = 0; i < n; i++) {
        cdb_accel_vulkan *vk;
        int software = 0;
        if (only != NULL && atoi(only) != i)
            continue;
        if (cdb_accel_vulkan_open(i, 0, &vk) != CDB_OK) {
            if (cdb_accel_vulkan_open(i, CDB_ACCEL_SOFTWARE, &vk) != CDB_OK) {
                printf("device %d: not usable\n", i);
                continue;
            }
            software = 1; /* refused by default, opened with the flag */
        }
        printf("device %d: %s%s\n", i, cdb_accel_vulkan_name(vk), software ? " (software)" : "");
        cdb_accel_vulkan_attach(vk, &env);
        run_device_tests();
        cdb_accel_vulkan_close(vk);
        hardware += !software;
    }
    failures = UNITY_END();
    if (failures == 0 && hardware == 0) {
        printf("no hardware Vulkan device: skipped\n");
        return 77; /* ctest's SKIP_RETURN_CODE */
    }
    return failures;
}
