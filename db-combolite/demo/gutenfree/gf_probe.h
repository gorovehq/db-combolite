/* GutenFree Diet: a live load for the diagnostics screen and the `probe`
 * command. Each tick does primary-key lookups, row reads, passage reads, an
 * index scan and two full-text searches, times each into its measure
 * (gf_metrics.h) and flushes the points. */
#ifndef GF_PROBE_H
#define GF_PROBE_H

#include "gf_ui.h"

#define GF_PROBE_BOOKS 256

typedef struct gf_probe {
    uint64_t first[GF_PROBE_BOOKS], count[GF_PROBE_BOOKS];
    unsigned nbooks;
    cdb_table_cursor scan;
    uint32_t rng;
} gf_probe;

/* Finds the books with passages to read from. */
void gf_probe_init(gf_app *a, gf_probe *p);
void gf_probe_tick(gf_app *a, gf_probe *p);

#endif
