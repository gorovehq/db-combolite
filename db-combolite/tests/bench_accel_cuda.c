/* Exact search on the CUDA accelerator against the CPU path (spec 18.4):
 * for growing collections, the time to fill the mirror, and the time of an
 * exact search on each path, with how the device's searches ended
 * (certified, widened, or sent to the CPU, spec 18.10). Every device
 * answer is compared with the CPU's. Runs on real files; not part of ctest.
 *
 *   bench_accel_cuda [max_n] [dims] [queries]
 *
 * Data as bench_graph: clusters of about 50 vectors (f32, cosine), queries
 * near random cluster centres. No neighbour graph, so every search is
 * exact. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "combolite/cdb_accel_cuda.h"
#include "combolite/cdb_env.h"
#include "combolite/cdb_value.h"
#include "cdb_doc.h"
#include "cdb_vec.h"

#define PAGE_LOG2 12u
#define PAGE (1u << PAGE_LOG2)
#define NODES 256u
#define MAX_DIMS 1024u
#define TOPK 10u
#define PER_TX 200u

static uint8_t arena[NODES * 2u * PAGE + 2u * PAGE];
static cdb_pager_node nodes[NODES];
static cdb_free_extent free_list[16384];
static uint8_t record_buf[2u * PAGE];
static uint8_t cat_work[96u * 1024u];
static cdb_index_entry cat_entries[2048];
static uint8_t cat_page[PAGE];
static uint8_t doc_buffers[8u * PAGE];
static uint8_t vec_buffers[3u * PAGE];
static cdb_db_config cfg;
static cdb_db db;
static cdb_hostenv he;
static cdb_env env;
static cdb_accel_cuda *cuda;

static float centres[4096][MAX_DIMS];
static uint32_t rng = 12345u;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double uniform(void)
{
    rng = rng * 1103515245u + 12345u;
    return (double)(rng >> 8) / 16777216.0 * 2.0 - 1.0;
}

static void check(cdb_status s, const char *what)
{
    if (s != CDB_OK) {
        fprintf(stderr, "%s: status %d\n", what, (int)s);
        exit(1);
    }
}

static size_t near(unsigned dims, unsigned c, uint8_t *out, size_t cap)
{
    float f[MAX_DIMS];
    cdb_value_writer w;
    unsigned i;
    for (i = 0; i < dims; i++)
        f[i] = centres[c][i] + (float)(0.15 * (uniform() + uniform() + uniform()) / 3.0);
    cdb_value_writer_init(&w, out, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    cdb_value_write_vector(&w, CDB_VEC_F32, dims, f);
    check(w.error, "encode");
    return cdb_value_writer_len(&w);
}

static void open_db(const char *base)
{
    cdb_header h;
    memset(&cfg, 0, sizeof cfg);
    cfg.pager.arena = arena;
    cfg.pager.arena_size = sizeof arena;
    cfg.pager.nodes = nodes;
    cfg.pager.node_cap = NODES;
    cfg.pager.free_list = free_list;
    cfg.pager.free_cap = sizeof free_list / sizeof free_list[0];
    cfg.record_buf = record_buf;
    cfg.record_buf_size = sizeof record_buf;
    cfg.catalog_work.bytes = cat_work;
    cfg.catalog_work.bytes_size = sizeof cat_work;
    cfg.catalog_work.entries = cat_entries;
    cfg.catalog_work.entries_cap = 2048;
    cfg.catalog_page = cat_page;
    cfg.doc_buffers = doc_buffers;
    cfg.vec_buffers = vec_buffers;
    cfg.durability = CDB_DURABILITY_NORMAL;
    memset(&h, 0, sizeof h);
    h.order = CDB_ORDER_LITTLE;
    h.layout_flags = CDB_LAYOUT_ADDR64;
    h.default_page_log2 = (uint8_t)PAGE_LOG2;
    memset(h.db_id, 0x42, sizeof h.db_id);
    check(cdb_db_open(&db, &env, base, &cfg, &h), "open");
}

static void run(unsigned n, unsigned dims, unsigned queries)
{
    char base[256], path[300];
    unsigned clusters = n / 50u ? n / 50u : 1u, i, j, t;
    cdb_vec_config vc;
    cdb_vec_hit cpu[TOPK], dev[TOPK];
    uint8_t enc[MAX_DIMS * 4u + 16u];
    uint64_t coll, e, fills;
    size_t len, nc, nd;
    double t0, t_insert, t_fill, t_cpu = 0, t_dev = 0;
    cdb_accel_stats st;

    if (clusters > 4096u)
        clusters = 4096u;
    snprintf(base, sizeof base, "bench_accel_%ld", (long)getpid());
    snprintf(path, sizeof path, "%s.cdbarch", base);
    unlink(path);
    for (i = 0; i < clusters; i++)
        for (j = 0; j < dims; j++)
            centres[i][j] = (float)uniform();
    open_db(base);
    memset(&vc, 0, sizeof vc);
    vc.elem = CDB_VEC_F32;
    vc.dims = dims;
    vc.measure = CDB_VEC_COSINE;
    vc.model = "bench";
    vc.model_len = 5;
    vc.model_version = "1";
    vc.model_version_len = 1;
    check(cdb_vec_create(&db, "v", 1, &vc, &coll), "create");

    t0 = now();
    for (i = 0; i < n; i += PER_TX) {
        check(cdb_tx_begin(&db, CDB_DURABILITY_DEFAULT), "begin");
        for (j = i; j < n && j < i + PER_TX; j++) {
            len = near(dims, j % clusters, enc, sizeof enc);
            check(cdb_vec_insert(&db, coll, enc, len, 0, 0, &e), "insert");
        }
        check(cdb_tx_commit(&db), "commit");
    }
    t_insert = now() - t0;

    /* The first search asks for the mirror; ticks fill it (spec 18.6). */
    fills = db.accel_stats.fills;
    len = near(dims, 0, enc, sizeof enc);
    check(cdb_vec_search(&db, coll, enc, len, 0, dev, TOPK, &nd), "search");
    t0 = now();
    while (db.accel_stats.fills == fills) {
        check(cdb_db_tick(&db), "tick");
        if (now() - t0 > 600.0) {
            fprintf(stderr, "the mirror was not filled\n");
            exit(1);
        }
    }
    t_fill = now() - t0;

    st = db.accel_stats;
    for (t = 0; t < queries; t++) {
        size_t a;
        len = near(dims, (unsigned)(rng >> 8) % clusters, enc, sizeof enc);
        db.env.accel = NULL;
        t0 = now();
        check(cdb_vec_search(&db, coll, enc, len, 0, cpu, TOPK, &nc), "cpu search");
        t_cpu += now() - t0;
        db.env.accel = env.accel;
        t0 = now();
        check(cdb_vec_search(&db, coll, enc, len, 0, dev, TOPK, &nd), "device search");
        t_dev += now() - t0;
        if (nc != nd) {
            fprintf(stderr, "query %u: %zu hits on the CPU, %zu on the device\n", t, nc, nd);
            exit(1);
        }
        for (a = 0; a < nc; a++)
            if (cpu[a].entry != dev[a].entry || cpu[a].distance != dev[a].distance) {
                fprintf(stderr, "query %u: hit %zu differs\n", t, a);
                exit(1);
            }
    }
    printf("%8u %10.2f %8.2f %10.3f %10.3f %7.1fx %6llu %6llu %6llu\n", n, t_insert, t_fill, t_cpu / queries * 1e3,
           t_dev / queries * 1e3, t_dev > 0 ? t_cpu / t_dev : 0.0,
           (unsigned long long)(db.accel_stats.certified - st.certified),
           (unsigned long long)(db.accel_stats.widened - st.widened),
           (unsigned long long)(db.accel_stats.cpu - st.cpu));
    fflush(stdout);
    check(cdb_db_close(&db), "close");
    unlink(path);
}

int main(int argc, char **argv)
{
    static const unsigned sizes[] = { 2000, 5000, 10000, 30000, 100000, 300000, 1000000 };
    unsigned max_n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 100000u;
    unsigned dims = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 384u;
    unsigned queries = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 50u;
    size_t i;
    if (dims == 0 || dims > MAX_DIMS || queries == 0) {
        fprintf(stderr, "usage: bench_accel_cuda [max_n] [dims 1..%u] [queries]\n", MAX_DIMS);
        return 2;
    }
    if (cdb_accel_cuda_open(-1, &cuda) != CDB_OK) {
        fprintf(stderr, "no usable CUDA device\n");
        return 1;
    }
    cdb_hostenv_init(&he, &env);
    cdb_accel_cuda_attach(cuda, &env);
    printf("%s; dims %u, cosine, f32, k=%u, %u queries, page %u\n", cdb_accel_cuda_name(cuda), dims, TOPK, queries,
           PAGE);
    printf("%8s %10s %8s %10s %10s %8s %6s %6s %6s\n", "n", "insert s", "fill s", "cpu ms", "device ms", "speedup",
           "cert", "widen", "cpu");
    for (i = 0; i < sizeof sizes / sizeof sizes[0] && sizes[i] <= max_n; i++)
        run(sizes[i], dims, queries);
    cdb_accel_cuda_close(cuda);
    return 0;
}
