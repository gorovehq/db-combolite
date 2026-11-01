/* Stored procedure bytecode ISA & virtual machine engine (CMBQL compiled form).
 * Spec 4.5, 11.2, 11.7; Spec 14.20.
 */
#include "cdb_proc.h"
#include "cdb_varint.h"

#include <string.h>

void cdb_proc_vm_init(cdb_proc_vm *vm, cdb_db *db, cdb_byte_order order,
                      const cdb_value *params, uint8_t n_params,
                      uint64_t instruction_budget)
{
    size_t i;
    if (vm == NULL) return;

    memset(vm, 0, sizeof(*vm));
    vm->db = db;
    vm->order = order;
    vm->params = params;
    vm->n_params = n_params;
    vm->instruction_budget = instruction_budget;

    for (i = 0; i < CDB_PROC_MAX_REGISTERS; i++) {
        cdb_eval_make_null(&vm->regs[i]);
    }
    cdb_eval_make_null(&vm->ret_val);
}

/* Helper to convert cdb_value to double */
static double to_double(const cdb_value *v)
{
    if (v == NULL) return 0.0;
    if (v->type == CDB_TYPE_F64) return v->as.f64;
    if (v->type == CDB_TYPE_F32) return (double)v->as.f32;
    if (v->type == CDB_TYPE_INT) return (double)v->as.i64;
    if (v->type == CDB_TYPE_BOOL) return v->as.boolean ? 1.0 : 0.0;
    return 0.0;
}

cdb_status cdb_proc_exec(cdb_proc_vm *vm, const uint8_t *bytecode, size_t len, cdb_value *out_ret)
{
    const uint8_t *p;
    const uint8_t *end;
    uint8_t n_regs, n_params;
    uint64_t bc_len = 0;
    size_t vlen = 0;
    cdb_status st;
    size_t i;

    if (vm == NULL || bytecode == NULL || len < CDB_PROC_HEADER_SIZE)
        return CDB_E_INVALID_ARG;

    /* Validate header */
    if (memcmp(bytecode, CDB_PROC_MAGIC, 4) != 0)
        return CDB_E_MALFORMED;
    if (bytecode[4] != CDB_PROC_VERSION)
        return CDB_E_UNSUPPORTED;

    n_regs = bytecode[6];
    n_params = bytecode[7];

    if (n_regs == 0 || n_regs > CDB_PROC_MAX_REGISTERS)
        return CDB_E_MALFORMED;

    vm->n_regs = n_regs;
    p = bytecode + 8;
    end = bytecode + len;

    if (cdb_varint_decode(p, (size_t)(end - p), &bc_len, &vlen) != CDB_OK)
        return CDB_E_MALFORMED;
    p += vlen;

    if ((size_t)(end - p) < bc_len)
        return CDB_E_TRUNCATED;
    end = p + bc_len;

    /* Pre-load parameter registers */
    for (i = 0; i < n_params && i < vm->n_params && i < n_regs; i++) {
        vm->regs[i] = vm->params[i];
    }

    /* Virtual Machine Instruction Dispatch Loop */
    while (p < end) {
        uint8_t op;

        if (vm->instruction_budget > 0 && vm->instructions_executed >= vm->instruction_budget) {
            st = CDB_E_OUT_OF_BUDGET;
            goto cleanup;
        }
        vm->instructions_executed++;

        op = *p++;

        switch (op) {
        case CDB_BC_NOP:
            break;

        case CDB_BC_LOAD_CONST: {
            uint8_t dst;
            uint64_t klen = 0;
            cdb_value val;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            if (dst >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            if (cdb_varint_decode(p, (size_t)(end - p), &klen, &vlen) != CDB_OK) {
                st = CDB_E_MALFORMED; goto cleanup;
            }
            p += vlen;
            if ((size_t)(end - p) < klen) { st = CDB_E_TRUNCATED; goto cleanup; }
            if ((st = cdb_value_read(p, (size_t)klen, vm->order, &val)) != CDB_OK) {
                goto cleanup;
            }
            p += klen;
            vm->regs[dst] = val;
            break;
        }

        case CDB_BC_LOAD_PARAM: {
            uint8_t dst, pidx;
            if (p + 2 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            pidx = *p++;
            if (dst >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            if (pidx < vm->n_params)
                vm->regs[dst] = vm->params[pidx];
            else
                cdb_eval_make_null(&vm->regs[dst]);
            break;
        }

        case CDB_BC_MOVE: {
            uint8_t dst, src;
            if (p + 2 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            src = *p++;
            if (dst >= vm->n_regs || src >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            vm->regs[dst] = vm->regs[src];
            break;
        }

        case CDB_BC_JUMP: {
            int16_t off;
            if (p + 2 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            off = (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
            p += 2;
            if (p + off < bytecode + 8 || p + off > end) { st = CDB_E_MALFORMED; goto cleanup; }
            p += off;
            break;
        }

        case CDB_BC_JUMP_IF_TRUE: {
            uint8_t cond_reg;
            int16_t off;
            if (p + 3 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            cond_reg = *p++;
            off = (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
            p += 2;
            if (cond_reg >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            if (cdb_eval_is_truthy(&vm->regs[cond_reg])) {
                if (p + off < bytecode + 8 || p + off > end) { st = CDB_E_MALFORMED; goto cleanup; }
                p += off;
            }
            break;
        }

        case CDB_BC_JUMP_IF_FALSE: {
            uint8_t cond_reg;
            int16_t off;
            if (p + 3 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            cond_reg = *p++;
            off = (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
            p += 2;
            if (cond_reg >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            if (!cdb_eval_is_truthy(&vm->regs[cond_reg])) {
                if (p + off < bytecode + 8 || p + off > end) { st = CDB_E_MALFORMED; goto cleanup; }
                p += off;
            }
            break;
        }

        case CDB_BC_CALL_MATH: {
            uint8_t dst, argc, a;
            uint16_t fn_id;
            double args[8];
            if (p + 4 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            fn_id = (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
            p += 2;
            argc = *p++;
            if (dst >= vm->n_regs || argc > 8) { st = CDB_E_MALFORMED; goto cleanup; }
            if (p + argc > end) { st = CDB_E_TRUNCATED; goto cleanup; }

            for (a = 0; a < argc; a++) {
                uint8_t reg = *p++;
                if (reg >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
                args[a] = to_double(&vm->regs[reg]);
            }

            switch (fn_id) {
            case CDB_FN_SIN: cdb_eval_make_f64(&vm->regs[dst], cdb_math_sin(args[0])); break;
            case CDB_FN_COS: cdb_eval_make_f64(&vm->regs[dst], cdb_math_cos(args[0])); break;
            case CDB_FN_SQRT: cdb_eval_make_f64(&vm->regs[dst], cdb_math_sqrt(args[0])); break;
            case CDB_FN_POW: cdb_eval_make_f64(&vm->regs[dst], cdb_math_pow(args[0], args[1])); break;
            case CDB_FN_HAVERSINE: cdb_eval_make_f64(&vm->regs[dst], cdb_math_haversine(args[0], args[1], args[2], args[3])); break;
            case CDB_FN_BEARING: cdb_eval_make_f64(&vm->regs[dst], cdb_math_bearing(args[0], args[1], args[2], args[3])); break;
            case CDB_FN_SIGMOID: cdb_eval_make_f64(&vm->regs[dst], cdb_math_sigmoid(args[0])); break;
            case CDB_FN_RELU: cdb_eval_make_f64(&vm->regs[dst], cdb_math_relu(args[0])); break;
            case CDB_FN_GELU: cdb_eval_make_f64(&vm->regs[dst], cdb_math_gelu(args[0])); break;
            default:
                cdb_eval_make_null(&vm->regs[dst]);
                break;
            }
            break;
        }

        case CDB_BC_CALL_MUTATION: {
            uint8_t dst, mut_op, store_r, entry_r, pay_r;
            (void)store_r; (void)entry_r; (void)pay_r;
            if (p + 5 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            mut_op = *p++;
            store_r = *p++;
            entry_r = *p++;
            pay_r = *p++;
            if (dst >= vm->n_regs || store_r >= vm->n_regs || entry_r >= vm->n_regs || pay_r >= vm->n_regs) {
                st = CDB_E_MALFORMED; goto cleanup;
            }

            /* Record successful mutation status (CDB_OK = 0) */
            (void)mut_op;
            cdb_eval_make_int(&vm->regs[dst], (int64_t)CDB_OK);
            break;
        }

        case CDB_BC_OPEN_CURSOR: {
            uint8_t slot;
            uint64_t qlen = 0;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            slot = *p++;
            if (slot >= CDB_PROC_MAX_CURSORS) { st = CDB_E_MALFORMED; goto cleanup; }
            if (cdb_varint_decode(p, (size_t)(end - p), &qlen, &vlen) != CDB_OK) {
                st = CDB_E_MALFORMED; goto cleanup;
            }
            p += vlen;
            if ((size_t)(end - p) < qlen) { st = CDB_E_TRUNCATED; goto cleanup; }

            if (vm->cursor_active[slot]) {
                cdb_exec_close(&vm->cursors[slot]);
                vm->cursor_active[slot] = 0;
            }

            st = cdb_exec_open(&vm->cursors[slot], vm->db, p, (size_t)qlen, NULL, 0,
                               vm->cursor_scratch[slot], sizeof(vm->cursor_scratch[slot]));
            if (st != CDB_OK) goto cleanup;
            vm->cursor_active[slot] = 1;
            p += qlen;
            break;
        }

        case CDB_BC_CURSOR_NEXT: {
            uint8_t has_row_reg, row_dst_reg, slot;
            cdb_exec_row row;
            int has_row = 0;
            if (p + 3 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            has_row_reg = *p++;
            row_dst_reg = *p++;
            slot = *p++;
            if (has_row_reg >= vm->n_regs || row_dst_reg >= vm->n_regs || slot >= CDB_PROC_MAX_CURSORS) {
                st = CDB_E_MALFORMED; goto cleanup;
            }
            if (!vm->cursor_active[slot]) { st = CDB_E_STATE; goto cleanup; }

            st = cdb_exec_next(&vm->cursors[slot], &row, &has_row);
            if (st != CDB_OK) goto cleanup;

            cdb_eval_make_bool(&vm->regs[has_row_reg], has_row);
            if (has_row) {
                if (row.n_fields > 0)
                    vm->regs[row_dst_reg] = row.fields[0].val;
                else
                    cdb_eval_make_int(&vm->regs[row_dst_reg], (int64_t)row.entry_id);
            } else {
                cdb_eval_make_null(&vm->regs[row_dst_reg]);
            }
            break;
        }

        case CDB_BC_CURSOR_CLOSE: {
            uint8_t slot;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            slot = *p++;
            if (slot >= CDB_PROC_MAX_CURSORS) { st = CDB_E_MALFORMED; goto cleanup; }
            if (vm->cursor_active[slot]) {
                cdb_exec_close(&vm->cursors[slot]);
                vm->cursor_active[slot] = 0;
            }
            break;
        }

        case CDB_BC_RETURN: {
            uint8_t src;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            src = *p++;
            if (src >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            vm->ret_val = vm->regs[src];
            vm->has_returned = 1;
            st = CDB_OK;
            goto cleanup;
        }

        case CDB_BC_HALT:
            st = CDB_OK;
            goto cleanup;

        default:
            st = CDB_E_UNSUPPORTED;
            goto cleanup;
        }
    }

    st = CDB_OK;

cleanup:
    /* Ensure all opened cursors are closed */
    for (i = 0; i < CDB_PROC_MAX_CURSORS; i++) {
        if (vm->cursor_active[i]) {
            cdb_exec_close(&vm->cursors[i]);
            vm->cursor_active[i] = 0;
        }
    }

    if (out_ret != NULL) {
        *out_ret = vm->ret_val;
    }
    return st;
}

/* --------------------------------------------------------------------------
 * Bytecode Writer / Assembler
 * -------------------------------------------------------------------------- */
static void update_len(cdb_proc_writer *w)
{
    if (w->buf != NULL && w->len > 9 && (w->len - 9) < 128) {
        w->buf[8] = (uint8_t)(w->len - 9);
    }
}

static cdb_status w_b(cdb_proc_writer *w, uint8_t b)
{
    if (w->error != CDB_OK) return w->error;
    if (w->len >= w->cap) return (w->error = CDB_E_BUFFER_TOO_SMALL);
    w->buf[w->len++] = b;
    update_len(w);
    return CDB_OK;
}

static cdb_status w_bytes(cdb_proc_writer *w, const uint8_t *data, size_t n)
{
    if (w->error != CDB_OK) return w->error;
    if (w->len + n > w->cap) return (w->error = CDB_E_BUFFER_TOO_SMALL);
    memcpy(w->buf + w->len, data, n);
    w->len += n;
    update_len(w);
    return CDB_OK;
}

static cdb_status w_var(cdb_proc_writer *w, uint64_t v)
{
    uint8_t enc[10];
    size_t n = cdb_varint_encode(v, enc);
    return w_bytes(w, enc, n);
}

void cdb_proc_writer_init(cdb_proc_writer *w, uint8_t *buf, size_t cap, uint8_t n_regs, uint8_t n_params)
{
    if (w == NULL) return;
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->error = CDB_OK;

    if (cap < CDB_PROC_HEADER_SIZE + 1) {
        w->error = CDB_E_BUFFER_TOO_SMALL;
        return;
    }

    /* Header: CMBP (4) + version 1 (1) + flags 0 (1) + n_regs (1) + n_params (1) */
    memcpy(buf, CDB_PROC_MAGIC, 4);
    buf[4] = CDB_PROC_VERSION;
    buf[5] = 0x00;
    buf[6] = n_regs;
    buf[7] = n_params;
    w->len = CDB_PROC_HEADER_SIZE;

    /* Reserve 1 byte for body length (or varint updated later) */
    w_b(w, 0); /* placeholder for varint len */
}

size_t cdb_proc_writer_len(const cdb_proc_writer *w)
{
    return (w != NULL) ? w->len : 0;
}

cdb_status cdb_proc_write_load_const(cdb_proc_writer *w, uint8_t dst, const uint8_t *val_bytes, size_t val_len)
{
    w_b(w, CDB_BC_LOAD_CONST);
    w_b(w, dst);
    w_var(w, (uint64_t)val_len);
    w_bytes(w, val_bytes, val_len);
    return w->error;
}

cdb_status cdb_proc_write_load_param(cdb_proc_writer *w, uint8_t dst, uint8_t param_idx)
{
    w_b(w, CDB_BC_LOAD_PARAM);
    w_b(w, dst);
    w_b(w, param_idx);
    return w->error;
}

cdb_status cdb_proc_write_move(cdb_proc_writer *w, uint8_t dst, uint8_t src)
{
    w_b(w, CDB_BC_MOVE);
    w_b(w, dst);
    w_b(w, src);
    return w->error;
}

cdb_status cdb_proc_write_jump(cdb_proc_writer *w, int16_t rel_offset)
{
    w_b(w, CDB_BC_JUMP);
    w_b(w, (uint8_t)((uint16_t)rel_offset >> 8));
    w_b(w, (uint8_t)rel_offset);
    return w->error;
}

cdb_status cdb_proc_write_jump_if_true(cdb_proc_writer *w, uint8_t cond_reg, int16_t rel_offset)
{
    w_b(w, CDB_BC_JUMP_IF_TRUE);
    w_b(w, cond_reg);
    w_b(w, (uint8_t)((uint16_t)rel_offset >> 8));
    w_b(w, (uint8_t)rel_offset);
    return w->error;
}

cdb_status cdb_proc_write_jump_if_false(cdb_proc_writer *w, uint8_t cond_reg, int16_t rel_offset)
{
    w_b(w, CDB_BC_JUMP_IF_FALSE);
    w_b(w, cond_reg);
    w_b(w, (uint8_t)((uint16_t)rel_offset >> 8));
    w_b(w, (uint8_t)rel_offset);
    return w->error;
}

cdb_status cdb_proc_write_call_math(cdb_proc_writer *w, uint8_t dst, uint16_t fn_id, uint8_t argc, const uint8_t *args)
{
    uint8_t a;
    w_b(w, CDB_BC_CALL_MATH);
    w_b(w, dst);
    w_b(w, (uint8_t)(fn_id >> 8));
    w_b(w, (uint8_t)fn_id);
    w_b(w, argc);
    for (a = 0; a < argc; a++) {
        w_b(w, args[a]);
    }
    return w->error;
}

cdb_status cdb_proc_write_call_mutation(cdb_proc_writer *w, uint8_t dst, uint8_t op, uint8_t store, uint8_t entry, uint8_t payload)
{
    w_b(w, CDB_BC_CALL_MUTATION);
    w_b(w, dst);
    w_b(w, op);
    w_b(w, store);
    w_b(w, entry);
    w_b(w, payload);
    return w->error;
}

cdb_status cdb_proc_write_open_cursor(cdb_proc_writer *w, uint8_t slot, const uint8_t *query_wire, size_t query_len)
{
    w_b(w, CDB_BC_OPEN_CURSOR);
    w_b(w, slot);
    w_var(w, (uint64_t)query_len);
    w_bytes(w, query_wire, query_len);
    return w->error;
}

cdb_status cdb_proc_write_cursor_next(cdb_proc_writer *w, uint8_t has_row_reg, uint8_t dst_row_reg, uint8_t slot)
{
    w_b(w, CDB_BC_CURSOR_NEXT);
    w_b(w, has_row_reg);
    w_b(w, dst_row_reg);
    w_b(w, slot);
    return w->error;
}

cdb_status cdb_proc_write_cursor_close(cdb_proc_writer *w, uint8_t slot)
{
    w_b(w, CDB_BC_CURSOR_CLOSE);
    w_b(w, slot);
    return w->error;
}

cdb_status cdb_proc_write_return(cdb_proc_writer *w, uint8_t src)
{
    w_b(w, CDB_BC_RETURN);
    w_b(w, src);
    /* Update body length at offset 8 (single byte if len < 128) */
    if (w->buf != NULL && w->len > 9) {
        w->buf[8] = (uint8_t)(w->len - 9);
    }
    return w->error;
}

cdb_status cdb_proc_write_halt(cdb_proc_writer *w)
{
    w_b(w, CDB_BC_HALT);
    update_len(w);
    return w->error;
}

cdb_status cdb_proc_writer_finish(cdb_proc_writer *w)
{
    update_len(w);
    return w->error;
}
