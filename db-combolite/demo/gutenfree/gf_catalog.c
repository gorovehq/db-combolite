#define _POSIX_C_SOURCE 200809L
#include "gf_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <strings.h>

static const char *const col_names[GF_NCOLS] = {
    "ebook", "title", "author", "subtitle", "language", "credits", "sort_title",
};

static const char *const bib_names[GF_NBIB] = {
    "type", "issued", "authors", "subjects", "locc", "bookshelves", "genre", "author_sort",
};

/* The second schema change: the pg_catalog.csv columns and the sort indexes
 * on them (each ends with sort_title, so ties read in title order). */
static cdb_status open_bib(gf_db *g, gf_catalog *c)
{
    static const struct { unsigned col; uint32_t number; } ix_def[] = {
        { GF_BIB_GENRE, GF_CATALOG_GENRE_INDEX },   { GF_BIB_AUTHOR_SORT, GF_CATALOG_AUTHOR_INDEX },
        { GF_BIB_ISSUED, GF_CATALOG_ISSUED_INDEX }, { GF_NBIB, GF_CATALOG_LANG_INDEX },
        { GF_BIB_LOCC, GF_CATALOG_LOCC_INDEX },
    };
    cdb_status s = cdb_table_column_id(&g->db, c->store, "author_sort", 11, &c->bib[GF_BIB_AUTHOR_SORT]);
    unsigned i;

    if (s == CDB_E_NOT_FOUND) {
        cdb_table_column col;
        memset(&col, 0, sizeof col);
        col.type = CDB_TYPE_STRING;
        for (i = 0; i < GF_NBIB; i++) {
            col.name = bib_names[i], col.name_len = strlen(bib_names[i]);
            if ((s = cdb_table_add_column(&g->db, c->store, &col, &c->bib[i])) != CDB_OK)
                return s;
        }
        for (i = 0; i < sizeof ix_def / sizeof ix_def[0]; i++) {
            uint64_t f[2];
            uint32_t ix;
            /* GF_NBIB stands for the language column, which GUTINDEX filled */
            f[0] = ix_def[i].col == GF_NBIB ? c->field[GF_COL_LANGUAGE] : c->bib[ix_def[i].col];
            f[1] = c->field[GF_COL_SORT];
            if ((s = cdb_table_create_index(&g->db, c->store, f, 2, 0, &ix)) != CDB_OK)
                return s;
            if (ix != ix_def[i].number)
                return CDB_E_STATE;
        }
        return CDB_OK;
    }
    for (i = 0; s == CDB_OK && i < GF_NBIB; i++)
        s = cdb_table_column_id(&g->db, c->store, bib_names[i], strlen(bib_names[i]), &c->bib[i]);
    return s;
}

cdb_status gf_catalog_open(gf_db *g, gf_catalog *c)
{
    cdb_status s;
    int found;
    unsigned i;

    memset(c, 0, sizeof *c);
    if ((s = cdb_db_find_store(&g->db, "books", 5, &c->store, &found)) != CDB_OK)
        return s;
    if (!found) {
        cdb_table_column cols[GF_NCOLS];
        static const unsigned pk[1] = { GF_COL_EBOOK };
        cdb_table_def def;
        uint32_t ix;
        memset(cols, 0, sizeof cols);
        for (i = 0; i < GF_NCOLS; i++) {
            cols[i].name = col_names[i];
            cols[i].name_len = strlen(col_names[i]);
            cols[i].type = CDB_TYPE_STRING;
        }
        cols[GF_COL_EBOOK].type = CDB_TYPE_INT;
        cols[GF_COL_TITLE].not_null = 1;
        cols[GF_COL_LANGUAGE].not_null = 1;
        cols[GF_COL_SORT].not_null = 1;
        memset(&def, 0, sizeof def);
        def.columns = cols;
        def.ncolumns = GF_NCOLS;
        def.primary_key = pk;
        def.npk = 1;
        def.keep = 1; /* the catalog needs no history */
        if ((s = cdb_table_create(&g->db, "books", 5, &def, &c->store)) != CDB_OK)
            return s;
        for (i = 0; i < GF_NCOLS; i++)
            if ((s = cdb_table_column_id(&g->db, c->store, col_names[i], strlen(col_names[i]), &c->field[i]))
                != CDB_OK)
                return s;
        if ((s = cdb_table_create_index(&g->db, c->store, &c->field[GF_COL_SORT], 1, 0, &ix)) != CDB_OK)
            return s;
        if (ix != GF_CATALOG_SORT_INDEX)
            return CDB_E_STATE;
    }
    for (i = 0; i < GF_NCOLS; i++)
        if ((s = cdb_table_column_id(&g->db, c->store, col_names[i], strlen(col_names[i]), &c->field[i]))
            != CDB_OK)
            return s;

    /* The schema change: passage columns and the shelf index, once. */
    s = cdb_table_column_id(&g->db, c->store, "passages", 8, &c->passages_field);
    if (s == CDB_E_NOT_FOUND) {
        cdb_table_column col;
        uint32_t ix;
        memset(&col, 0, sizeof col);
        col.type = CDB_TYPE_INT;
        col.name = "first_passage", col.name_len = 13;
        if ((s = cdb_table_add_column(&g->db, c->store, &col, &c->first_field)) != CDB_OK)
            return s;
        col.name = "passages", col.name_len = 8;
        if ((s = cdb_table_add_column(&g->db, c->store, &col, &c->passages_field)) != CDB_OK)
            return s;
        if ((s = cdb_table_create_index(&g->db, c->store, &c->passages_field, 1, 0, &ix)) != CDB_OK)
            return s;
        if (ix != GF_CATALOG_SHELF_INDEX)
            return CDB_E_STATE;
    } else if (s != CDB_OK
               || (s = cdb_table_column_id(&g->db, c->store, "first_passage", 13, &c->first_field)) != CDB_OK) {
        return s;
    }
    return open_bib(g, c);
}

cdb_status gf_catalog_set_passages(gf_db *g, const gf_catalog *c, uint64_t entry, uint64_t first, uint64_t count)
{
    uint8_t buf[64];
    cdb_value_writer w;
    int swap = c->first_field > c->passages_field;
    cdb_value_writer_init(&w, buf, sizeof buf, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, swap ? c->passages_field : c->first_field);
    cdb_value_write_int(&w, (int64_t)(swap ? count : first));
    cdb_value_field(&w, swap ? c->first_field : c->passages_field);
    cdb_value_write_int(&w, (int64_t)(swap ? first : count));
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error;
    return cdb_table_update(&g->db, c->store, entry, buf, cdb_value_writer_len(&w));
}

cdb_status gf_catalog_shelf(gf_db *g, const gf_catalog *c, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                            size_t *n)
{
    /* passages from 1 to the largest int: rows without text are left out
     * wherever nulls sort. */
    uint8_t lo[16], hi[16];
    cdb_value_writer w;
    size_t lo_len, hi_len;
    cdb_value_writer_init(&w, lo, sizeof lo, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, 1);
    cdb_value_end_array(&w);
    lo_len = cdb_value_writer_len(&w);
    cdb_value_writer_init(&w, hi, sizeof hi, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, INT64_MAX);
    cdb_value_end_array(&w);
    hi_len = cdb_value_writer_len(&w);
    return cdb_table_scan(&g->db, c->store, GF_CATALOG_SHELF_INDEX, lo, lo_len, hi, hi_len, cur, entries, max, n);
}

/* ------------------------------------------------------------ text */

void gf_utf8_clean(char *s)
{
    unsigned char *p = (unsigned char *)s;
    while (*p) {
        unsigned n, i;
        unsigned char lo = 0x80, hi = 0xBF;
        if (*p < 0x80) {
            p++;
            continue;
        }
        if (*p >= 0xC2 && *p <= 0xDF)
            n = 1;
        else if (*p >= 0xE0 && *p <= 0xEF) {
            n = 2;
            if (*p == 0xE0)
                lo = 0xA0;
            if (*p == 0xED)
                hi = 0x9F;
        } else if (*p >= 0xF0 && *p <= 0xF4) {
            n = 3;
            if (*p == 0xF0)
                lo = 0x90;
            if (*p == 0xF4)
                hi = 0x8F;
        } else {
            *p++ = '?';
            continue;
        }
        for (i = 1; i <= n; i++) {
            unsigned char b = p[i];
            if (b < (i == 1 ? lo : 0x80) || b > (i == 1 ? hi : 0xBF))
                break;
        }
        if (i <= n) {
            *p++ = '?';
            continue;
        }
        p += n + 1;
    }
}

/* Appends src to dst (cap bytes), stopping at a character boundary. */
static void append(char *dst, size_t cap, const char *src, size_t n)
{
    size_t len = strlen(dst);
    if (len + 1 >= cap)
        return;
    if (n > cap - 1 - len) {
        n = cap - 1 - len;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst + len, src, n);
    dst[len + n] = '\0';
}

static size_t trim_right(const char *s, size_t n)
{
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return n;
}

/* The sort title: lowercase ASCII, no leading quote or article, at most
 * GF_SORT_MAX bytes, cut at a character boundary. */
static void sort_title(const char *title, char *out)
{
    static const char *const articles[] = { "the ", "a ", "an " };
    char low[GF_SORT_MAX * 2 + 1];
    const char *p = title;
    size_t i, n;

    while (*p == '"' || *p == '\'' || *p == '_' || *p == '[' || *p == '(')
        p++;
    for (i = 0; p[i] != '\0' && i < sizeof low - 1; i++)
        low[i] = (char)tolower((unsigned char)p[i]);
    low[i] = '\0';
    p = low;
    for (i = 0; i < 3; i++) {
        n = strlen(articles[i]);
        if (strncmp(p, articles[i], n) == 0 && p[n] != '\0') {
            p += n;
            break;
        }
    }
    out[0] = '\0';
    append(out, GF_SORT_MAX + 1, p, strlen(p));
}

/* ------------------------------------------------------------ parsing
 *
 * GUTINDEX.ALL: after the first "TITLE and AUTHOR" heading, an entry starts
 * with "Title, by Author<two or more spaces>EBOOKNO" (a trailing C marks a
 * copyrighted book). Unbracketed lines straight after continue the title
 * line; bracketed tags ("[Subtitle: ...]", "[Language: ...]") may span
 * lines. A blank line ends the entry. */

typedef struct gentry {
    int open;
    int tags_seen;
    int in_tag;
    int64_t ebook;
    char head[1024];
    char tag[1024];
    char subtitle[512];
    char language[64];
    char credits[768];
} gentry;

/* Some entries pad with no-break spaces (U+00A0), in names and before the
 * ebook number; makes them plain spaces, in place. */
static void plain_spaces(char *s)
{
    char *w = s;
    for (; *s != '\0'; s++) {
        if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) {
            *w++ = ' ';
            s++;
        } else {
            *w++ = *s;
        }
    }
    *w = '\0';
}

static int entry_start(const char *line, size_t len, int64_t *ebook, size_t *head_len)
{
    size_t e = len, d;
    if (len == 0 || isspace((unsigned char)line[0]) || line[0] == '[' || line[0] == '~')
        return 0;
    if (line[e - 1] == 'C')
        e--;
    d = e;
    while (d > 0 && isdigit((unsigned char)line[d - 1]))
        d--;
    if (d == e || e - d > 9 || d < 3 || line[d - 1] != ' ' || line[d - 2] != ' ')
        return 0;
    *ebook = strtoll(line + d, NULL, 10);
    *head_len = trim_right(line, d);
    return *head_len > 0;
}

static void close_tag(gentry *en)
{
    char *t = en->tag, *colon;
    size_t n = trim_right(t, strlen(t));
    if (n > 0 && t[n - 1] == ']')
        n--;
    t[trim_right(t, n)] = '\0';
    en->in_tag = 0;
    colon = strchr(t, ':');
    if (colon == NULL)
        return;
    *colon = '\0';
    colon++;
    while (*colon == ' ')
        colon++;
    if (strcmp(t, "Subtitle") == 0) {
        append(en->subtitle, sizeof en->subtitle, colon, strlen(colon));
    } else if (strcmp(t, "Language") == 0) {
        en->language[0] = '\0';
        append(en->language, sizeof en->language, colon, strlen(colon));
    } else {
        if (en->credits[0] != '\0')
            append(en->credits, sizeof en->credits, "; ", 2);
        append(en->credits, sizeof en->credits, t, strlen(t));
        append(en->credits, sizeof en->credits, ": ", 2);
        append(en->credits, sizeof en->credits, colon, strlen(colon));
    }
}

/* ------------------------------------------------------------ rows */

typedef struct colval {
    uint64_t id;
    const char *s;  /* NULL: the int */
    int64_t i;
} colval;

static int by_id(const void *a, const void *b)
{
    uint64_t x = ((const colval *)a)->id, y = ((const colval *)b)->id;
    return x < y ? -1 : x > y;
}

static cdb_status encode_row(const gf_catalog *c, const gentry *en, uint8_t *buf, size_t cap, size_t *len)
{
    char title[1024], author[512], sort[GF_SORT_MAX + 1];
    const char *by;
    const char *head = en->head;
    colval v[GF_NCOLS];
    unsigned n = 0, i;
    cdb_value_writer w;

    /* "Title, by Author": the last ", by " splits them. */
    by = NULL;
    for (const char *p = strstr(head, ", by "); p != NULL; p = strstr(p + 1, ", by "))
        by = p;
    title[0] = author[0] = '\0';
    if (by != NULL) {
        append(title, sizeof title, head, (size_t)(by - head));
        append(author, sizeof author, by + 5, strlen(by + 5));
    } else {
        append(title, sizeof title, head, strlen(head));
    }
    sort_title(title, sort);

    v[n].id = c->field[GF_COL_EBOOK], v[n].s = NULL, v[n++].i = en->ebook;
    v[n].id = c->field[GF_COL_TITLE], v[n++].s = title;
    v[n].id = c->field[GF_COL_LANGUAGE], v[n++].s = en->language[0] ? en->language : "English";
    v[n].id = c->field[GF_COL_SORT], v[n++].s = sort;
    if (author[0])
        v[n].id = c->field[GF_COL_AUTHOR], v[n++].s = author;
    if (en->subtitle[0])
        v[n].id = c->field[GF_COL_SUBTITLE], v[n++].s = en->subtitle;
    if (en->credits[0])
        v[n].id = c->field[GF_COL_CREDITS], v[n++].s = en->credits;
    qsort(v, n, sizeof v[0], by_id);

    cdb_value_writer_init(&w, buf, cap, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 0; i < n; i++) {
        cdb_value_field(&w, v[i].id);
        if (v[i].s == NULL)
            cdb_value_write_int(&w, v[i].i);
        else
            cdb_value_write_string(&w, v[i].s, strlen(v[i].s));
    }
    cdb_value_end_object(&w);
    if (w.error != CDB_OK)
        return w.error;
    *len = cdb_value_writer_len(&w);
    return CDB_OK;
}

/* ------------------------------------------------------------ ingest */

typedef struct ingest {
    gf_db *g;
    const gf_catalog *c;
    gf_ingest_stats *st;
    gf_progress_fn progress;
    void *ctx;
    unsigned in_batch;
    double batch_t0;
    uint8_t row[8192];
} ingest;

static cdb_status commit_batch(ingest *in, int reopen)
{
    cdb_status s = cdb_tx_commit(&in->g->db);
    double t;
    if (s != CDB_OK)
        return s;
    t = gf_now_ms();
    in->st->batches++;
    in->st->last_batch_ms = t - in->batch_t0;
    if (in->st->last_batch_ms > in->st->worst_batch_ms)
        in->st->worst_batch_ms = in->st->last_batch_ms;
    in->in_batch = 0;
    if (in->progress != NULL)
        in->progress(in->st, in->ctx);
    if (!reopen)
        return CDB_OK;
    in->batch_t0 = gf_now_ms();
    return cdb_tx_begin(&in->g->db, CDB_DURABILITY_DEFAULT);
}

static cdb_status flush_entry(ingest *in, gentry *en)
{
    cdb_status s;
    size_t len;
    uint64_t row_entry;

    if (en->in_tag)
        close_tag(en);
    if (!en->open)
        return CDB_OK;
    en->open = 0;
    in->st->entries++;
    gf_utf8_clean(en->head);
    gf_utf8_clean(en->subtitle);
    gf_utf8_clean(en->language);
    gf_utf8_clean(en->credits);
    if ((s = encode_row(in->c, en, in->row, sizeof in->row, &len)) != CDB_OK) {
        in->st->refused++;
        return CDB_OK;
    }
    s = cdb_table_insert(&in->g->db, in->c->store, in->row, len, &row_entry);
    if (s == CDB_E_EXISTS)
        in->st->duplicates++;
    else if (s == CDB_E_SCHEMA || s == CDB_E_OVERFLOW)
        in->st->refused++;
    else if (s != CDB_OK)
        return s;
    else
        in->st->inserted++;
    if (++in->in_batch == in->st->batch_rows)
        return commit_batch(in, 1);
    return CDB_OK;
}

cdb_status gf_catalog_ingest(gf_db *g, const gf_catalog *c, const char *path, unsigned batch,
                             gf_ingest_stats *st, gf_progress_fn progress, void *ctx)
{
    static ingest in;
    static gentry en;
    char line[4096];
    FILE *f = fopen(path, "r");
    int started = 0;
    cdb_status s = CDB_OK;
    double t0 = gf_now_ms(), engine_ms = 0;

    if (f == NULL)
        return CDB_E_NOT_FOUND;
    memset(st, 0, sizeof *st);
    st->batch_rows = batch > 0 ? batch : GF_INGEST_BATCH;
    memset(&in, 0, sizeof in);
    memset(&en, 0, sizeof en);
    in.g = g, in.c = c, in.st = st, in.progress = progress, in.ctx = ctx;
    in.batch_t0 = t0;
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK) {
        fclose(f);
        return s;
    }

    while (s == CDB_OK && fgets(line, sizeof line, f) != NULL) {
        size_t len, head_len;
        const char *p = line;
        int64_t ebook;
        double te;

        plain_spaces(line);
        len = trim_right(line, strlen(line));
        if (!started) {
            started = strncmp(line, "TITLE and AUTHOR", 16) == 0;
            continue;
        }
        if (strncmp(line, "TITLE and AUTHOR", 16) == 0)
            continue;
        line[len] = '\0';
        if (len == 0 || entry_start(line, len, &ebook, &head_len)) {
            te = gf_now_ms();
            s = flush_entry(&in, &en);
            engine_ms += gf_now_ms() - te;
            if (len == 0 || s != CDB_OK)
                continue;
            memset(&en, 0, sizeof en);
            en.open = 1;
            en.ebook = ebook;
            append(en.head, sizeof en.head, line, head_len);
            continue;
        }
        if (!en.open)
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        if (en.in_tag) {
            append(en.tag, sizeof en.tag, " ", 1);
            append(en.tag, sizeof en.tag, p, strlen(p));
        } else if (*p == '[') {
            en.in_tag = 1;
            en.tags_seen = 1;
            en.tag[0] = '\0';
            append(en.tag, sizeof en.tag, p + 1, strlen(p + 1));
        } else if (!en.tags_seen) {
            append(en.head, sizeof en.head, " ", 1);
            append(en.head, sizeof en.head, p, strlen(p));
        }
        if (en.in_tag && strchr(p, ']') != NULL)
            close_tag(&en);
    }
    fclose(f);
    if (s == CDB_OK) {
        double te = gf_now_ms();
        s = flush_entry(&in, &en);
        engine_ms += gf_now_ms() - te;
    }
    if (s == CDB_OK)
        s = commit_batch(&in, 0);
    else
        cdb_tx_abort(&g->db);
    st->ms = gf_now_ms() - t0;
    st->parse_ms = st->ms - engine_ms;
    return s;
}

/* ------------------------------------------------------------ reads */

static void get_str(const cdb_value *obj, uint64_t id, char *out, size_t cap)
{
    cdb_value v;
    int found;
    out[0] = '\0';
    if (cdb_value_object_get(obj, GF_ORDER, id, &v, &found) == CDB_OK && found && v.type == CDB_TYPE_STRING)
        append(out, cap, (const char *)v.payload, v.payload_len);
}

cdb_status gf_catalog_read(gf_db *g, const gf_catalog *c, uint64_t entry, gf_book *b)
{
    static uint8_t buf[32768];
    size_t len;
    cdb_value obj, v;
    int found;
    cdb_status s = cdb_table_get(&g->db, c->store, entry, buf, sizeof buf, &len);

    memset(b, 0, sizeof *b);
    if (s != CDB_OK)
        return s;
    if ((s = cdb_value_read(buf, len, GF_ORDER, &obj)) != CDB_OK)
        return s;
    if (cdb_value_object_get(&obj, GF_ORDER, c->field[GF_COL_EBOOK], &v, &found) == CDB_OK && found)
        b->ebook = v.as.i64;
    get_str(&obj, c->field[GF_COL_TITLE], b->title, sizeof b->title);
    get_str(&obj, c->field[GF_COL_AUTHOR], b->author, sizeof b->author);
    get_str(&obj, c->field[GF_COL_SUBTITLE], b->subtitle, sizeof b->subtitle);
    get_str(&obj, c->field[GF_COL_LANGUAGE], b->language, sizeof b->language);
    get_str(&obj, c->field[GF_COL_CREDITS], b->credits, sizeof b->credits);
    if (cdb_value_object_get(&obj, GF_ORDER, c->first_field, &v, &found) == CDB_OK && found
        && v.type == CDB_TYPE_INT)
        b->first_passage = (uint64_t)v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, c->passages_field, &v, &found) == CDB_OK && found
        && v.type == CDB_TYPE_INT)
        b->passages = (uint64_t)v.as.i64;
    get_str(&obj, c->bib[GF_BIB_TYPE], b->type, sizeof b->type);
    get_str(&obj, c->bib[GF_BIB_ISSUED], b->issued, sizeof b->issued);
    get_str(&obj, c->bib[GF_BIB_AUTHORS], b->authors, sizeof b->authors);
    get_str(&obj, c->bib[GF_BIB_SUBJECTS], b->subjects, sizeof b->subjects);
    get_str(&obj, c->bib[GF_BIB_LOCC], b->locc, sizeof b->locc);
    get_str(&obj, c->bib[GF_BIB_SHELVES], b->shelves, sizeof b->shelves);
    get_str(&obj, c->bib[GF_BIB_GENRE], b->genre, sizeof b->genre);
    return CDB_OK;
}

cdb_status gf_catalog_find(gf_db *g, const gf_catalog *c, int64_t ebook, uint64_t *entry, int *found)
{
    uint8_t key[16];
    cdb_value_writer w;
    cdb_value_writer_init(&w, key, sizeof key, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, ebook);
    cdb_value_end_array(&w);
    if (w.error != CDB_OK)
        return w.error;
    return cdb_table_find(&g->db, c->store, key, cdb_value_writer_len(&w), entry, found);
}

cdb_status gf_catalog_count(gf_db *g, const gf_catalog *c, uint64_t *count)
{
    static uint64_t entries[4096];
    cdb_table_cursor cur;
    size_t n;
    cdb_status s;
    memset(&cur, 0, sizeof cur);
    *count = 0;
    do {
        s = cdb_table_scan(&g->db, c->store, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, &cur, entries, 4096, &n);
        *count += n;
    } while (s == CDB_OK && n > 0);
    return s;
}

/* ------------------------------------------------------------ pg_catalog.csv */

#define CSV_FIELDS 9 /* Text#,Type,Issued,Title,Language,Authors,Subjects,LoCC,Bookshelves */

/* Reads one CSV record from *p (quotes, doubled quotes and line ends inside
 * quotes allowed) into fields of cap bytes each. Returns 0 at the end. */
static int csv_record(const char **p, const char *end, char fields[CSV_FIELDS][1024])
{
    const char *s = *p;
    unsigned f = 0;
    size_t len = 0;
    int quoted = 0;

    if (s >= end)
        return 0;
    for (f = 0; f < CSV_FIELDS; f++)
        fields[f][0] = '\0';
    f = 0;
    while (s < end) {
        char ch = *s++;
        if (quoted) {
            if (ch == '"' && s < end && *s == '"')
                s++;
            else if (ch == '"') {
                quoted = 0;
                continue;
            }
        } else if (ch == '"') {
            quoted = 1;
            continue;
        } else if (ch == ',') {
            f++;
            len = 0;
            continue;
        } else if (ch == '\n') {
            break;
        } else if (ch == '\r') {
            continue;
        }
        if (f < CSV_FIELDS && len + 1 < 1024) {
            fields[f][len++] = ch;
            fields[f][len] = '\0';
        }
    }
    *p = s;
    return 1;
}

/* The first item of a "; "-separated list. */
static void first_item(const char *list, char *out, size_t cap)
{
    const char *semi = strstr(list, "; ");
    out[0] = '\0';
    append(out, cap, list, semi ? (size_t)(semi - list) : strlen(list));
}

static void genre_of(const char *shelves, char *out, size_t cap)
{
    const char *cat = strstr(shelves, "Category: ");
    out[0] = '\0';
    if (cat != NULL)
        first_item(cat + 10, out, cap);
    else
        first_item(shelves, out, cap);
}

static const char *language_name(const char *code)
{
    static const char *const map[][2] = {
        { "en", "English" }, { "fr", "French" },  { "de", "German" },     { "fi", "Finnish" },
        { "nl", "Dutch" },   { "it", "Italian" }, { "es", "Spanish" },    { "pt", "Portuguese" },
        { "zh", "Chinese" }, { "la", "Latin" },   { "sv", "Swedish" },    { "da", "Danish" },
        { "el", "Greek" },   { "hu", "Hungarian" }, { "eo", "Esperanto" }, { "pl", "Polish" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(code, map[i][0]) == 0)
            return map[i][1];
    return code;
}

static cdb_status merge_commit(gf_db *g, gf_ingest_stats *st, unsigned *in_batch, double *t0, gf_progress_fn progress,
                               void *ctx, int reopen)
{
    cdb_status s = cdb_tx_commit(&g->db);
    if (s != CDB_OK)
        return s;
    st->batches++;
    st->last_batch_ms = gf_now_ms() - *t0;
    if (st->last_batch_ms > st->worst_batch_ms)
        st->worst_batch_ms = st->last_batch_ms;
    *in_batch = 0;
    if (progress != NULL)
        progress(st, ctx);
    *t0 = gf_now_ms();
    return reopen ? cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT) : CDB_OK;
}

cdb_status gf_catalog_merge_csv(gf_db *g, const gf_catalog *c, const char *path, gf_ingest_stats *st,
                                gf_progress_fn progress, void *ctx)
{
    static char fields[CSV_FIELDS][1024];
    static uint8_t row[16384];
    char *data;
    const char *p, *end;
    FILE *f = fopen(path, "rb");
    long size;
    unsigned in_batch = 0;
    double t0 = gf_now_ms(), tb;
    cdb_status s;

    memset(st, 0, sizeof *st);
    st->batch_rows = GF_INGEST_BATCH;
    if (f == NULL)
        return CDB_E_NOT_FOUND;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0) {
        fclose(f);
        return CDB_E_IO;
    }
    rewind(f);
    if ((data = malloc((size_t)size)) == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return CDB_E_IO;
    }
    fclose(f);
    p = data, end = data + size;
    csv_record(&p, end, fields); /* the header */
    tb = gf_now_ms();
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK) {
        free(data);
        return s;
    }

    while (s == CDB_OK && csv_record(&p, end, fields)) {
        char genre[128], asort[GF_SORT_MAX + 1], locc[GF_LOCC_MAX + 1], title[1024], sub[1024], sort[GF_SORT_MAX + 1];
        colval v[GF_NCOLS + GF_NBIB];
        unsigned n = 0, i;
        int64_t ebook = strtoll(fields[0], NULL, 10);
        uint64_t entry;
        int found;
        cdb_value_writer w;
        char *nl;

        if (ebook <= 0)
            continue;
        st->entries++;
        for (i = 0; i < CSV_FIELDS; i++)
            gf_utf8_clean(fields[i]);
        genre_of(fields[8], genre, sizeof genre);
        first_item(fields[5], asort, sizeof asort);
        for (i = 0; asort[i] != '\0'; i++)
            asort[i] = (char)tolower((unsigned char)asort[i]);
        locc[0] = '\0';
        append(locc, sizeof locc, fields[7], strlen(fields[7]));

#define BIB(col, str)                                                                                                  \
    do {                                                                                                               \
        if ((str)[0] != '\0')                                                                                          \
            v[n].id = c->bib[col], v[n].s = (str), n++;                                                                \
    } while (0)
        BIB(GF_BIB_TYPE, fields[1]);
        BIB(GF_BIB_ISSUED, fields[2]);
        BIB(GF_BIB_AUTHORS, fields[5]);
        BIB(GF_BIB_SUBJECTS, fields[6]);
        BIB(GF_BIB_LOCC, locc);
        BIB(GF_BIB_SHELVES, fields[8]);
        BIB(GF_BIB_GENRE, genre);
        BIB(GF_BIB_AUTHOR_SORT, asort);
#undef BIB

        if ((s = gf_catalog_find(g, c, ebook, &entry, &found)) != CDB_OK)
            break;
        if (!found) { /* not in GUTINDEX: a whole row from the CSV */
            title[0] = sub[0] = '\0';
            append(title, sizeof title, fields[3], strlen(fields[3]));
            if ((nl = strchr(title, '\n')) != NULL) {
                *nl = '\0';
                append(sub, sizeof sub, nl + 1, strlen(nl + 1));
                for (char *q = sub; *q; q++)
                    if (*q == '\n')
                        *q = ' ';
            }
            sort_title(title, sort);
            v[n].id = c->field[GF_COL_EBOOK], v[n].s = NULL, v[n].i = ebook, n++;
            v[n].id = c->field[GF_COL_TITLE], v[n].s = title[0] ? title : "(untitled)", n++;
            v[n].id = c->field[GF_COL_LANGUAGE], v[n].s = language_name(fields[4]), n++;
            v[n].id = c->field[GF_COL_SORT], v[n].s = sort, n++;
            if (fields[5][0])
                v[n].id = c->field[GF_COL_AUTHOR], v[n].s = fields[5], n++;
            if (sub[0])
                v[n].id = c->field[GF_COL_SUBTITLE], v[n].s = sub, n++;
        }
        qsort(v, n, sizeof v[0], by_id);
        cdb_value_writer_init(&w, row, sizeof row, GF_ORDER, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&w);
        for (i = 0; i < n; i++) {
            cdb_value_field(&w, v[i].id);
            if (v[i].s == NULL)
                cdb_value_write_int(&w, v[i].i);
            else
                cdb_value_write_string(&w, v[i].s, strlen(v[i].s));
        }
        cdb_value_end_object(&w);
        if (w.error != CDB_OK) {
            st->refused++;
            continue;
        }
        if (found)
            s = cdb_table_update(&g->db, c->store, entry, row, cdb_value_writer_len(&w));
        else
            s = cdb_table_insert(&g->db, c->store, row, cdb_value_writer_len(&w), &entry);
        if (s == CDB_E_SCHEMA || s == CDB_E_OVERFLOW || s == CDB_E_EXISTS) {
            st->refused++;
            s = CDB_OK;
            continue;
        }
        if (s != CDB_OK)
            break;
        if (found)
            st->updated++;
        else
            st->inserted++;
        if (++in_batch == GF_INGEST_BATCH)
            s = merge_commit(g, st, &in_batch, &tb, progress, ctx, 1);
    }
    free(data);
    if (s == CDB_OK)
        s = merge_commit(g, st, &in_batch, &tb, progress, ctx, 0);
    else
        cdb_tx_abort(&g->db);
    st->ms = gf_now_ms() - t0;
    return s;
}

/* ------------------------------------------------------------ filter */

/* Folds case, and '-' to ' ' ("Science-Fiction" is "science fiction"). */
static int fold(char ch)
{
    return ch == '-' ? ' ' : tolower((unsigned char)ch);
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n == 0)
        return 1;
    for (; *hay != '\0'; hay++) {
        for (i = 0; i < n && hay[i] != '\0' && fold(hay[i]) == fold(needle[i]); i++)
            ;
        if (i == n)
            return 1;
    }
    return 0;
}

/* Some class in a "; "-separated LoCC list starts with prefix. */
static int locc_prefix(const char *locc, const char *prefix)
{
    size_t n = strlen(prefix);
    const char *p = locc;
    while (*p != '\0') {
        if (strncasecmp(p, prefix, n) == 0)
            return 1;
        if ((p = strstr(p, "; ")) == NULL)
            return 0;
        p += 2;
    }
    return 0;
}

int gf_filter_empty(const gf_filter *f)
{
    return !f->title[0] && !f->author[0] && !f->genre[0] && !f->subject[0] && !f->language[0] && !f->locc[0]
           && !f->type[0] && !f->from[0] && !f->to[0] && !f->on_shelf;
}

static int matches(const gf_filter *f, const gf_book *b)
{
    if (f->on_shelf && b->passages == 0)
        return 0;
    if (f->title[0] && !contains_ci(b->title, f->title) && !contains_ci(b->subtitle, f->title))
        return 0;
    if (f->author[0] && !contains_ci(b->author, f->author) && !contains_ci(b->authors, f->author))
        return 0;
    if (f->genre[0] && !contains_ci(b->shelves, f->genre) && !contains_ci(b->genre, f->genre))
        return 0;
    if (f->subject[0] && !contains_ci(b->subjects, f->subject))
        return 0;
    if (f->language[0] && !contains_ci(b->language, f->language))
        return 0;
    if (f->locc[0] && !locc_prefix(b->locc, f->locc))
        return 0;
    if (f->type[0] && strcasecmp(b->type, f->type) != 0)
        return 0;
    if (f->from[0] && (!b->issued[0] || strncmp(b->issued, f->from, strlen(f->from)) < 0))
        return 0;
    if (f->to[0] && (!b->issued[0] || strncmp(b->issued, f->to, strlen(f->to)) > 0))
        return 0;
    return 1;
}

cdb_status gf_catalog_filter(gf_db *g, const gf_catalog *c, uint32_t index, const gf_filter *f, uint64_t *out,
                             size_t cap, size_t *n, uint64_t *scanned)
{
    static uint64_t batch[512];
    static gf_book b;
    cdb_table_cursor cur;
    size_t got, i;
    int all = gf_filter_empty(f);
    cdb_status s;

    memset(&cur, 0, sizeof cur);
    *n = 0;
    *scanned = 0;
    do {
        s = cdb_table_scan(&g->db, c->store, index, NULL, 0, NULL, 0, &cur, batch, 512, &got);
        for (i = 0; s == CDB_OK && i < got && *n < cap; i++) {
            (*scanned)++;
            if (all || (gf_catalog_read(g, c, batch[i], &b) == CDB_OK && matches(f, &b)))
                out[(*n)++] = batch[i];
        }
    } while (s == CDB_OK && got > 0 && *n < cap);
    return s;
}
