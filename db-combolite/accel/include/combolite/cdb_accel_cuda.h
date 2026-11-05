/* The CUDA accelerator (spec 18.5): the accelerator calls of cdb_env.h on
 * an NVIDIA GPU, part of the optional combolite_accel library, never the
 * core. Distances are cuBLAS matrix products; the top-K selection is our
 * own kernel. Built with -DCDB_ACCEL_CUDA=ON. */
#ifndef CDB_ACCEL_CUDA_H
#define CDB_ACCEL_CUDA_H

#include "combolite/cdb_env.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdb_accel_cuda cdb_accel_cuda;

/* Opens CUDA device `device` (-1: the first). CDB_E_UNSUPPORTED when there
 * is no usable device or driver; the application then runs without an
 * accelerator, which is never an error to the engine (spec 18.1). */
cdb_status cdb_accel_cuda_open(int device, cdb_accel_cuda **out);

/* Installs the accelerator in env (env.accel, env.accel_ctx). ax must stay
 * open while any database opened with env does. */
void cdb_accel_cuda_attach(cdb_accel_cuda *ax, cdb_env *env);

/* The device's name, for messages. */
const char *cdb_accel_cuda_name(const cdb_accel_cuda *ax);

/* Closes the accelerator. Every database using it must be closed first. */
void cdb_accel_cuda_close(cdb_accel_cuda *ax);

#ifdef __cplusplus
}
#endif

#endif
