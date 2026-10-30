/* GutenFree Diet: the card catalog, a row-oriented table built from
 * GUTINDEX.ALL (spec 9). One row per ebook, keyed by its number, with an
 * index on a short sort title for browsing. */
#ifndef GF_CATALOG_H
#define GF_CATALOG_H

#include "gf_db.h"
#include "combolite/cdb_value.h" /* cdb_table.h needs it first */
#include "cdb_table.h"

enum {
    GF_COL_EBOOK,     /* int, primary key */
    GF_COL_TITLE,     /* string, not null */
    GF_COL_AUTHOR,    /* string */
    GF_COL_SUBTITLE,  /* string */
    GF_COL_LANGUAGE,  /* string, not null; English when the index names none */
    GF_COL_CREDITS,   /* string: the other bracketed tags, "Illustrator: X; ..." */
    GF_COL_SORT,      /* string, not null: lowercase title without a leading article */
    GF_NCOLS
};

/* Bibliographic columns from pg_catalog.csv, added by a second schema change
 * (all strings; null until gf_catalog_merge_csv has run). */
enum {
    GF_BIB_TYPE,        /* Text, Sound, ... */
    GF_BIB_ISSUED,      /* YYYY-MM-DD */
    GF_BIB_AUTHORS,     /* "Surname, Given, dates; ..." */
    GF_BIB_SUBJECTS,    /* "; "-separated */
    GF_BIB_LOCC,        /* Library of Congress classes, "; "-separated */
    GF_BIB_SHELVES,     /* Gutenberg bookshelves, "; "-separated */
    GF_BIB_GENRE,       /* the first "Category:" bookshelf, else the first one */
    GF_BIB_AUTHOR_SORT, /* lowercase first author, at most GF_SORT_MAX bytes */
    GF_NBIB
};

/* Indexes, by number (a table numbers them in creation order; 0 is the
 * primary key). Together they are the engine's limit of 8 per table. */
#define GF_CATALOG_PK_INDEX 0u      /* ebook */
#define GF_CATALOG_SORT_INDEX 1u    /* sort_title */
#define GF_CATALOG_SHELF_INDEX 2u   /* passages: the books with text */
#define GF_CATALOG_GENRE_INDEX 3u   /* genre, sort_title */
#define GF_CATALOG_AUTHOR_INDEX 4u  /* author_sort, sort_title */
#define GF_CATALOG_ISSUED_INDEX 5u  /* issued, sort_title */
#define GF_CATALOG_LANG_INDEX 6u    /* language, sort_title */
#define GF_CATALOG_LOCC_INDEX 7u    /* locc, sort_title */
#define GF_SORT_MAX 96u
#define GF_LOCC_MAX 64u

/* first_passage and passages (ints) are added by a schema change after the
 * table exists, as a real application would grow: rows written before it
 * read them as null (spec 9.8). */
typedef struct gf_catalog {
    uint64_t store;
    uint64_t field[GF_NCOLS];
    uint64_t first_field;     /* first_passage: entry ID of the first passage */
    uint64_t passages_field;  /* passages: how many */
    uint64_t bib[GF_NBIB];
} gf_catalog;

typedef struct gf_book {
    int64_t ebook;
    char title[512];
    char author[256];
    char subtitle[512];
    char language[64];
    char credits[768];
    uint64_t first_passage;   /* 0 with passages 0: no text stored */
    uint64_t passages;
    char type[16];
    char issued[16];
    char authors[512];
    char subjects[1024];
    char locc[GF_LOCC_MAX + 1];
    char shelves[1024];
    char genre[128];
} gf_book;

/* A catalog filter: every non-empty condition must hold. Text conditions
 * match a substring, ignoring ASCII case. */
typedef struct gf_filter {
    char title[64];      /* title or subtitle */
    char author[64];     /* GUTINDEX author or pg_catalog authors */
    char genre[64];      /* any bookshelf (genre) */
    char subject[64];
    char language[32];
    char locc[16];       /* LoCC class prefix, e.g. "PS" */
    char type[16];
    char from[16];       /* issued on or after (YYYY or YYYY-MM-DD) */
    char to[16];         /* issued on or before */
    int on_shelf;        /* only books with passages */
} gf_filter;

typedef struct gf_ingest_stats {
    uint64_t entries;      /* entries parsed */
    uint64_t inserted;
    uint64_t duplicates;   /* already in the table */
    uint64_t updated;      /* merge: rows given bibliographic data */
    uint64_t refused;      /* rejected by the table's checks */
    unsigned batches;      /* commits */
    unsigned batch_rows;   /* rows per commit */
    double ms;             /* whole ingest */
    double last_batch_ms;
    double worst_batch_ms;
    double parse_ms;       /* of ms, time spent parsing text */
} gf_ingest_stats;

typedef void (*gf_progress_fn)(const gf_ingest_stats *st, void *ctx);

/* Opens the "books" table, creating it and its sort index the first time. */
cdb_status gf_catalog_open(gf_db *g, gf_catalog *c);

/* Adds every entry of a GUTINDEX.ALL file that is not in the table yet,
 * committing every `batch` rows; progress is called after each commit.
 * The default is small because each insert's unique check reads back the
 * transaction's own rows at a cost that grows with the square of their
 * number (docs/BACKLOG.md). */
#define GF_INGEST_BATCH 100u
cdb_status gf_catalog_ingest(gf_db *g, const gf_catalog *c, const char *path, unsigned batch,
                             gf_ingest_stats *st, gf_progress_fn progress, void *ctx);

/* Reads one row into b (strings truncated to fit, always terminated). */
cdb_status gf_catalog_read(gf_db *g, const gf_catalog *c, uint64_t entry, gf_book *b);

/* Finds a row by ebook number. */
cdb_status gf_catalog_find(gf_db *g, const gf_catalog *c, int64_t ebook, uint64_t *entry, int *found);

/* Inside a transaction: records where a book's passages are. */
cdb_status gf_catalog_set_passages(gf_db *g, const gf_catalog *c, uint64_t entry, uint64_t first, uint64_t count);

/* The books with passages, through the shelf index; zero cur to start. */
cdb_status gf_catalog_shelf(gf_db *g, const gf_catalog *c, cdb_table_cursor *cur, uint64_t *entries, size_t max,
                            size_t *n);

/* Counts the rows by scanning the table. */
cdb_status gf_catalog_count(gf_db *g, const gf_catalog *c, uint64_t *n);

/* Replaces invalid UTF-8 bytes in s with '?', in place. */
void gf_utf8_clean(char *s);

/* Merges pg_catalog.csv: fills the bibliographic columns of every row it
 * names, and inserts the ebooks GUTINDEX does not list, committing every
 * GF_INGEST_BATCH rows. */
#define GF_DEFAULT_PGCSV "data/pg_catalog.csv"
cdb_status gf_catalog_merge_csv(gf_db *g, const gf_catalog *c, const char *path, gf_ingest_stats *st,
                                gf_progress_fn progress, void *ctx);

/* Every row matching f, in the order of index `index`: at most cap entry
 * IDs in out, *n of them; *scanned rows were looked at. */
cdb_status gf_catalog_filter(gf_db *g, const gf_catalog *c, uint32_t index, const gf_filter *f, uint64_t *out,
                             size_t cap, size_t *n, uint64_t *scanned);
int gf_filter_empty(const gf_filter *f);

#endif
