/* Structured query core (CMBQL core): binary wire format implementation
 * (spec 11.1, 11.7, 11.10, 14.19). */
#include "cdb_query.h"
#include "cdb_varint.h"

#include <string.h>

/* ------------------------------------------------------------- writer */

static cdb_status w_byte(cdb_query_writer *w, uint8_t b)
{
    if (w->error != CDB_OK)
        return w->error;
    if (w->len >= w->cap) {
        w->error = CDB_E_BUFFER_TOO_SMALL;
        return w->error;
    }
    w->buf[w->len++] = b;
    return CDB_OK;
}

static cdb_status w_bytes(cdb_query_writer *w, const void *data, size_t len)
{
    if (w->error != CDB_OK)
        return w->error;
    if (len > w->cap - w->len) {
        w->error = CDB_E_BUFFER_TOO_SMALL;
        return w->error;
    }
    if (len)
        memcpy(w->buf + w->len, data, len);
    w->len += len;
    return CDB_OK;
}

static cdb_status w_u64(cdb_query_writer *w, uint64_t v)
{
    uint8_t enc[CDB_VARINT_MAX_LEN];
    size_t n = cdb_varint_encode(v, enc);
    return w_bytes(w, enc, n);
}

static cdb_status w_str(cdb_query_writer *w, const char *s, size_t len)
{
    cdb_status status = w_u64(w, (uint64_t)len);
    if (status != CDB_OK)
        return status;
    return w_bytes(w, s, len);
}

void cdb_query_writer_init(cdb_query_writer *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->error = CDB_OK;
}

cdb_status cdb_query_writer_begin(cdb_query_writer *w)
{
    static const uint8_t header[CDB_QUERY_HEADER_SIZE] = {
        CDB_QUERY_MAGIC_0, CDB_QUERY_MAGIC_1, CDB_QUERY_MAGIC_2, CDB_QUERY_MAGIC_3,
        CDB_QUERY_VERSION_1, 0x00u
    };
    return w_bytes(w, header, sizeof header);
}

size_t cdb_query_writer_len(const cdb_query_writer *w)
{
    return w->len;
}

cdb_status cdb_query_write_scan(cdb_query_writer *w, const char *store_name, size_t len)
{
    cdb_status s;
    if (store_name == NULL && len != 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_SCAN)) != CDB_OK)
        return s;
    return w_str(w, store_name, len);
}

cdb_status cdb_query_write_filter(cdb_query_writer *w)
{
    return w_byte(w, CDB_QUERY_NODE_FILTER);
}

cdb_status cdb_query_write_project(cdb_query_writer *w, int passthrough, size_t n_outputs)
{
    cdb_status s;
    if ((s = w_byte(w, CDB_QUERY_NODE_PROJECT)) != CDB_OK
        || (s = w_byte(w, (uint8_t)(passthrough ? 1u : 0u))) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_outputs);
}

cdb_status cdb_query_write_project_output(cdb_query_writer *w, const char *alias, size_t alias_len)
{
    if (alias == NULL && alias_len != 0)
        return (w->error = CDB_E_INVALID_ARG);
    return w_str(w, alias, alias_len);
}

cdb_status cdb_query_write_join(cdb_query_writer *w, uint8_t kind)
{
    cdb_status s;
    if (kind != CDB_JOIN_INNER && kind != CDB_JOIN_LEFT_OUTER)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_JOIN)) != CDB_OK)
        return s;
    return w_byte(w, kind);
}

cdb_status cdb_query_write_sort(cdb_query_writer *w, size_t n_keys)
{
    cdb_status s;
    if (n_keys == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_SORT)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_keys);
}

cdb_status cdb_query_write_sort_key_suffix(cdb_query_writer *w, uint8_t dir, uint8_t nulls)
{
    cdb_status s;
    if ((dir != CDB_SORT_ASC && dir != CDB_SORT_DESC)
        || (nulls != CDB_NULLS_FIRST && nulls != CDB_NULLS_LAST))
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, dir)) != CDB_OK)
        return s;
    return w_byte(w, nulls);
}

cdb_status cdb_query_write_limit(cdb_query_writer *w, uint64_t count, uint64_t offset)
{
    cdb_status s;
    if ((s = w_byte(w, CDB_QUERY_NODE_LIMIT)) != CDB_OK
        || (s = w_u64(w, count)) != CDB_OK)
        return s;
    return w_u64(w, offset);
}

cdb_status cdb_query_write_union(cdb_query_writer *w, size_t n_branches)
{
    cdb_status s;
    if (n_branches == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_UNION)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_branches);
}

cdb_status cdb_query_write_aggregate(cdb_query_writer *w, size_t n_groups, size_t n_aggs)
{
    cdb_status s;
    if (n_groups == 0 && n_aggs == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_AGGREGATE)) != CDB_OK
        || (s = w_u64(w, (uint64_t)n_groups)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_aggs);
}

cdb_status cdb_query_write_aggregate_output(cdb_query_writer *w, const char *alias, size_t alias_len, uint8_t fn)
{
    cdb_status s;
    if (fn < CDB_AGG_COUNT || fn > CDB_AGG_MAX)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_str(w, alias, alias_len)) != CDB_OK)
        return s;
    return w_byte(w, fn);
}

cdb_status cdb_query_write_traverse(cdb_query_writer *w, const char *link_type, size_t len, uint8_t dir,
                                    uint64_t min_hops, uint64_t max_hops)
{
    cdb_status s;
    if (dir < CDB_TRAVERSE_FWD || dir > CDB_TRAVERSE_BOTH || max_hops < min_hops)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_TRAVERSE)) != CDB_OK
        || (s = w_str(w, link_type, len)) != CDB_OK
        || (s = w_byte(w, dir)) != CDB_OK
        || (s = w_u64(w, min_hops)) != CDB_OK)
        return s;
    return w_u64(w, max_hops);
}

cdb_status cdb_query_write_text_search(cdb_query_writer *w, uint8_t mode, uint64_t limit,
                                      const char *query, size_t query_len, size_t n_fields)
{
    cdb_status s;
    if (mode != CDB_QUERY_FTS_ANY && mode != CDB_QUERY_FTS_ALL)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_TEXT_SEARCH)) != CDB_OK
        || (s = w_byte(w, mode)) != CDB_OK
        || (s = w_u64(w, limit)) != CDB_OK
        || (s = w_str(w, query, query_len)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_fields);
}

cdb_status cdb_query_write_field_name(cdb_query_writer *w, const char *field, size_t field_len)
{
    return w_str(w, field, field_len);
}

cdb_status cdb_query_write_vector_search(cdb_query_writer *w, const char *field, size_t field_len,
                                        uint64_t k, uint8_t metric)
{
    cdb_status s;
    if (metric > CDB_QUERY_METRIC_DOT)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_VECTOR_SEARCH)) != CDB_OK
        || (s = w_str(w, field, field_len)) != CDB_OK
        || (s = w_u64(w, k)) != CDB_OK)
        return s;
    return w_byte(w, metric);
}

cdb_status cdb_query_write_hybrid_rank(cdb_query_writer *w, uint8_t fuse, size_t n_branches)
{
    cdb_status s;
    if (fuse != CDB_FUSE_RRF || n_branches == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_HYBRID_RANK)) != CDB_OK
        || (s = w_byte(w, fuse)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)n_branches);
}

cdb_status cdb_query_write_branch_weight(cdb_query_writer *w, uint32_t weight)
{
    return w_u64(w, (uint64_t)weight);
}

cdb_status cdb_query_write_window(cdb_query_writer *w, uint8_t type, const char *time_field, size_t len,
                                  uint64_t size_ns, uint64_t step_ns)
{
    cdb_status s;
    if (type < CDB_WINDOW_TUMBLING || type > CDB_WINDOW_HOPPING)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_WINDOW)) != CDB_OK
        || (s = w_byte(w, type)) != CDB_OK
        || (s = w_str(w, time_field, len)) != CDB_OK
        || (s = w_u64(w, size_ns)) != CDB_OK)
        return s;
    return w_u64(w, step_ns);
}

cdb_status cdb_query_write_asof_join(cdb_query_writer *w, uint8_t kind, uint64_t tolerance_ns)
{
    cdb_status s;
    if (kind < CDB_ASOF_BACKWARD || kind > CDB_ASOF_NEAREST)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_ASOF_JOIN)) != CDB_OK
        || (s = w_byte(w, kind)) != CDB_OK)
        return s;
    return w_u64(w, tolerance_ns);
}

cdb_status cdb_query_write_unnest(cdb_query_writer *w, const char *item_alias, size_t item_len,
                                  const char *index_alias, size_t idx_len)
{
    cdb_status s;
    if (item_alias == NULL || item_len == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_NODE_UNNEST)) != CDB_OK
        || (s = w_str(w, item_alias, item_len)) != CDB_OK)
        return s;
    return w_str(w, index_alias != NULL ? index_alias : "", index_alias != NULL ? idx_len : 0u);
}

cdb_status cdb_query_write_expr_literal(cdb_query_writer *w, const uint8_t *val_bytes, size_t val_len)
{
    cdb_status s;
    if (val_bytes == NULL && val_len != 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_EXPR_LITERAL)) != CDB_OK
        || (s = w_u64(w, (uint64_t)val_len)) != CDB_OK)
        return s;
    return w_bytes(w, val_bytes, val_len);
}

cdb_status cdb_query_write_expr_field(cdb_query_writer *w, const char *path, size_t path_len)
{
    cdb_status s;
    if (path == NULL && path_len != 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_EXPR_FIELD)) != CDB_OK)
        return s;
    return w_str(w, path, path_len);
}

cdb_status cdb_query_write_expr_param_index(cdb_query_writer *w, uint32_t index)
{
    cdb_status s;
    if ((s = w_byte(w, CDB_QUERY_EXPR_PARAM)) != CDB_OK
        || (s = w_byte(w, CDB_PARAM_INDEX)) != CDB_OK)
        return s;
    return w_u64(w, (uint64_t)index);
}

cdb_status cdb_query_write_expr_param_named(cdb_query_writer *w, const char *name, size_t name_len)
{
    cdb_status s;
    if (name == NULL || name_len == 0)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_EXPR_PARAM)) != CDB_OK
        || (s = w_byte(w, CDB_PARAM_NAMED)) != CDB_OK)
        return s;
    return w_str(w, name, name_len);
}

cdb_status cdb_query_write_expr_unary(cdb_query_writer *w, uint8_t op)
{
    cdb_status s;
    if (op < CDB_UNARY_NOT || op > CDB_UNARY_IS_NOT_NULL)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_EXPR_UNARY)) != CDB_OK)
        return s;
    return w_byte(w, op);
}

cdb_status cdb_query_write_expr_binary(cdb_query_writer *w, uint8_t op)
{
    cdb_status s;
    if (op < CDB_BIN_AND || op > CDB_BIN_CONTAINS)
        return (w->error = CDB_E_INVALID_ARG);
    if ((s = w_byte(w, CDB_QUERY_EXPR_BINARY)) != CDB_OK)
        return s;
    return w_byte(w, op);
}

/* ------------------------------------------------------------- reader & validation */

static cdb_status r_byte(const uint8_t **p, const uint8_t *end, uint8_t *b)
{
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    *b = **p;
    (*p)++;
    return CDB_OK;
}

static cdb_status r_u64(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    size_t n;
    cdb_status s;
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    s = cdb_varint_decode(*p, (size_t)(end - *p), v, &n);
    if (s != CDB_OK)
        return s;
    *p += n;
    return CDB_OK;
}

static cdb_status r_str(const uint8_t **p, const uint8_t *end, const char **s, size_t *len)
{
    uint64_t v;
    cdb_status status = r_u64(p, end, &v);
    if (status != CDB_OK)
        return status;
    if ((size_t)(end - *p) < (size_t)v)
        return CDB_E_INVALID_ARG;
    *s = (const char *)*p;
    *len = (size_t)v;
    *p += (size_t)v;
    return CDB_OK;
}

cdb_status cdb_query_skip_expr(const uint8_t **p, const uint8_t *end)
{
    uint8_t tag;
    cdb_status s;
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    tag = **p;
    (*p)++;
    switch (tag) {
    case CDB_QUERY_EXPR_LITERAL: {
        uint64_t len;
        s = r_u64(p, end, &len);
        if (s != CDB_OK)
            return s;
        if ((size_t)(end - *p) < (size_t)len)
            return CDB_E_INVALID_ARG;
        /* Validate embedded cdb_value */
        if (cdb_value_validate(*p, (size_t)len, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT, 0) != CDB_OK
            && cdb_value_validate(*p, (size_t)len, CDB_ORDER_BIG, CDB_DECIMAL_COMPACT, 0) != CDB_OK)
            return CDB_E_INVALID_ARG;
        *p += (size_t)len;
        return CDB_OK;
    }
    case CDB_QUERY_EXPR_FIELD: {
        const char *path;
        size_t len;
        return r_str(p, end, &path, &len);
    }
    case CDB_QUERY_EXPR_PARAM: {
        uint8_t kind;
        s = r_byte(p, end, &kind);
        if (s != CDB_OK)
            return s;
        if (kind == CDB_PARAM_INDEX) {
            uint64_t idx;
            return r_u64(p, end, &idx);
        } else if (kind == CDB_PARAM_NAMED) {
            const char *name;
            size_t len;
            return r_str(p, end, &name, &len);
        }
        return CDB_E_INVALID_ARG;
    }
    case CDB_QUERY_EXPR_UNARY: {
        uint8_t op;
        s = r_byte(p, end, &op);
        if (s != CDB_OK)
            return s;
        if (op < CDB_UNARY_NOT || op > CDB_UNARY_IS_NOT_NULL)
            return CDB_E_INVALID_ARG;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_EXPR_BINARY: {
        uint8_t op;
        s = r_byte(p, end, &op);
        if (s != CDB_OK)
            return s;
        if (op < CDB_BIN_AND || op > CDB_BIN_CONTAINS)
            return CDB_E_INVALID_ARG;
        s = cdb_query_skip_expr(p, end);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end);
    }
    default:
        return CDB_E_INVALID_ARG;
    }
}

cdb_status cdb_query_skip_node(const uint8_t **p, const uint8_t *end)
{
    uint8_t tag;
    cdb_status s;
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    tag = **p;
    (*p)++;
    switch (tag) {
    case CDB_QUERY_NODE_SCAN: {
        const char *name;
        size_t len;
        return r_str(p, end, &name, &len);
    }
    case CDB_QUERY_NODE_FILTER: {
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_PROJECT: {
        uint8_t passthrough;
        uint64_t count, i;
        s = r_byte(p, end, &passthrough);
        if (s != CDB_OK || passthrough > 1)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &count);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        for (i = 0; i < count; i++) {
            const char *alias;
            size_t alen;
            s = r_str(p, end, &alias, &alen);
            if (s != CDB_OK)
                return s;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_JOIN: {
        uint8_t kind;
        s = r_byte(p, end, &kind);
        if (s != CDB_OK || (kind != CDB_JOIN_INNER && kind != CDB_JOIN_LEFT_OUTER))
            return CDB_E_INVALID_ARG;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_SORT: {
        uint64_t count, i;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        for (i = 0; i < count; i++) {
            uint8_t dir, nulls;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
            s = r_byte(p, end, &dir);
            if (s != CDB_OK || (dir != CDB_SORT_ASC && dir != CDB_SORT_DESC))
                return CDB_E_INVALID_ARG;
            s = r_byte(p, end, &nulls);
            if (s != CDB_OK || (nulls != CDB_NULLS_FIRST && nulls != CDB_NULLS_LAST))
                return CDB_E_INVALID_ARG;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_LIMIT: {
        uint64_t count, offset;
        s = r_u64(p, end, &count);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &offset);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_UNION: {
        uint64_t count, i;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        for (i = 0; i < count; i++) {
            s = cdb_query_skip_node(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_AGGREGATE: {
        uint64_t n_groups, n_aggs, i;
        s = r_u64(p, end, &n_groups);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &n_aggs);
        if (s != CDB_OK || (n_groups == 0 && n_aggs == 0))
            return CDB_E_INVALID_ARG;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        for (i = 0; i < n_groups; i++) {
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        for (i = 0; i < n_aggs; i++) {
            const char *alias;
            size_t alen;
            uint8_t fn;
            s = r_str(p, end, &alias, &alen);
            if (s != CDB_OK)
                return s;
            s = r_byte(p, end, &fn);
            if (s != CDB_OK || fn < CDB_AGG_COUNT || fn > CDB_AGG_MAX)
                return CDB_E_INVALID_ARG;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_TRAVERSE: {
        const char *link_type;
        size_t ltl;
        uint8_t dir;
        uint64_t min_hops, max_hops;
        s = r_str(p, end, &link_type, &ltl);
        if (s != CDB_OK)
            return s;
        s = r_byte(p, end, &dir);
        if (s != CDB_OK || dir < CDB_TRAVERSE_FWD || dir > CDB_TRAVERSE_BOTH)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &min_hops);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &max_hops);
        if (s != CDB_OK || max_hops < min_hops)
            return CDB_E_INVALID_ARG;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_TEXT_SEARCH: {
        uint8_t mode;
        uint64_t limit, n_fields, i;
        const char *query;
        size_t ql;
        s = r_byte(p, end, &mode);
        if (s != CDB_OK || (mode != CDB_QUERY_FTS_ANY && mode != CDB_QUERY_FTS_ALL))
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &limit);
        if (s != CDB_OK)
            return s;
        s = r_str(p, end, &query, &ql);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &n_fields);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        for (i = 0; i < n_fields; i++) {
            const char *field;
            size_t flen;
            s = r_str(p, end, &field, &flen);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_VECTOR_SEARCH: {
        const char *field;
        size_t flen;
        uint64_t k;
        uint8_t metric;
        s = r_str(p, end, &field, &flen);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &k);
        if (s != CDB_OK)
            return s;
        s = r_byte(p, end, &metric);
        if (s != CDB_OK || metric > CDB_QUERY_METRIC_DOT)
            return CDB_E_INVALID_ARG;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end); /* near expr */
    }
    case CDB_QUERY_NODE_HYBRID_RANK: {
        uint8_t fuse;
        uint64_t count, i;
        s = r_byte(p, end, &fuse);
        if (s != CDB_OK || fuse != CDB_FUSE_RRF)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        for (i = 0; i < count; i++) {
            uint64_t weight;
            s = cdb_query_skip_node(p, end);
            if (s != CDB_OK)
                return s;
            s = r_u64(p, end, &weight);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_WINDOW: {
        uint8_t type;
        const char *tfield;
        size_t tflen;
        uint64_t size_ns, step_ns;
        s = r_byte(p, end, &type);
        if (s != CDB_OK || type < CDB_WINDOW_TUMBLING || type > CDB_WINDOW_HOPPING)
            return CDB_E_INVALID_ARG;
        s = r_str(p, end, &tfield, &tflen);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &size_ns);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &step_ns);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_ASOF_JOIN: {
        uint8_t kind;
        uint64_t tol;
        s = r_byte(p, end, &kind);
        if (s != CDB_OK || kind < CDB_ASOF_BACKWARD || kind > CDB_ASOF_NEAREST)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &tol);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_expr(p, end); /* left_time */
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_expr(p, end); /* right_time */
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end); /* match_cond */
    }
    case CDB_QUERY_NODE_UNNEST: {
        const char *item_alias, *idx_alias;
        size_t item_len, idx_len;
        s = r_str(p, end, &item_alias, &item_len);
        if (s != CDB_OK || item_len == 0)
            return CDB_E_INVALID_ARG;
        s = r_str(p, end, &idx_alias, &idx_len);
        if (s != CDB_OK)
            return s;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        return cdb_query_skip_expr(p, end); /* array_expr */
    }
    default:
        return CDB_E_INVALID_ARG;
    }
}

cdb_status cdb_query_validate(const uint8_t *wire, size_t len, size_t *root_offset)
{
    const uint8_t *p;
    const uint8_t *end;
    cdb_status s;

    if (wire == NULL || len < CDB_QUERY_HEADER_SIZE)
        return CDB_E_INVALID_ARG;
    if (wire[0] != CDB_QUERY_MAGIC_0 || wire[1] != CDB_QUERY_MAGIC_1
        || wire[2] != CDB_QUERY_MAGIC_2 || wire[3] != CDB_QUERY_MAGIC_3)
        return CDB_E_INVALID_ARG;
    if (wire[4] != CDB_QUERY_VERSION_1)
        return CDB_E_UNSUPPORTED;
    if (wire[5] != 0x00u)
        return CDB_E_INVALID_ARG;

    p = wire + CDB_QUERY_HEADER_SIZE;
    end = wire + len;
    if (root_offset != NULL)
        *root_offset = CDB_QUERY_HEADER_SIZE;

    s = cdb_query_skip_node(&p, end);
    if (s != CDB_OK)
        return s;
    if (p != end)
        return CDB_E_INVALID_ARG; /* trailing unparsed bytes */
    return CDB_OK;
}

cdb_status cdb_query_read_expr(const uint8_t **p, const uint8_t *end, cdb_query_expr *expr)
{
    uint8_t tag;
    cdb_status s;
    memset(expr, 0, sizeof *expr);
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    tag = **p;
    (*p)++;
    expr->tag = tag;
    switch (tag) {
    case CDB_QUERY_EXPR_LITERAL: {
        uint64_t len;
        s = r_u64(p, end, &len);
        if (s != CDB_OK)
            return s;
        if ((size_t)(end - *p) < (size_t)len)
            return CDB_E_INVALID_ARG;
        expr->as.literal.data = *p;
        expr->as.literal.len = (size_t)len;
        *p += (size_t)len;
        return CDB_OK;
    }
    case CDB_QUERY_EXPR_FIELD:
        return r_str(p, end, &expr->as.field.path, &expr->as.field.len);
    case CDB_QUERY_EXPR_PARAM: {
        uint8_t kind;
        s = r_byte(p, end, &kind);
        if (s != CDB_OK)
            return s;
        expr->as.param.kind = kind;
        if (kind == CDB_PARAM_INDEX) {
            uint64_t idx;
            s = r_u64(p, end, &idx);
            if (s != CDB_OK)
                return s;
            expr->as.param.index = (uint32_t)idx;
            return CDB_OK;
        } else if (kind == CDB_PARAM_NAMED) {
            return r_str(p, end, &expr->as.param.name, &expr->as.param.name_len);
        }
        return CDB_E_INVALID_ARG;
    }
    case CDB_QUERY_EXPR_UNARY: {
        s = r_byte(p, end, &expr->as.unary.op);
        if (s != CDB_OK || expr->as.unary.op < CDB_UNARY_NOT || expr->as.unary.op > CDB_UNARY_IS_NOT_NULL)
            return CDB_E_INVALID_ARG;
        expr->as.unary.operand_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_EXPR_BINARY: {
        s = r_byte(p, end, &expr->as.binary.op);
        if (s != CDB_OK || expr->as.binary.op < CDB_BIN_AND || expr->as.binary.op > CDB_BIN_CONTAINS)
            return CDB_E_INVALID_ARG;
        expr->as.binary.left_wire = *p;
        s = cdb_query_skip_expr(p, end);
        if (s != CDB_OK)
            return s;
        expr->as.binary.right_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    default:
        return CDB_E_INVALID_ARG;
    }
}

cdb_status cdb_query_read_node(const uint8_t **p, const uint8_t *end, cdb_query_node *node)
{
    uint8_t tag;
    cdb_status s;
    memset(node, 0, sizeof *node);
    if (*p >= end)
        return CDB_E_INVALID_ARG;
    tag = **p;
    (*p)++;
    node->tag = tag;
    switch (tag) {
    case CDB_QUERY_NODE_SCAN:
        return r_str(p, end, &node->as.scan.name, &node->as.scan.name_len);
    case CDB_QUERY_NODE_FILTER: {
        node->as.filter.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.filter.pred_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_PROJECT: {
        uint8_t pt;
        uint64_t count, i;
        s = r_byte(p, end, &pt);
        if (s != CDB_OK || pt > 1)
            return CDB_E_INVALID_ARG;
        node->as.project.passthrough = (int)pt;
        s = r_u64(p, end, &count);
        if (s != CDB_OK)
            return s;
        node->as.project.count = (size_t)count;
        node->as.project.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.project.outputs_wire = *p;
        for (i = 0; i < count; i++) {
            const char *alias;
            size_t alen;
            s = r_str(p, end, &alias, &alen);
            if (s != CDB_OK)
                return s;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_JOIN: {
        s = r_byte(p, end, &node->as.join.kind);
        if (s != CDB_OK || (node->as.join.kind != CDB_JOIN_INNER && node->as.join.kind != CDB_JOIN_LEFT_OUTER))
            return CDB_E_INVALID_ARG;
        node->as.join.left_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.join.right_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.join.cond_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_SORT: {
        uint64_t count, i;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        node->as.sort.count = (size_t)count;
        node->as.sort.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.sort.keys_wire = *p;
        for (i = 0; i < count; i++) {
            uint8_t dir, nulls;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
            s = r_byte(p, end, &dir);
            if (s != CDB_OK || (dir != CDB_SORT_ASC && dir != CDB_SORT_DESC))
                return CDB_E_INVALID_ARG;
            s = r_byte(p, end, &nulls);
            if (s != CDB_OK || (nulls != CDB_NULLS_FIRST && nulls != CDB_NULLS_LAST))
                return CDB_E_INVALID_ARG;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_LIMIT: {
        s = r_u64(p, end, &node->as.limit.count);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.limit.offset);
        if (s != CDB_OK)
            return s;
        node->as.limit.input_wire = *p;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_UNION: {
        uint64_t count, i;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        node->as.union_node.count = (size_t)count;
        node->as.union_node.branches_wire = *p;
        for (i = 0; i < count; i++) {
            s = cdb_query_skip_node(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_AGGREGATE: {
        uint64_t n_groups, n_aggs, i;
        s = r_u64(p, end, &n_groups);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &n_aggs);
        if (s != CDB_OK || (n_groups == 0 && n_aggs == 0))
            return CDB_E_INVALID_ARG;
        node->as.aggregate.n_groups = (size_t)n_groups;
        node->as.aggregate.n_aggs = (size_t)n_aggs;
        node->as.aggregate.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.aggregate.groups_wire = *p;
        for (i = 0; i < n_groups; i++) {
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        node->as.aggregate.aggs_wire = *p;
        for (i = 0; i < n_aggs; i++) {
            const char *alias;
            size_t alen;
            uint8_t fn;
            s = r_str(p, end, &alias, &alen);
            if (s != CDB_OK)
                return s;
            s = r_byte(p, end, &fn);
            if (s != CDB_OK || fn < CDB_AGG_COUNT || fn > CDB_AGG_MAX)
                return CDB_E_INVALID_ARG;
            s = cdb_query_skip_expr(p, end);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_TRAVERSE: {
        s = r_str(p, end, &node->as.traverse.link_type, &node->as.traverse.link_type_len);
        if (s != CDB_OK)
            return s;
        s = r_byte(p, end, &node->as.traverse.dir);
        if (s != CDB_OK || node->as.traverse.dir < CDB_TRAVERSE_FWD || node->as.traverse.dir > CDB_TRAVERSE_BOTH)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &node->as.traverse.min_hops);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.traverse.max_hops);
        if (s != CDB_OK || node->as.traverse.max_hops < node->as.traverse.min_hops)
            return CDB_E_INVALID_ARG;
        node->as.traverse.input_wire = *p;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_TEXT_SEARCH: {
        uint64_t i;
        s = r_byte(p, end, &node->as.text_search.mode);
        if (s != CDB_OK || (node->as.text_search.mode != CDB_QUERY_FTS_ANY && node->as.text_search.mode != CDB_QUERY_FTS_ALL))
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &node->as.text_search.limit);
        if (s != CDB_OK)
            return s;
        s = r_str(p, end, &node->as.text_search.query, &node->as.text_search.query_len);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.text_search.n_fields);
        if (s != CDB_OK)
            return s;
        node->as.text_search.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.text_search.fields_wire = *p;
        for (i = 0; i < node->as.text_search.n_fields; i++) {
            const char *field;
            size_t flen;
            s = r_str(p, end, &field, &flen);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_VECTOR_SEARCH: {
        s = r_str(p, end, &node->as.vector_search.field, &node->as.vector_search.field_len);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.vector_search.k);
        if (s != CDB_OK)
            return s;
        s = r_byte(p, end, &node->as.vector_search.metric);
        if (s != CDB_OK || node->as.vector_search.metric > CDB_QUERY_METRIC_DOT)
            return CDB_E_INVALID_ARG;
        node->as.vector_search.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.vector_search.near_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_HYBRID_RANK: {
        uint64_t count, i;
        s = r_byte(p, end, &node->as.hybrid_rank.fuse);
        if (s != CDB_OK || node->as.hybrid_rank.fuse != CDB_FUSE_RRF)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &count);
        if (s != CDB_OK || count == 0)
            return CDB_E_INVALID_ARG;
        node->as.hybrid_rank.count = (size_t)count;
        node->as.hybrid_rank.branches_wire = *p;
        for (i = 0; i < count; i++) {
            uint64_t weight;
            s = cdb_query_skip_node(p, end);
            if (s != CDB_OK)
                return s;
            s = r_u64(p, end, &weight);
            if (s != CDB_OK)
                return s;
        }
        return CDB_OK;
    }
    case CDB_QUERY_NODE_WINDOW: {
        s = r_byte(p, end, &node->as.window.type);
        if (s != CDB_OK || node->as.window.type < CDB_WINDOW_TUMBLING || node->as.window.type > CDB_WINDOW_HOPPING)
            return CDB_E_INVALID_ARG;
        s = r_str(p, end, &node->as.window.time_field, &node->as.window.time_field_len);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.window.size_ns);
        if (s != CDB_OK)
            return s;
        s = r_u64(p, end, &node->as.window.step_ns);
        if (s != CDB_OK)
            return s;
        node->as.window.input_wire = *p;
        return cdb_query_skip_node(p, end);
    }
    case CDB_QUERY_NODE_ASOF_JOIN: {
        s = r_byte(p, end, &node->as.asof_join.kind);
        if (s != CDB_OK || node->as.asof_join.kind < CDB_ASOF_BACKWARD || node->as.asof_join.kind > CDB_ASOF_NEAREST)
            return CDB_E_INVALID_ARG;
        s = r_u64(p, end, &node->as.asof_join.tolerance_ns);
        if (s != CDB_OK)
            return s;
        node->as.asof_join.left_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.asof_join.right_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.asof_join.left_time_wire = *p;
        s = cdb_query_skip_expr(p, end);
        if (s != CDB_OK)
            return s;
        node->as.asof_join.right_time_wire = *p;
        s = cdb_query_skip_expr(p, end);
        if (s != CDB_OK)
            return s;
        node->as.asof_join.match_cond_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    case CDB_QUERY_NODE_UNNEST: {
        s = r_str(p, end, &node->as.unnest.item_alias, &node->as.unnest.item_alias_len);
        if (s != CDB_OK || node->as.unnest.item_alias_len == 0)
            return CDB_E_INVALID_ARG;
        s = r_str(p, end, &node->as.unnest.index_alias, &node->as.unnest.index_alias_len);
        if (s != CDB_OK)
            return s;
        node->as.unnest.input_wire = *p;
        s = cdb_query_skip_node(p, end);
        if (s != CDB_OK)
            return s;
        node->as.unnest.array_expr_wire = *p;
        return cdb_query_skip_expr(p, end);
    }
    default:
        return CDB_E_INVALID_ARG;
    }
}
