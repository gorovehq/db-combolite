#define _POSIX_C_SOURCE 200809L
#include "gf_embed.h"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include "llama.h"

#define WINDOW 512

struct gf_embed {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    int encode;  /* encoder-only architecture: llama_encode, else llama_decode */
    llama_token tok[WINDOW * 4];
};

static void quiet(enum ggml_log_level level, const char *text, void *user)
{
    (void)level, (void)text, (void)user;
}

gf_embed *gf_embed_open(const char *path)
{
    struct llama_model_params mp;
    struct llama_context_params cp;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    gf_embed *e = calloc(1, sizeof *e);

    if (e == NULL)
        return NULL;
    llama_log_set(quiet, NULL);
    llama_backend_init();
    mp = llama_model_default_params();
    if ((e->model = llama_model_load_from_file(path, mp)) == NULL
        || llama_model_n_embd(e->model) != GF_EMBED_DIMS) {
        gf_embed_close(e);
        return NULL;
    }
    cp = llama_context_default_params();
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_CLS; /* bge: the [CLS] token's state */
    cp.n_ctx = WINDOW;
    cp.n_batch = WINDOW;
    cp.n_ubatch = WINDOW; /* non-causal attention: a whole text in one ubatch */
    cp.n_threads = cp.n_threads_batch = (int32_t)(cores > 0 ? cores : 4);
    if ((e->ctx = llama_init_from_model(e->model, cp)) == NULL) {
        gf_embed_close(e);
        return NULL;
    }
    e->vocab = llama_model_get_vocab(e->model);
    e->encode = llama_model_has_encoder(e->model) && !llama_model_has_decoder(e->model);
    return e;
}

void gf_embed_close(gf_embed *e)
{
    if (e == NULL)
        return;
    if (e->ctx != NULL)
        llama_free(e->ctx);
    if (e->model != NULL)
        llama_model_free(e->model);
    llama_backend_free();
    free(e);
}

int gf_embed_text(gf_embed *e, const char *text, size_t len, float *out, int *tokens)
{
    int n = llama_tokenize(e->vocab, text, (int32_t)len, e->tok, (int32_t)(sizeof e->tok / sizeof e->tok[0]), true,
                           false);
    const float *v;
    llama_memory_t mem;
    double norm = 0;
    int i;

    if (n < 0) /* longer than the token buffer: cut the text and retry */
        n = llama_tokenize(e->vocab, text, (int32_t)(len / 4), e->tok,
                           (int32_t)(sizeof e->tok / sizeof e->tok[0]), true, false);
    if (n <= 0)
        return -1;
    if (n > WINDOW) { /* keep the closing separator */
        e->tok[WINDOW - 1] = llama_vocab_sep(e->vocab);
        n = WINDOW;
    }
    if ((mem = llama_get_memory(e->ctx)) != NULL)
        llama_memory_clear(mem, true);
    if ((e->encode ? llama_encode(e->ctx, llama_batch_get_one(e->tok, n))
                   : llama_decode(e->ctx, llama_batch_get_one(e->tok, n))) != 0)
        return -1;
    if ((v = llama_get_embeddings_seq(e->ctx, 0)) == NULL)
        return -1;
    for (i = 0; i < GF_EMBED_DIMS; i++)
        norm += (double)v[i] * (double)v[i];
    norm = norm > 0 ? sqrt(norm) : 1.0;
    for (i = 0; i < GF_EMBED_DIMS; i++)
        out[i] = (float)((double)v[i] / norm);
    if (tokens != NULL)
        *tokens = n;
    return 0;
}
