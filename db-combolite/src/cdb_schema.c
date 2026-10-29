/* Document schemas (spec 8.3, 14.9.4). */
#include <string.h>

#include "cdb_schema.h"

typedef struct rule {
    uint64_t field;
    uint64_t types;
    uint64_t elem_types;
    uint64_t max_len;
    int required;
    int has_max;
    int has_object;
    int has_elem_object;
    cdb_value object;
    cdb_value elem_object;
} rule;

static cdb_status get_types(const cdb_value *v, uint64_t *types)
{
    if (v->type != CDB_TYPE_INT || v->as.i64 < 0 || (uint64_t)v->as.i64 > CDB_SCHEMA_TYPES_ALL)
        return CDB_E_INVALID_ARG;
    *types = (uint64_t)v->as.i64;
    return CDB_OK;
}

static cdb_status parse_rule(const cdb_value *rv, cdb_byte_order o, rule *r)
{
    cdb_value_iter it;
    cdb_value v;
    uint64_t fid;
    int done, have_field = 0;
    cdb_status s;

    memset(r, 0, sizeof *r);
    if (rv->type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_value_object_iter(rv, o, &it)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_object_next(&it, &fid, &v, &done)) != CDB_OK)
            return s;
        if (done)
            break;
        switch (fid) {
        case CDB_RULE_FIELD:
            if (v.type != CDB_TYPE_INT || v.as.i64 < 0)
                return CDB_E_INVALID_ARG;
            r->field = (uint64_t)v.as.i64;
            have_field = 1;
            break;
        case CDB_RULE_TYPES:
            if ((s = get_types(&v, &r->types)) != CDB_OK)
                return s;
            break;
        case CDB_RULE_REQUIRED:
            if (v.type != CDB_TYPE_BOOL)
                return CDB_E_INVALID_ARG;
            r->required = v.as.boolean;
            break;
        case CDB_RULE_MAX_LEN:
            if (v.type != CDB_TYPE_INT || v.as.i64 < 0)
                return CDB_E_INVALID_ARG;
            r->max_len = (uint64_t)v.as.i64;
            r->has_max = 1;
            break;
        case CDB_RULE_OBJECT:
            if (v.type != CDB_TYPE_OBJECT)
                return CDB_E_INVALID_ARG;
            r->object = v;
            r->has_object = 1;
            break;
        case CDB_RULE_ELEM_TYPES:
            if ((s = get_types(&v, &r->elem_types)) != CDB_OK)
                return s;
            break;
        case CDB_RULE_ELEM_OBJECT:
            if (v.type != CDB_TYPE_OBJECT)
                return CDB_E_INVALID_ARG;
            r->elem_object = v;
            r->has_elem_object = 1;
            break;
        default:
            return CDB_E_UNSUPPORTED;
        }
    }
    return have_field ? CDB_OK : CDB_E_INVALID_ARG;
}

static cdb_status parse_schema(const cdb_value *sv, cdb_byte_order o, int *open, cdb_value *fields, int *has_fields)
{
    cdb_value_iter it;
    cdb_value v;
    uint64_t fid;
    int done;
    cdb_status s;

    *open = 1;
    *has_fields = 0;
    if (sv->type != CDB_TYPE_OBJECT)
        return CDB_E_INVALID_ARG;
    if ((s = cdb_value_object_iter(sv, o, &it)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_object_next(&it, &fid, &v, &done)) != CDB_OK)
            return s;
        if (done)
            return CDB_OK;
        if (fid == CDB_SCHEMA_OPEN) {
            if (v.type != CDB_TYPE_BOOL)
                return CDB_E_INVALID_ARG;
            *open = v.as.boolean;
        } else if (fid == CDB_SCHEMA_FIELDS) {
            if (v.type != CDB_TYPE_ARRAY)
                return CDB_E_INVALID_ARG;
            *fields = v;
            *has_fields = 1;
        } else {
            return CDB_E_UNSUPPORTED;
        }
    }
}

static cdb_status check(const cdb_value *sv, cdb_byte_order o, int *trivial)
{
    cdb_value fields, rv;
    cdb_value_iter it;
    rule r;
    uint64_t prev = 0;
    int open, has_fields, done, first = 1, sub;
    cdb_status s;

    if ((s = parse_schema(sv, o, &open, &fields, &has_fields)) != CDB_OK)
        return s;
    *trivial = open && (!has_fields || fields.as.array.count == 0);
    if (!has_fields)
        return CDB_OK;
    if ((s = cdb_value_array_iter(&fields, o, &it)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_array_next(&it, &rv, &done)) != CDB_OK)
            return s;
        if (done)
            return CDB_OK;
        if ((s = parse_rule(&rv, o, &r)) != CDB_OK)
            return s;
        if (!first && r.field <= prev)
            return CDB_E_INVALID_ARG; /* rules in strictly ascending field order */
        first = 0;
        prev = r.field;
        if (r.has_object && (s = check(&r.object, o, &sub)) != CDB_OK)
            return s;
        if (r.has_elem_object && (s = check(&r.elem_object, o, &sub)) != CDB_OK)
            return s;
    }
}

cdb_status cdb_schema_check(const uint8_t *schema, size_t len, cdb_byte_order order, int *trivial)
{
    cdb_value sv;
    if (schema == NULL || cdb_value_validate(schema, len, order, CDB_DECIMAL_COMPACT, 0) != CDB_OK
        || cdb_value_read(schema, len, order, &sv) != CDB_OK)
        return CDB_E_INVALID_ARG;
    return check(&sv, order, trivial);
}

/* -------------------------------------------------------- validation */

static cdb_status validate_object(const cdb_value *sv, const cdb_value *dv, cdb_byte_order o);

static int type_allowed(uint64_t types, cdb_value_type t)
{
    return types == 0 || (types & CDB_SCHEMA_TYPE(t)) != 0;
}

static cdb_status validate_value(const rule *r, const cdb_value *v, cdb_byte_order o)
{
    cdb_status s;
    if (!type_allowed(r->types, v->type))
        return CDB_E_SCHEMA;
    if (r->has_max) {
        if ((v->type == CDB_TYPE_STRING || v->type == CDB_TYPE_BINARY) && (uint64_t)v->payload_len > r->max_len)
            return CDB_E_SCHEMA;
        if (v->type == CDB_TYPE_ARRAY && v->as.array.count > r->max_len)
            return CDB_E_SCHEMA;
    }
    if (v->type == CDB_TYPE_OBJECT && r->has_object)
        return validate_object(&r->object, v, o);
    if (v->type == CDB_TYPE_ARRAY && (r->elem_types || r->has_elem_object)) {
        cdb_value_iter it;
        cdb_value e;
        int done;
        if ((s = cdb_value_array_iter(v, o, &it)) != CDB_OK)
            return s;
        for (;;) {
            if ((s = cdb_value_array_next(&it, &e, &done)) != CDB_OK)
                return s;
            if (done)
                break;
            if (!type_allowed(r->elem_types, e.type))
                return CDB_E_SCHEMA;
            if (e.type == CDB_TYPE_OBJECT && r->has_elem_object
                && (s = validate_object(&r->elem_object, &e, o)) != CDB_OK)
                return s;
        }
    }
    return CDB_OK;
}

/* Walks the document's fields and the schema's rules together; both are in
 * ascending field-ID order. */
static cdb_status validate_object(const cdb_value *sv, const cdb_value *dv, cdb_byte_order o)
{
    cdb_value fields, rv, v;
    cdb_value_iter ri, di;
    rule r;
    uint64_t fid;
    int open, has_fields, rules_done = 1, doc_done;
    cdb_status s;

    if (dv->type != CDB_TYPE_OBJECT)
        return CDB_E_SCHEMA;
    if (parse_schema(sv, o, &open, &fields, &has_fields) != CDB_OK)
        return CDB_E_CORRUPT; /* it passed cdb_schema_check when stored */
    if (has_fields) {
        if ((s = cdb_value_array_iter(&fields, o, &ri)) != CDB_OK)
            return s;
        if ((s = cdb_value_array_next(&ri, &rv, &rules_done)) != CDB_OK)
            return s;
        if (!rules_done && parse_rule(&rv, o, &r) != CDB_OK)
            return CDB_E_CORRUPT;
    }
    if ((s = cdb_value_object_iter(dv, o, &di)) != CDB_OK)
        return s;
    for (;;) {
        if ((s = cdb_value_object_next(&di, &fid, &v, &doc_done)) != CDB_OK)
            return s;
        /* Rules for fields before this one (or for any field, at the end)
         * are for fields the document lacks. */
        while (!rules_done && (doc_done || r.field < fid)) {
            if (r.required)
                return CDB_E_SCHEMA;
            if ((s = cdb_value_array_next(&ri, &rv, &rules_done)) != CDB_OK)
                return s;
            if (!rules_done && parse_rule(&rv, o, &r) != CDB_OK)
                return CDB_E_CORRUPT;
        }
        if (doc_done)
            return CDB_OK;
        if (!rules_done && r.field == fid) {
            if ((s = validate_value(&r, &v, o)) != CDB_OK)
                return s;
            if ((s = cdb_value_array_next(&ri, &rv, &rules_done)) != CDB_OK)
                return s;
            if (!rules_done && parse_rule(&rv, o, &r) != CDB_OK)
                return CDB_E_CORRUPT;
        } else if (!open) {
            return CDB_E_SCHEMA;
        }
    }
}

cdb_status cdb_schema_validate(const uint8_t *schema, size_t schema_len, const uint8_t *doc, size_t doc_len,
                               cdb_byte_order order)
{
    cdb_value sv, dv;
    if (cdb_value_read(schema, schema_len, order, &sv) != CDB_OK)
        return CDB_E_CORRUPT;
    if (cdb_value_read(doc, doc_len, order, &dv) != CDB_OK)
        return CDB_E_INVALID_ARG;
    return validate_object(&sv, &dv, order);
}
