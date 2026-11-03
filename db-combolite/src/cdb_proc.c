/* Stored procedure bytecode ISA & virtual machine engine (CMBQL compiled form).
 * Spec 4.5, 11.2, 11.7; Spec 14.20.
 */
#include "cdb_proc.h"
#include "cdb_varint.h"
#include "cdb_db.h"
#include "cdb_doc.h"
#include "cdb_table.h"
#include "cdb_link.h"
#include "cdb_vec.h"
#include "cdb_ts.h"
#include "cdb_trigger.h"
#include "combolite/cdb_crdt.h"

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

static uint64_t reg_get_u64(const cdb_value *v)
{
    if (v == NULL) return 0;
    if (v->type == CDB_TYPE_INT) return (uint64_t)v->as.i64;
    if (v->type == CDB_TYPE_F64) return (uint64_t)v->as.f64;
    if (v->type == CDB_TYPE_F32) return (uint64_t)v->as.f32;
    if (v->type == CDB_TYPE_BOOL) return v->as.boolean ? 1u : 0u;
    return 0;
}

static void reg_get_str(const cdb_value *v, const char **out_str, size_t *out_len)
{
    if (v != NULL && (v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY)) {
        if (out_str) *out_str = (const char *)v->payload;
        if (out_len) *out_len = v->payload_len;
    } else {
        if (out_str) *out_str = "";
        if (out_len) *out_len = 0;
    }
}

static const char *reg_to_cstr(const cdb_value *v, char *buf, size_t buf_cap)
{
    const char *s = "";
    size_t len = 0;
    if (buf == NULL || buf_cap == 0) return "";
    reg_get_str(v, &s, &len);
    if (len >= buf_cap) len = buf_cap - 1;
    if (len > 0) memcpy(buf, s, len);
    buf[len] = '\0';
    return buf;
}

static uint8_t hex_digit(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}

static void parse_pubkey(const char *pk, size_t len, uint8_t pubkey[33])
{
    memset(pubkey, 0, 33);
    if (len == 66) {
        size_t i;
        for (i = 0; i < 33; i++) {
            pubkey[i] = (uint8_t)((hex_digit(pk[2 * i]) << 4) | hex_digit(pk[2 * i + 1]));
        }
    } else if (len >= 33) {
        memcpy(pubkey, pk, 33);
    }
}

static int str_eq_ci(const char *s, size_t len, const char *target)
{
    size_t i, tlen = strlen(target);
    if (len != tlen) return 0;
    for (i = 0; i < len; i++) {
        char c1 = s[i], c2 = target[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 = (char)(c1 + 'a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 = (char)(c2 + 'a' - 'A');
        if (c1 != c2) return 0;
    }
    return 1;
}

static uint32_t parse_model_type(const cdb_value *v)
{
    if (v == NULL) return 0;
    if (v->type == CDB_TYPE_INT) return (uint32_t)v->as.i64;
    if (v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY) {
        const char *s = (const char *)v->payload;
        size_t len = v->payload_len;
        if (str_eq_ci(s, len, "table") || str_eq_ci(s, len, "relational")) return 1;
        if (str_eq_ci(s, len, "document") || str_eq_ci(s, len, "doc")) return 2;
        if (str_eq_ci(s, len, "vector") || str_eq_ci(s, len, "vec")) return 3;
        if (str_eq_ci(s, len, "timeseries") || str_eq_ci(s, len, "ts")) return 4;
        if (str_eq_ci(s, len, "link") || str_eq_ci(s, len, "links")) return 5;
    }
    return 0;
}

static unsigned parse_cardinality(const cdb_value *v)
{
    if (v == NULL) return CDB_CARD_N_M;
    if (v->type == CDB_TYPE_INT) return (unsigned)v->as.i64;
    if (v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY) {
        const char *s = (const char *)v->payload;
        size_t len = v->payload_len;
        if (str_eq_ci(s, len, "1:1")) return CDB_CARD_1_1;
        if (str_eq_ci(s, len, "1:n") || str_eq_ci(s, len, "1:N")) return CDB_CARD_1_N;
        if (str_eq_ci(s, len, "n:m") || str_eq_ci(s, len, "N:M")) return CDB_CARD_N_M;
    }
    return CDB_CARD_N_M;
}

static uint64_t parse_rotate_target(const cdb_value *v)
{
    if (v == NULL) return 0;
    if (v->type == CDB_TYPE_INT) return (uint64_t)v->as.i64;
    if (v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY) {
        const char *s = (const char *)v->payload;
        size_t len = v->payload_len;
        if (str_eq_ci(s, len, "enclave")) return 1;
        if (str_eq_ci(s, len, "master")) return 0;
    }
    return 0;
}

static void ensure_doc_object(cdb_db *db, const void *doc, size_t doc_len, uint8_t *obj_buf, size_t obj_cap, const void **out_body, size_t *out_len)
{
    cdb_value v;
    if (doc != NULL && doc_len > 0 &&
        cdb_value_validate((const uint8_t *)doc, doc_len, db->pager.order, CDB_DECIMAL_COMPACT, 0) == CDB_OK &&
        cdb_value_read((const uint8_t *)doc, doc_len, db->pager.order, &v) == CDB_OK &&
        v.type == CDB_TYPE_OBJECT) {
        *out_body = doc;
        *out_len = doc_len;
        return;
    }
    cdb_value_writer w;
    cdb_value_writer_init(&w, obj_buf, obj_cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_object(&w);
    cdb_value_field(&w, 1);
    cdb_value_write_string(&w, (const char *)doc, doc_len);
    cdb_value_end_object(&w);
    *out_body = obj_buf;
    *out_len = cdb_value_writer_len(&w);
}

static void ensure_doc_patch(cdb_db *db, const void *patch, size_t patch_len, uint8_t *patch_buf, size_t patch_cap, const void **out_patch, size_t *out_len)
{
    cdb_value v;
    if (patch != NULL && patch_len > 0 &&
        cdb_value_validate((const uint8_t *)patch, patch_len, db->pager.order, CDB_DECIMAL_COMPACT, 0) == CDB_OK &&
        cdb_value_read((const uint8_t *)patch, patch_len, db->pager.order, &v) == CDB_OK &&
        v.type == CDB_TYPE_ARRAY) {
        *out_patch = patch;
        *out_len = patch_len;
        return;
    }
    cdb_value_writer w;
    cdb_value_writer_init(&w, patch_buf, patch_cap, db->pager.order, CDB_DECIMAL_COMPACT);
    cdb_value_begin_array(&w);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, CDB_PATCH_SET);
    cdb_value_begin_array(&w);
    cdb_value_write_int(&w, 1);
    cdb_value_end_array(&w);
    cdb_value_write_string(&w, (const char *)patch, patch_len);
    cdb_value_end_array(&w);
    cdb_value_end_array(&w);
    *out_patch = patch_buf;
    *out_len = cdb_value_writer_len(&w);
}

static cdb_status resolve_store_id(cdb_db *db, const cdb_value *v, uint64_t *out_store_id)
{
    if (v == NULL || out_store_id == NULL) return CDB_E_INVALID_ARG;
    if (v->type == CDB_TYPE_INT) {
        *out_store_id = (uint64_t)v->as.i64;
        return CDB_OK;
    }
    if (v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY) {
        const char *name = (const char *)v->payload;
        size_t len = v->payload_len;
        int found = 0;
        cdb_status s;
        if (db == NULL) return CDB_E_NOT_FOUND;
        s = cdb_db_find_store(db, name, len, out_store_id, &found);
        if (s != CDB_OK) return s;
        if (!found) return CDB_E_NOT_FOUND;
        return CDB_OK;
    }
    return CDB_E_INVALID_ARG;
}

static int mut_needs_tx(uint8_t op)
{
    switch (op) {
    case CDB_MUT_DOC_INSERT:
    case CDB_MUT_DOC_REPLACE:
    case CDB_MUT_DOC_PATCH:
    case CDB_MUT_DOC_DELETE:
    case CDB_MUT_TABLE_INSERT:
    case CDB_MUT_TABLE_UPDATE:
    case CDB_MUT_TABLE_DELETE:
    case CDB_MUT_LINK_ADD:
    case CDB_MUT_LINK_REINFORCE:
    case CDB_MUT_LINK_REMOVE:
    case CDB_MUT_TS_APPEND:
    case CDB_MUT_TS_DROP_BEFORE:
    case CDB_MUT_TS_RETENTION:
    case CDB_MUT_TS_APPLY_RET:
    case CDB_MUT_VEC_INSERT:
    case CDB_MUT_VEC_DELETE:
        return 1;
    default:
        return 0;
    }
}

static cdb_status proc_exec_mutation_core(cdb_proc_vm *vm, uint8_t op, uint8_t argc, const uint8_t *args, cdb_value *out_val)
{
    cdb_status st = CDB_OK;
    int autocommit = 0;

    if (vm == NULL || vm->db == NULL) return CDB_E_INVALID_ARG;
    if (out_val) cdb_eval_make_null(out_val);

    if (mut_needs_tx(op) && !vm->db->in_tx) {
        st = cdb_tx_begin(vm->db, CDB_DURABILITY_DEFAULT);
        if (st != CDB_OK) return st;
        autocommit = 1;
    }

    switch (op) {
    case CDB_MUT_DOC_INSERT: {
        uint64_t store = 0, entry = 0;
        const char *summary = NULL;
        size_t summary_len = 0;
        const char *doc = "";
        size_t doc_len = 0;
        uint8_t wrap_buf[512];
        const void *final_doc = NULL;
        size_t final_len = 0;

        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        reg_get_str(&vm->regs[args[1]], &doc, &doc_len);
        if (argc >= 3 && vm->regs[args[2]].type != CDB_TYPE_NULL) {
            reg_get_str(&vm->regs[args[2]], &summary, &summary_len);
        }
        ensure_doc_object(vm->db, doc, doc_len, wrap_buf, sizeof(wrap_buf), &final_doc, &final_len);
        st = cdb_doc_insert(vm->db, store, summary, summary_len, final_doc, final_len, &entry);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)entry);
        }
        break;
    }

    case CDB_MUT_DOC_REPLACE: {
        uint64_t store = 0, entry = 0;
        const char *summary = NULL;
        size_t summary_len = 0;
        const char *doc = "";
        size_t doc_len = 0;
        uint8_t wrap_buf[512];
        const void *final_doc = NULL;
        size_t final_len = 0;

        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        reg_get_str(&vm->regs[args[2]], &doc, &doc_len);
        if (argc >= 4 && vm->regs[args[3]].type != CDB_TYPE_NULL) {
            reg_get_str(&vm->regs[args[3]], &summary, &summary_len);
        }
        ensure_doc_object(vm->db, doc, doc_len, wrap_buf, sizeof(wrap_buf), &final_doc, &final_len);
        st = cdb_doc_replace(vm->db, store, entry, summary, summary_len, final_doc, final_len);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_DOC_PATCH: {
        uint64_t store = 0, entry = 0;
        const char *patch = "";
        size_t patch_len = 0;
        uint8_t wrap_buf[512];
        const void *final_patch = NULL;
        size_t final_len = 0;

        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        reg_get_str(&vm->regs[args[2]], &patch, &patch_len);
        ensure_doc_patch(vm->db, patch, patch_len, wrap_buf, sizeof(wrap_buf), &final_patch, &final_len);
        st = cdb_doc_patch(vm->db, store, entry, final_patch, final_len);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_DOC_DELETE: {
        uint64_t store = 0, entry = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        st = cdb_doc_delete(vm->db, store, entry);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TABLE_INSERT: {
        uint64_t store = 0, entry = 0;
        const char *row = "";
        size_t row_len = 0;
        uint8_t wrap_buf[512];
        const void *final_row = NULL;
        size_t final_len = 0;

        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        reg_get_str(&vm->regs[args[1]], &row, &row_len);
        ensure_doc_object(vm->db, row, row_len, wrap_buf, sizeof(wrap_buf), &final_row, &final_len);
        st = cdb_table_insert(vm->db, store, final_row, final_len, &entry);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)entry);
        }
        break;
    }

    case CDB_MUT_TABLE_UPDATE: {
        uint64_t store = 0, entry = 0;
        const char *changes = "";
        size_t ch_len = 0;
        uint8_t wrap_buf[512];
        const void *final_changes = NULL;
        size_t final_len = 0;

        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        reg_get_str(&vm->regs[args[2]], &changes, &ch_len);
        ensure_doc_object(vm->db, changes, ch_len, wrap_buf, sizeof(wrap_buf), &final_changes, &final_len);
        st = cdb_table_update(vm->db, store, entry, final_changes, final_len);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TABLE_DELETE: {
        uint64_t store = 0, entry = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        st = cdb_table_delete(vm->db, store, entry);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_LINK_ADD: {
        uint64_t type = 0, s_store = 0, s_entry = 0, t_store = 0, t_entry = 0;
        float weight = 1.0f;
        int64_t ltime = 0;

        if (argc < 5) { st = CDB_E_INVALID_ARG; break; }
        type = reg_get_u64(&vm->regs[args[0]]);
        st = resolve_store_id(vm->db, &vm->regs[args[1]], &s_store);
        if (st != CDB_OK) break;
        s_entry = reg_get_u64(&vm->regs[args[2]]);
        st = resolve_store_id(vm->db, &vm->regs[args[3]], &t_store);
        if (st != CDB_OK) break;
        t_entry = reg_get_u64(&vm->regs[args[4]]);
        if (argc >= 6) weight = (float)to_double(&vm->regs[args[5]]);
        if (argc >= 7) ltime = (int64_t)reg_get_u64(&vm->regs[args[6]]);
        st = cdb_link_add(vm->db, type, cdb_ref_entry(s_store, s_entry), cdb_ref_entry(t_store, t_entry), weight, ltime);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_LINK_REINFORCE: {
        uint64_t type = 0, s_store = 0, s_entry = 0, t_store = 0, t_entry = 0;
        float weight = 1.0f;
        int64_t ltime = 0;

        if (argc < 5) { st = CDB_E_INVALID_ARG; break; }
        type = reg_get_u64(&vm->regs[args[0]]);
        st = resolve_store_id(vm->db, &vm->regs[args[1]], &s_store);
        if (st != CDB_OK) break;
        s_entry = reg_get_u64(&vm->regs[args[2]]);
        st = resolve_store_id(vm->db, &vm->regs[args[3]], &t_store);
        if (st != CDB_OK) break;
        t_entry = reg_get_u64(&vm->regs[args[4]]);
        if (argc >= 6) weight = (float)to_double(&vm->regs[args[5]]);
        if (argc >= 7) ltime = (int64_t)reg_get_u64(&vm->regs[args[6]]);
        st = cdb_link_reinforce(vm->db, type, cdb_ref_entry(s_store, s_entry), cdb_ref_entry(t_store, t_entry), weight, ltime);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_LINK_REMOVE: {
        uint64_t type = 0, s_store = 0, s_entry = 0, t_store = 0, t_entry = 0;

        if (argc < 5) { st = CDB_E_INVALID_ARG; break; }
        type = reg_get_u64(&vm->regs[args[0]]);
        st = resolve_store_id(vm->db, &vm->regs[args[1]], &s_store);
        if (st != CDB_OK) break;
        s_entry = reg_get_u64(&vm->regs[args[2]]);
        st = resolve_store_id(vm->db, &vm->regs[args[3]], &t_store);
        if (st != CDB_OK) break;
        t_entry = reg_get_u64(&vm->regs[args[4]]);
        st = cdb_link_remove(vm->db, type, cdb_ref_entry(s_store, s_entry), cdb_ref_entry(t_store, t_entry));
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_LINK_DEF_TYPE: {
        const char *name = "";
        size_t nlen = 0;
        unsigned card = CDB_CARD_N_M;
        uint64_t enf = 0, inv = 0, type_id = 0;

        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        reg_get_str(&vm->regs[args[0]], &name, &nlen);
        card = parse_cardinality(&vm->regs[args[1]]);
        enf = reg_get_u64(&vm->regs[args[2]]);
        if (argc >= 4) inv = reg_get_u64(&vm->regs[args[3]]);
        st = cdb_link_define_type(vm->db, name, nlen, card, (int)enf, (unsigned)inv, &type_id);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)type_id);
        }
        break;
    }

    case CDB_MUT_TS_SERIES: {
        uint64_t store = 0, series = 0;
        const char *name = "";
        size_t nlen = 0;
        int created = 0;

        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        reg_get_str(&vm->regs[args[1]], &name, &nlen);
        st = cdb_ts_series(vm->db, store, name, nlen, NULL, 0, NULL, &series, &created);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)series);
        }
        break;
    }

    case CDB_MUT_TS_APPEND: {
        uint64_t store = 0, series = 0;
        int64_t ts = 0;
        cdb_ts_in pin;
        uint8_t enc[64];
        cdb_value_writer vw;

        if (argc < 4) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        series = reg_get_u64(&vm->regs[args[1]]);
        ts = (int64_t)reg_get_u64(&vm->regs[args[2]]);

        memset(&pin, 0, sizeof pin);
        pin.ts = ts;
        pin.use_clock = (ts == 0) ? 1 : 0;

        if (vm->regs[args[3]].type == CDB_TYPE_STRING || vm->regs[args[3]].type == CDB_TYPE_BINARY) {
            pin.values = vm->regs[args[3]].payload;
            pin.len = vm->regs[args[3]].payload_len;
        } else {
            cdb_value_writer_init(&vw, enc, sizeof enc, vm->order, CDB_DECIMAL_UNBOUNDED);
            if (vm->regs[args[3]].type == CDB_TYPE_INT) {
                cdb_value_write_int(&vw, vm->regs[args[3]].as.i64);
            } else if (vm->regs[args[3]].type == CDB_TYPE_F64) {
                cdb_value_write_f64(&vw, vm->regs[args[3]].as.f64);
            } else if (vm->regs[args[3]].type == CDB_TYPE_F32) {
                cdb_value_write_f32(&vw, vm->regs[args[3]].as.f32);
            } else if (vm->regs[args[3]].type == CDB_TYPE_BOOL) {
                cdb_value_write_bool(&vw, vm->regs[args[3]].as.boolean);
            } else {
                cdb_value_write_null(&vw);
            }
            pin.values = enc;
            pin.len = cdb_value_writer_len(&vw);
        }

        st = cdb_ts_append(vm->db, store, series, &pin, 1);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TS_DROP_BEFORE: {
        uint64_t store = 0, series = 0;
        int64_t ts = 0;
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        series = reg_get_u64(&vm->regs[args[1]]);
        ts = (int64_t)reg_get_u64(&vm->regs[args[2]]);
        st = cdb_ts_drop_before(vm->db, store, series, ts);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TS_RETENTION: {
        uint64_t store = 0, series = 0;
        int64_t max_age = 0;
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        series = reg_get_u64(&vm->regs[args[1]]);
        max_age = (int64_t)reg_get_u64(&vm->regs[args[2]]);
        st = cdb_ts_set_retention(vm->db, store, series, max_age);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TS_APPLY_RET: {
        uint64_t store = 0, series = 0;
        int64_t now_ts = 0, dropped = 0;
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        series = reg_get_u64(&vm->regs[args[1]]);
        now_ts = (int64_t)reg_get_u64(&vm->regs[args[2]]);
        st = cdb_ts_apply_retention(vm->db, store, series, now_ts, &dropped);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, dropped);
        }
        break;
    }

    case CDB_MUT_TS_DOWNSAMPLE: {
        cdb_ts_downsample_spec spec;
        size_t points_gen = 0;
        int64_t from_ts = 0, to_ts = 0;
        if (argc < 7) { st = CDB_E_INVALID_ARG; break; }
        memset(&spec, 0, sizeof spec);
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &spec.src_store);
        if (st != CDB_OK) break;
        spec.src_series = reg_get_u64(&vm->regs[args[1]]);
        st = resolve_store_id(vm->db, &vm->regs[args[2]], &spec.dst_store);
        if (st != CDB_OK) break;
        spec.dst_series = reg_get_u64(&vm->regs[args[3]]);
        spec.bucket_step = (int64_t)reg_get_u64(&vm->regs[args[4]]);
        from_ts = (int64_t)reg_get_u64(&vm->regs[args[5]]);
        to_ts = (int64_t)reg_get_u64(&vm->regs[args[6]]);
        st = cdb_ts_downsample_run(vm->db, &spec, from_ts, to_ts, &points_gen);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)points_gen);
        }
        break;
    }

    case CDB_MUT_VEC_INSERT: {
        uint64_t store = 0, entry = 0;
        uint64_t src_store = 0, src_entry = 0;
        const char *vec_data = "";
        size_t vec_len = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        reg_get_str(&vm->regs[args[1]], &vec_data, &vec_len);
        if (argc >= 4) {
            src_store = reg_get_u64(&vm->regs[args[2]]);
            src_entry = reg_get_u64(&vm->regs[args[3]]);
        }
        st = cdb_vec_insert(vm->db, store, vec_data, vec_len, src_store, src_entry, &entry);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)entry);
        }
        break;
    }

    case CDB_MUT_VEC_DELETE: {
        uint64_t store = 0, entry = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        entry = reg_get_u64(&vm->regs[args[1]]);
        st = cdb_vec_delete(vm->db, store, entry);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_BLOB_FREE: {
        uint64_t store = 0;
        cdb_blob_ref ref;
        uint8_t page_buf[4096];
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store);
        if (st != CDB_OK) break;
        if (vm->regs[args[1]].type == CDB_TYPE_BLOB_REF) {
            ref = vm->regs[args[1]].as.blob;
        } else {
            memset(&ref, 0, sizeof ref);
            ref.first_page = reg_get_u64(&vm->regs[args[1]]);
            if (argc >= 3) ref.page_count = reg_get_u64(&vm->regs[args[2]]);
            if (argc >= 4) ref.total_len = reg_get_u64(&vm->regs[args[3]]);
        }
        st = cdb_db_blob_free(vm->db, store, &ref, page_buf);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_CREATE_STORE: {
        const char *name = "";
        size_t nlen = 0;
        uint32_t model = 0;
        uint64_t store_id = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        reg_get_str(&vm->regs[args[0]], &name, &nlen);
        model = parse_model_type(&vm->regs[args[1]]);
        st = cdb_db_create_store(vm->db, name, nlen, model, &store_id);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)store_id);
        }
        break;
    }

    case CDB_MUT_DROP_STORE: {
        uint64_t store_id = 0;
        if (argc < 1) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store_id);
        if (st != CDB_OK) break;
        st = cdb_db_drop_store(vm->db, store_id);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_ADD_SCHEMA: {
        uint64_t store_id = 0;
        const char *sch = "";
        size_t slen = 0;
        uint32_t ver = 0;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        st = resolve_store_id(vm->db, &vm->regs[args[0]], &store_id);
        if (st != CDB_OK) break;
        reg_get_str(&vm->regs[args[1]], &sch, &slen);
        st = cdb_db_add_schema(vm->db, store_id, sch, slen, &ver);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)ver);
        }
        break;
    }

    case CDB_MUT_CREATE_ENCLAVE: {
        uint64_t eid = 0;
        char kref_buf[256];
        const char *kref = NULL;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        eid = reg_get_u64(&vm->regs[args[0]]);
        if (vm->regs[args[1]].type != CDB_TYPE_NULL) {
            kref = reg_to_cstr(&vm->regs[args[1]], kref_buf, sizeof(kref_buf));
        }
        st = cdb_db_create_enclave(vm->db, eid, kref);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_OPEN_ENCLAVE: {
        uint64_t eid = 0;
        char kref_buf[256];
        const char *kref = NULL;
        if (argc < 2) { st = CDB_E_INVALID_ARG; break; }
        eid = reg_get_u64(&vm->regs[args[0]]);
        if (vm->regs[args[1]].type != CDB_TYPE_NULL) {
            kref = reg_to_cstr(&vm->regs[args[1]], kref_buf, sizeof(kref_buf));
        }
        st = cdb_db_open_enclave(vm->db, eid, kref);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_ROTATE_KEY: {
        uint64_t target = 0, eid = 0;
        char uri_buf[256];
        const char *uri = NULL;
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        target = parse_rotate_target(&vm->regs[args[0]]);
        eid = reg_get_u64(&vm->regs[args[1]]);
        if (vm->regs[args[2]].type != CDB_TYPE_NULL) {
            uri = reg_to_cstr(&vm->regs[args[2]], uri_buf, sizeof(uri_buf));
        }
        if (target == 0) {
            st = cdb_db_rotate_master_key(vm->db, uri);
        } else {
            st = cdb_db_rotate_enclave_key(vm->db, eid, uri);
        }
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_CHECK_ROTATION: {
        st = cdb_db_check_key_rotation(vm->db);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_REGISTER_PRIN: {
        uint64_t pid = 0, tid = 0;
        const char *pk = "";
        size_t pk_len = 0;
        uint8_t pubkey[33];
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        pid = reg_get_u64(&vm->regs[args[0]]);
        tid = reg_get_u64(&vm->regs[args[1]]);
        reg_get_str(&vm->regs[args[2]], &pk, &pk_len);
        parse_pubkey(pk, pk_len, pubkey);
        st = cdb_db_register_principal(vm->db, pid, (uint32_t)tid, pubkey);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_GRANT_ROLE: {
        uint64_t pid = 0, rid = 0, scope = 0, perms = 0;
        if (argc < 4) { st = CDB_E_INVALID_ARG; break; }
        pid = reg_get_u64(&vm->regs[args[0]]);
        rid = reg_get_u64(&vm->regs[args[1]]);
        scope = reg_get_u64(&vm->regs[args[2]]);
        perms = reg_get_u64(&vm->regs[args[3]]);
        st = cdb_db_grant_role(vm->db, pid, (uint32_t)rid, scope, (uint8_t)perms);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_CHECK_PERM: {
        uint64_t pid = 0, scope = 0, perms = 0;
        if (argc < 3) { st = CDB_E_INVALID_ARG; break; }
        pid = reg_get_u64(&vm->regs[args[0]]);
        scope = reg_get_u64(&vm->regs[args[1]]);
        perms = reg_get_u64(&vm->regs[args[2]]);
        st = cdb_db_check_perm(vm->db, pid, scope, (uint8_t)perms);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TRIGGER_REG: {
        const char *name = "", *proc = "";
        size_t nlen = 0, plen = 0;
        uint64_t sid = 0, ev = 0, depth = 8;
        if (argc < 4) { st = CDB_E_INVALID_ARG; break; }
        reg_get_str(&vm->regs[args[0]], &name, &nlen);
        st = resolve_store_id(vm->db, &vm->regs[args[1]], &sid);
        if (st != CDB_OK) break;
        ev = reg_get_u64(&vm->regs[args[2]]);
        reg_get_str(&vm->regs[args[3]], &proc, &plen);
        if (argc >= 5) depth = reg_get_u64(&vm->regs[args[4]]);
        st = cdb_db_trigger_register(vm->db, name, nlen, sid, (uint8_t)ev, proc, plen, (uint8_t)depth);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_TRIGGER_UNREG: {
        const char *name = "";
        size_t nlen = 0;
        if (argc < 1) { st = CDB_E_INVALID_ARG; break; }
        reg_get_str(&vm->regs[args[0]], &name, &nlen);
        st = cdb_db_trigger_unregister(vm->db, name, nlen);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_COMPACT: {
        uint64_t max_moves = 0, moved = 0, reclaimed = 0;
        if (argc >= 1) max_moves = reg_get_u64(&vm->regs[args[0]]);
        st = cdb_db_compact(vm->db, max_moves, &moved, &reclaimed);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)moved);
        }
        break;
    }

    case CDB_MUT_RECLAIM: {
        uint64_t pages_freed = 0, nibs_freed = 0;
        st = cdb_db_reclaim(vm->db, &pages_freed, &nibs_freed);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)pages_freed);
        }
        break;
    }

    case CDB_MUT_RECONCILE: {
        st = cdb_db_reconcile(vm->db);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_SET_PEER: {
        uint64_t peer_id = 0;
        if (argc < 1) { st = CDB_E_INVALID_ARG; break; }
        peer_id = reg_get_u64(&vm->regs[args[0]]);
        st = cdb_db_set_peer_id(vm->db, (uint32_t)peer_id);
        if (out_val) cdb_eval_make_int(out_val, (int64_t)st);
        break;
    }

    case CDB_MUT_SYNC_IMPORT: {
        const char *slice = "";
        size_t slen = 0, applied = 0;
        if (argc < 1) { st = CDB_E_INVALID_ARG; break; }
        reg_get_str(&vm->regs[args[0]], &slice, &slen);
        st = cdb_db_crdt_import_slice(vm->db, (const uint8_t *)slice, slen, &applied);
        if (st == CDB_OK && out_val) {
            cdb_eval_make_int(out_val, (int64_t)applied);
        }
        break;
    }

    default:
        st = CDB_E_UNSUPPORTED;
        break;
    }

    if (autocommit) {
        if (st == CDB_OK) {
            st = cdb_tx_commit(vm->db);
        } else {
            cdb_tx_abort(vm->db);
        }
    }

    return st;
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
            uint8_t m_args[3];
            if (p + 5 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            mut_op = *p++;
            store_r = *p++;
            entry_r = *p++;
            pay_r = *p++;
            if (dst >= vm->n_regs || store_r >= vm->n_regs || entry_r >= vm->n_regs || pay_r >= vm->n_regs) {
                st = CDB_E_MALFORMED; goto cleanup;
            }
            m_args[0] = store_r;
            m_args[1] = entry_r;
            m_args[2] = pay_r;
            st = proc_exec_mutation_core(vm, mut_op, 3, m_args, &vm->regs[dst]);
            if (st != CDB_OK) goto cleanup;
            break;
        }

        case CDB_BC_CALL_MUTATION_EX: {
            uint8_t dst, mut_op, argc, a_idx;
            uint8_t m_args[16];
            if (p + 3 > end) { st = CDB_E_TRUNCATED; goto cleanup; }
            dst = *p++;
            mut_op = *p++;
            argc = *p++;
            if (argc > 16 || p + argc > end) { st = CDB_E_MALFORMED; goto cleanup; }
            if (dst >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            for (a_idx = 0; a_idx < argc; a_idx++) {
                m_args[a_idx] = *p++;
                if (m_args[a_idx] >= vm->n_regs) { st = CDB_E_MALFORMED; goto cleanup; }
            }
            st = proc_exec_mutation_core(vm, mut_op, argc, m_args, &vm->regs[dst]);
            if (st != CDB_OK) goto cleanup;
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

        case CDB_BC_SAVEPOINT: {
            uint8_t sp_id;
            size_t r;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            sp_id = *p++;
            if (vm->n_savepoints < CDB_PROC_MAX_SAVEPOINTS) {
                cdb_proc_savepoint *sp = &vm->savepoints[vm->n_savepoints++];
                sp->id = sp_id;
                sp->is_active = 1;
                sp->n_regs = vm->n_regs;
                for (r = 0; r < vm->n_regs; r++) {
                    sp->saved_regs[r] = vm->regs[r];
                }
            }
            break;
        }

        case CDB_BC_ROLLBACK_TO: {
            uint8_t sp_id;
            int found = 0;
            int sidx;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            sp_id = *p++;
            for (sidx = (int)vm->n_savepoints - 1; sidx >= 0; sidx--) {
                if (vm->savepoints[sidx].is_active && vm->savepoints[sidx].id == sp_id) {
                    size_t r;
                    for (r = 0; r < vm->savepoints[sidx].n_regs; r++) {
                        vm->regs[r] = vm->savepoints[sidx].saved_regs[r];
                    }
                    vm->n_savepoints = (uint8_t)(sidx + 1);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                st = CDB_E_NOT_FOUND;
                goto cleanup;
            }
            break;
        }

        case CDB_BC_RELEASE_SP: {
            uint8_t sp_id;
            int sidx;
            if (p >= end) { st = CDB_E_TRUNCATED; goto cleanup; }
            sp_id = *p++;
            for (sidx = (int)vm->n_savepoints - 1; sidx >= 0; sidx--) {
                if (vm->savepoints[sidx].is_active && vm->savepoints[sidx].id == sp_id) {
                    vm->savepoints[sidx].is_active = 0;
                    if ((uint8_t)(sidx + 1) == vm->n_savepoints) {
                        vm->n_savepoints--;
                    }
                    break;
                }
            }
            break;
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

cdb_status cdb_proc_write_call_mutation_ex(cdb_proc_writer *w, uint8_t dst, uint8_t op, uint8_t argc, const uint8_t *args)
{
    uint8_t i;
    w_b(w, CDB_BC_CALL_MUTATION_EX);
    w_b(w, dst);
    w_b(w, op);
    w_b(w, argc);
    for (i = 0; i < argc; i++) {
        w_b(w, args[i]);
    }
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

cdb_status cdb_proc_write_savepoint(cdb_proc_writer *w, uint8_t sp_id)
{
    w_b(w, CDB_BC_SAVEPOINT);
    w_b(w, sp_id);
    update_len(w);
    return w->error;
}

cdb_status cdb_proc_write_rollback_to(cdb_proc_writer *w, uint8_t sp_id)
{
    w_b(w, CDB_BC_ROLLBACK_TO);
    w_b(w, sp_id);
    update_len(w);
    return w->error;
}

cdb_status cdb_proc_write_release_sp(cdb_proc_writer *w, uint8_t sp_id)
{
    w_b(w, CDB_BC_RELEASE_SP);
    w_b(w, sp_id);
    update_len(w);
    return w->error;
}

cdb_status cdb_proc_writer_finish(cdb_proc_writer *w)
{
    update_len(w);
    return w->error;
}
