/* GutenFree Diet: the database with every data model configured.
 *
 * The demo is an application, not the engine, so it takes its buffers from
 * malloc. Every size still comes from the engine's own helpers
 * (cdb_doc_buffers_size and friends), so the demo also shows what an
 * application has to provide. */
#ifndef GF_DB_H
#define GF_DB_H

#include "combolite/cdb_env.h"
#include "cdb_db.h"

#define GF_PAGE_LOG2 12u
#define GF_PAGE (1u << GF_PAGE_LOG2)
#define GF_NODES 1024u                 /* pager cache: 8 MB of 4 KB pages */
/* Verified data pages kept in memory (cdb_pager_config.page_cache): the
 * vector blocks and graph indexes a refresh or search reads over and over */
#define GF_PAGE_CACHE (128u * 1024u * 1024u)
/* Largest document body. Each byte costs about 41 bytes of engine buffers
 * (8 MB: about 330 MB), mostly full-text token slots. Books are stored as
 * passages far smaller than this, so a book of any size fits. */
#define GF_DOC_MAX (8u * 1024u * 1024u)
#define GF_DOC_STAGE (1024u * 1024u)   /* per-transaction staging */
#define GF_VEC_DIMS 384u               /* bge-small-en-v1.5 */
#define GF_VEC_K 16u                   /* neighbour-graph fan-out */
/* Neighbour-graph working memory. The minimum (cdb_vec_work_size) holds a
 * few vectors and 8 entry points, so every refresh slice overflows its
 * coverage list and walks from every entry; this much refreshes the whole
 * collection in one pass per slice and keeps about sqrt(N) entry points. */
#define GF_VEC_WORK (64u * 1024u * 1024u)
#define GF_MAX_BUFS 24
#define GF_ORDER CDB_ORDER_LITTLE      /* the byte order gf_db_open creates */

typedef struct gf_db {
    cdb_db db;
    cdb_hostenv he;
    cdb_env env;
    void *bufs[GF_MAX_BUFS];  /* everything malloc'd for the engine */
    unsigned nbufs;
    size_t bytes;             /* their total, for the diagnostics screen */
    int failed;               /* an allocation failed */
    int open;
} gf_db;

/* Opens <base>.cdbarch, creating it if it does not exist. */
cdb_status gf_db_open(gf_db *g, const char *base);

/* Closes the database (one file at rest) and frees the buffers. */
cdb_status gf_db_close(gf_db *g);

/* Monotonic clock in milliseconds, for timing ingest steps. */
double gf_now_ms(void);

#endif
