/* Column chunk encodings (spec 9.12, 14.10.2).
 *
 * One column chunk of a column-oriented table holds the present (non-null)
 * values of one column, all of one type. This module turns those values into
 * the "values" part of the chunk and back. The chunk's null bitmap, its
 * encoding ID and its compression codec (cdb_codec, applied after encoding)
 * belong to the caller. The caller also knows the value count n (the number
 * of present values, from the null bitmap) and the column's type tag, so
 * neither is stored in the encoded bytes.
 *
 * Input and output values. A value is in the shared canonical encoding
 * (spec 14.6): tag, then payload. In an encoded chunk the tag is dropped,
 * because it is the same for every value; what remains is the "item":
 *   int        zigzag varint                         (as in 14.6, no tag)
 *   f64 / f32  8 / 4 bytes in the file's byte order
 *   timestamp  precision byte, then 8 bytes (file byte order)
 *   ref        two varints
 *   bool       one byte, 0 = false, 1 = true (the two bool tags become one
 *              column type; pass either CDB_TAG_FALSE or CDB_TAG_TRUE)
 *   tag >= 0x40  length varint, then that many payload bytes
 * Item bytes equal if and only if the values are equal, since values are
 * canonical. Null values and the reserved fixed tags 0x08-0x3F are not
 * allowed in a column.
 *
 * Encoding IDs (stored by the caller in the chunk header):
 *   0 plain       the n items one after another.
 *   1 dictionary  varint k (the distinct items, 1 <= k <= n), the k items in
 *                 order of first use, then n indexes, each in
 *                 bitlen(k - 1) bits (0 bits when k == 1), as one bitstream.
 *   2 run-length  pairs of (varint run length >= 1, item) until n values are
 *                 covered; adjacent runs never hold the same item.
 *   3 bit-packed  integer-like columns only (int, timestamp, bool):
 *                 [timestamp: precision byte, the same for every value]
 *                 zigzag varint of the minimum m, one byte w (0..64), then
 *                 the n values (v - m, computed in uint64) in w bits each.
 *                 w is the bit length of the largest v - m, so some value is
 *                 m and some value needs all w bits. A timestamp column whose
 *                 precisions differ cannot be bit-packed.
 * A bitstream is most significant bit first (cdb_tsbits.h); the bits after
 * the last one are zero, and the stream ends exactly there. None of these
 * forms has a fixed-width offset or length field: every count, run length
 * and the dictionary's k are varints, so a chunk is never capped at 4 GiB by
 * its own byte form. (Scratch and chunk-length arguments are size_t, so the
 * real limit is what the host's size_t and the caller's buffers allow.)
 *
 * Choice (spec 9.12): the chunk is encoded every way that applies and the
 * smallest result is kept; a tie goes to the lowest ID. An empty chunk
 * (n == 0) is plain and zero bytes; any other encoding with n == 0 is
 * CDB_E_NOT_CANONICAL on read, and encode_as refuses it.
 *
 * Canonical (spec 14.6.5): every set of values has one form per encoding.
 * A decoder rejects with CDB_E_NOT_CANONICAL: dictionary entries repeated
 * or not in first-use order or unused, adjacent runs of one item, a
 * bit width larger than needed, a minimum that no value reaches, non-zero
 * padding, bytes after the end. It rejects with CDB_E_CORRUPT: a run length
 * of 0 or a total that disagrees with n, an index or count out of range, a
 * width over 64, bit-packed on a type that cannot use it, a value that
 * wraps below the minimum. Running out of bytes is CDB_E_TRUNCATED, an item
 * that is not a canonical value of the column's type is CDB_E_MALFORMED or
 * CDB_E_NOT_CANONICAL (whichever cdb_value.c's own rules give). An item is
 * checked as deeply as a tag-less item allows:
 *   - fixed-shape scalars (int, f64, f32, timestamp, ref, bool) are checked
 *     with cdb_value_validate itself, by putting the column's tag back in
 *     front of the item in a small internal buffer: this is exactly the
 *     canonical check cdb_value.c applies everywhere else.
 *   - string is checked as UTF-8 (cdb_value_utf8_valid): for a string that
 *     is the whole canonical check (spec 14.6.5).
 *   - array and object elements are themselves full tagged values, present
 *     in the chunk as such, so once this module parses the container's own
 *     header (the element count; for objects, the widths byte and
 *     directory - spec 14.6.3, using only cdb_varint_decode and
 *     cdb_endian.h) each element is checked with the public
 *     cdb_value_validate, recursively. The container's own canonical rules
 *     (ascending field IDs, contiguous offsets, smallest directory widths)
 *     are also checked, matching cdb_value.c's validate_object.
 *   - binary has no canonical rule beyond its bytes, so it is not checked
 *     further.
 *   - decimal is checked for shape (the scale varint, then at least one
 *     unscaled byte) and then canonical form: cdb_value_decimal_is_canonical
 *     (shortest big-endian two's complement) and, in compact mode,
 *     cdb_value_decimal_fits_compact (the 128-bit limit).
 *   - vector is checked for shape (the element type, dims, and exactly
 *     enough element bytes) and then each element's canonical bit pattern,
 *     with cdb_value_f32_canonical and cdb_value_half_canonical (F16/BF16),
 *     and a BINARY vector's unused tail bits.
 *   These four are public wrappers in cdb_value.h over the exact checks
 *   cdb_value_validate itself applies, exposed because a column chunk's
 *   items have no tag byte in front, so this module cannot call
 *   cdb_value_validate directly on them the way the encoder does (below).
 *   An ID over 3 is CDB_E_UNSUPPORTED. The decoder cannot tell whether the
 *   encoder chose the smallest form; that rule is the writer's.
 *
 * Encoding, by contrast, is given each value with its real tag (the shared
 * input format below), so every value - including decimal and vector - is
 * checked with the public cdb_value_validate in full, exactly as if it were
 * being written anywhere else in the file.
 *
 * Memory: nothing allocates. The caller supplies a scratch array of 64-bit
 * words (cdb_colenc_scratch_words(n)) and buffers; the inputs and chunks are
 * sized in size_t. Encoding keeps a hash table over the scratch array to
 * find whether a value has already been placed in the dictionary (open
 * addressing, linear probing), so building the dictionary is close to
 * linear in n rather than O(n * distinct values); a run of hash collisions
 * only costs time, never a different encoded result, since a match is
 * always confirmed by comparing the actual bytes. */
#ifndef CDB_COLENC_H
#define CDB_COLENC_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"
#include "cdb_tsbits.h"

#define CDB_COLENC_PLAIN 0u
#define CDB_COLENC_DICT 1u
#define CDB_COLENC_RLE 2u
#define CDB_COLENC_BITPACK 3u

/* 64-bit scratch words that encode and the dictionary reader need for n
 * values (value offsets, dictionary indexes and first-use positions, plus a
 * power-of-two hash table sized to keep its load factor at 1/2 or better). */
size_t cdb_colenc_scratch_words(size_t n);

/* Upper bound on the encoded bytes of values_len bytes of input values:
 * plain is never larger than the input, and the smallest form is kept. */
size_t cdb_colenc_max_size(size_t values_len);

/* Whether a column of this tag can be bit-packed (int, timestamp, bool). */
int cdb_colenc_can_bitpack(uint8_t tag);

/* Encodes the n values in values[0..values_len) (tag and payload each, back
 * to back, all of column type `tag`) in whichever encoding is smallest.
 * decimal_mode is the file's decimal mode (cdb_value.h), needed to validate
 * any decimal values. Stores the encoding ID in *enc and the byte count in
 * *out_len. With CDB_E_BUFFER_TOO_SMALL (out and the scratch left in an
 * unspecified state) *enc and *out_len still say what is needed.
 * CDB_E_INVALID_ARG: a bad tag, a value of another type, bytes left over.
 * Other errors come from a value that is not canonical (cdb_value_validate's
 * errors). */
cdb_status cdb_colenc_encode(uint8_t tag, cdb_byte_order order, cdb_decimal_mode decimal_mode,
                             const uint8_t *values, size_t values_len, size_t n,
                             uint64_t *scratch, size_t scratch_words, uint8_t *out,
                             size_t out_cap, unsigned *enc, size_t *out_len);

/* The same with the encoding forced (used by tests and for comparing).
 * CDB_E_INVALID_ARG if it does not apply (bit-packed on a string, say, or
 * any non-plain encoding with n == 0). */
cdb_status cdb_colenc_encode_as(unsigned want, uint8_t tag, cdb_byte_order order,
                                cdb_decimal_mode decimal_mode, const uint8_t *values,
                                size_t values_len, size_t n, uint64_t *scratch,
                                size_t scratch_words, uint8_t *out, size_t out_cap,
                                size_t *out_len);

/* Reads a chunk's values one at a time. The item it yields (see above: the
 * value without its tag; for bool, one byte 0 or 1) is valid until the next
 * call and points into the chunk or into the reader. */
typedef struct cdb_colenc_reader {
    uint8_t tag;
    uint8_t enc;
    cdb_byte_order order;
    cdb_decimal_mode decimal_mode;
    cdb_status err; /* sticky */
    const uint8_t *data;
    size_t len;
    size_t pos; /* byte cursor (plain, run-length) */
    size_t n;
    size_t i;                    /* values yielded so far */
    uint64_t run_left;           /* run-length: values left in the current run */
    const uint8_t *run_item;
    size_t run_item_len;
    int have_prev;               /* run-length: a previous run exists */
    uint64_t *offs;              /* dictionary: item offsets */
    uint64_t k;                  /* dictionary: entries */
    uint64_t next_new;           /* dictionary: next index that may first appear */
    unsigned width;              /* dictionary and bit-packed: bits per value */
    cdb_bitr bits;
    int64_t min;                 /* bit-packed */
    uint8_t prec;
    uint64_t max_delta;
    int saw_min;
    uint8_t tmp[24];
} cdb_colenc_reader;

/* Checks the chunk's header (dictionary, minimum, width, sizes) and
 * positions the reader at the first value. The dictionary needs scratch for
 * its entry offsets (n + 1 words is always enough; cdb_colenc_scratch_words
 * is sized generously too). data and scratch must outlive the reader. */
cdb_status cdb_colenc_reader_init(cdb_colenc_reader *r, uint8_t tag, cdb_byte_order order,
                                  cdb_decimal_mode decimal_mode, unsigned enc,
                                  const uint8_t *data, size_t len, size_t n, uint64_t *scratch,
                                  size_t scratch_words);

/* Next item, or *done = 1 (and CDB_OK) after n values. Errors are sticky. */
cdb_status cdb_colenc_next(cdb_colenc_reader *r, const uint8_t **item, size_t *item_len,
                           int *done);

/* After all n values: checks the form's closing rules (see above) and that
 * the chunk ends here. CDB_E_STATE if values remain. */
cdb_status cdb_colenc_reader_finish(const cdb_colenc_reader *r);

/* init, read everything, finish. */
cdb_status cdb_colenc_validate(uint8_t tag, cdb_byte_order order, cdb_decimal_mode decimal_mode,
                               unsigned enc, const uint8_t *data, size_t len, size_t n,
                               uint64_t *scratch, size_t scratch_words);

#endif
