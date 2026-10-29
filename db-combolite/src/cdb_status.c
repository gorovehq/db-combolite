#include "combolite/cdb_common.h"

const char *cdb_status_str(cdb_status status)
{
    switch (status) {
    case CDB_OK: return "ok";
    case CDB_E_INVALID_ARG: return "invalid argument";
    case CDB_E_BUFFER_TOO_SMALL: return "buffer too small";
    case CDB_E_TRUNCATED: return "truncated input";
    case CDB_E_MALFORMED: return "malformed input";
    case CDB_E_NOT_CANONICAL: return "not canonical";
    case CDB_E_OVERFLOW: return "overflow";
    case CDB_E_DEPTH: return "nesting too deep";
    case CDB_E_STATE: return "invalid state";
    case CDB_E_UNSUPPORTED: return "unsupported";
    case CDB_E_IO: return "I/O error";
    case CDB_E_NOT_FOUND: return "not found";
    case CDB_E_READ_ONLY: return "read-only storage";
    case CDB_E_CORRUPT: return "checksum mismatch";
    }
    return "unknown status";
}
