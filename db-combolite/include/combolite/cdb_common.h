/* Common definitions shared by every db-combolite module. */
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
    CDB_E_READ_ONLY = -12        /* write attempted on read-only storage */
} cdb_status;

const char *cdb_status_str(cdb_status status);

/* Byte order of a file (spec 14.1.2). The values are the on-disk flag bytes. */
typedef enum cdb_byte_order {
    CDB_ORDER_LITTLE = 0x4C, /* 'L' */
    CDB_ORDER_BIG = 0x42     /* 'B' */
} cdb_byte_order;

#endif
