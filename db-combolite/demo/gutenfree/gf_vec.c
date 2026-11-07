#define _POSIX_C_SOURCE 200809L
#include "gf_vec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cdb_endian.h"
#include "cdb_link.h"
#include "gf_bench.h"
#include "gf_metrics.h"
#include "cdb_vec.h"

#define MODEL "bge-small-en-v1.5"
#define VERSION "gguf-q8_0"
#define VEC_BYTES 2048u

static cdb_status collection(gf_db *g, const char *name, cdb_vector_elem elem, unsigned measure, int graph,
                             uint64_t *id)
{
    cdb_vec_config cfg;
    cdb_status s;
    int found;
    if ((s = cdb_db_find_store(&g->db, name, strlen(name), id, &found)) != CDB_OK || found)
        return s;
    memset(&cfg, 0, sizeof cfg);
    cfg.elem = elem;
    cfg.dims = GF_EMBED_DIMS;
    cfg.measure = measure;
    cfg.model = MODEL, cfg.model_len = strlen(MODEL);
    cfg.model_version = VERSION, cfg.model_version_len = strlen(VERSION);
    if ((s = cdb_vec_create(&g->db, name, strlen(name), &cfg, id)) != CDB_OK)
        return s;
    return graph ? cdb_vec_create_graph(&g->db, *id, GF_VEC_K) : CDB_OK;
}

cdb_status gf_vec_open(gf_db *g, gf_vec *v)
{
    cdb_status s;
    int found;
    if ((s = collection(g, "passage_vec", CDB_VEC_F32, CDB_VEC_COSINE, 1, &v->passages)) != CDB_OK
        || (s = collection(g, "passage_bits", CDB_VEC_BINARY, CDB_VEC_HAMMING, 0, &v->bits)) != CDB_OK
        || (s = collection(g, "book_vec", CDB_VEC_F32, CDB_VEC_COSINE, 0, &v->books)) != CDB_OK)
        return s;
    if ((s = cdb_link_find_type(&g->db, "similar_to", 10, &v->similar, &found)) != CDB_OK || found)
        return s;
    return cdb_link_define_type(&g->db, "similar_to", 10, CDB_CARD_N_M, 0, CDB_LINK_ON_DELETE_TOMBSTONE,
                                &v->similar);
}

size_t gf_vec_encode(const float *f, int bits, uint8_t *out, size_t cap)
{
    cdb_value_writer w;
    cdb_value_writer_init(&w, out, cap, GF_ORDER, CDB_DECIMAL_COMPACT);
    if (bits) {
        uint8_t packed[GF_EMBED_DIMS / 8];
        unsigned i;
        memset(packed, 0, sizeof packed);
        for (i = 0; i < GF_EMBED_DIMS; i++)
            if (f[i] > 0.0f)
                packed[i / 8] = (uint8_t)(packed[i / 8] | (1u << (i % 8))); /* LSB first */
        cdb_value_write_vector(&w, CDB_VEC_BINARY, GF_EMBED_DIMS, packed);
    } else {
        cdb_value_write_vector(&w, CDB_VEC_F32, GF_EMBED_DIMS, f);
    }
    return w.error == CDB_OK ? cdb_value_writer_len(&w) : 0;
}

cdb_status gf_vec_book_entry(gf_db *g, const gf_catalog *c, const gf_vec *v, uint64_t row, uint64_t *entry,
                             int *found)
{
    static gf_book b;
    size_t n = 0;
    cdb_status s = gf_catalog_read(g, c, row, &b);

    *found = 0;
    if (s != CDB_OK || b.lib_row == 0)
        return s;
    /* A book vector's source is the book's library row */
    s = cdb_vec_by_source(&g->db, v->books, c->lib_store, b.lib_row, entry, 1, &n);
    *found = s == CDB_OK && n > 0;
    return s;
}

#define JOB_MAX_PASSAGES 8192u

void gf_vec_job_free(gf_vec_job *j)
{
    free(j->vecs);
    j->vecs = NULL;
}

cdb_status gf_vec_job_start(gf_db *g, const gf_catalog *c, const gf_vec *v, uint64_t row, gf_vec_job *j)
{
    uint64_t entry;
    int found;
    cdb_status s;

    memset(j, 0, sizeof *j);
    j->row = row;
    if ((s = gf_catalog_read(g, c, row, &j->bk)) != CDB_OK)
        return s;
    if (j->bk.lib_row == 0 || j->bk.passages == 0)
        return CDB_E_NOT_FOUND;
    if ((s = gf_vec_book_entry(g, c, v, row, &entry, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;
    j->n = j->bk.passages < JOB_MAX_PASSAGES ? j->bk.passages : JOB_MAX_PASSAGES;
    j->vecs = malloc(j->n * sizeof *j->vecs);
    return j->vecs != NULL ? CDB_OK : CDB_E_BUFFER_TOO_SMALL;
}

cdb_status gf_vec_job_step(gf_db *g, const gf_books *b, gf_embed *e, gf_vec_job *j, double budget_ms, int *done)
{
    static char text[GF_PASSAGE_MAX + 1];
    double t0 = gf_now_ms();
    cdb_status s;
    int tokens;

    while (j->i < j->n && gf_now_ms() - t0 < budget_ms) {
        int64_t eb, seq;
        int rc;
        GF_TIME("doc.passage_read", s = gf_books_passage(g, b, j->bk.first_passage + j->i, text, sizeof text, &eb, &seq));
        if (s != CDB_OK)
            return s;
        GF_TIME("embed.passage", rc = gf_embed_text(e, text, strlen(text), j->vecs[j->i], &tokens));
        if (rc != 0)
            return CDB_E_STATE;
        j->st.tokens += (uint64_t)tokens;
        for (unsigned d = 0; d < GF_EMBED_DIMS; d++)
            j->mean[d] += j->vecs[j->i][d];
        j->i++;
    }
    j->st.embed_ms += gf_now_ms() - t0;
    *done = j->i == j->n;
    return CDB_OK;
}

cdb_status gf_vec_job_commit(gf_db *g, const gf_catalog *c, const gf_books *b, const gf_vec *v, gf_vec_job *j)
{
    static uint8_t enc[VEC_BYTES];
    float book[GF_EMBED_DIMS];
    double norm = 0, t = gf_now_ms();
    uint64_t i, entry;
    size_t len;
    cdb_status s;

    for (unsigned d = 0; d < GF_EMBED_DIMS; d++)
        norm += j->mean[d] * j->mean[d];
    norm = norm > 0 ? sqrt(norm) : 1.0;
    for (unsigned d = 0; d < GF_EMBED_DIMS; d++)
        book[d] = (float)(j->mean[d] / norm);

    GF_TIME("tx.begin", s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT));
    if (s != CDB_OK)
        return s;
    for (i = 0; s == CDB_OK && i < j->n; i++) {
        len = gf_vec_encode(j->vecs[i], 0, enc, sizeof enc);
        GF_TIME("vec.insert_f32", s = cdb_vec_insert(&g->db, v->passages, enc, len, b->pool, j->bk.first_passage + i, &entry));
        if (s == CDB_OK) {
            len = gf_vec_encode(j->vecs[i], 1, enc, sizeof enc);
            GF_TIME("vec.insert_bits", s = cdb_vec_insert(&g->db, v->bits, enc, len, b->pool, j->bk.first_passage + i, &entry));
        }
    }
    if (s == CDB_OK) {
        len = gf_vec_encode(book, 0, enc, sizeof enc);
        GF_TIME("vec.insert_book", s = cdb_vec_insert(&g->db, v->books, enc, len, c->lib_store, j->bk.lib_row, &entry));
    }
    if (s == CDB_OK)
        GF_TIME("library.set_embedded", s = gf_library_set_embedded(g, c, j->bk.lib_row, MODEL " " VERSION));
    if (s != CDB_OK) {
        cdb_tx_abort(&g->db);
        return s;
    }
    GF_TIME("tx.commit_vectors", s = cdb_tx_commit(&g->db));
    j->st.commit_ms = gf_now_ms() - t;
    j->st.passages = j->n;
    return s;
}

cdb_status gf_vec_embed_book(gf_db *g, const gf_catalog *c, const gf_books *b, const gf_vec *v, gf_embed *e,
                             uint64_t row, gf_embed_stats *st)
{
    gf_vec_job j;
    double t0 = gf_now_ms();
    int done = 0;
    cdb_status s = gf_vec_job_start(g, c, v, row, &j);

    memset(st, 0, sizeof *st);
    while (s == CDB_OK && !done)
        s = gf_vec_job_step(g, b, e, &j, 1e12, &done);
    if (s == CDB_OK)
        s = gf_vec_job_commit(g, c, b, v, &j);
    *st = j.st;
    st->ms = gf_now_ms() - t0;
    gf_vec_job_free(&j);
    return s;
}

cdb_status gf_vec_bench_copy(gf_db *g, const gf_vec *v, uint64_t *bench, uint64_t *copied)
{
    static uint8_t enc[VEC_BYTES];
    uint64_t entry, src_store, src_entry, id, misses = 0;
    size_t len;
    cdb_status s;
    int found;

    *copied = 0;
    if ((s = cdb_db_find_store(&g->db, "graph_bench", 11, bench, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;
    if ((s = collection(g, "graph_bench", CDB_VEC_F32, CDB_VEC_COSINE, 1, bench)) != CDB_OK)
        return s;
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    /* Entry IDs are claimed in blocks, so there can be gaps: stop once
     * more are missing in a row than every block a process holds. */
    for (entry = 1; misses < (uint64_t)CDB_COORD_ID_BLOCK * CDB_COORD_MAX_BLOCKS; entry++) {
        s = cdb_vec_get(&g->db, v->passages, entry, enc, sizeof enc, &len, &src_store, &src_entry);
        if (s == CDB_E_NOT_FOUND) {
            misses++;
            continue;
        }
        misses = 0;
        if (s != CDB_OK || (s = cdb_vec_insert(&g->db, *bench, enc, len, src_store, src_entry, &id)) != CDB_OK)
            break;
        if (++*copied % 500u == 0
            && ((s = cdb_tx_commit(&g->db)) != CDB_OK || (s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK))
            return s;
    }
    if (s != CDB_OK && s != CDB_E_NOT_FOUND) {
        cdb_tx_abort(&g->db);
        return s;
    }
    return cdb_tx_commit(&g->db);
}

cdb_status gf_vec_refresh_graph(gf_db *g, const gf_vec *v, gf_graph_progress_fn progress, void *ctx,
                                size_t *remaining)
{
    double t0 = gf_now_ms();
    size_t done = 0, pending = 0, before;
    cdb_status s = cdb_vec_refresh(&g->db, v->passages, 1, &pending); /* learns the size of the work */

    if (s == CDB_OK)
        done = 1;
    /* About GF_GRAPH_STEPS slices, never smaller than GF_GRAPH_SLICE: each
     * slice is a pass over every vector, so fewer, larger ones finish
     * sooner, and give an accelerator more to do per pass. */
    size_t slice = pending / GF_GRAPH_STEPS > GF_GRAPH_SLICE ? pending / GF_GRAPH_STEPS : GF_GRAPH_SLICE;
    while (s == CDB_OK && pending > 0) {
        double ts = gf_t0(), us;
        before = pending;
        s = cdb_vec_refresh(&g->db, v->passages, slice, &pending);
        us = gf_us_since(ts);
        gf_op_add(gf_op_get("vec.graph_slice"), us);
        if (before > pending) /* one sample per slice: its cost per entry */
            gf_op_add(gf_op_get("vec.graph_per_entry"), us / (double)(before - pending));
        done += before > pending ? before - pending : 0;
        if (progress != NULL)
            progress(done, pending, gf_now_ms() - t0, ctx);
    }
    if (remaining != NULL)
        *remaining = pending;
    return s;
}

/* How many of the first n of a are among the first n of b, by source. */
static unsigned agree(const cdb_vec_hit *a, size_t na, const cdb_vec_hit *b, size_t nb, size_t n)
{
    unsigned k = 0;
    for (size_t i = 0; i < nb && i < n; i++)
        for (size_t j = 0; j < na && j < n; j++)
            if (a[j].source_entry == b[i].source_entry) {
                k++;
                break;
            }
    return k;
}

static uint32_t next_rand(uint32_t *r)
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return *r;
}

/* The highest passage vector entry ID: they are dense from 1, apart from
 * unclaimed ends of ID blocks, which callers skip as misses. */
static uint64_t last_passage(gf_db *g, const gf_vec *v, uint8_t *enc, size_t cap)
{
    uint64_t hi = 1, lo, ss, se;
    size_t len;
    while (cdb_vec_get(&g->db, v->passages, hi * 2u, enc, cap, &len, &ss, &se) == CDB_OK)
        hi *= 2u;
    for (lo = hi, hi *= 2u; hi - lo > 1u;) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (cdb_vec_get(&g->db, v->passages, mid, enc, cap, &len, &ss, &se) == CDB_OK)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

cdb_status gf_vec_stored_queries(gf_db *g, const gf_vec *v, unsigned n, uint32_t *rng, float (*out)[GF_EMBED_DIMS],
                                 unsigned *got)
{
    static uint8_t enc[VEC_BYTES];
    uint64_t lo, ss, se;
    size_t len;
    unsigned tries = 0;

    *got = 0;
    if (*rng == 0)
        *rng = 2463534242u;
    lo = last_passage(g, v, enc, sizeof enc);
    while (*got < n && tries++ < n * 4u) {
        cdb_value val;
        cdb_status s = cdb_vec_get(&g->db, v->passages, 1u + next_rand(rng) % lo, enc, sizeof enc, &len, &ss, &se);
        if (s == CDB_E_NOT_FOUND)
            continue;
        if (s != CDB_OK)
            return s;
        if (cdb_value_read(enc, len, GF_ORDER, &val) != CDB_OK || val.as.vector.dims != GF_EMBED_DIMS
            || val.as.vector.elem != CDB_VEC_F32)
            return CDB_E_CORRUPT;
        for (unsigned d = 0; d < GF_EMBED_DIMS; d++) {
            uint32_t u = cdb_load_u32(val.as.vector.data + 4u * d, GF_ORDER);
            memcpy(&out[*got][d], &u, sizeof out[*got][d]);
        }
        (*got)++;
    }
    return CDB_OK;
}

/* A cold mirror sends the first searches to the CPU: fill it first (on the
 * worker pool, finished by ticks), timed as its own op, so the searches
 * measured are the ones a warm database does. */
static cdb_status warm_mirror(gf_db *g, const gf_vec *v, const uint8_t *enc, size_t len)
{
    uint64_t fills = g->db.accel_stats.fills, certified = g->db.accel_stats.certified;
    cdb_vec_hit hits[10];
    size_t n;
    double t0 = gf_t0();
    cdb_status s;

    static int warm;
    if (g->env.accel == NULL || warm)
        return CDB_OK;
    warm = 1;
    if ((s = cdb_vec_search_mode(&g->db, v->passages, enc, len, CDB_VEC_BASE, CDB_VEC_EXACT, hits, 10, &n)) != CDB_OK)
        return s;
    if (g->db.accel_stats.certified != certified)
        return CDB_OK; /* answered on the device: warm already */
    while (g->db.accel_stats.fills == fills && gf_us_since(t0) < 60e6)
        if ((s = cdb_db_tick(&g->db)) != CDB_OK)
            return s;
    if (g->db.accel_stats.fills != fills)
        gf_op_add(gf_op_get("vec.mirror_fill"), gf_us_since(t0));
    return CDB_OK;
}

cdb_status gf_vec_check_search(gf_db *g, const gf_vec *v, const float (*q)[GF_EMBED_DIMS], unsigned nq,
                               const char *set, gf_vec_check *out)
{
    static uint8_t enc[VEC_BYTES], benc[VEC_BYTES];
    cdb_vec_hit exact[10], graph[10], bits[10];
    char op_exact[40], op_graph[40], op_bits[40];
    size_t len, blen, ne, na, nb;
    unsigned i;
    cdb_status s;

    memset(out, 0, sizeof *out);
    out->recall_min = out->bits_min = 10;
    snprintf(op_exact, sizeof op_exact, "vec.search_exact.%s", set);
    snprintf(op_graph, sizeof op_graph, "vec.search_graph.%s", set);
    snprintf(op_bits, sizeof op_bits, "vec.search_bits.%s", set);
    for (i = 0; i < nq; i++) {
        unsigned ra, rb;
        len = gf_vec_encode(q[i], 0, enc, sizeof enc);
        blen = gf_vec_encode(q[i], 1, benc, sizeof benc);
        if (i == 0 && (s = warm_mirror(g, v, enc, len)) != CDB_OK)
            return s;
        GF_TIME(op_exact, s = cdb_vec_search_mode(&g->db, v->passages, enc, len, CDB_VEC_BASE, CDB_VEC_EXACT, exact,
                                                   10, &ne));
        if (s != CDB_OK)
            return s;
        GF_TIME(op_graph, s = cdb_vec_search_mode(&g->db, v->passages, enc, len, CDB_VEC_BASE, CDB_VEC_APPROX, graph,
                                                   10, &na));
        if (s != CDB_OK)
            return s;
        GF_TIME(op_bits, s = cdb_vec_search(&g->db, v->bits, benc, blen, CDB_VEC_BASE, bits, 10, &nb));
        if (s != CDB_OK)
            return s;
        ra = agree(exact, ne, graph, na, 10);
        rb = agree(exact, ne, bits, nb, 10);
        out->recall += ra;
        out->bits_agree += rb;
        if (ra < out->recall_min) {
            out->recall_min = ra;
            out->worst_query = i;
        }
        if (rb < out->bits_min)
            out->bits_min = rb;
        gf_metric("vec.approx_recall", ra / 10.0);
        gf_metric("vec.bits_agreement", rb / 10.0);
        out->queries++;
    }
    if (out->queries > 0) {
        out->recall /= out->queries;
        out->bits_agree /= out->queries;
    }
    return CDB_OK;
}

/* One book vector, read for the bulk search below. */
typedef struct book_vec {
    uint8_t enc[VEC_BYTES];
    size_t len;
    uint64_t src_store, src_entry;
} book_vec;

cdb_status gf_vec_link_similar(gf_db *g, const gf_vec *v, uint64_t *added)
{
    book_vec *bv = NULL;
    const void **qp = NULL;
    size_t *ql = NULL, *counts = NULL, nb = 0, cap = 0, b, i;
    cdb_vec_hit *hits = NULL;
    cdb_status s = CDB_OK;

    *added = 0;
    /* Book vectors have entry IDs 1, 2, ...: read until one is missing. */
    for (;;) {
        if (nb == cap) {
            size_t ncap = cap ? cap * 2u : 256u;
            book_vec *nbv = (book_vec *)realloc(bv, ncap * sizeof *bv);
            if (nbv == NULL) {
                s = CDB_E_OUT_OF_BUDGET;
                goto out;
            }
            bv = nbv;
            cap = ncap;
        }
        GF_TIME("vec.get", s = cdb_vec_get(&g->db, v->books, (uint64_t)nb + 1u, bv[nb].enc, sizeof bv[nb].enc,
                                           &bv[nb].len, &bv[nb].src_store, &bv[nb].src_entry));
        if (s == CDB_E_NOT_FOUND) {
            s = CDB_OK;
            break;
        }
        if (s != CDB_OK)
            goto out;
        nb++;
    }
    if (nb == 0)
        goto out;
    /* Every book's nearest at once: one bulk search (spec 18.4), on the
     * accelerator when there is one. */
    qp = (const void **)malloc(nb * sizeof *qp);
    ql = (size_t *)malloc(nb * sizeof *ql);
    counts = (size_t *)malloc(nb * sizeof *counts);
    hits = (cdb_vec_hit *)malloc(nb * (GF_SIMILAR + 1u) * sizeof *hits);
    if (qp == NULL || ql == NULL || counts == NULL || hits == NULL) {
        s = CDB_E_OUT_OF_BUDGET;
        goto out;
    }
    for (b = 0; b < nb; b++) {
        qp[b] = bv[b].enc;
        ql[b] = bv[b].len;
    }
    GF_TIME("vec.search_bulk", s = cdb_vec_search_bulk(&g->db, v->books, qp, ql, nb, CDB_VEC_BASE, CDB_VEC_AUTO, hits,
                                                       GF_SIMILAR + 1u, counts));
    if (s != CDB_OK)
        goto out;
    /* The links join the books themselves, the library rows the vectors
     * come from, so they read as "this book is similar to that one". */
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        goto out;
    for (b = 0; b < nb && s == CDB_OK; b++) {
        const cdb_vec_hit *h = hits + b * (GF_SIMILAR + 1u);
        cdb_ref from = cdb_ref_entry(bv[b].src_store, bv[b].src_entry);
        for (i = 0; i < counts[b]; i++) {
            cdb_status ls;
            if (h[i].entry == (uint64_t)b + 1u)
                continue;
            GF_TIME("link.add_similar", ls = cdb_link_add(&g->db, v->similar, from,
                                                           cdb_ref_entry(h[i].source_store, h[i].source_entry),
                                                           (float)(1.0 - h[i].distance), 0));
            if (ls == CDB_OK)
                (*added)++;
            else if (ls != CDB_E_EXISTS)
                s = ls;
        }
    }
    if (s != CDB_OK)
        cdb_tx_abort(&g->db);
    else
        GF_TIME("tx.commit_links", s = cdb_tx_commit(&g->db));
out:
    free(bv);
    free(qp);
    free(ql);
    free(counts);
    free(hits);
    return s;
}
