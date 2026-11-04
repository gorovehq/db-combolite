/* Streaming query plan executor and cursor engine (CMBQL core).
 * Spec 11.5, 11.10; Spec 14.19.
 *
 * Implements Volcano-style streaming iterator cursors for the 15 CMBQL
 * operator nodes: Scan, Filter, Project, Join, Sort, Limit, Union,
 * Aggregate, Window, AsofJoin, Traverse, TextSearch, VectorSearch,
 * HybridRank, and Unnest.
 *
 * Runs under strict execution step and memory budgets without heap allocation.
 */
#ifndef CDB_EXEC_H
#define CDB_EXEC_H

#include <stddef.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"
#include "cdb_catalog.h"
#include "cdb_db.h"
#include "cdb_eval.h"
#include "cdb_profile.h"
#include "cdb_query.h"

#define CDB_EXEC_MAX_FIELDS 16u
#define CDB_EXEC_MAX_BRANCHES 8u
#define CDB_EXEC_MAX_SORT_KEYS 8u
#define CDB_EXEC_MAX_AGGS 16u
#define CDB_EXEC_DEFAULT_BUFFERED_ROWS 256u
/* A live Scan's record buffer, in pages, taken from the cursor's scratch */
#define CDB_EXEC_SCAN_RECORD_PAGES 4u

/* Forward declarations */
typedef struct cdb_exec_op cdb_exec_op;
typedef struct cdb_exec_cursor cdb_exec_cursor;

/* One named field in an executed row */
typedef struct cdb_exec_field {
    const char *name;
    size_t name_len;
    cdb_value val;
} cdb_exec_field;

/* Unified tuple emitted by query plan operators */
typedef struct cdb_exec_row {
    uint64_t store_id;
    uint64_t entry_id;
    uint32_t model;
    float score;
    int64_t timestamp;
    const float *vector;
    size_t vector_dim;

    /* Raw underlying record data */
    const uint8_t *record_data;
    size_t record_len;

    /* Secondary context for Joins */
    int has_right;
    uint64_t right_store_id;
    uint64_t right_entry_id;
    uint32_t right_model;
    const uint8_t *right_record_data;
    size_t right_record_len;

    /* Explicit/computed fields (Project, Aggregate, Unnest, Join). Last, so
     * a buffered row is stored with only the fields it has
     * (CDB_EXEC_ROW_SIZE). */
    size_t n_fields;
    cdb_exec_field fields[CDB_EXEC_MAX_FIELDS];
} cdb_exec_row;

/* Bytes a row with n fields needs when stored compactly */
#define CDB_EXEC_ROW_SIZE(n) (offsetof(cdb_exec_row, fields) + (n) * sizeof(cdb_exec_field))

/* Virtual function table for an operator node iterator */
typedef struct cdb_exec_vtab {
    cdb_status (*open)(cdb_exec_op *op, cdb_exec_cursor *cur);
    cdb_status (*next)(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row);
    cdb_status (*reset)(cdb_exec_op *op, cdb_exec_cursor *cur);
    void (*close)(cdb_exec_op *op, cdb_exec_cursor *cur);
} cdb_exec_vtab;

/* Base struct for an operator node instance in the plan tree */
struct cdb_exec_op {
    const cdb_exec_vtab *vtab;
    uint8_t tag; /* CDB_QUERY_NODE_* */
};

/* Linear memory arena for operator allocations within caller scratch buffer */
typedef struct cdb_exec_arena {
    uint8_t *buf;
    size_t cap;
    size_t used;
} cdb_exec_arena;

void *cdb_exec_arena_alloc(cdb_exec_arena *arena, size_t size, size_t align);

/* Streaming execution cursor */
struct cdb_exec_cursor {
    cdb_db *db;
    cdb_catalog *cat;
    cdb_byte_order order;

    /* Parameters */
    const cdb_eval_param *params;
    size_t n_params;

    /* Memory arena */
    cdb_exec_arena arena;

    /* Expression evaluator context */
    cdb_eval_ctx eval_ctx;
    uint8_t eval_scratch[2048];

    /* Root operator */
    cdb_exec_op *root_op;

    /* Limits */
    uint64_t step_budget;
    size_t max_buffered_rows;
    int is_open;

    /* Host profile (spec 17.4): memory grown through the environment's
     * allocator, at most mem_budget bytes (0: none can grow), each block on
     * a list freed by its owner or at close */
    size_t mem_budget;
    size_t mem_used;
    struct cdb_exec_block *blocks; /* the cursor's own: scan buffers */
};

/* The sizes of the structs a binding mirrors, to check its copy against */
size_t cdb_exec_cursor_size(void);
size_t cdb_exec_row_size(void);

/* Opens a query cursor from wire format bytes */
cdb_status cdb_exec_open(cdb_exec_cursor *cur, cdb_db *db, const uint8_t *wire, size_t len,
                         const cdb_eval_param *params, size_t n_params,
                         uint8_t *scratch, size_t scratch_cap);

/* Fetches the next row from the query cursor.
 * Returns CDB_OK with *has_row = 1 if a row was produced;
 * returns CDB_OK with *has_row = 0 when the stream reaches EOF. */
cdb_status cdb_exec_next(cdb_exec_cursor *cur, cdb_exec_row *row, int *has_row);

/* Resets the cursor to the beginning of the stream */
cdb_status cdb_exec_reset(cdb_exec_cursor *cur);

/* Closes the cursor and releases all operator resources */
cdb_status cdb_exec_close(cdb_exec_cursor *cur);

/* Looks up a field value by name or path in an executed row */
cdb_status cdb_exec_row_get_field(const cdb_exec_row *row, cdb_eval_ctx *eval_ctx,
                                  const char *name, size_t name_len,
                                  cdb_value *out_val, int *found);

/* Helper to copy an executed row into target buffer */
void cdb_exec_row_copy(cdb_exec_row *dst, const cdb_exec_row *src);

/* Binds an array of mock rows to a named store scan (for testing and pipeline validation) */
cdb_status cdb_exec_set_mock_scan(cdb_exec_cursor *cur, const char *store_name, size_t name_len,
                                  const cdb_exec_row *rows, size_t n_rows);

#endif /* CDB_EXEC_H */
