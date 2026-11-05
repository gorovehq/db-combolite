/* The Vulkan accelerator (spec 18.5): the accelerator calls of cdb_env.h on
 * any Vulkan 1.2 device with buffer device addresses, part of the
 * optional combolite_accel library, never the core. Distances and the
 * top-K selection are our own compute shaders, checked in as SPIR-V.
 * Built with -DCDB_ACCEL_VULKAN=ON. */
#ifndef CDB_ACCEL_VULKAN_H
#define CDB_ACCEL_VULKAN_H

#include "combolite/cdb_accel.h"
#include "combolite/cdb_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdb_accel_vulkan cdb_accel_vulkan;

/* How many Vulkan devices the loader reports (0: none, or no loader).
 * Not all of them need be usable. */
int cdb_accel_vulkan_devices(void);

/* Opens Vulkan device `device`, in the loader's order, or with -1 the best
 * usable one: discrete, then integrated, then virtual. A device that runs
 * on the CPU (llvmpipe) is used only with CDB_ACCEL_SOFTWARE in flags,
 * whether chosen or named. CDB_E_UNSUPPORTED when it is missing, cannot
 * run the shaders or is software without the flag; the application then
 * runs without an accelerator (spec 18.1). */
cdb_status cdb_accel_vulkan_open(int device, unsigned flags, cdb_accel_vulkan **out);

/* Installs the accelerator in env (env.accel, env.accel_ctx). ax must stay
 * open while any database opened with env does. */
void cdb_accel_vulkan_attach(cdb_accel_vulkan *ax, cdb_env *env);

/* The device's name, for messages. */
const char *cdb_accel_vulkan_name(const cdb_accel_vulkan *ax);

/* Closes the accelerator. Every database using it must be closed first. */
void cdb_accel_vulkan_close(cdb_accel_vulkan *ax);

#ifdef __cplusplus
}
#endif

#endif
