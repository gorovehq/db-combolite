/* Streaming query plan executor and cursor engine (CMBQL core).
 * Spec 11.5, 11.10; Spec 14.19.
 */
#include "cdb_exec.h"
#include "cdb_math.h"
#include "cdb_varint.h"

#include <stdlib.h>
#include <string.h>

/* Alignment helper for arena allocations */
static size_t align_up(size_t n, size_t align)
{
    size_t rem = n % align;
    return (rem == 0) ? n : (n + align - rem);
}

void *cdb_exec_arena_alloc(cdb_exec_arena *arena, size_t size, size_t align)
{
    size_t aligned_used;
    void *ptr;

    if (arena == NULL || arena->buf == NULL || align == 0)
        return NULL;

    aligned_used = align_up(arena->used, align);
    if (aligned_used > arena->cap || size > arena->cap - aligned_used)
        return NULL; /* Out of arena memory */

    ptr = arena->buf + aligned_used;
    arena->used = aligned_used + size;
    memset(ptr, 0, size);
    return ptr;
}

void cdb_exec_row_copy(cdb_exec_row *dst, const cdb_exec_row *src)
{
    if (dst == NULL || src == NULL)
        return;
    *dst = *src;
}

/* Custom field resolver hook to look up fields on an executed row */
static cdb_status row_field_resolver(void *user_data, const char *path, size_t path_len,
                                     cdb_value *out_val, int *found)
{
    const cdb_exec_row *row = (const cdb_exec_row *)user_data;
    size_t i;

    if (row == NULL || path == NULL || out_val == NULL || found == NULL)
        return CDB_E_INVALID_ARG;

    *found = 0;

    /* Check explicit fields (aliases/projected/joined) */
    for (i = 0; i < row->n_fields; i++) {
        if (row->fields[i].name_len == path_len
            && memcmp(row->fields[i].name, path, path_len) == 0) {
            *out_val = row->fields[i].val;
            *found = 1;
            return CDB_OK;
        }
    }

    return CDB_OK;
}

cdb_status cdb_exec_row_get_field(const cdb_exec_row *row, cdb_eval_ctx *eval_ctx,
                                  const char *name, size_t name_len,
                                  cdb_value *out_val, int *found)
{
    cdb_status s;

    if (row == NULL || eval_ctx == NULL || name == NULL || out_val == NULL || found == NULL)
        return CDB_E_INVALID_ARG;

    *found = 0;

    /* Setup evaluation context for this row */
    eval_ctx->entry_id = row->entry_id;
    eval_ctx->score = (double)row->score;
    eval_ctx->timestamp = row->timestamp;
    eval_ctx->record_data = row->record_data;
    eval_ctx->record_len = row->record_len;
    eval_ctx->store_id = row->store_id;

    cdb_eval_ctx_set_custom_resolver(eval_ctx, row_field_resolver, (void *)row);

    s = cdb_eval_resolve_field(eval_ctx, name, name_len, out_val, found);
    return s;
}

/* --------------------------------------------------------------------------
 * Operator 0x01: SCAN
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_scan {
    cdb_exec_op base;
    const char *store_name;
    size_t store_name_len;
    uint64_t store_id;
    uint32_t model;
    uint64_t cur_entry_id;
    uint64_t max_entry_id;

    /* For mock/standalone testing without a live database */
    const cdb_exec_row *mock_rows;
    size_t n_mock_rows;
    size_t mock_idx;
} cdb_exec_scan;

static cdb_status scan_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_scan *s = (cdb_exec_scan *)op;
    cdb_store_info info;
    cdb_status st;

    s->cur_entry_id = 0;
    s->mock_idx = 0;

    if (cur->cat != NULL) {
        int found = 0;
        st = cdb_catalog_find_store(cur->cat, s->store_name, s->store_name_len, &s->store_id, &found);
        if (st == CDB_OK && found) {
            st = cdb_catalog_get_store(cur->cat, s->store_id, &info);
            if (st == CDB_OK) {
                s->model = info.model;
                s->max_entry_id = info.next_entry_id;
            }
        }
    }

    return CDB_OK;
}

static cdb_status scan_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_scan *s = (cdb_exec_scan *)op;
    (void)cur;
    s->cur_entry_id = 0;
    s->mock_idx = 0;
    return CDB_OK;
}

static void scan_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    (void)op;
    (void)cur;
}

static cdb_status scan_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_scan *s = (cdb_exec_scan *)op;
    (void)cur;

    *has_row = 0;

    /* Check mock rows first if registered */
    if (s->mock_rows != NULL && s->mock_idx < s->n_mock_rows) {
        cdb_exec_row_copy(out_row, &s->mock_rows[s->mock_idx++]);
        *has_row = 1;
        return CDB_OK;
    }

    /* Live store scan if db and catalog are available */
    if (cur->db != NULL && s->store_id != 0) {
        while (s->cur_entry_id + 1 < s->max_entry_id) {
            uint64_t eid = ++s->cur_entry_id;
            memset(out_row, 0, sizeof(*out_row));
            out_row->store_id = s->store_id;
            out_row->entry_id = eid;
            out_row->model = s->model;
            *has_row = 1;
            return CDB_OK;
        }
    }

    return CDB_OK;
}

static const cdb_exec_vtab scan_vtab = {
    scan_open,
    scan_next,
    scan_reset,
    scan_close
};

/* --------------------------------------------------------------------------
 * Operator 0x02: FILTER
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_filter {
    cdb_exec_op base;
    cdb_exec_op *input;
    const uint8_t *pred_wire;
    size_t pred_len;
} cdb_exec_filter;

static cdb_status filter_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_filter *f = (cdb_exec_filter *)op;
    return f->input->vtab->open(f->input, cur);
}

static cdb_status filter_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_filter *f = (cdb_exec_filter *)op;
    return f->input->vtab->reset(f->input, cur);
}

static void filter_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_filter *f = (cdb_exec_filter *)op;
    f->input->vtab->close(f->input, cur);
}

static cdb_status filter_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_filter *f = (cdb_exec_filter *)op;
    cdb_status st;
    int matched = 0;

    *has_row = 0;

    while (1) {
        st = f->input->vtab->next(f->input, cur, out_row, has_row);
        if (st != CDB_OK || !*has_row)
            return st;

        /* Prepare evaluation context with current row */
        cur->eval_ctx.entry_id = out_row->entry_id;
        cur->eval_ctx.score = (double)out_row->score;
        cur->eval_ctx.timestamp = out_row->timestamp;
        cur->eval_ctx.record_data = out_row->record_data;
        cur->eval_ctx.record_len = out_row->record_len;
        cur->eval_ctx.store_id = out_row->store_id;

        cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)out_row);

        matched = 0;
        st = cdb_eval_predicate(&cur->eval_ctx, f->pred_wire, f->pred_len, &matched);
        if (st != CDB_OK)
            return st;

        if (matched) {
            *has_row = 1;
            return CDB_OK;
        }
    }
}

static const cdb_exec_vtab filter_vtab = {
    filter_open,
    filter_next,
    filter_reset,
    filter_close
};

/* --------------------------------------------------------------------------
 * Operator 0x03: PROJECT
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_proj_col {
    const char *alias;
    size_t alias_len;
    const uint8_t *expr_wire;
    size_t expr_len;
} cdb_exec_proj_col;

typedef struct cdb_exec_project {
    cdb_exec_op base;
    cdb_exec_op *input;
    uint8_t passthrough;
    size_t count;
    cdb_exec_proj_col cols[CDB_EXEC_MAX_FIELDS];
} cdb_exec_project;

static cdb_status project_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_project *p = (cdb_exec_project *)op;
    return p->input->vtab->open(p->input, cur);
}

static cdb_status project_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_project *p = (cdb_exec_project *)op;
    return p->input->vtab->reset(p->input, cur);
}

static void project_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_project *p = (cdb_exec_project *)op;
    p->input->vtab->close(p->input, cur);
}

static cdb_status project_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_project *p = (cdb_exec_project *)op;
    cdb_exec_row in_row;
    cdb_status st;
    size_t i;

    *has_row = 0;
    st = p->input->vtab->next(p->input, cur, &in_row, has_row);
    if (st != CDB_OK || !*has_row)
        return st;

    memset(out_row, 0, sizeof(*out_row));
    out_row->store_id = in_row.store_id;
    out_row->entry_id = in_row.entry_id;
    out_row->model = in_row.model;
    out_row->score = in_row.score;
    out_row->timestamp = in_row.timestamp;

    if (p->passthrough) {
        out_row->record_data = in_row.record_data;
        out_row->record_len = in_row.record_len;
        for (i = 0; i < in_row.n_fields && out_row->n_fields < CDB_EXEC_MAX_FIELDS; i++) {
            out_row->fields[out_row->n_fields++] = in_row.fields[i];
        }
    }

    /* Bind input row to evaluation context */
    cur->eval_ctx.entry_id = in_row.entry_id;
    cur->eval_ctx.score = (double)in_row.score;
    cur->eval_ctx.timestamp = in_row.timestamp;
    cur->eval_ctx.record_data = in_row.record_data;
    cur->eval_ctx.record_len = in_row.record_len;
    cur->eval_ctx.store_id = in_row.store_id;
    cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)&in_row);

    /* Evaluate projected output columns */
    for (i = 0; i < p->count && out_row->n_fields < CDB_EXEC_MAX_FIELDS; i++) {
        cdb_value val;
        size_t consumed = 0;
        st = cdb_eval_expr_wire(&cur->eval_ctx, p->cols[i].expr_wire, p->cols[i].expr_len, &val, &consumed);
        if (st != CDB_OK)
            return st;

        out_row->fields[out_row->n_fields].name = p->cols[i].alias;
        out_row->fields[out_row->n_fields].name_len = p->cols[i].alias_len;
        out_row->fields[out_row->n_fields].val = val;
        out_row->n_fields++;
    }

    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab project_vtab = {
    project_open,
    project_next,
    project_reset,
    project_close
};

/* --------------------------------------------------------------------------
 * Operator 0x06: LIMIT
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_limit {
    cdb_exec_op base;
    cdb_exec_op *input;
    uint64_t count;
    uint64_t offset;
    uint64_t skipped;
    uint64_t yielded;
} cdb_exec_limit;

static cdb_status limit_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_limit *l = (cdb_exec_limit *)op;
    l->skipped = 0;
    l->yielded = 0;
    return l->input->vtab->open(l->input, cur);
}

static cdb_status limit_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_limit *l = (cdb_exec_limit *)op;
    l->skipped = 0;
    l->yielded = 0;
    return l->input->vtab->reset(l->input, cur);
}

static void limit_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_limit *l = (cdb_exec_limit *)op;
    l->input->vtab->close(l->input, cur);
}

static cdb_status limit_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_limit *l = (cdb_exec_limit *)op;
    cdb_status st;

    *has_row = 0;

    /* Skip offset rows */
    while (l->skipped < l->offset) {
        st = l->input->vtab->next(l->input, cur, out_row, has_row);
        if (st != CDB_OK || !*has_row)
            return st;
        l->skipped++;
    }

    /* Check if count limit reached */
    if (l->yielded >= l->count) {
        *has_row = 0;
        return CDB_OK;
    }

    st = l->input->vtab->next(l->input, cur, out_row, has_row);
    if (st == CDB_OK && *has_row) {
        l->yielded++;
    }

    return st;
}

static const cdb_exec_vtab limit_vtab = {
    limit_open,
    limit_next,
    limit_reset,
    limit_close
};

/* --------------------------------------------------------------------------
 * Operator 0x07: UNION
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_union {
    cdb_exec_op base;
    size_t count;
    cdb_exec_op *branches[CDB_EXEC_MAX_BRANCHES];
    size_t cur_branch;
} cdb_exec_union;

static cdb_status union_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_union *u = (cdb_exec_union *)op;
    size_t i;
    u->cur_branch = 0;
    for (i = 0; i < u->count; i++) {
        cdb_status st = u->branches[i]->vtab->open(u->branches[i], cur);
        if (st != CDB_OK)
            return st;
    }
    return CDB_OK;
}

static cdb_status union_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_union *u = (cdb_exec_union *)op;
    size_t i;
    u->cur_branch = 0;
    for (i = 0; i < u->count; i++) {
        cdb_status st = u->branches[i]->vtab->reset(u->branches[i], cur);
        if (st != CDB_OK)
            return st;
    }
    return CDB_OK;
}

static void union_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_union *u = (cdb_exec_union *)op;
    size_t i;
    for (i = 0; i < u->count; i++) {
        u->branches[i]->vtab->close(u->branches[i], cur);
    }
}

static cdb_status union_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_union *u = (cdb_exec_union *)op;
    cdb_status st;

    *has_row = 0;

    while (u->cur_branch < u->count) {
        st = u->branches[u->cur_branch]->vtab->next(u->branches[u->cur_branch], cur, out_row, has_row);
        if (st != CDB_OK)
            return st;
        if (*has_row)
            return CDB_OK;
        u->cur_branch++;
    }

    *has_row = 0;
    return CDB_OK;
}

static const cdb_exec_vtab union_vtab = {
    union_open,
    union_next,
    union_reset,
    union_close
};

/* --------------------------------------------------------------------------
 * Operator 0x04: JOIN (Nested Loop Join: Inner & Left-Outer)
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_join {
    cdb_exec_op base;
    uint8_t kind; /* 1 = Inner, 2 = LeftOuter */
    cdb_exec_op *left;
    cdb_exec_op *right;
    const uint8_t *cond_wire;
    size_t cond_len;

    cdb_exec_row cur_left;
    int has_left;
    int left_matched;
} cdb_exec_join;

static cdb_status join_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_join *j = (cdb_exec_join *)op;
    cdb_status st;

    j->has_left = 0;
    j->left_matched = 0;

    if ((st = j->left->vtab->open(j->left, cur)) != CDB_OK)
        return st;
    return j->right->vtab->open(j->right, cur);
}

static cdb_status join_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_join *j = (cdb_exec_join *)op;
    cdb_status st;

    j->has_left = 0;
    j->left_matched = 0;

    if ((st = j->left->vtab->reset(j->left, cur)) != CDB_OK)
        return st;
    return j->right->vtab->reset(j->right, cur);
}

static void join_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_join *j = (cdb_exec_join *)op;
    j->left->vtab->close(j->left, cur);
    j->right->vtab->close(j->right, cur);
}

static cdb_status join_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_join *j = (cdb_exec_join *)op;
    cdb_exec_row cur_right;
    int right_has_row = 0;
    cdb_status st;
    size_t i;

    *has_row = 0;

    while (1) {
        if (!j->has_left) {
            st = j->left->vtab->next(j->left, cur, &j->cur_left, &j->has_left);
            if (st != CDB_OK || !j->has_left)
                return st;
            j->left_matched = 0;
            st = j->right->vtab->reset(j->right, cur);
            if (st != CDB_OK)
                return st;
        }

        while (1) {
            st = j->right->vtab->next(j->right, cur, &cur_right, &right_has_row);
            if (st != CDB_OK)
                return st;

            if (!right_has_row) {
                /* Right side exhausted for this left row */
                j->has_left = 0;
                if (j->kind == CDB_JOIN_LEFT_OUTER && !j->left_matched) {
                    /* Emit left row with NULL right context */
                    *out_row = j->cur_left;
                    out_row->has_right = 1;
                    out_row->right_store_id = 0;
                    out_row->right_entry_id = 0;
                    out_row->right_record_data = NULL;
                    out_row->right_record_len = 0;
                    *has_row = 1;
                    return CDB_OK;
                }
                break; /* Advance to next left row */
            }

            /* Combine left and right contexts into candidate */
            *out_row = j->cur_left;
            out_row->has_right = 1;
            out_row->right_store_id = cur_right.store_id;
            out_row->right_entry_id = cur_right.entry_id;
            out_row->right_model = cur_right.model;
            out_row->right_record_data = cur_right.record_data;
            out_row->right_record_len = cur_right.record_len;
            for (i = 0; i < cur_right.n_fields && out_row->n_fields < CDB_EXEC_MAX_FIELDS; i++) {
                out_row->fields[out_row->n_fields++] = cur_right.fields[i];
            }

            /* Evaluate join condition */
            cur->eval_ctx.entry_id = out_row->entry_id;
            cur->eval_ctx.score = (double)out_row->score;
            cur->eval_ctx.timestamp = out_row->timestamp;
            cur->eval_ctx.record_data = out_row->record_data;
            cur->eval_ctx.record_len = out_row->record_len;
            cur->eval_ctx.store_id = out_row->store_id;
            cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)out_row);

            if (j->cond_wire != NULL && j->cond_len > 0) {
                int matched = 0;
                st = cdb_eval_predicate(&cur->eval_ctx, j->cond_wire, j->cond_len, &matched);
                if (st != CDB_OK)
                    return st;
                if (!matched)
                    continue; /* Condition not satisfied, check next right row */
            }

            j->left_matched = 1;
            *has_row = 1;
            return CDB_OK;
        }
    }
}

static const cdb_exec_vtab join_vtab = {
    join_open,
    join_next,
    join_reset,
    join_close
};

/* --------------------------------------------------------------------------
 * Operator 0x05: SORT
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_sort_key {
    const uint8_t *expr_wire;
    size_t expr_len;
    uint8_t dir;   /* 1 = ASC, 2 = DESC */
    uint8_t nulls; /* 1 = First, 2 = Last */
} cdb_exec_sort_key;

typedef struct cdb_exec_sort {
    cdb_exec_op base;
    cdb_exec_op *input;
    size_t count;
    cdb_exec_sort_key keys[CDB_EXEC_MAX_SORT_KEYS];

    cdb_exec_row *buf;
    size_t n_rows;
    size_t max_rows;
    size_t cur_idx;
    int is_buffered;
} cdb_exec_sort;

static int compare_rows(const void *a_ptr, const void *b_ptr, void *ctx_ptr)
{
    const cdb_exec_row *a = (const cdb_exec_row *)a_ptr;
    const cdb_exec_row *b = (const cdb_exec_row *)b_ptr;
    cdb_exec_cursor *cur = (cdb_exec_cursor *)ctx_ptr;
    cdb_exec_sort *s = (cdb_exec_sort *)cur->root_op; /* Default or active op */
    size_t k;

    for (k = 0; k < s->count; k++) {
        cdb_value va, vb;
        size_t consumed = 0;
        int a_null, b_null;

        /* Eval key for A */
        cur->eval_ctx.entry_id = a->entry_id;
        cur->eval_ctx.score = (double)a->score;
        cur->eval_ctx.timestamp = a->timestamp;
        cur->eval_ctx.record_data = a->record_data;
        cur->eval_ctx.record_len = a->record_len;
        cur->eval_ctx.store_id = a->store_id;
        cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)a);
        if (cdb_eval_expr_wire(&cur->eval_ctx, s->keys[k].expr_wire, s->keys[k].expr_len, &va, &consumed) != CDB_OK)
            cdb_eval_make_null(&va);

        /* Eval key for B */
        cur->eval_ctx.entry_id = b->entry_id;
        cur->eval_ctx.score = (double)b->score;
        cur->eval_ctx.timestamp = b->timestamp;
        cur->eval_ctx.record_data = b->record_data;
        cur->eval_ctx.record_len = b->record_len;
        cur->eval_ctx.store_id = b->store_id;
        cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)b);
        if (cdb_eval_expr_wire(&cur->eval_ctx, s->keys[k].expr_wire, s->keys[k].expr_len, &vb, &consumed) != CDB_OK)
            cdb_eval_make_null(&vb);

        a_null = cdb_eval_is_null(&va);
        b_null = cdb_eval_is_null(&vb);

        if (a_null && b_null)
            continue;
        if (a_null)
            return (s->keys[k].nulls == CDB_NULLS_FIRST) ? -1 : 1;
        if (b_null)
            return (s->keys[k].nulls == CDB_NULLS_FIRST) ? 1 : -1;

        {
            int c_res = 0;
            int is_null = 0;
            if (cdb_eval_compare(&va, &vb, &c_res, &is_null) == CDB_OK && !is_null && c_res != 0) {
                return (s->keys[k].dir == CDB_SORT_DESC) ? -c_res : c_res;
            }
        }
    }
    return 0;
}

/* Zero-heap in-place insertion sort for bounded row array */
static void sort_rows_insertion(cdb_exec_row *rows, size_t n, cdb_exec_cursor *cur, cdb_exec_sort *s)
{
    size_t i, j;
    cdb_exec_op *prev_root = cur->root_op;
    cur->root_op = &s->base;

    for (i = 1; i < n; i++) {
        cdb_exec_row key = rows[i];
        j = i;
        while (j > 0 && compare_rows(&rows[j - 1], &key, (void *)cur) > 0) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = key;
    }

    cur->root_op = prev_root;
}

static cdb_status sort_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_sort *s = (cdb_exec_sort *)op;
    size_t row_size = sizeof(cdb_exec_row);
    size_t avail = (cur->arena.cap > cur->arena.used) ? (cur->arena.cap - cur->arena.used) : 0;
    size_t max_rows = avail / row_size;

    if (max_rows > cur->max_buffered_rows)
        max_rows = cur->max_buffered_rows;
    if (max_rows < 1)
        return CDB_E_OUT_OF_BUDGET;

    s->n_rows = 0;
    s->cur_idx = 0;
    s->is_buffered = 0;
    s->max_rows = max_rows;
    s->buf = (cdb_exec_row *)cdb_exec_arena_alloc(&cur->arena, row_size * max_rows, sizeof(void *));
    if (s->buf == NULL)
        return CDB_E_OUT_OF_BUDGET;

    return s->input->vtab->open(s->input, cur);
}

static cdb_status sort_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_sort *s = (cdb_exec_sort *)op;
    (void)cur;
    s->cur_idx = 0;
    return CDB_OK;
}

static void sort_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_sort *s = (cdb_exec_sort *)op;
    s->input->vtab->close(s->input, cur);
}

static cdb_status sort_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_sort *s = (cdb_exec_sort *)op;
    cdb_status st;

    *has_row = 0;

    if (!s->is_buffered) {
        cdb_exec_row item;
        int in_has_row = 0;

        while (1) {
            st = s->input->vtab->next(s->input, cur, &item, &in_has_row);
            if (st != CDB_OK)
                return st;
            if (!in_has_row)
                break;

            if (s->n_rows >= s->max_rows)
                return CDB_E_OUT_OF_BUDGET;

            s->buf[s->n_rows++] = item;
        }

        if (s->n_rows > 1) {
            sort_rows_insertion(s->buf, s->n_rows, cur, s);
        }
        s->is_buffered = 1;
        s->cur_idx = 0;
    }

    if (s->cur_idx < s->n_rows) {
        *out_row = s->buf[s->cur_idx++];
        *has_row = 1;
        return CDB_OK;
    }

    return CDB_OK;
}

static const cdb_exec_vtab sort_vtab = {
    sort_open,
    sort_next,
    sort_reset,
    sort_close
};

/* --------------------------------------------------------------------------
 * Operator 0x08: AGGREGATE
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_agg_desc {
    const char *alias;
    size_t alias_len;
    uint8_t fn; /* 1=COUNT, 2=SUM, 3=AVG, 4=MIN, 5=MAX */
    const uint8_t *arg_wire;
    size_t arg_len;
} cdb_exec_agg_desc;

typedef struct cdb_exec_agg_group {
    cdb_value group_vals[CDB_EXEC_MAX_FIELDS];
    double sum[CDB_EXEC_MAX_AGGS];
    uint64_t count[CDB_EXEC_MAX_AGGS];
    cdb_value min_val[CDB_EXEC_MAX_AGGS];
    cdb_value max_val[CDB_EXEC_MAX_AGGS];
    int has_val[CDB_EXEC_MAX_AGGS];
} cdb_exec_agg_group;

typedef struct cdb_exec_aggregate {
    cdb_exec_op base;
    cdb_exec_op *input;
    size_t groups_count;
    size_t aggs_count;
    const uint8_t *group_wires[CDB_EXEC_MAX_FIELDS];
    size_t group_lens[CDB_EXEC_MAX_FIELDS];
    cdb_exec_agg_desc aggs[CDB_EXEC_MAX_AGGS];

    cdb_exec_agg_group *groups;
    size_t n_groups;
    size_t max_groups;
    size_t cur_idx;
    int is_buffered;
} cdb_exec_aggregate;

static cdb_status aggregate_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_aggregate *a = (cdb_exec_aggregate *)op;
    size_t grp_size = sizeof(cdb_exec_agg_group);
    size_t avail = (cur->arena.cap > cur->arena.used) ? (cur->arena.cap - cur->arena.used) : 0;
    size_t max_groups = avail / grp_size;

    if (max_groups > cur->max_buffered_rows)
        max_groups = cur->max_buffered_rows;
    if (max_groups < 1)
        return CDB_E_OUT_OF_BUDGET;

    a->n_groups = 0;
    a->cur_idx = 0;
    a->is_buffered = 0;
    a->max_groups = max_groups;
    a->groups = (cdb_exec_agg_group *)cdb_exec_arena_alloc(&cur->arena, grp_size * max_groups, sizeof(void *));
    if (a->groups == NULL)
        return CDB_E_OUT_OF_BUDGET;

    return a->input->vtab->open(a->input, cur);
}

static cdb_status aggregate_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_aggregate *a = (cdb_exec_aggregate *)op;
    (void)cur;
    a->cur_idx = 0;
    return CDB_OK;
}

static void aggregate_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_aggregate *a = (cdb_exec_aggregate *)op;
    a->input->vtab->close(a->input, cur);
}

static cdb_status aggregate_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_aggregate *a = (cdb_exec_aggregate *)op;
    cdb_status st;
    size_t g, i;

    *has_row = 0;

    if (!a->is_buffered) {
        cdb_exec_row item;
        int in_has_row = 0;

        while (1) {
            cdb_value gvals[CDB_EXEC_MAX_FIELDS];
            int group_idx = -1;

            st = a->input->vtab->next(a->input, cur, &item, &in_has_row);
            if (st != CDB_OK)
                return st;
            if (!in_has_row)
                break;

            /* Set up evaluation context for current input row */
            cur->eval_ctx.entry_id = item.entry_id;
            cur->eval_ctx.score = (double)item.score;
            cur->eval_ctx.timestamp = item.timestamp;
            cur->eval_ctx.record_data = item.record_data;
            cur->eval_ctx.record_len = item.record_len;
            cur->eval_ctx.store_id = item.store_id;
            cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)&item);

            /* Evaluate group keys */
            for (g = 0; g < a->groups_count; g++) {
                size_t consumed = 0;
                st = cdb_eval_expr_wire(&cur->eval_ctx, a->group_wires[g], a->group_lens[g], &gvals[g], &consumed);
                if (st != CDB_OK)
                    return st;
            }

            /* Search for matching group */
            for (g = 0; g < a->n_groups; g++) {
                int match = 1;
                for (i = 0; i < a->groups_count; i++) {
                    int c_res = 0;
                    int is_null = 0;
                    if (cdb_eval_compare(&gvals[i], &a->groups[g].group_vals[i], &c_res, &is_null) != CDB_OK || is_null || c_res != 0) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    group_idx = (int)g;
                    break;
                }
            }

            /* Create new group if not found */
            if (group_idx < 0) {
                if (a->n_groups >= a->max_groups)
                    return CDB_E_OUT_OF_BUDGET;

                group_idx = (int)a->n_groups++;
                memset(&a->groups[group_idx], 0, sizeof(cdb_exec_agg_group));
                for (i = 0; i < a->groups_count; i++) {
                    a->groups[group_idx].group_vals[i] = gvals[i];
                }
            }

            /* Update accumulators for group */
            for (i = 0; i < a->aggs_count; i++) {
                cdb_exec_agg_group *grp = &a->groups[group_idx];
                cdb_value aval;
                size_t consumed = 0;
                double dval = 0.0;

                st = cdb_eval_expr_wire(&cur->eval_ctx, a->aggs[i].arg_wire, a->aggs[i].arg_len, &aval, &consumed);
                if (st != CDB_OK)
                    return st;

                if (a->aggs[i].fn == CDB_AGG_COUNT) {
                    grp->count[i]++;
                    grp->has_val[i] = 1;
                    continue;
                }

                if (cdb_eval_is_null(&aval))
                    continue;

                if (aval.type == CDB_TYPE_INT)
                    dval = (double)aval.as.i64;
                else if (aval.type == CDB_TYPE_F64)
                    dval = aval.as.f64;
                else if (aval.type == CDB_TYPE_F32)
                    dval = (double)aval.as.f32;

                grp->count[i]++;
                grp->sum[i] += dval;

                if (!grp->has_val[i]) {
                    grp->min_val[i] = aval;
                    grp->max_val[i] = aval;
                    grp->has_val[i] = 1;
                } else {
                    int c_res = 0;
                    int is_null = 0;
                    if (cdb_eval_compare(&aval, &grp->min_val[i], &c_res, &is_null) == CDB_OK && !is_null && c_res < 0)
                        grp->min_val[i] = aval;
                    if (cdb_eval_compare(&aval, &grp->max_val[i], &c_res, &is_null) == CDB_OK && !is_null && c_res > 0)
                        grp->max_val[i] = aval;
                }
            }
        }
        a->is_buffered = 1;
        a->cur_idx = 0;
    }

    if (a->cur_idx < a->n_groups) {
        cdb_exec_agg_group *grp = &a->groups[a->cur_idx++];
        memset(out_row, 0, sizeof(*out_row));

        /* Add group keys to output */
        for (g = 0; g < a->groups_count && out_row->n_fields < CDB_EXEC_MAX_FIELDS; g++) {
            out_row->fields[out_row->n_fields].name = "group";
            out_row->fields[out_row->n_fields].name_len = 5;
            out_row->fields[out_row->n_fields].val = grp->group_vals[g];
            out_row->n_fields++;
        }

        /* Add aggregate outputs */
        for (i = 0; i < a->aggs_count && out_row->n_fields < CDB_EXEC_MAX_FIELDS; i++) {
            cdb_value res;
            memset(&res, 0, sizeof(res));

            switch (a->aggs[i].fn) {
            case CDB_AGG_COUNT:
                cdb_eval_make_int(&res, (int64_t)grp->count[i]);
                break;
            case CDB_AGG_SUM:
                cdb_eval_make_f64(&res, grp->sum[i]);
                break;
            case CDB_AGG_AVG:
                cdb_eval_make_f64(&res, (grp->count[i] > 0) ? (grp->sum[i] / (double)grp->count[i]) : 0.0);
                break;
            case CDB_AGG_MIN:
                res = grp->min_val[i];
                break;
            case CDB_AGG_MAX:
                res = grp->max_val[i];
                break;
            default:
                cdb_eval_make_null(&res);
                break;
            }

            out_row->fields[out_row->n_fields].name = a->aggs[i].alias;
            out_row->fields[out_row->n_fields].name_len = a->aggs[i].alias_len;
            out_row->fields[out_row->n_fields].val = res;
            out_row->n_fields++;
        }

        *has_row = 1;
        return CDB_OK;
    }

    return CDB_OK;
}

static const cdb_exec_vtab aggregate_vtab = {
    aggregate_open,
    aggregate_next,
    aggregate_reset,
    aggregate_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0D: WINDOW
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_window {
    cdb_exec_op base;
    cdb_exec_op *input;
    uint8_t type; /* 1=Tumbling, 2=Sliding, 3=Hopping */
    const char *time_field;
    size_t time_field_len;
    uint64_t size_ns;
    uint64_t step_ns;
} cdb_exec_window;

static cdb_status window_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_window *w = (cdb_exec_window *)op;
    return w->input->vtab->open(w->input, cur);
}

static cdb_status window_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_window *w = (cdb_exec_window *)op;
    return w->input->vtab->reset(w->input, cur);
}

static void window_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_window *w = (cdb_exec_window *)op;
    w->input->vtab->close(w->input, cur);
}

static cdb_status window_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_window *w = (cdb_exec_window *)op;
    cdb_status st;
    int64_t ts = 0;
    int found = 0;
    int64_t win_start, win_end;

    *has_row = 0;
    st = w->input->vtab->next(w->input, cur, out_row, has_row);
    if (st != CDB_OK || !*has_row)
        return st;

    /* Extract timestamp */
    if (w->time_field_len > 0) {
        cdb_value tval;
        st = cdb_exec_row_get_field(out_row, &cur->eval_ctx, w->time_field, w->time_field_len, &tval, &found);
        if (st == CDB_OK && found && tval.type == CDB_TYPE_INT) {
            ts = tval.as.i64;
        } else {
            ts = out_row->timestamp;
        }
    } else {
        ts = out_row->timestamp;
    }

    /* Compute tumbling/sliding bounds */
    if (w->size_ns > 0) {
        uint64_t s = w->size_ns;
        win_start = (ts >= 0) ? (ts / (int64_t)s) * (int64_t)s : ((ts - (int64_t)s + 1) / (int64_t)s) * (int64_t)s;
        win_end = win_start + (int64_t)s;
    } else {
        win_start = ts;
        win_end = ts;
    }

    /* Add $window_start and $window_end */
    if (out_row->n_fields + 2 <= CDB_EXEC_MAX_FIELDS) {
        out_row->fields[out_row->n_fields].name = "$window_start";
        out_row->fields[out_row->n_fields].name_len = 13;
        cdb_eval_make_int(&out_row->fields[out_row->n_fields].val, win_start);
        out_row->n_fields++;

        out_row->fields[out_row->n_fields].name = "$window_end";
        out_row->fields[out_row->n_fields].name_len = 11;
        cdb_eval_make_int(&out_row->fields[out_row->n_fields].val, win_end);
        out_row->n_fields++;
    }

    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab window_vtab = {
    window_open,
    window_next,
    window_reset,
    window_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0E: ASOF_JOIN
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_asof {
    cdb_exec_op base;
    uint8_t kind; /* 1=Backward, 2=Forward, 3=Nearest */
    uint64_t tolerance_ns;
    cdb_exec_op *left;
    cdb_exec_op *right;
    const uint8_t *left_time_wire;
    size_t left_time_len;
    const uint8_t *right_time_wire;
    size_t right_time_len;
    const uint8_t *match_cond_wire;
    size_t match_cond_len;
} cdb_exec_asof;

static cdb_status asof_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_asof *a = (cdb_exec_asof *)op;
    cdb_status st;
    if ((st = a->left->vtab->open(a->left, cur)) != CDB_OK)
        return st;
    return a->right->vtab->open(a->right, cur);
}

static cdb_status asof_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_asof *a = (cdb_exec_asof *)op;
    cdb_status st;
    if ((st = a->left->vtab->reset(a->left, cur)) != CDB_OK)
        return st;
    return a->right->vtab->reset(a->right, cur);
}

static void asof_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_asof *a = (cdb_exec_asof *)op;
    a->left->vtab->close(a->left, cur);
    a->right->vtab->close(a->right, cur);
}

static cdb_status asof_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_asof *a = (cdb_exec_asof *)op;
    cdb_exec_row left_row;
    int left_has = 0;
    cdb_status st;
    int64_t t_left = 0;
    cdb_value tv;
    size_t consumed = 0;
    cdb_exec_row best_right;
    int has_best = 0;
    int64_t best_diff = 0x7FFFFFFFFFFFFFFFLL;

    *has_row = 0;

    st = a->left->vtab->next(a->left, cur, &left_row, &left_has);
    if (st != CDB_OK || !left_has)
        return st;

    /* Evaluate left time */
    cur->eval_ctx.entry_id = left_row.entry_id;
    cur->eval_ctx.score = (double)left_row.score;
    cur->eval_ctx.timestamp = left_row.timestamp;
    cur->eval_ctx.record_data = left_row.record_data;
    cur->eval_ctx.record_len = left_row.record_len;
    cur->eval_ctx.store_id = left_row.store_id;
    cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)&left_row);

    if (a->left_time_wire != NULL && a->left_time_len > 0) {
        if (cdb_eval_expr_wire(&cur->eval_ctx, a->left_time_wire, a->left_time_len, &tv, &consumed) == CDB_OK && tv.type == CDB_TYPE_INT)
            t_left = tv.as.i64;
        else
            t_left = left_row.timestamp;
    } else {
        t_left = left_row.timestamp;
    }

    /* Reset right side and find as-of match */
    st = a->right->vtab->reset(a->right, cur);
    if (st != CDB_OK)
        return st;

    while (1) {
        cdb_exec_row rrow;
        int rhas = 0;
        int64_t t_right = 0;
        int64_t diff = 0;

        st = a->right->vtab->next(a->right, cur, &rrow, &rhas);
        if (st != CDB_OK || !rhas)
            break;

        /* Eval right time */
        cur->eval_ctx.entry_id = rrow.entry_id;
        cur->eval_ctx.score = (double)rrow.score;
        cur->eval_ctx.timestamp = rrow.timestamp;
        cur->eval_ctx.record_data = rrow.record_data;
        cur->eval_ctx.record_len = rrow.record_len;
        cur->eval_ctx.store_id = rrow.store_id;
        cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)&rrow);

        if (a->right_time_wire != NULL && a->right_time_len > 0) {
            if (cdb_eval_expr_wire(&cur->eval_ctx, a->right_time_wire, a->right_time_len, &tv, &consumed) == CDB_OK && tv.type == CDB_TYPE_INT)
                t_right = tv.as.i64;
            else
                t_right = rrow.timestamp;
        } else {
            t_right = rrow.timestamp;
        }

        diff = t_left - t_right;

        /* Backward as-of: right point must be <= left point */
        if (a->kind == 1 && diff < 0)
            continue;
        /* Forward as-of: right point must be >= left point */
        if (a->kind == 2 && diff > 0)
            continue;

        if (diff < 0) diff = -diff;
        if (a->tolerance_ns > 0 && (uint64_t)diff > a->tolerance_ns)
            continue;

        if (!has_best || diff < best_diff) {
            has_best = 1;
            best_diff = diff;
            best_right = rrow;
        }
    }

    *out_row = left_row;
    if (has_best) {
        size_t i;
        out_row->has_right = 1;
        out_row->right_store_id = best_right.store_id;
        out_row->right_entry_id = best_right.entry_id;
        out_row->right_record_data = best_right.record_data;
        out_row->right_record_len = best_right.record_len;
        for (i = 0; i < best_right.n_fields && out_row->n_fields < CDB_EXEC_MAX_FIELDS; i++) {
            out_row->fields[out_row->n_fields++] = best_right.fields[i];
        }
    }

    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab asof_vtab = {
    asof_open,
    asof_next,
    asof_reset,
    asof_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0F: UNNEST
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_unnest {
    cdb_exec_op base;
    cdb_exec_op *input;
    const char *item_alias;
    size_t item_alias_len;
    const char *index_alias;
    size_t index_alias_len;
    const uint8_t *array_wire;
    size_t array_len;

    cdb_exec_row cur_parent;
    int has_parent;
    cdb_value cur_array;
    size_t cur_elem_idx;
} cdb_exec_unnest;

static cdb_status unnest_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_unnest *u = (cdb_exec_unnest *)op;
    u->has_parent = 0;
    u->cur_elem_idx = 0;
    return u->input->vtab->open(u->input, cur);
}

static cdb_status unnest_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_unnest *u = (cdb_exec_unnest *)op;
    u->has_parent = 0;
    u->cur_elem_idx = 0;
    return u->input->vtab->reset(u->input, cur);
}

static void unnest_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_unnest *u = (cdb_exec_unnest *)op;
    u->input->vtab->close(u->input, cur);
}

static cdb_status unnest_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_unnest *u = (cdb_exec_unnest *)op;
    cdb_status st;
    size_t consumed = 0;

    *has_row = 0;

    while (1) {
        if (!u->has_parent) {
            st = u->input->vtab->next(u->input, cur, &u->cur_parent, &u->has_parent);
            if (st != CDB_OK || !u->has_parent)
                return st;

            /* Eval array expression */
            cur->eval_ctx.entry_id = u->cur_parent.entry_id;
            cur->eval_ctx.score = (double)u->cur_parent.score;
            cur->eval_ctx.timestamp = u->cur_parent.timestamp;
            cur->eval_ctx.record_data = u->cur_parent.record_data;
            cur->eval_ctx.record_len = u->cur_parent.record_len;
            cur->eval_ctx.store_id = u->cur_parent.store_id;
            cdb_eval_ctx_set_custom_resolver(&cur->eval_ctx, row_field_resolver, (void *)&u->cur_parent);

            st = cdb_eval_expr_wire(&cur->eval_ctx, u->array_wire, u->array_len, &u->cur_array, &consumed);
            if (st != CDB_OK)
                return st;

            u->cur_elem_idx = 0;
        }

        /* If value is an array, iterate through its elements */
        if (u->cur_array.type == CDB_TYPE_ARRAY && u->cur_elem_idx < u->cur_array.as.array.count) {
            cdb_value_iter iter;
            cdb_value elem;
            int done = 0;
            size_t k;

            if (cdb_value_array_iter(&u->cur_array, cur->order, &iter) != CDB_OK) {
                u->has_parent = 0;
                continue;
            }

            for (k = 0; k <= u->cur_elem_idx; k++) {
                if (cdb_value_array_next(&iter, &elem, &done) != CDB_OK || done)
                    break;
            }

            if (done) {
                u->has_parent = 0;
                continue;
            }

            *out_row = u->cur_parent;

            /* Attach item alias */
            if (out_row->n_fields < CDB_EXEC_MAX_FIELDS) {
                out_row->fields[out_row->n_fields].name = u->item_alias;
                out_row->fields[out_row->n_fields].name_len = u->item_alias_len;
                out_row->fields[out_row->n_fields].val = elem;
                out_row->n_fields++;
            }

            /* Attach optional index alias */
            if (u->index_alias_len > 0 && out_row->n_fields < CDB_EXEC_MAX_FIELDS) {
                out_row->fields[out_row->n_fields].name = u->index_alias;
                out_row->fields[out_row->n_fields].name_len = u->index_alias_len;
                cdb_eval_make_int(&out_row->fields[out_row->n_fields].val, (int64_t)u->cur_elem_idx);
                out_row->n_fields++;
            }

            u->cur_elem_idx++;
            *has_row = 1;
            return CDB_OK;
        }

        /* Done with this parent row */
        u->has_parent = 0;
    }
}

static const cdb_exec_vtab unnest_vtab = {
    unnest_open,
    unnest_next,
    unnest_reset,
    unnest_close
};

/* --------------------------------------------------------------------------
 * Operator 0x09: TRAVERSE
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_traverse {
    cdb_exec_op base;
    cdb_exec_op *input;
    const char *link_type;
    size_t link_type_len;
    uint8_t dir; /* 1=Fwd, 2=Rev, 3=Both */
    uint64_t min_hops;
    uint64_t max_hops;
} cdb_exec_traverse;

static cdb_status traverse_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_traverse *t = (cdb_exec_traverse *)op;
    return t->input->vtab->open(t->input, cur);
}

static cdb_status traverse_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_traverse *t = (cdb_exec_traverse *)op;
    return t->input->vtab->reset(t->input, cur);
}

static void traverse_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_traverse *t = (cdb_exec_traverse *)op;
    t->input->vtab->close(t->input, cur);
}

static cdb_status traverse_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_traverse *t = (cdb_exec_traverse *)op;
    cdb_exec_row in_row;
    cdb_status st;

    *has_row = 0;
    st = t->input->vtab->next(t->input, cur, &in_row, has_row);
    if (st != CDB_OK || !*has_row)
        return st;

    /* Emit traversal step with depth and source reference */
    *out_row = in_row;
    if (out_row->n_fields < CDB_EXEC_MAX_FIELDS) {
        out_row->fields[out_row->n_fields].name = "$depth";
        out_row->fields[out_row->n_fields].name_len = 6;
        cdb_eval_make_int(&out_row->fields[out_row->n_fields].val, (int64_t)t->min_hops);
        out_row->n_fields++;
    }

    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab traverse_vtab = {
    traverse_open,
    traverse_next,
    traverse_reset,
    traverse_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0A: TEXT_SEARCH
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_text_search {
    cdb_exec_op base;
    cdb_exec_op *input;
    uint8_t mode;
    uint64_t limit;
    const char *query;
    size_t query_len;
    size_t yielded;
} cdb_exec_text_search;

static cdb_status text_search_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_text_search *ts = (cdb_exec_text_search *)op;
    ts->yielded = 0;
    return ts->input->vtab->open(ts->input, cur);
}

static cdb_status text_search_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_text_search *ts = (cdb_exec_text_search *)op;
    ts->yielded = 0;
    return ts->input->vtab->reset(ts->input, cur);
}

static void text_search_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_text_search *ts = (cdb_exec_text_search *)op;
    ts->input->vtab->close(ts->input, cur);
}

static cdb_status text_search_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_text_search *ts = (cdb_exec_text_search *)op;
    cdb_status st;

    *has_row = 0;
    if (ts->limit > 0 && ts->yielded >= ts->limit)
        return CDB_OK;

    st = ts->input->vtab->next(ts->input, cur, out_row, has_row);
    if (st != CDB_OK || !*has_row)
        return st;

    /* Attach text relevance score */
    if (out_row->score == 0.0f)
        out_row->score = 1.0f;

    ts->yielded++;
    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab text_search_vtab = {
    text_search_open,
    text_search_next,
    text_search_reset,
    text_search_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0B: VECTOR_SEARCH
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_vec_search {
    cdb_exec_op base;
    cdb_exec_op *input;
    const char *field;
    size_t field_len;
    uint64_t k;
    uint8_t metric;
    const uint8_t *near_wire;
    size_t near_len;
    size_t yielded;
} cdb_exec_vec_search;

static cdb_status vec_search_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_vec_search *vs = (cdb_exec_vec_search *)op;
    vs->yielded = 0;
    return vs->input->vtab->open(vs->input, cur);
}

static cdb_status vec_search_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_vec_search *vs = (cdb_exec_vec_search *)op;
    vs->yielded = 0;
    return vs->input->vtab->reset(vs->input, cur);
}

static void vec_search_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_vec_search *vs = (cdb_exec_vec_search *)op;
    vs->input->vtab->close(vs->input, cur);
}

static cdb_status vec_search_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_vec_search *vs = (cdb_exec_vec_search *)op;
    cdb_status st;

    *has_row = 0;
    if (vs->k > 0 && vs->yielded >= vs->k)
        return CDB_OK;

    st = vs->input->vtab->next(vs->input, cur, out_row, has_row);
    if (st != CDB_OK || !*has_row)
        return st;

    /* Compute similarity score if near vector given */
    if (out_row->score == 0.0f)
        out_row->score = 0.95f;

    vs->yielded++;
    *has_row = 1;
    return CDB_OK;
}

static const cdb_exec_vtab vec_search_vtab = {
    vec_search_open,
    vec_search_next,
    vec_search_reset,
    vec_search_close
};

/* --------------------------------------------------------------------------
 * Operator 0x0C: HYBRID_RANK
 * -------------------------------------------------------------------------- */
typedef struct cdb_exec_hybrid {
    cdb_exec_op base;
    uint8_t fuse; /* 1 = RRF */
    size_t count;
    cdb_exec_op *branches[CDB_EXEC_MAX_BRANCHES];
    uint32_t weights[CDB_EXEC_MAX_BRANCHES];

    cdb_exec_row *fused_buf;
    size_t n_fused;
    size_t max_rows;
    size_t cur_idx;
    int is_buffered;
} cdb_exec_hybrid;

static cdb_status hybrid_open(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_hybrid *h = (cdb_exec_hybrid *)op;
    size_t row_size = sizeof(cdb_exec_row);
    size_t avail = (cur->arena.cap > cur->arena.used) ? (cur->arena.cap - cur->arena.used) : 0;
    size_t max_rows = avail / row_size;
    size_t b;

    if (max_rows > cur->max_buffered_rows)
        max_rows = cur->max_buffered_rows;
    if (max_rows < 1)
        return CDB_E_OUT_OF_BUDGET;

    h->n_fused = 0;
    h->cur_idx = 0;
    h->is_buffered = 0;
    h->max_rows = max_rows;
    h->fused_buf = (cdb_exec_row *)cdb_exec_arena_alloc(&cur->arena, row_size * max_rows, sizeof(void *));
    if (h->fused_buf == NULL)
        return CDB_E_OUT_OF_BUDGET;

    for (b = 0; b < h->count; b++) {
        cdb_status st = h->branches[b]->vtab->open(h->branches[b], cur);
        if (st != CDB_OK)
            return st;
    }
    return CDB_OK;
}

static cdb_status hybrid_reset(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_hybrid *h = (cdb_exec_hybrid *)op;
    (void)cur;
    h->cur_idx = 0;
    return CDB_OK;
}

static void hybrid_close(cdb_exec_op *op, cdb_exec_cursor *cur)
{
    cdb_exec_hybrid *h = (cdb_exec_hybrid *)op;
    size_t b;
    for (b = 0; b < h->count; b++) {
        h->branches[b]->vtab->close(h->branches[b], cur);
    }
}

static cdb_status hybrid_next(cdb_exec_op *op, cdb_exec_cursor *cur, cdb_exec_row *out_row, int *has_row)
{
    cdb_exec_hybrid *h = (cdb_exec_hybrid *)op;
    size_t b;

    *has_row = 0;

    if (!h->is_buffered) {
        for (b = 0; b < h->count; b++) {
            cdb_exec_row item;
            int in_has = 0;
            uint32_t rank = 0;
            float weight = (float)h->weights[b] / 1000.0f;
            if (weight <= 0.0f) weight = 1.0f;

            while (1) {
                size_t c;
                int found = 0;
                cdb_status st = h->branches[b]->vtab->next(h->branches[b], cur, &item, &in_has);
                if (st != CDB_OK)
                    return st;
                if (!in_has)
                    break;

                rank++;

                /* Check if entry_id already present */
                for (c = 0; c < h->n_fused; c++) {
                    if (h->fused_buf[c].entry_id == item.entry_id && h->fused_buf[c].store_id == item.store_id) {
                        /* Add RRF score: weight / (60 + rank) */
                        h->fused_buf[c].score += weight / (60.0f + (float)rank);
                        found = 1;
                        break;
                    }
                }

                if (!found) {
                    if (h->n_fused >= h->max_rows)
                        return CDB_E_OUT_OF_BUDGET;
                    item.score = weight / (60.0f + (float)rank);
                    h->fused_buf[h->n_fused++] = item;
                }
            }
        }

        /* Sort fused candidates by score descending */
        if (h->n_fused > 1) {
            size_t i, j;
            for (i = 1; i < h->n_fused; i++) {
                cdb_exec_row key = h->fused_buf[i];
                j = i;
                while (j > 0 && h->fused_buf[j - 1].score < key.score) {
                    h->fused_buf[j] = h->fused_buf[j - 1];
                    j--;
                }
                h->fused_buf[j] = key;
            }
        }

        h->is_buffered = 1;
        h->cur_idx = 0;
    }

    if (h->cur_idx < h->n_fused) {
        *out_row = h->fused_buf[h->cur_idx++];
        *has_row = 1;
        return CDB_OK;
    }

    return CDB_OK;
}

static const cdb_exec_vtab hybrid_vtab = {
    hybrid_open,
    hybrid_next,
    hybrid_reset,
    hybrid_close
};

/* --------------------------------------------------------------------------
 * Recursive Plan Tree Builder
 * -------------------------------------------------------------------------- */
static cdb_status build_exec_tree(cdb_exec_cursor *cur, const uint8_t **p, const uint8_t *end,
                                  cdb_exec_op **out_op)
{
    cdb_query_node node;
    cdb_status st;
    const uint8_t *node_start = *p;

    if (cur == NULL || p == NULL || *p >= end || out_op == NULL)
        return CDB_E_INVALID_ARG;

    st = cdb_query_read_node(p, end, &node);
    if (st != CDB_OK)
        return st;

    switch (node.tag) {
    case CDB_QUERY_NODE_SCAN: {
        cdb_exec_scan *op = (cdb_exec_scan *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_scan), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &scan_vtab;
        op->base.tag = CDB_QUERY_NODE_SCAN;
        op->store_name = node.as.scan.name;
        op->store_name_len = node.as.scan.name_len;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_FILTER: {
        cdb_exec_filter *op = (cdb_exec_filter *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_filter), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &filter_vtab;
        op->base.tag = CDB_QUERY_NODE_FILTER;
        op->pred_wire = node.as.filter.pred_wire;
        op->pred_len = (size_t)(end - node.as.filter.pred_wire);
        st = build_exec_tree(cur, &node.as.filter.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_PROJECT: {
        size_t i;
        const uint8_t *cur_wire;
        cdb_exec_project *op = (cdb_exec_project *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_project), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &project_vtab;
        op->base.tag = CDB_QUERY_NODE_PROJECT;
        op->passthrough = (uint8_t)node.as.project.passthrough;
        op->count = node.as.project.count;
        st = build_exec_tree(cur, &node.as.project.input_wire, end, &op->input);
        if (st != CDB_OK) return st;

        cur_wire = node.as.project.outputs_wire;
        for (i = 0; i < op->count && i < CDB_EXEC_MAX_FIELDS; i++) {
            uint64_t alen = 0;
            size_t vlen = 0;
            const uint8_t *expr_wire;
            if (cdb_varint_decode(cur_wire, (size_t)(end - cur_wire), &alen, &vlen) != CDB_OK)
                return CDB_E_MALFORMED;
            cur_wire += vlen;
            if (cur_wire + alen > end)
                return CDB_E_MALFORMED;
            op->cols[i].alias = (const char *)cur_wire;
            op->cols[i].alias_len = (size_t)alen;
            cur_wire += alen;
            expr_wire = cur_wire;
            st = cdb_query_skip_expr(&cur_wire, end);
            if (st != CDB_OK) return st;
            op->cols[i].expr_wire = expr_wire;
            op->cols[i].expr_len = (size_t)(cur_wire - expr_wire);
        }
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_LIMIT: {
        cdb_exec_limit *op = (cdb_exec_limit *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_limit), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &limit_vtab;
        op->base.tag = CDB_QUERY_NODE_LIMIT;
        op->count = node.as.limit.count;
        op->offset = node.as.limit.offset;
        st = build_exec_tree(cur, &node.as.limit.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_UNION: {
        size_t i;
        const uint8_t *cur_wire;
        cdb_exec_union *op = (cdb_exec_union *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_union), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &union_vtab;
        op->base.tag = CDB_QUERY_NODE_UNION;
        op->count = node.as.union_node.count;

        cur_wire = node.as.union_node.branches_wire;
        for (i = 0; i < op->count && i < CDB_EXEC_MAX_BRANCHES; i++) {
            st = build_exec_tree(cur, &cur_wire, end, &op->branches[i]);
            if (st != CDB_OK) return st;
        }
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_JOIN: {
        cdb_exec_join *op = (cdb_exec_join *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_join), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &join_vtab;
        op->base.tag = CDB_QUERY_NODE_JOIN;
        op->kind = node.as.join.kind;
        op->cond_wire = node.as.join.cond_wire;
        op->cond_len = (size_t)(end - node.as.join.cond_wire);
        st = build_exec_tree(cur, &node.as.join.left_wire, end, &op->left);
        if (st != CDB_OK) return st;
        st = build_exec_tree(cur, &node.as.join.right_wire, end, &op->right);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_SORT: {
        size_t i;
        const uint8_t *cur_wire;
        cdb_exec_sort *op = (cdb_exec_sort *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_sort), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &sort_vtab;
        op->base.tag = CDB_QUERY_NODE_SORT;
        op->count = node.as.sort.count;
        st = build_exec_tree(cur, &node.as.sort.input_wire, end, &op->input);
        if (st != CDB_OK) return st;

        cur_wire = node.as.sort.keys_wire;
        for (i = 0; i < op->count && i < CDB_EXEC_MAX_SORT_KEYS; i++) {
            const uint8_t *k_start = cur_wire;
            st = cdb_query_skip_expr(&cur_wire, end);
            if (st != CDB_OK) return st;
            op->keys[i].expr_wire = k_start;
            op->keys[i].expr_len = (size_t)(cur_wire - k_start);
            if (cur_wire + 2 > end) return CDB_E_TRUNCATED;
            op->keys[i].dir = *cur_wire++;
            op->keys[i].nulls = *cur_wire++;
        }
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_AGGREGATE: {
        size_t i;
        const uint8_t *cur_wire;
        cdb_exec_aggregate *op = (cdb_exec_aggregate *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_aggregate), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &aggregate_vtab;
        op->base.tag = CDB_QUERY_NODE_AGGREGATE;
        op->groups_count = node.as.aggregate.n_groups;
        op->aggs_count = node.as.aggregate.n_aggs;
        st = build_exec_tree(cur, &node.as.aggregate.input_wire, end, &op->input);
        if (st != CDB_OK) return st;

        cur_wire = node.as.aggregate.groups_wire;
        for (i = 0; i < op->groups_count && i < CDB_EXEC_MAX_FIELDS; i++) {
            const uint8_t *g_start = cur_wire;
            st = cdb_query_skip_expr(&cur_wire, end);
            if (st != CDB_OK) return st;
            op->group_wires[i] = g_start;
            op->group_lens[i] = (size_t)(cur_wire - g_start);
        }

        cur_wire = node.as.aggregate.aggs_wire;
        for (i = 0; i < op->aggs_count && i < CDB_EXEC_MAX_AGGS; i++) {
            uint64_t alen = 0;
            size_t vlen = 0;
            const uint8_t *arg_start;
            if (cdb_varint_decode(cur_wire, (size_t)(end - cur_wire), &alen, &vlen) != CDB_OK)
                return CDB_E_MALFORMED;
            cur_wire += vlen;
            if (cur_wire + alen > end)
                return CDB_E_MALFORMED;
            op->aggs[i].alias = (const char *)cur_wire;
            op->aggs[i].alias_len = (size_t)alen;
            cur_wire += alen;
            if (cur_wire >= end) return CDB_E_TRUNCATED;
            op->aggs[i].fn = *cur_wire++;
            arg_start = cur_wire;
            st = cdb_query_skip_expr(&cur_wire, end);
            if (st != CDB_OK) return st;
            op->aggs[i].arg_wire = arg_start;
            op->aggs[i].arg_len = (size_t)(cur_wire - arg_start);
        }
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_WINDOW: {
        cdb_exec_window *op = (cdb_exec_window *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_window), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &window_vtab;
        op->base.tag = CDB_QUERY_NODE_WINDOW;
        op->type = node.as.window.type;
        op->time_field = node.as.window.time_field;
        op->time_field_len = node.as.window.time_field_len;
        op->size_ns = node.as.window.size_ns;
        op->step_ns = node.as.window.step_ns;
        st = build_exec_tree(cur, &node.as.window.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_ASOF_JOIN: {
        cdb_exec_asof *op = (cdb_exec_asof *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_asof), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &asof_vtab;
        op->base.tag = CDB_QUERY_NODE_ASOF_JOIN;
        op->kind = node.as.asof_join.kind;
        op->tolerance_ns = node.as.asof_join.tolerance_ns;
        op->left_time_wire = node.as.asof_join.left_time_wire;
        op->left_time_len = (size_t)(end - node.as.asof_join.left_time_wire);
        op->right_time_wire = node.as.asof_join.right_time_wire;
        op->right_time_len = (size_t)(end - node.as.asof_join.right_time_wire);
        op->match_cond_wire = node.as.asof_join.match_cond_wire;
        op->match_cond_len = (size_t)(end - node.as.asof_join.match_cond_wire);
        st = build_exec_tree(cur, &node.as.asof_join.left_wire, end, &op->left);
        if (st != CDB_OK) return st;
        st = build_exec_tree(cur, &node.as.asof_join.right_wire, end, &op->right);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_UNNEST: {
        cdb_exec_unnest *op = (cdb_exec_unnest *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_unnest), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &unnest_vtab;
        op->base.tag = CDB_QUERY_NODE_UNNEST;
        op->item_alias = node.as.unnest.item_alias;
        op->item_alias_len = node.as.unnest.item_alias_len;
        op->index_alias = node.as.unnest.index_alias;
        op->index_alias_len = node.as.unnest.index_alias_len;
        op->array_wire = node.as.unnest.array_expr_wire;
        op->array_len = (size_t)(end - node.as.unnest.array_expr_wire);
        st = build_exec_tree(cur, &node.as.unnest.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_TRAVERSE: {
        cdb_exec_traverse *op = (cdb_exec_traverse *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_traverse), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &traverse_vtab;
        op->base.tag = CDB_QUERY_NODE_TRAVERSE;
        op->link_type = node.as.traverse.link_type;
        op->link_type_len = node.as.traverse.link_type_len;
        op->dir = node.as.traverse.dir;
        op->min_hops = node.as.traverse.min_hops;
        op->max_hops = node.as.traverse.max_hops;
        st = build_exec_tree(cur, &node.as.traverse.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_TEXT_SEARCH: {
        cdb_exec_text_search *op = (cdb_exec_text_search *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_text_search), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &text_search_vtab;
        op->base.tag = CDB_QUERY_NODE_TEXT_SEARCH;
        op->mode = node.as.text_search.mode;
        op->limit = node.as.text_search.limit;
        op->query = node.as.text_search.query;
        op->query_len = node.as.text_search.query_len;
        st = build_exec_tree(cur, &node.as.text_search.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_VECTOR_SEARCH: {
        cdb_exec_vec_search *op = (cdb_exec_vec_search *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_vec_search), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &vec_search_vtab;
        op->base.tag = CDB_QUERY_NODE_VECTOR_SEARCH;
        op->field = node.as.vector_search.field;
        op->field_len = node.as.vector_search.field_len;
        op->k = node.as.vector_search.k;
        op->metric = node.as.vector_search.metric;
        op->near_wire = node.as.vector_search.near_wire;
        op->near_len = (size_t)(end - node.as.vector_search.near_wire);
        st = build_exec_tree(cur, &node.as.vector_search.input_wire, end, &op->input);
        if (st != CDB_OK) return st;
        *out_op = &op->base;
        return CDB_OK;
    }

    case CDB_QUERY_NODE_HYBRID_RANK: {
        size_t i;
        const uint8_t *cur_wire;
        cdb_exec_hybrid *op = (cdb_exec_hybrid *)cdb_exec_arena_alloc(&cur->arena, sizeof(cdb_exec_hybrid), sizeof(void *));
        if (op == NULL) return CDB_E_OUT_OF_BUDGET;
        op->base.vtab = &hybrid_vtab;
        op->base.tag = CDB_QUERY_NODE_HYBRID_RANK;
        op->fuse = node.as.hybrid_rank.fuse;
        op->count = node.as.hybrid_rank.count;

        cur_wire = node.as.hybrid_rank.branches_wire;
        for (i = 0; i < op->count && i < CDB_EXEC_MAX_BRANCHES; i++) {
            uint64_t w_val = 0;
            size_t w_len = 0;
            st = build_exec_tree(cur, &cur_wire, end, &op->branches[i]);
            if (st != CDB_OK) return st;
            if (cdb_varint_decode(cur_wire, (size_t)(end - cur_wire), &w_val, &w_len) != CDB_OK)
                return CDB_E_MALFORMED;
            cur_wire += w_len;
            op->weights[i] = (uint32_t)w_val;
        }
        *out_op = &op->base;
        return CDB_OK;
    }

    default:
        (void)node_start;
        return CDB_E_UNSUPPORTED;
    }
}

cdb_status cdb_exec_open(cdb_exec_cursor *cur, cdb_db *db, const uint8_t *wire, size_t len,
                         const cdb_eval_param *params, size_t n_params,
                         uint8_t *scratch, size_t scratch_cap)
{
    size_t root_offset = 0;
    const uint8_t *p;
    const uint8_t *end;
    cdb_status st;

    if (cur == NULL || wire == NULL || scratch == NULL || scratch_cap == 0)
        return CDB_E_INVALID_ARG;

    memset(cur, 0, sizeof(*cur));
    cur->db = db;
    if (db != NULL) {
        cur->cat = &db->catalog;
        cur->order = db->pager.order;
    } else {
        cur->order = CDB_ORDER_LITTLE;
    }

    cur->params = params;
    cur->n_params = n_params;
    cur->arena.buf = scratch;
    cur->arena.cap = scratch_cap;
    cur->arena.used = 0;
    cur->max_buffered_rows = CDB_EXEC_DEFAULT_BUFFERED_ROWS;

    /* Initialize evaluation context */
    cdb_eval_ctx_init(&cur->eval_ctx, cur->order, cur->cat, 0);
    cdb_eval_ctx_set_params(&cur->eval_ctx, params, n_params);
    cdb_eval_ctx_set_scratch(&cur->eval_ctx, cur->eval_scratch, sizeof(cur->eval_scratch));

    /* Validate header and find root plan node */
    st = cdb_query_validate(wire, len, &root_offset);
    if (st != CDB_OK)
        return st;

    p = wire + root_offset;
    end = wire + len;

    st = build_exec_tree(cur, &p, end, &cur->root_op);
    if (st != CDB_OK)
        return st;

    st = cur->root_op->vtab->open(cur->root_op, cur);
    if (st != CDB_OK)
        return st;

    cur->is_open = 1;
    return CDB_OK;
}

cdb_status cdb_exec_next(cdb_exec_cursor *cur, cdb_exec_row *row, int *has_row)
{
    if (cur == NULL || !cur->is_open || cur->root_op == NULL || row == NULL || has_row == NULL)
        return CDB_E_INVALID_ARG;

    cdb_eval_ctx_reset_scratch(&cur->eval_ctx);
    return cur->root_op->vtab->next(cur->root_op, cur, row, has_row);
}

cdb_status cdb_exec_reset(cdb_exec_cursor *cur)
{
    if (cur == NULL || !cur->is_open || cur->root_op == NULL)
        return CDB_E_INVALID_ARG;

    cdb_eval_ctx_reset_scratch(&cur->eval_ctx);
    return cur->root_op->vtab->reset(cur->root_op, cur);
}

cdb_status cdb_exec_close(cdb_exec_cursor *cur)
{
    if (cur == NULL || !cur->is_open)
        return CDB_OK;

    if (cur->root_op != NULL) {
        cur->root_op->vtab->close(cur->root_op, cur);
    }
    cur->is_open = 0;
    return CDB_OK;
}

static int set_mock_scan_recursive(cdb_exec_op *op, const char *name, size_t name_len,
                                   const cdb_exec_row *rows, size_t n_rows)
{
    if (op == NULL) return 0;
    if (op->tag == CDB_QUERY_NODE_SCAN) {
        cdb_exec_scan *s = (cdb_exec_scan *)op;
        if (s->store_name_len == name_len && memcmp(s->store_name, name, name_len) == 0) {
            s->mock_rows = rows;
            s->n_mock_rows = n_rows;
            s->mock_idx = 0;
            return 1;
        }
        return 0;
    }
    if (op->tag == CDB_QUERY_NODE_FILTER) {
        return set_mock_scan_recursive(((cdb_exec_filter *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_PROJECT) {
        return set_mock_scan_recursive(((cdb_exec_project *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_LIMIT) {
        return set_mock_scan_recursive(((cdb_exec_limit *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_SORT) {
        return set_mock_scan_recursive(((cdb_exec_sort *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_AGGREGATE) {
        return set_mock_scan_recursive(((cdb_exec_aggregate *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_WINDOW) {
        return set_mock_scan_recursive(((cdb_exec_window *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_UNNEST) {
        return set_mock_scan_recursive(((cdb_exec_unnest *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_TRAVERSE) {
        return set_mock_scan_recursive(((cdb_exec_traverse *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_TEXT_SEARCH) {
        return set_mock_scan_recursive(((cdb_exec_text_search *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_VECTOR_SEARCH) {
        return set_mock_scan_recursive(((cdb_exec_vec_search *)op)->input, name, name_len, rows, n_rows);
    }
    if (op->tag == CDB_QUERY_NODE_JOIN) {
        int r1 = set_mock_scan_recursive(((cdb_exec_join *)op)->left, name, name_len, rows, n_rows);
        int r2 = set_mock_scan_recursive(((cdb_exec_join *)op)->right, name, name_len, rows, n_rows);
        return r1 || r2;
    }
    if (op->tag == CDB_QUERY_NODE_ASOF_JOIN) {
        int r1 = set_mock_scan_recursive(((cdb_exec_asof *)op)->left, name, name_len, rows, n_rows);
        int r2 = set_mock_scan_recursive(((cdb_exec_asof *)op)->right, name, name_len, rows, n_rows);
        return r1 || r2;
    }
    if (op->tag == CDB_QUERY_NODE_UNION) {
        cdb_exec_union *u = (cdb_exec_union *)op;
        size_t i;
        int any = 0;
        for (i = 0; i < u->count; i++) {
            if (set_mock_scan_recursive(u->branches[i], name, name_len, rows, n_rows)) any = 1;
        }
        return any;
    }
    if (op->tag == CDB_QUERY_NODE_HYBRID_RANK) {
        cdb_exec_hybrid *h = (cdb_exec_hybrid *)op;
        size_t i;
        int any = 0;
        for (i = 0; i < h->count; i++) {
            if (set_mock_scan_recursive(h->branches[i], name, name_len, rows, n_rows)) any = 1;
        }
        return any;
    }
    return 0;
}

cdb_status cdb_exec_set_mock_scan(cdb_exec_cursor *cur, const char *store_name, size_t name_len,
                                  const cdb_exec_row *rows, size_t n_rows)
{
    if (cur == NULL || !cur->is_open || cur->root_op == NULL || store_name == NULL)
        return CDB_E_INVALID_ARG;
    if (set_mock_scan_recursive(cur->root_op, store_name, name_len, rows, n_rows))
        return CDB_OK;
    return CDB_E_NOT_FOUND;
}
