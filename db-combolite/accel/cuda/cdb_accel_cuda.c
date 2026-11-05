/* The CUDA accelerator (spec 18.5, 18.7-18.10). A mirror holds one row per
 * vector in device memory, packed: row i holds the vector of entry ids[i],
 * and a delete moves the last row into the hole. Rows and norms are as
 * cax.h describes. A request computes dot products with cuBLAS, turns
 * them into sort keys, and selects the nearest with our own kernels
 * (cak_kernels.cu). Its memory comes from malloc and cudaMalloc: it stands
 * for a device, not for the core. */
#include "combolite/cdb_accel_cuda.h"

#include <cublas_v2.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cak.h"
#include "cax.h"
#include "combolite/cdb_value.h"

#define SCRATCH_MAX ((size_t)256u << 20) /* device bytes a request's queries may use at once */
#define CHUNK_MAX 4096u                  /* queries per chunk (a grid's second dimension) */

struct cdb_accel_cuda {
    int device;
    char name[256];
};

struct cdb_accel_mirror {
    cdb_accel_cuda *ax;
    cax_shape sh;
    cudaStream_t stream;
    cublasHandle_t blas;
    /* Device: rows, the norm or squared norm per row, and (Hamming) IDs. */
    uint32_t *drows;
    float *daux;
    uint64_t *dids;
    size_t n, cap;
    /* Host: the entry ID per row, and entry ID -> row. */
    uint64_t *ids;
    cax_map map;
    /* Per row, the put that last staged it and where, so a put naming an
     * entry twice sends its last vector once. */
    uint32_t *stamp, *staged_at, batch;
    double vmax, vmin; /* largest norm ever put, smallest nonzero one */
    uint64_t id_max;
    /* Staging and scratch, grown as needed. */
    void *hstage, *dstage, *dscratch;
    size_t hstage_size, dstage_size, dscratch_size;
};

/* ------------------------------------------------------------ errors */

static cdb_status from_cuda(cudaError_t e)
{
    if (e == cudaSuccess)
        return CDB_OK;
    if (e == cudaErrorMemoryAllocation)
        return CDB_E_OUT_OF_BUDGET;
    return CDB_E_IO;
}

static cdb_status from_cak(int e)
{
    return from_cuda((cudaError_t)e);
}

static cdb_status from_blas(cublasStatus_t e)
{
    switch (e) {
    case CUBLAS_STATUS_SUCCESS: return CDB_OK;
    case CUBLAS_STATUS_ALLOC_FAILED: return CDB_E_OUT_OF_BUDGET;
    case CUBLAS_STATUS_NOT_SUPPORTED:
    case CUBLAS_STATUS_ARCH_MISMATCH: return CDB_E_UNSUPPORTED;
    default: return CDB_E_IO;
    }
}

/* ------------------------------------------------------------ memory */

/* Grows host buffer *p to at least `need` bytes, keeping nothing. */
static cdb_status hgrow(void **p, size_t *have, size_t need)
{
    void *q;
    if (*have >= need)
        return CDB_OK;
    if ((q = malloc(need)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    free(*p);
    *p = q;
    *have = need;
    return CDB_OK;
}

static cdb_status dgrow(void **p, size_t *have, size_t need)
{
    void *q;
    cudaError_t e;
    if (*have >= need)
        return CDB_OK;
    if ((e = cudaMalloc(&q, need)) != cudaSuccess)
        return from_cuda(e);
    if (*p != NULL)
        (void)cudaFree(*p);
    *p = q;
    *have = need;
    return CDB_OK;
}

static size_t up256(size_t n)
{
    return (n + 255u) & ~(size_t)255u;
}

static cdb_status dresize(void **p, size_t keep, size_t size, cudaStream_t st)
{
    void *q;
    cudaError_t e;
    if ((e = cudaMalloc(&q, size)) != cudaSuccess)
        return from_cuda(e);
    if (*p != NULL) {
        if (keep && (e = cudaMemcpyAsync(q, *p, keep, cudaMemcpyDeviceToDevice, st)) == cudaSuccess)
            e = cudaStreamSynchronize(st);
        (void)cudaFree(*p);
        if (e != cudaSuccess) {
            (void)cudaFree(q);
            *p = NULL;
            return from_cuda(e);
        }
    }
    *p = q;
    return CDB_OK;
}

static cdb_status hresize(void **p, size_t size)
{
    void *q = realloc(*p, size);
    if (q == NULL)
        return CDB_E_OUT_OF_BUDGET;
    *p = q;
    return CDB_OK;
}

/* Room for at least `need` vectors: device rows and host tables grow to
 * twice what is needed. */
static cdb_status reserve(cdb_accel_mirror *m, size_t need)
{
    size_t cap, row = m->sh.words * 4u;
    cdb_status s;
    if (need <= m->cap)
        return CDB_OK;
    cap = m->cap * 2u > need ? m->cap * 2u : need;
    if (cap < 64u)
        cap = 64u;
    if (cap >= UINT32_MAX)
        return CDB_E_OUT_OF_BUDGET;
    if ((s = dresize((void **)&m->drows, m->n * row, cap * row, m->stream)) != CDB_OK
        || (s = dresize((void **)&m->daux, m->n * 4u, cap * 4u, m->stream)) != CDB_OK
        || (m->sh.measure == CAX_HAMMING
            && (s = dresize((void **)&m->dids, m->n * 8u, cap * 8u, m->stream)) != CDB_OK)
        || (s = hresize((void **)&m->ids, cap * 8u)) != CDB_OK
        || (s = hresize((void **)&m->stamp, cap * 4u)) != CDB_OK
        || (s = hresize((void **)&m->staged_at, cap * 4u)) != CDB_OK)
        return s;
    memset(m->stamp + m->cap, 0, (cap - m->cap) * 4u);
    m->cap = cap;
    return cax_map_reserve(&m->map, cap, m->ids, m->n);
}

/* ------------------------------------------------------------ calls */

static void free_mirror(cdb_accel_mirror *m)
{
    if (m->stream != NULL)
        (void)cudaStreamSynchronize(m->stream);
    (void)cudaFree(m->drows);
    (void)cudaFree(m->daux);
    (void)cudaFree(m->dids);
    (void)cudaFree(m->dstage);
    (void)cudaFree(m->dscratch);
    if (m->blas != NULL)
        (void)cublasDestroy(m->blas);
    if (m->stream != NULL)
        (void)cudaStreamDestroy(m->stream);
    free(m->ids);
    cax_map_free(&m->map);
    free(m->stamp);
    free(m->staged_at);
    free(m->hstage);
    free(m);
}

static cdb_status c_open(void *ctx, uint64_t store, unsigned elem, uint32_t dims, unsigned measure,
                         cdb_byte_order order, uint64_t expect, cdb_accel_mirror **out)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    cdb_accel_mirror *m;
    cax_shape sh;
    cdb_status s;
    (void)store;
    if ((s = cax_shape_init(&sh, elem, dims, measure, order)) != CDB_OK)
        return s;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    if ((m = (cdb_accel_mirror *)calloc(1, sizeof *m)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    m->ax = ax;
    m->sh = sh;
    if ((s = from_cuda(cudaStreamCreateWithFlags(&m->stream, cudaStreamNonBlocking))) != CDB_OK
        || (s = from_blas(cublasCreate(&m->blas))) != CDB_OK
        || (s = from_blas(cublasSetStream(m->blas, m->stream))) != CDB_OK
        || (s = from_blas(cublasSetMathMode(m->blas, CUBLAS_PEDANTIC_MATH))) != CDB_OK
        || (s = reserve(m, expect < (uint64_t)(UINT32_MAX / 2u) ? (size_t)expect : 64u)) != CDB_OK) {
        free_mirror(m);
        return s;
    }
    *out = m;
    return CDB_OK;
}

static cdb_status c_put(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, const void *vecs, size_t vec_bytes,
                        size_t n)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    size_t i, staged = 0, row = m->sh.words * 4u, need;
    uint8_t *h, *d;
    uint32_t *hrows, *hslots, *drows_in, *dslots_in;
    float *haux, *daux_in;
    uint64_t *hids, *dids_in;
    cdb_status s;
    if (vec_bytes != m->sh.vec_bytes)
        return CDB_E_INVALID_ARG;
    if (n == 0)
        return CDB_OK;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    if ((s = reserve(m, m->n + n)) != CDB_OK)
        return s;
    need = up256(n * row) + up256(n * 4u) * 2u + up256(n * 8u);
    if ((s = hgrow(&m->hstage, &m->hstage_size, need)) != CDB_OK
        || (s = dgrow(&m->dstage, &m->dstage_size, need)) != CDB_OK)
        return s;
    h = (uint8_t *)m->hstage;
    d = (uint8_t *)m->dstage;
    hrows = (uint32_t *)h;
    drows_in = (uint32_t *)d;
    haux = (float *)(h + up256(n * row));
    daux_in = (float *)(d + up256(n * row));
    hslots = (uint32_t *)(h + up256(n * row) + up256(n * 4u));
    dslots_in = (uint32_t *)(d + up256(n * row) + up256(n * 4u));
    hids = (uint64_t *)(h + up256(n * row) + up256(n * 4u) * 2u);
    dids_in = (uint64_t *)(d + up256(n * row) + up256(n * 4u) * 2u);
    if (++m->batch == 0) {
        memset(m->stamp, 0, m->cap * 4u);
        m->batch = 1;
    }
    for (i = 0; i < n; i++) {
        size_t at = cax_map_find(&m->map, ids[i]), r;
        uint32_t slot = m->map.rows[at];
        double sum, norm;
        if (slot == CAX_NO_ROW) {
            slot = (uint32_t)m->n++;
            m->ids[slot] = ids[i];
            m->map.ids[at] = ids[i];
            m->map.rows[at] = slot;
            if (ids[i] > m->id_max)
                m->id_max = ids[i];
        }
        if (m->stamp[slot] == m->batch) {
            r = m->staged_at[slot];
        } else {
            r = staged++;
            m->stamp[slot] = m->batch;
            m->staged_at[slot] = (uint32_t)r;
            hslots[r] = slot;
        }
        sum = cax_to_row(&m->sh, (const uint8_t *)vecs + i * vec_bytes, hrows + r * m->sh.words);
        norm = sqrt(sum);
        if (!(norm <= CAX_NORM_MAX))
            return CDB_E_UNSUPPORTED; /* the mirror goes stale; the CPU answers */
        haux[r] = m->sh.measure == CAX_EUCLIDEAN ? (float)sum : (float)norm;
        hids[r] = ids[i];
        if (norm > m->vmax)
            m->vmax = norm;
        if (norm > 0.0 && (m->vmin == 0.0 || norm < m->vmin))
            m->vmin = norm;
    }
    if ((s = from_cuda(cudaMemcpyAsync(d, h, need, cudaMemcpyHostToDevice, m->stream))) != CDB_OK
        || (s = from_cak(cak_scatter(m->stream, m->drows, drows_in, dslots_in, m->sh.words, staged))) != CDB_OK
        || (s = from_cak(cak_scatter(m->stream, (uint32_t *)m->daux, (const uint32_t *)daux_in, dslots_in, 1u,
                                     staged)))
               != CDB_OK
        || (m->sh.measure == CAX_HAMMING
            && (s = from_cak(cak_scatter(m->stream, (uint32_t *)m->dids, (const uint32_t *)dids_in, dslots_in, 2u,
                                         staged)))
                   != CDB_OK))
        return s;
    return from_cuda(cudaStreamSynchronize(m->stream));
}

static cdb_status c_del(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, size_t n)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    size_t i, row = m->sh.words * 4u;
    cdb_status s;
    if (n == 0)
        return CDB_OK;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    for (i = 0; i < n; i++) {
        size_t at = cax_map_find(&m->map, ids[i]), last;
        uint32_t slot = m->map.rows[at];
        if (slot == CAX_NO_ROW)
            continue;
        cax_map_erase(&m->map, at);
        last = --m->n;
        if (slot != last) { /* the last row fills the hole */
            uint64_t moved = m->ids[last];
            uint8_t *rows = (uint8_t *)m->drows;
            if ((s = from_cuda(cudaMemcpyAsync(rows + slot * row, rows + last * row, row, cudaMemcpyDeviceToDevice,
                                               m->stream)))
                    != CDB_OK
                || (s = from_cuda(cudaMemcpyAsync(m->daux + slot, m->daux + last, 4u, cudaMemcpyDeviceToDevice,
                                                  m->stream)))
                       != CDB_OK
                || (m->sh.measure == CAX_HAMMING
                    && (s = from_cuda(cudaMemcpyAsync(m->dids + slot, m->dids + last, 8u,
                                                      cudaMemcpyDeviceToDevice, m->stream)))
                           != CDB_OK))
                return s;
            m->ids[slot] = moved;
            m->map.rows[cax_map_find(&m->map, moved)] = slot;
        }
    }
    return from_cuda(cudaStreamSynchronize(m->stream));
}

static void c_close(void *ctx, cdb_accel_mirror *m)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    (void)cudaSetDevice(ax->device);
    free_mirror(m);
}

static cdb_status c_nearest(void *ctx, cdb_accel_mirror *m, const void *queries, size_t nq, size_t want,
                            uint64_t *ids, float *dists, size_t *counts, double *bounds)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    const cax_shape *sh = &m->sh;
    size_t w, seg, ld, per, chunk, q0, row = sh->words * 4u;
    unsigned id_bits = 0;
    int hamming = sh->measure == CAX_HAMMING, is_int = sh->elem == CDB_VEC_I8;
    cdb_status s;
    if (m->n == 0 || want == 0) {
        for (q0 = 0; q0 < nq; q0++) {
            counts[q0] = 0;
            bounds[q0] = 0.0;
        }
        return CDB_OK;
    }
    if (hamming) {
        id_bits = 64u - cax_bits_for(sh->dims);
        if (m->id_max >> id_bits)
            return CDB_E_UNSUPPORTED; /* IDs too large to order ties by on the device */
    }
    if (m->n > (size_t)INT32_MAX)
        return CDB_E_UNSUPPORTED;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    w = want < m->n ? want : m->n;
    for (seg = 1; seg < w; seg *= 2u)
        ;
    ld = (m->n + 3u) & ~(size_t)3u;
    per = up256(row) + 8u + (hamming ? 0u : ld * 4u) + m->n * 8u + seg * 8u + cak_select_work(1) + 1024u;
    chunk = SCRATCH_MAX / per;
    if (chunk < 1u)
        chunk = 1u;
    if (chunk > CHUNK_MAX)
        chunk = CHUNK_MAX;
    if (chunk > nq)
        chunk = nq;
    for (q0 = 0; q0 < nq; q0 += chunk) {
        size_t qc = nq - q0 < chunk ? nq - q0 : chunk, j, r;
        size_t o_q = 0, o_qaux = up256(qc * row), o_prod = o_qaux + up256(qc * 8u);
        size_t o_keys = o_prod + (hamming ? 0u : up256(ld * qc * 4u)), o_out = o_keys + up256(m->n * qc * 8u);
        size_t o_work = o_out + up256(qc * seg * 8u), dneed = o_work + up256(cak_select_work(qc));
        size_t h_qaux = up256(qc * row), h_out = h_qaux + up256(qc * 8u), hneed = h_out + qc * w * 8u;
        uint8_t *d, *h;
        double *qn;
        uint64_t *hout;
        if ((s = dgrow(&m->dscratch, &m->dscratch_size, dneed)) != CDB_OK
            || (s = hgrow(&m->hstage, &m->hstage_size, hneed)) != CDB_OK)
            return s;
        d = (uint8_t *)m->dscratch;
        h = (uint8_t *)m->hstage;
        qn = (double *)(h + h_qaux);
        hout = (uint64_t *)(h + h_out);
        for (j = 0; j < qc; j++) {
            double sum = cax_to_row(sh, (const uint8_t *)queries + (q0 + j) * sh->vec_bytes, (uint32_t *)(h + j * row));
            double norm = sqrt(sum);
            if (!(norm <= CAX_NORM_MAX) || (sh->measure == CAX_COSINE && norm == 0.0))
                return CDB_E_UNSUPPORTED; /* the CPU answers: overflow, or every distance ties at 1 */
            qn[j] = sh->measure == CAX_EUCLIDEAN ? sum : norm;
        }
        if ((s = from_cuda(cudaMemcpyAsync(d + o_q, h, h_out, cudaMemcpyHostToDevice, m->stream))) != CDB_OK)
            return s;
        if (hamming) {
            s = from_cak(cak_keys_hamming(m->stream, m->drows, (const uint32_t *)(d + o_q), sh->words, m->n, qc,
                                          m->dids, id_bits, (uint64_t *)(d + o_keys)));
        } else {
            /* products = rows^T queries: rows are kpad x n, queries kpad x qc. */
            cudaDataType_t t = sh->elem == CDB_VEC_F32    ? CUDA_R_32F
                               : sh->elem == CDB_VEC_F16  ? CUDA_R_16F
                               : sh->elem == CDB_VEC_BF16 ? CUDA_R_16BF
                                                          : CUDA_R_8I;
            float fa = 1.0f, fb = 0.0f;
            int32_t ia = 1, ib = 0;
            s = from_blas(cublasGemmEx(m->blas, CUBLAS_OP_T, CUBLAS_OP_N, (int)m->n, (int)qc, (int)sh->kpad,
                                       is_int ? (const void *)&ia : (const void *)&fa, m->drows, t, (int)sh->kpad,
                                       d + o_q, t, (int)sh->kpad, is_int ? (const void *)&ib : (const void *)&fb,
                                       d + o_prod, is_int ? CUDA_R_32I : CUDA_R_32F, (int)ld,
                                       is_int ? CUBLAS_COMPUTE_32I_PEDANTIC : CUBLAS_COMPUTE_32F_PEDANTIC,
                                       CUBLAS_GEMM_DEFAULT));
            if (s == CDB_OK)
                s = from_cak(cak_keys_product(m->stream, d + o_prod, is_int, ld, m->n, qc, sh->measure, m->daux,
                                              (const double *)(d + o_qaux), (uint64_t *)(d + o_keys)));
        }
        if (s != CDB_OK
            || (s = from_cak(cak_select(m->stream, (const uint64_t *)(d + o_keys), m->n, qc, w, seg,
                                        (uint64_t *)(d + o_out), d + o_work)))
                   != CDB_OK
            || (s = from_cuda(cudaMemcpy2DAsync(hout, w * 8u, d + o_out, seg * 8u, w * 8u, qc,
                                                cudaMemcpyDeviceToHost, m->stream)))
                   != CDB_OK
            || (s = from_cuda(cudaStreamSynchronize(m->stream))) != CDB_OK)
            return s;
        for (j = 0; j < qc; j++) {
            uint64_t *oi = ids + (q0 + j) * want;
            float *od = dists + (q0 + j) * want;
            for (r = 0; r < w; r++) {
                uint64_t key = hout[j * w + r];
                if (hamming) {
                    od[r] = (float)(key >> id_bits);
                    oi[r] = key & ((UINT64_C(1) << id_bits) - 1u);
                } else {
                    od[r] = cax_from_order((uint32_t)(key >> 32));
                    oi[r] = m->ids[(uint32_t)key];
                }
            }
            counts[q0 + j] = w;
            bounds[q0 + j] = cax_bound(sh, CAX_EPILOGUE_DOUBLE, sh->measure == CAX_EUCLIDEAN ? sqrt(qn[j]) : qn[j],
                                       m->vmax, m->vmin);
        }
    }
    return CDB_OK;
}

static const cdb_accel_ops cuda_ops = { c_open, c_put, c_del, c_close, c_nearest };

/* ------------------------------------------------------------ public */

cdb_status cdb_accel_cuda_open(int device, cdb_accel_cuda **out)
{
    cdb_accel_cuda *ax;
    struct cudaDeviceProp prop;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0)
        return CDB_E_UNSUPPORTED;
    if (device < 0)
        device = 0;
    if (device >= count || cudaSetDevice(device) != cudaSuccess || cudaFree(NULL) != cudaSuccess
        || cudaGetDeviceProperties(&prop, device) != cudaSuccess)
        return CDB_E_UNSUPPORTED;
    if ((ax = (cdb_accel_cuda *)calloc(1, sizeof *ax)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    ax->device = device;
    snprintf(ax->name, sizeof ax->name, "%s", prop.name);
    *out = ax;
    return CDB_OK;
}

void cdb_accel_cuda_attach(cdb_accel_cuda *ax, cdb_env *env)
{
    env->accel = &cuda_ops;
    env->accel_ctx = ax;
}

const char *cdb_accel_cuda_name(const cdb_accel_cuda *ax)
{
    return ax->name;
}

void cdb_accel_cuda_close(cdb_accel_cuda *ax)
{
    free(ax);
}
