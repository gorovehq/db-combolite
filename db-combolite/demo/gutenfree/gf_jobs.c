#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Engine headers before curses.h (gf_main.c). */
#include "gf_jobs.h"
#include "gf_metrics.h"
#include "cdb_vec.h"

#include <curses.h>

#define QUEUE_MAX 8192u
#define FETCH_SCRIPT "demo/gutenfree/tools/gf_fetch.py"
#define PAUSE_MS 2000.0  /* between books, for the mirror */
#define GRAPH_BUDGET 64u /* neighbour-graph entries refreshed per slice */

static uint64_t queue[QUEUE_MAX];
static unsigned char qstate[QUEUE_MAX];
static unsigned nqueue;

enum { J_IDLE, J_NEXT, J_FETCH, J_STORE, J_EMBED, J_COMMIT, J_PAUSE, J_LINK, J_GRAPH };

static struct {
    gf_app *a;
    int state;
    int stop;           /* stop after the current book */
    int cur;            /* queue index being worked on, -1 for none */
    pid_t child;
    int fd;             /* the child's output */
    char last[160];     /* its last line */
    char lbuf[512];
    size_t llen;
    double pause_until;
    double graph_t0;
    gf_vec_job vj;
    unsigned added, failed, skipped;
    char status[200];
} J = { .cur = -1, .fd = -1 };

/* ------------------------------------------------------------ queue */

unsigned gf_queue_n(void) { return nqueue; }
uint64_t gf_queue_row(unsigned i) { return queue[i]; }
int gf_queue_state(unsigned i) { return qstate[i]; }

int gf_queue_has(uint64_t row)
{
    for (unsigned i = 0; i < nqueue; i++)
        if (queue[i] == row)
            return 1;
    return 0;
}

void gf_queue_add(uint64_t row)
{
    if (!gf_queue_has(row) && nqueue < QUEUE_MAX) {
        queue[nqueue] = row;
        qstate[nqueue++] = GF_Q_WAIT;
    }
}

/* Removes by shifting, so the order (and the index being worked on) holds. */
void gf_queue_remove(unsigned i)
{
    if (i >= nqueue || (int)i == J.cur)
        return;
    memmove(queue + i, queue + i + 1, (nqueue - i - 1) * sizeof queue[0]);
    memmove(qstate + i, qstate + i + 1, nqueue - i - 1);
    nqueue--;
    if (J.cur > (int)i)
        J.cur--;
}

void gf_queue_remove_row(uint64_t row)
{
    for (unsigned i = 0; i < nqueue; i++)
        if (queue[i] == row) {
            gf_queue_remove(i);
            return;
        }
}

void gf_queue_clear_finished(void)
{
    for (unsigned i = nqueue; i-- > 0;)
        if (qstate[i] == GF_Q_DONE || qstate[i] == GF_Q_SHELF)
            gf_queue_remove(i);
}

void gf_queue_clear(void)
{
    for (unsigned i = nqueue; i-- > 0;)
        gf_queue_remove(i);
}

unsigned gf_queue_waiting(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < nqueue; i++)
        n += qstate[i] == GF_Q_WAIT || qstate[i] == GF_Q_FAIL;
    return n;
}

/* ------------------------------------------------------------ the job */

int gf_jobs_active(void) { return J.state != J_IDLE; }

void gf_jobs_start(gf_app *a)
{
    J.a = a;
    for (unsigned i = 0; i < nqueue; i++)
        if (qstate[i] == GF_Q_FAIL)
            qstate[i] = GF_Q_WAIT;
    if (J.state == J_IDLE) {
        J.added = J.failed = J.skipped = 0;
        J.stop = 0;
        J.state = J_NEXT;
    }
}

void gf_jobs_stop(void) { J.stop = 1; }

const char *gf_jobs_status(void) { return J.status; }

/* The status line: "⇣ done/total <what>". */
static void set_status(const char *what)
{
    unsigned done = 0;
    for (unsigned i = 0; i < nqueue; i++)
        done += qstate[i] == GF_Q_DONE || qstate[i] == GF_Q_SHELF || qstate[i] == GF_Q_FAIL;
    snprintf(J.status, sizeof J.status, "\xe2\x87\xa3 %u/%u %s%s", done, nqueue, what, J.stop ? " (stopping)" : "");
}

static void set_statusf(const char *what, unsigned long long n, unsigned long long of)
{
    char buf[120];
    snprintf(buf, sizeof buf, what, n, of);
    set_status(buf);
}

/* Starts gf_fetch.py for one ebook without a shell; output on J.fd. */
static int spawn_fetch(int64_t ebook)
{
    char num[24];
    char *argv[] = { "python3", "-I", FETCH_SCRIPT, "--mirror", (char *)J.a->mirror, "--ebook", num, "--pause",
                     "0", "--out", (char *)J.a->books_dir, NULL };
    int fds[2];
    snprintf(num, sizeof num, "%lld", (long long)ebook);
    if (pipe(fds) != 0)
        return -1;
    if ((J.child = fork()) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (J.child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    J.fd = fds[0];
    J.llen = 0;
    J.last[0] = '\0';
    return 0;
}

/* Reads what the child wrote; returns 1 once it has exited (*ok set). */
static int poll_fetch(int *ok)
{
    char buf[512];
    ssize_t r;
    int status;
    while ((r = read(J.fd, buf, sizeof buf)) > 0) {
        for (ssize_t k = 0; k < r; k++) {
            if (buf[k] == '\n' || J.llen + 1 >= sizeof J.lbuf) {
                J.lbuf[J.llen] = '\0';
                if (J.llen > 0)
                    snprintf(J.last, sizeof J.last, "%.159s", J.lbuf);
                J.llen = 0;
            } else {
                J.lbuf[J.llen++] = buf[k];
            }
        }
    }
    if (waitpid(J.child, &status, WNOHANG) != J.child)
        return 0;
    while (read(J.fd, buf, sizeof buf) > 0)
        ;
    close(J.fd);
    J.fd = -1;
    *ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return 1;
}

static void finish_book(int state)
{
    qstate[J.cur] = (unsigned char)state;
    if (state == GF_Q_DONE)
        J.added++;
    else if (state == GF_Q_FAIL)
        J.failed++;
    J.cur = -1;
    gf_metrics_flush(J.a->g);
    J.pause_until = gf_now_ms() + PAUSE_MS;
    J.state = J_PAUSE;
}

/* One step of the state machine; returns 0 when it is waiting on time or
 * on the child, so the slice can end. */
static int step(void)
{
    gf_app *a = J.a;
    gf_book b;
    cdb_status s;
    int ok, done;

    switch (J.state) {
    case J_NEXT: {
        unsigned i;
        for (i = 0; i < nqueue && qstate[i] != GF_Q_WAIT; i++)
            ;
        if (i == nqueue || J.stop) {
            J.state = J.added > 0 ? J_LINK : J_IDLE;
            if (J.state == J_IDLE)
                snprintf(J.status, sizeof J.status, "\xe2\x87\xa3 done: %u added, %u failed", J.added, J.failed);
            return 1;
        }
        J.cur = (int)i;
        qstate[i] = GF_Q_RUN;
        if (gf_catalog_read(a->g, &a->cat, queue[i], &b) == CDB_OK && b.passages > 0) {
            qstate[i] = GF_Q_SHELF;
            J.skipped++;
            J.cur = -1;
            return 1;
        }
        if (spawn_fetch(b.ebook) != 0) {
            finish_book(GF_Q_FAIL);
            return 1;
        }
        set_statusf("fetching #%llu%.0llu", (unsigned long long)b.ebook, 0);
        J.state = J_FETCH;
        return 1;
    }
    case J_FETCH:
        if (!poll_fetch(&ok))
            return 0;
        if (!ok) {
            finish_book(GF_Q_FAIL);
            return 1;
        }
        J.state = J_STORE;
        return 1;
    case J_STORE: {
        gf_book_stats st;
        gf_catalog_read(a->g, &a->cat, queue[J.cur], &b);
        set_statusf("storing #%llu%.0llu", (unsigned long long)b.ebook, 0);
        s = gf_books_ingest_file(a->g, &a->cat, &a->books, b.ebook, a->books_dir, &st);
        if (s != CDB_OK) {
            finish_book(GF_Q_FAIL);
            return 1;
        }
        gf_metric("book.ms_per_passage", st.ms / (double)st.passages);
        if (a->emb == NULL && access(a->model, R_OK) == 0)
            a->emb = gf_embed_open(a->model);
        if (a->emb == NULL || gf_vec_job_start(a->g, &a->cat, &a->vec, queue[J.cur], &J.vj) != CDB_OK) {
            gf_vec_job_free(&J.vj);
            finish_book(GF_Q_DONE); /* stored; no vectors */
            return 1;
        }
        J.state = J_EMBED;
        return 1;
    }
    case J_EMBED:
        s = gf_vec_job_step(a->g, &a->books, a->emb, &J.vj, 100.0, &done);
        {
            char buf[120];
            snprintf(buf, sizeof buf, "embedding #%lld %llu/%llu", (long long)J.vj.bk.ebook,
                     (unsigned long long)J.vj.i, (unsigned long long)J.vj.n);
            set_status(buf);
        }
        if (s != CDB_OK) {
            gf_vec_job_free(&J.vj);
            finish_book(GF_Q_FAIL);
            return 1;
        }
        if (done)
            J.state = J_COMMIT;
        return 0; /* let the screen breathe between embedding slices */
    case J_COMMIT:
        s = gf_vec_job_commit(a->g, &a->cat, &a->books, &a->vec, &J.vj);
        if (s == CDB_OK) {
            gf_metric("embed.ms_per_passage", J.vj.st.embed_ms / (double)J.vj.st.passages);
            gf_metric("vec.commit_us_per_vector",
                      J.vj.st.commit_ms * 1000.0 / (double)(2 * J.vj.st.passages + 1));
        }
        gf_vec_job_free(&J.vj);
        finish_book(s == CDB_OK ? GF_Q_DONE : GF_Q_FAIL);
        return 1;
    case J_PAUSE:
        set_status("pausing for the mirror");
        if (gf_now_ms() < J.pause_until)
            return 0;
        J.state = J_NEXT;
        return 1;
    case J_LINK: {
        uint64_t links = 0;
        set_status("linking similar books");
        gf_vec_link_similar(a->g, &a->vec, &links);
        J.graph_t0 = gf_now_ms();
        J.state = J_GRAPH;
        return 0;
    }
    case J_GRAPH: {
        size_t remaining = 0;
        s = cdb_vec_refresh(&a->g->db, a->vec.passages, GRAPH_BUDGET, &remaining);
        set_statusf("neighbour graph: %llu pending%.0llu", (unsigned long long)remaining, 0);
        if (s != CDB_OK || remaining == 0) {
            gf_metric("vec.graph_refresh_ms", gf_now_ms() - J.graph_t0);
            gf_metrics_flush(a->g);
            snprintf(J.status, sizeof J.status, "\xe2\x87\xa3 done: %u added, %u failed, %u already on shelf",
                     J.added, J.failed, J.skipped);
            J.state = J_IDLE;
        }
        return 0;
    }
    default:
        return 0;
    }
}

void gf_jobs_tick(double budget_ms)
{
    double t0 = gf_now_ms();
    while (J.state != J_IDLE && gf_now_ms() - t0 < budget_ms)
        if (!step())
            break;
}

static gf_app *app;
static int in_diag;

void gf_jobs_set_app(gf_app *a) { app = a; }

int gf_key_diag(int ch)
{
    if (ch != KEY_F(2) && ch != 4 /* Ctrl-D */)
        return 0;
    if (app != NULL && !in_diag) {
        in_diag = 1;
        gf_screen_diag(app);
        in_diag = 0;
    }
    return 1;
}

static int read_key(void)
{
    int ch;
    if (!gf_jobs_active())
        return getch();
    for (;;) {
        timeout(100);
        ch = getch();
        if (ch != ERR) {
            timeout(-1);
            return ch;
        }
        gf_jobs_tick(100.0);
        gf_status_refresh();
        refresh();
        if (!gf_jobs_active()) {
            timeout(-1);
            return getch();
        }
    }
}

int gf_getch(void)
{
    int ch = read_key();
    /* The diagnostics key works everywhere; the screen underneath is then
     * told to redraw, as after a resize. */
    return gf_key_diag(ch) ? KEY_RESIZE : ch;
}
