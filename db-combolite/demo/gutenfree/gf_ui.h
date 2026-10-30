/* GutenFree Diet: the ncurses screens. Include after the engine headers
 * (see gf_main.c). */
#ifndef GF_UI_H
#define GF_UI_H

#include "gf_books.h"
#include "gf_catalog.h"
#include "gf_embed.h"
#include "gf_vec.h"

/* Everything the screens use. */
typedef struct gf_app {
    gf_db *g;
    const char *base;   /* the database path without extension */
    const char *model;  /* the embedding model file */
    const char *books_dir; /* where gf_fetch.py puts book texts */
    const char *mirror;    /* the Gutenberg mirror gf_fetch.py uses */
    gf_catalog cat;
    gf_books books;
    gf_vec vec;
    gf_embed *emb;      /* loaded on first use */
} gf_app;

/* Prints s at (y, x), at most width characters (UTF-8 code points). */
void gf_put_clip(int y, int x, const char *s, int width);

/* Word-wraps s into the box from (y, x), width wide, down to row maxy;
 * returns the row after the last one used. */
int gf_put_wrap(int y, int x, int width, int maxy, const char *s);

/* Draws the reverse-video status line at the bottom, with the background
 * download's line on the right. refresh redraws the last one. */
void gf_status(const char *fmt, ...);
void gf_status_refresh(void);

/* Asks for a line of input on the status row; returns its length, 0 if
 * cancelled. */
int gf_prompt(const char *question, char *out, int cap);

void gf_screen_catalog(gf_app *a);
void gf_screen_book(gf_app *a, uint64_t entry);
void gf_screen_ingest(gf_app *a, const char *gutindex);
void gf_screen_shelf(gf_app *a);
void gf_screen_reader(gf_app *a, uint64_t passage);
void gf_screen_search(gf_app *a);
void gf_screen_diag(gf_app *a);
void gf_screen_semantic(gf_app *a);
void gf_screen_similar(gf_app *a, uint64_t row);
void gf_screen_embed(gf_app *a);
/* Fetches the book in catalog row `row` from the mirror, stores it and
 * embeds it (when the model is there); offers to open it. */
/* Queues the book in catalog row `row` and starts the background download
 * (gf_jobs.h); opens the reader instead if it is already on the shelf. */
void gf_screen_add(gf_app *a, uint64_t row);
/* The download queue: its books and their state; starts and stops the
 * background job. */
void gf_screen_queue(gf_app *a);

#endif
