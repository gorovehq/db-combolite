/* Common definitions shared by every db-combolite module.
 *
 * Every fallible function returns a cdb_status: CDB_OK (0) on success, or
 * one of the negative error codes below. Output parameters are only
 * meaningful on CDB_OK unless a function's comment says otherwise (several
 * report a needed size alongside CDB_E_BUFFER_TOO_SMALL). */
#ifndef COMBOLITE_CDB_COMMON_H
#define COMBOLITE_CDB_COMMON_H

#define CDB_VERSION_MAJOR 0
#define CDB_VERSION_MINOR 1
#define CDB_VERSION_PATCH 0

typedef enum cdb_status {
    CDB_OK = 0,
    CDB_E_INVALID_ARG = -1,      /* a caller passed a bad argument */
    CDB_E_BUFFER_TOO_SMALL = -2, /* output buffer cannot hold the result */
    CDB_E_TRUNCATED = -3,        /* input ended before the structure did */
    CDB_E_MALFORMED = -4,        /* input is structurally invalid */
    CDB_E_NOT_CANONICAL = -5,    /* valid, but not the one canonical encoding (spec 14.6.5) */
    CDB_E_OVERFLOW = -6,         /* a number does not fit its type or limit */
    CDB_E_DEPTH = -7,            /* nesting deeper than CDB_VALUE_MAX_DEPTH */
    CDB_E_STATE = -8,            /* call not valid in the current writer state */
    CDB_E_UNSUPPORTED = -9,      /* recognised, but not supported by this build */
    CDB_E_IO = -10,              /* the storage device reported an error */
    CDB_E_NOT_FOUND = -11,       /* the named file or object does not exist */
    CDB_E_READ_ONLY = -12,       /* write attempted on read-only storage */
    CDB_E_CORRUPT = -13,         /* a checksum or integrity check failed */
    CDB_E_EXISTS = -14,          /* the name or key is already in use */
    CDB_E_SCHEMA = -15,          /* a document does not satisfy its pool's schema */
    CDB_E_BUSY = -16,            /* another process holds what is needed (spec 5) */
    CDB_E_COLLISION = -17,       /* a concurrent transaction wrote the same entry or key (spec 5.9.4) */
    CDB_E_TIMEOUT = -18,         /* a timed operation did not complete before its deadline */
    CDB_E_OUT_OF_BUDGET = -19,   /* a query or procedure exceeded its instruction or memory budget, or one
                                  * transaction claimed more new entry IDs than a process may hold (5.9.3) */
    CDB_E_SECURITY = -20         /* security, authentication or permission denied (spec 12) */
} cdb_status;

/* A short English name for a status, for logs and messages. The string is
 * static: do not free or modify it. */
const char *cdb_status_str(cdb_status status);

/* Byte order of a file (spec 14.1.2). The values are the on-disk flag bytes. */
typedef enum cdb_byte_order {
    CDB_ORDER_LITTLE = 0x4C, /* 'L' */
    CDB_ORDER_BIG = 0x42     /* 'B' */
} cdb_byte_order;

#endif
