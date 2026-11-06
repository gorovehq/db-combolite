#define _POSIX_C_SOURCE 200809L
#include "gf_books.h"
#include "gf_bench.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cdb_doc.h"
#include "cdb_link.h"

static const char *const field_names[GF_NP] = {
    "passage", "ebook", "title", "seq", "heading", "hint", "words", "text",
};

static const char *const text_cols[GF_NT] = { "ebook", "title", "body" };

/* The book_text table and the full_text link from the library to it */
static cdb_status open_text_table(gf_db *g, gf_books *b)
{
    cdb_status s;
    int found;
    unsigned i;

    if ((s = cdb_db_find_store(&g->db, "book_text", 9, &b->text_table, &found)) != CDB_OK)
        return s;
    if (!found) {
        cdb_table_column cols[GF_NT];
        static const unsigned pk[1] = { GF_T_EBOOK };
        cdb_table_def def;
        memset(cols, 0, sizeof cols);
        for (i = 0; i < GF_NT; i++) {
            cols[i].name = text_cols[i];
            cols[i].name_len = strlen(text_cols[i]);
            cols[i].type = CDB_TYPE_STRING;
            cols[i].not_null = 1;
        }
        cols[GF_T_EBOOK].type = CDB_TYPE_INT;
        memset(&def, 0, sizeof def);
        def.columns = cols;
        def.ncolumns = GF_NT;
        def.primary_key = pk;
        def.npk = 1;
        def.keep = 1;
        if ((s = cdb_table_create(&g->db, "book_text", 9, &def, &b->text_table)) != CDB_OK)
            return s;
    }
    for (i = 0; i < GF_NT; i++)
        if ((s = cdb_table_column_id(&g->db, b->text_table, text_cols[i], strlen(text_cols[i]), &b->tcol[i]))
            != CDB_OK)
            return s;
    if ((s = cdb_link_find_type(&g->db, "full_text", 9, &b->full_text, &found)) != CDB_OK || found)
        return s;
    return cdb_link_define_type(&g->db, "full_text", 9, CDB_CARD_1_1, 1, CDB_LINK_ON_DELETE_TOMBSTONE,
                                &b->full_text);
}

cdb_status gf_books_open(gf_db *g, gf_books *b)
{
    cdb_fts_config fc;
    cdb_status s;
    int found, created = 0;
    unsigned i;

    if ((s = cdb_db_find_store(&g->db, "passages", 8, &b->pool, &found)) != CDB_OK)
        return s;
    if (!found) {
        if ((s = cdb_db_create_store(&g->db, "passages", 8, CDB_MODEL_DOCUMENT, &b->pool)) != CDB_OK)
            return s;
        created = 1;
    }
    /* Named fields: anyone reading the pool, the studio or an agent, sees
     * "passage" and "text", not field numbers */
    for (i = 0; i < GF_NP; i++)
        if ((s = cdb_catalog_field_id(&g->db.catalog, b->pool, field_names[i], strlen(field_names[i]),
                                      &b->field[i])) != CDB_OK)
            return s;
    if (created) {
        memset(&fc, 0, sizeof fc);
        fc.paths[0].ids[0] = b->field[GF_P_TEXT];
        fc.paths[0].depth = 1;
        fc.npaths = 1;
        if ((s = cdb_doc_create_full_text(&g->db, b->pool, &fc)) != CDB_OK)
            return s;
    }
    if ((s = cdb_link_find_type(&g->db, "next_passage", 12, &b->next_passage, &found)) != CDB_OK)
        return s;
    if (!found && (s = cdb_link_define_type(&g->db, "next_passage", 12, CDB_CARD_1_1, 1,
                                            CDB_LINK_ON_DELETE_TOMBSTONE, &b->next_passage)) != CDB_OK)
        return s;
    return open_text_table(g, b);
}

/* One field of a passage: a string of n bytes, or with s NULL an int. */
typedef struct pfield {
    uint64_t id;
    const char *s;
    size_t n;
    int64_t i;
} pfield;

static int by_field(const void *a, const void *b)
{
    uint64_t x = ((const pfield *)a)->id, y = ((const pfield *)b)->id;
    return x < y ? -1 : x > y;
}

/* Encodes the fields as an object, in field-ID order (spec 14.6.5). */
static cdb_status encode_fields(pfield *v, unsigned n, uint8_t *out, size_t cap, size_t *len)
{
    cdb_value_writer w;
    unsigned i;
    qsort(v, n, sizeof v[0], by_field);
    cdb_value_writer_init(&w, out, cap, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    for (i = 0; i < n; i++) {
        cdb_value_field(&w, v[i].id);
        if (v[i].s == NULL)
            cdb_value_write_int(&w, v[i].i);
        else
            cdb_value_write_string(&w, v[i].s, v[i].n);
    }
    cdb_value_end_object(&w);
    *len = cdb_value_writer_len(&w);
    return w.error;
}

/* The words of text[0..n): runs of non-space. */
static uint64_t count_words(const char *text, size_t n)
{
    uint64_t w = 0;
    int in = 0;
    for (size_t i = 0; i < n; i++) {
        int sp = text[i] == ' ' || text[i] == '\n' || text[i] == '\t';
        if (!sp && !in)
            w++;
        in = !sp;
    }
    return w;
}

/* Whether the paragraph p[0..n) is a heading: one short line that starts
 * with a division word ("CHAPTER I.", "Book 2", "LETTER IV") or is a roman
 * numeral alone ("XIV."). */
static int is_heading(const char *p, size_t n)
{
    static const char *const words[] = {
        "chapter", "book", "part", "letter", "act", "scene", "volume", "canto", "stave", "section",
        "prologue", "epilogue", "preface", "introduction", "contents",
    };
    size_t i, k;
    if (n == 0 || n > 60 || memchr(p, '\n', n) != NULL)
        return 0;
    for (k = 0; k < sizeof words / sizeof words[0]; k++) {
        size_t wl = strlen(words[k]);
        if (n >= wl && strncasecmp(p, words[k], wl) == 0 && (n == wl || !isalpha((unsigned char)p[wl])))
            return 1;
    }
    for (i = 0; i < n && strchr("IVXLC", p[i]) != NULL; i++)
        ;
    return i > 0 && (i == n || (i + 1 == n && p[i] == '.'));
}

/* Walks the paragraphs of a passage: sets *first to its first heading (if
 * any) and *last to its last; *body to where the text after any leading
 * headings starts. */
static void scan_headings(const char *t, size_t n, const char **first, size_t *flen, const char **last,
                          size_t *llen, size_t *body)
{
    size_t p = 0;
    int leading = 1;
    *first = *last = NULL;
    *flen = *llen = 0;
    *body = 0;
    while (p < n) {
        size_t e = p, q;
        while (e < n && !(t[e] == '\n' && e + 1 < n && t[e + 1] == '\n'))
            e++;
        q = e;
        while (q > p && (t[q - 1] == ' ' || t[q - 1] == '\n'))
            q--;
        if (is_heading(t + p, q - p)) {
            if (*first == NULL)
                *first = t + p, *flen = q - p;
            *last = t + p, *llen = q - p;
        } else if (leading && q > p) {
            *body = p;
            leading = 0;
        }
        while (e < n && (t[e] == '\n' || t[e] == ' '))
            e++;
        p = e;
    }
    if (leading)
        *body = n;
}

/* Copies at most cap - 1 bytes of src[0..n) to dst with line breaks as
 * spaces, ending at a word (and a character) boundary; returns the length. */
static size_t copy_line(char *dst, size_t cap, const char *src, size_t n)
{
    size_t len = 0, i;
    for (i = 0; i < n && len + 1 < cap; i++) {
        char ch = src[i] == '\n' || src[i] == '\t' ? ' ' : src[i];
        if (ch == ' ' && len > 0 && dst[len - 1] == ' ')
            continue;
        dst[len++] = ch;
    }
    if (i < n) { /* cut: back to the last space */
        size_t sp = len;
        while (sp > 0 && dst[sp - 1] != ' ')
            sp--;
        if (sp > cap / 2)
            len = sp - 1;
        while (len > 0 && ((unsigned char)dst[len] & 0xC0) == 0x80)
            len--;
    }
    dst[len] = '\0';
    return len;
}

/* The next passage of text[*pos..len): whole paragraphs up to
 * GF_PASSAGE_MAX bytes, or a piece of one paragraph that is longer, cut at
 * a line end or space and at a character boundary. Returns its length
 * (0 at the end) and sets *start. */
static size_t next_passage(const char *text, size_t len, size_t *pos, size_t *start)
{
    size_t p = *pos, end, cut = 0;

    while (p < len && (text[p] == '\n' || text[p] == ' ' || text[p] == '\t'))
        p++;
    *start = p;
    if (p == len) {
        *pos = p;
        return 0;
    }
    end = p + GF_PASSAGE_MAX < len ? p + GF_PASSAGE_MAX : len;
    if (end == len) {
        *pos = len;
        return len - p;
    }
    /* The last paragraph break within reach, else the last line end, else
     * the last space, else a character boundary. */
    for (size_t i = end; i > p + 1; i--)
        if (text[i - 1] == '\n' && text[i - 2] == '\n') {
            cut = i;
            break;
        }
    for (size_t i = end; cut == 0 && i > p; i--)
        if (text[i - 1] == '\n')
            cut = i;
    for (size_t i = end; cut == 0 && i > p; i--)
        if (text[i - 1] == ' ')
            cut = i;
    if (cut == 0) {
        cut = end;
        while (cut > p && ((unsigned char)text[cut] & 0xC0) == 0x80)
            cut--;
    }
    *pos = cut;
    while (cut > p && (text[cut - 1] == '\n' || text[cut - 1] == ' '))
        cut--;
    return cut - p;
}

/* Plain line ends and valid UTF-8, in place; *len is updated */
static void clean_text(char *text, size_t *len)
{
    size_t w = 0;
    for (size_t r = 0; r < *len; r++)
        if (text[r] != '\r')
            text[w++] = text[r];
    text[w] = '\0';
    *len = w;
    gf_utf8_clean(text);
}

/* Inside a transaction: the book's whole text as its book_text row, linked
 * from its library row by full_text */
static cdb_status store_text(gf_db *g, const gf_catalog *c, const gf_books *b, uint64_t lib_row,
                             const gf_book *book, const char *text, size_t len)
{
    size_t title_len = strlen(book->title), cap = len + title_len + 128u, n;
    uint8_t *row = malloc(cap);
    uint64_t entry;
    pfield v[GF_NT];
    cdb_status s;

    if (row == NULL)
        return CDB_E_BUFFER_TOO_SMALL;
    v[0].id = b->tcol[GF_T_EBOOK], v[0].s = NULL, v[0].i = book->ebook;
    v[1].id = b->tcol[GF_T_TITLE], v[1].s = book->title, v[1].n = title_len;
    v[2].id = b->tcol[GF_T_BODY], v[2].s = text, v[2].n = len;
    if ((s = encode_fields(v, GF_NT, row, cap, &n)) == CDB_OK
        && (s = GF_T("table.insert_text", cdb_table_insert(&g->db, b->text_table, row, n, &entry))) == CDB_OK)
        s = GF_T("link.add_full_text", cdb_link_add(&g->db, b->full_text, cdb_ref_entry(c->lib_store, lib_row),
                         cdb_ref_entry(b->text_table, entry), 1.0f, 0));
    free(row);
    return s;
}

cdb_status gf_books_ingest(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, char *text,
                           size_t len, gf_book_stats *st)
{
    static uint8_t body[GF_PASSAGE_BODY];
    static gf_book book;
    char key[48], first_key[48], last_key[48], heading[GF_HEADING_MAX + 1], hint[GF_HINT_MAX + 1];
    char summary[GF_HINT_MAX + 600];
    uint64_t row, lib_row, entry, prev = 0, seq = 0, count = 0;
    size_t pos = 0, start, n, w = 0;
    int found;
    cdb_status s;
    double t0 = gf_now_ms(), tc;

    memset(st, 0, sizeof *st);
    st->ebook = ebook;
    st->bytes = len;
    st->step = "catalog find";
    if ((s = gf_catalog_find(g, c, ebook, &row, &found)) != CDB_OK)
        return s;
    if (!found)
        return CDB_E_NOT_FOUND;
    st->step = "catalog read";
    if ((s = gf_catalog_read(g, c, row, &book)) != CDB_OK)
        return s;
    if (book.lib_row != 0)
        return CDB_E_EXISTS;

    clean_text(text, &len);

    /* The library row comes first, so the passages can be linked to it:
     * count them before writing any */
    while ((n = next_passage(text, len, &pos, &start)) > 0)
        count++;
    if (count == 0)
        return CDB_OK;
    snprintf(first_key, sizeof first_key, "%lld/%04llu", (long long)ebook, 0ull);
    snprintf(last_key, sizeof last_key, "%lld/%04llu", (long long)ebook, (unsigned long long)(count - 1));

    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    st->step = "library row";
    s = gf_library_add(g, c, row, &book, count, count_words(text, len), first_key, last_key, &lib_row);
    if (s == CDB_OK) {
        st->step = "book text";
        s = store_text(g, c, b, lib_row, &book, text, len);
    }
    heading[0] = '\0';
    pos = 0;
    while (s == CDB_OK && (n = next_passage(text, len, &pos, &start)) > 0) {
        const char *t = text + start, *h1, *hl;
        size_t h1len, hllen, at, hlen;
        pfield v[GF_NP];
        unsigned k = 0;

        if (n > GF_PASSAGE_MAX) {
            st->step = "split";
            s = CDB_E_STATE;
            break;
        }
        /* Its heading: one it starts, else the one it falls under */
        scan_headings(t, n, &h1, &h1len, &hl, &hllen, &at);
        if (h1 != NULL && at > 0)
            copy_line(heading, sizeof heading, h1, h1len);
        hlen = 0;
        if (heading[0])
            hlen = (size_t)snprintf(hint, sizeof hint, "%s: ", heading);
        copy_line(hint + hlen, sizeof hint - hlen, t + at, n - at);
        snprintf(key, sizeof key, "%lld/%04llu", (long long)ebook, (unsigned long long)seq);
        /* Key first: it is what tells passages of one book apart wherever a
         * label is cut short (graph nodes, link lists) */
        snprintf(summary, sizeof summary, "%s \xc2\xb7 %s (%s)", key, hint, book.title);

        v[k].id = b->field[GF_P_PASSAGE], v[k].s = key, v[k].n = strlen(key), k++;
        v[k].id = b->field[GF_P_EBOOK], v[k].s = NULL, v[k].i = ebook, k++;
        v[k].id = b->field[GF_P_TITLE], v[k].s = book.title, v[k].n = strlen(book.title), k++;
        v[k].id = b->field[GF_P_SEQ], v[k].s = NULL, v[k].i = (int64_t)seq, k++;
        if (heading[0])
            v[k].id = b->field[GF_P_HEADING], v[k].s = heading, v[k].n = strlen(heading), k++;
        v[k].id = b->field[GF_P_HINT], v[k].s = hint, v[k].n = strlen(hint), k++;
        v[k].id = b->field[GF_P_WORDS], v[k].s = NULL, v[k].i = (int64_t)count_words(t, n), k++;
        v[k].id = b->field[GF_P_TEXT], v[k].s = t, v[k].n = n, k++;
        st->step = "encode passage";
        if ((s = encode_fields(v, k, body, sizeof body, &w)) != CDB_OK)
            break;
        st->step = "document insert";
        if ((s = GF_T("doc.insert_passage", cdb_doc_insert(&g->db, b->pool, summary, strlen(summary), body, w, &entry))) != CDB_OK)
            break;
        if (seq > 0 && entry != prev + 1) {
            s = CDB_E_STATE; /* the reader relies on consecutive entry IDs */
            break;
        }
        st->step = "link has_passage";
        if ((s = GF_T("link.add_has_passage", cdb_link_add(&g->db, c->has_passage, cdb_ref_entry(c->lib_store, lib_row),
                              cdb_ref_entry(b->pool, entry), 1.0f, 0))) != CDB_OK)
            break;
        if (seq > 0) {
            st->step = "link next_passage";
            if ((s = GF_T("link.add_next", cdb_link_add(&g->db, b->next_passage, cdb_ref_entry(b->pool, prev), cdb_ref_entry(b->pool, entry),
                                  1.0f, 0))) != CDB_OK)
                break;
        }
        /* The next passage falls under the last heading this one has */
        if (hl != NULL)
            copy_line(heading, sizeof heading, hl, hllen);
        prev = entry;
        st->passages = ++seq;
    }
    if (s != CDB_OK) {
        cdb_tx_abort(&g->db);
        return s;
    }
    st->step = "commit";
    tc = gf_now_ms();
    s = GF_T("tx.commit_book", cdb_tx_commit(&g->db));
    st->commit_ms = gf_now_ms() - tc;
    st->passages = seq;
    st->ms = gf_now_ms() - t0;
    return s;
}

/* dir/<ebook>.txt, read whole into a malloc'd, terminated buffer */
static cdb_status read_book_file(const char *dir, int64_t ebook, char **text, size_t *len)
{
    char path[1024];
    FILE *f;
    long size;

    *text = NULL;
    snprintf(path, sizeof path, "%s/%lld.txt", dir, (long long)ebook);
    if ((f = fopen(path, "rb")) == NULL)
        return CDB_E_IO;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return CDB_E_IO;
    }
    rewind(f);
    *text = malloc((size_t)size + 1);
    if (*text == NULL || fread(*text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(*text);
        *text = NULL;
        return CDB_E_IO;
    }
    fclose(f);
    (*text)[size] = '\0';
    *len = (size_t)size;
    return CDB_OK;
}

cdb_status gf_books_ingest_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                gf_book_stats *st)
{
    char *text;
    size_t len;
    cdb_status s;

    memset(st, 0, sizeof *st);
    st->ebook = ebook;
    st->step = "read file";
    if ((s = read_book_file(dir, ebook, &text, &len)) != CDB_OK)
        return s;
    s = gf_books_ingest(g, c, b, ebook, text, len, st);
    free(text);
    return s;
}

cdb_status gf_books_add_text_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                  gf_book_stats *st)
{
    static gf_book book;
    uint8_t key[16];
    cdb_value_writer w;
    uint64_t row, entry;
    char *text;
    size_t len;
    int found;
    cdb_status s;
    double t0 = gf_now_ms();

    memset(st, 0, sizeof *st);
    st->ebook = ebook;
    st->step = "library find";
    if ((s = gf_catalog_find(g, c, ebook, &row, &found)) != CDB_OK)
        return s;
    if (!found || (s = gf_catalog_read(g, c, row, &book)) != CDB_OK || book.lib_row == 0)
        return s != CDB_OK ? s : CDB_E_NOT_FOUND;
    cdb_value_writer_init(&w, key, sizeof key, GF_ORDER, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, ebook);
    cdb_value_end_array(&w);
    if ((s = cdb_table_find(&g->db, b->text_table, key, cdb_value_writer_len(&w), &entry, &found)) != CDB_OK)
        return s;
    if (found)
        return CDB_E_EXISTS;
    st->step = "read file";
    if ((s = read_book_file(dir, ebook, &text, &len)) != CDB_OK)
        return s;
    clean_text(text, &len);
    st->bytes = len;
    st->step = "book text";
    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) == CDB_OK) {
        if ((s = store_text(g, c, b, book.lib_row, &book, text, len)) == CDB_OK)
            s = cdb_tx_commit(&g->db);
        else
            cdb_tx_abort(&g->db);
    }
    free(text);
    st->ms = gf_now_ms() - t0;
    return s;
}

cdb_status gf_books_passage(gf_db *g, const gf_books *b, uint64_t entry, char *text, size_t cap, int64_t *ebook,
                            int64_t *seq)
{
    static uint8_t body[GF_PASSAGE_BODY];
    size_t len, n;
    cdb_value obj, v;
    int found;
    cdb_status s = cdb_doc_get(&g->db, b->pool, entry, 0, body, sizeof body, &len, NULL, 0, NULL, NULL);

    text[0] = '\0';
    *ebook = *seq = 0;
    if (s != CDB_OK)
        return s;
    if ((s = cdb_value_read(body, len, GF_ORDER, &obj)) != CDB_OK)
        return s;
    if (cdb_value_object_get(&obj, GF_ORDER, b->field[GF_P_EBOOK], &v, &found) == CDB_OK && found)
        *ebook = v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, b->field[GF_P_SEQ], &v, &found) == CDB_OK && found)
        *seq = v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, b->field[GF_P_TEXT], &v, &found) == CDB_OK && found) {
        n = v.payload_len < cap - 1 ? v.payload_len : cap - 1;
        memcpy(text, v.payload, n);
        text[n] = '\0';
    }
    return CDB_OK;
}
