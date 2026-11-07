/* GutenFree Diet: measuring the engine one operation at a time.
 *
 * Every timed operation (a lookup, an insert, a search, a commit) goes into
 * the histogram of its named op. A histogram has 8 buckets per doubling
 * from 0.05 us up, so recording costs a log and an add, and its quantiles
 * are within about 9% of the true value. Ops live in one registry, found by
 * name; the demo is one process with one measuring thread at a time.
 *
 * A stage brackets a command's work: it snapshots the engine's counters
 * (accelerator searches, mirror fills, commits, file growth) at its start
 * and reports their change at its end, with every op touched in between,
 * so a report says how the work was done as well as how long it took.
 * gf_bench_report prints the table and stores each op's summary as metric
 * points (<op>.p50 and so on), so `gutenfree metrics` shows them later. */
#ifndef GF_BENCH_H
#define GF_BENCH_H

#include <stdint.h>
#include <stdio.h>

#include "gf_db.h"

#define GF_OP_BUCKETS 256u
#define GF_OP_MAX 64u

typedef struct gf_op {
    char name[40];
    uint64_t count;
    double sum_us, min_us, max_us;
    uint32_t hist[GF_OP_BUCKETS];
} gf_op;

typedef struct gf_stage {
    const char *name;
    double t0_ms;
    cdb_accel_stats accel;
    uint64_t commit_seq;   /* archive commits */
    uint64_t file_pages;   /* the archive's append point */
} gf_stage;

/* The op called name, created on first use; NULL when the registry is full. */
gf_op *gf_op_get(const char *name);
/* Adds one sample. */
void gf_op_add(gf_op *op, double us);
/* The q quantile (0..1) of the samples, from the histogram. */
double gf_op_quantile(const gf_op *op, double q);
void gf_op_reset(gf_op *op);

/* A start time, and the time since it, in microseconds. */
double gf_t0(void);
double gf_us_since(double t0);

/* An expression that times call (a cdb_status) into the op called name and
 * gives its status, for use inside conditions. The comma operator takes
 * the start time before the call; the start is one static, so the call
 * must not itself contain a GF_T, and every GF_T runs under the pipeline's
 * engine lock (gf_pipeline.c), never from two threads at once. GF_TIME
 * keeps its start in a local and works anywhere. Recording an op is
 * thread-safe. */
void gf_mark(void);
cdb_status gf_lap(const char *name, cdb_status s);
#define GF_T(name, call) (gf_mark(), gf_lap((name), (call)))

/* Times stmt into the op called name. */
#define GF_TIME(name, stmt)                                    \
    do {                                                       \
        double gf_time_t0_ = gf_t0();                          \
        stmt;                                                  \
        gf_op_add(gf_op_get(name), gf_us_since(gf_time_t0_));  \
    } while (0)

/* Starts a stage: every op is emptied, so the stage reports only its own
 * samples. */
void gf_stage_begin(gf_db *g, gf_stage *st, const char *name);
/* Prints the ops with samples and the change in the engine's counters,
 * and stores each op's summary as metric points. */
void gf_stage_end(gf_db *g, gf_stage *st, FILE *out);

/* Writes every op with samples to f, and adds the ops in f into this
 * process's ops of the same names: how worker processes hand their
 * measurements to the pipeline, which reports them as one. */
int gf_bench_dump(FILE *f);
int gf_bench_merge(FILE *f);

/* The conditions a measurement was taken under: build type, sanitizers,
 * accelerator. */
void gf_bench_conditions(const gf_db *g, FILE *out);

#endif
