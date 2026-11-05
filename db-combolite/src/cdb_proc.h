/* Stored procedure bytecode ISA & virtual machine engine (CMBQL compiled form).
 * Spec 4.5, 11.2, 11.7; Spec 14.20.
 *
 * Implements a sandboxed, zero-heap, register-based virtual machine with
 * instruction execution budgets, control flow, mathematical dispatch,
 * mutation API calls, and streaming cursor integration.
 */
#ifndef CDB_PROC_H
#define CDB_PROC_H

#include "combolite/cdb_common.h"
#include "combolite/cdb_value.h"
#include "cdb_db.h"
#include "cdb_exec.h"
#include "cdb_math.h"

#define CDB_PROC_MAGIC "\x43\x4D\x42\x50" /* CMBP */
#define CDB_PROC_VERSION 0x01u
#define CDB_PROC_HEADER_SIZE 8u
#define CDB_PROC_MAX_REGISTERS 64u
#define CDB_PROC_MAX_CURSORS 4u

/* Bytecode instruction opcodes */
#define CDB_BC_NOP           0x00u
#define CDB_BC_LOAD_CONST    0x01u
#define CDB_BC_LOAD_PARAM    0x02u
#define CDB_BC_MOVE          0x03u
#define CDB_BC_JUMP          0x04u
#define CDB_BC_JUMP_IF_TRUE  0x05u
#define CDB_BC_JUMP_IF_FALSE 0x06u
#define CDB_BC_CALL_MATH     0x07u
#define CDB_BC_CALL_MUTATION 0x08u
#define CDB_BC_OPEN_CURSOR   0x09u
#define CDB_BC_CURSOR_NEXT   0x0Au
#define CDB_BC_CURSOR_CLOSE  0x0Bu
#define CDB_BC_RETURN        0x0Cu
#define CDB_BC_HALT          0x0Du
#define CDB_BC_SAVEPOINT     0x0Eu
#define CDB_BC_ROLLBACK_TO   0x0Fu
#define CDB_BC_RELEASE_SP    0x10u
#define CDB_BC_CALL_MUTATION_EX 0x11u

#define CDB_PROC_MAX_SAVEPOINTS 8u

/* A savepoint set by the procedure (spec 11.12.3): its ID and the point in
 * the open transaction to roll back to. Registers are not saved. */
typedef struct cdb_proc_savepoint {
    uint8_t id;
    cdb_tx_mark mark;
} cdb_proc_savepoint;

/* Mutation opcodes for CALL_MUTATION & CALL_MUTATION_EX */
#define CDB_MUT_DOC_INSERT       0x01u
#define CDB_MUT_DOC_REPLACE      0x02u
#define CDB_MUT_DOC_PATCH        0x03u
#define CDB_MUT_DOC_DELETE       0x04u
#define CDB_MUT_TABLE_INSERT     0x05u
#define CDB_MUT_TABLE_UPDATE     0x06u
#define CDB_MUT_TABLE_DELETE     0x07u
#define CDB_MUT_LINK_ADD         0x08u
#define CDB_MUT_LINK_REINFORCE   0x09u
#define CDB_MUT_LINK_REMOVE      0x0Au
#define CDB_MUT_LINK_DEF_TYPE    0x0Bu
#define CDB_MUT_TS_SERIES        0x0Cu
#define CDB_MUT_TS_APPEND        0x0Du
#define CDB_MUT_TS_DROP_BEFORE   0x0Eu
#define CDB_MUT_TS_RETENTION     0x0Fu
#define CDB_MUT_TS_APPLY_RET     0x10u
#define CDB_MUT_TS_DOWNSAMPLE    0x11u
#define CDB_MUT_VEC_INSERT       0x12u
#define CDB_MUT_VEC_DELETE       0x13u
#define CDB_MUT_BLOB_FREE        0x14u
#define CDB_MUT_CREATE_STORE     0x15u
#define CDB_MUT_DROP_STORE       0x16u
#define CDB_MUT_ADD_SCHEMA       0x17u
#define CDB_MUT_CREATE_ENCLAVE   0x18u
#define CDB_MUT_OPEN_ENCLAVE     0x19u
#define CDB_MUT_ROTATE_KEY       0x1Au
#define CDB_MUT_CHECK_ROTATION   0x1Bu
#define CDB_MUT_REGISTER_PRIN    0x1Cu
#define CDB_MUT_GRANT_ROLE       0x1Du
#define CDB_MUT_CHECK_PERM       0x1Eu
#define CDB_MUT_TRIGGER_REG      0x1Fu
#define CDB_MUT_TRIGGER_UNREG    0x20u
#define CDB_MUT_COMPACT          0x21u
#define CDB_MUT_RECLAIM          0x22u
#define CDB_MUT_RECONCILE        0x23u
#define CDB_MUT_SET_PEER         0x24u
#define CDB_MUT_SYNC_IMPORT      0x25u

/* Virtual machine context */
typedef struct cdb_proc_vm {
    cdb_db *db;
    cdb_byte_order order;

    /* Registers */
    cdb_value regs[CDB_PROC_MAX_REGISTERS];
    uint8_t n_regs;

    /* Parameters */
    const cdb_value *params;
    uint8_t n_params;

    /* Query cursor slots */
    cdb_exec_cursor cursors[CDB_PROC_MAX_CURSORS];
    uint8_t cursor_scratch[CDB_PROC_MAX_CURSORS][8192];
    int cursor_active[CDB_PROC_MAX_CURSORS];

    /* Instruction execution budget */
    uint64_t instruction_budget;
    uint64_t instructions_executed;

    /* Savepoints for error trapping and rollback */
    cdb_proc_savepoint savepoints[CDB_PROC_MAX_SAVEPOINTS];
    uint8_t n_savepoints;

    /* Return value */
    cdb_value ret_val;
    int has_returned;
} cdb_proc_vm;

/* Initializes the VM context */
void cdb_proc_vm_init(cdb_proc_vm *vm, cdb_db *db, cdb_byte_order order,
                      const cdb_value *params, uint8_t n_params,
                      uint64_t instruction_budget);

/* Executes compiled bytecode inside the sandboxed VM */
cdb_status cdb_proc_exec(cdb_proc_vm *vm, const uint8_t *bytecode, size_t len, cdb_value *out_ret);

/* Bytecode writer for compilers and assemblers */
typedef struct cdb_proc_writer {
    uint8_t *buf;
    size_t cap;
    size_t len;
    cdb_status error;
} cdb_proc_writer;

void cdb_proc_writer_init(cdb_proc_writer *w, uint8_t *buf, size_t cap, uint8_t n_regs, uint8_t n_params);
size_t cdb_proc_writer_len(const cdb_proc_writer *w);

cdb_status cdb_proc_write_load_const(cdb_proc_writer *w, uint8_t dst, const uint8_t *val_bytes, size_t val_len);
cdb_status cdb_proc_write_load_param(cdb_proc_writer *w, uint8_t dst, uint8_t param_idx);
cdb_status cdb_proc_write_move(cdb_proc_writer *w, uint8_t dst, uint8_t src);
cdb_status cdb_proc_write_jump(cdb_proc_writer *w, int16_t rel_offset);
cdb_status cdb_proc_write_jump_if_true(cdb_proc_writer *w, uint8_t cond_reg, int16_t rel_offset);
cdb_status cdb_proc_write_jump_if_false(cdb_proc_writer *w, uint8_t cond_reg, int16_t rel_offset);
cdb_status cdb_proc_write_call_math(cdb_proc_writer *w, uint8_t dst, uint16_t fn_id, uint8_t argc, const uint8_t *args);
cdb_status cdb_proc_write_call_mutation(cdb_proc_writer *w, uint8_t dst, uint8_t op, uint8_t store, uint8_t entry, uint8_t payload);
cdb_status cdb_proc_write_call_mutation_ex(cdb_proc_writer *w, uint8_t dst, uint8_t op, uint8_t argc, const uint8_t *args);
cdb_status cdb_proc_write_open_cursor(cdb_proc_writer *w, uint8_t slot, const uint8_t *query_wire, size_t query_len);
cdb_status cdb_proc_write_cursor_next(cdb_proc_writer *w, uint8_t has_row_reg, uint8_t dst_row_reg, uint8_t slot);
cdb_status cdb_proc_write_cursor_close(cdb_proc_writer *w, uint8_t slot);
cdb_status cdb_proc_write_return(cdb_proc_writer *w, uint8_t src);
cdb_status cdb_proc_write_halt(cdb_proc_writer *w);
cdb_status cdb_proc_write_savepoint(cdb_proc_writer *w, uint8_t sp_id);
cdb_status cdb_proc_write_rollback_to(cdb_proc_writer *w, uint8_t sp_id);
cdb_status cdb_proc_write_release_sp(cdb_proc_writer *w, uint8_t sp_id);
cdb_status cdb_proc_writer_finish(cdb_proc_writer *w);

#endif /* CDB_PROC_H */
