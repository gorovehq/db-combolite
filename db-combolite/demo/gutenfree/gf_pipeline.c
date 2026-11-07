#define _POSIX_C_SOURCE 200809L
#include "gf_pipeline.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gf_bench.h"
#include "gf_books.h"
#include "gf_catalog.h"
#include "gf_metrics.h"
#include "gf_queries.h"

extern char **environ;

#define MAX_WORKERS 32u
#define HOLD_EVERY 10u     /* one book in this many is held out */
#define MAX_HELD 64u
#define HELD_QUERIES 200u
#define RETRIES 3u
#define FETCH_JOBS 8u

/* One book for a worker: a fixed-size record, so a read by any one worker
 * takes whole records (writes below PIPE_BUF are atomic). */
typedef struct book_rec {
    int64_t ebook;
    int64_t spare;
} book_rec;

/* ------------------------------------------------------------ search checks */

cdb_status gf_search_checks(gf_db *g, const gf_vec *v, gf_embed *e, unsigned stored, const float (*held)[GF_EMBED_DIMS],
                            unsigned nheld)
{
    static float q[256][GF_EMBED_DIMS];
    static uint32_t rng = 12345u;
    static char text[600];
    gf_vec_check ck;
    unsigned n = 0, i;
    cdb_status s;

    if (stored > 256)
        stored = 256;
    if ((s = gf_vec_stored_queries(g, v, stored, &rng, q, &n)) != CDB_OK)
        return s;
    s = gf_vec_check_search(g, v, (const float (*)[GF_EMBED_DIMS])q, n, "stored", &ck);
    printf("search check, %u stored passages as queries: graph finds %.2f of the exact top 10 (worst %.0f), "
           "1-bit %.2f (worst %.0f)\n", ck.queries, ck.recall, ck.recall_min, ck.bits_agree, ck.bits_min);
    if (s != CDB_OK)
        return s;
    if (e != NULL) {
        for (i = 0; i < gf_nqueries && i < 256; i++) {
            int rc;
            snprintf(text, sizeof text, "%s%s", GF_EMBED_QUERY_PREFIX, gf_queries[i]);
            GF_TIME("embed.query", rc = gf_embed_text(e, text, strlen(text), q[i], NULL));
            if (rc != 0)
                return CDB_E_STATE;
        }
        s = gf_vec_check_search(g, v, (const float (*)[GF_EMBED_DIMS])q, i, "fresh", &ck);
        printf("search check, %u fresh phrases: graph finds %.2f of the exact top 10 (worst %.0f: \"%s\"), "
               "1-bit %.2f (worst %.0f)\n", ck.queries, ck.recall, ck.recall_min, gf_queries[ck.worst_query],
               ck.bits_agree, ck.bits_min);
        if (s != CDB_OK)
            return s;
    }
    if (held != NULL && nheld > 0) {
        s = gf_vec_check_search(g, v, held, nheld, "heldout", &ck);
        printf("search check, %u passages of held-out books: graph finds %.2f of the exact top 10 (worst %.0f), "
               "1-bit %.2f (worst %.0f)\n", ck.queries, ck.recall, ck.recall_min, ck.bits_agree, ck.bits_min);
    }
    return s;
}

/* ------------------------------------------------------------ worker */

static int retryable(cdb_status s)
{
    return s == CDB_E_BUSY || s == CDB_E_COLLISION || s == CDB_E_TIMEOUT;
}

/* Counts one attempt of kind ("store", "commit") ending in s, as an op of
 * its own whose samples are the attempts' times. */
static void count_status(const char *kind, cdb_status s, double us)
{
    char name[40];
    snprintf(name, sizeof name, "%s.%s", kind, cdb_status_str(s));
    gf_op_add(gf_op_get(name), us);
}

/* Waits a little before a retry, letting the engine move the baton on. */
static void back_off(gf_db *g, unsigned attempt)
{
    struct timespec ts = { 0, (long)(1000000L << attempt) };
    (void)cdb_db_tick(&g->db);
    nanosleep(&ts, NULL);
}

static cdb_status store_book(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                             gf_book_stats *st)
{
    cdb_status s = CDB_OK;
    unsigned attempt;
    for (attempt = 0; attempt <= RETRIES; attempt++) {
        double t0 = gf_t0();
        s = gf_books_ingest_file(g, c, b, ebook, dir, st);
        count_status("store", s, gf_us_since(t0));
        if (!retryable(s))
            break;
        back_off(g, attempt);
    }
    return s;
}

/* Reads, embeds and commits one book's vectors; the commit is retried. */
static cdb_status embed_book(gf_db *g, const gf_catalog *c, const gf_books *b, const gf_vec *v, gf_embed *e,
                             uint64_t row, uint64_t *passages)
{
    gf_vec_job j;
    int done = 0;
    unsigned attempt;
    cdb_status s = gf_vec_job_start(g, c, v, row, &j);

    while (s == CDB_OK && !done)
        s = gf_vec_job_step(g, b, e, &j, 1e12, &done);
    for (attempt = 0; s == CDB_OK && attempt <= RETRIES; attempt++) {
        double t0 = gf_t0();
        s = gf_vec_job_commit(g, c, b, v, &j);
        count_status("commit_vectors", s, gf_us_since(t0));
        if (!retryable(s))
            break;
        back_off(g, attempt);
        s = CDB_OK;
    }
    *passages = j.n;
    gf_vec_job_free(&j);
    return s;
}

int gf_pipeline_worker(gf_db *g, unsigned index, const char *ops_path, const char *dir, const char *model)
{
    gf_catalog c;
    gf_books b;
    gf_vec v;
    gf_embed *e;
    book_rec r;
    unsigned books = 0, failed = 0;
    uint64_t passages = 0;
    FILE *f;
    double t0;
    cdb_status s;

    if ((s = gf_catalog_open(g, &c)) != CDB_OK || (s = gf_books_open(g, &b)) != CDB_OK
        || (s = gf_vec_open(g, &v)) != CDB_OK) {
        fprintf(stderr, "[w%u] open stores: %s\n", index, cdb_status_str(s));
        return 1;
    }
    t0 = gf_t0();
    if ((e = gf_embed_open(model)) == NULL) {
        fprintf(stderr, "[w%u] cannot load %s\n", index, model);
        return 1;
    }
    gf_op_add(gf_op_get("embed.model_load"), gf_us_since(t0));
    for (;;) {
        gf_book_stats st;
        uint64_t row = 0, n = 0;
        int found = 0;
        ssize_t got = read(0, &r, sizeof r);
        if (got < 0 && errno == EINTR)
            continue;
        if (got != (ssize_t)sizeof r)
            break;
        t0 = gf_t0();
        s = store_book(g, &c, &b, r.ebook, dir, &st);
        if (s == CDB_OK && (s = gf_catalog_find(g, &c, r.ebook, &row, &found)) == CDB_OK && !found)
            s = CDB_E_NOT_FOUND;
        if (s == CDB_OK)
            s = embed_book(g, &c, &b, &v, e, row, &n);
        if (s != CDB_OK) {
            printf("[w%u] #%lld: %s\n", index, (long long)r.ebook, cdb_status_str(s));
            failed++;
            continue;
        }
        gf_op_add(gf_op_get("pipe.book_end_to_end"), gf_us_since(t0));
        books++;
        passages += n;
        fflush(stdout);
    }
    gf_embed_close(e);
    printf("[w%u] %u books, %llu passages, %u failed; accelerator: %llu searches certified, %llu on the CPU\n", index,
           books, (unsigned long long)passages, failed, (unsigned long long)g->db.accel_stats.certified,
           (unsigned long long)(g->db.accel_stats.cpu + g->db.accel_stats.small));
    if ((f = fopen(ops_path, "wb")) == NULL || gf_bench_dump(f) != 0) {
        fprintf(stderr, "[w%u] cannot write %s\n", index, ops_path);
        if (f != NULL)
            fclose(f);
        return 1;
    }
    fclose(f);
    return failed > 0 ? 1 : 0;
}

/* ------------------------------------------------------------ parent */

static unsigned worker_count(void)
{
    const char *w = getenv("GUTENFREE_WORKERS");
    unsigned n = w != NULL ? (unsigned)strtoul(w, NULL, 10) : 4u;
    return n == 0 ? 1u : n > MAX_WORKERS ? MAX_WORKERS : n;
}

/* Up to HELD_QUERIES passages from the held-out books, cut as stored books
 * are and embedded as stored passages are (no query prefix). */
static unsigned held_queries(gf_embed *e, const char *dir, const int64_t *held, unsigned nheld,
                             float (*out)[GF_EMBED_DIMS])
{
    static char path[1024];
    unsigned got = 0, per = nheld > 0 ? (HELD_QUERIES + nheld - 1u) / nheld : 0, i;
    for (i = 0; i < nheld && got < HELD_QUERIES; i++) {
        FILE *f;
        char *text;
        long size;
        size_t pos = 0, start, n, k = 0, every;
        gf_books_path(dir, held[i], path, sizeof path);
        if ((f = fopen(path, "rb")) == NULL)
            continue;
        if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0
            || (text = malloc((size_t)size + 1)) == NULL) {
            fclose(f);
            continue;
        }
        n = fread(text, 1, (size_t)size, f);
        fclose(f);
        text[n] = '\0';
        /* Spread this book's share over its whole length. */
        every = (size_t)size / (GF_PASSAGE_MAX * (per ? per : 1)) + 1u;
        while (got < HELD_QUERIES && (n = gf_books_next_passage(text, (size_t)size, &pos, &start)) > 0) {
            char save;
            int rc;
            if (k++ % every != 0 || n < 200)
                continue;
            save = text[start + n];
            text[start + n] = '\0';
            GF_TIME("embed.heldout_passage", rc = gf_embed_text(e, text + start, n, out[got], NULL));
            text[start + n] = save;
            if (rc == 0)
                got++;
        }
        free(text);
    }
    return got;
}

int gf_pipeline_cli(gf_db *g, const char *base, unsigned count, const char *dir, const char *model)
{
    static float held_q[HELD_QUERIES][GF_EMBED_DIMS];
    static char self[1024], line[1024], cmd[2048], ops_path[MAX_WORKERS][1100], idx[MAX_WORKERS][12];
    int64_t held[MAX_HELD];
    pid_t pid[MAX_WORKERS];
    unsigned workers = worker_count(), i, nheld = 0, sent = 0, arrived = 0, bad = 0;
    int fds[2];
    ssize_t len;
    FILE *fetch;
    double t0, t_fetch;
    gf_catalog c;
    gf_books b;
    gf_vec v;
    gf_embed *e;
    uint64_t links = 0;
    cdb_status s;

    if ((len = readlink("/proc/self/exe", self, sizeof self - 1)) <= 0) {
        fprintf(stderr, "gutenfree: cannot find own executable\n");
        return 1;
    }
    self[len] = '\0';
    /* The workers open the database themselves; this process steps aside
     * until they are done. */
    if ((s = gf_db_close(g)) != CDB_OK) {
        fprintf(stderr, "gutenfree: close before the workers: %s\n", cdb_status_str(s));
        return 1;
    }
    if (pipe(fds) != 0) {
        perror("gutenfree: pipe");
        return 1;
    }
    printf("pipeline: %u new books, %u writer processes, %u downloads at once, one book in %u held out\n", count,
           workers, FETCH_JOBS, HOLD_EVERY);
    fflush(stdout);
    t0 = gf_t0();
    for (i = 0; i < workers; i++) {
        posix_spawn_file_actions_t fa;
        char *argv[9];
        snprintf(idx[i], sizeof idx[i], "%u", i);
        snprintf(ops_path[i], sizeof ops_path[i], "%s.w%u.ops", base, i);
        argv[0] = self;
        argv[1] = (char *)"pipeline-worker";
        argv[2] = idx[i];
        argv[3] = ops_path[i];
        argv[4] = (char *)dir;
        argv[5] = (char *)model;
        argv[6] = (char *)base;
        argv[7] = NULL;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        posix_spawn_file_actions_addclose(&fa, fds[1]);
        if (posix_spawn(&pid[i], self, &fa, NULL, argv, environ) != 0) {
            perror("gutenfree: spawn worker");
            posix_spawn_file_actions_destroy(&fa);
            return 1;
        }
        posix_spawn_file_actions_destroy(&fa);
    }
    close(fds[0]);

    snprintf(cmd, sizeof cmd, "python3 -I gf_fetch.py --count %u --new --stream --jobs %u --seed %u --out '%s' 2>&1",
             count + count / (HOLD_EVERY - 1u), FETCH_JOBS, (unsigned)time(NULL) ^ (unsigned)getpid(), dir);
    if ((fetch = popen(cmd, "r")) == NULL) {
        perror("gutenfree: start gf_fetch.py");
        close(fds[1]);
        return 1;
    }
    while (fgets(line, sizeof line, fetch) != NULL) {
        long long n;
        if (strncmp(line, "BOOK ", 5) != 0) {
            printf("fetch: %s", line);
            fflush(stdout);
            continue;
        }
        n = strtoll(line + 5, NULL, 10);
        arrived++;
        if (arrived % HOLD_EVERY == 0 && nheld < MAX_HELD) {
            held[nheld++] = n;
            continue;
        }
        {
            book_rec r;
            r.ebook = n;
            r.spare = 0;
            if (write(fds[1], &r, sizeof r) != (ssize_t)sizeof r) {
                perror("gutenfree: hand a book to the workers");
                break;
            }
            sent++;
        }
    }
    pclose(fetch);
    t_fetch = gf_us_since(t0);
    close(fds[1]); /* the workers see the end once the pipe is empty */
    for (i = 0; i < workers; i++) {
        int status = 0;
        FILE *f;
        while (waitpid(pid[i], &status, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            bad++;
        if ((f = fopen(ops_path[i], "rb")) != NULL) {
            gf_bench_merge(f);
            fclose(f);
            remove(ops_path[i]);
        }
    }
    printf("pipeline: %u books to %u writers, %u held out; downloads done after %.1f s, writers after %.1f s; "
           "%u writers reported failures\n", sent, workers, nheld, t_fetch / 1e6, gf_us_since(t0) / 1e6, bad);

    GF_TIME("db.reopen", s = gf_db_open(g, base));
    if (s != CDB_OK) {
        fprintf(stderr, "gutenfree: reopen: %s\n", cdb_status_str(s));
        return 1;
    }
    if ((s = gf_catalog_open(g, &c)) != CDB_OK || (s = gf_books_open(g, &b)) != CDB_OK
        || (s = gf_vec_open(g, &v)) != CDB_OK) {
        fprintf(stderr, "gutenfree: open stores: %s\n", cdb_status_str(s));
        return 1;
    }
    (void)b;
    if ((s = gf_vec_link_similar(g, &v, &links)) == CDB_OK)
        s = gf_vec_refresh_graph(g, &v, NULL, NULL, NULL);
    printf("similar_to: %llu links added; neighbour graph: %s\n", (unsigned long long)links, cdb_status_str(s));
    if (s == CDB_OK && (e = gf_embed_open(model)) != NULL) {
        unsigned nq = held_queries(e, dir, held, nheld, held_q);
        s = gf_search_checks(g, &v, e, 200, (const float (*)[GF_EMBED_DIMS])held_q, nq);
        gf_embed_close(e);
    }
    return s == CDB_OK && bad == 0 ? 0 : 1;
}
