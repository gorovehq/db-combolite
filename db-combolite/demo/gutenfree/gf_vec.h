/* GutenFree Diet: vectors (spec 7) and links (spec 6).
 *
 *   passage_vec   f32 x 384, cosine, one per passage, source = the passage
 *                 document (so search can tell a stale vector), with a
 *                 neighbour graph (k = 16) for approximate search
 *   passage_bits  the same vectors as 384 sign bits (48 bytes), Hamming
 *   book_vec      the mean of a book's passage vectors, source = its
 *                 library row
 *   similar_to    links between library rows (N:M, recorded), to each
 *                 book's nearest books, weight = cosine similarity */
#ifndef GF_VEC_H
#define GF_VEC_H

#include "gf_books.h"
#include "gf_catalog.h"
#include "gf_embed.h"

#define GF_SIMILAR 5u

typedef struct gf_vec {
    uint64_t passages;
    uint64_t bits;
    uint64_t books;
    uint64_t similar;   /* link type */
} gf_vec;

typedef struct gf_embed_stats {
    uint64_t passages;
    uint64_t tokens;
    double embed_ms;    /* in the model */
    double commit_ms;   /* writing every vector of the book */
    double ms;
} gf_embed_stats;

/* Embedding one book a slice at a time, for the background download job:
 * start checks the book, step embeds passages until budget_ms is spent,
 * commit stores every vector in one transaction. Holds its own memory. */
typedef struct gf_vec_job {
    uint64_t row;
    gf_book bk;
    uint64_t n, i;         /* passages, and how many are embedded */
    double mean[GF_EMBED_DIMS];
    float (*vecs)[GF_EMBED_DIMS];
    gf_embed_stats st;
} gf_vec_job;

/* CDB_E_EXISTS: the book has vectors; CDB_E_NOT_FOUND: no passages. */
cdb_status gf_vec_job_start(gf_db *g, const gf_catalog *c, const gf_vec *v, uint64_t row, gf_vec_job *j);
/* *done is 1 once every passage is embedded. */
cdb_status gf_vec_job_step(gf_db *g, const gf_books *b, gf_embed *e, gf_vec_job *j, double budget_ms, int *done);
cdb_status gf_vec_job_commit(gf_db *g, const gf_catalog *c, const gf_books *b, const gf_vec *v, gf_vec_job *j);
void gf_vec_job_free(gf_vec_job *j);

/* Opens the collections and the link type, creating them the first time. */
cdb_status gf_vec_open(gf_db *g, gf_vec *v);

/* Embeds every passage of the book in catalog row `row` and stores the
 * vectors in one transaction. CDB_E_EXISTS: it has vectors already. */
cdb_status gf_vec_embed_book(gf_db *g, const gf_catalog *c, const gf_books *b, const gf_vec *v, gf_embed *e,
                             uint64_t row, gf_embed_stats *st);

/* Links every book vector to its GF_SIMILAR nearest; adds only links that
 * are missing. *added counts them. */
cdb_status gf_vec_link_similar(gf_db *g, const gf_vec *v, uint64_t *added);

/* Refreshes the passage neighbour graph until nothing is pending, in about
 * GF_GRAPH_STEPS slices of at least GF_GRAPH_SLICE entries, calling
 * progress after each with how many are done and still pending and the
 * milliseconds so far. Each slice commits, so an interrupted refresh keeps
 * what it did. */
#define GF_GRAPH_SLICE 256u
#define GF_GRAPH_STEPS 16u
typedef void (*gf_graph_progress_fn)(size_t done, size_t pending, double ms, void *ctx);
cdb_status gf_vec_refresh_graph(gf_db *g, const gf_vec *v, gf_graph_progress_fn progress, void *ctx,
                                size_t *remaining);

/* For measuring a whole graph build (run it on a copy of the database):
 * copies every passage vector into a new collection, graph_bench, with a
 * neighbour graph, so all of them are pending. *bench is its store ID,
 * *copied how many were copied. CDB_E_EXISTS if graph_bench exists. */
cdb_status gf_vec_bench_copy(gf_db *g, const gf_vec *v, uint64_t *bench, uint64_t *copied);

/* Encodes a vector value: f32, or with bits set, its sign bits. */
size_t gf_vec_encode(const float *f, int bits, uint8_t *out, size_t cap);

/* The book_vec entry of a catalog row; *found 0 if it has none. */
cdb_status gf_vec_book_entry(gf_db *g, const gf_catalog *c, const gf_vec *v, uint64_t row, uint64_t *entry,
                             int *found);

#endif
