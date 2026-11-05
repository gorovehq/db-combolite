/* The CUDA accelerator on a real device (spec 18.11), with the tests of
 * accel_device.h. On a machine without a usable device it reports skipped
 * (exit code 77), not passed. */
#include "accel_device.h"

#include "combolite/cdb_accel_cuda.h"

int main(void)
{
    cdb_accel_cuda *cuda;
    int failures;
    if (cdb_accel_cuda_open(-1, &cuda) != CDB_OK) {
        printf("no usable CUDA device: skipped\n");
        return 77; /* ctest's SKIP_RETURN_CODE */
    }
    printf("device: %s\n", cdb_accel_cuda_name(cuda));
    cdb_accel_cuda_attach(cuda, &env);
    UNITY_BEGIN();
    run_device_tests();
    failures = UNITY_END();
    cdb_accel_cuda_close(cuda);
    return failures;
}
