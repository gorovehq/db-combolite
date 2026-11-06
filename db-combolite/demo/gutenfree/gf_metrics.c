#define _POSIX_C_SOURCE 200809L
#include "gf_metrics.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "combolite/cdb_value.h"
#include "cdb_ts.h"

#define F_VALUE 1u
#define MAX_SERIES 256u
#define APPEND_CHUNK 32u

/* Thresholds are first guesses for a workstation: spec 2.8's device-class
 * table, which would give them per class, has no values yet. */
const gf_measure gf_measures[] = {
    { "db.open_ms", "Database open", "ms", 0, 5, 50,
      "Time to open the archive, recover the journal if needed and start a session. "
      "Long opens mean a journal had to be replayed after a crash, or a cold file cache." },
    { "catalog.batch_ms", "Catalog commit (100 rows)", "ms", 0, 5, 20,
      "Time to insert and commit one batch of 100 GUTINDEX rows into the books table: the primary-key "
      "and unique checks, the row documents, the sort-title index and the journal write. The unique "
      "check rereads the batch's own rows, so bigger batches cost more per row (BACKLOG)." },
    { "catalog.rows_per_s", "Catalog ingest rate", "rows/s", 1, 20000, 5000,
      "Rows per second over a whole GUTINDEX load, parsing included." },
    { "catalog.filter_ms", "Catalog filter", "ms", 0, 100, 1000,
      "Applying a catalog filter: walking the whole table in the chosen index order, reading and "
      "testing every row. No query planner yet (M20), so every filter is a full scan." },
    { "book.ms_per_passage", "Book ingest per passage", "ms", 0, 2, 10,
      "Time to store one ~2 KB passage: the document revision, and at commit the full-text index "
      "(terms, postings, lengths) for every word in it." },
    { "probe.pk_lookup_us", "Primary-key lookup", "us", 0, 20, 100,
      "Live probe: find a catalog row by ebook number through the primary-key index (a B+tree "
      "descent). Averaged over each probe tick." },
    { "probe.row_read_us", "Row read", "us", 0, 30, 150,
      "Live probe: read one catalog row and decode it through the current columns." },
    { "probe.passage_read_us", "Passage read", "us", 0, 50, 250,
      "Live probe: read a random passage document (head index, revision, body)." },
    { "probe.scan_us_per_row", "Index scan per row", "us", 0, 5, 25,
      "Live probe: walk 200 entries of the sort-title index, per entry." },
    { "probe.search_ms", "Full-text search", "ms", 0, 5, 50,
      "Live probe: a BM25 query of one or two common words over every passage." },
    { "embed.ms_per_passage", "Embedding per passage", "ms", 0, 10, 50,
      "llama.cpp turning one ~450-token passage into a 384-dimension vector on the CPU (bge-small, "
      "Q8_0). This is the application's model, not the database: it shows what the database has to keep up with." },
    { "vec.commit_us_per_vector", "Vector write", "us", 0, 100, 500,
      "Committing one vector: the f32 copy into a vector block with its source and the document's "
      "revision, the 48-byte sign-bit copy, and queuing it for the neighbour graph." },
    { "vec.graph_refresh_ms", "Neighbour graph refresh", "ms", 0, 1000, 10000,
      "Bringing every pending entry's stored ranking (its 16 exact nearest neighbours) up to date "
      "after an embedding run." },
    { "vec.search_ms", "Vector search (exact)", "ms", 0, 5, 50,
      "Comparing a query vector with every passage vector (cosine) and keeping the 20 nearest." },
    { "vec.approx_recall", "Graph search recall", "of 10", 1, 0.95, 0.8,
      "How many of the exact top 10 a best-first walk of the neighbour graph also finds. 1.0 is perfect." },
    { "vec.bits_agreement", "1-bit vectors agree", "of 10", 1, 0.8, 0.5,
      "How many of the exact top 10 passages a Hamming search over the 48-byte sign-bit copies also "
      "finds: what 32x smaller vectors cost in quality." },
    { "ts.flush_ms", "Telemetry write", "ms", 0, 2, 10,
      "Writing the queued metric points themselves, as one time-series transaction. The database "
      "measuring itself." },
};
const unsigned gf_nmeasures = sizeof gf_measures / sizeof gf_measures[0];

int gf_measure_band(const gf_measure *m, double v)
{
    if (m->higher_is_better)
        return v >= m->exceptional ? 2 : v >= m->satisfactory ? 1 : 0;
    return v <= m->exceptional ? 2 : v <= m->satisfactory ? 1 : 0;
}

#define NAME_MAX_LEN 48u /* names are copied: callers may build them on the stack */

typedef struct point {
    char name[NAME_MAX_LEN];
    int sent;          /* written by the flush under way */
    int64_t ts;
    double v;
} point;

static struct {
    uint64_t store;
    struct { char name[NAME_MAX_LEN]; uint64_t id; } series[MAX_SERIES];
    unsigned nseries;
    point q[GF_METRICS_QUEUE];
    unsigned nq;
    uint64_t written, dropped;
    int64_t last_ts;
    char error[160];  /* the last flush failure, shown on screen */
} M;

const char *gf_metrics_error(void) { return M.error; }

static int64_t now_us(void)
{
    struct timespec t;
    int64_t us;
    clock_gettime(CLOCK_REALTIME, &t);
    us = (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
    if (us <= M.last_ts) /* distinct timestamps: a point never replaces another */
        us = M.last_ts + 1;
    return M.last_ts = us;
}

cdb_status gf_metrics_open(gf_db *g)
{
    int found;
    cdb_status s = cdb_db_find_store(&g->db, "metrics", 7, &M.store, &found);
    M.nseries = 0;
    if (s != CDB_OK || found)
        return s;
    return cdb_ts_create(&g->db, "metrics", 7, CDB_TS_LATE_ACCEPT, 0, &M.store);
}

void gf_metric(const char *name, double value)
{
    if (M.nq == GF_METRICS_QUEUE) {
        M.dropped++;
        return;
    }
    snprintf(M.q[M.nq].name, sizeof M.q[M.nq].name, "%s", name);
    M.q[M.nq].sent = 0;
    M.q[M.nq].ts = now_us();
    M.q[M.nq].v = value;
    M.nq++;
}

static cdb_status series_id(gf_db *g, const char *name, int create, uint64_t *id)
{
    cdb_ts_def def;
    unsigned i;
    int created;
    cdb_status s;
    for (i = 0; i < M.nseries; i++)
        if (strcmp(M.series[i].name, name) == 0) {
            *id = M.series[i].id;
            return CDB_OK;
        }
    memset(&def, 0, sizeof def);
    def.precision = CDB_TS_MICROS;
    def.nfields = 1;
    def.fields[0].id = F_VALUE;
    def.fields[0].type = CDB_TYPE_F64;
    s = cdb_ts_series(&g->db, M.store, name, strlen(name), NULL, 0, create ? &def : NULL, id, &created);
    if (s == CDB_OK && M.nseries < MAX_SERIES) {
        snprintf(M.series[M.nseries].name, sizeof M.series[M.nseries].name, "%s", name);
        M.series[M.nseries].id = *id;
        M.nseries++;
    }
    return s;
}

cdb_status gf_metrics_flush(gf_db *g)
{
    /* One encoded {1: f64}: the writer also needs 16 bytes per open field
     * while it builds an object (cdb_value.h). */
    static uint8_t vals[GF_METRICS_QUEUE + 1][64];
    static cdb_ts_in in[GF_METRICS_QUEUE + 1];
    cdb_status s;
    unsigned i, done = 0;
    double t0 = gf_now_ms();

    if (M.nq == 0 || M.store == 0)
        return CDB_OK;
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    /* One append per series, of its points in order. */
    while (s == CDB_OK && done < M.nq) {
        const char *name = NULL;
        size_t n = 0;
        uint64_t id;
        for (i = 0; i < M.nq; i++) {
            cdb_value_writer w;
            if (M.q[i].sent || (name != NULL && strcmp(M.q[i].name, name) != 0))
                continue;
            name = M.q[i].name;
            cdb_value_writer_init(&w, vals[n], sizeof vals[n], GF_ORDER, CDB_DECIMAL_COMPACT);
            cdb_value_begin_object(&w);
            cdb_value_field(&w, F_VALUE);
            cdb_value_write_f64(&w, M.q[i].v);
            cdb_value_end_object(&w);
            if (w.error != CDB_OK) {
                s = w.error;
                snprintf(M.error, sizeof M.error, "encode %s: %s", name, cdb_status_str(s));
                break;
            }
            in[n].ts = M.q[i].ts;
            in[n].use_clock = 0;
            in[n].values = vals[n];
            in[n].len = cdb_value_writer_len(&w);
            n++;
            M.q[i].sent = 1;
            done++;
        }
        if (s != CDB_OK)
            break;
        if ((s = series_id(g, name, 1, &id)) != CDB_OK) {
            snprintf(M.error, sizeof M.error, "series %s (store %llu): %s", name, (unsigned long long)M.store,
                     cdb_status_str(s));
            break;
        }
        /* One append is one journal record, at most a page (BACKLOG, M16
         * limits): send the points in small chunks. */
        for (size_t off = 0; s == CDB_OK && off < n; off += APPEND_CHUNK) {
            size_t k = n - off < APPEND_CHUNK ? n - off : APPEND_CHUNK;
            if ((s = cdb_ts_append(&g->db, M.store, id, in + off, k)) != CDB_OK)
                snprintf(M.error, sizeof M.error, "append to %s (series %llu, %zu points): %s", name,
                         (unsigned long long)id, k, cdb_status_str(s));
            else
                M.written += k;
        }
    }
    M.nq = 0;
    if (s != CDB_OK) {
        cdb_tx_abort(&g->db);
        M.nseries = 0; /* series created in the aborted transaction are gone */
        return s;
    }
    M.error[0] = '\0';
    if ((s = cdb_tx_commit(&g->db)) != CDB_OK)
        return s;
    gf_metric("ts.flush_ms", gf_now_ms() - t0); /* written with the next flush */
    return CDB_OK;
}

/* Calls fn for every point of a series, oldest first. */
static cdb_status walk(gf_db *g, const char *name, void (*fn)(double v, void *ctx), void *ctx)
{
    static uint8_t buf[64 * 1024];
    static cdb_ts_row rows[1024];
    int64_t from = 0, next, to = INT64_MAX;
    uint64_t id;
    size_t got, i;
    cdb_status s = series_id(g, name, 0, &id);

    if (s == CDB_E_NOT_FOUND)
        return CDB_OK;
    while (s == CDB_OK && from < to) {
        if ((s = cdb_ts_range(&g->db, M.store, id, from, to, buf, sizeof buf, rows, 1024, &got, &next)) != CDB_OK)
            break;
        for (i = 0; i < got; i++) {
            cdb_value obj, v;
            int found = 0;
            if (cdb_value_read(buf + rows[i].off, rows[i].len, GF_ORDER, &obj) != CDB_OK
                || cdb_value_object_get(&obj, GF_ORDER, F_VALUE, &v, &found) != CDB_OK || !found)
                continue;
            fn(v.as.f64, ctx);
        }
        if (got == 0 || next <= from)
            break;
        from = next;
    }
    return s;
}

typedef struct recent_ctx {
    double *out;
    size_t max, keep, head;
    uint64_t points;
} recent_ctx;

static void keep_recent(double v, void *ctx)
{
    recent_ctx *r = (recent_ctx *)ctx;
    r->out[(r->head + r->keep) % r->max] = v;
    if (r->keep < r->max)
        r->keep++;
    else
        r->head = (r->head + 1) % r->max;
    r->points++;
}

cdb_status gf_metrics_recent(gf_db *g, const char *name, double *out, size_t max, size_t *n, uint64_t *points,
                             double *read_ms)
{
    recent_ctx r = { out, max, 0, 0, 0 };
    double t0 = gf_now_ms();
    cdb_status s = walk(g, name, keep_recent, &r);
    size_t i;

    /* Unroll the ring, oldest first. */
    if (r.head != 0) {
        double tmp[512];
        for (i = 0; i < r.keep && i < 512; i++)
            tmp[i] = out[(r.head + i) % max];
        memcpy(out, tmp, r.keep * sizeof *out);
    }
    *n = r.keep;
    *points = r.points;
    *read_ms = gf_now_ms() - t0;
    return s;
}

static void add_summary(double v, void *ctx)
{
    gf_metric_summary *m = (gf_metric_summary *)ctx;
    if (m->count == 0 || v < m->min)
        m->min = v;
    if (m->count == 0 || v > m->max)
        m->max = v;
    m->sum += v;
    m->last = v;
    m->count++;
}

cdb_status gf_metrics_summary(gf_db *g, const char *name, gf_metric_summary *out)
{
    memset(out, 0, sizeof *out);
    return walk(g, name, add_summary, out);
}


uint64_t gf_metrics_written(void) { return M.written; }
uint64_t gf_metrics_dropped(void) { return M.dropped; }
