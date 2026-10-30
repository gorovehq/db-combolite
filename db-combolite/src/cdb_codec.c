/* Compression codecs (cdb_codec.h; spec 15). */
#include <string.h>

#include "cdb_codec.h"
#include "cdb_varint.h"

#ifdef CDB_WITH_XZ
#include <lzma.h>
#endif

#define PRESET 1u               /* low preset (spec 15.4) */
#define DICT_MIN 4096u          /* LZMA2's smallest dictionary */
#define WORK_ALIGN 16u

int cdb_codec_available(unsigned codec)
{
#ifdef CDB_WITH_XZ
    return codec == CDB_CODEC_NONE || codec == CDB_CODEC_LZMA2;
#else
    return codec == CDB_CODEC_NONE;
#endif
}

int cdb_codec_worth_trying(size_t len, size_t page_size, int always)
{
    return always || len > page_size / 4u;
}

int cdb_codec_keeps(size_t plain, size_t packed)
{
    return packed <= plain - plain / 8u;
}

size_t cdb_codec_bound(size_t len)
{
    /* LZMA2 stores incompressible data in uncompressed chunks: 3 bytes per
     * 64 KiB plus an end marker; then the header (length and property). */
    return len + (len / 65536u + 1u) * 3u + 1u + CDB_VARINT_MAX_LEN + 1u;
}

cdb_status cdb_codec_length(const void *in, size_t len, size_t *plain)
{
    uint64_t v;
    size_t n;
    if (in == NULL || cdb_varint_decode((const uint8_t *)in, len, &v, &n) != CDB_OK || v > (uint64_t)SIZE_MAX)
        return CDB_E_CORRUPT;
    *plain = (size_t)v;
    return CDB_OK;
}

#ifdef CDB_WITH_XZ

/* ------------------------------------------------------------ memory */

static void *mem_alloc(void *opaque, size_t nmemb, size_t size)
{
    cdb_codec_mem *m = (cdb_codec_mem *)opaque;
    size_t want, start;
    if (size != 0 && nmemb > SIZE_MAX / size)
        return NULL;
    want = nmemb * size;
    if (m->env != NULL && m->env->ops->alloc != NULL && m->env->ops->free != NULL)
        return m->env->ops->alloc(m->env->ctx, want);
    /* Scratch: a bump allocator, reset per call; free does nothing. */
    start = (m->used + WORK_ALIGN - 1u) / WORK_ALIGN * WORK_ALIGN;
    if (m->work == NULL || start > m->work_size || want > m->work_size - start)
        return NULL;
    m->used = start + want;
    return m->work + start;
}

static void mem_free(void *opaque, void *ptr)
{
    cdb_codec_mem *m = (cdb_codec_mem *)opaque;
    if (ptr != NULL && m->env != NULL && m->env->ops->alloc != NULL && m->env->ops->free != NULL)
        m->env->ops->free(m->env->ctx, ptr);
}

/* The dictionary for an input: a power of two no smaller than it, within
 * LZMA2's bounds. */
static uint32_t dict_for(size_t len)
{
    uint32_t d = DICT_MIN;
    while (d < len && d < (UINT32_C(1) << 30))
        d <<= 1;
    return d;
}

static int filters_for(size_t len, lzma_options_lzma *opt, lzma_filter f[2])
{
    if (lzma_lzma_preset(opt, PRESET))
        return 0;
    opt->dict_size = dict_for(len);
    f[0].id = LZMA_FILTER_LZMA2;
    f[0].options = opt;
    f[1].id = LZMA_VLI_UNKNOWN;
    f[1].options = NULL;
    return 1;
}

/* The dictionary size an LZMA2 property byte stands for (0..40). */
static int dict_of_prop(uint8_t p, uint32_t *dict)
{
    if (p > 40u)
        return 0;
    *dict = p == 40u ? UINT32_MAX : (uint32_t)(2u | (p & 1u)) << (p / 2u + 11u);
    return 1;
}

size_t cdb_codec_work_size(unsigned codec, size_t max_len)
{
    lzma_options_lzma opt;
    lzma_filter f[2];
    uint64_t enc, dec;
    if (codec != CDB_CODEC_LZMA2 || !filters_for(max_len, &opt, f))
        return 0;
    enc = lzma_raw_encoder_memusage(f);
    dec = lzma_raw_decoder_memusage(f);
    if (enc == UINT64_MAX || dec == UINT64_MAX)
        return 0;
    /* The larger of the two, plus room for alignment of each block. */
    return (size_t)(enc > dec ? enc : dec) + 64u * WORK_ALIGN;
}

cdb_status cdb_codec_compress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                              size_t cap, size_t *out_len)
{
    lzma_options_lzma opt;
    lzma_filter f[2];
    lzma_allocator a;
    uint8_t *o = (uint8_t *)out;
    size_t pos, hdr;
    lzma_ret r;

    if (codec != CDB_CODEC_LZMA2)
        return CDB_E_UNSUPPORTED;
    if ((in == NULL && len) || out == NULL || mem == NULL || !filters_for(len, &opt, f))
        return CDB_E_INVALID_ARG;
    if (cap < CDB_VARINT_MAX_LEN + 1u)
        return CDB_E_BUFFER_TOO_SMALL;
    hdr = cdb_varint_encode((uint64_t)len, o);
    if (lzma_properties_encode(&f[0], o + hdr) != LZMA_OK)
        return CDB_E_INVALID_ARG;
    hdr++;
    a.alloc = mem_alloc;
    a.free = mem_free;
    a.opaque = mem;
    mem->used = 0;
    pos = hdr;
    r = lzma_raw_buffer_encode(f, &a, (const uint8_t *)in, len, o, &pos, cap);
    if (r == LZMA_BUF_ERROR)
        return CDB_E_BUFFER_TOO_SMALL;
    if (r == LZMA_MEM_ERROR)
        return CDB_E_BUFFER_TOO_SMALL; /* no allocator and too little work memory */
    if (r != LZMA_OK)
        return CDB_E_INVALID_ARG;
    *out_len = pos;
    return CDB_OK;
}

cdb_status cdb_codec_decompress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                                size_t cap, size_t *out_len)
{
    const uint8_t *p = (const uint8_t *)in;
    lzma_options_lzma opt;
    lzma_filter f[2];
    lzma_allocator a;
    uint64_t plain;
    size_t n, in_pos, out_pos = 0;
    uint32_t dict;
    lzma_ret r;

    if (codec != CDB_CODEC_LZMA2)
        return CDB_E_UNSUPPORTED;
    if (in == NULL || mem == NULL)
        return CDB_E_INVALID_ARG;
    if (cdb_varint_decode(p, len, &plain, &n) != CDB_OK || n >= len || plain > (uint64_t)SIZE_MAX
        || !dict_of_prop(p[n], &dict))
        return CDB_E_CORRUPT;
    *out_len = (size_t)plain;
    if (plain > cap)
        return CDB_E_BUFFER_TOO_SMALL;
    if (lzma_lzma_preset(&opt, PRESET))
        return CDB_E_CORRUPT;
    opt.dict_size = dict;
    f[0].id = LZMA_FILTER_LZMA2;
    f[0].options = &opt;
    f[1].id = LZMA_VLI_UNKNOWN;
    f[1].options = NULL;
    a.alloc = mem_alloc;
    a.free = mem_free;
    a.opaque = mem;
    mem->used = 0;
    in_pos = n + 1u;
    /* The stream must decode to exactly the stated length and end exactly
     * at the end of the input. */
    r = lzma_raw_buffer_decode(f, &a, p, &in_pos, len, (uint8_t *)out, &out_pos, (size_t)plain);
    if (r == LZMA_MEM_ERROR)
        return CDB_E_BUFFER_TOO_SMALL;
    if (r != LZMA_OK || out_pos != (size_t)plain || in_pos != len)
        return CDB_E_CORRUPT;
    return CDB_OK;
}

#else /* no codec built in */

size_t cdb_codec_work_size(unsigned codec, size_t max_len)
{
    (void)codec;
    (void)max_len;
    return 0;
}

cdb_status cdb_codec_compress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                              size_t cap, size_t *out_len)
{
    (void)mem;
    (void)codec;
    (void)in;
    (void)len;
    (void)out;
    (void)cap;
    (void)out_len;
    return CDB_E_UNSUPPORTED;
}

cdb_status cdb_codec_decompress(cdb_codec_mem *mem, unsigned codec, const void *in, size_t len, void *out,
                                size_t cap, size_t *out_len)
{
    (void)mem;
    (void)codec;
    (void)in;
    (void)len;
    (void)out;
    (void)cap;
    (void)out_len;
    return CDB_E_UNSUPPORTED;
}

#endif
