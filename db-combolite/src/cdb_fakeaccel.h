/* A fake accelerator for tests (spec 18.11): the accelerator interface of
 * cdb_env.h on the CPU, with distances deliberately off by up to the bound
 * it reports, so near-ties and widening happen on purpose, and every call
 * scriptable to decline or fail, so each fallback is exercised. Built only
 * into the combolite_testing library, never the production one. Its
 * memory comes from malloc: it stands for a device, not for the core. */
#ifndef CDB_FAKEACCEL_H
#define CDB_FAKEACCEL_H

#include "combolite/cdb_env.h"

typedef struct cdb_fakeaccel {
    /* Behaviour, set by the test. */
    double noise;            /* each distance is off by up to this; 0: the CPU's own */
    unsigned decline_elems;  /* bit per cdb_vector_elem: mirror_open declines it */
    uint64_t fail_put_at;    /* the n-th mirror_put fails (0: none) */
    uint64_t fail_del_at;    /* the n-th mirror_del fails */
    uint64_t fail_nearest_at;/* the n-th nearest fails */
    /* What happened. */
    uint64_t opens, closes, puts, dels, nearests;
    unsigned live;           /* mirrors open now */
    struct cdb_accel_mirror *head; /* the open mirrors */
} cdb_fakeaccel;

/* Resets fa, freeing any mirror still open (as a device reset would), and
 * installs it as env's accelerator. fa must start zeroed (static). */
void cdb_fakeaccel_init(cdb_fakeaccel *fa, cdb_env *env);

#endif
