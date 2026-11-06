/* GutenFree Diet: self-telemetry as time series (spec 10). Every measure
 * the demo takes is a series in the "metrics" store, one f64 field per
 * point, timestamped in microseconds of wall-clock time. Points queue in
 * memory and are written by gf_metrics_flush in one transaction of their
 * own, never inside a data transaction, so a rolled-back ingest keeps its
 * timings. The demo has one database, so this module keeps one queue. */
#ifndef GF_METRICS_H
#define GF_METRICS_H

#include "gf_db.h"

#define GF_METRICS_QUEUE 4096u

/* Which way is better for a measure, and where its bands start. */
typedef struct gf_measure {
    const char *name;        /* the series name */
    const char *label;
    const char *unit;
    int higher_is_better;
    double exceptional;      /* at or beyond this: exceptional */
    double satisfactory;     /* at or beyond this: satisfactory; else poor */
    const char *explain;
} gf_measure;

extern const gf_measure gf_measures[];
extern const unsigned gf_nmeasures;

/* 0 poor, 1 satisfactory, 2 exceptional. */
int gf_measure_band(const gf_measure *m, double v);

/* Opens the "metrics" store, creating it the first time. */
cdb_status gf_metrics_open(gf_db *g);

/* Queues a point now; dropped (and counted) when the queue is full. */
void gf_metric(const char *name, double value);

/* Writes the queued points in one transaction; times itself into
 * ts.flush_ms. Must not be called inside a transaction. */
cdb_status gf_metrics_flush(gf_db *g);

/* The most recent points of a series, oldest first: at most max values in
 * out, *n of them; also the number of points read and the read's time. */
cdb_status gf_metrics_recent(gf_db *g, const char *name, double *out, size_t max, size_t *n, uint64_t *points,
                             double *read_ms);

/* Every point of a series: how many, the smallest, largest, sum and last. */
typedef struct gf_metric_summary {
    uint64_t count;
    double min, max, sum, last;
} gf_metric_summary;
cdb_status gf_metrics_summary(gf_db *g, const char *name, gf_metric_summary *out);

/* The last flush failure ("" when the last flush worked). */
const char *gf_metrics_error(void);

/* Points written and dropped since open. */
uint64_t gf_metrics_written(void);
uint64_t gf_metrics_dropped(void);

#endif
