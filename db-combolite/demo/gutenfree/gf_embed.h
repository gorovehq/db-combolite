/* GutenFree Diet: sentence embeddings with llama.cpp (demo-only; the engine
 * never depends on it). bge-small-en-v1.5 on the CPU: CLS pooling, a
 * 512-token window, vectors normalised to length 1. */
#ifndef GF_EMBED_H
#define GF_EMBED_H

#include <stddef.h>

#define GF_EMBED_DIMS 384
#define GF_EMBED_DEFAULT_MODEL "data/models/bge-small-en-v1.5-q8_0.gguf"
/* bge wants this before a search query, and nothing before a passage. */
#define GF_EMBED_QUERY_PREFIX "Represent this sentence for searching relevant passages: "

typedef struct gf_embed gf_embed;

/* Loads the model; NULL on failure (llama.cpp's own logging is silenced,
 * so it cannot write over the screen). */
gf_embed *gf_embed_open(const char *path);
void gf_embed_close(gf_embed *e);

/* Embeds text into out[GF_EMBED_DIMS]; text past the window is cut.
 * Returns 0, or -1 on failure. *tokens (if not NULL) is how many tokens
 * were used. */
int gf_embed_text(gf_embed *e, const char *text, size_t len, float *out, int *tokens);

#endif
