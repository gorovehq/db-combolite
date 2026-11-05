#define _POSIX_C_SOURCE 200809L
#include "gf_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cdb_doc.h"
#include "cdb_table.h"
#include "cdb_ts.h"
#include "cdb_vec.h"
#ifdef GF_ACCEL
#include "combolite/cdb_accel.h"
#endif

/* Installs the accelerator in the environment, before the database opens
 * with it (gf_db.h: GUTENFREE_ACCEL). */
static void accel_open(gf_db *g)
{
#ifdef GF_ACCEL
    const char *want = getenv("GUTENFREE_ACCEL");
    unsigned flags = 0;
    cdb_accel *ax;
    if (want != NULL && strcmp(want, "off") == 0) {
        snprintf(g->accel_name, sizeof g->accel_name, "off (GUTENFREE_ACCEL)");
        return;
    }
    if (want != NULL && strcmp(want, "software") == 0)
        flags |= CDB_ACCEL_SOFTWARE;
    if (cdb_accel_open(flags, &ax) != CDB_OK) {
        snprintf(g->accel_name, sizeof g->accel_name, "none found");
        return;
    }
    cdb_accel_attach(ax, &g->env);
    g->accel = ax;
    snprintf(g->accel_name, sizeof g->accel_name, "%s", cdb_accel_name(ax));
#else
    snprintf(g->accel_name, sizeof g->accel_name, "not built (CDB_ACCEL_CUDA, CDB_ACCEL_VULKAN)");
#endif
}

static void accel_close(gf_db *g)
{
#ifdef GF_ACCEL
    if (g->accel != NULL)
        cdb_accel_close((cdb_accel *)g->accel);
#endif
    g->accel = NULL;
}

static void *take(gf_db *g, size_t size)
{
    void *p;
    p = g->nbufs < GF_MAX_BUFS ? calloc(1, size) : NULL;
    if (p == NULL) {
        g->failed = 1;
        return NULL;
    }
    g->bufs[g->nbufs++] = p;
    g->bytes += size;
    return p;
}

static void release(gf_db *g)
{
    while (g->nbufs > 0)
        free(g->bufs[--g->nbufs]);
    g->bytes = 0;
}

/* A database ID for a new file: the clock and process ID, mixed. */
static void new_id(uint8_t id[16])
{
    struct timespec ts;
    uint64_t x;
    unsigned i;
    clock_gettime(CLOCK_REALTIME, &ts);
    x = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    x ^= (uint64_t)getpid() << 32;
    for (i = 0; i < 16; i++) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdu;
        x ^= x >> 29;
        id[i] = (uint8_t)x;
    }
}

cdb_status gf_db_open(gf_db *g, const char *base)
{
    cdb_db_config cfg;
    cdb_header h;
    cdb_status s;
    size_t fts_tokens = cdb_fts_tokens_needed(GF_PAGE, GF_DOC_MAX);
    size_t entries = cdb_index_entries_needed(GF_PAGE);

    memset(g, 0, sizeof *g);
    cdb_hostenv_init(&g->he, &g->env);
    accel_open(g);
    memset(&cfg, 0, sizeof cfg);

    cfg.pager.node_cap = GF_NODES;
    cfg.pager.arena_size = GF_NODES * 2u * GF_PAGE + 2u * GF_PAGE;
    cfg.pager.arena = take(g, cfg.pager.arena_size);
    cfg.pager.nodes = take(g, GF_NODES * sizeof(cdb_pager_node));
    cfg.pager.free_cap = 16384u;
    cfg.pager.free_list = take(g, cfg.pager.free_cap * sizeof(cdb_free_extent));
    cfg.pager.page_cache_size = GF_PAGE_CACHE;
    cfg.pager.page_cache = take(g, cfg.pager.page_cache_size);

    /* A change's journal record holds the whole body (cdb_doc.h). */
    cfg.record_buf_size = GF_DOC_MAX + 2u * GF_PAGE + 64u;
    cfg.record_buf = take(g, cfg.record_buf_size);

    cfg.catalog_work.bytes_size = cdb_index_work_size(GF_PAGE);
    cfg.catalog_work.bytes = take(g, cfg.catalog_work.bytes_size);
    cfg.catalog_work.entries_cap = entries;
    cfg.catalog_work.entries = take(g, entries * sizeof(cdb_index_entry));
    cfg.catalog_page = take(g, GF_PAGE);

    cfg.doc_max = GF_DOC_MAX;
    cfg.doc_buffers = take(g, cdb_doc_buffers_size(GF_PAGE, GF_DOC_MAX));
    cfg.doc_stage_size = GF_DOC_STAGE;
    cfg.doc_stage = take(g, GF_DOC_STAGE);
    cfg.fts_buffers = take(g, cdb_fts_buffers_size(GF_PAGE, GF_DOC_MAX));
    cfg.fts_tokens_cap = fts_tokens;
    cfg.fts_tokens = take(g, fts_tokens * sizeof(cdb_fts_token));
    cfg.table_buffers = take(g, cdb_table_buffers_size(GF_PAGE, GF_DOC_MAX));

    cfg.vec_buffers = take(g, 3u * GF_PAGE);
    cfg.vec_work_size = cdb_vec_work_size(CDB_VEC_F32, GF_VEC_DIMS, GF_VEC_K); /* refreshes grow (vec_mem) */
    cfg.vec_work = take(g, cfg.vec_work_size);

    cfg.ts_buffers = take(g, 3u * GF_PAGE);
    cfg.ts_work_size = cdb_ts_work_size();
    cfg.ts_work = take(g, cfg.ts_work_size);

    cfg.durability = CDB_DURABILITY_NORMAL;
    cfg.max_open = 0; /* workstation: no cap */

    if (g->failed) { /* out of memory: the engine has no code of its own for it */
        release(g);
        accel_close(g);
        return CDB_E_BUFFER_TOO_SMALL;
    }

    memset(&h, 0, sizeof h);
    h.order = GF_ORDER;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)GF_PAGE_LOG2;
    new_id(h.db_id);

    s = cdb_db_open(&g->db, &g->env, base, &cfg, &h);
    if (s != CDB_OK) {
        release(g);
        accel_close(g);
        return s;
    }
    g->open = 1;
    return CDB_OK;
}

double gf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}

cdb_status gf_db_close(gf_db *g)
{
    cdb_status s = CDB_OK;
    if (g->open)
        s = cdb_db_close(&g->db);
    g->open = 0;
    release(g);
    accel_close(g); /* after the database, which used it */
    return s;
}
