/* Compression codecs (spec 15).
 *
 * Compressed form (spec 15.2): the uncompressed length (varint), the
 * codec's parameters, then the codec's raw stream with no container. For
 * LZMA2 (codec 1) the parameters are one byte, the LZMA2 dictionary-size
 * property. Decompression must produce exactly the stated length; anything
 * else is CDB_E_CORRUPT.
 *
 * Settings (spec 15.4): LZMA2 at preset 1 with a dictionary no larger than
 * the input (at least 4 KiB). A caller keeps the compressed form only when
 * cdb_codec_keeps() says it saves at least 1/8.
 *
 * Memory: liblzma allocates while it works. It is given the environment's
 * allocator when the environment has one (cdb_env.h), and otherwise carves
 * from mem->work, which is reset at the start of every call: size it with
 * cdb_codec_work_size() for the largest input the caller compresses.
 *
 * Without CDB_WITH_XZ the LZMA2 calls return CDB_E_UNSUPPORTED (spec 15.7). */
#ifndef CDB_CODEC_H
#define CDB_CODEC_H

#include <stddef.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_env.h"

/* Codec registry (spec 15.1): algorithms, not containers. */
#define CDB_CODEC_NONE 0u
#define CDB_CODEC_LZMA2 1u
#define CDB_CODEC_DEFLATE 2u   /* reserved */
#define CDB_CODEC_ZSTD 3u      /* reserved */
#define CDB_CODEC_LZ4 4u       /* reserved */
#define CDB_CODEC_BZIP2 5u     /* reserved */
#define CDB_CODEC_LZMA 6u      /* reserved */
#define CDB_CODEC_BROTLI 7u    /* reserved */
#define CDB_CODEC_SNAPPY 8u    /* reserved */

typedef struct cdb_codec_mem {
    const cdb_env *env;  /* its alloc/free are used when both are set */
    uint8_t *work;       /* otherwise: scratch memory, reset per call */
    size_t work_size;
    size_t used;         /* scratch in use during a call */
} cdb_codec_mem;

/* Whether this build has the codec. */
int cdb_codec_available(unsigned codec);

/* Scratch memory one call needs for inputs of up to max_len bytes, without
 * an environment allocator; 0 when the codec is not built in. */
size_t cdb_codec_work_size(unsigned codec, size_t max_len);

/* The largest compressed form of len bytes (for sizing output buffers). */
size_t cdb_codec_bound(size_t len);

/* Whether compressing an item is worth trying (spec 15.4): only items
 * larger than a quarter page, unless always (column chunks, pages). */
int cdb_codec_worth_trying(size_t len, size_t page_size, int always);

/* Whether a compressed form of `packed` bytes is kept for `plain` bytes:
 * it must save at least 1/8. */
int cdb_codec_keeps(size_t plain, size_t packed);

/* Compresses in into out in the spec 15.2 form. CDB_E_BUFFER_TOO_SMALL if
 * out cannot hold it (size out with cdb_codec_bound). */
cdb_status cdb_codec_compress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                              size_t cap, size_t *out_len);

/* The stated uncompressed length of a compressed form, without decoding. */
cdb_status cdb_codec_length(const void *in, size_t len, size_t *plain);

/* Decompresses a spec 15.2 form into out; *out_len is the stated length.
 * CDB_E_BUFFER_TOO_SMALL if it does not fit (with *out_len set),
 * CDB_E_CORRUPT if the stream is damaged or its length differs. */
cdb_status cdb_codec_decompress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                                size_t cap, size_t *out_len);

#endif
