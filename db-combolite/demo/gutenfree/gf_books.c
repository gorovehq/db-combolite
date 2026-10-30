#define _POSIX_C_SOURCE 200809L
#include "gf_books.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cdb_doc.h"

cdb_status gf_books_open(gf_db *g, gf_books *b)
{
    cdb_fts_config fc;
    cdb_status s;
    int found;

    if ((s = cdb_db_find_store(&g->db, "passages", 8, &b->pool, &found)) != CDB_OK || found)
        return s;
    if ((s = cdb_db_create_store(&g->db, "passages", 8, CDB_MODEL_DOCUMENT, &b->pool)) != CDB_OK)
        return s;
    memset(&fc, 0, sizeof fc);
    fc.paths[0].ids[0] = GF_P_TEXT;
    fc.paths[0].depth = 1;
    fc.npaths = 1;
    return cdb_doc_create_full_text(&g->db, b->pool, &fc);
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

cdb_status gf_books_ingest(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, char *text,
                           size_t len, gf_book_stats *st)
{
    static uint8_t body[GF_PASSAGE_BODY];
    uint64_t row, first = 0, entry, seq = 0;
    size_t pos = 0, start, n, w = 0;
    gf_book book;
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
    if (book.passages > 0)
        return CDB_E_EXISTS;

    /* Plain line ends and valid UTF-8, in place. */
    for (size_t r = 0; r < len; r++)
        if (text[r] != '\r')
            text[w++] = text[r];
    text[w] = '\0';
    len = w;
    gf_utf8_clean(text);

    if ((s = cdb_tx_begin(&g->db, CDB_DURABILITY_DEFAULT)) != CDB_OK)
        return s;
    while (s == CDB_OK && (n = next_passage(text, len, &pos, &start)) > 0) {
        cdb_value_writer vw;
        if (n > GF_PASSAGE_MAX) {
            st->step = "split";
            s = CDB_E_STATE;
            break;
        }
        cdb_value_writer_init(&vw, body, sizeof body, GF_ORDER, CDB_DECIMAL_COMPACT);
        cdb_value_begin_object(&vw);
        cdb_value_field(&vw, GF_P_EBOOK);
        cdb_value_write_int(&vw, ebook);
        cdb_value_field(&vw, GF_P_SEQ);
        cdb_value_write_int(&vw, (int64_t)seq);
        cdb_value_field(&vw, GF_P_TEXT);
        cdb_value_write_string(&vw, text + start, n);
        cdb_value_end_object(&vw);
        st->step = "encode passage";
        if ((s = vw.error) != CDB_OK)
            break;
        st->step = "document insert";
        s = cdb_doc_insert(&g->db, b->pool, NULL, 0, body, cdb_value_writer_len(&vw), &entry);
        if (s != CDB_OK)
            break;
        if (seq == 0)
            first = entry;
        else if (entry != first + seq)
            s = CDB_E_STATE; /* the reader relies on consecutive entry IDs */
        st->passages = ++seq;
    }
    if (s == CDB_OK && seq > 0) {
        st->step = "catalog update";
        s = gf_catalog_set_passages(g, c, row, first, seq);
    }
    if (s != CDB_OK || seq == 0) {
        cdb_tx_abort(&g->db);
        return s;
    }
    st->step = "commit";
    tc = gf_now_ms();
    s = cdb_tx_commit(&g->db);
    st->commit_ms = gf_now_ms() - tc;
    st->passages = seq;
    st->ms = gf_now_ms() - t0;
    return s;
}

cdb_status gf_books_ingest_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                gf_book_stats *st)
{
    char path[1024];
    char *text;
    FILE *f;
    long size;
    cdb_status s;

    memset(st, 0, sizeof *st);
    st->ebook = ebook;
    st->step = "read file";
    snprintf(path, sizeof path, "%s/%lld.txt", dir, (long long)ebook);
    if ((f = fopen(path, "rb")) == NULL)
        return CDB_E_IO;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
        fclose(f);
        return CDB_E_IO;
    }
    rewind(f);
    text = malloc((size_t)size + 1);
    if (text == NULL || fread(text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(text);
        return CDB_E_IO;
    }
    fclose(f);
    text[size] = '\0';
    s = gf_books_ingest(g, c, b, ebook, text, (size_t)size, st);
    free(text);
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
    if (cdb_value_object_get(&obj, GF_ORDER, GF_P_EBOOK, &v, &found) == CDB_OK && found)
        *ebook = v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, GF_P_SEQ, &v, &found) == CDB_OK && found)
        *seq = v.as.i64;
    if (cdb_value_object_get(&obj, GF_ORDER, GF_P_TEXT, &v, &found) == CDB_OK && found) {
        n = v.payload_len < cap - 1 ? v.payload_len : cap - 1;
        memcpy(text, v.payload, n);
        text[n] = '\0';
    }
    return CDB_OK;
}
