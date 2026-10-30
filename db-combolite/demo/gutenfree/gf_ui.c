#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Engine headers before curses.h (gf_main.c). */
#include "gf_ui.h"
#include "gf_metrics.h"
#include "gf_probe.h"
#include "cdb_doc.h"
#include "cdb_link.h"
#include "cdb_vec.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curses.h>

/* ------------------------------------------------------------ helpers */

static int is_lead(unsigned char b) { return (b & 0xC0) != 0x80; }

void gf_put_clip(int y, int x, const char *s, int width)
{
    size_t n = 0;
    int cols = 0;
    while (s[n] != '\0') {
        if (is_lead((unsigned char)s[n])) {
            if (cols == width)
                break;
            cols++;
        }
        n++;
    }
    mvaddnstr(y, x, s, (int)n);
}

int gf_put_wrap(int y, int x, int width, int maxy, const char *s)
{
    while (*s != '\0' && y < maxy) {
        size_t n = 0, brk = 0;
        int cols = 0;
        while (s[n] != '\0' && s[n] != '\n') {
            if (is_lead((unsigned char)s[n])) {
                if (cols == width)
                    break;
                cols++;
            }
            if (s[n] == ' ')
                brk = n;
            n++;
        }
        if (s[n] != '\0' && s[n] != '\n' && brk > 0)
            n = brk;
        mvaddnstr(y++, x, s, (int)n);
        s += n;
        while (*s == ' ' || *s == '\n')
            s++;
    }
    return y;
}

void gf_status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    attron(A_REVERSE);
    move(LINES - 1, 0);
    clrtoeol();
    gf_put_clip(LINES - 1, 0, buf, COLS);
    for (int x = getcurx(stdscr); x < COLS; x++)
        addch(' ');
    attroff(A_REVERSE);
}

int gf_prompt(const char *question, char *out, int cap)
{
    int n;
    move(LINES - 1, 0);
    clrtoeol();
    mvprintw(LINES - 1, 0, "%s", question);
    echo();
    curs_set(1);
    n = getnstr(out, cap - 1) == OK ? (int)strlen(out) : 0;
    noecho();
    curs_set(0);
    return n;
}

/* ------------------------------------------------------------ book */

static int field(int y, const char *label, const char *value, int maxy)
{
    if (value[0] == '\0' || y >= maxy)
        return y;
    attron(A_BOLD);
    mvprintw(y, 2, "%-10s", label);
    attroff(A_BOLD);
    return gf_put_wrap(y, 13, COLS - 15, maxy, value) + 1;
}

void gf_screen_book(gf_app *a, uint64_t entry)
{
    gf_book b;
    char num[32], text[96];
    double t0 = gf_now_ms(), ms;
    cdb_status s = gf_catalog_read(a->g, &a->cat, entry, &b);
    int y = 1;

    ms = gf_now_ms() - t0;
    erase();
    if (s != CDB_OK) {
        mvprintw(1, 2, "cannot read row %llu: %s", (unsigned long long)entry, cdb_status_str(s));
    } else {
        snprintf(num, sizeof num, "#%lld", (long long)b.ebook);
        y = field(y, "Ebook", num, LINES - 2);
        y = field(y, "Title", b.title, LINES - 2);
        y = field(y, "Subtitle", b.subtitle, LINES - 2);
        y = field(y, "Author", b.author, LINES - 2);
        y = field(y, "Language", b.language, LINES - 2);
        y = field(y, "Credits", b.credits, LINES - 2);
        y = field(y, "Authors", b.authors, LINES - 2);
        y = field(y, "Genre", b.genre, LINES - 2);
        y = field(y, "Shelves", b.shelves, LINES - 2);
        y = field(y, "Subjects", b.subjects, LINES - 2);
        y = field(y, "Issued", b.issued, LINES - 2);
        y = field(y, "LoCC", b.locc, LINES - 2);
        y = field(y, "Type", b.type, LINES - 2);
        if (b.passages > 0)
            snprintf(text, sizeof text, "%llu passages: press r to read", (unsigned long long)b.passages);
        else
            snprintf(text, sizeof text, "not on the shelf yet: press a to fetch and add it");
        field(y, "Text", text, LINES - 2);
    }
    gf_status(" row %llu read in %.3f ms | r: read | a: add | m: more like this | any other key: back ",
              (unsigned long long)entry, ms);
    refresh();
    switch (getch()) {
    case 'r':
        if (s == CDB_OK && b.passages > 0)
            gf_screen_reader(a, b.first_passage);
        break;
    case 'a':
        if (s == CDB_OK)
            gf_screen_add(a, entry);
        break;
    case 'm':
        gf_screen_similar(a, entry);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ catalog */

#define MATCH_MAX 200000u
#define QUEUE_MAX 8192u

/* The download queue: catalog rows marked for fetching. */
static uint64_t queue[QUEUE_MAX];
static unsigned nqueue;

static int queued(uint64_t row)
{
    for (unsigned i = 0; i < nqueue; i++)
        if (queue[i] == row)
            return (int)i;
    return -1;
}

static void enqueue(uint64_t row)
{
    if (queued(row) < 0 && nqueue < QUEUE_MAX)
        queue[nqueue++] = row;
}

static void dequeue(uint64_t row)
{
    int i = queued(row);
    if (i >= 0)
        queue[i] = queue[--nqueue];
}

static const struct {
    uint32_t index;
    const char *name;
} sorts[] = {
    { GF_CATALOG_SORT_INDEX, "title" },     { GF_CATALOG_GENRE_INDEX, "genre" },
    { GF_CATALOG_AUTHOR_INDEX, "author" },  { GF_CATALOG_ISSUED_INDEX, "date issued" },
    { GF_CATALOG_LANG_INDEX, "language" },  { GF_CATALOG_LOCC_INDEX, "LoCC class" },
    { GF_CATALOG_PK_INDEX, "ebook #" },
};
#define NSORTS (sizeof sorts / sizeof sorts[0])

/* The filter form: returns 1 to apply, 0 to cancel. */
static int filter_form(gf_filter *f)
{
    struct {
        const char *label;
        char *buf;
        int cap;
        const char *hint;
    } fl[] = {
        { "Title contains", f->title, (int)sizeof f->title, "title or subtitle" },
        { "Author contains", f->author, (int)sizeof f->author, "e.g. Verne" },
        { "Genre / bookshelf", f->genre, (int)sizeof f->genre, "e.g. Science Fiction, Poetry, Category: Novels" },
        { "Subject contains", f->subject, (int)sizeof f->subject, "e.g. Detective, Whaling, France -- History" },
        { "Language", f->language, (int)sizeof f->language, "e.g. English, French" },
        { "LoCC class starts", f->locc, (int)sizeof f->locc, "e.g. PS (American lit.), PR, Q (science)" },
        { "Type", f->type, (int)sizeof f->type, "Text or Sound" },
        { "Issued from", f->from, (int)sizeof f->from, "YYYY or YYYY-MM-DD" },
        { "Issued to", f->to, (int)sizeof f->to, "YYYY or YYYY-MM-DD" },
    };
    int nf = (int)(sizeof fl / sizeof fl[0]), sel = 0, i;
    for (;;) {
        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "Catalog filter: every filled-in condition must hold (case is ignored)");
        attroff(A_BOLD);
        for (i = 0; i <= nf; i++) {
            if (i == sel)
                attron(A_REVERSE);
            if (i < nf)
                mvprintw(2 + i, 4, "%-20s [%-30s]", fl[i].label, fl[i].buf);
            else
                mvprintw(2 + i, 4, "%-20s [%c]", "Only books on shelf", f->on_shelf ? 'x' : ' ');
            if (i == sel)
                attroff(A_REVERSE);
            if (i < nf)
                printw("  %s", fl[i].hint);
        }
        mvprintw(4 + nf, 4, "Genres come from pg_catalog.csv bookshelves (gf_fetch.py --catalog, then merge-csv).");
        gf_status(" up/down: field | Enter: edit (space toggles) | c: clear all | a: apply | q: cancel ");
        refresh();
        switch (getch()) {
        case KEY_UP: case 'k':
            if (sel > 0)
                sel--;
            break;
        case KEY_DOWN: case 'j': case '\t':
            if (sel < nf)
                sel++;
            break;
        case '\n': case KEY_ENTER: case ' ':
            if (sel == nf) {
                f->on_shelf = !f->on_shelf;
            } else {
                char q[96], buf[96];
                snprintf(q, sizeof q, " %s: ", fl[sel].label);
                buf[0] = '\0';
                gf_prompt(q, buf, fl[sel].cap < 96 ? fl[sel].cap : 96);
                snprintf(fl[sel].buf, (size_t)fl[sel].cap, "%s", buf); /* empty clears it */
            }
            break;
        case 'c':
            memset(f, 0, sizeof *f);
            break;
        case 'a':
            return 1;
        case 'q': case 27:
            return 0;
        default:
            break;
        }
    }
}

static void filter_summary(const gf_filter *f, char *out, size_t cap)
{
    out[0] = '\0';
#define PART(cond, fmt, val)                                                                                           \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            size_t l = strlen(out);                                                                                    \
            snprintf(out + l, cap - l, fmt, val);                                                                      \
        }                                                                                                              \
    } while (0)
    PART(f->title[0], "title~%s ", f->title);
    PART(f->author[0], "author~%s ", f->author);
    PART(f->genre[0], "genre~%s ", f->genre);
    PART(f->subject[0], "subject~%s ", f->subject);
    PART(f->language[0], "lang~%s ", f->language);
    PART(f->locc[0], "locc=%s* ", f->locc);
    PART(f->type[0], "type=%s ", f->type);
    PART(f->from[0], "from %s ", f->from);
    PART(f->to[0], "to %s ", f->to);
    PART(f->on_shelf, "%s", "on-shelf ");
#undef PART
    if (out[0] == '\0')
        snprintf(out, cap, "none");
}

void gf_screen_catalog(gf_app *a)
{
    static uint64_t *list;
    static gf_filter f;
    static unsigned sort;
    size_t n = 0;
    uint64_t scanned = 0;
    int sel = 0, top = 0, done = 0, apply = 1;
    double filter_ms = 0, t0;
    char note[160] = "", summary[256];

    if (list == NULL && (list = malloc(MATCH_MAX * sizeof *list)) == NULL)
        return;
    while (!done) {
        int rows = LINES - 3, ch, i;
        if (apply) {
            erase();
            mvprintw(2, 2, "Filtering by %s ...", sorts[sort].name);
            refresh();
            t0 = gf_now_ms();
            gf_catalog_filter(a->g, &a->cat, sorts[sort].index, &f, list, MATCH_MAX, &n, &scanned);
            filter_ms = gf_now_ms() - t0;
            gf_metric("catalog.filter_ms", filter_ms);
            gf_metrics_flush(a->g);
            sel = top = 0;
            apply = 0;
            if (scanned == 0) {
                erase();
                mvprintw(2, 2, "The catalog is empty. Press [i] on the main menu to ingest GUTINDEX.");
                gf_status(" any key: back ");
                refresh();
                getch();
                return;
            }
        }
        if (rows < 1)
            rows = 1;
        if (sel < top)
            top = sel;
        if (sel >= top + rows)
            top = sel - rows + 1;

        erase();
        filter_summary(&f, summary, sizeof summary);
        attron(A_BOLD);
        mvprintw(0, 1, "Card catalog by %s", sorts[sort].name);
        attroff(A_BOLD);
        printw(" | filter: %s| %zu of %llu books (%.0f ms) | queue: %u", summary, n, (unsigned long long)scanned,
               filter_ms, nqueue);
        for (i = 0; i < rows && (size_t)(top + i) < n; i++) {
            static gf_book b;
            char line[1200];
            const char *extra = "";
            uint64_t row = list[top + i];
            int y = 1 + i;
            gf_catalog_read(a->g, &a->cat, row, &b);
            switch (sorts[sort].index) {
            case GF_CATALOG_GENRE_INDEX: extra = b.genre; break;
            case GF_CATALOG_ISSUED_INDEX: extra = b.issued; break;
            case GF_CATALOG_LANG_INDEX: extra = b.language; break;
            case GF_CATALOG_LOCC_INDEX: extra = b.locc; break;
            default: break;
            }
            snprintf(line, sizeof line, "%s%s%7lld  %s%s%s%s%s%s", queued(row) >= 0 ? "[*]" : "[ ]",
                     b.passages > 0 ? "\xe2\x96\xa0" : " ", (long long)b.ebook, extra[0] ? "[" : "", extra,
                     extra[0] ? "] " : "", b.title, b.author[0] ? " -- " : "", b.author);
            if (top + i == sel)
                attron(A_REVERSE);
            move(y, 0);
            clrtoeol();
            gf_put_clip(y, 0, line, COLS - 1);
            if (top + i == sel)
                attroff(A_REVERSE);
        }
        if (note[0])
            mvprintw(LINES - 2, 2, "%s", note);
        gf_status(" Space: queue | A: queue all %zu | F: fetch %u queued | x: clear | /: filter | o: sort | Enter: details | a: add | g: ebook # | q ",
                  n, nqueue);
        refresh();

        ch = getch();
        note[0] = '\0';
        switch (ch) {
        case KEY_UP: case 'k':
            if (sel > 0)
                sel--;
            break;
        case KEY_DOWN: case 'j':
            if ((size_t)(sel + 1) < n)
                sel++;
            break;
        case KEY_NPAGE:
            sel = (size_t)(sel + rows) < n ? sel + rows : (n > 0 ? (int)n - 1 : 0);
            break;
        case KEY_PPAGE:
            sel = sel > rows ? sel - rows : 0;
            break;
        case KEY_HOME:
            sel = 0;
            break;
        case KEY_END:
            sel = n > 0 ? (int)n - 1 : 0;
            break;
        case ' ':
            if (n > 0) {
                if (queued(list[sel]) >= 0)
                    dequeue(list[sel]);
                else
                    enqueue(list[sel]);
                if ((size_t)(sel + 1) < n)
                    sel++;
            }
            break;
        case 'A':
            for (size_t k = 0; k < n && nqueue < QUEUE_MAX; k++)
                enqueue(list[k]);
            snprintf(note, sizeof note, "queue now holds %u books (F fetches them)", nqueue);
            break;
        case 'x':
            nqueue = 0;
            break;
        case 'F':
            if (nqueue > 0) {
                gf_screen_fetch_queue(a, queue, nqueue);
                apply = 1; /* on-shelf marks changed */
            }
            break;
        case '/':
            if (filter_form(&f))
                apply = 1;
            break;
        case 'o':
            sort = (sort + 1) % NSORTS;
            apply = 1;
            break;
        case '\n': case KEY_ENTER:
            if (n > 0)
                gf_screen_book(a, list[sel]);
            break;
        case 'a':
            if (n > 0)
                gf_screen_add(a, list[sel]);
            break;
        case 'g': {
            char buf[24];
            uint64_t e;
            int found = 0;
            if (gf_prompt(" go to ebook #: ", buf, sizeof buf) > 0) {
                t0 = gf_now_ms();
                gf_catalog_find(a->g, &a->cat, strtoll(buf, NULL, 10), &e, &found);
                if (found)
                    gf_screen_book(a, e);
                else
                    snprintf(note, sizeof note, "no ebook #%s (primary-key lookup %.3f ms)", buf, gf_now_ms() - t0);
            }
            break;
        }
        case 'q': case 27:
            done = 1;
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------ ingest */

#define HIST 512

typedef struct ingest_view {
    double batch_ms[HIST];
    unsigned n;
} ingest_view;

static void draw_ingest(const gf_ingest_stats *st, void *ctx)
{
    ingest_view *v = ctx;
    int top = 9, height = LINES - top - 2, width = COLS - 4, i;
    double max = 1;
    unsigned first;

    v->batch_ms[v->n % HIST] = st->last_batch_ms;
    v->n++;
    gf_metric("catalog.batch_ms", st->last_batch_ms);
    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "Ingesting GUTINDEX into the card catalog");
    attroff(A_BOLD);
    mvprintw(2, 4, "entries parsed   %10llu", (unsigned long long)st->entries);
    mvprintw(3, 4, "rows inserted    %10llu", (unsigned long long)st->inserted);
    mvprintw(4, 4, "already present  %10llu", (unsigned long long)st->duplicates);
    mvprintw(5, 4, "refused          %10llu", (unsigned long long)st->refused);
    mvprintw(6, 4, "commits          %10u   last %.1f ms, worst %.1f ms (%u rows each)", st->batches,
             st->last_batch_ms, st->worst_batch_ms, st->batch_rows);
    mvprintw(8, 4, "time per commit batch:");
    if (height < 3 || width < 10) {
        refresh();
        return;
    }
    first = v->n > (unsigned)width ? v->n - (unsigned)width : 0;
    for (unsigned k = first; k < v->n; k++)
        if (v->batch_ms[k % HIST] > max)
            max = v->batch_ms[k % HIST];
    for (unsigned k = first; k < v->n; k++) {
        int h = (int)(v->batch_ms[k % HIST] / max * (double)height + 0.5);
        for (i = 0; i < h; i++)
            mvaddch(top + height - 1 - i, 2 + (int)(k - first), ACS_CKBOARD);
    }
    mvprintw(top - 1, COLS - 20, "top: %.1f ms", max);
    refresh();
}

void gf_screen_ingest(gf_app *a, const char *gutindex)
{
    static ingest_view v;
    gf_ingest_stats st;
    cdb_status s;

    memset(&v, 0, sizeof v);
    erase();
    mvprintw(2, 2, "Reading %s ...", gutindex);
    refresh();
    s = gf_catalog_ingest(a->g, &a->cat, gutindex, GF_INGEST_BATCH, &st, draw_ingest, &v);
    if (st.ms > 0)
        gf_metric("catalog.rows_per_s", (double)st.entries / (st.ms / 1000.0));
    gf_metrics_flush(a->g);
    if (s == CDB_OK && access(GF_DEFAULT_PGCSV, R_OK) == 0) { /* genres, subjects, dates */
        gf_ingest_stats ms;
        memset(&v, 0, sizeof v);
        s = gf_catalog_merge_csv(a->g, &a->cat, GF_DEFAULT_PGCSV, &ms, draw_ingest, &v);
        mvprintw(LINES - 4, 4, "pg_catalog.csv: %llu rows updated, %llu new, in %.0f ms",
                 (unsigned long long)ms.updated, (unsigned long long)ms.inserted, ms.ms);
    }
    if (s != CDB_OK)
        mvprintw(LINES - 3, 4, "ingest stopped: %s", cdb_status_str(s));
    else
        mvprintw(LINES - 3, 4, "done in %.0f ms (%.0f ms parsing text, %.0f ms in the engine), %.0f rows/s",
                 st.ms, st.parse_ms, st.ms - st.parse_ms,
                 st.ms > 0 ? (double)st.inserted / (st.ms / 1000.0) : 0.0);
    gf_status(" any key: back ");
    refresh();
    getch();
}

/* ------------------------------------------------------------ lists */

#define PICK_MAX 200
#define PICK_LINE 512

/* Shows lines[0..n) under a title; returns the one chosen with Enter, or
 * -1. status is drawn at the bottom. */
static int pick(const char *title, char (*lines)[PICK_LINE], int n, const char *status)
{
    int sel = 0, top = 0;
    for (;;) {
        int rows = LINES - 3, ch, i;
        if (sel < top)
            top = sel;
        if (sel >= top + rows)
            top = sel - rows + 1;
        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "%s", title);
        attroff(A_BOLD);
        for (i = 0; i < rows && top + i < n; i++) {
            if (top + i == sel)
                attron(A_REVERSE);
            move(1 + i, 0);
            clrtoeol();
            gf_put_clip(1 + i, 1, lines[top + i], COLS - 2);
            if (top + i == sel)
                attroff(A_REVERSE);
        }
        if (n == 0)
            mvprintw(2, 2, "nothing here");
        gf_status("%s", status);
        refresh();
        ch = getch();
        if ((ch == KEY_UP || ch == 'k') && sel > 0)
            sel--;
        else if ((ch == KEY_DOWN || ch == 'j') && sel + 1 < n)
            sel++;
        else if (ch == KEY_NPAGE || ch == ' ')
            sel = sel + rows < n ? sel + rows : (n > 0 ? n - 1 : 0);
        else if (ch == KEY_PPAGE || ch == 'b')
            sel = sel > rows ? sel - rows : 0;
        else if ((ch == '\n' || ch == KEY_ENTER) && n > 0)
            return sel;
        else if (ch == 'q' || ch == 27)
            return -1;
    }
}

static char lines[PICK_MAX][PICK_LINE];
static uint64_t targets[PICK_MAX];

void gf_screen_shelf(gf_app *a)
{
    cdb_table_cursor cur;
    uint64_t rows[PICK_MAX];
    size_t n = 0, i;
    char status[160];
    double t0 = gf_now_ms(), ms;
    int k;

    memset(&cur, 0, sizeof cur);
    gf_catalog_shelf(a->g, &a->cat, &cur, rows, PICK_MAX, &n);
    for (i = 0; i < n; i++) {
        gf_book b;
        gf_catalog_read(a->g, &a->cat, rows[i], &b);
        snprintf(lines[i], PICK_LINE, "%7lld  %.250s%s%.200s  (%llu passages)", (long long)b.ebook, b.title,
                 b.author[0] ? " -- " : "", b.author, (unsigned long long)b.passages);
        targets[i] = b.first_passage;
    }
    ms = gf_now_ms() - t0;
    snprintf(status, sizeof status, " %zu books with text (shelf index scan + rows: %.2f ms) | Enter: read | q: back ",
             n, ms);
    while ((k = pick("Shelf: books with text", lines, (int)n, status)) >= 0)
        gf_screen_reader(a, targets[k]);
}

/* ------------------------------------------------------------ reader */

typedef struct textrow { const char *p; int len; } textrow;

/* Wraps text into screen rows, keeping its own line ends and blank lines. */
static int layout(const char *text, int width, textrow *rows, int max)
{
    int n = 0;
    while (*text != '\0' && n < max) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        do {
            size_t take = 0, brk = 0;
            int cols = 0;
            while (take < len) {
                if (is_lead((unsigned char)text[take])) {
                    if (cols == width)
                        break;
                    cols++;
                }
                if (text[take] == ' ')
                    brk = take;
                take++;
            }
            if (take < len && brk > 0)
                take = brk + 1;
            rows[n].p = text, rows[n].len = (int)take, n++;
            text += take, len -= take;
        } while (len > 0 && n < max);
        text += eol ? 1 : 0;
        if (eol == NULL)
            break;
    }
    return n;
}

void gf_screen_reader(gf_app *a, uint64_t passage)
{
    static char text[GF_PASSAGE_MAX + 1];
    static textrow rows[1024];
    gf_book b;
    int64_t ebook = 0, seq = 0;
    int top = 0, nrows = 0, reload = 1;
    double ms = 0;
    cdb_status s = CDB_OK;

    memset(&b, 0, sizeof b);
    for (;;) {
        int width = COLS - 8 > 20 ? (COLS - 8 < 76 ? COLS - 8 : 76) : 20, height = LINES - 3, ch, i;
        if (reload) {
            double t0 = gf_now_ms();
            uint64_t row_entry;
            int found = 0;
            s = gf_books_passage(a->g, &a->books, passage, text, sizeof text, &ebook, &seq);
            if (s == CDB_OK && ebook != b.ebook && gf_catalog_find(a->g, &a->cat, ebook, &row_entry, &found) == CDB_OK
                && found)
                gf_catalog_read(a->g, &a->cat, row_entry, &b);
            ms = gf_now_ms() - t0;
            top = 0;
            reload = 0;
        }
        nrows = layout(text, width, rows, 1024);
        erase();
        attron(A_BOLD);
        gf_put_clip(0, 2, b.title, COLS - 4);
        attroff(A_BOLD);
        if (s != CDB_OK)
            mvprintw(2, 4, "passage %llu: %s", (unsigned long long)passage, cdb_status_str(s));
        for (i = 0; i < height && top + i < nrows; i++)
            mvaddnstr(2 + i, 4, rows[top + i].p, rows[top + i].len);
        gf_status(" passage %lld of %llu | read %.3f ms | space: next, b: back, j/k: scroll, q: close ", (long long)seq + 1,
                  (unsigned long long)b.passages, ms);
        refresh();
        ch = getch();
        if (ch == 'q' || ch == 27)
            return;
        if ((ch == 'j' || ch == KEY_DOWN) && top + height < nrows)
            top++;
        else if ((ch == 'k' || ch == KEY_UP) && top > 0)
            top--;
        else if ((ch == ' ' || ch == KEY_NPAGE) && b.passages > 0 && (uint64_t)seq + 1 < b.passages)
            passage++, reload = 1;
        else if ((ch == 'b' || ch == KEY_PPAGE) && seq > 0)
            passage--, reload = 1;
    }
}

/* ------------------------------------------------------------ search */

/* The text around the first query word, on one line. */
static void snippet(const char *text, const char *query, char *out, size_t cap)
{
    char word[64];
    size_t n = 0, i, from = 0;
    const char *hit = NULL;
    while (*query == ' ')
        query++;
    while (query[n] != '\0' && query[n] != ' ' && n < sizeof word - 1)
        word[n] = query[n], n++;
    word[n] = '\0';
    for (i = 0; n > 0 && text[i] != '\0' && hit == NULL; i++)
        if (strncasecmp(text + i, word, n) == 0)
            hit = text + i;
    if (hit != NULL && hit - text > 30) {
        from = (size_t)(hit - text) - 30;
        while (from > 0 && !is_lead((unsigned char)text[from]))
            from--;
    }
    for (i = 0; i + 1 < cap && text[from + i] != '\0'; i++)
        out[i] = text[from + i] == '\n' ? ' ' : text[from + i];
    out[i] = '\0';
}

void gf_screen_search(gf_app *a)
{
    static char text[GF_PASSAGE_MAX + 1];
    cdb_fts_hit hits[PICK_MAX];
    char query[128], status[200], snip[160];
    size_t n = 0, i;
    double t0, search_ms, read_ms;
    int mode = CDB_FTS_ALL, k;
    cdb_status s;

    if (gf_prompt(" search passages: ", query, sizeof query) == 0)
        return;
    t0 = gf_now_ms();
    s = cdb_doc_search(&a->g->db, a->books.pool, query, strlen(query), mode, hits, PICK_MAX, &n);
    if (s == CDB_OK && n == 0) { /* no passage has every word: any of them */
        mode = CDB_FTS_ANY;
        s = cdb_doc_search(&a->g->db, a->books.pool, query, strlen(query), mode, hits, PICK_MAX, &n);
    }
    search_ms = gf_now_ms() - t0;
    if (s == CDB_OK) {
        gf_metric("probe.search_ms", search_ms);
        gf_metrics_flush(a->g);
    }
    t0 = gf_now_ms();
    for (i = 0; s == CDB_OK && i < n; i++) {
        int64_t ebook, seq;
        uint64_t row_entry;
        int found = 0;
        gf_book b;
        b.title[0] = '\0';
        gf_books_passage(a->g, &a->books, hits[i].entry, text, sizeof text, &ebook, &seq);
        if (gf_catalog_find(a->g, &a->cat, ebook, &row_entry, &found) == CDB_OK && found)
            gf_catalog_read(a->g, &a->cat, row_entry, &b);
        snippet(text, query, snip, sizeof snip);
        snprintf(lines[i], PICK_LINE, "%6.2f  %.40s [%lld]  %s", hits[i].score, b.title, (long long)seq + 1, snip);
        targets[i] = hits[i].entry;
    }
    read_ms = gf_now_ms() - t0;
    if (s != CDB_OK)
        snprintf(status, sizeof status, " search failed: %s ", cdb_status_str(s));
    else
        snprintf(status, sizeof status, " %zu hits (%s words) | BM25 %.2f ms, reading hits %.2f ms | Enter: read | q: back ",
                 n, mode == CDB_FTS_ALL ? "all" : "any", search_ms, read_ms);
    while ((k = pick("Search results (score, title [passage], text)", lines, (int)n, status)) >= 0)
        gf_screen_reader(a, targets[k]);
}

/* ------------------------------------------------------------ diagnostics */

#define SPARK 32

typedef struct gauge {
    double recent[SPARK];
    size_t n;
    uint64_t points, seen;  /* seen: points at the previous frame */
    int blink;              /* frames left to show the light lit */
    double read_ms;
} gauge;

static void spark(int y, int x, const double *v, size_t n)
{
    static const char *const bars[] = { "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
                                        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88" };
    double lo = v[0], hi = v[0];
    size_t i;
    for (i = 1; i < n; i++) {
        if (v[i] < lo)
            lo = v[i];
        if (v[i] > hi)
            hi = v[i];
    }
    move(y, x);
    for (i = 0; i < n; i++) {
        int k = hi > lo ? (int)((v[i] - lo) / (hi - lo) * 7.0 + 0.5) : 3;
        addstr(bars[k]);
    }
}

static void context_panel(const gf_measure *m, const gauge *gg)
{
    int w = COLS - 8 < 76 ? COLS - 8 : 76, y = 3, x = (COLS - w) / 2, r;
    const char *cmp = m->higher_is_better ? ">=" : "<=";
    char buf[160];
    for (r = 2; r < LINES - 2; r++) {
        move(r, x - 2);
        for (int c = 0; c < w + 4; c++)
            addch(' ');
    }
    attron(A_BOLD);
    mvprintw(y++, x, "%s (%s, series %s)", m->label, m->unit, m->name);
    attroff(A_BOLD);
    y = gf_put_wrap(y + 1, x, w, LINES - 9, m->explain) + 1;
    if (has_colors())
        attron(COLOR_PAIR(3));
    snprintf(buf, sizeof buf, "exceptional   %s %g %s", cmp, m->exceptional, m->unit);
    mvprintw(y++, x, "%s", buf);
    if (has_colors())
        attron(COLOR_PAIR(2));
    snprintf(buf, sizeof buf, "satisfactory  %s %g %s", cmp, m->satisfactory, m->unit);
    mvprintw(y++, x, "%s", buf);
    if (has_colors())
        attron(COLOR_PAIR(1));
    mvprintw(y++, x, "poor          otherwise");
    if (has_colors())
        attroff(COLOR_PAIR(1));
    y++;
    mvprintw(y++, x, "%llu points stored; the last %zu read back in %.2f ms.", (unsigned long long)gg->points,
             gg->n, gg->read_ms);
    gf_put_wrap(y + 1, x, w, LINES - 2,
                "Thresholds are first guesses for a workstation: spec 2.8's device-class table, which would "
                "set them per class of device, has no values yet.");
    gf_status(" any key: back ");
    refresh();
    timeout(-1);
    getch();
}

static long file_size(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_size : -1;
}

void gf_screen_diag(gf_app *a)
{
    static gauge gs[32];
    static gf_probe p;
    size_t i;
    int sel = 0, load = 0, frame = 0;
    double tick_ms = 0;
    char path[1100];

    memset(gs, 0, sizeof gs);
    gf_probe_init(a, &p);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_RED, -1);
        init_pair(2, COLOR_YELLOW, -1);
        init_pair(3, COLOR_GREEN, -1);
        init_pair(4, COLOR_CYAN, -1);
    }

    for (;;) {
        int ch, y = 2;
        unsigned m;
        double t0;
        long arch, jour;

        if (load) {
            t0 = gf_now_ms();
            gf_probe_tick(a, &p);
            tick_ms = gf_now_ms() - t0;
        }
        gf_metrics_flush(a->g);
        for (m = 0; m < gf_nmeasures && m < 32; m++) {
            gauge *g = &gs[m];
            gf_metrics_recent(a->g, gf_measures[m].name, g->recent, SPARK, &g->n, &g->points, &g->read_ms);
            if (g->points != g->seen)
                g->blink = 2;
            g->seen = g->points;
        }

        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "Diagnostics: db-combolite measuring itself");
        attroff(A_BOLD);
        printw("   probe load: ");
        if (load && has_colors())
            attron(COLOR_PAIR(3) | A_BOLD);
        printw(load ? "ON" : "off");
        if (load && has_colors())
            attroff(COLOR_PAIR(3) | A_BOLD);
        printw("   build: %s", GF_BUILD_LABEL);
        mvprintw(1, 4, "%-28s %10s %10s %10s %10s  %-*s", "measure", "last", "avg", "min", "max", SPARK, "recent");
        for (m = 0; m < gf_nmeasures && y < LINES - 5; m++, y++) {
            const gf_measure *ms = &gf_measures[m];
            gauge *g = &gs[m];
            double lo = 0, hi = 0, sum = 0, last = 0;
            int band = -1, pair;
            for (i = 0; i < g->n; i++) {
                double v = g->recent[i];
                lo = i == 0 || v < lo ? v : lo;
                hi = i == 0 || v > hi ? v : hi;
                sum += v;
            }
            if (g->n > 0) {
                last = g->recent[g->n - 1];
                band = gf_measure_band(ms, sum / (double)g->n);
            }
            pair = band == 2 ? 3 : band == 1 ? 2 : band == 0 ? 1 : 0;
            /* the light: lit while new points arrive */
            if (has_colors() && pair)
                attron(COLOR_PAIR(pair) | (g->blink > 0 ? A_BOLD : A_DIM));
            mvaddstr(y, 1, g->blink > 0 && frame % 2 == 0 ? "\xe2\x97\x8f" : (g->n > 0 ? "\xe2\x97\x8b" : " "));
            if (has_colors() && pair)
                attroff(COLOR_PAIR(pair) | A_BOLD | A_DIM);
            if (g->blink > 0)
                g->blink--;
            if (m == (unsigned)sel)
                attron(A_REVERSE);
            mvprintw(y, 3, " %-28.28s", ms->label);
            if (g->n > 0)
                printw("%10.2f %10.2f %10.2f %10.2f ", last, sum / (double)g->n, lo, hi);
            else
                printw("%10s %10s %10s %10s ", "--", "--", "--", "--");
            if (m == (unsigned)sel)
                attroff(A_REVERSE);
            addch(' ');
            if (g->n > 0) {
                if (has_colors() && pair)
                    attron(COLOR_PAIR(pair));
                spark(y, getcurx(stdscr), g->recent, g->n);
                printw("  %s", band == 2 ? "exceptional" : band == 1 ? "satisfactory" : "poor");
                if (has_colors() && pair)
                    attroff(COLOR_PAIR(pair));
            }
        }
        snprintf(path, sizeof path, "%s.cdbarch", a->base);
        arch = file_size(path);
        snprintf(path, sizeof path, "%s.cdbwork/main.0.cdb", a->base);
        jour = file_size(path);
        y++;
        if (has_colors())
            attron(COLOR_PAIR(4));
        mvprintw(y++, 4, "archive %.2f MB | journal %.2f MB | engine buffers %.1f MB", (double)arch / 1048576.0,
                 (double)jour / 1048576.0, (double)a->g->bytes / 1048576.0);
        mvprintw(y++, 4, "telemetry points written %llu, dropped %llu | probe books %u | last tick %.1f ms",
                 (unsigned long long)gf_metrics_written(), (unsigned long long)gf_metrics_dropped(), p.nbooks, tick_ms);
        if (has_colors())
            attroff(COLOR_PAIR(4));
        if (gf_metrics_error()[0] != '\0') {
            attron(A_BOLD | (has_colors() ? COLOR_PAIR(1) : 0));
            mvprintw(y++, 4, "telemetry write failed: %s", gf_metrics_error());
            attroff(A_BOLD | (has_colors() ? COLOR_PAIR(1) : 0));
        }
        gf_status(" up/down: choose | Enter: what this measures | L: probe load on/off | q: back ");
        refresh();

        timeout(load ? 100 : 500);
        ch = getch();
        frame++;
        if (ch == 'q' || ch == 27)
            break;
        if ((ch == KEY_UP || ch == 'k') && sel > 0)
            sel--;
        else if ((ch == KEY_DOWN || ch == 'j') && sel + 1 < (int)gf_nmeasures)
            sel++;
        else if (ch == 'L' || ch == 'l')
            load = !load;
        else if (ch == '\n' || ch == KEY_ENTER)
            context_panel(&gf_measures[sel], &gs[sel]);
    }
    timeout(-1);
}

/* ------------------------------------------------------------ vectors */

static int load_model(gf_app *a)
{
    double t0;
    if (a->emb != NULL)
        return 1;
    erase();
    mvprintw(2, 2, "Loading the embedding model %s ...", a->model);
    refresh();
    t0 = gf_now_ms();
    a->emb = gf_embed_open(a->model);
    if (a->emb == NULL) {
        mvprintw(4, 2, "Could not load it. Run demo/gutenfree/tools/gf_model.py first.");
        gf_status(" any key: back ");
        refresh();
        getch();
        return 0;
    }
    gf_metric("embed.load_ms", gf_now_ms() - t0);
    return 1;
}

/* How many of b's first n hits have a source among a's first n. */
static unsigned agree(const cdb_vec_hit *a, size_t na, const cdb_vec_hit *b, size_t nb, size_t n)
{
    unsigned k = 0;
    for (size_t i = 0; i < nb && i < n; i++)
        for (size_t j = 0; j < na && j < n; j++)
            if (a[j].source_entry == b[i].source_entry) {
                k++;
                break;
            }
    return k;
}

void gf_screen_semantic(gf_app *a)
{
    static char text[GF_PASSAGE_MAX + 1];
    static char q[600];
    char query[400], status[256], snip[160];
    float qv[GF_EMBED_DIMS];
    uint8_t enc[2048];
    cdb_vec_hit exact[PICK_MAX], approx[10], bits[10];
    size_t ne = 0, na = 0, nb = 0, len, i;
    double t0, embed_ms, exact_ms, approx_ms, bits_ms;
    unsigned ra, rb;
    int k;

    if (!load_model(a) || gf_prompt(" semantic search: ", query, sizeof query) == 0)
        return;
    snprintf(q, sizeof q, "%s%s", GF_EMBED_QUERY_PREFIX, query);
    t0 = gf_now_ms();
    if (gf_embed_text(a->emb, q, strlen(q), qv, NULL) != 0)
        return;
    embed_ms = gf_now_ms() - t0;

    len = gf_vec_encode(qv, 0, enc, sizeof enc);
    t0 = gf_now_ms();
    cdb_vec_search_mode(&a->g->db, a->vec.passages, enc, len, CDB_VEC_BASE, CDB_VEC_EXACT, exact, 20, &ne);
    exact_ms = gf_now_ms() - t0;
    t0 = gf_now_ms();
    cdb_vec_search_mode(&a->g->db, a->vec.passages, enc, len, CDB_VEC_BASE, CDB_VEC_APPROX, approx, 10, &na);
    approx_ms = gf_now_ms() - t0;
    len = gf_vec_encode(qv, 1, enc, sizeof enc);
    t0 = gf_now_ms();
    cdb_vec_search(&a->g->db, a->vec.bits, enc, len, CDB_VEC_BASE, bits, 10, &nb);
    bits_ms = gf_now_ms() - t0;
    ra = agree(exact, ne, approx, na, 10);
    rb = agree(exact, ne, bits, nb, 10);
    gf_metric("vec.search_ms", exact_ms);
    if (ne >= 10) {
        gf_metric("vec.approx_recall", ra / 10.0);
        gf_metric("vec.bits_agreement", rb / 10.0);
    }
    gf_metrics_flush(a->g);

    for (i = 0; i < ne; i++) {
        int64_t ebook = 0, seq = 0;
        uint64_t row;
        int found = 0;
        gf_book b;
        b.title[0] = '\0';
        gf_books_passage(a->g, &a->books, exact[i].source_entry, text, sizeof text, &ebook, &seq);
        if (gf_catalog_find(a->g, &a->cat, ebook, &row, &found) == CDB_OK && found)
            gf_catalog_read(a->g, &a->cat, row, &b);
        snippet(text, "", snip, sizeof snip);
        snprintf(lines[i], PICK_LINE, "%.3f%s %.40s [%lld]  %s", 1.0 - exact[i].distance,
                 exact[i].source_state == CDB_VEC_SOURCE_CURRENT ? " " : "*", b.title, (long long)seq + 1, snip);
        targets[i] = exact[i].source_entry;
    }
    snprintf(status, sizeof status,
             " embed %.1f ms | exact %.2f ms | graph walk %.2f ms, %u/10 | 1-bit %.2f ms, %u/10 | * = stale | Enter: read ",
             embed_ms, exact_ms, approx_ms, ra, bits_ms, rb);
    while ((k = pick("Semantic search (similarity, title [passage], text)", lines, (int)ne, status)) >= 0)
        gf_screen_reader(a, targets[k]);
}

void gf_screen_similar(gf_app *a, uint64_t row)
{
    cdb_link out[GF_SIMILAR * 4];
    uint64_t rows[GF_SIMILAR * 4];
    cdb_ref from;
    size_t n = 0, i, m = 0;
    int found, k;
    char status[160];
    double t0 = gf_now_ms();

    if (gf_vec_book_entry(a->g, &a->cat, &a->vec, row, &from.entry, &found) == CDB_OK && found) {
        from.store = a->vec.books;
        cdb_link_list(&a->g->db, from, CDB_LINK_OUT, a->vec.similar, NULL, out, GF_SIMILAR * 4, &n);
    }
    for (i = 0; i < n; i++) {
        uint8_t enc[2048];
        size_t len;
        uint64_t ss, se;
        gf_book b;
        if ((out[i].flags & CDB_LINK_TOMBSTONE) != 0
            || cdb_vec_get(&a->g->db, a->vec.books, out[i].target.entry, enc, sizeof enc, &len, &ss, &se) != CDB_OK
            || gf_catalog_read(a->g, &a->cat, se, &b) != CDB_OK)
            continue;
        snprintf(lines[m], PICK_LINE, "%.3f  %7lld  %.250s%s%.200s", out[i].weight, (long long)b.ebook, b.title,
                 b.author[0] ? " -- " : "", b.author);
        rows[m++] = se;
    }
    snprintf(status, sizeof status, " %zu similar_to links (links + vectors + rows: %.2f ms) | Enter: open | q: back ",
             m, gf_now_ms() - t0);
    if (!found)
        snprintf(status, sizeof status, " this book has no vectors yet: press e on the main menu | q: back ");
    while ((k = pick("More like this (similarity, ebook, title)", lines, (int)m, status)) >= 0)
        gf_screen_book(a, rows[k]);
}

/* ------------------------------------------------------------ embedding */

static void bar(int y, int x, int width, double frac)
{
    int fill = (int)(frac * (double)width + 0.5), i;
    move(y, x);
    addch('[');
    for (i = 0; i < width; i++)
        addch(i < fill ? ACS_CKBOARD : ' ');
    addch(']');
}

void gf_screen_embed(gf_app *a)
{
    static uint64_t rows[4096];
    cdb_table_cursor cur;
    size_t n = 0, i, remaining = 0;
    uint64_t done_passages = 0, total_passages = 0, links = 0;
    unsigned done = 0, skipped = 0;
    int stop = 0, width = COLS - 12 > 10 ? COLS - 12 : 10;
    double t0 = gf_now_ms(), t;
    cdb_status s = CDB_OK;
    gf_book b;

    if (!load_model(a))
        return;
    memset(&cur, 0, sizeof cur);
    gf_catalog_shelf(a->g, &a->cat, &cur, rows, 4096, &n);
    for (i = 0; i < n; i++)
        if (gf_catalog_read(a->g, &a->cat, rows[i], &b) == CDB_OK)
            total_passages += b.passages;

    timeout(0); /* poll for q between books */
    for (i = 0; i < n && !stop && s == CDB_OK; i++) {
        gf_embed_stats st;
        gf_catalog_read(a->g, &a->cat, rows[i], &b);
        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "Embedding passages with %s", a->model);
        attroff(A_BOLD);
        mvprintw(2, 4, "book %zu of %zu: %.60s", i + 1, n, b.title);
        mvprintw(3, 4, "%llu passages; embedded %u books, %u already had vectors", (unsigned long long)b.passages,
                 done, skipped);
        bar(5, 4, width, total_passages ? (double)done_passages / (double)total_passages : 0.0);
        mvprintw(6, 4, "%llu of %llu passages, %.0f s so far", (unsigned long long)done_passages,
                 (unsigned long long)total_passages, (gf_now_ms() - t0) / 1000.0);
        gf_status(" q: stop after this book | build: %s ", GF_BUILD_LABEL);
        refresh();

        s = gf_vec_embed_book(a->g, &a->cat, &a->books, &a->vec, a->emb, rows[i], &st);
        done_passages += b.passages;
        if (s == CDB_E_EXISTS) {
            skipped++;
            s = CDB_OK;
        } else if (s == CDB_OK) {
            done++;
            gf_metric("embed.ms_per_passage", st.embed_ms / (double)st.passages);
            gf_metric("vec.commit_us_per_vector", st.commit_ms * 1000.0 / (double)(2 * st.passages + 1));
            gf_metrics_flush(a->g);
        }
        if (getch() == 'q')
            stop = 1;
    }
    timeout(-1);

    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "Embedding passages");
    attroff(A_BOLD);
    if (s != CDB_OK) {
        mvprintw(2, 4, "stopped at book %zu: %s", i, cdb_status_str(s));
    } else {
        mvprintw(2, 4, "%u books embedded, %u already had vectors%s, %.0f s", done, skipped,
                 stop ? " (stopped early)" : "", (gf_now_ms() - t0) / 1000.0);
        mvprintw(4, 4, "linking similar books ...");
        refresh();
        t = gf_now_ms();
        s = gf_vec_link_similar(a->g, &a->vec, &links);
        mvprintw(4, 4, "similar_to: %s, %llu links added in %.1f ms", cdb_status_str(s), (unsigned long long)links,
                 gf_now_ms() - t);
        mvprintw(5, 4, "refreshing the neighbour graph ...");
        refresh();
        t = gf_now_ms();
        if (s == CDB_OK)
            s = cdb_vec_refresh(&a->g->db, a->vec.passages, 0, &remaining);
        mvprintw(5, 4, "neighbour graph: %s, refreshed in %.0f ms, %zu pending", cdb_status_str(s), gf_now_ms() - t,
                 remaining);
        gf_metric("vec.graph_refresh_ms", gf_now_ms() - t);
        gf_metrics_flush(a->g);
    }
    gf_status(" any key: back ");
    refresh();
    getch();
}

/* ------------------------------------------------------------ adding a book */

#define FETCH_SCRIPT "demo/gutenfree/tools/gf_fetch.py"

/* Runs gf_fetch.py for one ebook, without a shell, printing its output from
 * row *y down. Returns its exit status (0: fetched), or -1. */
static int run_fetch(gf_app *a, int64_t ebook, int *y)
{
    char num[24], line[512];
    char *argv[] = { "python3", "-I", FETCH_SCRIPT, "--mirror", NULL, "--ebook", num, "--pause", "0", "--out",
                     NULL, NULL };
    int fds[2], status = -1;
    pid_t pid;
    FILE *out;

    snprintf(num, sizeof num, "%lld", (long long)ebook);
    argv[4] = (char *)a->mirror;
    argv[10] = (char *)a->books_dir;
    if (pipe(fds) != 0)
        return -1;
    if ((pid = fork()) < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    if ((out = fdopen(fds[0], "r")) != NULL) {
        while (fgets(line, sizeof line, out) != NULL) {
            line[strcspn(line, "\n")] = '\0';
            if (*y < LINES - 4)
                gf_put_clip((*y)++, 6, line, COLS - 8);
            refresh();
        }
        fclose(out);
    } else {
        close(fds[0]);
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

/* Fetches, stores and (with the model) embeds one book, reporting from row
 * *y down. Returns 1 when the book is now on the shelf. */
static int add_one(gf_app *a, uint64_t row, int *y)
{
    gf_book b;
    gf_book_stats st;
    cdb_status s;
    int rc;
    double t = gf_now_ms();

    if (gf_catalog_read(a->g, &a->cat, row, &b) != CDB_OK)
        return 0;
    if (b.passages > 0)
        return 1;
    mvprintw((*y)++, 4, "fetching #%lld from %s ...", (long long)b.ebook, a->mirror);
    refresh();
    rc = run_fetch(a, b.ebook, y);
    if (rc != 0) {
        mvprintw((*y)++, 4, rc < 0 ? "could not run python3 %s" : "no text on the mirror for this ebook (%s)",
                 FETCH_SCRIPT);
        return 0;
    }
    mvprintw((*y)++, 4, "fetched in %.1f s; storing passages and the full-text index ...",
             (gf_now_ms() - t) / 1000.0);
    refresh();
    s = gf_books_ingest_file(a->g, &a->cat, &a->books, b.ebook, a->books_dir, &st);
    if (s != CDB_OK) {
        mvprintw((*y)++, 4, "store failed at %s: %s", st.step, cdb_status_str(s));
        return 0;
    }
    mvprintw((*y)++, 4, "stored %llu bytes as %llu passages in %.0f ms (commit %.0f ms)",
             (unsigned long long)st.bytes, (unsigned long long)st.passages, st.ms, st.commit_ms);
    gf_metric("book.ms_per_passage", st.ms / (double)st.passages);
    if (a->emb == NULL && access(a->model, R_OK) == 0)
        a->emb = gf_embed_open(a->model);
    if (a->emb != NULL) {
        gf_embed_stats es;
        mvprintw((*y)++, 4, "embedding %llu passages ...", (unsigned long long)st.passages);
        refresh();
        s = gf_vec_embed_book(a->g, &a->cat, &a->books, &a->vec, a->emb, row, &es);
        if (s == CDB_OK) {
            gf_metric("embed.ms_per_passage", es.embed_ms / (double)es.passages);
            gf_metric("vec.commit_us_per_vector", es.commit_ms * 1000.0 / (double)(2 * es.passages + 1));
            mvprintw((*y)++, 4, "embedded in %.1f s (%.0f ms per passage)", es.embed_ms / 1000.0,
                     es.embed_ms / (double)es.passages);
        } else {
            mvprintw((*y)++, 4, "embedding failed: %s", cdb_status_str(s));
        }
    } else {
        mvprintw((*y)++, 4, "no embedding model (tools/gf_model.py): vectors skipped");
    }
    gf_metrics_flush(a->g);
    refresh();
    return 1;
}

/* After a batch: similar_to links and the neighbour graph, once. */
static void finish_vectors(gf_app *a, int y)
{
    uint64_t links = 0;
    size_t remaining = 0;
    double t;
    cdb_status s;
    if (a->emb == NULL)
        return;
    mvprintw(y, 4, "linking similar books and refreshing the neighbour graph ...");
    refresh();
    t = gf_now_ms();
    s = gf_vec_link_similar(a->g, &a->vec, &links);
    if (s == CDB_OK)
        s = cdb_vec_refresh(&a->g->db, a->vec.passages, 0, &remaining);
    gf_metric("vec.graph_refresh_ms", gf_now_ms() - t);
    gf_metrics_flush(a->g);
    move(y, 0);
    clrtoeol();
    mvprintw(y, 4, "%llu similar_to links added; graph refreshed in %.1f s: %s", (unsigned long long)links,
             (gf_now_ms() - t) / 1000.0, cdb_status_str(s));
}

void gf_screen_fetch_queue(gf_app *a, uint64_t *rows, unsigned n)
{
    unsigned i, added = 0, failed = 0, skipped = 0;
    int stop = 0;
    double t0 = gf_now_ms();

    if (n > 50) {
        erase();
        attron(A_BOLD);
        mvprintw(2, 2, "Fetch %u books?", n);
        attroff(A_BOLD);
        mvprintw(4, 4, "The mirror is asked for one book at a time, with a 2 s pause between books.");
        mvprintw(5, 4, "Embedding takes about 80 ms per passage, roughly 10-30 s per book on this machine.");
        mvprintw(6, 4, "Estimate: %.0f to %.0f minutes. q stops after the current book.", n * 5.0 / 60.0,
                 n * 35.0 / 60.0);
        gf_status(" y: start | any other key: cancel ");
        refresh();
        if (getch() != 'y')
            return;
    }
    for (i = 0; i < n && !stop; i++) {
        gf_book b;
        int y = 4;
        if (gf_catalog_read(a->g, &a->cat, rows[i], &b) != CDB_OK)
            continue;
        if (b.passages > 0) {
            skipped++;
            continue;
        }
        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "Fetching the queue: book %u of %u", i + 1, n);
        attroff(A_BOLD);
        mvprintw(1, 4, "%u added, %u failed, %u already on the shelf, %.0f s so far", added, failed, skipped,
                 (gf_now_ms() - t0) / 1000.0);
        gf_put_clip(2, 4, b.title, COLS - 8);
        gf_status(" q: stop after this book ");
        refresh();
        if (add_one(a, rows[i], &y))
            added++;
        else
            failed++;
        timeout(0);
        if (getch() == 'q')
            stop = 1;
        timeout(-1);
        if (!stop && i + 1 < n)
            napms(2000); /* be gentle with the mirror */
    }
    /* Keep only what did not make it onto the shelf. */
    for (i = 0; i < n;) {
        gf_book b;
        if (gf_catalog_read(a->g, &a->cat, rows[i], &b) == CDB_OK && b.passages > 0)
            rows[i] = rows[--n];
        else
            i++;
    }
    nqueue = n;
    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "Queue done%s", stop ? " (stopped early)" : "");
    attroff(A_BOLD);
    mvprintw(2, 4, "%u added, %u failed, %u already on the shelf, in %.0f s; %u left in the queue", added, failed,
             skipped, (gf_now_ms() - t0) / 1000.0, nqueue);
    if (added > 0)
        finish_vectors(a, 4);
    gf_status(" any key: back ");
    refresh();
    getch();
}

void gf_screen_add(gf_app *a, uint64_t row)
{
    gf_book b;
    int y = 2;

    if (gf_catalog_read(a->g, &a->cat, row, &b) != CDB_OK)
        return;
    if (b.passages == 0) {
        erase();
        attron(A_BOLD);
        mvprintw(0, 2, "Adding #%lld: %.60s", (long long)b.ebook, b.title);
        attroff(A_BOLD);
        gf_status(" working ... ");
        refresh();
        if (add_one(a, row, &y))
            finish_vectors(a, y + 1);
        gf_status(" r: read it now | any other key: back ");
        refresh();
        if (getch() != 'r' || gf_catalog_read(a->g, &a->cat, row, &b) != CDB_OK)
            return;
    }
    if (b.passages > 0)
        gf_screen_reader(a, b.first_passage);
}
