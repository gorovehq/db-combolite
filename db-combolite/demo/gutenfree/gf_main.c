/* GutenFree Diet: an ncurses e-reader on db-combolite.
 *
 *   gutenfree [base]                        the e-reader
 *   gutenfree ingest-index [--batch N] [gutindex] [base]
 *                                           load GUTINDEX without the screen,
 *                                           committing every N rows
 *   gutenfree ingest-books [books-dir] [base]
 *                                           store the books gf_fetch.py fetched
 *                                           (listed in books-dir/manifest.tsv)
 *   gutenfree merge-csv [pg_catalog.csv] [base]
 *                                           add genres, subjects and dates
 *                                           (ingest-index does it too when
 *                                           the file is there; fetch it with
 *                                           gf_fetch.py --catalog)
 *   gutenfree filter key=value ...          the catalog filter: title= author=
 *                                           genre= subject= lang= locc= type=
 *                                           from= to= shelf sort=<index 0-7>
 *                                           (database from $GUTENFREE_DB)
 *   gutenfree probe [seconds] [base]        run the diagnostics probe load
 *   gutenfree metrics [base]                print what each measure holds
 *   gutenfree embed [model.gguf] [base]     embed every stored passage (the
 *                                           model from gf_model.py), link
 *                                           similar books, refresh the graph
 *
 * base is the database path without extension: the last argument, else
 * $GUTENFREE_DB, else data/gutenfree (giving data/gutenfree.cdbarch);
 * run from demo/gutenfree/, where the build puts the binary. Only one process may have a
 * database open at a time: the engine does not stop a second one yet. The screens arrive one at a time as the demo grows. */
#define _POSIX_C_SOURCE 200809L
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Engine headers before curses.h: curses defines erase() as a macro, which
 * would rewrite the storage interface's ops->erase call. */
#include "gf_books.h"
#include "gf_catalog.h"
#include "gf_db.h"
#include "gf_embed.h"
#include "gf_jobs.h"
#include "gf_metrics.h"
#include "gf_probe.h"
#include "gf_ui.h"
#include "gf_vec.h"
#include "cdb_vec.h"

#include <curses.h>

/* Relative to demo/gutenfree/, where the demo runs from. */
#define GF_DEFAULT_BASE "data/gutenfree"
#define GF_DEFAULT_GUTINDEX "data/GUTINDEX.ALL"
#define GF_DEFAULT_BOOKS "data/books"

typedef struct gf_item {
    int key;
    const char *label;
    const char *pending;  /* NULL once the screen exists */
} gf_item;

static const gf_item items[] = {
    { 'c', "Card catalog", NULL },
    { 'i', "Ingest GUTINDEX into the catalog", NULL },
    { 'r', "Shelf: read a book", NULL },
    { 's', "Search the text of every book", NULL },
    { 'v', "Semantic search (vectors)", NULL },
    { 'e', "Embed stored books (vectors, links, graph)", NULL },
    { 'f', "Download queue (runs in the background)", NULL },
    { 'd', "Diagnostics: gauges, live probe load (F2 or Ctrl-D from any screen)", NULL },
    { 'q', "Quit", NULL },
};
#define NITEMS (sizeof items / sizeof items[0])

static void draw(const char *base, const gf_db *g, double open_ms, const char *note)
{
    unsigned i;
    erase();
    attron(A_BOLD);
    mvprintw(1, 2, "GutenFree Diet");
    attroff(A_BOLD);
    mvprintw(2, 2, "an e-reader on db-combolite");
    for (i = 0; i < NITEMS; i++) {
        attron(A_BOLD);
        mvprintw(4 + (int)i, 4, "[%c]", items[i].key);
        attroff(A_BOLD);
        printw(" %s", items[i].label);
    }
    if (note != NULL)
        mvprintw(5 + (int)NITEMS, 4, "%s", note);
    gf_status(" %s.cdbarch | page %u B | buffers %.1f MB | opened in %.1f ms | build: %s ", base, GF_PAGE,
              (double)g->bytes / (1024.0 * 1024.0), open_ms, GF_BUILD_LABEL);
    refresh();
}

static void print_progress(const gf_ingest_stats *st, void *ctx)
{
    (void)ctx;
    gf_metric("catalog.batch_ms", st->last_batch_ms);
    if (st->batches % 100 == 0)
        printf("  %8llu entries, %8llu inserted, batch %4u: %6.1f ms (worst %.1f)\n",
               (unsigned long long)st->entries, (unsigned long long)st->inserted, st->batches,
               st->last_batch_ms, st->worst_batch_ms);
}

/* Merges pg_catalog.csv into the catalog: genres, subjects, dates. */
static cdb_status merge_csv(gf_db *g, const gf_catalog *c, const char *path)
{
    gf_ingest_stats st;
    cdb_status s = gf_catalog_merge_csv(g, c, path, &st, NULL, NULL);
    printf("merge %s: %s\n", path, cdb_status_str(s));
    printf("  %llu records: %llu rows updated, %llu new rows, %llu refused; %.0f ms, %u commits, worst %.1f ms\n",
           (unsigned long long)st.entries, (unsigned long long)st.updated, (unsigned long long)st.inserted,
           (unsigned long long)st.refused, st.ms, st.batches, st.worst_batch_ms);
    if (st.ms > 0)
        gf_metric("catalog.merge_rows_per_s", (double)st.entries / (st.ms / 1000.0));
    gf_metrics_flush(g);
    return s;
}

static int merge_cli(gf_db *g, const char *path)
{
    gf_catalog c;
    cdb_status s = gf_catalog_open(g, &c);
    if (s != CDB_OK) {
        fprintf(stderr, "gutenfree: catalog: %s\n", cdb_status_str(s));
        return 1;
    }
    return merge_csv(g, &c, path) == CDB_OK ? 0 : 1;
}

static int ingest_cli(gf_db *g, const char *gutindex, unsigned batch)
{
    gf_catalog c;
    gf_ingest_stats st;
    cdb_status s = gf_catalog_open(g, &c);
    double t0;
    uint64_t rows;

    if (s != CDB_OK) {
        fprintf(stderr, "gutenfree: catalog: %s\n", cdb_status_str(s));
        return 1;
    }
    s = gf_catalog_ingest(g, &c, gutindex, batch, &st, print_progress, NULL);
    if (st.ms > 0)
        gf_metric("catalog.rows_per_s", (double)st.entries / (st.ms / 1000.0));
    gf_metrics_flush(g);
    printf("ingest: %s, %u rows per commit\n", cdb_status_str(s), st.batch_rows);
    printf("  entries %llu, inserted %llu, already present %llu, refused %llu\n",
           (unsigned long long)st.entries, (unsigned long long)st.inserted,
           (unsigned long long)st.duplicates, (unsigned long long)st.refused);
    printf("  %.0f ms in all: %.0f ms parsing, %.0f ms in the engine; %u commits, worst %.1f ms\n", st.ms,
           st.parse_ms, st.ms - st.parse_ms, st.batches, st.worst_batch_ms);
    if (s == CDB_OK && access(GF_DEFAULT_PGCSV, R_OK) == 0)
        s = merge_csv(g, &c, GF_DEFAULT_PGCSV);
    t0 = gf_now_ms();
    if (gf_catalog_count(g, &c, &rows) == CDB_OK)
        printf("  table now holds %llu rows (counted in %.1f ms)\n", (unsigned long long)rows, gf_now_ms() - t0);
    return s == CDB_OK ? 0 : 1;
}

/* Stores every book listed in dir/manifest.tsv that has no passages yet. */
static int ingest_books_cli(gf_db *g, const char *dir)
{
    gf_catalog c;
    gf_books b;
    char path[1024], line[1024];
    FILE *mf;
    cdb_status s;
    unsigned done = 0, skipped = 0, failed = 0;
    uint64_t bytes = 0, passages = 0;
    double t0 = gf_now_ms();

    if ((s = gf_catalog_open(g, &c)) != CDB_OK || (s = gf_books_open(g, &b)) != CDB_OK) {
        fprintf(stderr, "gutenfree: open stores: %s\n", cdb_status_str(s));
        return 1;
    }
    snprintf(path, sizeof path, "%s/manifest.tsv", dir);
    if ((mf = fopen(path, "r")) == NULL) {
        fprintf(stderr, "gutenfree: no %s (run python3 gf_fetch.py first)\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, mf) != NULL) {
        long long ebook = strtoll(line, NULL, 10);
        gf_book_stats st;

        if (ebook <= 0)
            continue;
        s = gf_books_ingest_file(g, &c, &b, ebook, dir, &st);
        if (s == CDB_E_EXISTS) {
            skipped++;
            continue;
        }
        if (s != CDB_OK) {
            printf("  #%lld: %s: %s (after %llu passages)\n", ebook, st.step, cdb_status_str(s),
                   (unsigned long long)st.passages);
            failed++;
            continue;
        }
        done++;
        bytes += st.bytes;
        passages += st.passages;
        gf_metric("book.ms_per_passage", st.ms / (double)st.passages);
        gf_metrics_flush(g);
        printf("  #%lld: %llu bytes, %llu passages, %.1f ms (commit %.1f ms)\n", ebook,
               (unsigned long long)st.bytes, (unsigned long long)st.passages, st.ms, st.commit_ms);
    }
    fclose(mf);
    printf("books: %u stored, %u already stored, %u failed; %llu bytes in %llu passages, %.0f ms\n", done, skipped,
           failed, (unsigned long long)bytes, (unsigned long long)passages, gf_now_ms() - t0);
    return failed == 0 ? 0 : 1;
}

/* Embeds every book with passages but no vectors, then links and refreshes. */
static int embed_cli(gf_db *g, const char *model)
{
    static uint64_t rows[4096];
    gf_catalog c;
    gf_books b;
    gf_vec v;
    gf_embed *e;
    cdb_table_cursor cur;
    size_t n = 0, i, remaining = 0;
    uint64_t links = 0, total = 0;
    double t0;
    cdb_status s;

    if ((s = gf_catalog_open(g, &c)) != CDB_OK || (s = gf_books_open(g, &b)) != CDB_OK
        || (s = gf_vec_open(g, &v)) != CDB_OK) {
        fprintf(stderr, "gutenfree: open stores: %s\n", cdb_status_str(s));
        return 1;
    }
    t0 = gf_now_ms();
    if ((e = gf_embed_open(model)) == NULL) {
        fprintf(stderr, "gutenfree: cannot load %s (run python3 gf_model.py)\n", model);
        return 1;
    }
    printf("model loaded in %.0f ms\n", gf_now_ms() - t0);
    memset(&cur, 0, sizeof cur);
    gf_catalog_shelf(g, &c, &cur, rows, 4096, &n);
    for (i = 0; i < n; i++) {
        gf_embed_stats st;
        s = gf_vec_embed_book(g, &c, &b, &v, e, rows[i], &st);
        if (s == CDB_E_EXISTS)
            continue;
        if (s != CDB_OK) {
            printf("  row %llu: %s\n", (unsigned long long)rows[i], cdb_status_str(s));
            break;
        }
        total += st.passages;
        printf("  row %6llu: %4llu passages, %6llu tokens | embed %7.0f ms (%.1f ms/passage) | commit %6.1f ms\n",
               (unsigned long long)rows[i], (unsigned long long)st.passages, (unsigned long long)st.tokens,
               st.embed_ms, st.embed_ms / (double)st.passages, st.commit_ms);
        gf_metric("embed.ms_per_passage", st.embed_ms / (double)st.passages);
        gf_metric("vec.commit_us_per_vector", st.commit_ms * 1000.0 / (double)(2 * st.passages + 1));
        gf_metrics_flush(g);
    }
    gf_embed_close(e);
    if (s != CDB_OK && s != CDB_E_EXISTS)
        return 1;

    t0 = gf_now_ms();
    s = gf_vec_link_similar(g, &v, &links);
    printf("similar_to: %s, %llu links added in %.1f ms\n", cdb_status_str(s), (unsigned long long)links,
           gf_now_ms() - t0);
    t0 = gf_now_ms();
    if (s == CDB_OK)
        s = cdb_vec_refresh(&g->db, v.passages, 0, &remaining);
    printf("neighbour graph: %s, refreshed in %.0f ms, %zu pending\n", cdb_status_str(s), gf_now_ms() - t0,
           remaining);
    gf_metric("vec.graph_refresh_ms", gf_now_ms() - t0);
    gf_metrics_flush(g);
    printf("embedded %llu passages this run\n", (unsigned long long)total);
    return s == CDB_OK ? 0 : 1;
}

/* Runs the diagnostics screen's probe load for a number of seconds. */
static int probe_cli(gf_db *g, double seconds)
{
    static gf_app app;
    static gf_probe p;
    unsigned ticks = 0;
    double t0, end;
    cdb_status s;

    app.g = g;
    if ((s = gf_catalog_open(g, &app.cat)) != CDB_OK || (s = gf_books_open(g, &app.books)) != CDB_OK) {
        fprintf(stderr, "gutenfree: open stores: %s\n", cdb_status_str(s));
        return 1;
    }
    gf_probe_init(&app, &p);
    t0 = gf_now_ms();
    end = t0 + seconds * 1000.0;
    while (gf_now_ms() < end) {
        gf_probe_tick(&app, &p);
        ticks++;
        if (gf_metrics_error()[0] != '\0') {
            fprintf(stderr, "gutenfree: telemetry: %s\n", gf_metrics_error());
            return 1;
        }
    }
    printf("probe: %u ticks in %.0f ms over %u books\n", ticks, gf_now_ms() - t0, p.nbooks);
    return 0;
}

/* Applies a catalog filter given as key=value arguments and prints the
 * count and the first matches: the same code the catalog screen runs. */
static int filter_cli(gf_db *g, int argc, char **argv, int a)
{
    static uint64_t list[200000];
    gf_catalog c;
    gf_filter f;
    uint32_t index = GF_CATALOG_SORT_INDEX;
    size_t n = 0, i;
    uint64_t scanned = 0;
    double t0;
    cdb_status s;

    memset(&f, 0, sizeof f);
    for (; a < argc; a++) {
        const char *eq = strchr(argv[a], '=');
        const char *v = eq ? eq + 1 : "";
        size_t k = eq ? (size_t)(eq - argv[a]) : strlen(argv[a]);
#define SET(name, buf)                                                                                                 \
    if (strncmp(argv[a], name, k) == 0 && strlen(name) == k) {                                                         \
        snprintf(buf, sizeof buf, "%s", v);                                                                            \
        continue;                                                                                                      \
    }
        SET("title", f.title) SET("author", f.author) SET("genre", f.genre) SET("subject", f.subject)
        SET("lang", f.language) SET("locc", f.locc) SET("type", f.type) SET("from", f.from) SET("to", f.to)
#undef SET
        if (strncmp(argv[a], "shelf", k) == 0) {
            f.on_shelf = 1;
            continue;
        }
        if (strncmp(argv[a], "sort", k) == 0) {
            index = (uint32_t)strtoul(v, NULL, 10);
            continue;
        }
        fprintf(stderr, "gutenfree: unknown filter %s (title= author= genre= subject= lang= locc= type= from= to= "
                        "shelf sort=<index 0-7>)\n", argv[a]);
        return 1;
    }
    if ((s = gf_catalog_open(g, &c)) != CDB_OK)
        return 1;
    t0 = gf_now_ms();
    s = gf_catalog_filter(g, &c, index, &f, list, sizeof list / sizeof list[0], &n, &scanned);
    printf("filter: %s, %zu matches of %llu rows scanned in %.0f ms (index %u)\n", cdb_status_str(s), n,
           (unsigned long long)scanned, gf_now_ms() - t0, index);
    for (i = 0; i < n && i < 10; i++) {
        gf_book b;
        gf_catalog_read(g, &c, list[i], &b);
        printf("  %7lld  [%s | %s | %s]  %.60s -- %.40s\n", (long long)b.ebook, b.genre, b.issued, b.locc, b.title,
               b.author);
    }
    return s == CDB_OK ? 0 : 1;
}

/* Prints what each measure's series holds. */
static int metrics_cli(gf_db *g)
{
    static double v[64];
    unsigned m;
    for (m = 0; m < gf_nmeasures; m++) {
        size_t n = 0;
        uint64_t points = 0;
        double ms = 0;
        cdb_status s = gf_metrics_recent(g, gf_measures[m].name, v, 64, &n, &points, &ms);
        printf("  %-26s %s: %llu points", gf_measures[m].name, cdb_status_str(s), (unsigned long long)points);
        if (n > 0)
            printf(", last %.3f %s", v[n - 1], gf_measures[m].unit);
        printf(" (read %.2f ms)\n", ms);
    }
    return 0;
}

int main(int argc, char **argv)
{
    static gf_db g;
    static gf_app app;
    int cli = argc > 1 && strcmp(argv[1], "ingest-index") == 0;
    int cli_books = argc > 1 && strcmp(argv[1], "ingest-books") == 0;
    int cli_embed = argc > 1 && strcmp(argv[1], "embed") == 0;
    int cli_metrics = argc > 1 && strcmp(argv[1], "metrics") == 0;
    int cli_probe = argc > 1 && strcmp(argv[1], "probe") == 0;
    int cli_merge = argc > 1 && strcmp(argv[1], "merge-csv") == 0;
    int cli_filter = argc > 1 && strcmp(argv[1], "filter") == 0;
    int a = cli_filter ? argc : cli || cli_books || cli_embed || cli_metrics || cli_probe || cli_merge ? 2 : 1;
    const char *pgcsv = GF_DEFAULT_PGCSV;
    double seconds = 10;
    unsigned batch = GF_INGEST_BATCH;
    const char *gutindex = GF_DEFAULT_GUTINDEX, *base = GF_DEFAULT_BASE, *books = GF_DEFAULT_BOOKS;
    const char *model = GF_EMBED_DEFAULT_MODEL;
    const char *note = NULL;
    double t0, open_ms;
    cdb_status s;
    int quit = 0, rc = 0;

    if (cli && a + 1 < argc && strcmp(argv[a], "--batch") == 0) {
        batch = (unsigned)strtoul(argv[a + 1], NULL, 10);
        a += 2;
    }
    if (cli && a < argc)
        gutindex = argv[a++];
    if (cli_books && a < argc)
        books = argv[a++];
    if (cli_embed && a < argc)
        model = argv[a++];
    if (cli_probe && a < argc)
        seconds = strtod(argv[a++], NULL);
    if (cli_merge && a < argc)
        pgcsv = argv[a++];
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("gutenfree [base] | ingest-index [--batch N] [gutindex] [base] | merge-csv [csv] [base]\n"
               "          | ingest-books [dir] [base] | embed [model.gguf] [base] | filter key=value ...\n"
               "          | probe [seconds] [base] | metrics [base]\n"
               "base: database path without extension (default %s, or $GUTENFREE_DB).\n"
               "Run from demo/gutenfree/; see README.md there.\n", GF_DEFAULT_BASE);
        return 0;
    }
    if (getenv("GUTENFREE_DB") != NULL && getenv("GUTENFREE_DB")[0] != '\0')
        base = getenv("GUTENFREE_DB");
    if (a < argc)
        base = argv[a];
    setvbuf(stdout, NULL, _IOLBF, 0); /* progress lines appear as they happen */
    if (base[0] == '-') {
        fprintf(stderr, "gutenfree: unknown option %s (try --help)\n", base);
        return 1;
    }

    t0 = gf_now_ms();
    s = gf_db_open(&g, base);
    if (s != CDB_OK) {
        fprintf(stderr, "gutenfree: cannot open %s: %s\n", base, cdb_status_str(s));
        return 1;
    }
    open_ms = gf_now_ms() - t0;
    if ((s = gf_metrics_open(&g)) != CDB_OK)
        fprintf(stderr, "gutenfree: metrics store: %s\n", cdb_status_str(s));
    gf_metric("db.open_ms", open_ms);
    if ((s = gf_metrics_flush(&g)) != CDB_OK)
        fprintf(stderr, "gutenfree: metrics flush: %s\n", gf_metrics_error());

    if (cli_metrics) {
        rc = metrics_cli(&g);
    } else if (cli_probe) {
        rc = probe_cli(&g, seconds);
    } else if (cli_merge) {
        rc = merge_cli(&g, pgcsv);
    } else if (cli_filter) {
        rc = filter_cli(&g, argc, argv, 2);
    } else if (cli || cli_books || cli_embed) {
        printf("build: %s\nopen: %.1f ms\n", GF_BUILD_LABEL, open_ms);
        rc = cli ? ingest_cli(&g, gutindex, batch) : cli_books ? ingest_books_cli(&g, books) : embed_cli(&g, model);
    } else {
        app.g = &g;
        gf_jobs_set_app(&app);
        app.base = base;
        app.model = model;
        app.books_dir = books;
        app.mirror = getenv("GUTENFREE_MIRROR") != NULL && getenv("GUTENFREE_MIRROR")[0] != '\0'
                         ? getenv("GUTENFREE_MIRROR")
                         : "https://gutenberg.pglaf.org/";
        s = gf_catalog_open(&g, &app.cat);
        if (s == CDB_OK)
            s = gf_books_open(&g, &app.books);
        if (s == CDB_OK)
            s = gf_vec_open(&g, &app.vec);
        setlocale(LC_ALL, "");
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        if (s != CDB_OK)
            note = "the stores could not be opened";
        while (!quit) {
            int ch;
            unsigned i;
            draw(base, &g, open_ms, note);
            ch = gf_getch(); /* downloads go on while the menu waits */
            note = "unknown key";
            for (i = 0; i < NITEMS; i++) {
                if (ch != items[i].key)
                    continue;
                note = items[i].pending;
                if (note != NULL)
                    break;
                if (ch == 'q' && gf_jobs_active()) {
                    gf_status(" a download is running: the current book would be dropped. Quit anyway? y/n ");
                    refresh();
                    quit = gf_getch() == 'y';
                    note = quit ? NULL : "download still running (f shows it)";
                } else if (ch == 'q')
                    quit = 1;
                else if (s != CDB_OK)
                    note = "the stores could not be opened";
                else if (ch == 'c')
                    gf_screen_catalog(&app);
                else if (ch == 'i')
                    gf_screen_ingest(&app, GF_DEFAULT_GUTINDEX);
                else if (ch == 'r')
                    gf_screen_shelf(&app);
                else if (ch == 's')
                    gf_screen_search(&app);
                else if (ch == 'd')
                    gf_screen_diag(&app);
                else if (ch == 'v')
                    gf_screen_semantic(&app);
                else if (ch == 'e')
                    gf_screen_embed(&app);
                else if (ch == 'f')
                    gf_screen_queue(&app);
            }
        }
        endwin();
        gf_embed_close(app.emb);
    }

    gf_metrics_flush(&g);
    t0 = gf_now_ms();
    s = gf_db_close(&g);
    if (s != CDB_OK) {
        fprintf(stderr, "gutenfree: close failed: %s\n", cdb_status_str(s));
        return 1;
    }
    if (cli)
        printf("close: %.1f ms\n", gf_now_ms() - t0);
    return rc;
}
