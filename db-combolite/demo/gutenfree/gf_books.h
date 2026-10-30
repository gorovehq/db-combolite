/* GutenFree Diet: book text as passages in a document pool (spec 8), with a
 * full-text index (spec 8.10). A passage is about GF_PASSAGE_MAX bytes of a
 * book's own paragraphs, small enough for one embedding (bge-small reads 512
 * tokens), one search hit and one reader screen. A book's passages have
 * consecutive entry IDs; its catalog row records the first and the count. */
#ifndef GF_BOOKS_H
#define GF_BOOKS_H

#include "gf_catalog.h"

#define GF_PASSAGE_MAX 2000u
/* An encoded passage: the text, its fields, and while the object is being
 * written 16 bytes per field of staged directory (cdb_value.h). */
#define GF_PASSAGE_BODY (GF_PASSAGE_MAX + 256u)

/* Passage fields: documents take field IDs as given. */
#define GF_P_EBOOK 1u
#define GF_P_SEQ 2u
#define GF_P_TEXT 3u

typedef struct gf_books {
    uint64_t pool;
} gf_books;

typedef struct gf_book_stats {
    int64_t ebook;
    uint64_t bytes;      /* text read */
    uint64_t passages;
    double ms;           /* the whole book, commit included */
    double commit_ms;    /* of ms, the commit: pages, full-text index */
    const char *step;    /* on failure: the call that failed */
} gf_book_stats;

/* Opens the "passages" pool, creating it and its full-text index the first
 * time. */
cdb_status gf_books_open(gf_db *g, gf_books *b);

/* Stores the text of one ebook, in one transaction, and records its
 * passages in the catalog. CDB_E_EXISTS: the book has passages already;
 * CDB_E_NOT_FOUND: no catalog row for it. text is changed in place. */
cdb_status gf_books_ingest(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, char *text,
                           size_t len, gf_book_stats *st);

/* gf_books_ingest for the text in dir/<ebook>.txt (from gf_fetch.py).
 * CDB_E_IO with st->step "read file" when the file cannot be read. */
cdb_status gf_books_ingest_file(gf_db *g, const gf_catalog *c, const gf_books *b, int64_t ebook, const char *dir,
                                gf_book_stats *st);

/* Reads passage `entry`: its text (terminated, truncated to cap), ebook and
 * position in the book. */
cdb_status gf_books_passage(gf_db *g, const gf_books *b, uint64_t entry, char *text, size_t cap, int64_t *ebook,
                            int64_t *seq);

#endif
