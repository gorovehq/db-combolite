/* The CUDA accelerator's kernels (spec 18.5): sort keys from cuBLAS dot
 * products or from bit counts, and our own top-K selection. */
#include <cuda_runtime.h>

#include "cak.h"

#define THREADS 256u
#define SHARED_SORT_MAX 2048u /* segments up to this many keys sort in shared memory */

/* Blocks for n items of work, `per` items each, at most 1024. */
static unsigned blocks_for(size_t n, size_t per)
{
    size_t b = (n + per - 1u) / per;
    if (b < 1u)
        b = 1u;
    if (b > 1024u)
        b = 1024u;
    return (unsigned)b;
}

static int launched(void)
{
    return (int)cudaGetLastError();
}

/* ------------------------------------------------------------ scatter */

__global__ void k_scatter(uint32_t *dst, const uint32_t *src, const uint32_t *slots, size_t words, size_t n)
{
    size_t t = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < n * words; t += stride) {
        size_t r = t / words, w = t % words;
        dst[(size_t)slots[r] * words + w] = src[t];
    }
}

extern "C" int cak_scatter(void *stream, uint32_t *ddst, const uint32_t *dsrc, const uint32_t *dslots, size_t words,
                           size_t n)
{
    if (n == 0)
        return 0;
    k_scatter<<<blocks_for(n * words, THREADS), THREADS, 0, (cudaStream_t)stream>>>(ddst, dsrc, dslots, words, n);
    return launched();
}

/* ------------------------------------------------------------ keys */

/* A float's bits, made to order as unsigned integers do: negatives have
 * every bit flipped, the others only the sign bit. */
__device__ static uint32_t order_bits(float f)
{
    uint32_t u = __float_as_uint(f);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

/* Each distance as the CPU path defines it (cdb_vec.c), from the product:
 * dot -s; cosine 1 - s / (|q| |v|), 1 for a zero vector; Euclidean
 * sqrt(|q|^2 + |v|^2 - 2 s). */
template <typename T>
__global__ void k_keys(const T *prod, size_t ld, size_t n, unsigned measure, const float *vaux, const double *qaux,
                       uint64_t *keys)
{
    size_t j = blockIdx.y;
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    double qa = qaux[j];
    for (; i < n; i += stride) {
        double s = (double)prod[j * ld + i], d;
        if (measure == CAK_DOT) {
            d = -s;
        } else if (measure == CAK_COSINE) {
            double vn = (double)vaux[i];
            d = (qa == 0.0 || vn == 0.0) ? 1.0 : 1.0 - s / (qa * vn);
        } else {
            double a = qa + (double)vaux[i] - 2.0 * s;
            d = sqrt(a > 0.0 ? a : 0.0);
        }
        keys[j * n + i] = ((uint64_t)order_bits((float)d) << 32) | (uint64_t)i;
    }
}

extern "C" int cak_keys_product(void *stream, const void *dprod, int is_int, size_t ld, size_t n, size_t nq,
                                unsigned measure, const float *dvaux, const double *dqaux, uint64_t *dkeys)
{
    dim3 grid(blocks_for(n, THREADS * 4u), (unsigned)nq);
    if (is_int)
        k_keys<int32_t><<<grid, THREADS, 0, (cudaStream_t)stream>>>((const int32_t *)dprod, ld, n, measure, dvaux,
                                                                    dqaux, dkeys);
    else
        k_keys<float><<<grid, THREADS, 0, (cudaStream_t)stream>>>((const float *)dprod, ld, n, measure, dvaux, dqaux,
                                                                  dkeys);
    return launched();
}

__global__ void k_hamming(const uint32_t *rows, const uint32_t *q, size_t words, size_t n, const uint64_t *ids,
                          unsigned id_bits, uint64_t *keys)
{
    size_t j = blockIdx.y;
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const uint32_t *qj = q + j * words;
    for (; i < n; i += stride) {
        const uint32_t *r = rows + i * words;
        uint64_t bits = 0;
        size_t w;
        for (w = 0; w < words; w++)
            bits += (uint64_t)__popc(r[w] ^ qj[w]);
        keys[j * n + i] = (bits << id_bits) | ids[i];
    }
}

extern "C" int cak_keys_hamming(void *stream, const uint32_t *drows, const uint32_t *dq, size_t words, size_t n,
                                size_t nq, const uint64_t *dids, unsigned id_bits, uint64_t *dkeys)
{
    dim3 grid(blocks_for(n, THREADS * 4u), (unsigned)nq);
    k_hamming<<<grid, THREADS, 0, (cudaStream_t)stream>>>(drows, dq, words, n, dids, id_bits, dkeys);
    return launched();
}

/* ------------------------------------------------------------ selection
 *
 * Radix select, 8 bits a pass from the top: a key is still a candidate
 * while its top bits equal the query's prefix under its mask. Each pass
 * counts the candidates' next digit, takes the digit holding the need-th
 * candidate, and keeps everything below it. Keys are unique, so once the
 * chosen digit's candidates are exactly the remaining need (at the latest
 * after the last pass), the keys at or below the prefix are exactly the
 * want smallest. */

typedef struct sel {
    uint64_t *prefix, *mask;
    uint32_t *need, *done, *count, *hist;
} sel;

static size_t up8(size_t n)
{
    return (n + 7u) & ~(size_t)7u;
}

static sel sel_at(void *work, size_t nq)
{
    sel s;
    uint8_t *p = (uint8_t *)work;
    s.prefix = (uint64_t *)p;
    p += nq * 8u;
    s.mask = (uint64_t *)p;
    p += nq * 8u;
    s.need = (uint32_t *)p;
    p += up8(nq * 4u);
    s.done = (uint32_t *)p;
    p += up8(nq * 4u);
    s.count = (uint32_t *)p;
    p += up8(nq * 4u);
    s.hist = (uint32_t *)p;
    return s;
}

extern "C" size_t cak_select_work(size_t nq)
{
    return nq * 16u + 3u * up8(nq * 4u) + nq * 256u * 4u;
}

__global__ void k_sel_init(sel s, size_t nq, uint32_t want, uint32_t all)
{
    size_t j = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    if (j >= nq)
        return;
    s.prefix[j] = 0;
    s.mask[j] = 0;
    s.need[j] = want;
    s.done[j] = all;
    s.count[j] = 0;
}

__global__ void k_sel_hist(const uint64_t *keys, size_t n, sel s, unsigned shift)
{
    __shared__ uint32_t h[256];
    size_t j = blockIdx.y;
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const uint64_t *kj = keys + j * n;
    uint64_t prefix = s.prefix[j], mask = s.mask[j];
    unsigned t;
    if (s.done[j])
        return; /* the same for the whole block */
    for (t = threadIdx.x; t < 256u; t += blockDim.x)
        h[t] = 0;
    __syncthreads();
    for (; i < n; i += stride) {
        uint64_t k = kj[i];
        if ((k & mask) == prefix)
            atomicAdd(&h[(k >> shift) & 0xFFu], 1u);
    }
    __syncthreads();
    for (t = threadIdx.x; t < 256u; t += blockDim.x)
        if (h[t])
            atomicAdd(&s.hist[j * 256u + t], h[t]);
}

__global__ void k_sel_choose(sel s, size_t nq, unsigned shift)
{
    size_t j = blockIdx.x * (size_t)blockDim.x + threadIdx.x;
    const uint32_t *h;
    uint32_t cum = 0, need;
    unsigned d;
    if (j >= nq || s.done[j])
        return;
    h = s.hist + j * 256u;
    need = s.need[j];
    for (d = 0; d < 255u; d++) {
        if (cum + h[d] >= need)
            break;
        cum += h[d];
    }
    need -= cum;
    s.need[j] = need;
    s.prefix[j] |= (uint64_t)d << shift;
    s.mask[j] |= (uint64_t)0xFFu << shift;
    if (h[d] == need || shift == 0)
        s.done[j] = 1;
}

__global__ void k_sel_gather(const uint64_t *keys, size_t n, sel s, uint64_t *out, size_t seg)
{
    size_t j = blockIdx.y;
    size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    const uint64_t *kj = keys + j * n;
    uint64_t prefix = s.prefix[j], mask = s.mask[j];
    for (; i < n; i += stride) {
        uint64_t k = kj[i];
        if ((k & mask) <= prefix) {
            uint32_t at = atomicAdd(&s.count[j], 1u);
            if (at < seg)
                out[j * seg + at] = k;
        }
    }
}

/* Bitonic sort of each seg-key segment, ascending. */
__global__ void k_sort_shared(uint64_t *out, unsigned seg)
{
    extern __shared__ uint64_t sk[];
    uint64_t *g = out + (size_t)blockIdx.x * seg;
    unsigned i, k, jj;
    for (i = threadIdx.x; i < seg; i += blockDim.x)
        sk[i] = g[i];
    __syncthreads();
    for (k = 2; k <= seg; k <<= 1)
        for (jj = k >> 1; jj > 0; jj >>= 1) {
            for (i = threadIdx.x; i < seg; i += blockDim.x) {
                unsigned l = i ^ jj;
                if (l > i) {
                    uint64_t a = sk[i], b = sk[l];
                    if ((a > b) == ((i & k) == 0)) {
                        sk[i] = b;
                        sk[l] = a;
                    }
                }
            }
            __syncthreads();
        }
    for (i = threadIdx.x; i < seg; i += blockDim.x)
        g[i] = sk[i];
}

__global__ void k_sort_step(uint64_t *out, size_t seg, size_t total, size_t k, size_t jj)
{
    size_t t = blockIdx.x * (size_t)blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t i = t % seg, l = i ^ jj;
        if (l > i) {
            uint64_t *base = out + (t - i);
            uint64_t a = base[i], b = base[l];
            if ((a > b) == ((i & k) == 0)) {
                base[i] = b;
                base[l] = a;
            }
        }
    }
}

extern "C" int cak_select(void *stream, const uint64_t *dkeys, size_t n, size_t nq, size_t want, size_t seg,
                          uint64_t *dout, void *dwork)
{
    cudaStream_t st = (cudaStream_t)stream;
    sel s = sel_at(dwork, nq);
    dim3 grid(blocks_for(n, THREADS * 8u), (unsigned)nq);
    unsigned qb = blocks_for(nq, THREADS);
    int e;
    k_sel_init<<<qb, THREADS, 0, st>>>(s, nq, (uint32_t)want, want >= n ? 1u : 0u);
    if (want < n) {
        unsigned shift;
        for (shift = 64u; shift > 0;) {
            shift -= 8u;
            if ((e = (int)cudaMemsetAsync(s.hist, 0, nq * 256u * 4u, st)) != 0)
                return e;
            k_sel_hist<<<grid, THREADS, 0, st>>>(dkeys, n, s, shift);
            k_sel_choose<<<qb, THREADS, 0, st>>>(s, nq, shift);
        }
    }
    if ((e = (int)cudaMemsetAsync(dout, 0xFF, nq * seg * 8u, st)) != 0)
        return e;
    k_sel_gather<<<grid, THREADS, 0, st>>>(dkeys, n, s, dout, seg);
    if (seg <= SHARED_SORT_MAX) {
        unsigned threads = seg < 512u ? (unsigned)seg : 512u;
        k_sort_shared<<<(unsigned)nq, threads, seg * 8u, st>>>(dout, (unsigned)seg);
    } else {
        size_t k, jj, total = nq * seg;
        for (k = 2; k <= seg; k <<= 1)
            for (jj = k >> 1; jj > 0; jj >>= 1)
                k_sort_step<<<blocks_for(total, THREADS * 4u), THREADS, 0, st>>>(dout, seg, total, k, jj);
    }
    return launched();
}
