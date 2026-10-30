/* GutenFree Diet: the download queue as a background job.
 *
 * The engine is not safe to share between threads, so the job runs in
 * slices on the UI's own thread: gf_getch, which every screen uses to read
 * a key, advances the job while it waits. A slice is fetching (a child
 * process, polled), storing a book (one transaction), embedding for about
 * 100 ms, committing the vectors, the 2 s pause between books, or a budget
 * of neighbour-graph refresh at the end. No transaction stays open between
 * slices, so the screens' own reads and writes never collide with it. */
#ifndef GF_JOBS_H
#define GF_JOBS_H

#include "gf_ui.h"

enum { GF_Q_WAIT, GF_Q_RUN, GF_Q_DONE, GF_Q_FAIL, GF_Q_SHELF };

/* The queue: catalog rows, each with a state. */
unsigned gf_queue_n(void);
uint64_t gf_queue_row(unsigned i);
int gf_queue_state(unsigned i);
int gf_queue_has(uint64_t row);
void gf_queue_add(uint64_t row);
void gf_queue_remove_row(uint64_t row);
void gf_queue_remove(unsigned i);       /* not the one being worked on */
void gf_queue_clear_finished(void);     /* drop added and on-shelf books */
void gf_queue_clear(void);              /* everything not being worked on */
unsigned gf_queue_waiting(void);        /* queued or failed */

/* Starts working through the queue (failed books are retried). */
void gf_jobs_start(gf_app *a);
/* Stops after the current book (the links and graph still get done). */
void gf_jobs_stop(void);
int gf_jobs_active(void);
/* One line for status bars: what the job is doing. */
const char *gf_jobs_status(void);
/* Advances the job by about budget_ms (more while a book is stored). */
void gf_jobs_tick(double budget_ms);

/* getch that advances the job while waiting for a key, keeping the status
 * bar's job line current. Screens call it instead of getch. It also
 * handles the global diagnostics key (F2 or Ctrl-D): it opens the
 * diagnostics screen and, back from it, returns KEY_RESIZE so the screen
 * underneath redraws. */
int gf_getch(void);
/* For screens that read keys themselves: 1 when ch was the diagnostics
 * key (and the screen was shown). */
int gf_key_diag(int ch);
/* The app the diagnostics key opens the screen for. */
void gf_jobs_set_app(gf_app *a);

#endif
