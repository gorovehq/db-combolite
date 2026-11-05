/* For benchmarks that run on an existing database (bench_accel and
 * bench_graph --file): opening it with buffers sized as GutenFree Diet
 * sizes its own, and reading a collection's vectors. The database is
 * opened for writing, so give it a copy. Memory comes from calloc: this
 * is a benchmark, not the core. */
#ifndef BENCH_FILE_H
#define BENCH_FILE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "combolite/cdb_env.h"
#include "cdb_catalog.h"
#include "cdb_doc.h"
#include "cdb_fts.h"
#include "cdb_table.h"
#include "cdb_ts.h"
#include "cdb_vec.h"

static void bench_check(cdb_status s, const char *what)
{
    if (s != CDB_OK) {
        fprintf(stderr, "%s: status %d\n", what, (int)s);
        exit(1);
    }
}

static void *bench_take(size_t n)
{
    void *p = calloc(1, n ? n : 1u);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

/* Opens database `base` with 4 KB pages and 8 MB documents, its graph
 * refreshes sized for 384 dims and k = 16, and reconciliation leaving the
 * graph alone; cfg is filled in, and may be adjusted by the caller first
 * through `tune` (NULL: none). */
static void bench_open_file(cdb_db *db, cdb_db_config *cfg, cdb_env *env, const char *base,
                            void (*tune)(cdb_db_config *))
{
    const size_t page = 4096u, doc_max = 8u * 1024u * 1024u, nodes_n = 1024u;
    size_t tokens = cdb_fts_tokens_needed(page, doc_max), entries = cdb_index_entries_needed(page);
    cdb_header h;
    memset(cfg, 0, sizeof *cfg);
    cfg->pager.node_cap = nodes_n;
    cfg->pager.arena_size = nodes_n * 2u * page + 2u * page;
    cfg->pager.arena = bench_take(cfg->pager.arena_size);
    cfg->pager.nodes = bench_take(nodes_n * sizeof(cdb_pager_node));
    cfg->pager.free_cap = 16384u;
    cfg->pager.free_list = bench_take(cfg->pager.free_cap * sizeof(cdb_free_extent));
    cfg->record_buf_size = doc_max + 2u * page + 64u;
    cfg->record_buf = bench_take(cfg->record_buf_size);
    cfg->catalog_work.bytes_size = cdb_index_work_size(page);
    cfg->catalog_work.bytes = bench_take(cfg->catalog_work.bytes_size);
    cfg->catalog_work.entries_cap = entries;
    cfg->catalog_work.entries = bench_take(entries * sizeof(cdb_index_entry));
    cfg->catalog_page = bench_take(page);
    cfg->doc_max = doc_max;
    cfg->doc_buffers = bench_take(cdb_doc_buffers_size(page, doc_max));
    cfg->doc_stage_size = 1024u * 1024u;
    cfg->doc_stage = bench_take(cfg->doc_stage_size);
    cfg->fts_buffers = bench_take(cdb_fts_buffers_size(page, doc_max));
    cfg->fts_tokens_cap = tokens;
    cfg->fts_tokens = bench_take(tokens * sizeof(cdb_fts_token));
    cfg->table_buffers = bench_take(cdb_table_buffers_size(page, doc_max));
    cfg->vec_buffers = bench_take(3u * page);
    cfg->vec_work_size = cdb_vec_work_size(CDB_VEC_F32, 384u, 16u);
    cfg->vec_work = bench_take(cfg->vec_work_size);
    cfg->vec_refresh_budget = 1u; /* reconciliation leaves the graph alone */
    cfg->ts_buffers = bench_take(3u * page);
    cfg->ts_work_size = cdb_ts_work_size();
    cfg->ts_work = bench_take(cfg->ts_work_size);
    cfg->durability = CDB_DURABILITY_NORMAL;
    if (tune != NULL)
        tune(cfg);
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = 12u;
    bench_check(cdb_db_open(db, env, base, cfg, &h), "open");
}

static int bench_get_vec(cdb_db *db, uint64_t coll, uint64_t id, uint8_t *out, size_t cap, size_t *len)
{
    uint64_t ss, se;
    return cdb_vec_get(db, coll, id, out, cap, len, &ss, &se) == CDB_OK;
}

/* The named store, or exit. */
static uint64_t bench_find(cdb_db *db, const char *name)
{
    uint64_t id;
    int found;
    bench_check(cdb_catalog_find_store(&db->catalog, name, strlen(name), &id, &found), "find");
    if (!found) {
        fprintf(stderr, "no collection %s\n", name);
        exit(1);
    }
    return id;
}

/* The highest live entry ID, by doubling and bisecting: entry IDs are
 * dense from 1 in the collections these benchmarks read. */
static uint64_t bench_last_id(cdb_db *db, uint64_t coll, uint8_t *buf, size_t cap)
{
    uint64_t hi = 1, lo;
    size_t len;
    while (bench_get_vec(db, coll, hi * 2u, buf, cap, &len))
        hi *= 2u;
    lo = hi;
    hi *= 2u;
    while (hi - lo > 1u) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (bench_get_vec(db, coll, mid, buf, cap, &len))
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

#endif
