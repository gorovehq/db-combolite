#define _POSIX_C_SOURCE 200809L
#include "gf_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <strings.h>
#include <time.h>

#include "cdb_link.h"

static const char *const col_names[GF_NCOLS] = {
    "ebook", "title", "author", "subtitle", "language", "credits", "sort_title",
};

static const char *const bib_names[GF_NBIB] = {
    "media_type", "issued", "authors", "subjects", "locc", "bookshelves", "genre", "author_sort",
};

static const char *const lib_names[GF_NLIB] = {
    "ebook", "title", "author", "language", "passages", "words",
    "first_passage", "last_passage", "ingested", "embedded", "embed_model",
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

static cdb_status open_library(gf_db *g, gf_catalog *c);

cdb_status gf_catalog_open(gf_db *g, gf_catalog *c)
{
    cdb_status s;
    int found;
    unsigned i;

    memset(c, 0, sizeof *c);
    if ((s = cdb_db_find_store(&g->db, "catalog", 7, &c->store, &found)) != CDB_OK)
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
        if ((s = cdb_table_create(&g->db, "catalog", 7, &def, &c->store)) != CDB_OK)
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

    if ((s = open_bib(g, c)) != CDB_OK)
        return s;
    return open_library(g, c);
}

/* A link type, defined the first time. */
static cdb_status link_type(gf_db *g, const char *name, unsigned card, unsigned on_delete, uint64_t *type)
{
    int found;
    cdb_status s = cdb_link_find_type(&g->db, name, strlen(name), type, &found);
    if (s != CDB_OK || found)
        return s;
    return cdb_link_define_type(&g->db, name, strlen(name), card, 1, on_delete, type);
}

/* The library table and the links from it. */
static cdb_status open_library(gf_db *g, gf_catalog *c)
{
    cdb_status s;
    int found;
    unsigned i;

    if ((s = cdb_db_find_store(&g->db, "library", 7, &c->lib_store, &found)) != CDB_OK)
        return s;
    if (!found) {
        cdb_table_column cols[GF_NLIB];
        static const unsigned pk[1] = { GF_LIB_EBOOK };
        cdb_table_def def;
        memset(cols, 0, sizeof cols);
        for (i = 0; i < GF_NLIB; i++) {
            cols[i].name = lib_names[i];
            cols[i].name_len = strlen(lib_names[i]);
            cols[i].type = CDB_TYPE_STRING;
        }
        cols[GF_LIB_EBOOK].type = CDB_TYPE_INT;
        cols[GF_LIB_PASSAGES].type = CDB_TYPE_INT;
        cols[GF_LIB_WORDS].type = CDB_TYPE_INT;
        cols[GF_LIB_TITLE].not_null = 1;
        memset(&def, 0, sizeof def);
        def.columns = cols;
        def.ncolumns = GF_NLIB;
        def.primary_key = pk;
        def.npk = 1;
        def.keep = 1;
        if ((s = cdb_table_create(&g->db, "library", 7, &def, &c->lib_store)) != CDB_OK)
            return s;
    }
    for (i = 0; i < GF_NLIB; i++)
        if ((s = cdb_table_column_id(&g->db, c->lib_store, lib_names[i], strlen(lib_names[i]), &c->lib[i]))
            != CDB_OK)
            return s;
    /* A library row is its catalog row's text; removing it removes its
     * passages' membership, not the passages (the reader keeps working) */
    if ((s = link_type(g, "in_catalog", CDB_CARD_1_1, CDB_LINK_ON_DELETE_TOMBSTONE, &c->in_catalog)) != CDB_OK)
        return s;
    return link_type(g, "has_passage", CDB_CARD_1_N, CDB_LINK_ON_DELETE_TOMBSTONE, &c->has_passage);
}

void gf_iso_now(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

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

/* Encodes the columns as an object, in field-ID order (spec 14.6.5). */
static cdb_status encode_cols(colval *v, size_t n, uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    size_t i;
    qsort(v, n, sizeof v[0], by_id);
    cdb_value_writer_init(&w, out, cap, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 0; i < n; i++) {
        cdb_value_field(&w, v[i].id);
        if (v[i].s == NULL)
            cdb_value_write_int(&w, v[i].i);
        else
            cdb_value_write_string(&w, v[i].s, strlen(v[i].s));
    }
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

cdb_status gf_library_add(gf_db *g, const gf_catalog *c, uint64_t row, const gf_book *b, uint64_t passages,
                          uint64_t words, const char *first_key, const char *last_key, uint64_t *lib_row)
{
    static uint8_t buf[4096];
    char now[32];
    colval v[GF_NLIB];
    size_t n = 0, len;
    cdb_status s;

    gf_iso_now(now, sizeof now);
    v[n].id = c->lib[GF_LIB_EBOOK], v[n].s = NULL, v[n].i = b->ebook, n++;
    v[n].id = c->lib[GF_LIB_TITLE], v[n].s = b->title, n++;
    if (b->author[0])
        v[n].id = c->lib[GF_LIB_AUTHOR], v[n].s = b->author, n++;
    if (b->language[0])
        v[n].id = c->lib[GF_LIB_LANGUAGE], v[n].s = b->language, n++;
    v[n].id = c->lib[GF_LIB_PASSAGES], v[n].s = NULL, v[n].i = (int64_t)passages, n++;
    v[n].id = c->lib[GF_LIB_WORDS], v[n].s = NULL, v[n].i = (int64_t)words, n++;
    v[n].id = c->lib[GF_LIB_FIRST], v[n].s = first_key, n++;
    v[n].id = c->lib[GF_LIB_LAST], v[n].s = last_key, n++;
    v[n].id = c->lib[GF_LIB_INGESTED], v[n].s = now, n++;
    if ((s = encode_cols(v, n, buf, sizeof buf, &len)) != CDB_OK)
        return s;
    if ((s = cdb_table_insert(&g->db, c->lib_store, buf, len, lib_row)) != CDB_OK)
        return s;
    return cdb_link_add(&g->db, c->in_catalog, cdb_ref_entry(c->lib_store, *lib_row), cdb_ref_entry(c->store, row),
                        1.0f, 0);
}

cdb_status gf_library_set_embedded(gf_db *g, const gf_catalog *c, uint64_t lib_row, const char *model)
{
    uint8_t buf[256];
    char now[32];
    colval v[2];
    size_t len;
    cdb_status s;

    gf_iso_now(now, sizeof now);
    v[0].id = c->lib[GF_LIB_EMBEDDED], v[0].s = now;
    v[1].id = c->lib[GF_LIB_MODEL], v[1].s = model;
    if ((s = encode_cols(v, 2, buf, sizeof buf, &len)) != CDB_OK)
        return s;
    return cdb_table_update(&g->db, c->lib_store, lib_row, buf, len);
}

cdb_status gf_catalog_shelf(gf_db *g, const gf_catalog *c, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                            size_t *n)
{
    uint64_t lib[64];
    size_t got = 0, i;
    cdb_status s;

    *n = 0;
    while (*n < max) {
        size_t want = max - *n < 64 ? max - *n : 64;
        if ((s = cdb_table_scan(&g->db, c->lib_store, CDB_TABLE_NO_INDEX, NULL, 0, NULL, 0, cur, lib, want, &got))
                != CDB_OK || got == 0)
            return s;
        for (i = 0; i < got; i++) {
            cdb_link out;
            size_t k = 0;
            if ((s = cdb_link_list(&g->db, cdb_ref_entry(c->lib_store, lib[i]), CDB_LINK_OUT, c->in_catalog, NULL,
                                   &out, 1, &k)) != CDB_OK)
                return s;
            if (k == 1)
                entries[(*n)++] = out.target.entry;
        }
    }
    return CDB_OK;
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

/* A column value that clears the column: written as null */
static const char null_value[] = "";

/* A title with a '?' right before a letter or digit: GUTINDEX's stand-in
 * for a character it could not represent, never a question mark. */
static int garbled(const char *t)
{
    for (const char *q = strchr(t, '?'); q != NULL; q = strchr(q + 1, '?'))
        if (isalnum((unsigned char)q[1]))
            return 1;
    return 0;
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
    cdb_status s = gf_db_commit(in->g);
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
    else if (s == CDB_E_SCHEMA || s == CDB_E_OVERFLOW) {
        if (in->st->refused++ == 0) {
            in->st->first_refused = s;
            in->st->first_refused_ebook = en->ebook;
        }
    } else if (s != CDB_OK)
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

static cdb_status read_library(gf_db *g, const gf_catalog *c, gf_book *b);

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
    get_str(&obj, c->bib[GF_BIB_TYPE], b->type, sizeof b->type);
    get_str(&obj, c->bib[GF_BIB_ISSUED], b->issued, sizeof b->issued);
    get_str(&obj, c->bib[GF_BIB_AUTHORS], b->authors, sizeof b->authors);
    get_str(&obj, c->bib[GF_BIB_SUBJECTS], b->subjects, sizeof b->subjects);
    get_str(&obj, c->bib[GF_BIB_LOCC], b->locc, sizeof b->locc);
    get_str(&obj, c->bib[GF_BIB_SHELVES], b->shelves, sizeof b->shelves);
    get_str(&obj, c->bib[GF_BIB_GENRE], b->genre, sizeof b->genre);
    return read_library(g, c, b);
}

/* The library's part of a book, when its text is stored: the row with the
 * same ebook number, and its first passage by has_passage (links list in
 * key order, so the lowest entry, which is the first stored). */
static cdb_status read_library(gf_db *g, const gf_catalog *c, gf_book *b)
{
    static uint8_t buf[4096];
    uint8_t key[16];
    cdb_value_writer w;
    cdb_value obj, v;
    cdb_link first;
    size_t len, k = 0;
    int found;
    cdb_status s;

    cdb_value_writer_init(&w, key, sizeof key, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, b->ebook);
    cdb_value_end_array(&w);
    if ((s = cdb_table_find(&g->db, c->lib_store, key, cdb_value_writer_len(&w), &b->lib_row, &found)) != CDB_OK
        || !found) {
        b->lib_row = 0;
        return s;
    }
    if ((s = cdb_table_get(&g->db, c->lib_store, b->lib_row, buf, sizeof buf, &len)) != CDB_OK
        || (s = cdb_value_read(buf, len, GF_ORDER, &obj)) != CDB_OK)
        return s;
    if (cdb_value_object_get(&obj, GF_ORDER, c->lib[GF_LIB_PASSAGES], &v, &found) == CDB_OK && found
        && v.type == CDB_TYPE_INT)
        b->passages = (uint64_t)v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, c->lib[GF_LIB_WORDS], &v, &found) == CDB_OK && found
        && v.type == CDB_TYPE_INT)
        b->words = (uint64_t)v.as.i64;
    get_str(&obj, c->lib[GF_LIB_FIRST], b->first_key, sizeof b->first_key);
    get_str(&obj, c->lib[GF_LIB_LAST], b->last_key, sizeof b->last_key);
    get_str(&obj, c->lib[GF_LIB_INGESTED], b->ingested, sizeof b->ingested);
    get_str(&obj, c->lib[GF_LIB_EMBEDDED], b->embedded, sizeof b->embedded);
    if ((s = cdb_link_list(&g->db, cdb_ref_entry(c->lib_store, b->lib_row), CDB_LINK_OUT, c->has_passage, NULL,
                           &first, 1, &k)) != CDB_OK)
        return s;
    if (k == 1)
        b->first_passage = first.target.entry;
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
    cdb_status s = gf_db_commit(g);
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
        /* The CSV's title: its first line, the rest a subtitle */
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
        if (found) {
            static gf_book old;
            /* GUTINDEX names a language only when it is not English, so a
             * French book it does not mark reads as English: the CSV's
             * language is the one to keep */
            if (fields[4][0])
                v[n].id = c->field[GF_COL_LANGUAGE], v[n].s = language_name(fields[4]), n++;
            /* GUTINDEX replaced letters it could not represent with '?'
             * ("Sensibilit?tome"); the CSV has the real title, and its
             * subtitle or none */
            if (title[0] && strchr(title, '?') == NULL && gf_catalog_read(g, c, entry, &old) == CDB_OK) {
                if (garbled(old.title)) {
                    v[n].id = c->field[GF_COL_TITLE], v[n].s = title, n++;
                    v[n].id = c->field[GF_COL_SORT], v[n].s = sort, n++;
                    st->retitled++;
                }
                if (garbled(old.subtitle))
                    v[n].id = c->field[GF_COL_SUBTITLE], v[n].s = sub[0] ? sub : null_value, n++;
            }
        } else { /* not in GUTINDEX: a whole row from the CSV */
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
            else if (v[i].s == null_value)
                cdb_value_write_null(&w);
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
