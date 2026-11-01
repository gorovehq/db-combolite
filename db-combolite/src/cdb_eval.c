/* Expression evaluation engine and cross-model field resolution (spec 11.10). */
#include "cdb_eval.h"
#include "cdb_varint.h"
#include "cdb_math.h"

#include <string.h>

static int64_t ts_to_nanos(cdb_ts_precision p, int64_t ticks)
{
    switch (p) {
    case CDB_TS_SECONDS:
        return ticks * 1000000000LL;
    case CDB_TS_MILLIS:
        return ticks * 1000000LL;
    case CDB_TS_MICROS:
        return ticks * 1000LL;
    case CDB_TS_NANOS:
        return ticks;
    }
    return ticks;
}

static void *eval_scratch_alloc(cdb_eval_ctx *ctx, size_t size)
{
    void *ptr;
    if (ctx == NULL || ctx->scratch == NULL)
        return NULL;
    if (ctx->scratch_cap - ctx->scratch_used < size)
        return NULL;
    ptr = ctx->scratch + ctx->scratch_used;
    ctx->scratch_used += size;
    return ptr;
}

static double to_double(const cdb_value *v)
{
    if (v == NULL)
        return 0.0;
    switch (v->type) {
    case CDB_TYPE_INT:
        return (double)v->as.i64;
    case CDB_TYPE_F64:
        return v->as.f64;
    case CDB_TYPE_F32:
        return (double)v->as.f32;
    case CDB_TYPE_BOOL:
        return (double)v->as.boolean;
    default:
        return 0.0;
    }
}

static cdb_status step_budget(cdb_eval_ctx *ctx)
{
    if (ctx->budget_enabled) {
        if (ctx->steps_remaining == 0)
            return CDB_E_OUT_OF_BUDGET;
        ctx->steps_remaining--;
    }
    return CDB_OK;
}

void cdb_eval_ctx_init(cdb_eval_ctx *ctx, cdb_byte_order order, cdb_catalog *cat, uint64_t store_id)
{
    if (ctx == NULL)
        return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->order = order;
    ctx->cat = cat;
    ctx->store_id = store_id;
    ctx->max_depth = CDB_EVAL_MAX_DEPTH;
}

void cdb_eval_ctx_set_scratch(cdb_eval_ctx *ctx, uint8_t *buf, size_t cap)
{
    if (ctx == NULL)
        return;
    ctx->scratch = buf;
    ctx->scratch_cap = cap;
    ctx->scratch_used = 0;
}

void cdb_eval_ctx_reset_scratch(cdb_eval_ctx *ctx)
{
    if (ctx == NULL)
        return;
    ctx->scratch_used = 0;
}

void cdb_eval_ctx_set_params(cdb_eval_ctx *ctx, const cdb_eval_param *params, size_t n_params)
{
    if (ctx == NULL)
        return;
    ctx->params = params;
    ctx->n_params = n_params;
}

void cdb_eval_ctx_set_record(cdb_eval_ctx *ctx, const uint8_t *data, size_t len)
{
    if (ctx == NULL)
        return;
    ctx->record_data = data;
    ctx->record_len = len;
}

void cdb_eval_ctx_set_entry_id(cdb_eval_ctx *ctx, uint64_t entry_id)
{
    if (ctx == NULL)
        return;
    ctx->entry_id = entry_id;
    ctx->has_entry_id = 1;
}

void cdb_eval_ctx_set_score(cdb_eval_ctx *ctx, double score)
{
    if (ctx == NULL)
        return;
    ctx->score = score;
    ctx->has_score = 1;
}

void cdb_eval_ctx_set_timestamp(cdb_eval_ctx *ctx, int64_t ts, cdb_ts_precision prec)
{
    if (ctx == NULL)
        return;
    ctx->timestamp = ts;
    ctx->ts_precision = prec;
    ctx->has_timestamp = 1;
}

void cdb_eval_ctx_set_vector(cdb_eval_ctx *ctx, const cdb_value *vec)
{
    if (ctx == NULL)
        return;
    if (vec != NULL) {
        ctx->vector = *vec;
        ctx->has_vector = 1;
    } else {
        ctx->has_vector = 0;
    }
}

void cdb_eval_ctx_set_budget(cdb_eval_ctx *ctx, uint64_t step_limit)
{
    if (ctx == NULL)
        return;
    ctx->budget_enabled = 1;
    ctx->steps_remaining = step_limit;
}

void cdb_eval_ctx_set_custom_resolver(cdb_eval_ctx *ctx, cdb_field_resolver_fn fn, void *user_data)
{
    if (ctx == NULL)
        return;
    ctx->custom_resolver = fn;
    ctx->custom_data = user_data;
}

void cdb_eval_make_null(cdb_value *out)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->type = CDB_TYPE_NULL;
    out->tag = CDB_TAG_NULL;
    out->encoded_len = 1;
}

void cdb_eval_make_bool(cdb_value *out, int b)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->type = CDB_TYPE_BOOL;
    out->tag = b ? CDB_TAG_TRUE : CDB_TAG_FALSE;
    out->as.boolean = b ? 1 : 0;
    out->encoded_len = 1;
}

void cdb_eval_make_int(cdb_value *out, int64_t i)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->type = CDB_TYPE_INT;
    out->tag = CDB_TAG_INT;
    out->as.i64 = i;
    out->encoded_len = 1 + cdb_varint_size(cdb_zigzag_encode(i));
}

void cdb_eval_make_f64(cdb_value *out, double d)
{
    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->type = CDB_TYPE_F64;
    out->tag = CDB_TAG_F64;
    out->as.f64 = d;
    out->encoded_len = 9;
}

int cdb_eval_is_null(const cdb_value *v)
{
    return v == NULL || v->type == CDB_TYPE_NULL;
}

int cdb_eval_is_truthy(const cdb_value *v)
{
    if (v == NULL)
        return 0;
    switch (v->type) {
    case CDB_TYPE_BOOL:
        return v->as.boolean != 0;
    case CDB_TYPE_INT:
        return v->as.i64 != 0;
    case CDB_TYPE_F64:
        return v->as.f64 != 0.0;
    case CDB_TYPE_F32:
        return v->as.f32 != 0.0f;
    case CDB_TYPE_STRING:
    case CDB_TYPE_BINARY:
        return v->payload_len > 0;
    default:
        return 0;
    }
}

static int parse_digits_u64(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 1;
}

cdb_status cdb_eval_resolve_field(cdb_eval_ctx *ctx, const char *path, size_t path_len,
                                  cdb_value *out_val, int *found)
{
    const char *seg;
    const char *path_end;
    cdb_value curr_obj;
    cdb_status s;

    if (ctx == NULL || path == NULL || out_val == NULL || found == NULL)
        return CDB_E_INVALID_ARG;

    *found = 0;
    cdb_eval_make_null(out_val);

    if (ctx->custom_resolver != NULL)
        return ctx->custom_resolver(ctx->custom_data, path, path_len, out_val, found);

    /* Check pseudo-fields */
    if (path_len > 0 && path[0] == '$') {
        if ((path_len == 3 && memcmp(path, "$id", 3) == 0)
            || (path_len == 9 && memcmp(path, "$entry_id", 9) == 0)) {
            if (ctx->has_entry_id) {
                cdb_eval_make_int(out_val, (int64_t)ctx->entry_id);
                *found = 1;
            }
            return CDB_OK;
        }
        if (path_len == 6 && memcmp(path, "$score", 6) == 0) {
            if (ctx->has_score) {
                cdb_eval_make_f64(out_val, ctx->score);
                *found = 1;
            }
            return CDB_OK;
        }
        if (path_len == 10 && memcmp(path, "$timestamp", 10) == 0) {
            if (ctx->has_timestamp) {
                out_val->type = CDB_TYPE_TIMESTAMP;
                out_val->tag = CDB_TAG_TIMESTAMP;
                out_val->as.ts.precision = ctx->ts_precision;
                out_val->as.ts.ticks = ctx->timestamp;
                out_val->encoded_len = 9;
                *found = 1;
            }
            return CDB_OK;
        }
        if (path_len == 7 && memcmp(path, "$vector", 7) == 0) {
            if (ctx->has_vector) {
                *out_val = ctx->vector;
                *found = 1;
            }
            return CDB_OK;
        }
    }

    if (ctx->record_data == NULL || ctx->record_len == 0)
        return CDB_OK;

    if ((s = cdb_value_read(ctx->record_data, ctx->record_len, ctx->order, &curr_obj)) != CDB_OK)
        return s;
    if (curr_obj.type != CDB_TYPE_OBJECT)
        return CDB_OK;

    seg = path;
    path_end = path + path_len;

    while (seg < path_end) {
        const char *next_sep = seg;
        size_t seg_len;
        uint64_t field_id = 0;
        int fid_found = 0;
        cdb_value val;
        int val_found = 0;

        while (next_sep < path_end && *next_sep != '\x1f')
            next_sep++;

        seg_len = (size_t)(next_sep - seg);

        if (ctx->cat != NULL) {
            if ((s = cdb_catalog_field_lookup(ctx->cat, ctx->store_id, seg, seg_len, &field_id, &fid_found)) != CDB_OK)
                return s;
        } else {
            /* Support direct numeric IDs when catalog is absent (e.g. testing) */
            fid_found = parse_digits_u64(seg, seg_len, &field_id);
        }

        if (!fid_found)
            return CDB_OK;

        if ((s = cdb_value_object_get(&curr_obj, ctx->order, field_id, &val, &val_found)) != CDB_OK)
            return s;

        if (!val_found)
            return CDB_OK;

        if (next_sep < path_end) {
            /* Intermediate path segment must be an object */
            if (val.type != CDB_TYPE_OBJECT)
                return CDB_OK;
            curr_obj = val;
            seg = next_sep + 1;
        } else {
            /* Final segment reached */
            *out_val = val;
            *found = 1;
            return CDB_OK;
        }
    }

    return CDB_OK;
}

cdb_status cdb_eval_compare(const cdb_value *left, const cdb_value *right, int *cmp, int *is_null)
{
    if (left == NULL || right == NULL || cmp == NULL || is_null == NULL)
        return CDB_E_INVALID_ARG;

    if (left->type == CDB_TYPE_NULL || right->type == CDB_TYPE_NULL) {
        *is_null = 1;
        *cmp = 0;
        return CDB_OK;
    }

    *is_null = 0;

    /* Numbers: INT, F64, F32 */
    if ((left->type == CDB_TYPE_INT || left->type == CDB_TYPE_F64 || left->type == CDB_TYPE_F32)
        && (right->type == CDB_TYPE_INT || right->type == CDB_TYPE_F64 || right->type == CDB_TYPE_F32)) {
        if (left->type == CDB_TYPE_INT && right->type == CDB_TYPE_INT) {
            if (left->as.i64 < right->as.i64)
                *cmp = -1;
            else if (left->as.i64 > right->as.i64)
                *cmp = 1;
            else
                *cmp = 0;
            return CDB_OK;
        }
        /* At least one float/double: convert to double */
        {
            double l = (left->type == CDB_TYPE_INT) ? (double)left->as.i64
                     : ((left->type == CDB_TYPE_F64) ? left->as.f64 : (double)left->as.f32);
            double r = (right->type == CDB_TYPE_INT) ? (double)right->as.i64
                     : ((right->type == CDB_TYPE_F64) ? right->as.f64 : (double)right->as.f32);
            if (l < r)
                *cmp = -1;
            else if (l > r)
                *cmp = 1;
            else
                *cmp = 0;
            return CDB_OK;
        }
    }

    /* Booleans */
    if (left->type == CDB_TYPE_BOOL && right->type == CDB_TYPE_BOOL) {
        int l = left->as.boolean != 0;
        int r = right->as.boolean != 0;
        *cmp = l - r;
        return CDB_OK;
    }

    /* Strings */
    if (left->type == CDB_TYPE_STRING && right->type == CDB_TYPE_STRING) {
        size_t min_len = left->payload_len < right->payload_len ? left->payload_len : right->payload_len;
        int res = min_len > 0 ? memcmp(left->payload, right->payload, min_len) : 0;
        if (res != 0)
            *cmp = res < 0 ? -1 : 1;
        else if (left->payload_len < right->payload_len)
            *cmp = -1;
        else if (left->payload_len > right->payload_len)
            *cmp = 1;
        else
            *cmp = 0;
        return CDB_OK;
    }

    /* Binary */
    if (left->type == CDB_TYPE_BINARY && right->type == CDB_TYPE_BINARY) {
        size_t min_len = left->payload_len < right->payload_len ? left->payload_len : right->payload_len;
        int res = min_len > 0 ? memcmp(left->payload, right->payload, min_len) : 0;
        if (res != 0)
            *cmp = res < 0 ? -1 : 1;
        else if (left->payload_len < right->payload_len)
            *cmp = -1;
        else if (left->payload_len > right->payload_len)
            *cmp = 1;
        else
            *cmp = 0;
        return CDB_OK;
    }

    /* Timestamps */
    if (left->type == CDB_TYPE_TIMESTAMP && right->type == CDB_TYPE_TIMESTAMP) {
        int64_t l_ns = ts_to_nanos(left->as.ts.precision, left->as.ts.ticks);
        int64_t r_ns = ts_to_nanos(right->as.ts.precision, right->as.ts.ticks);
        if (l_ns < r_ns)
            *cmp = -1;
        else if (l_ns > r_ns)
            *cmp = 1;
        else
            *cmp = 0;
        return CDB_OK;
    }

    /* Incompatible types: order deterministically by type tag */
    *cmp = (int)left->type - (int)right->type;
    return CDB_OK;
}

static int str_like(const uint8_t *str, size_t slen, const uint8_t *pat, size_t plen)
{
    while (plen > 0) {
        if (*pat == '%') {
            pat++;
            plen--;
            if (plen == 0)
                return 1;
            while (slen > 0) {
                if (str_like(str, slen, pat, plen))
                    return 1;
                str++;
                slen--;
            }
            return 0;
        } else if (*pat == '_') {
            if (slen == 0)
                return 0;
            str++;
            slen--;
            pat++;
            plen--;
        } else {
            if (slen == 0 || *str != *pat)
                return 0;
            str++;
            slen--;
            pat++;
            plen--;
        }
    }
    return slen == 0;
}

static int str_contains(const uint8_t *str, size_t slen, const uint8_t *sub, size_t sublen)
{
    size_t i;
    if (sublen == 0)
        return 1;
    if (slen < sublen)
        return 0;
    for (i = 0; i <= slen - sublen; i++) {
        if (memcmp(str + i, sub, sublen) == 0)
            return 1;
    }
    return 0;
}

cdb_status cdb_eval_expr(cdb_eval_ctx *ctx, const cdb_query_expr *expr, cdb_value *out_val)
{
    cdb_status s;

    if (ctx == NULL || expr == NULL || out_val == NULL)
        return CDB_E_INVALID_ARG;

    if ((s = step_budget(ctx)) != CDB_OK)
        return s;

    if (ctx->depth >= ctx->max_depth)
        return CDB_E_DEPTH;

    ctx->depth++;

    switch (expr->tag) {
    case CDB_QUERY_EXPR_LITERAL: {
        s = cdb_value_read(expr->as.literal.data, expr->as.literal.len, ctx->order, out_val);
        break;
    }

    case CDB_QUERY_EXPR_FIELD: {
        int found = 0;
        s = cdb_eval_resolve_field(ctx, expr->as.field.path, expr->as.field.len, out_val, &found);
        break;
    }

    case CDB_QUERY_EXPR_PARAM: {
        size_t i;
        int found = 0;
        for (i = 0; i < ctx->n_params; i++) {
            const cdb_eval_param *p = &ctx->params[i];
            if (expr->as.param.kind == CDB_PARAM_INDEX) {
                if (p->index == expr->as.param.index) {
                    *out_val = p->val;
                    found = 1;
                    break;
                }
            } else if (expr->as.param.kind == CDB_PARAM_NAMED) {
                if (p->name != NULL && p->name_len == expr->as.param.name_len
                    && memcmp(p->name, expr->as.param.name, p->name_len) == 0) {
                    *out_val = p->val;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            cdb_eval_make_null(out_val);
            s = CDB_E_NOT_FOUND;
        } else {
            s = CDB_OK;
        }
        break;
    }

    case CDB_QUERY_EXPR_UNARY: {
        cdb_value op_val;
        size_t consumed = 0;
        size_t rem = (ctx->wire_end != NULL && ctx->wire_end > expr->as.unary.operand_wire)
                     ? (size_t)(ctx->wire_end - expr->as.unary.operand_wire) : 0;
        s = cdb_eval_expr_wire(ctx, expr->as.unary.operand_wire, rem, &op_val, &consumed);
        if (s != CDB_OK)
            break;

        switch (expr->as.unary.op) {
        case CDB_UNARY_NOT:
            if (cdb_eval_is_null(&op_val))
                cdb_eval_make_null(out_val);
            else
                cdb_eval_make_bool(out_val, !cdb_eval_is_truthy(&op_val));
            s = CDB_OK;
            break;

        case CDB_UNARY_IS_NULL:
            cdb_eval_make_bool(out_val, cdb_eval_is_null(&op_val));
            s = CDB_OK;
            break;

        case CDB_UNARY_IS_NOT_NULL:
            cdb_eval_make_bool(out_val, !cdb_eval_is_null(&op_val));
            s = CDB_OK;
            break;

        case CDB_UNARY_NEG:
            if (cdb_eval_is_null(&op_val)) {
                cdb_eval_make_null(out_val);
                s = CDB_OK;
            } else if (op_val.type == CDB_TYPE_INT) {
                cdb_eval_make_int(out_val, -op_val.as.i64);
                s = CDB_OK;
            } else if (op_val.type == CDB_TYPE_F64) {
                cdb_eval_make_f64(out_val, -op_val.as.f64);
                s = CDB_OK;
            } else if (op_val.type == CDB_TYPE_F32) {
                cdb_eval_make_f64(out_val, -(double)op_val.as.f32);
                s = CDB_OK;
            } else {
                s = CDB_E_INVALID_ARG;
            }
            break;

        default:
            s = CDB_E_INVALID_ARG;
            break;
        }
        break;
    }

    case CDB_QUERY_EXPR_BINARY: {
        uint8_t op = expr->as.binary.op;
        cdb_value l_val, r_val;
        size_t l_cons = 0, r_cons = 0;
        size_t l_rem = (ctx->wire_end != NULL && ctx->wire_end > expr->as.binary.left_wire)
                       ? (size_t)(ctx->wire_end - expr->as.binary.left_wire) : 0;
        size_t r_rem = (ctx->wire_end != NULL && ctx->wire_end > expr->as.binary.right_wire)
                       ? (size_t)(ctx->wire_end - expr->as.binary.right_wire) : 0;

        /* Short-circuit evaluation for AND / OR */
        if (op == CDB_BIN_AND) {
            s = cdb_eval_expr_wire(ctx, expr->as.binary.left_wire, l_rem, &l_val, &l_cons);
            if (s != CDB_OK)
                break;
            if (!cdb_eval_is_null(&l_val) && !cdb_eval_is_truthy(&l_val)) {
                cdb_eval_make_bool(out_val, 0);
                s = CDB_OK;
                break;
            }
            s = cdb_eval_expr_wire(ctx, expr->as.binary.right_wire, r_rem, &r_val, &r_cons);
            if (s != CDB_OK)
                break;
            if (!cdb_eval_is_null(&r_val) && !cdb_eval_is_truthy(&r_val)) {
                cdb_eval_make_bool(out_val, 0);
            } else if (cdb_eval_is_null(&l_val) || cdb_eval_is_null(&r_val)) {
                cdb_eval_make_null(out_val);
            } else {
                cdb_eval_make_bool(out_val, 1);
            }
            s = CDB_OK;
            break;
        }

        if (op == CDB_BIN_OR) {
            s = cdb_eval_expr_wire(ctx, expr->as.binary.left_wire, l_rem, &l_val, &l_cons);
            if (s != CDB_OK)
                break;
            if (cdb_eval_is_truthy(&l_val)) {
                cdb_eval_make_bool(out_val, 1);
                s = CDB_OK;
                break;
            }
            s = cdb_eval_expr_wire(ctx, expr->as.binary.right_wire, r_rem, &r_val, &r_cons);
            if (s != CDB_OK)
                break;
            if (cdb_eval_is_truthy(&r_val)) {
                cdb_eval_make_bool(out_val, 1);
            } else if (cdb_eval_is_null(&l_val) || cdb_eval_is_null(&r_val)) {
                cdb_eval_make_null(out_val);
            } else {
                cdb_eval_make_bool(out_val, 0);
            }
            s = CDB_OK;
            break;
        }

        s = cdb_eval_expr_wire(ctx, expr->as.binary.left_wire, l_rem, &l_val, &l_cons);
        if (s != CDB_OK)
            break;
        s = cdb_eval_expr_wire(ctx, expr->as.binary.right_wire, r_rem, &r_val, &r_cons);
        if (s != CDB_OK)
            break;

        /* Comparisons */
        if (op >= CDB_BIN_EQ && op <= CDB_BIN_GE) {
            int cmp = 0;
            int is_null = 0;
            s = cdb_eval_compare(&l_val, &r_val, &cmp, &is_null);
            if (s != CDB_OK)
                break;
            if (is_null) {
                cdb_eval_make_null(out_val);
            } else {
                int matched = 0;
                switch (op) {
                case CDB_BIN_EQ: matched = (cmp == 0); break;
                case CDB_BIN_NE: matched = (cmp != 0); break;
                case CDB_BIN_LT: matched = (cmp < 0); break;
                case CDB_BIN_LE: matched = (cmp <= 0); break;
                case CDB_BIN_GT: matched = (cmp > 0); break;
                case CDB_BIN_GE: matched = (cmp >= 0); break;
                default: break;
                }
                cdb_eval_make_bool(out_val, matched);
            }
            s = CDB_OK;
            break;
        }

        /* Arithmetic */
        if (op >= CDB_BIN_ADD && op <= CDB_BIN_MOD) {
            if (cdb_eval_is_null(&l_val) || cdb_eval_is_null(&r_val)) {
                cdb_eval_make_null(out_val);
                s = CDB_OK;
                break;
            }

            /* String concatenation */
            if (op == CDB_BIN_ADD && l_val.type == CDB_TYPE_STRING && r_val.type == CDB_TYPE_STRING) {
                size_t total_len = l_val.payload_len + r_val.payload_len;
                uint8_t *buf = (uint8_t *)eval_scratch_alloc(ctx, total_len);
                if (buf == NULL && total_len > 0) {
                    s = CDB_E_BUFFER_TOO_SMALL;
                    break;
                }
                if (l_val.payload_len > 0)
                    memcpy(buf, l_val.payload, l_val.payload_len);
                if (r_val.payload_len > 0)
                    memcpy(buf + l_val.payload_len, r_val.payload, r_val.payload_len);

                memset(out_val, 0, sizeof(*out_val));
                out_val->type = CDB_TYPE_STRING;
                out_val->tag = CDB_TAG_STRING;
                out_val->payload = buf;
                out_val->payload_len = total_len;
                s = CDB_OK;
                break;
            }

            /* Numeric operations */
            if (l_val.type == CDB_TYPE_INT && r_val.type == CDB_TYPE_INT) {
                int64_t a = l_val.as.i64;
                int64_t b = r_val.as.i64;
                switch (op) {
                case CDB_BIN_ADD: cdb_eval_make_int(out_val, a + b); break;
                case CDB_BIN_SUB: cdb_eval_make_int(out_val, a - b); break;
                case CDB_BIN_MUL: cdb_eval_make_int(out_val, a * b); break;
                case CDB_BIN_DIV:
                    if (b == 0)
                        cdb_eval_make_null(out_val);
                    else
                        cdb_eval_make_int(out_val, a / b);
                    break;
                case CDB_BIN_MOD:
                    if (b == 0)
                        cdb_eval_make_null(out_val);
                    else
                        cdb_eval_make_int(out_val, a % b);
                    break;
                default: break;
                }
                s = CDB_OK;
                break;
            }

            /* Float operations */
            {
                double a = (l_val.type == CDB_TYPE_INT) ? (double)l_val.as.i64
                         : ((l_val.type == CDB_TYPE_F64) ? l_val.as.f64 : (double)l_val.as.f32);
                double b = (r_val.type == CDB_TYPE_INT) ? (double)r_val.as.i64
                         : ((r_val.type == CDB_TYPE_F64) ? r_val.as.f64 : (double)r_val.as.f32);
                switch (op) {
                case CDB_BIN_ADD: cdb_eval_make_f64(out_val, a + b); break;
                case CDB_BIN_SUB: cdb_eval_make_f64(out_val, a - b); break;
                case CDB_BIN_MUL: cdb_eval_make_f64(out_val, a * b); break;
                case CDB_BIN_DIV:
                    if (b == 0.0)
                        cdb_eval_make_null(out_val);
                    else
                        cdb_eval_make_f64(out_val, a / b);
                    break;
                case CDB_BIN_MOD:
                    cdb_eval_make_null(out_val);
                    break;
                default: break;
                }
                s = CDB_OK;
            }
            break;
        }

        /* String functions: LIKE and CONTAINS */
        if (op == CDB_BIN_LIKE) {
            if (cdb_eval_is_null(&l_val) || cdb_eval_is_null(&r_val)) {
                cdb_eval_make_null(out_val);
            } else if (l_val.type == CDB_TYPE_STRING && r_val.type == CDB_TYPE_STRING) {
                int matched = str_like(l_val.payload, l_val.payload_len, r_val.payload, r_val.payload_len);
                cdb_eval_make_bool(out_val, matched);
            } else {
                cdb_eval_make_bool(out_val, 0);
            }
            s = CDB_OK;
            break;
        }

        if (op == CDB_BIN_CONTAINS) {
            if (cdb_eval_is_null(&l_val) || cdb_eval_is_null(&r_val)) {
                cdb_eval_make_null(out_val);
            } else if (l_val.type == CDB_TYPE_STRING && r_val.type == CDB_TYPE_STRING) {
                int matched = str_contains(l_val.payload, l_val.payload_len, r_val.payload, r_val.payload_len);
                cdb_eval_make_bool(out_val, matched);
            } else {
                cdb_eval_make_bool(out_val, 0);
            }
            s = CDB_OK;
            break;
        }

        s = CDB_E_INVALID_ARG;
        break;
    }

    case CDB_QUERY_EXPR_CALL: {
        uint16_t fn_id = expr->as.call.fn_id;
        uint16_t n_args = expr->as.call.n_args;
        cdb_value args[8];
        const uint8_t *ap = expr->as.call.args_wire;
        size_t i;
        int has_null = 0;

        if (n_args > 8)
            return CDB_E_UNSUPPORTED;

        for (i = 0; i < n_args; i++) {
            size_t a_rem = (ctx->wire_end != NULL && ctx->wire_end > ap)
                           ? (size_t)(ctx->wire_end - ap) : 0;
            size_t a_cons = 0;
            s = cdb_eval_expr_wire(ctx, ap, a_rem, &args[i], &a_cons);
            if (s != CDB_OK)
                break;
            if (cdb_eval_is_null(&args[i]))
                has_null = 1;
            ap += a_cons;
        }
        if (s != CDB_OK)
            break;

        if (has_null) {
            cdb_eval_make_null(out_val);
            s = CDB_OK;
            break;
        }

        switch (fn_id) {
        /* Trigonometry */
        case CDB_FN_SIN: cdb_eval_make_f64(out_val, cdb_math_sin(to_double(&args[0]))); break;
        case CDB_FN_COS: cdb_eval_make_f64(out_val, cdb_math_cos(to_double(&args[0]))); break;
        case CDB_FN_TAN: cdb_eval_make_f64(out_val, cdb_math_tan(to_double(&args[0]))); break;
        case CDB_FN_ASIN: cdb_eval_make_f64(out_val, cdb_math_asin(to_double(&args[0]))); break;
        case CDB_FN_ACOS: cdb_eval_make_f64(out_val, cdb_math_acos(to_double(&args[0]))); break;
        case CDB_FN_ATAN: cdb_eval_make_f64(out_val, cdb_math_atan(to_double(&args[0]))); break;
        case CDB_FN_ATAN2: cdb_eval_make_f64(out_val, cdb_math_atan2(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_SINH: cdb_eval_make_f64(out_val, cdb_math_sinh(to_double(&args[0]))); break;
        case CDB_FN_COSH: cdb_eval_make_f64(out_val, cdb_math_cosh(to_double(&args[0]))); break;
        case CDB_FN_TANH: cdb_eval_make_f64(out_val, cdb_math_tanh(to_double(&args[0]))); break;
        case CDB_FN_DEGREES: cdb_eval_make_f64(out_val, cdb_math_degrees(to_double(&args[0]))); break;
        case CDB_FN_RADIANS: cdb_eval_make_f64(out_val, cdb_math_radians(to_double(&args[0]))); break;

        /* Geometry & Navigation */
        case CDB_FN_HYPOT: cdb_eval_make_f64(out_val, cdb_math_hypot(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_HYPOT3: cdb_eval_make_f64(out_val, cdb_math_hypot3(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]))); break;
        case CDB_FN_HAVERSINE:
            cdb_eval_make_f64(out_val, cdb_math_haversine(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_BBOX_CONTAINS:
            cdb_eval_make_bool(out_val, cdb_math_bbox_contains(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), to_double(&args[4]), to_double(&args[5])));
            break;
        case CDB_FN_BEARING:
            cdb_eval_make_f64(out_val, cdb_math_bearing(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_POINT_IN_CIRCLE:
            cdb_eval_make_bool(out_val, cdb_math_point_in_circle(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), to_double(&args[4])));
            break;
        case CDB_FN_CROSS_TRACK_DISTANCE:
            cdb_eval_make_f64(out_val, cdb_math_cross_track_distance(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), to_double(&args[4]), to_double(&args[5])));
            break;
        case CDB_FN_ALONG_TRACK_DISTANCE:
            cdb_eval_make_f64(out_val, cdb_math_along_track_distance(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), to_double(&args[4]), to_double(&args[5])));
            break;
        case CDB_FN_WGS84_TO_MERCATOR_X: cdb_eval_make_f64(out_val, cdb_math_wgs84_to_mercator_x(to_double(&args[0]))); break;
        case CDB_FN_WGS84_TO_MERCATOR_Y: cdb_eval_make_f64(out_val, cdb_math_wgs84_to_mercator_y(to_double(&args[0]))); break;
        case CDB_FN_MERCATOR_TO_WGS84_LON: cdb_eval_make_f64(out_val, cdb_math_mercator_to_wgs84_lon(to_double(&args[0]))); break;
        case CDB_FN_MERCATOR_TO_WGS84_LAT: cdb_eval_make_f64(out_val, cdb_math_mercator_to_wgs84_lat(to_double(&args[0]))); break;
        case CDB_FN_TILE_X: cdb_eval_make_int(out_val, cdb_math_lon_to_tile_x(to_double(&args[0]), (int)to_double(&args[1]))); break;
        case CDB_FN_TILE_Y: cdb_eval_make_int(out_val, cdb_math_lat_to_tile_y(to_double(&args[0]), (int)to_double(&args[1]))); break;
        case CDB_FN_MORTON2D: cdb_eval_make_int(out_val, (int64_t)cdb_math_morton2d(to_double(&args[0]), to_double(&args[1]))); break;

        /* Algebra & ML */
        case CDB_FN_SQRT: cdb_eval_make_f64(out_val, cdb_math_sqrt(to_double(&args[0]))); break;
        case CDB_FN_CBRT: cdb_eval_make_f64(out_val, cdb_math_cbrt(to_double(&args[0]))); break;
        case CDB_FN_POW: cdb_eval_make_f64(out_val, cdb_math_pow(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_EXP: cdb_eval_make_f64(out_val, cdb_math_exp(to_double(&args[0]))); break;
        case CDB_FN_EXPM1: cdb_eval_make_f64(out_val, cdb_math_expm1(to_double(&args[0]))); break;
        case CDB_FN_LN: cdb_eval_make_f64(out_val, cdb_math_ln(to_double(&args[0]))); break;
        case CDB_FN_LOG10: cdb_eval_make_f64(out_val, cdb_math_log10(to_double(&args[0]))); break;
        case CDB_FN_LOG2: cdb_eval_make_f64(out_val, cdb_math_log2(to_double(&args[0]))); break;
        case CDB_FN_LOG1P: cdb_eval_make_f64(out_val, cdb_math_log1p(to_double(&args[0]))); break;
        case CDB_FN_ABS: cdb_eval_make_f64(out_val, cdb_math_abs(to_double(&args[0]))); break;
        case CDB_FN_SIGN: cdb_eval_make_f64(out_val, cdb_math_sign(to_double(&args[0]))); break;
        case CDB_FN_ROUND: cdb_eval_make_f64(out_val, cdb_math_round(to_double(&args[0]), n_args > 1 ? (int)to_double(&args[1]) : 0)); break;
        case CDB_FN_FLOOR: cdb_eval_make_f64(out_val, cdb_math_floor(to_double(&args[0]))); break;
        case CDB_FN_CEIL: cdb_eval_make_f64(out_val, cdb_math_ceil(to_double(&args[0]))); break;
        case CDB_FN_CLAMP: cdb_eval_make_f64(out_val, cdb_math_clamp(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]))); break;
        case CDB_FN_LERP: cdb_eval_make_f64(out_val, cdb_math_lerp(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]))); break;
        case CDB_FN_SIGMOID: cdb_eval_make_f64(out_val, cdb_math_sigmoid(to_double(&args[0]))); break;
        case CDB_FN_RELU: cdb_eval_make_f64(out_val, cdb_math_relu(to_double(&args[0]))); break;
        case CDB_FN_GELU: cdb_eval_make_f64(out_val, cdb_math_gelu(to_double(&args[0]))); break;
        case CDB_FN_ERF: cdb_eval_make_f64(out_val, cdb_math_erf(to_double(&args[0]))); break;

        /* Calculus */
        case CDB_FN_DIFF: cdb_eval_make_f64(out_val, cdb_math_diff(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_DERIVATIVE: cdb_eval_make_f64(out_val, cdb_math_derivative(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_TRAPEZOID: cdb_eval_make_f64(out_val, cdb_math_trapezoid(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]))); break;

        /* SDR & DSP */
        case CDB_FN_IQ_MAGNITUDE: cdb_eval_make_f64(out_val, cdb_math_iq_magnitude(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_IQ_PHASE: cdb_eval_make_f64(out_val, cdb_math_iq_phase(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_IQ_DEMOD_FM:
            cdb_eval_make_f64(out_val, cdb_math_iq_demod_fm(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_DBFS: cdb_eval_make_f64(out_val, cdb_math_dbfs(to_double(&args[0]))); break;

        /* Astronomy, Telescope Mount & Satellite Tracking */
        case CDB_FN_ASTRO_ALT: {
            double az, alt, r;
            cdb_math_astro_topocentric(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]),
                                       to_double(&args[3]), to_double(&args[4]), to_double(&args[5]),
                                       &az, &alt, &r);
            cdb_eval_make_f64(out_val, alt);
            break;
        }
        case CDB_FN_ASTRO_AZ: {
            double az, alt, r;
            cdb_math_astro_topocentric(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]),
                                       to_double(&args[3]), to_double(&args[4]), to_double(&args[5]),
                                       &az, &alt, &r);
            cdb_eval_make_f64(out_val, az);
            break;
        }
        case CDB_FN_ASTRO_RANGE: {
            double az, alt, r;
            cdb_math_astro_topocentric(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]),
                                       to_double(&args[3]), to_double(&args[4]), to_double(&args[5]),
                                       &az, &alt, &r);
            cdb_eval_make_f64(out_val, r);
            break;
        }
        case CDB_FN_ASTRO_ALTAZ_TO_RA: {
            double ra, dec;
            int64_t ts = (n_args > 4 && args[4].type == CDB_TYPE_TIMESTAMP) ? args[4].as.ts.ticks : (int64_t)to_double(&args[4]);
            cdb_math_astro_altaz_to_radec(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), ts, &ra, &dec);
            cdb_eval_make_f64(out_val, ra);
            break;
        }
        case CDB_FN_ASTRO_ALTAZ_TO_DEC: {
            double ra, dec;
            int64_t ts = (n_args > 4 && args[4].type == CDB_TYPE_TIMESTAMP) ? args[4].as.ts.ticks : (int64_t)to_double(&args[4]);
            cdb_math_astro_altaz_to_radec(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), ts, &ra, &dec);
            cdb_eval_make_f64(out_val, dec);
            break;
        }
        case CDB_FN_ASTRO_RADEC_TO_ALT: {
            double alt, az;
            int64_t ts = (n_args > 4 && args[4].type == CDB_TYPE_TIMESTAMP) ? args[4].as.ts.ticks : (int64_t)to_double(&args[4]);
            cdb_math_astro_radec_to_altaz(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), ts, &alt, &az);
            cdb_eval_make_f64(out_val, alt);
            break;
        }
        case CDB_FN_ASTRO_RADEC_TO_AZ: {
            double alt, az;
            int64_t ts = (n_args > 4 && args[4].type == CDB_TYPE_TIMESTAMP) ? args[4].as.ts.ticks : (int64_t)to_double(&args[4]);
            cdb_math_astro_radec_to_altaz(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), ts, &alt, &az);
            cdb_eval_make_f64(out_val, az);
            break;
        }
        case CDB_FN_ASTRO_REFRACTION: cdb_eval_make_f64(out_val, cdb_math_astro_refraction(to_double(&args[0]))); break;
        case CDB_FN_ASTRO_ANGULAR_SEP:
            cdb_eval_make_f64(out_val, cdb_math_astro_angular_sep(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;

        /* Electronics & Circuits */
        case CDB_FN_ELEC_THERMISTOR_NTC:
            cdb_eval_make_f64(out_val, cdb_math_elec_thermistor_ntc(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_ELEC_STEINHART_HART:
            cdb_eval_make_f64(out_val, cdb_math_elec_steinhart_hart(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_ELEC_ADC_VOLTS:
            cdb_eval_make_f64(out_val, cdb_math_elec_adc_volts(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_ELEC_VOLTAGE_DIVIDER:
            cdb_eval_make_f64(out_val, cdb_math_elec_voltage_divider(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_ELEC_PARALLEL_R:
            cdb_eval_make_f64(out_val, cdb_math_elec_parallel_r(to_double(&args[0]), to_double(&args[1])));
            break;
        case CDB_FN_ELEC_DBM_TO_WATTS: cdb_eval_make_f64(out_val, cdb_math_elec_dbm_to_watts(to_double(&args[0]))); break;
        case CDB_FN_ELEC_WATTS_TO_DBM: cdb_eval_make_f64(out_val, cdb_math_elec_watts_to_dbm(to_double(&args[0]))); break;
        case CDB_FN_ELEC_RC_CUTOFF: cdb_eval_make_f64(out_val, cdb_math_elec_rc_cutoff(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ELEC_LC_RESONANCE: cdb_eval_make_f64(out_val, cdb_math_elec_lc_resonance(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ELEC_REACTANCE_C: cdb_eval_make_f64(out_val, cdb_math_elec_reactance_c(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ELEC_REACTANCE_L: cdb_eval_make_f64(out_val, cdb_math_elec_reactance_l(to_double(&args[0]), to_double(&args[1]))); break;

        /* Atmospheric Science & Gas Body Modeling */
        case CDB_FN_ATMO_PRESSURE_TO_ALT: cdb_eval_make_f64(out_val, cdb_math_atmo_pressure_to_alt(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ATMO_ALT_TO_SEALEVEL_P:
            cdb_eval_make_f64(out_val, cdb_math_atmo_alt_to_sealevel_p(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_ATMO_DEW_POINT: cdb_eval_make_f64(out_val, cdb_math_atmo_dew_point(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ATMO_VAPOR_PRESSURE: cdb_eval_make_f64(out_val, cdb_math_atmo_vapor_pressure(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ATMO_HEAT_INDEX: cdb_eval_make_f64(out_val, cdb_math_atmo_heat_index(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ATMO_WIND_CHILL: cdb_eval_make_f64(out_val, cdb_math_atmo_wind_chill(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_ATMO_AIR_DENSITY:
            cdb_eval_make_f64(out_val, cdb_math_atmo_air_density(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_GAS_IDEAL_PRESSURE:
            cdb_eval_make_f64(out_val, cdb_math_gas_ideal_pressure(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;

        /* Fluid Dynamics & Thermal Flow */
        case CDB_FN_FLUID_DYNAMIC_PRESSURE: cdb_eval_make_f64(out_val, cdb_math_fluid_dynamic_pressure(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_FLUID_PITOT_AIRSPEED:
            cdb_eval_make_f64(out_val, cdb_math_fluid_pitot_airspeed(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_FLUID_REYNOLDS:
            cdb_eval_make_f64(out_val, cdb_math_fluid_reynolds(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3])));
            break;
        case CDB_FN_FLUID_HYDROSTATIC_PRESSURE: cdb_eval_make_f64(out_val, cdb_math_fluid_hydrostatic_pressure(to_double(&args[0]), to_double(&args[1]))); break;
        case CDB_FN_FLUID_VENTURI_FLOW:
            cdb_eval_make_f64(out_val, cdb_math_fluid_venturi_flow(to_double(&args[0]), to_double(&args[1]), to_double(&args[2]), to_double(&args[3]), to_double(&args[4])));
            break;
        case CDB_FN_THERMAL_JUNCTION_TEMP:
            cdb_eval_make_f64(out_val, cdb_math_thermal_junction_temp(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;
        case CDB_FN_THERMAL_RADIATION_POWER:
            cdb_eval_make_f64(out_val, cdb_math_thermal_radiation_power(to_double(&args[0]), to_double(&args[1]), to_double(&args[2])));
            break;

        default:
            s = CDB_E_UNSUPPORTED;
            break;
        }
        break;
    }

    default:
        s = CDB_E_UNSUPPORTED;
        break;
    }

    ctx->depth--;
    return s;
}

cdb_status cdb_eval_expr_wire(cdb_eval_ctx *ctx, const uint8_t *wire, size_t len,
                             cdb_value *out_val, size_t *consumed)
{
    const uint8_t *p = wire;
    const uint8_t *end = wire + len;
    const uint8_t *prev_wire_end;
    cdb_query_expr expr;
    cdb_status s;

    if (ctx == NULL || wire == NULL || out_val == NULL || consumed == NULL)
        return CDB_E_INVALID_ARG;

    if ((s = cdb_query_read_expr(&p, end, &expr)) != CDB_OK)
        return s;

    *consumed = (size_t)(p - wire);
    prev_wire_end = ctx->wire_end;
    ctx->wire_end = end;
    s = cdb_eval_expr(ctx, &expr, out_val);
    ctx->wire_end = prev_wire_end;
    return s;
}

cdb_status cdb_eval_predicate(cdb_eval_ctx *ctx, const uint8_t *wire, size_t len, int *matched)
{
    cdb_value res;
    size_t consumed = 0;
    cdb_status s;

    if (ctx == NULL || wire == NULL || matched == NULL)
        return CDB_E_INVALID_ARG;

    *matched = 0;
    if ((s = cdb_eval_expr_wire(ctx, wire, len, &res, &consumed)) != CDB_OK)
        return s;

    *matched = cdb_eval_is_truthy(&res);
    return CDB_OK;
}
