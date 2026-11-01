/* Expression evaluation engine and cross-model field resolution (spec 11.10).
 *
 * Evaluates binary-wire expressions (CDB_QUERY_EXPR_*) over records across
 * any model: table rows, document objects, time-series points, or vector
 * entries. Supports:
 *   - Tri-state boolean logic (TRUE, FALSE, UNKNOWN/NULL).
 *   - Unified cross-model field resolution (nested paths with 0x1F delimiter,
 *     pseudo-fields $entry_id, $score, $timestamp, $vector).
 *   - Cross-model type coercion and comparisons.
 *   - Arithmetic (+, -, *, /, %) with divide-by-zero protection.
 *   - Parameter bindings (indexed and named).
 *   - Execution sandboxing: recursion depth limits and instruction step budgets.
 *
 * Nothing here allocates on the heap. Caller provides scratch memory for
 * computed values (e.g. concatenated strings) and context buffers. */
#ifndef CDB_EVAL_H
#define CDB_EVAL_H

#include <stddef.h>
#include <stdint.h>

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"
#include "cdb_catalog.h"
#include "cdb_query.h"

#define CDB_EVAL_MAX_DEPTH 32u /* recursion depth limit */

/* Tri-state boolean values for predicate evaluation and logic. */
typedef enum cdb_tristate {
    CDB_TRI_FALSE = 0,
    CDB_TRI_TRUE = 1,
    CDB_TRI_UNKNOWN = 2 /* NULL */
} cdb_tristate;

/* One query parameter binding. */
typedef struct cdb_eval_param {
    const char *name;      /* NULL if index-only */
    size_t name_len;
    uint32_t index;        /* 0-based index */
    cdb_value val;
} cdb_eval_param;

/* Custom field resolver hook for non-standard record shapes. */
typedef cdb_status (*cdb_field_resolver_fn)(void *user_data,
                                            const char *path,
                                            size_t path_len,
                                            cdb_value *out_val,
                                            int *found);

/* Evaluation context for expressions. */
typedef struct cdb_eval_ctx {
    cdb_byte_order order;
    cdb_catalog *cat;      /* catalog for field dictionary lookup; optional */
    uint64_t store_id;     /* catalog store ID for field lookup */

    /* Active entry attributes */
    uint64_t entry_id;
    int has_entry_id;

    double score;
    int has_score;

    int64_t timestamp;
    cdb_ts_precision ts_precision;
    int has_timestamp;

    cdb_value vector;
    int has_vector;

    /* Encoded record payload (spec 14.6 object) */
    const uint8_t *record_data;
    size_t record_len;

    /* Parameters */
    const cdb_eval_param *params;
    size_t n_params;

    /* Custom resolver hook */
    cdb_field_resolver_fn custom_resolver;
    void *custom_data;

    /* Caller-provided scratch memory for intermediate/computed values */
    uint8_t *scratch;
    size_t scratch_cap;
    size_t scratch_used;

    /* Budget and execution limits */
    uint32_t depth;
    uint32_t max_depth;
    uint64_t steps_remaining;
    int budget_enabled;

    /* Current wire bound for sub-expression evaluation */
    const uint8_t *wire_end;
} cdb_eval_ctx;

/* Initializes an evaluation context with default limits. */
void cdb_eval_ctx_init(cdb_eval_ctx *ctx, cdb_byte_order order, cdb_catalog *cat, uint64_t store_id);

/* Sets caller scratch buffer for dynamic allocations (e.g. string concatenation). */
void cdb_eval_ctx_set_scratch(cdb_eval_ctx *ctx, uint8_t *buf, size_t cap);

/* Clears used scratch space so caller can reuse buffer across rows. */
void cdb_eval_ctx_reset_scratch(cdb_eval_ctx *ctx);

/* Binds parameters array. */
void cdb_eval_ctx_set_params(cdb_eval_ctx *ctx, const cdb_eval_param *params, size_t n_params);

/* Binds active record data (encoded object). */
void cdb_eval_ctx_set_record(cdb_eval_ctx *ctx, const uint8_t *data, size_t len);

/* Sets pseudo-field attributes for active entry. */
void cdb_eval_ctx_set_entry_id(cdb_eval_ctx *ctx, uint64_t entry_id);
void cdb_eval_ctx_set_score(cdb_eval_ctx *ctx, double score);
void cdb_eval_ctx_set_timestamp(cdb_eval_ctx *ctx, int64_t ts, cdb_ts_precision prec);
void cdb_eval_ctx_set_vector(cdb_eval_ctx *ctx, const cdb_value *vec);

/* Sets execution step budget limit. */
void cdb_eval_ctx_set_budget(cdb_eval_ctx *ctx, uint64_t step_limit);

/* Sets custom field resolver hook. */
void cdb_eval_ctx_set_custom_resolver(cdb_eval_ctx *ctx, cdb_field_resolver_fn fn, void *user_data);

/* Resolves a field path (may contain 0x1F unit separators for nested objects).
 * Returns CDB_OK with *found = 1 if present; *found = 0 and out_val set to NULL if absent. */
cdb_status cdb_eval_resolve_field(cdb_eval_ctx *ctx, const char *path, size_t path_len,
                                  cdb_value *out_val, int *found);

/* Evaluates an expression directly from wire format bytes.
 * *consumed receives the number of bytes read from wire. */
cdb_status cdb_eval_expr_wire(cdb_eval_ctx *ctx, const uint8_t *wire, size_t len,
                             cdb_value *out_val, size_t *consumed);

/* Evaluates a decoded cdb_query_expr structure. */
cdb_status cdb_eval_expr(cdb_eval_ctx *ctx, const cdb_query_expr *expr, cdb_value *out_val);

/* Evaluates an expression wire slice as a filter predicate.
 * *matched is 1 if truthy (TRUE), 0 if falsy (FALSE or NULL/UNKNOWN). */
cdb_status cdb_eval_predicate(cdb_eval_ctx *ctx, const uint8_t *wire, size_t len, int *matched);

/* Compares two values with cross-model type coercion.
 * If either value is NULL, *is_null is set to 1 and *cmp is 0.
 * Otherwise *is_null is 0, and *cmp is negative if left < right, 0 if equal, positive if left > right. */
cdb_status cdb_eval_compare(const cdb_value *left, const cdb_value *right, int *cmp, int *is_null);

/* Value constructor and helper utilities. */
int cdb_eval_is_truthy(const cdb_value *v);
int cdb_eval_is_null(const cdb_value *v);
void cdb_eval_make_null(cdb_value *out);
void cdb_eval_make_bool(cdb_value *out, int b);
void cdb_eval_make_int(cdb_value *out, int64_t i);
void cdb_eval_make_f64(cdb_value *out, double d);

#endif /* CDB_EVAL_H */
