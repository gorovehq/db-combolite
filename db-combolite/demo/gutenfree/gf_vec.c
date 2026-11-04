#define _POSIX_C_SOURCE 200809L
#include "gf_vec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cdb_link.h"
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
        if ((s = gf_books_passage(g, b, j->bk.first_passage + j->i, text, sizeof text, &eb, &seq)) != CDB_OK)
            return s;
        if (gf_embed_text(e, text, strlen(text), j->vecs[j->i], &tokens) != 0)
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

    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    for (i = 0; s == CDB_OK && i < j->n; i++) {
        len = gf_vec_encode(j->vecs[i], 0, enc, sizeof enc);
        s = cdb_vec_insert(&g->db, v->passages, enc, len, b->pool, j->bk.first_passage + i, &entry);
        if (s == CDB_OK) {
            len = gf_vec_encode(j->vecs[i], 1, enc, sizeof enc);
            s = cdb_vec_insert(&g->db, v->bits, enc, len, b->pool, j->bk.first_passage + i, &entry);
        }
    }
    if (s == CDB_OK) {
        len = gf_vec_encode(book, 0, enc, sizeof enc);
        s = cdb_vec_insert(&g->db, v->books, enc, len, c->lib_store, j->bk.lib_row, &entry);
    }
    if (s == CDB_OK)
        s = gf_library_set_embedded(g, c, j->bk.lib_row, MODEL " " VERSION);
    if (s != CDB_OK) {
        cdb_tx_abort(&g->db);
        return s;
    }
    s = cdb_tx_commit(&g->db);
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

cdb_status gf_vec_refresh_graph(gf_db *g, const gf_vec *v, gf_graph_progress_fn progress, void *ctx,
                                size_t *remaining)
{
    double t0 = gf_now_ms();
    size_t done = 0, pending = 0, before;
    cdb_status s = cdb_vec_refresh(&g->db, v->passages, 1, &pending); /* learns the size of the work */

    if (s == CDB_OK)
        done = 1;
    while (s == CDB_OK && pending > 0) {
        before = pending;
        s = cdb_vec_refresh(&g->db, v->passages, GF_GRAPH_SLICE, &pending);
        done += before > pending ? before - pending : 0;
        if (progress != NULL)
            progress(done, pending, gf_now_ms() - t0, ctx);
    }
    if (remaining != NULL)
        *remaining = pending;
    return s;
}

cdb_status gf_vec_link_similar(gf_db *g, const gf_vec *v, uint64_t *added)
{
    static uint8_t enc[VEC_BYTES];
    cdb_vec_hit hits[GF_SIMILAR + 1];
    uint64_t entry = 0, src_store, src_entry;
    size_t len, n, i;
    cdb_status s = CDB_OK;

    *added = 0;
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    /* Book vectors have entry IDs 1, 2, ...: walk until one is missing. The
     * links join the books themselves, the library rows the vectors come
     * from, so they read as "this book is similar to that one". */
    for (entry = 1; s == CDB_OK; entry++) {
        cdb_ref from, to;
        s = cdb_vec_get(&g->db, v->books, entry, enc, sizeof enc, &len, &src_store, &src_entry);
        if (s == CDB_E_NOT_FOUND) {
            s = CDB_OK;
            break;
        }
        if (s != CDB_OK || (s = cdb_vec_search(&g->db, v->books, enc, len, CDB_VEC_BASE, hits, GF_SIMILAR + 1, &n))
                               != CDB_OK)
            break;
        from = cdb_ref_entry(src_store, src_entry);
        for (i = 0; i < n; i++) {
            cdb_status ls;
            if (hits[i].entry == entry)
                continue;
            to = cdb_ref_entry(hits[i].source_store, hits[i].source_entry);
            ls = cdb_link_add(&g->db, v->similar, from, to, (float)(1.0 - hits[i].distance), 0);
            if (ls == CDB_OK)
                (*added)++;
            else if (ls != CDB_E_EXISTS)
                s = ls;
        }
    }
    if (s != CDB_OK) {
        cdb_tx_abort(&g->db);
        return s;
    }
    return cdb_tx_commit(&g->db);
}
