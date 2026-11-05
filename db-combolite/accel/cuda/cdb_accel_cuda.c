/* The CUDA accelerator (spec 18.5, 18.7-18.10). A mirror holds one row per
 * vector in device memory, packed: slot i holds the vector of entry
 * ids[i], and a delete moves the last row into the hole. Rows are the
 * stored elements in the device's byte order, padded with zeros to a
 * multiple of 8 elements (binary: of 32 bits, unused bits cleared), next
 * to each vector's norm as the CPU path computes it. A request computes
 * dot products with cuBLAS, turns them into sort keys, and selects the
 * nearest with our own kernels (cak_kernels.cu). Its memory comes from
 * malloc and cudaMalloc: it stands for a device, not for the core. */
#include "combolite/cdb_accel_cuda.h"

#include <cublas_v2.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cak.h"
#include "combolite/cdb_value.h"

#define SCRATCH_MAX ((size_t)256u << 20) /* device bytes a request's queries may use at once */
#define CHUNK_MAX 4096u                  /* queries per chunk (a grid's second dimension) */
#define NO_SLOT UINT32_MAX
#define NORM_MAX 1e18 /* larger norms could overflow the float products */

struct cdb_accel_cuda {
    int device;
    char name[256];
};

struct cdb_accel_mirror {
    cdb_accel_cuda *ax;
    unsigned elem, measure;
    uint32_t dims, kpad;   /* elements, and padded to a multiple of 8 */
    size_t vec_bytes;      /* a stored vector */
    size_t words;          /* a device row, in 32-bit words */
    cdb_byte_order order;
    cudaStream_t stream;
    cublasHandle_t blas;
    /* Device: rows, the norm or squared norm per row, and (Hamming) IDs. */
    uint32_t *drows;
    float *daux;
    uint64_t *dids;
    size_t n, cap;
    /* Host: the entry ID per slot, and entry ID -> slot (open addressing). */
    uint64_t *ids;
    uint64_t *map_ids;
    uint32_t *map_slots;
    size_t map_cap;
    /* Per slot, the put that last staged it and where, so a put naming an
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

/* ------------------------------------------------------------ elements */

static uint16_t load16(const uint8_t *p, cdb_byte_order o)
{
    return o == CDB_ORDER_BIG ? (uint16_t)((unsigned)p[0] << 8 | p[1]) : (uint16_t)((unsigned)p[1] << 8 | p[0]);
}

static uint32_t load32(const uint8_t *p, cdb_byte_order o)
{
    if (o == CDB_ORDER_BIG)
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* IEEE 754 half precision to float, as the CPU path decodes it. */
static float half_to_float(uint16_t h)
{
    unsigned e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0)
        v = ldexpf((float)m, -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = ldexpf((float)(m | 0x400u), (int)e - 25);
    return (h & 0x8000u) ? -v : v;
}

static double sq(double x)
{
    return x * x;
}

/* A stored vector as a device row (row: m->words words), returning the
 * sum of its squared elements, added in index order in double, as the
 * CPU path's norm (cdb_vec.c) adds them. The device is little-endian. */
static double to_row(const cdb_accel_mirror *m, const uint8_t *v, uint32_t *row)
{
    uint8_t *out = (uint8_t *)row;
    double sum = 0.0;
    uint32_t i;
    memset(row, 0, m->words * 4u);
    switch (m->elem) {
    case CDB_VEC_F32:
        for (i = 0; i < m->dims; i++) {
            uint32_t b = load32(v + i * 4u, m->order);
            float f;
            memcpy(&f, &b, sizeof f);
            memcpy(out + i * 4u, &b, 4u);
            sum += sq(f);
        }
        break;
    case CDB_VEC_F16:
        for (i = 0; i < m->dims; i++) {
            uint16_t h = load16(v + i * 2u, m->order);
            memcpy(out + i * 2u, &h, 2u);
            sum += sq(half_to_float(h));
        }
        break;
    case CDB_VEC_BF16:
        for (i = 0; i < m->dims; i++) {
            uint16_t h = load16(v + i * 2u, m->order);
            uint32_t b = (uint32_t)h << 16;
            float f;
            memcpy(&f, &b, sizeof f);
            memcpy(out + i * 2u, &h, 2u);
            sum += sq(f);
        }
        break;
    case CDB_VEC_I8:
        for (i = 0; i < m->dims; i++) {
            out[i] = v[i];
            sum += sq((double)(int8_t)v[i]);
        }
        break;
    default: /* binary: the unused high bits of the last byte cleared */
        memcpy(out, v, m->vec_bytes);
        if (m->dims % 8u)
            out[m->vec_bytes - 1u] &= (uint8_t)((1u << (m->dims % 8u)) - 1u);
        break;
    }
    return sum;
}

/* ------------------------------------------------------------ id map */

static size_t home(const cdb_accel_mirror *m, uint64_t id)
{
    uint64_t h = id * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h ^ (h >> 29)) & (m->map_cap - 1u);
}

/* Where id is in the map, or the empty place it would go. */
static size_t map_find(const cdb_accel_mirror *m, uint64_t id)
{
    size_t i = home(m, id);
    while (m->map_slots[i] != NO_SLOT && m->map_ids[i] != id)
        i = (i + 1u) & (m->map_cap - 1u);
    return i;
}

/* Removes the entry at i, moving later entries of its run back so every
 * lookup still finds them (no tombstones). */
static void map_erase(cdb_accel_mirror *m, size_t i)
{
    size_t mask = m->map_cap - 1u, j = i;
    for (;;) {
        size_t k;
        m->map_slots[i] = NO_SLOT;
        for (;;) {
            j = (j + 1u) & mask;
            if (m->map_slots[j] == NO_SLOT)
                return;
            k = home(m, m->map_ids[j]);
            /* Entry j may move to i unless its home lies after i, up to j. */
            if (i <= j ? (k <= i || k > j) : (k <= i && k > j))
                break;
        }
        m->map_ids[i] = m->map_ids[j];
        m->map_slots[i] = m->map_slots[j];
        i = j;
    }
}

/* ------------------------------------------------------------ growth */

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
 * twice what is needed, and the map is rebuilt at no more than half full. */
static cdb_status reserve(cdb_accel_mirror *m, size_t need)
{
    size_t cap, i, map_cap;
    cdb_status s;
    if (need <= m->cap)
        return CDB_OK;
    cap = m->cap * 2u > need ? m->cap * 2u : need;
    if (cap < 64u)
        cap = 64u;
    if (cap >= UINT32_MAX)
        return CDB_E_OUT_OF_BUDGET;
    if ((s = dresize((void **)&m->drows, m->n * m->words * 4u, cap * m->words * 4u, m->stream)) != CDB_OK
        || (s = dresize((void **)&m->daux, m->n * 4u, cap * 4u, m->stream)) != CDB_OK
        || (m->measure == CAK_HAMMING && (s = dresize((void **)&m->dids, m->n * 8u, cap * 8u, m->stream)) != CDB_OK)
        || (s = hresize((void **)&m->ids, cap * 8u)) != CDB_OK
        || (s = hresize((void **)&m->stamp, cap * 4u)) != CDB_OK
        || (s = hresize((void **)&m->staged_at, cap * 4u)) != CDB_OK)
        return s;
    memset(m->stamp + m->cap, 0, (cap - m->cap) * 4u);
    m->cap = cap;
    for (map_cap = 64u; map_cap < cap * 2u; map_cap *= 2u)
        ;
    if (map_cap > m->map_cap) {
        free(m->map_ids);
        free(m->map_slots);
        m->map_ids = (uint64_t *)malloc(map_cap * 8u);
        m->map_slots = (uint32_t *)malloc(map_cap * 4u);
        if (m->map_ids == NULL || m->map_slots == NULL)
            return CDB_E_OUT_OF_BUDGET;
        m->map_cap = map_cap;
        memset(m->map_slots, 0xFF, map_cap * 4u);
        for (i = 0; i < m->n; i++) {
            size_t at = map_find(m, m->ids[i]);
            m->map_ids[at] = m->ids[i];
            m->map_slots[at] = (uint32_t)i;
        }
    }
    return CDB_OK;
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
    free(m->map_ids);
    free(m->map_slots);
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
    cdb_status s;
    (void)store;
    if (elem > CDB_VEC_BINARY || dims == 0 || (elem == CDB_VEC_BINARY) != (measure == CAK_HAMMING)
        || (measure != CAK_COSINE && measure != CAK_DOT && measure != CAK_EUCLIDEAN && measure != CAK_HAMMING))
        return CDB_E_UNSUPPORTED;
    /* Limits that keep the arithmetic exact or bounded (spec 18.10): an i8
     * product fits an int32, a bit count a key's top bits, and the float
     * products' error bound stays below 1. */
    if (dims >= (1u << 22) || (elem == CDB_VEC_I8 && dims >= 131072u))
        return CDB_E_UNSUPPORTED;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    if ((m = (cdb_accel_mirror *)calloc(1, sizeof *m)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    m->ax = ax;
    m->elem = elem;
    m->measure = measure;
    m->dims = dims;
    m->kpad = (dims + 7u) & ~7u;
    m->order = order;
    switch (elem) {
    case CDB_VEC_F32:
        m->vec_bytes = (size_t)dims * 4u;
        m->words = m->kpad;
        break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        m->vec_bytes = (size_t)dims * 2u;
        m->words = m->kpad / 2u;
        break;
    case CDB_VEC_I8:
        m->vec_bytes = dims;
        m->words = m->kpad / 4u;
        break;
    default:
        m->vec_bytes = ((size_t)dims + 7u) / 8u;
        m->words = ((size_t)dims + 31u) / 32u;
        break;
    }
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
    size_t i, staged = 0, row = m->words * 4u, hneed, dneed;
    uint8_t *h, *d;
    uint32_t *hrows, *hslots, *drows_in, *dslots_in;
    float *haux, *daux_in;
    uint64_t *hids, *dids_in;
    cdb_status s;
    if (vec_bytes != m->vec_bytes)
        return CDB_E_INVALID_ARG;
    if (n == 0)
        return CDB_OK;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    if ((s = reserve(m, m->n + n)) != CDB_OK)
        return s;
    hneed = up256(n * row) + up256(n * 4u) * 2u + up256(n * 8u);
    dneed = hneed;
    if ((s = hgrow(&m->hstage, &m->hstage_size, hneed)) != CDB_OK
        || (s = dgrow(&m->dstage, &m->dstage_size, dneed)) != CDB_OK)
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
        size_t at = map_find(m, ids[i]), r;
        uint32_t slot = m->map_slots[at];
        double sum, norm;
        if (slot == NO_SLOT) {
            slot = (uint32_t)m->n++;
            m->ids[slot] = ids[i];
            m->map_ids[at] = ids[i];
            m->map_slots[at] = slot;
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
        sum = to_row(m, (const uint8_t *)vecs + i * vec_bytes, hrows + r * m->words);
        norm = sqrt(sum);
        if (!(norm <= NORM_MAX))
            return CDB_E_UNSUPPORTED; /* the mirror goes stale; the CPU answers */
        haux[r] = m->measure == CAK_EUCLIDEAN ? (float)sum : (float)norm;
        hids[r] = ids[i];
        if (norm > m->vmax)
            m->vmax = norm;
        if (norm > 0.0 && (m->vmin == 0.0 || norm < m->vmin))
            m->vmin = norm;
    }
    if ((s = from_cuda(cudaMemcpyAsync(d, h, hneed, cudaMemcpyHostToDevice, m->stream))) != CDB_OK
        || (s = from_cak(cak_scatter(m->stream, m->drows, drows_in, dslots_in, m->words, staged))) != CDB_OK
        || (s = from_cak(cak_scatter(m->stream, (uint32_t *)m->daux, (const uint32_t *)daux_in, dslots_in, 1u,
                                     staged)))
               != CDB_OK
        || (m->measure == CAK_HAMMING
            && (s = from_cak(cak_scatter(m->stream, (uint32_t *)m->dids, (const uint32_t *)dids_in, dslots_in, 2u,
                                         staged)))
                   != CDB_OK))
        return s;
    return from_cuda(cudaStreamSynchronize(m->stream));
}

static cdb_status c_del(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, size_t n)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    size_t i, row = m->words * 4u;
    cdb_status s;
    if (n == 0)
        return CDB_OK;
    if (cudaSetDevice(ax->device) != cudaSuccess)
        return CDB_E_IO;
    for (i = 0; i < n; i++) {
        size_t at = map_find(m, ids[i]), last;
        uint32_t slot = m->map_slots[at];
        if (slot == NO_SLOT)
            continue;
        map_erase(m, at);
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
                || (m->measure == CAK_HAMMING
                    && (s = from_cuda(cudaMemcpyAsync(m->dids + slot, m->dids + last, 8u,
                                                      cudaMemcpyDeviceToDevice, m->stream)))
                           != CDB_OK))
                return s;
            m->ids[slot] = moved;
            m->map_slots[map_find(m, moved)] = slot;
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

/* The bound on how far a distance this mirror reports for a query of norm
 * qn can be from the CPU path's (spec 18.10), from worst-case rounding:
 *  - u is float's unit roundoff. A float product's sum over d elements is
 *    off by at most g |q| |v|, g = 2 d u / (1 - 2 d u): twice the usual
 *    constant, in case the device's adds truncate instead of rounding.
 *    i8 products are exact integers (g = 0).
 *  - Products the device flushes to zero (below 2^-126) add at most
 *    d 2^-125 more.
 *  - The CPU's own double arithmetic adds cd; the device's double
 *    arithmetic after the product, and each float rounding, a few u.
 *  - Euclidean takes a square root of |q|^2 + |v|^2 - 2 s, whose error
 *    da gives at most sqrt(da) in the distance.
 * vmax and vmin are the largest and smallest nonzero norms ever put, so
 * the bound covers every vector the mirror holds. */
static double bound_for(const cdb_accel_mirror *m, double qn)
{
    const double u = ldexp(1.0, -24), d = (double)m->dims;
    const double cd = (d + 4.0) * ldexp(1.0, -52), flush = d * ldexp(1.0, -125);
    double g = m->elem == CDB_VEC_I8 ? 0.0 : 2.0 * d * u / (1.0 - 2.0 * d * u);
    double vm = m->vmax;
    switch (m->measure) {
    case CAK_DOT:
        return (g + cd + 2.0 * u) * qn * vm + flush;
    case CAK_COSINE:
        return (g + cd) * (1.0 + 2.0 * u) + 6.0 * u + (m->vmin > 0.0 ? flush / (qn * m->vmin) : 0.0);
    case CAK_EUCLIDEAN: {
        double da = 2.0 * g * qn * vm + u * vm * vm + 4.0 * cd * sq(qn + vm) + 2.0 * flush;
        return sqrt(da) + (u + cd) * (qn + vm) * 2.0;
    }
    default:
        return 0.0; /* bit counts are exact */
    }
}

static unsigned bits_for(uint32_t v)
{
    unsigned b = 1;
    while (b < 32u && (v >> b) != 0)
        b++;
    return b;
}

/* A key's top 32 bits back to the float distance they encode. */
static float from_order(uint32_t o)
{
    uint32_t bits = (o & 0x80000000u) ? (o & 0x7FFFFFFFu) : ~o;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static cdb_status c_nearest(void *ctx, cdb_accel_mirror *m, const void *queries, size_t nq, size_t want,
                            uint64_t *ids, float *dists, size_t *counts, double *bounds)
{
    cdb_accel_cuda *ax = (cdb_accel_cuda *)ctx;
    size_t w, seg, ld, per, chunk, q0, row = m->words * 4u;
    unsigned id_bits = 0;
    int hamming = m->measure == CAK_HAMMING, is_int = m->elem == CDB_VEC_I8;
    cdb_status s;
    if (m->n == 0 || want == 0) {
        for (q0 = 0; q0 < nq; q0++) {
            counts[q0] = 0;
            bounds[q0] = 0.0;
        }
        return CDB_OK;
    }
    if (hamming) {
        id_bits = 64u - bits_for(m->dims);
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
            double sum = to_row(m, (const uint8_t *)queries + (q0 + j) * m->vec_bytes, (uint32_t *)(h + j * row));
            double norm = sqrt(sum);
            if (!(norm <= NORM_MAX) || (m->measure == CAK_COSINE && norm == 0.0))
                return CDB_E_UNSUPPORTED; /* the CPU answers: overflow, or every distance ties at 1 */
            qn[j] = m->measure == CAK_EUCLIDEAN ? sum : norm;
        }
        if ((s = from_cuda(cudaMemcpyAsync(d + o_q, h, h_out, cudaMemcpyHostToDevice, m->stream))) != CDB_OK)
            return s;
        if (hamming) {
            s = from_cak(cak_keys_hamming(m->stream, m->drows, (const uint32_t *)(d + o_q), m->words, m->n, qc,
                                          m->dids, id_bits, (uint64_t *)(d + o_keys)));
        } else {
            /* products = rows^T queries: rows are kpad x n, queries kpad x qc. */
            cudaDataType_t t = m->elem == CDB_VEC_F32    ? CUDA_R_32F
                               : m->elem == CDB_VEC_F16  ? CUDA_R_16F
                               : m->elem == CDB_VEC_BF16 ? CUDA_R_16BF
                                                         : CUDA_R_8I;
            float fa = 1.0f, fb = 0.0f;
            int32_t ia = 1, ib = 0;
            s = from_blas(cublasGemmEx(m->blas, CUBLAS_OP_T, CUBLAS_OP_N, (int)m->n, (int)qc, (int)m->kpad,
                                       is_int ? (const void *)&ia : (const void *)&fa, m->drows, t, (int)m->kpad,
                                       d + o_q, t, (int)m->kpad, is_int ? (const void *)&ib : (const void *)&fb,
                                       d + o_prod, is_int ? CUDA_R_32I : CUDA_R_32F, (int)ld,
                                       is_int ? CUBLAS_COMPUTE_32I_PEDANTIC : CUBLAS_COMPUTE_32F_PEDANTIC,
                                       CUBLAS_GEMM_DEFAULT));
            if (s == CDB_OK)
                s = from_cak(cak_keys_product(m->stream, d + o_prod, is_int, ld, m->n, qc, m->measure, m->daux,
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
                    od[r] = from_order((uint32_t)(key >> 32));
                    oi[r] = m->ids[(uint32_t)key];
                }
            }
            counts[q0 + j] = w;
            bounds[q0 + j] = bound_for(m, m->measure == CAK_EUCLIDEAN ? sqrt(qn[j]) : qn[j]);
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
