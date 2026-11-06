#define _POSIX_C_SOURCE 200809L
#include "gf_bench.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "gf_metrics.h"

/* Bucket i holds samples from BASE_US * 2^(i/8) up to the next bucket's
 * start; bucket 0 also holds everything smaller, the last everything
 * larger (0.05 us to about 0.05 * 2^32 us, over an hour). */
#define BASE_US 0.05
#define PER_DOUBLING 8.0

static gf_op ops[GF_OP_MAX];
static unsigned nops;

gf_op *gf_op_get(const char *name)
{
    unsigned i;
    for (i = 0; i < nops; i++)
        if (strcmp(ops[i].name, name) == 0)
            return &ops[i];
    if (nops == GF_OP_MAX)
        return NULL;
    memset(&ops[nops], 0, sizeof ops[nops]);
    snprintf(ops[nops].name, sizeof ops[nops].name, "%s", name);
    return &ops[nops++];
}

static unsigned bucket_of(double us)
{
    double b;
    if (us <= BASE_US)
        return 0;
    b = floor(log2(us / BASE_US) * PER_DOUBLING);
    return b >= (double)(GF_OP_BUCKETS - 1u) ? GF_OP_BUCKETS - 1u : (unsigned)b;
}

void gf_op_add(gf_op *op, double us)
{
    if (op == NULL)
        return;
    if (op->count == 0 || us < op->min_us)
        op->min_us = us;
    if (op->count == 0 || us > op->max_us)
        op->max_us = us;
    op->count++;
    op->sum_us += us;
    op->hist[bucket_of(us)]++;
}

/* The bucket's geometric middle, clamped to what was actually seen. */
double gf_op_quantile(const gf_op *op, double q)
{
    uint64_t want, seen = 0;
    unsigned i;
    if (op == NULL || op->count == 0)
        return 0;
    want = (uint64_t)ceil(q * (double)op->count);
    if (want == 0)
        want = 1;
    for (i = 0; i < GF_OP_BUCKETS; i++) {
        seen += op->hist[i];
        if (seen >= want) {
            double v = BASE_US * pow(2.0, ((double)i + 0.5) / PER_DOUBLING);
            return v < op->min_us ? op->min_us : v > op->max_us ? op->max_us : v;
        }
    }
    return op->max_us;
}

void gf_op_reset(gf_op *op)
{
    char name[sizeof op->name];
    memcpy(name, op->name, sizeof name);
    memset(op, 0, sizeof *op);
    memcpy(op->name, name, sizeof name);
}

double gf_t0(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec * 1e-3;
}

double gf_us_since(double t0)
{
    return gf_t0() - t0;
}

static double mark_t0;

void gf_mark(void)
{
    mark_t0 = gf_t0();
}

cdb_status gf_lap(const char *name, cdb_status s)
{
    gf_op_add(gf_op_get(name), gf_us_since(mark_t0));
    return s;
}

void gf_bench_conditions(const gf_db *g, FILE *out)
{
    fprintf(out, "conditions: build %s; accelerator %s\n", GF_BUILD_LABEL, g->accel_name);
}

void gf_stage_begin(gf_db *g, gf_stage *st, const char *name)
{
    unsigned i;
    for (i = 0; i < nops; i++)
        gf_op_reset(&ops[i]);
    memset(st, 0, sizeof *st);
    st->name = name;
    st->t0_ms = gf_t0() / 1e3;
    st->accel = g->db.accel_stats;
    st->commit_seq = g->db.pager.hs.record.committed_seq;
    st->file_pages = g->db.pager.append_point;
}

/* Microseconds as the shortest readable unit. */
static const char *fmt_us(double us, char *buf, size_t cap)
{
    if (us < 1000.0)
        snprintf(buf, cap, "%.2f us", us);
    else if (us < 1e6)
        snprintf(buf, cap, "%.2f ms", us / 1e3);
    else
        snprintf(buf, cap, "%.2f s", us / 1e6);
    return buf;
}

void gf_stage_end(gf_db *g, gf_stage *st, FILE *out)
{
    const cdb_accel_stats *a = &g->db.accel_stats;
    double secs = (gf_t0() / 1e3 - st->t0_ms) / 1e3;
    unsigned i;

    fprintf(out, "\n== %s: %.2f s\n", st->name, secs);
    gf_bench_conditions(g, out);
    fprintf(out, "%-28s %10s %10s %10s %10s %10s %10s %12s\n", "op", "count", "min", "p50", "p95", "p99", "max",
            "ops/s");
    for (i = 0; i < nops; i++) {
        const gf_op *op = &ops[i];
        char b[6][24], key[64];
        double busy;
        if (op->count == 0)
            continue;
        busy = op->sum_us / 1e6;
        fprintf(out, "%-28s %10llu %10s %10s %10s %10s %10s %12.0f\n", op->name, (unsigned long long)op->count,
                fmt_us(op->min_us, b[0], sizeof b[0]), fmt_us(gf_op_quantile(op, 0.5), b[1], sizeof b[1]),
                fmt_us(gf_op_quantile(op, 0.95), b[2], sizeof b[2]), fmt_us(gf_op_quantile(op, 0.99), b[3], sizeof b[3]),
                fmt_us(op->max_us, b[4], sizeof b[4]), busy > 0 ? (double)op->count / busy : 0.0);
        snprintf(key, sizeof key, "%.39s.p50_us", op->name);
        gf_metric(key, gf_op_quantile(op, 0.5));
        snprintf(key, sizeof key, "%.39s.p99_us", op->name);
        gf_metric(key, gf_op_quantile(op, 0.99));
        snprintf(key, sizeof key, "%.39s.mean_us", op->name);
        gf_metric(key, op->sum_us / (double)op->count);
    }
    fprintf(out, "engine: %llu archive commits, file grew %lld pages\n",
            (unsigned long long)(g->db.pager.hs.record.committed_seq - st->commit_seq),
            (long long)(g->db.pager.append_point - st->file_pages));
    fprintf(out, "accelerator: %llu searches certified, %llu widened, %llu on the CPU, %llu too small for it, "
                 "%llu mirror fills, %llu evicted\n",
            (unsigned long long)(a->certified - st->accel.certified), (unsigned long long)(a->widened - st->accel.widened),
            (unsigned long long)(a->cpu - st->accel.cpu), (unsigned long long)(a->small - st->accel.small),
            (unsigned long long)(a->fills - st->accel.fills), (unsigned long long)(a->evicted - st->accel.evicted));
    gf_metrics_flush(g);
}
