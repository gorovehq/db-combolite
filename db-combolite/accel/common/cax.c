#include "cax.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "combolite/cdb_value.h"

/* ------------------------------------------------------------ shape */

cdb_status cax_shape_init(cax_shape *sh, unsigned elem, uint32_t dims, unsigned measure, cdb_byte_order order)
{
    memset(sh, 0, sizeof *sh);
    if (elem > CDB_VEC_BINARY || dims == 0 || (elem == CDB_VEC_BINARY) != (measure == CAX_HAMMING)
        || (measure != CAX_COSINE && measure != CAX_DOT && measure != CAX_EUCLIDEAN && measure != CAX_HAMMING))
        return CDB_E_UNSUPPORTED;
    if (dims >= (1u << 22) || (elem == CDB_VEC_I8 && dims >= 131072u))
        return CDB_E_UNSUPPORTED;
    sh->elem = elem;
    sh->measure = measure;
    sh->dims = dims;
    sh->kpad = (dims + 7u) & ~7u;
    sh->order = order;
    switch (elem) {
    case CDB_VEC_F32:
        sh->vec_bytes = (size_t)dims * 4u;
        sh->words = sh->kpad;
        break;
    case CDB_VEC_F16:
    case CDB_VEC_BF16:
        sh->vec_bytes = (size_t)dims * 2u;
        sh->words = sh->kpad / 2u;
        break;
    case CDB_VEC_I8:
        sh->vec_bytes = dims;
        sh->words = sh->kpad / 4u;
        break;
    default:
        sh->vec_bytes = ((size_t)dims + 7u) / 8u;
        sh->words = ((size_t)dims + 31u) / 32u;
        break;
    }
    return CDB_OK;
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

static void store16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

double cax_to_row(const cax_shape *sh, const uint8_t *v, uint32_t *row)
{
    uint8_t *out = (uint8_t *)row;
    double sum = 0.0;
    uint32_t i;
    memset(row, 0, sh->words * 4u);
    switch (sh->elem) {
    case CDB_VEC_F32:
        for (i = 0; i < sh->dims; i++) {
            uint32_t b = load32(v + i * 4u, sh->order);
            float f;
            memcpy(&f, &b, sizeof f);
            store32(out + i * 4u, b);
            sum += sq(f);
        }
        break;
    case CDB_VEC_F16:
        for (i = 0; i < sh->dims; i++) {
            uint16_t h = load16(v + i * 2u, sh->order);
            store16(out + i * 2u, h);
            sum += sq(half_to_float(h));
        }
        break;
    case CDB_VEC_BF16:
        for (i = 0; i < sh->dims; i++) {
            uint16_t h = load16(v + i * 2u, sh->order);
            uint32_t b = (uint32_t)h << 16;
            float f;
            memcpy(&f, &b, sizeof f);
            store16(out + i * 2u, h);
            sum += sq(f);
        }
        break;
    case CDB_VEC_I8:
        for (i = 0; i < sh->dims; i++) {
            out[i] = v[i];
            sum += sq((double)(int8_t)v[i]);
        }
        break;
    default: /* binary: the unused high bits of the last byte cleared */
        memcpy(out, v, sh->vec_bytes);
        if (sh->dims % 8u)
            out[sh->vec_bytes - 1u] &= (uint8_t)((1u << (sh->dims % 8u)) - 1u);
        break;
    }
    return sum;
}

/* ------------------------------------------------------------ id map */

static size_t home(const cax_map *m, uint64_t id)
{
    uint64_t h = id * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h ^ (h >> 29)) & (m->cap - 1u);
}

size_t cax_map_find(const cax_map *m, uint64_t id)
{
    size_t i = home(m, id);
    while (m->rows[i] != CAX_NO_ROW && m->ids[i] != id)
        i = (i + 1u) & (m->cap - 1u);
    return i;
}

/* Later entries of the run move back so every lookup still finds them. */
void cax_map_erase(cax_map *m, size_t at)
{
    size_t mask = m->cap - 1u, i = at, j = at;
    for (;;) {
        size_t k;
        m->rows[i] = CAX_NO_ROW;
        for (;;) {
            j = (j + 1u) & mask;
            if (m->rows[j] == CAX_NO_ROW)
                return;
            k = home(m, m->ids[j]);
            /* Entry j may move to i unless its home lies after i, up to j. */
            if (i <= j ? (k <= i || k > j) : (k <= i && k > j))
                break;
        }
        m->ids[i] = m->ids[j];
        m->rows[i] = m->rows[j];
        i = j;
    }
}

cdb_status cax_map_reserve(cax_map *m, size_t rows, const uint64_t *ids, size_t n)
{
    size_t cap, i;
    for (cap = 64u; cap < rows * 2u; cap *= 2u)
        ;
    if (cap <= m->cap)
        return CDB_OK;
    free(m->ids);
    free(m->rows);
    m->ids = (uint64_t *)malloc(cap * 8u);
    m->rows = (uint32_t *)malloc(cap * 4u);
    if (m->ids == NULL || m->rows == NULL) {
        m->cap = 0;
        return CDB_E_OUT_OF_BUDGET;
    }
    m->cap = cap;
    memset(m->rows, 0xFF, cap * 4u);
    for (i = 0; i < n; i++) {
        size_t at = cax_map_find(m, ids[i]);
        m->ids[at] = ids[i];
        m->rows[at] = (uint32_t)i;
    }
    return CDB_OK;
}

void cax_map_free(cax_map *m)
{
    free(m->ids);
    free(m->rows);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------ bound */

/* From worst-case rounding:
 *  - u is float's unit roundoff. A float sum of d products is off by at
 *    most g |q| |v|, g = 2 d u / (1 - 2 d u): twice the usual constant, in
 *    case the device's adds truncate instead of rounding. i8 products are
 *    exact integers (g = 0).
 *  - A device may flush subnormal inputs and products to zero: each of the
 *    d products then loses at most 2^-126 (1 + |q|)(1 + |v|).
 *  - The CPU's own double arithmetic adds cd.
 *  - After the product: in double (CUDA), a final float rounding; in float
 *    (Vulkan), a few more roundings, each u of the values involved.
 *  - Euclidean takes the square root of |q|^2 + |v|^2 - 2 s, whose error
 *    da gives at most sqrt(da) in the distance.
 * vmax and vmin bound every vector the mirror holds. */
double cax_bound(const cax_shape *sh, int epilogue, double qn, double vmax, double vmin)
{
    const double u = ldexp(1.0, -24), d = (double)sh->dims;
    const double cd = (d + 4.0) * ldexp(1.0, -52);
    const double flush = d * ldexp(1.0, -126) * (1.0 + qn) * (1.0 + vmax);
    double g = sh->elem == CDB_VEC_I8 ? 0.0 : 2.0 * d * u / (1.0 - 2.0 * d * u);
    double vm = vmax, f = epilogue == CAX_EPILOGUE_FLOAT ? 1.0 : 0.0;
    switch (sh->measure) {
    case CAX_DOT:
        return (g + cd + 2.0 * u) * qn * vm + flush;
    case CAX_COSINE:
        /* float: 1/|q| and 1/|v| rounded, their product, s times it, and 1 - r */
        return (g + cd) * (1.0 + 2.0 * u) + (6.0 + 8.0 * f) * u + (vmin > 0.0 ? flush / (qn * vmin) : 0.0);
    case CAX_EUCLIDEAN: {
        /* float: |q|^2 and the two adds rounded, then the square root's 3 ulp */
        double da = 2.0 * g * qn * vm + u * vm * vm + 4.0 * cd * sq(qn + vm) + 2.0 * flush
                    + f * 4.0 * u * sq(qn + vm);
        return sqrt(da) + (2.0 * u + cd + f * 4.0 * u) * (qn + vm) * 2.0;
    }
    default:
        return 0.0; /* bit counts are exact */
    }
}

unsigned cax_bits_for(uint32_t v)
{
    unsigned b = 1;
    while (b < 32u && (v >> b) != 0)
        b++;
    return b;
}

uint32_t cax_order_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

float cax_from_order(uint32_t o)
{
    uint32_t bits = (o & 0x80000000u) ? (o & 0x7FFFFFFFu) : ~o;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
