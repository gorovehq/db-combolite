/* combolite_accel's own opener (spec 18.1): whichever accelerator this
 * build has and this machine can run. CUDA first, when built and a CUDA
 * device is present (cuBLAS is the faster path on NVIDIA), then Vulkan's
 * best device. An application that needs no choice uses this alone. */
#ifndef CDB_ACCEL_H
#define CDB_ACCEL_H

#include "combolite/cdb_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdb_accel cdb_accel;

/* Open flags, for cdb_accel_open and the backends' own openers. */
#define CDB_ACCEL_SOFTWARE 1u /* also use devices that run on the CPU, such as llvmpipe: off by default,
                               * since the engine's own CPU path is usually faster */

/* flags: CDB_ACCEL_* (0 for the defaults). CDB_E_UNSUPPORTED when no
 * backend finds a usable device: run without an accelerator, which is
 * never an error to the engine. */
cdb_status cdb_accel_open(unsigned flags, cdb_accel **out);

/* Installs it in env; it must stay open while any database using env is. */
void cdb_accel_attach(cdb_accel *a, cdb_env *env);

/* "CUDA: <device>" or "Vulkan: <device>", for messages. */
const char *cdb_accel_name(const cdb_accel *a);

void cdb_accel_close(cdb_accel *a);

#ifdef __cplusplus
}
#endif

#endif
