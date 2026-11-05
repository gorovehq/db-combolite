/* What every backend of combolite_accel shares (spec 18.5-18.10): the
 * shape of a mirror's device rows, stored vectors turned into rows with
 * their norms as the CPU path computes them, the entry ID -> row map, and
 * the error bound a shortlist reports. Host code; C99. */
#ifndef CAX_H
#define CAX_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"

/* Measures, as CDB_VEC_* in cdb_vec.h. */
#define CAX_COSINE 1u
#define CAX_DOT 2u
#define CAX_EUCLIDEAN 3u
#define CAX_HAMMING 4u

#define CAX_NORM_MAX 1e18 /* larger norms could overflow float products */

/* A mirror's vectors: stored form, and the device row each becomes. Rows
 * are the stored elements in little-endian order, padded with zeros to a
 * multiple of 8 elements; binary rows are whole 32-bit words with the
 * unused bits cleared. */
typedef struct cax_shape {
    unsigned elem, measure;
    uint32_t dims, kpad;   /* elements, and padded to a multiple of 8 */
    size_t vec_bytes;      /* a stored vector */
    size_t words;          /* a device row, in 32-bit words */
    cdb_byte_order order;
} cax_shape;

/* The shape for a collection, or CDB_E_UNSUPPORTED for a type, measure or
 * size the backends do not take: an i8 product must fit an int32, a bit
 * count a key's top bits, and the float error bound must stay below 1. */
cdb_status cax_shape_init(cax_shape *sh, unsigned elem, uint32_t dims, unsigned measure, cdb_byte_order order);

/* Stored vector v as a device row (sh->words words), returning the sum of
 * its squared elements, added in index order in double as the CPU path's
 * norm adds them. */
double cax_to_row(const cax_shape *sh, const uint8_t *v, uint32_t *row);

/* Entry ID -> row, open addressing without tombstones. */
typedef struct cax_map {
    uint64_t *ids;
    uint32_t *rows; /* CAX_NO_ROW: empty */
    size_t cap;     /* a power of two */
} cax_map;

#define CAX_NO_ROW UINT32_MAX

/* Where id is, or the empty place it would go. */
size_t cax_map_find(const cax_map *m, uint64_t id);
void cax_map_erase(cax_map *m, size_t at);
/* Makes room for at least 2 * rows entries, rebuilding from ids[0..n). */
cdb_status cax_map_reserve(cax_map *m, size_t rows, const uint64_t *ids, size_t n);
void cax_map_free(cax_map *m);

/* How a backend turns products into distances. */
#define CAX_EPILOGUE_DOUBLE 0 /* in double, as the CPU path does (CUDA) */
#define CAX_EPILOGUE_FLOAT 1  /* in float, multiplies and adds only, and reciprocal norms (Vulkan) */

/* The bound on how far a distance the mirror reports for a query of norm
 * qn can be from the CPU path's (spec 18.10), given the largest norm ever
 * put (vmax) and the smallest nonzero one (vmin, 0: none). */
double cax_bound(const cax_shape *sh, int epilogue, double qn, double vmax, double vmin);

/* Bits needed to hold v (at least 1). */
unsigned cax_bits_for(uint32_t v);

/* A float's bits, made to order as unsigned integers do, and back. */
uint32_t cax_order_bits(float f);
float cax_from_order(uint32_t o);

#endif
