/* CMBQL Text Language Parser & Compiler (Spec 11.1, 11.11).
 *
 * Compiles human-readable CMBQL queries (both declarative SQL syntax and
 * pipeline Flux syntax) into the binary structured operator tree (CMBQ).
 * Compiles CMBQL stored procedures into sandboxed bytecode (CMBP).
 *
 * Provides automated 4-tier dotted path resolution, zero-heap recursive descent
 * compilation, and rich diagnostic error tracking.
 */
#ifndef CDB_COMPILER_H
#define CDB_COMPILER_H

#include "combolite/cdb_common.h"
#include "cdb_catalog.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cdb_compile_error {
    uint32_t line;
    uint32_t col;
    char msg[128];
} cdb_compile_error;

/* Compiles a CMBQL query string (either declarative SQL or pipeline syntax)
 * into binary wire format (CMBQ).
 *
 * If cat is non-NULL, enables schema lookup for unquoted dotted field names.
 * Returns CDB_OK on success, or an error status code and fills err.
 */
cdb_status cdb_compile_query(const char *src, size_t len,
                             cdb_catalog *cat,
                             uint8_t *wire_buf, size_t wire_cap,
                             size_t *out_wire_len,
                             cdb_compile_error *err);

/* Compiles a CMBQL stored procedure script into sandboxed bytecode (CMBP).
 *
 * Returns CDB_OK on success, or an error status code and fills err.
 */
cdb_status cdb_compile_proc(const char *src, size_t len,
                            cdb_catalog *cat,
                            uint8_t *bc_buf, size_t bc_cap,
                            size_t *out_bc_len,
                            cdb_compile_error *err);

#ifdef __cplusplus
}
#endif

#endif /* CDB_COMPILER_H */
