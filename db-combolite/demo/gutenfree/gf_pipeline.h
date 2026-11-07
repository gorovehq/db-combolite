/* GutenFree Diet: the ingest pipeline, as many writers as the engine takes.
 *
 * gutenfree pipeline [N] [base] fetches N books the database has never
 * seen (gf_fetch.py --new: 8 downloads at once, no pause) and hands each to
 * one of W worker processes the moment it lands (GUTENFREE_WORKERS,
 * default 4):
 *
 *   fetcher --BOOK n--> parent --pipe--> worker 1 .. worker W --> one database
 *
 * A worker is `gutenfree pipeline-worker`, started fresh (posix_spawn, not
 * fork: CUDA does not survive a fork) with its own cdb_db handle, so the
 * engine sees W real writers at once: W coordinators, the baton passing
 * between them, overlays, journal merges, ID-block claims, collisions. A
 * worker takes a book end to end: stores it (library row, passages, links,
 * text, one transaction), embeds its passages with its own model context on
 * the GPU, and commits the vectors. Engine refusals (busy, collision) are
 * retried three times and counted per status, never hidden. At exit each
 * worker writes its op histograms to a file; the parent merges them, so the
 * report's distributions cover every call of every writer.
 *
 * About one book in ten is held out: downloaded, never stored, its
 * passages kept as search queries the collection has never seen. After the
 * workers finish, the parent links similar books, refreshes the graph and
 * runs the search checks (stored passages, fresh phrases, held-out
 * passages). */
#ifndef GF_PIPELINE_H
#define GF_PIPELINE_H

#include "gf_embed.h"
#include "gf_vec.h"

/* The parent. g is open on base; it is closed while the workers run and
 * opened again after. */
int gf_pipeline_cli(gf_db *g, const char *base, unsigned count, const char *books_dir, const char *model);

/* A worker: books from stdin (fixed 16-byte records) until EOF, op
 * histograms to ops_path at exit. */
int gf_pipeline_worker(gf_db *g, unsigned index, const char *ops_path, const char *books_dir, const char *model);

/* The search checks embed and pipeline end with: `stored` stored passages,
 * the fresh phrases when e is non-NULL, and nheld held-out query vectors. */
cdb_status gf_search_checks(gf_db *g, const gf_vec *v, gf_embed *e, unsigned stored, const float (*held)[GF_EMBED_DIMS],
                            unsigned nheld);

#endif
