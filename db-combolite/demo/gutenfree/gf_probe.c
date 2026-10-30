#define _POSIX_C_SOURCE 200809L
#include "gf_probe.h"

#include <string.h>

#include "gf_metrics.h"
#include "cdb_doc.h"

static uint32_t rnd(gf_probe *p)
{
    p->rng ^= p->rng << 13;
    p->rng ^= p->rng >> 17;
    p->rng ^= p->rng << 5;
    return p->rng;
}

void gf_probe_init(gf_app *a, gf_probe *p)
{
    cdb_table_cursor cur;
    uint64_t shelf[GF_PROBE_BOOKS];
    size_t n = 0, i;

    memset(p, 0, sizeof *p);
    p->rng = 0x9e3779b9u;
    memset(&cur, 0, sizeof cur);
    gf_catalog_shelf(a->g, &a->cat, &cur, shelf, GF_PROBE_BOOKS, &n);
    for (i = 0; i < n; i++) {
        gf_book b;
        if (gf_catalog_read(a->g, &a->cat, shelf[i], &b) == CDB_OK && b.passages > 0) {
            p->first[p->nbooks] = b.first_passage;
            p->count[p->nbooks] = b.passages;
            p->nbooks++;
        }
    }
}

void gf_probe_tick(gf_app *a, gf_probe *p)
{
    static const char *const words[] = { "love", "war", "sea", "god", "night", "king", "river", "gold",
                                         "death", "light", "house", "horse", "ship", "fire", "heart", "old man" };
    static char text[GF_PASSAGE_MAX + 1];
    uint64_t rows[200], e;
    cdb_fts_hit hits[20];
    size_t n;
    int found, i, hit = 0;
    double t0;

    t0 = gf_now_ms();
    for (i = 0; i < 200; i++) {
        gf_catalog_find(a->g, &a->cat, (int64_t)(rnd(p) % 79900u) + 1, &e, &found);
        if (found && hit < 50)
            rows[hit++] = e;
    }
    gf_metric("probe.pk_lookup_us", (gf_now_ms() - t0) * 1000.0 / 200.0);

    if (hit > 0) {
        gf_book b;
        t0 = gf_now_ms();
        for (i = 0; i < hit; i++)
            gf_catalog_read(a->g, &a->cat, rows[i], &b);
        gf_metric("probe.row_read_us", (gf_now_ms() - t0) * 1000.0 / hit);
    }

    if (p->nbooks > 0) {
        int64_t eb, seq;
        t0 = gf_now_ms();
        for (i = 0; i < 50; i++) {
            unsigned k = rnd(p) % p->nbooks;
            gf_books_passage(a->g, &a->books, p->first[k] + rnd(p) % p->count[k], text, sizeof text, &eb, &seq);
        }
        gf_metric("probe.passage_read_us", (gf_now_ms() - t0) * 1000.0 / 50.0);
    }

    t0 = gf_now_ms();
    if (cdb_table_scan(&a->g->db, a->cat.store, GF_CATALOG_SORT_INDEX, NULL, 0, NULL, 0, &p->scan, rows, 200, &n)
            == CDB_OK
        && n > 0)
        gf_metric("probe.scan_us_per_row", (gf_now_ms() - t0) * 1000.0 / (double)n);
    else
        memset(&p->scan, 0, sizeof p->scan); /* the end: start over */

    for (i = 0; i < 2; i++) {
        const char *q = words[rnd(p) % (sizeof words / sizeof words[0])];
        t0 = gf_now_ms();
        if (cdb_doc_search(&a->g->db, a->books.pool, q, strlen(q), CDB_FTS_ANY, hits, 20, &n) == CDB_OK)
            gf_metric("probe.search_ms", gf_now_ms() - t0);
    }
    gf_metrics_flush(a->g);
}
