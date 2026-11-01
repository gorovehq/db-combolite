/* Row groups of column-oriented tables (spec 9.12, 14.10.2).
 *
 * A column-oriented table keeps its rows in row groups. A group holds up to
 * `rows` rows (default 1024) and closes early when any column's values would
 * pass `cap` bytes (default 64 KiB), so the memory one group needs is bounded
 * by those two numbers. Groups cover disjoint, ascending entry-ID ranges of
 * varying width.
 *
 * Groups index (an ordered index of the table): key = the group's LAST entry
 * ID (cdb_key_u64), so seeking entry e lands on the only group that can hold
 * it; value = the group header, all big-endian:
 *   first entry (8) . rows (4) . schema version (4) . column count (2)
 *   then 1 + column count chunk references, the entry-ID chunk first and the
 *   columns in ascending field-ID order:
 *     field ID (8; 0 for the entry-ID chunk) . tag (1) . address length (1)
 *     . address (8 or 10, cdb_nib_addr_store) . stored bytes (4)
 *     . null count (4) . min length (1) . min . max length (1) . max
 * The tag is the column's value tag (a bool column uses CDB_TAG_FALSE). min
 * and max are the smallest and largest present values in the shared
 * encoding, kept only when each is at most CDB_RG_STAT_MAX bytes (length 0
 * otherwise, and always for the entry-ID chunk and for types without an
 * order: decimal, array, object, vector).
 *
 * Entry-ID chunk: the group's entry IDs, strictly ascending, as an int
 * column with no nulls. Position p of every column chunk is the row whose
 * entry ID is at position p here.
 *
 * Chunk bytes: encoding ID (1) . codec (1) . null bitmap . values. The
 * bitmap is present only when the null count is not 0: ceil(rows / 8)
 * bytes, bit p (most significant bit first) set when row p is null, unused
 * bits zero. The values are the column's present values in the cdb_colenc
 * form for that encoding, as is (codec 0) or as a cdb_codec form (codec 1,
 * LZMA2), which is kept only when it saves at least 1/8 (cdb_codec_keeps).
 * A chunk of at most a quarter page is a nibblet of kind 3 whose owner key
 * is the group's last entry ID and the field ID (8 + 8, big-endian);
 * a larger one is an overflow chain of its own.
 *
 * Memory: everything comes from one caller work area (cdb_rg_work_size),
 * plus the environment's allocator for the codec when it has one. */
#ifndef CDB_ROWGROUP_H
#define CDB_ROWGROUP_H

#include "cdb_codec.h"
#include "cdb_colenc.h"
#include "cdb_db.h"
#include "cdb_index.h"

#define CDB_RG_ROWS_DEFAULT 1024u
#define CDB_RG_CAP_DEFAULT 65536u
#define CDB_RG_ROWS_MAX 65536u
#define CDB_RG_STAT_MAX 32u
#define CDB_RG_ADDR_MAX 10u

/* One chunk reference of a group header. */
typedef struct cdb_rg_ref {
    uint64_t field_id;      /* 0: the entry-ID chunk */
    uint8_t tag;            /* column value tag */
    uint64_t addr;          /* nibblet address or first overflow page */
    uint32_t stored;        /* chunk bytes as stored */
    uint32_t nulls;
    uint8_t min_len, max_len;
    uint8_t min[CDB_RG_STAT_MAX];
    uint8_t max[CDB_RG_STAT_MAX];
} cdb_rg_ref;

typedef struct cdb_rg_header {
    uint64_t first, last;
    uint32_t rows;
    uint32_t schema;
    unsigned ncols;
    cdb_rg_ref ids;
    cdb_rg_ref cols[CDB_TABLE_MAX_COLUMNS];
} cdb_rg_header;

/* A column to build: its field ID (> 0) and value tag. */
typedef struct cdb_rg_col {
    uint64_t field_id;
    uint8_t tag;
} cdb_rg_col;

/* The rows of a group being built, read once per chunk: rewind, then
 * `rows` calls of next. next gives the row's entry ID and its value of
 * field_id (field 0: only the entry is wanted), or *val NULL for null. The
 * value stays valid until the next call. */
typedef struct cdb_rg_source {
    void *ctx;
    cdb_status (*rewind)(void *ctx);
    cdb_status (*next)(void *ctx, uint64_t field_id, uint64_t *entry, const uint8_t **val, size_t *vlen);
} cdb_rg_source;

/* A table's row groups with their working memory. */
typedef struct cdb_rg {
    cdb_db *db;
    uint64_t store;
    cdb_index *groups;
    uint32_t rows;
    size_t cap;
    cdb_rg_header *hdr;     /* the group last read or built */
    uint8_t *hbytes;        /* an encoded header */
    uint64_t *entries;      /* the entry IDs of hdr's group, once loaded */
    uint8_t *bitmap;        /* a null bitmap being built */
    uint8_t *vals;          /* one column's values, or one row's (cap bytes) */
    uint64_t *scratch;      /* cdb_colenc scratch */
    size_t scratch_words;
    uint8_t *enc;           /* encoded or decompressed values; a row being assembled */
    size_t enc_cap;
    uint8_t *chunk;         /* a chunk as stored */
    size_t chunk_cap;
    uint8_t *page;          /* a page buffer */
    cdb_codec_mem codec;
} cdb_rg;

/* A column of a loaded group, read value by value. */
typedef struct cdb_rg_column {
    cdb_rg *g;
    const cdb_rg_ref *ref;
    const uint8_t *bitmap;  /* NULL when the column has no nulls */
    uint32_t row;           /* rows read so far */
    cdb_colenc_reader r;
} cdb_rg_column;

/* Bytes of work memory for groups of up to rows rows and cap value bytes per
 * column; with_codec adds the codec's memory for an environment without an
 * allocator. */
size_t cdb_rg_work_size(size_t page_size, uint32_t rows, size_t cap, int with_codec);

/* Sets g up over the table's groups index. CDB_E_BUFFER_TOO_SMALL if the
 * work area is too small for rows and cap. */
cdb_status cdb_rg_init(cdb_rg *g, cdb_db *db, uint64_t store, cdb_index *groups, uint32_t rows, size_t cap,
                       uint8_t *work, size_t work_size);

/* Builds a group of n rows (1..g->rows) from src with the given columns
 * (ascending field IDs), written under schema version `schema`: chunks
 * first, then the header. CDB_E_OVERFLOW if a column's values pass g->cap;
 * nothing is left behind then. CDB_E_INVALID_ARG for entries that do not
 * ascend or a value of another type. */
cdb_status cdb_rg_write(cdb_rg *g, const cdb_rg_source *src, uint32_t n, uint32_t schema, const cdb_rg_col *cols,
                        unsigned ncols);

/* Loads the header of the group that holds entry (or would: the group whose
 * range reaches it) into g->hdr. *found 0 when no group's last entry is at
 * or after entry. */
cdb_status cdb_rg_find(cdb_rg *g, uint64_t entry, int *found);
/* Loads the header of the group whose last entry is `last`. */
cdb_status cdb_rg_load(cdb_rg *g, uint64_t last, int *found);
/* Loads the entry IDs of g->hdr's group into g->entries. */
cdb_status cdb_rg_load_entries(cdb_rg *g);

/* Reads the row of entry as an encoded object of its present columns.
 * *found 0 if no group holds it. CDB_E_BUFFER_TOO_SMALL with *len set. */
cdb_status cdb_rg_get_row(cdb_rg *g, uint64_t entry, void *out, size_t cap, size_t *len, int *found);

/* Opens column i of g->hdr (-1: the entry-ID chunk) for reading. Only one
 * column of a cdb_rg is open at a time; it uses g->chunk and g->enc. */
cdb_status cdb_rg_column_open(cdb_rg *g, int i, cdb_rg_column *c);
/* The next row's value, written to dst as an encoded value (tag and
 * payload), *vlen bytes; *present 0 for null. *done after the last row,
 * once the chunk's closing rules have been checked. */
cdb_status cdb_rg_column_next(cdb_rg_column *c, uint8_t *dst, size_t dcap, size_t *vlen, int *present,
                              int *done);

/* Frees the chunks of the group whose last entry is `last` and removes it. */
cdb_status cdb_rg_drop(cdb_rg *g, uint64_t last);

/* A chunk nibblet of the group moved (cdb_nib_merge): the header points at
 * its new address. CDB_E_NOT_FOUND if no group refers to it. */
cdb_status cdb_rg_nib_moved(cdb_rg *g, const uint8_t *okey, size_t oklen, uint64_t old_addr, uint64_t addr);

#endif
