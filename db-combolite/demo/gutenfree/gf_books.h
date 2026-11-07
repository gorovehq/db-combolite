/* GutenFree Diet: book text as passages in a document pool (spec 8), with a
 * full-text index on the text (spec 8.10). A passage is about GF_PASSAGE_MAX
 * bytes of a book's own paragraphs, small enough for one embedding
 * (bge-small reads 512 tokens), one search hit and one reader screen.
 *
 * Each passage says what it is, by named fields (the field dictionary):
 *   passage  its key, "<ebook>/<seq>", e.g. "1342/0005"
 *   ebook    the book's number, as in the catalog and the library
 *   title    the book's title
 *   seq      its position in the book, from 0
 *   heading  the chapter (or book, part, letter...) it falls in, if found
 *   hint     the heading and the passage's opening words
 *   words    how many words it has
 *   text     the passage itself
 * and its document summary is "<passage> · <hint> (<title>)". Links place it: the
 * library row has_passage it, and next_passage leads to the one after it
 * (1:1). A book's passages have consecutive entry IDs. */
#ifndef GF_BOOKS_H
#define GF_BOOKS_H

#include "gf_catalog.h"

#define GF_PASSAGE_MAX 2000u
#define GF_HEADING_MAX 96u
#define GF_HINT_MAX 160u
/* An encoded passage: the text, the title (at most 511 bytes), heading and
 * hint, the other fields, and while the object is being written 16 bytes
 * per field of staged directory (cdb_value.h). */
#define GF_PASSAGE_BODY (GF_PASSAGE_MAX + 512u + GF_HEADING_MAX + GF_HINT_MAX + 512u)

/* Passage fields, in the order their names are registered. */
enum { GF_P_PASSAGE, GF_P_EBOOK, GF_P_TITLE, GF_P_SEQ, GF_P_HEADING, GF_P_HINT, GF_P_WORDS, GF_P_TEXT, GF_NP };

/* book_text columns: a book's whole text in one table row (tables overlay
 * the document engine, spec 9.7, so a row may be as large as doc_max) */
enum { GF_T_EBOOK, GF_T_TITLE, GF_T_BODY, GF_NT };

typedef struct gf_books {
    uint64_t pool;
    uint64_t field[GF_NP];  /* field IDs from the pool's field dictionary */
    uint64_t next_passage;  /* link type: passage -> the next one, 1:1 */
    uint64_t text_table;    /* book_text: ebook (primary key), title, body */
    uint64_t tcol[GF_NT];
    uint64_t full_text;     /* link type: library row -> book_text row, 1:1 */
} gf_books;

typedef struct gf_book_stats {
    int64_t ebook;
    uint64_t bytes;      /* text read */
    uint64_t passages;
    double ms;           /* the whole book, commit included */
    double commit_ms;    /* of ms, the commit: pages, full-text index */
    const char *step;    /* on failure: the call that failed */
} gf_book_stats;

/* Opens the "passages" pool and the "book_text" table, creating them, the
 * pool's field names and full-text index, and the next_passage and
 * full_text link types the first time. */
cdb_status gf_books_open(gf_db *g, gf_books *b);

/* Stores the text of one ebook, in one transaction: its library row, its
 * passages and their links, and its whole text in book_text.
 * CDB_E_EXISTS: the book is in the library already; CDB_E_NOT_FOUND: no
 * catalog row for it. text is changed in place. */
cdb_status gf_books_ingest(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, char *text,
                           size_t len, gf_book_stats *st);

/* gf_books_ingest for the text in dir/<ebook>.txt (from gf_fetch.py).
 * CDB_E_IO with st->step "read file" when the file cannot be read. */
cdb_status gf_books_ingest_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                gf_book_stats *st);

/* For a book in the library without a book_text row (stored before there
 * was one): adds the row from dir/<ebook>.txt, and its full_text link, in
 * one transaction. CDB_E_EXISTS: it has one; CDB_E_NOT_FOUND: not in the
 * library; CDB_E_IO: the file cannot be read. */
cdb_status gf_books_add_text_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                  gf_book_stats *st);

/* The file of ebook in dir: the name its line in dir/manifest.tsv gives
 * (gf_fetch.py names books by title), else the older <ebook>.txt. */
void gf_books_path(const char *dir, int64_t ebook, char *path, size_t cap);

/* The next passage of text[*pos..len), as stored books are cut: whole
 * paragraphs up to GF_PASSAGE_MAX bytes, or a piece of a longer one.
 * Returns its length (0 at the end) and sets *start. */
size_t gf_books_next_passage(const char *text, size_t len, size_t *pos, size_t *start);

/* Reads passage `entry`: its text (terminated, truncated to cap), ebook and
 * position in the book. */
cdb_status gf_books_passage(gf_db *g, const gf_books *b, uint64_t entry, char *text, size_t cap, int64_t *ebook,
                            int64_t *seq);

#endif
