#define _POSIX_C_SOURCE 200809L
#include "gf_bench.h"

#include <math.h>
#include <pthread.h>
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
/* Ops are recorded from the pipeline's threads too. */
static pthread_mutex_t ops_lock = PTHREAD_MUTEX_INITIALIZER;

gf_op *gf_op_get(const char *name)
{
    gf_op *op = NULL;
    unsigned i;
    pthread_mutex_lock(&ops_lock);
    for (i = 0; i < nops && op == NULL; i++)
        if (strcmp(ops[i].name, name) == 0)
            op = &ops[i];
    if (op == NULL && nops < GF_OP_MAX) {
        memset(&ops[nops], 0, sizeof ops[nops]);
        snprintf(ops[nops].name, sizeof ops[nops].name, "%s", name);
        op = &ops[nops++];
    }
    pthread_mutex_unlock(&ops_lock);
    return op;
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
    pthread_mutex_lock(&ops_lock);
    if (op->count == 0 || us < op->min_us)
        op->min_us = us;
    if (op->count == 0 || us > op->max_us)
        op->max_us = us;
    op->count++;
    op->sum_us += us;
    op->hist[bucket_of(us)]++;
    pthread_mutex_unlock(&ops_lock);
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

int gf_bench_dump(FILE *f)
{
    unsigned i, n = 0;
    pthread_mutex_lock(&ops_lock);
    for (i = 0; i < nops; i++)
        n += ops[i].count > 0;
    if (fwrite(&n, sizeof n, 1, f) != 1) {
        pthread_mutex_unlock(&ops_lock);
        return -1;
    }
    for (i = 0; i < nops; i++)
        if (ops[i].count > 0 && fwrite(&ops[i], sizeof ops[i], 1, f) != 1) {
            pthread_mutex_unlock(&ops_lock);
            return -1;
        }
    pthread_mutex_unlock(&ops_lock);
    return 0;
}

int gf_bench_merge(FILE *f)
{
    unsigned n, i, b;
    gf_op in, *op;
    if (fread(&n, sizeof n, 1, f) != 1)
        return -1;
    for (i = 0; i < n; i++) {
        if (fread(&in, sizeof in, 1, f) != 1)
            return -1;
        in.name[sizeof in.name - 1] = '\0';
        if ((op = gf_op_get(in.name)) == NULL || in.count == 0)
            continue;
        pthread_mutex_lock(&ops_lock);
        if (op->count == 0 || in.min_us < op->min_us)
            op->min_us = in.min_us;
        if (op->count == 0 || in.max_us > op->max_us)
            op->max_us = in.max_us;
        op->count += in.count;
        op->sum_us += in.sum_us;
        for (b = 0; b < GF_OP_BUCKETS; b++)
            op->hist[b] += in.hist[b];
        pthread_mutex_unlock(&ops_lock);
    }
    return 0;
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
            (long long)g->db.pager.append_point - (long long)st->file_pages);
    /* A handle reopened during the stage (the pipeline) restarts its
     * counters: then the counts are since the reopen. */
#define SINCE(f) (unsigned long long)(a->f >= st->accel.f ? a->f - st->accel.f : a->f)
    fprintf(out, "accelerator (this process): %llu searches certified, %llu widened, %llu on the CPU, %llu too small "
                 "for it, %llu mirror fills, %llu evicted\n",
            SINCE(certified), SINCE(widened), SINCE(cpu), SINCE(small), SINCE(fills), SINCE(evicted));
#undef SINCE
    gf_metrics_flush(g);
}
