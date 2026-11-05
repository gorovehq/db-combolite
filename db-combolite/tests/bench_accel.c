/* Exact search on an accelerator against the CPU path (spec 18.4): for
 * growing collections, the time to fill the mirror, and the time of an
 * exact search on each path, with how the device's searches ended
 * (certified, widened, or sent to the CPU, spec 18.10). Every device
 * answer is compared with the CPU's. Runs on real files; not part of ctest.
 *
 *   bench_accel [backend] [max_n] [dims] [queries]
 *   bench_accel [backend] --file <base> <collection> [queries] [k]
 *
 * backend: --cuda, --vulkan or --vulkan=<device>; by default whatever
 * cdb_accel_open finds. --software (before it) also allows a device that
 * runs on the CPU, such as llvmpipe.
 *
 * Data as bench_graph: clusters of about 50 vectors (f32, cosine), queries
 * near random cluster centres. No neighbour graph, so every search is
 * exact. With --file, an existing database instead (opened read and
 * written, so give it a copy): the named collection's own vectors, sampled
 * at random, are the queries, and each search is exact. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "combolite/cdb_accel.h"
#ifdef CDB_ACCEL_CUDA
#include "combolite/cdb_accel_cuda.h"
#endif
#ifdef CDB_ACCEL_VULKAN
#include "combolite/cdb_accel_vulkan.h"
#endif
#include "combolite/cdb_env.h"
#include "combolite/cdb_value.h"
#include "bench_file.h"
#include "cdb_catalog.h"
#include "cdb_doc.h"
#include "cdb_fts.h"
#include "cdb_table.h"
#include "cdb_ts.h"
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
/* The accelerator under test, one of these. */
static cdb_accel *any;
#ifdef CDB_ACCEL_CUDA
static cdb_accel_cuda *cuda;
#endif
#ifdef CDB_ACCEL_VULKAN
static cdb_accel_vulkan *vulkan;
#endif
static char backend_name[300];

static float centres[4096][MAX_DIMS];

/* The synthetic run's queries and both paths' answers. */
#define MAX_QUERIES 1000u
static uint8_t qenc[MAX_QUERIES][MAX_DIMS * 4u + 16u];
static size_t qlen[MAX_QUERIES], cpu_n[MAX_QUERIES], dev_n[MAX_QUERIES];
static cdb_vec_hit cpu_hits[MAX_QUERIES][TOPK], dev_hits[MAX_QUERIES][TOPK];
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
    cdb_vec_hit dev[TOPK];
    uint8_t enc[MAX_DIMS * 4u + 16u];
    uint64_t coll, e, fills;
    size_t len, nd;
    double t0, t_insert, t_fill, t_cpu, t_dev;
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

    /* Every CPU search first, then every device search, so neither path
     * runs while the other has just left the device or caches idle. */
    for (t = 0; t < queries; t++)
        qlen[t] = near(dims, (unsigned)(rng >> 8) % clusters, qenc[t], sizeof qenc[t]);
    db.env.accel = NULL;
    t0 = now();
    for (t = 0; t < queries; t++)
        check(cdb_vec_search(&db, coll, qenc[t], qlen[t], 0, cpu_hits[t], TOPK, &cpu_n[t]), "cpu search");
    t_cpu = now() - t0;
    db.env.accel = env.accel;
    st = db.accel_stats;
    t0 = now();
    for (t = 0; t < queries; t++)
        check(cdb_vec_search(&db, coll, qenc[t], qlen[t], 0, dev_hits[t], TOPK, &dev_n[t]), "device search");
    t_dev = now() - t0;
    for (t = 0; t < queries; t++) {
        size_t a;
        if (cpu_n[t] != dev_n[t]) {
            fprintf(stderr, "query %u: %zu hits on the CPU, %zu on the device\n", t, cpu_n[t], dev_n[t]);
            exit(1);
        }
        for (a = 0; a < cpu_n[t]; a++)
            if (cpu_hits[t][a].entry != dev_hits[t][a].entry || cpu_hits[t][a].distance != dev_hits[t][a].distance) {
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

/* ------------------------------------------------------------ --file */

static int get_vec(uint64_t coll, uint64_t id, uint8_t *out, size_t cap, size_t *len)
{
    return bench_get_vec(&db, coll, id, out, cap, len);
}

static void run_file(const char *base, const char *name, unsigned queries, unsigned k)
{
    static cdb_vec_hit cpu[256], dev[256];
    static uint8_t enc[MAX_DIMS * 4u + 16u];
    uint64_t coll, lo, fills, id;
    size_t len, nc, nd;
    unsigned t, done = 0, misses = 0;
    double t0, t_fill, t_cpu = 0, t_dev = 0, gap_sum = 0;
    cdb_accel_stats st;
    bench_open_file(&db, &cfg, &env, base, NULL);
    coll = bench_find(&db, name);
    lo = bench_last_id(&db, coll, enc, sizeof enc);
    printf("%s in %s: entry IDs up to %llu; %u queries, k=%u\n", name, base, (unsigned long long)lo, queries, k);

    fills = db.accel_stats.fills;
    check(get_vec(coll, 1, enc, sizeof enc, &len) ? CDB_OK : CDB_E_NOT_FOUND, "first vector");
    check(cdb_vec_search_mode(&db, coll, enc, len, 0, CDB_VEC_EXACT, dev, k, &nd), "search");
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
    for (t = 0; done < queries && t < queries * 20u; t++) {
        size_t a;
        uint64_t r;
        rng = rng * 1103515245u + 12345u;
        r = (uint64_t)rng << 16;
        rng = rng * 1103515245u + 12345u;
        id = 1u + (r ^ (rng >> 8)) % lo;
        if (!get_vec(coll, id, enc, sizeof enc, &len)) {
            misses++;
            continue;
        }
        db.env.accel = NULL;
        t0 = now();
        check(cdb_vec_search_mode(&db, coll, enc, len, 0, CDB_VEC_EXACT, cpu, k, &nc), "cpu search");
        t_cpu += now() - t0;
        db.env.accel = env.accel;
        t0 = now();
        check(cdb_vec_search_mode(&db, coll, enc, len, 0, CDB_VEC_EXACT, dev, k, &nd), "device search");
        t_dev += now() - t0;
        if (nc != nd) {
            fprintf(stderr, "entry %llu: %zu hits on the CPU, %zu on the device\n", (unsigned long long)id, nc, nd);
            exit(1);
        }
        for (a = 0; a < nc; a++)
            if (cpu[a].entry != dev[a].entry || cpu[a].distance != dev[a].distance) {
                fprintf(stderr, "entry %llu: hit %zu differs\n", (unsigned long long)id, a);
                exit(1);
            }
        if (nc == k)
            gap_sum += cpu[k - 1].distance - cpu[k / 2u].distance;
        done++;
    }
    printf("fill %.2f s; cpu %.3f ms, device %.3f ms per search (%.1fx); %u searches: %llu certified, %llu widened, "
           "%llu on the CPU; mean gap between hit %u and hit %u: %.4g\n",
           t_fill, t_cpu / done * 1e3, t_dev / done * 1e3, t_dev > 0 ? t_cpu / t_dev : 0.0, done,
           (unsigned long long)(db.accel_stats.certified - st.certified),
           (unsigned long long)(db.accel_stats.widened - st.widened),
           (unsigned long long)(db.accel_stats.cpu - st.cpu), k / 2u + 1u, k, done ? gap_sum / done : 0.0);
    if (misses)
        printf("(%u sampled IDs were not live)\n", misses);
    check(cdb_db_close(&db), "close");
}

/* Opens the backend `flag` names (NULL: cdb_accel_open's choice) into env,
 * with open flags `flags`. */
static int open_backend(const char *flag, unsigned flags)
{
    cdb_hostenv_init(&he, &env);
    if (flag == NULL) {
        if (cdb_accel_open(flags, &any) != CDB_OK)
            return 0;
        cdb_accel_attach(any, &env);
        snprintf(backend_name, sizeof backend_name, "%s", cdb_accel_name(any));
        return 1;
    }
#ifdef CDB_ACCEL_CUDA
    (void)flags; /* CUDA devices are never software */
    if (strcmp(flag, "--cuda") == 0) {
        if (cdb_accel_cuda_open(-1, &cuda) != CDB_OK)
            return 0;
        cdb_accel_cuda_attach(cuda, &env);
        snprintf(backend_name, sizeof backend_name, "CUDA: %s", cdb_accel_cuda_name(cuda));
        return 1;
    }
#endif
#ifdef CDB_ACCEL_VULKAN
    if (strncmp(flag, "--vulkan", 8) == 0) {
        int dev = flag[8] == '=' ? atoi(flag + 9) : -1;
        if (cdb_accel_vulkan_open(dev, flags, &vulkan) != CDB_OK)
            return 0;
        cdb_accel_vulkan_attach(vulkan, &env);
        snprintf(backend_name, sizeof backend_name, "Vulkan: %s", cdb_accel_vulkan_name(vulkan));
        return 1;
    }
#endif
    return 0;
}

static void close_backend(void)
{
    if (any != NULL)
        cdb_accel_close(any);
#ifdef CDB_ACCEL_CUDA
    if (cuda != NULL)
        cdb_accel_cuda_close(cuda);
#endif
#ifdef CDB_ACCEL_VULKAN
    if (vulkan != NULL)
        cdb_accel_vulkan_close(vulkan);
#endif
}

int main(int argc, char **argv)
{
    static const unsigned sizes[] = { 2000, 5000, 10000, 30000, 100000, 300000, 1000000 };
    const char *flag = NULL;
    unsigned max_n, dims, queries, flags = 0;
    size_t i;
    if (argc > 1 && strcmp(argv[1], "--software") == 0) {
        flags |= CDB_ACCEL_SOFTWARE;
        argv++;
        argc--;
    }
    if (argc > 1 && strncmp(argv[1], "--", 2) == 0 && strcmp(argv[1], "--file") != 0) {
        flag = argv[1];
        argv++;
        argc--;
    }
    if (!open_backend(flag, flags)) {
        fprintf(stderr, "no usable accelerator%s%s in this build and machine\n", flag ? " for " : "",
                flag ? flag : "");
        return 1;
    }
    if (argc >= 4 && strcmp(argv[1], "--file") == 0) {
        unsigned k = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 10) : TOPK;
        queries = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 10) : 200u;
        if (queries == 0 || k == 0 || k > 256u) {
            fprintf(stderr, "usage: bench_accel [backend] --file <base> <collection> [queries] [k 1..256]\n");
            return 2;
        }
        printf("%s\n", backend_name);
        run_file(argv[2], argv[3], queries, k);
        close_backend();
        return 0;
    }
    max_n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 100000u;
    dims = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 384u;
    queries = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 50u;
    if (dims == 0 || dims > MAX_DIMS || queries == 0 || queries > MAX_QUERIES) {
        fprintf(stderr, "usage: bench_accel [backend] [max_n] [dims 1..%u] [queries 1..%u]\n", MAX_DIMS,
                MAX_QUERIES);
        return 2;
    }
    printf("%s; dims %u, cosine, f32, k=%u, %u queries, page %u\n", backend_name, dims, TOPK, queries, PAGE);
    printf("%8s %10s %8s %10s %10s %8s %6s %6s %6s\n", "n", "insert s", "fill s", "cpu ms", "device ms", "speedup",
           "cert", "widen", "cpu");
    for (i = 0; i < sizeof sizes / sizeof sizes[0] && sizes[i] <= max_n; i++)
        run(sizes[i], dims, queries);
    close_backend();
    return 0;
}
