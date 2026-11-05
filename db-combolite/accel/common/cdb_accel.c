#include "combolite/cdb_accel.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef CDB_ACCEL_CUDA
#include "combolite/cdb_accel_cuda.h"
#endif
#ifdef CDB_ACCEL_VULKAN
#include "combolite/cdb_accel_vulkan.h"
#endif

struct cdb_accel {
#ifdef CDB_ACCEL_CUDA
    cdb_accel_cuda *cuda;
#endif
#ifdef CDB_ACCEL_VULKAN
    cdb_accel_vulkan *vulkan;
#endif
    char name[300];
};

cdb_status cdb_accel_open(unsigned flags, cdb_accel **out)
{
    cdb_accel *a = (cdb_accel *)calloc(1, sizeof *a);
    if (a == NULL)
        return CDB_E_OUT_OF_BUDGET;
#ifdef CDB_ACCEL_CUDA
    if (cdb_accel_cuda_open(-1, &a->cuda) == CDB_OK) {
        snprintf(a->name, sizeof a->name, "CUDA: %s", cdb_accel_cuda_name(a->cuda));
        *out = a;
        return CDB_OK;
    }
#endif
#ifdef CDB_ACCEL_VULKAN
    if (cdb_accel_vulkan_open(-1, flags, &a->vulkan) == CDB_OK) {
        snprintf(a->name, sizeof a->name, "Vulkan: %s", cdb_accel_vulkan_name(a->vulkan));
        *out = a;
        return CDB_OK;
    }
#endif
    free(a);
    (void)flags; /* CUDA devices are never software */
    return CDB_E_UNSUPPORTED;
}

void cdb_accel_attach(cdb_accel *a, cdb_env *env)
{
#ifdef CDB_ACCEL_CUDA
    if (a->cuda != NULL)
        cdb_accel_cuda_attach(a->cuda, env);
#endif
#ifdef CDB_ACCEL_VULKAN
    if (a->vulkan != NULL)
        cdb_accel_vulkan_attach(a->vulkan, env);
#endif
    (void)env;
}

const char *cdb_accel_name(const cdb_accel *a)
{
    return a->name;
}

void cdb_accel_close(cdb_accel *a)
{
#ifdef CDB_ACCEL_CUDA
    if (a->cuda != NULL)
        cdb_accel_cuda_close(a->cuda);
#endif
#ifdef CDB_ACCEL_VULKAN
    if (a->vulkan != NULL)
        cdb_accel_vulkan_close(a->vulkan);
#endif
    free(a);
}
