#include "cdb_fakeaccel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cdb_vec.h"

struct cdb_accel_mirror {
    struct cdb_accel_mirror *next, *prev; /* every open mirror, for a reset */
    unsigned elem, measure;
    uint32_t dims;
    cdb_byte_order order;
    size_t vec_bytes;
    uint64_t *ids;
    uint8_t *vecs;
    size_t n, cap;
};

/* Where entry id is, or n. */
static size_t find(const cdb_accel_mirror *m, uint64_t id)
{
    size_t i;
    for (i = 0; i < m->n; i++)
        if (m->ids[i] == id)
            return i;
    return m->n;
}

/* A fixed offset in [-1, 1] for (id, salt), so every run sees the same. */
static double jitter(uint64_t id, uint64_t salt)
{
    uint64_t x = id * UINT64_C(0x9E3779B97F4A7C15) ^ (salt + UINT64_C(0xD1B54A32D192ED03));
    x ^= x >> 31;
    x *= UINT64_C(0xBF58476D1CE4E5B9);
    x ^= x >> 29;
    return (double)(x >> 11) / (double)(UINT64_C(1) << 53) * 2.0 - 1.0;
}

static cdb_status f_open(void *ctx, uint64_t store, unsigned elem, uint32_t dims, unsigned measure,
                         cdb_byte_order order, uint64_t expect, cdb_accel_mirror **out)
{
    cdb_fakeaccel *fa = (cdb_fakeaccel *)ctx;
    cdb_accel_mirror *m;
    (void)store;
    (void)expect;
    if (elem < 32u && (fa->decline_elems & (1u << elem)))
        return CDB_E_UNSUPPORTED;
    if ((m = (cdb_accel_mirror *)calloc(1, sizeof *m)) == NULL)
        return CDB_E_OUT_OF_BUDGET;
    m->elem = elem;
    m->dims = dims;
    m->measure = measure;
    m->order = order;
    switch (elem) { /* bytes per stored vector (spec 14.6) */
    case CDB_VEC_F32:
        m->vec_bytes = (size_t)dims * 4u;
        break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        m->vec_bytes = (size_t)dims * 2u;
        break;
    case CDB_VEC_I8:
        m->vec_bytes = dims;
        break;
    default:
        m->vec_bytes = ((size_t)dims + 7u) / 8u;
        break;
    }
    m->next = fa->head;
    if (fa->head != NULL)
        fa->head->prev = m;
    fa->head = m;
    fa->opens++;
    fa->live++;
    *out = m;
    return CDB_OK;
}

static cdb_status f_put(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, const void *vecs, size_t vec_bytes,
                        size_t n)
{
    cdb_fakeaccel *fa = (cdb_fakeaccel *)ctx;
    size_t i;
    if (++fa->puts == fa->fail_put_at)
        return CDB_E_IO;
    if (vec_bytes != m->vec_bytes)
        return CDB_E_INVALID_ARG;
    for (i = 0; i < n; i++) {
        size_t at = find(m, ids[i]);
        if (at == m->n) {
            if (m->n == m->cap) {
                size_t cap = m->cap ? m->cap * 2u : 64u;
                uint64_t *ni = (uint64_t *)realloc(m->ids, cap * sizeof *ni);
                uint8_t *nv;
                if (ni == NULL)
                    return CDB_E_OUT_OF_BUDGET;
                m->ids = ni;
                if ((nv = (uint8_t *)realloc(m->vecs, cap * vec_bytes)) == NULL)
                    return CDB_E_OUT_OF_BUDGET;
                m->vecs = nv;
                m->cap = cap;
            }
            m->n++;
        }
        m->ids[at] = ids[i];
        memcpy(m->vecs + at * vec_bytes, (const uint8_t *)vecs + i * vec_bytes, vec_bytes);
    }
    return CDB_OK;
}

static cdb_status f_del(void *ctx, cdb_accel_mirror *m, const uint64_t *ids, size_t n)
{
    cdb_fakeaccel *fa = (cdb_fakeaccel *)ctx;
    size_t i;
    if (++fa->dels == fa->fail_del_at)
        return CDB_E_IO;
    for (i = 0; i < n; i++) {
        size_t at = find(m, ids[i]);
        if (at == m->n)
            continue;
        m->n--;
        m->ids[at] = m->ids[m->n];
        memmove(m->vecs + at * m->vec_bytes, m->vecs + m->n * m->vec_bytes, m->vec_bytes);
    }
    return CDB_OK;
}

static void f_close(void *ctx, cdb_accel_mirror *m)
{
    cdb_fakeaccel *fa = (cdb_fakeaccel *)ctx;
    if (m->prev != NULL)
        m->prev->next = m->next;
    else
        fa->head = m->next;
    if (m->next != NULL)
        m->next->prev = m->prev;
    free(m->ids);
    free(m->vecs);
    free(m);
    fa->closes++;
    fa->live--;
}

/* Each query against every vector: the CPU's distance plus a fixed offset
 * of up to `noise`, rounded to float; the bound covers both. Hamming
 * without noise is exact, so its bound is 0 and ties go to the lower ID. */
static cdb_status f_nearest(void *ctx, cdb_accel_mirror *m, const void *queries, size_t nq, size_t want,
                            uint64_t *ids, float *dists, size_t *counts, double *bounds)
{
    cdb_fakeaccel *fa = (cdb_fakeaccel *)ctx;
    size_t qi;
    if (++fa->nearests == fa->fail_nearest_at)
        return CDB_E_IO;
    for (qi = 0; qi < nq; qi++) {
        const uint8_t *q = (const uint8_t *)queries + qi * m->vec_bytes;
        uint64_t *oi = ids + qi * want;
        float *od = dists + qi * want;
        double top = 0.0;
        size_t i, have = 0;
        for (i = 0; i < m->n; i++) {
            double d = cdb_vec_cpu_distance(m->elem, m->dims, m->measure, q, m->vecs + i * m->vec_bytes, m->order);
            float f;
            size_t pos;
            if (fabs(d) > top)
                top = fabs(d);
            f = (float)(d + fa->noise * jitter(m->ids[i], qi));
            /* Insert in (distance, ID) order, keeping the first `want`. */
            pos = have;
            while (pos > 0 && (od[pos - 1] > f || (od[pos - 1] == f && oi[pos - 1] > m->ids[i])))
                pos--;
            if (pos >= want)
                continue;
            if (have < want)
                have++;
            memmove(oi + pos + 1, oi + pos, (have - pos - 1u) * sizeof *oi);
            memmove(od + pos + 1, od + pos, (have - pos - 1u) * sizeof *od);
            oi[pos] = m->ids[i];
            od[pos] = f;
        }
        counts[qi] = have;
        if (m->measure == CDB_VEC_HAMMING && fa->noise == 0.0)
            bounds[qi] = 0.0; /* bit counts are exact in a float */
        else
            bounds[qi] = fa->noise + (1.0 + top) * 1e-6; /* the noise, and float rounding */
    }
    return CDB_OK;
}

static const cdb_accel_ops fake_ops = { f_open, f_put, f_del, f_close, f_nearest };

void cdb_fakeaccel_init(cdb_fakeaccel *fa, cdb_env *env)
{
    /* Mirrors left by a database that was never closed (a crash test) go,
     * as a device's memory would at a reboot. */
    while (fa->head != NULL) {
        cdb_accel_mirror *m = fa->head;
        fa->head = m->next;
        free(m->ids);
        free(m->vecs);
        free(m);
    }
    memset(fa, 0, sizeof *fa);
    env->accel = &fake_ops;
    env->accel_ctx = fa;
}
