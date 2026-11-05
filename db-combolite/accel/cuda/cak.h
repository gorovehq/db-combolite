/* The CUDA accelerator's kernels (cak_kernels.cu), callable from C. Every
 * call queues work on `stream` and returns a cudaError_t as an int (0:
 * success); nothing waits for the device. Pointers named d* are device
 * memory. Matrices are column-major, as cuBLAS has them: column j of an
 * n-row matrix with leading dimension ld starts at j * ld. */
#ifndef CAK_H
#define CAK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measures, as CDB_VEC_* in cdb_vec.h. */
#define CAK_COSINE 1u
#define CAK_DOT 2u
#define CAK_EUCLIDEAN 3u
#define CAK_HAMMING 4u

/* Copies n rows of `words` 32-bit words from dsrc (packed) to row
 * dslots[i] of ddst. */
int cak_scatter(void *stream, uint32_t *ddst, const uint32_t *dsrc, const uint32_t *dslots, size_t words,
                size_t n);

/* Turns dot products into sort keys (spec 18.5). dprod is n x nq with
 * leading dimension ld, float, or int32 when is_int. Each key is a
 * distance's order bits (high 32) above its row (low 32), so keys are
 * unique and order by distance. dvaux holds per row the f32 norm (cosine)
 * or squared norm (Euclidean); dqaux per query the norm or squared norm.
 * The arithmetic after the product is double. dkeys is n x nq, ld n. */
int cak_keys_product(void *stream, const void *dprod, int is_int, size_t ld, size_t n, size_t nq, unsigned measure,
                     const float *dvaux, const double *dqaux, uint64_t *dkeys);

/* Hamming keys: differing bits between each of n rows and nq queries, rows
 * and queries of `words` words with unused bits zero; each key is the
 * count shifted above the row's entry ID (did), which takes id_bits, so
 * ties order by lower ID (spec 18.10). dkeys is n x nq, ld n. */
int cak_keys_hamming(void *stream, const uint32_t *drows, const uint32_t *dq, size_t words, size_t n, size_t nq,
                     const uint64_t *dids, unsigned id_bits, uint64_t *dkeys);

/* Device bytes the selection's working state needs for nq queries. */
size_t cak_select_work(size_t nq);

/* For each of nq queries, the `want` smallest of its n keys (want <= n),
 * ascending, into dout (nq x seg, seg a power of two >= want; the rest of
 * each segment is UINT64_MAX). A radix select over the keys finds the
 * want-th smallest, a gather takes those at or below it, and a bitonic
 * sort orders them. dwork holds cak_select_work(nq) bytes. */
int cak_select(void *stream, const uint64_t *dkeys, size_t n, size_t nq, size_t want, size_t seg, uint64_t *dout,
               void *dwork);

#ifdef __cplusplus
}
#endif

#endif
