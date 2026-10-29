/* Document schemas (spec 8.3, 14.9.4).
 *
 * A schema is an encoded object value (spec 14.6):
 *   1  open     bool: fields the schema does not list are allowed (default true)
 *   2  fields   array of rules, in strictly ascending field-ID order
 * A rule is an object:
 *   1  field          int: the field ID it constrains (required)
 *   2  types          int: allowed types, CDB_SCHEMA_TYPE bits; 0 = any
 *   3  required       bool (default false)
 *   4  max length     int: bytes of a string or binary, elements of an array
 *   5  object schema  schema for an object value
 *   6  element types  int: allowed types of array elements; 0 = any
 *   7  element schema schema for object elements of an array
 * A schema with unknown fields is refused, so an older build never enforces
 * less than the schema says. */
#ifndef CDB_SCHEMA_H
#define CDB_SCHEMA_H

#include "combolite/cdb_value.h"

#define CDB_SCHEMA_OPEN 1u
#define CDB_SCHEMA_FIELDS 2u

#define CDB_RULE_FIELD 1u
#define CDB_RULE_TYPES 2u
#define CDB_RULE_REQUIRED 3u
#define CDB_RULE_MAX_LEN 4u
#define CDB_RULE_OBJECT 5u
#define CDB_RULE_ELEM_TYPES 6u
#define CDB_RULE_ELEM_OBJECT 7u

/* Type bits, by cdb_value_type: CDB_SCHEMA_TYPE(CDB_TYPE_STRING). Stored in
 * schemas, so the order of cdb_value_type is part of the format (14.9.4). */
#define CDB_SCHEMA_TYPE(t) (1u << (unsigned)(t))
#define CDB_SCHEMA_TYPES_ALL (CDB_SCHEMA_TYPE(CDB_TYPE_UNKNOWN) - 1u)

/* Checks that schema is a canonical, well-formed schema. *trivial is set
 * when it accepts every document (open, with no rules). CDB_E_INVALID_ARG
 * for a malformed schema, CDB_E_UNSUPPORTED for unknown fields. */
cdb_status cdb_schema_check(const uint8_t *schema, size_t len, cdb_byte_order order, int *trivial);

/* Validates an encoded object against a schema that passed cdb_schema_check.
 * CDB_E_SCHEMA if it does not conform. */
cdb_status cdb_schema_validate(const uint8_t *schema, size_t schema_len, const uint8_t *doc, size_t doc_len,
                               cdb_byte_order order);

#endif
