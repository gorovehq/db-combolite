/* CMBQL Text Language Parser & Compiler (Spec 11.1, 11.11).
 *
 * Implements recursive-descent compilation of declarative SQL and pipeline Flux
 * queries into binary CMBQ operator trees, and stored procedures into CMBP bytecode.
 */
#include "cdb_compiler.h"
#include "cdb_lexer.h"
#include "cdb_query.h"
#include "cdb_proc.h"
#include "cdb_math.h"
#include "combolite/cdb_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_AST_NODES 128
#define MAX_AST_EXPRS 512
#define MAX_OUTPUTS 32
#define MAX_KEYS 16
#define MAX_STRING_POOL 8192
#define MAX_VARS 64

typedef struct ast_expr ast_expr;
typedef struct ast_node ast_node;

struct ast_expr {
    uint8_t tag; /* CDB_QUERY_EXPR_* */
    union {
        struct {
            uint8_t bytes[64];
            size_t len;
        } literal;
        struct {
            const char *path;
            size_t len;
        } field;
        struct {
            uint32_t index;
            const char *name;
            size_t name_len;
        } param;
        struct {
            uint8_t op;
            ast_expr *child;
        } unary;
        struct {
            uint8_t op;
            ast_expr *left;
            ast_expr *right;
        } binary;
        struct {
            uint16_t fn_id;
            uint16_t n_args;
            ast_expr *args[8];
        } call;
    } as;
};

typedef struct project_item {
    const char *alias;
    size_t alias_len;
    ast_expr *expr;
} project_item;

typedef struct sort_key_item {
    ast_expr *expr;
    uint8_t dir;
    uint8_t nulls;
} sort_key_item;

typedef struct agg_item {
    const char *alias;
    size_t alias_len;
    uint8_t fn;
    ast_expr *expr;
} agg_item;

struct ast_node {
    uint8_t tag; /* CDB_QUERY_NODE_* */
    union {
        struct {
            const char *store_name;
            size_t len;
        } scan;
        struct {
            ast_node *input;
            ast_expr *cond;
        } filter;
        struct {
            ast_node *input;
            int passthrough;
            size_t n_outputs;
            project_item outputs[MAX_OUTPUTS];
        } project;
        struct {
            ast_node *left;
            ast_node *right;
            uint8_t kind;
            ast_expr *cond;
        } join;
        struct {
            ast_node *input;
            size_t n_keys;
            sort_key_item keys[MAX_KEYS];
        } sort;
        struct {
            ast_node *input;
            uint64_t count;
            uint64_t offset;
        } limit;
        struct {
            ast_node *input;
            size_t n_groups;
            ast_expr *groups[MAX_OUTPUTS];
            size_t n_aggs;
            agg_item aggs[MAX_OUTPUTS];
        } aggregate;
        struct {
            ast_node *input;
            uint8_t type;
            const char *time_field;
            size_t time_field_len;
            uint64_t size_ns;
            uint64_t step_ns;
        } window;
        struct {
            ast_node *left;
            ast_node *right;
            uint8_t kind;
            uint64_t tolerance_ns;
            ast_expr *left_time;
            ast_expr *right_time;
            ast_expr *match_cond;
        } asof_join;
        struct {
            ast_node *input;
            const char *link_type;
            size_t link_len;
            uint8_t dir;
            uint64_t min_hops;
            uint64_t max_hops;
        } traverse;
        struct {
            ast_node *input;
            uint8_t mode;
            uint64_t limit;
            const char *query;
            size_t query_len;
            size_t n_fields;
            const char *fields[MAX_KEYS];
            size_t field_lens[MAX_KEYS];
        } text_search;
        struct {
            ast_node *input;
            const char *field;
            size_t field_len;
            ast_expr *near_expr;
            uint64_t k;
            uint8_t metric;
        } vector_search;
        struct {
            uint8_t fuse;
            size_t n_branches;
            ast_node *branches[MAX_KEYS];
            uint32_t weights[MAX_KEYS];
        } hybrid_rank;
        struct {
            ast_node *input;
            ast_expr *array_expr;
            const char *item_alias;
            size_t item_len;
            const char *idx_alias;
            size_t idx_len;
        } unnest;
    } as;
};

typedef struct compiler_ctx {
    cdb_lexer lex;
    cdb_catalog *cat;
    cdb_compile_error *err;

    ast_expr exprs[MAX_AST_EXPRS];
    size_t n_exprs;

    ast_node nodes[MAX_AST_NODES];
    size_t n_nodes;

    char str_pool[MAX_STRING_POOL];
    size_t str_pos;

    /* Current active store name for field qualification */
    const char *active_store;
    size_t active_store_len;
} compiler_ctx;

static void set_error(compiler_ctx *ctx, const cdb_token *tok, const char *msg) {
    if (!ctx->err) return;
    ctx->err->line = tok ? tok->line : ctx->lex.line;
    ctx->err->col = tok ? tok->col : ctx->lex.col;
    snprintf(ctx->err->msg, sizeof(ctx->err->msg), "%s", msg);
}

static const char *pool_strdup(compiler_ctx *ctx, const char *s, size_t len) {
    if (ctx->str_pos + len + 1 >= MAX_STRING_POOL) return NULL;
    char *p = &ctx->str_pool[ctx->str_pos];
    memcpy(p, s, len);
    p[len] = '\0';
    ctx->str_pos += len + 1;
    return p;
}

static ast_expr *alloc_expr(compiler_ctx *ctx) {
    if (ctx->n_exprs >= MAX_AST_EXPRS) return NULL;
    ast_expr *e = &ctx->exprs[ctx->n_exprs++];
    memset(e, 0, sizeof(*e));
    return e;
}

static ast_node *alloc_node(compiler_ctx *ctx) {
    if (ctx->n_nodes >= MAX_AST_NODES) return NULL;
    ast_node *n = &ctx->nodes[ctx->n_nodes++];
    memset(n, 0, sizeof(*n));
    return n;
}

static int match(compiler_ctx *ctx, cdb_token_kind kind, cdb_token *out_tok) {
    cdb_token tok;
    if (cdb_lexer_peek(&ctx->lex, &tok) != CDB_OK) return 0;
    if (tok.kind == kind) {
        cdb_lexer_next(&ctx->lex, &tok);
        if (out_tok) *out_tok = tok;
        return 1;
    }
    return 0;
}

static cdb_status expect(compiler_ctx *ctx, cdb_token_kind kind, const char *msg, cdb_token *out_tok) {
    cdb_token tok;
    cdb_status s = cdb_lexer_next(&ctx->lex, &tok);
    if (s != CDB_OK) {
        set_error(ctx, NULL, "Unexpected end of input");
        return s;
    }
    if (tok.kind != kind) {
        set_error(ctx, &tok, msg);
        return CDB_E_INVALID_ARG;
    }
    if (out_tok) *out_tok = tok;
    return CDB_OK;
}

static size_t encode_int_val(int64_t val, uint8_t *buf, size_t cap) {
    cdb_value_writer vw;
    cdb_value_writer_init(&vw, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_int(&vw, val) != CDB_OK) return 0;
    return cdb_value_writer_len(&vw);
}

static size_t encode_f64_val(double val, uint8_t *buf, size_t cap) {
    cdb_value_writer vw;
    cdb_value_writer_init(&vw, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_f64(&vw, val) != CDB_OK) return 0;
    return cdb_value_writer_len(&vw);
}

static size_t encode_bool_val(int val, uint8_t *buf, size_t cap) {
    cdb_value_writer vw;
    cdb_value_writer_init(&vw, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_bool(&vw, val) != CDB_OK) return 0;
    return cdb_value_writer_len(&vw);
}

static size_t encode_null_val(uint8_t *buf, size_t cap) {
    cdb_value_writer vw;
    cdb_value_writer_init(&vw, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_null(&vw) != CDB_OK) return 0;
    return cdb_value_writer_len(&vw);
}

static size_t encode_string_val(const char *s, size_t len, uint8_t *buf, size_t cap) {
    cdb_value_writer vw;
    cdb_value_writer_init(&vw, buf, cap, CDB_ORDER_LITTLE, CDB_DECIMAL_COMPACT);
    if (cdb_value_write_string(&vw, s, len) != CDB_OK) return 0;
    return cdb_value_writer_len(&vw);
}

/* Forward declarations */
static ast_expr *parse_expr(compiler_ctx *ctx);
static ast_node *parse_source(compiler_ctx *ctx);
static ast_node *parse_query(compiler_ctx *ctx);

/* Maps standard function names to math call IDs (Spec 14.19.4) */
static uint16_t lookup_function_id(const char *name, size_t len) {
    #define MATCH_FN(str, id) if (len == strlen(str) && memcmp(name, str, len) == 0) return id
    MATCH_FN("sin", CDB_FN_SIN);
    MATCH_FN("cos", CDB_FN_COS);
    MATCH_FN("tan", CDB_FN_TAN);
    MATCH_FN("asin", CDB_FN_ASIN);
    MATCH_FN("acos", CDB_FN_ACOS);
    MATCH_FN("atan", CDB_FN_ATAN);
    MATCH_FN("atan2", CDB_FN_ATAN2);
    MATCH_FN("sinh", CDB_FN_SINH);
    MATCH_FN("cosh", CDB_FN_COSH);
    MATCH_FN("tanh", CDB_FN_TANH);
    MATCH_FN("degrees", CDB_FN_DEGREES);
    MATCH_FN("radians", CDB_FN_RADIANS);
    MATCH_FN("exp", CDB_FN_EXP);
    MATCH_FN("log", CDB_FN_LN);
    MATCH_FN("ln", CDB_FN_LN);
    MATCH_FN("log10", CDB_FN_LOG10);
    MATCH_FN("log2", CDB_FN_LOG2);
    MATCH_FN("pow", CDB_FN_POW);
    MATCH_FN("sqrt", CDB_FN_SQRT);
    MATCH_FN("cbrt", CDB_FN_CBRT);
    MATCH_FN("hypot", CDB_FN_HYPOT);
    MATCH_FN("abs", CDB_FN_ABS);
    MATCH_FN("ceil", CDB_FN_CEIL);
    MATCH_FN("floor", CDB_FN_FLOOR);
    MATCH_FN("round", CDB_FN_ROUND);
    MATCH_FN("erf", CDB_FN_ERF);

    /* Geodesics & Navigation */
    MATCH_FN("distance_haversine", CDB_FN_HAVERSINE);
    MATCH_FN("haversine", CDB_FN_HAVERSINE);
    MATCH_FN("bearing", CDB_FN_BEARING);
    MATCH_FN("destination", CDB_FN_DESTINATION_LAT);
    MATCH_FN("cross_track_distance", CDB_FN_CROSS_TRACK_DISTANCE);

    /* SDR & DSP */
    MATCH_FN("fft_magnitude", CDB_FN_FFT_MAGNITUDE);
    MATCH_FN("fft_power", CDB_FN_FFT_POWER);
    MATCH_FN("rfft", CDB_FN_RFFT);
    MATCH_FN("ifft", CDB_FN_IFFT);
    MATCH_FN("iq_magnitude", CDB_FN_IQ_MAGNITUDE);
    MATCH_FN("iq_phase", CDB_FN_IQ_PHASE);
    MATCH_FN("iq_demod_fm", CDB_FN_IQ_DEMOD_FM);
    MATCH_FN("dbfs", CDB_FN_DBFS);
    MATCH_FN("fir_filter", CDB_FN_FIR_FILTER);

    /* Astronomy */
    MATCH_FN("astro_alt", CDB_FN_ASTRO_ALT);
    MATCH_FN("astro_az", CDB_FN_ASTRO_AZ);
    MATCH_FN("astro_range", CDB_FN_ASTRO_RANGE);
    MATCH_FN("astro_altaz_to_ra", CDB_FN_ASTRO_ALTAZ_TO_RA);
    MATCH_FN("astro_altaz_to_dec", CDB_FN_ASTRO_ALTAZ_TO_DEC);
    MATCH_FN("astro_radec_to_alt", CDB_FN_ASTRO_RADEC_TO_ALT);
    MATCH_FN("astro_radec_to_az", CDB_FN_ASTRO_RADEC_TO_AZ);
    MATCH_FN("astro_refraction", CDB_FN_ASTRO_REFRACTION);
    MATCH_FN("astro_angular_sep", CDB_FN_ASTRO_ANGULAR_SEP);

    /* Electronics */
    MATCH_FN("elec_thermistor_ntc", CDB_FN_ELEC_THERMISTOR_NTC);
    MATCH_FN("elec_steinhart_hart", CDB_FN_ELEC_STEINHART_HART);
    MATCH_FN("elec_adc_volts", CDB_FN_ELEC_ADC_VOLTS);
    MATCH_FN("elec_voltage_divider", CDB_FN_ELEC_VOLTAGE_DIVIDER);
    MATCH_FN("elec_parallel_r", CDB_FN_ELEC_PARALLEL_R);
    MATCH_FN("elec_dbm_to_watts", CDB_FN_ELEC_DBM_TO_WATTS);
    MATCH_FN("elec_watts_to_dbm", CDB_FN_ELEC_WATTS_TO_DBM);
    MATCH_FN("elec_rc_cutoff", CDB_FN_ELEC_RC_CUTOFF);
    MATCH_FN("elec_lc_resonance", CDB_FN_ELEC_LC_RESONANCE);
    MATCH_FN("elec_reactance_c", CDB_FN_ELEC_REACTANCE_C);
    MATCH_FN("elec_reactance_l", CDB_FN_ELEC_REACTANCE_L);

    /* Atmospheric & Fluid Dynamics */
    MATCH_FN("atmo_pressure_to_alt", CDB_FN_ATMO_PRESSURE_TO_ALT);
    MATCH_FN("atmo_alt_to_sealevel_p", CDB_FN_ATMO_ALT_TO_SEALEVEL_P);
    MATCH_FN("atmo_dew_point", CDB_FN_ATMO_DEW_POINT);
    MATCH_FN("atmo_vapor_pressure", CDB_FN_ATMO_VAPOR_PRESSURE);
    MATCH_FN("atmo_heat_index", CDB_FN_ATMO_HEAT_INDEX);
    MATCH_FN("fluid_reynolds", CDB_FN_FLUID_REYNOLDS);
    MATCH_FN("fluid_dynamic_pressure", CDB_FN_FLUID_DYNAMIC_PRESSURE);
    MATCH_FN("fluid_pitot_airspeed", CDB_FN_FLUID_PITOT_AIRSPEED);
    MATCH_FN("fluid_hydrostatic_pressure", CDB_FN_FLUID_HYDROSTATIC_PRESSURE);
    MATCH_FN("fluid_venturi_flow", CDB_FN_FLUID_VENTURI_FLOW);
    #undef MATCH_FN

    return 0;
}

/* Automated 4-tier dotted path resolution (Spec 11.11.2) */
static ast_expr *resolve_field_identifier(compiler_ctx *ctx, const cdb_token *tok) {
    ast_expr *e = alloc_expr(ctx);
    if (!e) return NULL;
    e->tag = CDB_QUERY_EXPR_FIELD;

    /* Tier 1: Quoted identifier */
    if (tok->is_quoted) {
        char buf[256];
        size_t len = 0;
        cdb_token_unescape_string(tok, buf, sizeof(buf), &len);
        e->as.field.path = pool_strdup(ctx, buf, len);
        e->as.field.len = len;
        return e;
    }

    /* Unquoted identifier */
    const char *text = tok->start;
    size_t len = tok->len;

    /* Check if token has dots */
    int has_dot = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '.') {
            has_dot = 1;
            break;
        }
    }

    if (!has_dot) {
        e->as.field.path = pool_strdup(ctx, text, len);
        e->as.field.len = len;
        return e;
    }

    /* Tier 2: Check schema catalog for exact literal match */
    if (ctx->cat != NULL && ctx->active_store != NULL) {
        uint64_t store_id = 0;
        int sfound = 0;
        if (cdb_catalog_find_store(ctx->cat, ctx->active_store, ctx->active_store_len, &store_id, &sfound) == CDB_OK && sfound) {
            uint64_t fid = 0;
            int found = 0;
            if (cdb_catalog_field_lookup(ctx->cat, store_id, text, len, &fid, &found) == CDB_OK && found) {
                /* Exact field match in catalog schema: preserve literal dot */
                e->as.field.path = pool_strdup(ctx, text, len);
                e->as.field.len = len;
                return e;
            }
        }
    }

    /* Tier 3: Check if prefix is active table/store */
    if (ctx->active_store != NULL && len > ctx->active_store_len + 1 &&
        memcmp(text, ctx->active_store, ctx->active_store_len) == 0 &&
        text[ctx->active_store_len] == '.') {
        /* Qualified field: strip store prefix */
        const char *sub = text + ctx->active_store_len + 1;
        size_t sub_len = len - (ctx->active_store_len + 1);
        e->as.field.path = pool_strdup(ctx, sub, sub_len);
        e->as.field.len = sub_len;
        return e;
    }

    /* Tier 4: Schemaless document fallback: replace dots with 0x1F Unit Separator */
    char buf[256];
    size_t out_idx = 0;
    for (size_t i = 0; i < len && out_idx + 1 < sizeof(buf); ++i) {
        if (text[i] == '.') {
            buf[out_idx++] = CDB_QUERY_PATH_SEP; /* 0x1F */
        } else {
            buf[out_idx++] = text[i];
        }
    }
    buf[out_idx] = '\0';
    e->as.field.path = pool_strdup(ctx, buf, out_idx);
    e->as.field.len = out_idx;
    return e;
}

static ast_expr *parse_primary(compiler_ctx *ctx) {
    cdb_token tok;
    if (cdb_lexer_next(&ctx->lex, &tok) != CDB_OK) return NULL;

    /* Integer literal */
    if (tok.kind == CDB_TOK_INT) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_LITERAL;
        size_t elen = encode_int_val(tok.num_val.i64, e->as.literal.bytes, sizeof(e->as.literal.bytes));
        e->as.literal.len = elen;
        return e;
    }

    /* Float literal */
    if (tok.kind == CDB_TOK_FLOAT) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_LITERAL;
        size_t elen = encode_f64_val(tok.num_val.f64, e->as.literal.bytes, sizeof(e->as.literal.bytes));
        e->as.literal.len = elen;
        return e;
    }

    /* Boolean literals */
    if (tok.kind == CDB_TOK_KW_TRUE || tok.kind == CDB_TOK_KW_FALSE) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_LITERAL;
        int bval = (tok.kind == CDB_TOK_KW_TRUE) ? 1 : 0;
        size_t elen = encode_bool_val(bval, e->as.literal.bytes, sizeof(e->as.literal.bytes));
        e->as.literal.len = elen;
        return e;
    }

    /* NULL literal */
    if (tok.kind == CDB_TOK_KW_NULL) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_LITERAL;
        size_t elen = encode_null_val(e->as.literal.bytes, sizeof(e->as.literal.bytes));
        e->as.literal.len = elen;
        return e;
    }

    /* String literal */
    if (tok.kind == CDB_TOK_STRING) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        char unesc[256];
        size_t ulen = 0;
        cdb_token_unescape_string(&tok, unesc, sizeof(unesc), &ulen);
        e->tag = CDB_QUERY_EXPR_LITERAL;
        size_t elen = encode_string_val(unesc, ulen, e->as.literal.bytes, sizeof(e->as.literal.bytes));
        e->as.literal.len = elen;
        return e;
    }

    /* Parameter: $param */
    if (tok.kind == CDB_TOK_PARAM) {
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_PARAM;
        const char *name = tok.start + 1; /* Skip '$' */
        size_t nlen = tok.len - 1;
        /* If all digits, treat as param index */
        int all_digits = 1;
        for (size_t i = 0; i < nlen; ++i) {
            if (name[i] < '0' || name[i] > '9') {
                all_digits = 0;
                break;
            }
        }
        if (all_digits && nlen > 0) {
            e->as.param.index = (uint32_t)strtoul(name, NULL, 10);
        } else {
            e->as.param.name = pool_strdup(ctx, name, nlen);
            e->as.param.name_len = nlen;
        }
        return e;
    }

    /* Explicit Field("name") */
    if (tok.kind == CDB_TOK_KW_FIELD) {
        if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after Field", NULL) != CDB_OK) return NULL;
        cdb_token str_tok;
        if (expect(ctx, CDB_TOK_STRING, "Expected string argument in Field(...)", &str_tok) != CDB_OK) return NULL;
        if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after Field(string)", NULL) != CDB_OK) return NULL;
        char buf[256];
        size_t len = 0;
        cdb_token_unescape_string(&str_tok, buf, sizeof(buf), &len);
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_FIELD;
        e->as.field.path = pool_strdup(ctx, buf, len);
        e->as.field.len = len;
        return e;
    }

    /* Parenthesized expression: ( expr ) */
    if (tok.kind == CDB_TOK_LPAREN) {
        ast_expr *e = parse_expr(ctx);
        if (!e) return NULL;
        if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' to close expression", NULL) != CDB_OK) return NULL;
        return e;
    }

    /* Identifier or function call */
    if (tok.kind == CDB_TOK_IDENT) {
        cdb_token next;
        if (cdb_lexer_peek(&ctx->lex, &next) == CDB_OK && next.kind == CDB_TOK_LPAREN) {
            /* Function call */
            cdb_lexer_next(&ctx->lex, &next); /* Consume '(' */
            uint16_t fn_id = lookup_function_id(tok.start, (size_t)tok.len);
            if (fn_id == 0) {
                set_error(ctx, &tok, "Unknown function call");
                return NULL;
            }
            ast_expr *e = alloc_expr(ctx);
            if (!e) return NULL;
            e->tag = CDB_QUERY_EXPR_CALL;
            e->as.call.fn_id = fn_id;
            e->as.call.n_args = 0;

            if (!match(ctx, CDB_TOK_RPAREN, NULL)) {
                do {
                    ast_expr *arg = parse_expr(ctx);
                    if (!arg) return NULL;
                    if (e->as.call.n_args < 8) {
                        e->as.call.args[e->as.call.n_args++] = arg;
                    }
                } while (match(ctx, CDB_TOK_COMMA, NULL));
                if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after arguments", NULL) != CDB_OK) return NULL;
            }
            return e;
        }

        /* Check for dotted identifier: ident.ident */
        cdb_token dot;
        if (cdb_lexer_peek(&ctx->lex, &dot) == CDB_OK && dot.kind == CDB_TOK_DOT) {
            /* Assemble dotted identifier into single string */
            char dpath[256];
            size_t dlen = (size_t)tok.len < sizeof(dpath) - 1 ? (size_t)tok.len : sizeof(dpath) - 1;
            memcpy(dpath, tok.start, dlen);
            while (match(ctx, CDB_TOK_DOT, NULL)) {
                cdb_token part;
                if (cdb_lexer_next(&ctx->lex, &part) != CDB_OK) return NULL;
                if (dlen + 1 < sizeof(dpath)) dpath[dlen++] = '.';
                size_t plen = (size_t)part.len < sizeof(dpath) - 1 - dlen ? (size_t)part.len : sizeof(dpath) - 1 - dlen;
                memcpy(&dpath[dlen], part.start, plen);
                dlen += plen;
            }
            dpath[dlen] = '\0';
            cdb_token synthetic;
            memset(&synthetic, 0, sizeof(synthetic));
            synthetic.start = dpath;
            synthetic.len = (uint32_t)dlen;
            synthetic.kind = CDB_TOK_IDENT;
            return resolve_field_identifier(ctx, &synthetic);
        }

        /* Standard single field identifier */
        return resolve_field_identifier(ctx, &tok);
    }

    set_error(ctx, &tok, "Unexpected token in expression");
    return NULL;
}

static ast_expr *parse_unary(compiler_ctx *ctx) {
    if (match(ctx, CDB_TOK_KW_NOT, NULL) || match(ctx, CDB_TOK_BANG, NULL)) {
        ast_expr *child = parse_unary(ctx);
        if (!child) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_UNARY;
        e->as.unary.op = CDB_UNARY_NOT;
        e->as.unary.child = child;
        return e;
    }
    if (match(ctx, CDB_TOK_MINUS, NULL)) {
        ast_expr *child = parse_unary(ctx);
        if (!child) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_UNARY;
        e->as.unary.op = CDB_UNARY_NEG;
        e->as.unary.child = child;
        return e;
    }
    return parse_primary(ctx);
}

static ast_expr *parse_mul(compiler_ctx *ctx) {
    ast_expr *left = parse_unary(ctx);
    if (!left) return NULL;
    while (1) {
        uint8_t op = 0;
        if (match(ctx, CDB_TOK_STAR, NULL)) op = CDB_BIN_MUL;
        else if (match(ctx, CDB_TOK_SLASH, NULL)) op = CDB_BIN_DIV;
        else if (match(ctx, CDB_TOK_PERCENT, NULL)) op = CDB_BIN_MOD;
        else break;

        ast_expr *right = parse_unary(ctx);
        if (!right) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_BINARY;
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static ast_expr *parse_add(compiler_ctx *ctx) {
    ast_expr *left = parse_mul(ctx);
    if (!left) return NULL;
    while (1) {
        uint8_t op = 0;
        if (match(ctx, CDB_TOK_PLUS, NULL)) op = CDB_BIN_ADD;
        else if (match(ctx, CDB_TOK_MINUS, NULL)) op = CDB_BIN_SUB;
        else break;

        ast_expr *right = parse_mul(ctx);
        if (!right) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_BINARY;
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static ast_expr *parse_cmp(compiler_ctx *ctx) {
    ast_expr *left = parse_add(ctx);
    if (!left) return NULL;
    while (1) {
        uint8_t op = 0;
        if (match(ctx, CDB_TOK_EQ_EQ, NULL) || match(ctx, CDB_TOK_EQ, NULL)) op = CDB_BIN_EQ;
        else if (match(ctx, CDB_TOK_NE, NULL)) op = CDB_BIN_NE;
        else if (match(ctx, CDB_TOK_LT, NULL)) op = CDB_BIN_LT;
        else if (match(ctx, CDB_TOK_LE, NULL)) op = CDB_BIN_LE;
        else if (match(ctx, CDB_TOK_GT, NULL)) op = CDB_BIN_GT;
        else if (match(ctx, CDB_TOK_GE, NULL)) op = CDB_BIN_GE;
        else if (match(ctx, CDB_TOK_KW_LIKE, NULL)) op = CDB_BIN_LIKE;
        else break;

        ast_expr *right = parse_add(ctx);
        if (!right) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_BINARY;
        e->as.binary.op = op;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static ast_expr *parse_and(compiler_ctx *ctx) {
    ast_expr *left = parse_cmp(ctx);
    if (!left) return NULL;
    while (match(ctx, CDB_TOK_KW_AND, NULL)) {
        ast_expr *right = parse_cmp(ctx);
        if (!right) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_BINARY;
        e->as.binary.op = CDB_BIN_AND;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static ast_expr *parse_or(compiler_ctx *ctx) {
    ast_expr *left = parse_and(ctx);
    if (!left) return NULL;
    while (match(ctx, CDB_TOK_KW_OR, NULL)) {
        ast_expr *right = parse_and(ctx);
        if (!right) return NULL;
        ast_expr *e = alloc_expr(ctx);
        if (!e) return NULL;
        e->tag = CDB_QUERY_EXPR_BINARY;
        e->as.binary.op = CDB_BIN_OR;
        e->as.binary.left = left;
        e->as.binary.right = right;
        left = e;
    }
    return left;
}

static ast_expr *parse_expr(compiler_ctx *ctx) {
    return parse_or(ctx);
}

/* Parse stream sources: Scan("name"), OperatorCalls, or table identifiers */
static ast_node *parse_source(compiler_ctx *ctx) {
    cdb_token tok;
    if (match(ctx, CDB_TOK_KW_SCAN, &tok)) {
        if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after Scan", NULL) != CDB_OK) return NULL;
        cdb_token name_tok;
        if (expect(ctx, CDB_TOK_STRING, "Expected store name string in Scan(...)", &name_tok) != CDB_OK) return NULL;
        if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after Scan(string)", NULL) != CDB_OK) return NULL;

        char buf[128];
        size_t len = 0;
        cdb_token_unescape_string(&name_tok, buf, sizeof(buf), &len);

        ast_node *n = alloc_node(ctx);
        if (!n) return NULL;
        n->tag = CDB_QUERY_NODE_SCAN;
        n->as.scan.store_name = pool_strdup(ctx, buf, len);
        n->as.scan.len = len;

        ctx->active_store = n->as.scan.store_name;
        ctx->active_store_len = len;
        return n;
    }

    if (match(ctx, CDB_TOK_IDENT, &tok)) {
        /* Bare table/pool name identifier */
        ast_node *n = alloc_node(ctx);
        if (!n) return NULL;
        n->tag = CDB_QUERY_NODE_SCAN;
        n->as.scan.store_name = pool_strdup(ctx, tok.start, (size_t)tok.len);
        n->as.scan.len = (size_t)tok.len;

        ctx->active_store = n->as.scan.store_name;
        ctx->active_store_len = n->as.scan.len;
        return n;
    }

    if (match(ctx, CDB_TOK_LPAREN, NULL)) {
        ast_node *sub = parse_query(ctx);
        if (!sub) return NULL;
        if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after subquery", NULL) != CDB_OK) return NULL;
        return sub;
    }

    set_error(ctx, NULL, "Expected valid source (Scan, table name, or subquery)");
    return NULL;
}

/* Parses pipeline stages: | filter(...) | sort(...) | limit(...) */
static ast_node *parse_pipeline(compiler_ctx *ctx, ast_node *source) {
    ast_node *cur = source;

    while (match(ctx, CDB_TOK_PIPE, NULL) || match(ctx, CDB_TOK_PIPE_FWD, NULL)) {
        cdb_token stage_tok;
        if (cdb_lexer_next(&ctx->lex, &stage_tok) != CDB_OK) return NULL;

        /* Filter stage: | filter(expr) */
        if (stage_tok.kind == CDB_TOK_KW_FILTER || cdb_token_equals_ci(&stage_tok, "filter") || cdb_token_equals_ci(&stage_tok, "where")) {
            if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after filter", NULL) != CDB_OK) return NULL;
            ast_expr *cond = parse_expr(ctx);
            if (!cond) return NULL;
            if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after filter condition", NULL) != CDB_OK) return NULL;

            ast_node *n = alloc_node(ctx);
            if (!n) return NULL;
            n->tag = CDB_QUERY_NODE_FILTER;
            n->as.filter.input = cur;
            n->as.filter.cond = cond;
            cur = n;
            continue;
        }

        /* Limit stage: | limit(count [, offset]) */
        if (stage_tok.kind == CDB_TOK_KW_LIMIT || cdb_token_equals_ci(&stage_tok, "limit")) {
            if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after limit", NULL) != CDB_OK) return NULL;
            cdb_token cnt_tok;
            if (expect(ctx, CDB_TOK_INT, "Expected integer count in limit(...)", &cnt_tok) != CDB_OK) return NULL;
            uint64_t count = (uint64_t)cnt_tok.num_val.i64;
            uint64_t offset = 0;
            if (match(ctx, CDB_TOK_COMMA, NULL)) {
                cdb_token off_tok;
                if (expect(ctx, CDB_TOK_INT, "Expected integer offset in limit(count, offset)", &off_tok) != CDB_OK) return NULL;
                offset = (uint64_t)off_tok.num_val.i64;
            }
            if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after limit arguments", NULL) != CDB_OK) return NULL;

            ast_node *n = alloc_node(ctx);
            if (!n) return NULL;
            n->tag = CDB_QUERY_NODE_LIMIT;
            n->as.limit.input = cur;
            n->as.limit.count = count;
            n->as.limit.offset = offset;
            cur = n;
            continue;
        }

        /* Sort stage: | sort(key1 [ASC|DESC], ...) */
        if (stage_tok.kind == CDB_TOK_KW_SORT || cdb_token_equals_ci(&stage_tok, "sort") || cdb_token_equals_ci(&stage_tok, "order")) {
            if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after sort", NULL) != CDB_OK) return NULL;
            ast_node *n = alloc_node(ctx);
            if (!n) return NULL;
            n->tag = CDB_QUERY_NODE_SORT;
            n->as.sort.input = cur;
            n->as.sort.n_keys = 0;

            do {
                ast_expr *kexpr = parse_expr(ctx);
                if (!kexpr) return NULL;
                uint8_t dir = CDB_SORT_ASC;
                uint8_t nulls = CDB_NULLS_LAST;
                if (match(ctx, CDB_TOK_KW_DESC, NULL)) dir = CDB_SORT_DESC;
                else if (match(ctx, CDB_TOK_KW_ASC, NULL)) dir = CDB_SORT_ASC;

                if (match(ctx, CDB_TOK_KW_NULLS, NULL)) {
                    if (match(ctx, CDB_TOK_KW_FIRST, NULL)) nulls = CDB_NULLS_FIRST;
                    else if (match(ctx, CDB_TOK_KW_LAST, NULL)) nulls = CDB_NULLS_LAST;
                }
                if (n->as.sort.n_keys < MAX_KEYS) {
                    size_t ki = n->as.sort.n_keys++;
                    n->as.sort.keys[ki].expr = kexpr;
                    n->as.sort.keys[ki].dir = dir;
                    n->as.sort.keys[ki].nulls = nulls;
                }
            } while (match(ctx, CDB_TOK_COMMA, NULL));

            if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after sort keys", NULL) != CDB_OK) return NULL;
            cur = n;
            continue;
        }

        /* Window stage: | window(field: "ts", size: ..., step: ...) */
        if (stage_tok.kind == CDB_TOK_KW_WINDOW || cdb_token_equals_ci(&stage_tok, "window")) {
            if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after window", NULL) != CDB_OK) return NULL;
            ast_node *n = alloc_node(ctx);
            if (!n) return NULL;
            n->tag = CDB_QUERY_NODE_WINDOW;
            n->as.window.input = cur;
            n->as.window.type = CDB_WINDOW_TUMBLING;
            n->as.window.time_field = pool_strdup(ctx, "ts", 2);
            n->as.window.time_field_len = 2;
            n->as.window.size_ns = 60000;
            n->as.window.step_ns = 60000;

            while (!match(ctx, CDB_TOK_RPAREN, NULL)) {
                cdb_token arg_name;
                if (cdb_lexer_next(&ctx->lex, &arg_name) != CDB_OK) return NULL;
                if (expect(ctx, CDB_TOK_COLON, "Expected ':' after argument name", NULL) != CDB_OK) return NULL;
                if (cdb_token_equals_ci(&arg_name, "field")) {
                    cdb_token ftok;
                    if (cdb_lexer_next(&ctx->lex, &ftok) != CDB_OK) return NULL;
                    char fbuf[128];
                    size_t flen = 0;
                    cdb_token_unescape_string(&ftok, fbuf, sizeof(fbuf), &flen);
                    n->as.window.time_field = pool_strdup(ctx, fbuf, flen);
                    n->as.window.time_field_len = flen;
                } else if (cdb_token_equals_ci(&arg_name, "size")) {
                    cdb_token stok;
                    if (expect(ctx, CDB_TOK_INT, "Expected integer size", &stok) != CDB_OK) return NULL;
                    n->as.window.size_ns = (uint64_t)stok.num_val.i64;
                } else if (cdb_token_equals_ci(&arg_name, "step")) {
                    cdb_token stok;
                    if (expect(ctx, CDB_TOK_INT, "Expected integer step", &stok) != CDB_OK) return NULL;
                    n->as.window.step_ns = (uint64_t)stok.num_val.i64;
                    n->as.window.type = CDB_WINDOW_SLIDING;
                }
                match(ctx, CDB_TOK_COMMA, NULL);
            }
            cur = n;
            continue;
        }

        /* Project / Select stage: | project(field1, field2 AS alias) */
        if (stage_tok.kind == CDB_TOK_KW_PROJECT || cdb_token_equals_ci(&stage_tok, "project") ||
            stage_tok.kind == CDB_TOK_KW_SELECT || cdb_token_equals_ci(&stage_tok, "select")) {
            if (expect(ctx, CDB_TOK_LPAREN, "Expected '(' after project/select", NULL) != CDB_OK) return NULL;
            ast_node *n = alloc_node(ctx);
            if (!n) return NULL;
            n->tag = CDB_QUERY_NODE_PROJECT;
            n->as.project.input = cur;
            n->as.project.passthrough = 0;
            n->as.project.n_outputs = 0;

            do {
                ast_expr *pexpr = parse_expr(ctx);
                if (!pexpr) return NULL;
                const char *alias = NULL;
                size_t alen = 0;
                if (match(ctx, CDB_TOK_KW_AS, NULL) || match(ctx, CDB_TOK_COLON, NULL)) {
                    cdb_token atok;
                    if (expect(ctx, CDB_TOK_IDENT, "Expected alias identifier", &atok) != CDB_OK) return NULL;
                    alias = pool_strdup(ctx, atok.start, (size_t)atok.len);
                    alen = (size_t)atok.len;
                } else if (pexpr->tag == CDB_QUERY_EXPR_FIELD) {
                    alias = pexpr->as.field.path;
                    alen = pexpr->as.field.len;
                }
                if (n->as.project.n_outputs < MAX_OUTPUTS) {
                    size_t oi = n->as.project.n_outputs++;
                    n->as.project.outputs[oi].expr = pexpr;
                    n->as.project.outputs[oi].alias = alias;
                    n->as.project.outputs[oi].alias_len = alen;
                }
            } while (match(ctx, CDB_TOK_COMMA, NULL));

            if (expect(ctx, CDB_TOK_RPAREN, "Expected ')' after project list", NULL) != CDB_OK) return NULL;
            cur = n;
            continue;
        }

        set_error(ctx, &stage_tok, "Unknown pipeline stage");
        return NULL;
    }

    return cur;
}

/* Parse top-level query (either SQL-style or Pipeline-style) */
static ast_node *parse_query(compiler_ctx *ctx) {
    /* Optional QUERY name(...) */
    if (match(ctx, CDB_TOK_KW_QUERY, NULL)) {
        cdb_token qname;
        if (expect(ctx, CDB_TOK_IDENT, "Expected query name identifier", &qname) != CDB_OK) return NULL;
        if (match(ctx, CDB_TOK_LPAREN, NULL)) {
            while (!match(ctx, CDB_TOK_RPAREN, NULL)) {
                cdb_token ptok;
                if (cdb_lexer_next(&ctx->lex, &ptok) != CDB_OK) return NULL;
                match(ctx, CDB_TOK_COMMA, NULL);
            }
        }
    }

    /* Declarative SQL syntax */
    if (match(ctx, CDB_TOK_KW_SELECT, NULL)) {
        project_item selects[MAX_OUTPUTS];
        size_t n_selects = 0;
        int is_star = 0;

        if (match(ctx, CDB_TOK_STAR, NULL)) {
            is_star = 1;
        } else {
            do {
                ast_expr *se = parse_expr(ctx);
                if (!se) return NULL;
                const char *alias = NULL;
                size_t alen = 0;
                if (match(ctx, CDB_TOK_KW_AS, NULL) || match(ctx, CDB_TOK_COLON, NULL)) {
                    cdb_token atok;
                    if (expect(ctx, CDB_TOK_IDENT, "Expected alias name", &atok) != CDB_OK) return NULL;
                    alias = pool_strdup(ctx, atok.start, (size_t)atok.len);
                    alen = (size_t)atok.len;
                } else if (se->tag == CDB_QUERY_EXPR_FIELD) {
                    alias = se->as.field.path;
                    alen = se->as.field.len;
                }
                if (n_selects < MAX_OUTPUTS) {
                    selects[n_selects].expr = se;
                    selects[n_selects].alias = alias;
                    selects[n_selects].alias_len = alen;
                    n_selects++;
                }
            } while (match(ctx, CDB_TOK_COMMA, NULL));
        }

        if (expect(ctx, CDB_TOK_KW_FROM, "Expected FROM clause in SELECT query", NULL) != CDB_OK) return NULL;
        ast_node *cur = parse_source(ctx);
        if (!cur) return NULL;

        /* Check for JOIN clauses */
        while (match(ctx, CDB_TOK_KW_JOIN, NULL) || match(ctx, CDB_TOK_KW_INNER, NULL) || match(ctx, CDB_TOK_KW_LEFT, NULL)) {
            uint8_t kind = CDB_JOIN_INNER;
            if (ctx->lex.peeked.kind == CDB_TOK_KW_LEFT) {
                kind = CDB_JOIN_LEFT_OUTER;
                match(ctx, CDB_TOK_KW_OUTER, NULL);
                expect(ctx, CDB_TOK_KW_JOIN, "Expected JOIN after LEFT", NULL);
            } else if (ctx->lex.peeked.kind == CDB_TOK_KW_INNER) {
                expect(ctx, CDB_TOK_KW_JOIN, "Expected JOIN after INNER", NULL);
            }
            ast_node *right = parse_source(ctx);
            if (!right) return NULL;
            if (expect(ctx, CDB_TOK_KW_ON, "Expected ON in JOIN clause", NULL) != CDB_OK) return NULL;
            ast_expr *jcond = parse_expr(ctx);
            if (!jcond) return NULL;

            ast_node *jn = alloc_node(ctx);
            if (!jn) return NULL;
            jn->tag = CDB_QUERY_NODE_JOIN;
            jn->as.join.left = cur;
            jn->as.join.right = right;
            jn->as.join.kind = kind;
            jn->as.join.cond = jcond;
            cur = jn;
        }

        /* Check for WHERE / FILTER */
        if (match(ctx, CDB_TOK_KW_WHERE, NULL) || match(ctx, CDB_TOK_KW_FILTER, NULL)) {
            ast_expr *wcond = parse_expr(ctx);
            if (!wcond) return NULL;
            ast_node *fn = alloc_node(ctx);
            if (!fn) return NULL;
            fn->tag = CDB_QUERY_NODE_FILTER;
            fn->as.filter.input = cur;
            fn->as.filter.cond = wcond;
            cur = fn;
        }

        /* Check for SORT / ORDER BY */
        if (match(ctx, CDB_TOK_KW_ORDER, NULL) || match(ctx, CDB_TOK_KW_SORT, NULL)) {
            expect(ctx, CDB_TOK_KW_BY, "Expected BY after ORDER/SORT", NULL);
            ast_node *sn = alloc_node(ctx);
            if (!sn) return NULL;
            sn->tag = CDB_QUERY_NODE_SORT;
            sn->as.sort.input = cur;
            sn->as.sort.n_keys = 0;

            do {
                ast_expr *kexpr = parse_expr(ctx);
                if (!kexpr) return NULL;
                uint8_t dir = CDB_SORT_ASC;
                uint8_t nulls = CDB_NULLS_LAST;
                if (match(ctx, CDB_TOK_KW_DESC, NULL)) dir = CDB_SORT_DESC;
                else if (match(ctx, CDB_TOK_KW_ASC, NULL)) dir = CDB_SORT_ASC;

                if (match(ctx, CDB_TOK_KW_NULLS, NULL)) {
                    if (match(ctx, CDB_TOK_KW_FIRST, NULL)) nulls = CDB_NULLS_FIRST;
                    else if (match(ctx, CDB_TOK_KW_LAST, NULL)) nulls = CDB_NULLS_LAST;
                }
                if (sn->as.sort.n_keys < MAX_KEYS) {
                    size_t ki = sn->as.sort.n_keys++;
                    sn->as.sort.keys[ki].expr = kexpr;
                    sn->as.sort.keys[ki].dir = dir;
                    sn->as.sort.keys[ki].nulls = nulls;
                }
            } while (match(ctx, CDB_TOK_COMMA, NULL));
            cur = sn;
        }

        /* Apply projection if not SELECT * */
        if (!is_star && n_selects > 0) {
            ast_node *pn = alloc_node(ctx);
            if (!pn) return NULL;
            pn->tag = CDB_QUERY_NODE_PROJECT;
            pn->as.project.input = cur;
            pn->as.project.passthrough = 0;
            pn->as.project.n_outputs = n_selects;
            for (size_t i = 0; i < n_selects; ++i) {
                pn->as.project.outputs[i] = selects[i];
            }
            cur = pn;
        }

        /* Check for LIMIT */
        if (match(ctx, CDB_TOK_KW_LIMIT, NULL)) {
            cdb_token ltok;
            if (expect(ctx, CDB_TOK_INT, "Expected integer in LIMIT", &ltok) != CDB_OK) return NULL;
            uint64_t count = (uint64_t)ltok.num_val.i64;
            uint64_t offset = 0;
            if (match(ctx, CDB_TOK_KW_OFFSET, NULL) || match(ctx, CDB_TOK_COMMA, NULL)) {
                cdb_token otok;
                if (expect(ctx, CDB_TOK_INT, "Expected integer in OFFSET", &otok) != CDB_OK) return NULL;
                offset = (uint64_t)otok.num_val.i64;
            }
            ast_node *ln = alloc_node(ctx);
            if (!ln) return NULL;
            ln->tag = CDB_QUERY_NODE_LIMIT;
            ln->as.limit.input = cur;
            ln->as.limit.count = count;
            ln->as.limit.offset = offset;
            cur = ln;
        }

        return cur;
    }

    /* Pipeline / Flux syntax */
    ast_node *src_node = parse_source(ctx);
    if (!src_node) return NULL;
    return parse_pipeline(ctx, src_node);
}

/* Recursive AST Serializer to CMBQ binary wire format */
static cdb_status serialize_expr(cdb_query_writer *w, const ast_expr *e) {
    if (!e) return CDB_E_INVALID_ARG;
    switch (e->tag) {
        case CDB_QUERY_EXPR_LITERAL:
            return cdb_query_write_expr_literal(w, e->as.literal.bytes, e->as.literal.len);

        case CDB_QUERY_EXPR_FIELD:
            return cdb_query_write_expr_field(w, e->as.field.path, e->as.field.len);

        case CDB_QUERY_EXPR_PARAM:
            if (e->as.param.name) {
                return cdb_query_write_expr_param_named(w, e->as.param.name, e->as.param.name_len);
            }
            return cdb_query_write_expr_param_index(w, e->as.param.index);

        case CDB_QUERY_EXPR_UNARY: {
            cdb_status s = cdb_query_write_expr_unary(w, e->as.unary.op);
            if (s != CDB_OK) return s;
            return serialize_expr(w, e->as.unary.child);
        }

        case CDB_QUERY_EXPR_BINARY: {
            cdb_status s = cdb_query_write_expr_binary(w, e->as.binary.op);
            if (s != CDB_OK) return s;
            s = serialize_expr(w, e->as.binary.left);
            if (s != CDB_OK) return s;
            return serialize_expr(w, e->as.binary.right);
        }

        case CDB_QUERY_EXPR_CALL: {
            cdb_status s = cdb_query_write_expr_call(w, e->as.call.fn_id, e->as.call.n_args);
            if (s != CDB_OK) return s;
            for (uint16_t i = 0; i < e->as.call.n_args; ++i) {
                s = serialize_expr(w, e->as.call.args[i]);
                if (s != CDB_OK) return s;
            }
            return CDB_OK;
        }

        default:
            return CDB_E_INVALID_ARG;
    }
}

static cdb_status serialize_node(cdb_query_writer *w, const ast_node *n) {
    if (!n) return CDB_E_INVALID_ARG;
    switch (n->tag) {
        case CDB_QUERY_NODE_SCAN:
            return cdb_query_write_scan(w, n->as.scan.store_name, n->as.scan.len);

        case CDB_QUERY_NODE_FILTER: {
            cdb_status s = cdb_query_write_filter(w);
            if (s != CDB_OK) return s;
            s = serialize_node(w, n->as.filter.input);
            if (s != CDB_OK) return s;
            return serialize_expr(w, n->as.filter.cond);
        }

        case CDB_QUERY_NODE_PROJECT: {
            cdb_status s = cdb_query_write_project(w, n->as.project.passthrough, n->as.project.n_outputs);
            if (s != CDB_OK) return s;
            s = serialize_node(w, n->as.project.input);
            if (s != CDB_OK) return s;
            for (size_t i = 0; i < n->as.project.n_outputs; ++i) {
                s = cdb_query_write_project_output(w, n->as.project.outputs[i].alias, n->as.project.outputs[i].alias_len);
                if (s != CDB_OK) return s;
                s = serialize_expr(w, n->as.project.outputs[i].expr);
                if (s != CDB_OK) return s;
            }
            return CDB_OK;
        }

        case CDB_QUERY_NODE_LIMIT: {
            cdb_status s = cdb_query_write_limit(w, n->as.limit.count, n->as.limit.offset);
            if (s != CDB_OK) return s;
            return serialize_node(w, n->as.limit.input);
        }

        case CDB_QUERY_NODE_SORT: {
            cdb_status s = cdb_query_write_sort(w, n->as.sort.n_keys);
            if (s != CDB_OK) return s;
            s = serialize_node(w, n->as.sort.input);
            if (s != CDB_OK) return s;
            for (size_t i = 0; i < n->as.sort.n_keys; ++i) {
                s = serialize_expr(w, n->as.sort.keys[i].expr);
                if (s != CDB_OK) return s;
                s = cdb_query_write_sort_key_suffix(w, n->as.sort.keys[i].dir, n->as.sort.keys[i].nulls);
                if (s != CDB_OK) return s;
            }
            return CDB_OK;
        }

        case CDB_QUERY_NODE_JOIN: {
            cdb_status s = cdb_query_write_join(w, n->as.join.kind);
            if (s != CDB_OK) return s;
            s = serialize_node(w, n->as.join.left);
            if (s != CDB_OK) return s;
            s = serialize_node(w, n->as.join.right);
            if (s != CDB_OK) return s;
            return serialize_expr(w, n->as.join.cond);
        }

        case CDB_QUERY_NODE_WINDOW: {
            cdb_status s = cdb_query_write_window(w, n->as.window.type, n->as.window.time_field,
                                                  n->as.window.time_field_len,
                                                  n->as.window.size_ns, n->as.window.step_ns);
            if (s != CDB_OK) return s;
            return serialize_node(w, n->as.window.input);
        }

        default:
            return CDB_E_INVALID_ARG;
    }
}

cdb_status cdb_compile_query(const char *src, size_t len,
                             cdb_catalog *cat,
                             uint8_t *wire_buf, size_t wire_cap,
                             size_t *out_wire_len,
                             cdb_compile_error *err) {
    if (!src || !wire_buf || wire_cap == 0) return CDB_E_INVALID_ARG;

    compiler_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    cdb_lexer_init(&ctx.lex, src, len);
    ctx.cat = cat;
    ctx.err = err;

    ast_node *root = parse_query(&ctx);
    if (!root) {
        if (!err || err->msg[0] == '\0') {
            set_error(&ctx, NULL, "Failed to parse query syntax");
        }
        return CDB_E_INVALID_ARG;
    }

    cdb_query_writer w;
    cdb_query_writer_init(&w, wire_buf, wire_cap);
    cdb_status s = cdb_query_writer_begin(&w);
    if (s != CDB_OK) return s;

    s = serialize_node(&w, root);
    if (s != CDB_OK) return s;

    size_t wlen = cdb_query_writer_len(&w);
    if (out_wire_len) *out_wire_len = wlen;

    /* Validate compiled wire format */
    size_t root_off = 0;
    return cdb_query_validate(wire_buf, wlen, &root_off);
}

/* Stored procedure variable registry */
typedef struct proc_var {
    const char *name;
    size_t len;
    uint8_t reg;
} proc_var;

typedef struct proc_compiler {
    cdb_lexer lex;
    cdb_catalog *cat;
    cdb_compile_error *err;
    proc_var vars[MAX_VARS];
    uint8_t n_vars;
    uint8_t next_reg;
} proc_compiler;

static uint8_t proc_get_or_alloc_var(proc_compiler *pc, const char *name, size_t len) {
    for (uint8_t i = 0; i < pc->n_vars; ++i) {
        if (pc->vars[i].len == len && memcmp(pc->vars[i].name, name, len) == 0) {
            return pc->vars[i].reg;
        }
    }
    if (pc->n_vars < MAX_VARS && pc->next_reg < CDB_PROC_MAX_REGISTERS) {
        uint8_t r = pc->next_reg++;
        pc->vars[pc->n_vars].name = name;
        pc->vars[pc->n_vars].len = len;
        pc->vars[pc->n_vars].reg = r;
        pc->n_vars++;
        return r;
    }
    return 0;
}

static int proc_match(proc_compiler *pc, cdb_token_kind kind, cdb_token *out_tok) {
    cdb_token tok;
    if (cdb_lexer_peek(&pc->lex, &tok) != CDB_OK) return 0;
    if (tok.kind == kind) {
        cdb_lexer_next(&pc->lex, &tok);
        if (out_tok) *out_tok = tok;
        return 1;
    }
    return 0;
}

static cdb_status proc_expect(proc_compiler *pc, cdb_token_kind kind, const char *msg, cdb_token *out_tok) {
    cdb_token tok;
    cdb_status s = cdb_lexer_next(&pc->lex, &tok);
    if (s != CDB_OK) {
        if (pc->err) {
            pc->err->line = pc->lex.line;
            pc->err->col = pc->lex.col;
            snprintf(pc->err->msg, sizeof(pc->err->msg), "%s", "Unexpected end of input");
        }
        return s;
    }
    if (tok.kind != kind) {
        if (pc->err) {
            pc->err->line = tok.line;
            pc->err->col = tok.col;
            snprintf(pc->err->msg, sizeof(pc->err->msg), "%s", msg);
        }
        return CDB_E_INVALID_ARG;
    }
    if (out_tok) *out_tok = tok;
    return CDB_OK;
}

static int proc_match_str(proc_compiler *pc, const char *s) {
    cdb_token tok;
    if (cdb_lexer_peek(&pc->lex, &tok) != CDB_OK) return 0;
    if (cdb_token_equals_ci(&tok, s)) {
        cdb_lexer_next(&pc->lex, &tok);
        return 1;
    }
    return 0;
}

static uint8_t proc_alloc_temp_reg(proc_compiler *pc) {
    if (pc->next_reg < CDB_PROC_MAX_REGISTERS) {
        return pc->next_reg++;
    }
    return 0;
}

static int proc_is_ident_or_kw(cdb_token_kind k) {
    return k == CDB_TOK_IDENT || (k >= CDB_TOK_KW_QUERY && k <= CDB_TOK_KW_FIELD);
}

static cdb_status proc_parse_val_to_reg(proc_compiler *pc, cdb_proc_writer *pw, uint8_t dst_reg) {
    cdb_token tok;
    if (cdb_lexer_peek(&pc->lex, &tok) != CDB_OK) return CDB_E_INVALID_ARG;

    if (tok.kind == CDB_TOK_PARAM) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t src_reg = proc_get_or_alloc_var(pc, tok.start, (size_t)tok.len);
        cdb_proc_write_move(pw, dst_reg, src_reg);
        return CDB_OK;
    }
    if (tok.kind == CDB_TOK_INT) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t enc[16];
        size_t elen = encode_int_val(tok.num_val.i64, enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    if (tok.kind == CDB_TOK_FLOAT) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t enc[16];
        size_t elen = encode_f64_val(tok.num_val.f64, enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    if (tok.kind == CDB_TOK_STRING) {
        cdb_lexer_next(&pc->lex, &tok);
        char sbuf[512];
        size_t slen = 0;
        cdb_token_unescape_string(&tok, sbuf, sizeof(sbuf), &slen);
        uint8_t enc[512];
        size_t elen = encode_string_val(sbuf, slen, enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    if (tok.kind == CDB_TOK_KW_TRUE || tok.kind == CDB_TOK_KW_FALSE) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t enc[8];
        size_t elen = encode_bool_val(tok.kind == CDB_TOK_KW_TRUE ? 1 : 0, enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    if (tok.kind == CDB_TOK_KW_NULL) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t enc[8];
        size_t elen = encode_null_val(enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    if (proc_is_ident_or_kw(tok.kind)) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t enc[256];
        size_t elen = encode_string_val(tok.start, (size_t)tok.len, enc, sizeof(enc));
        return cdb_proc_write_load_const(pw, dst_reg, enc, elen);
    }
    return CDB_E_INVALID_ARG;
}

static cdb_status proc_parse_arg(proc_compiler *pc, cdb_proc_writer *pw, uint8_t dst_reg) {
    cdb_token tok, next_tok;
    if (cdb_lexer_peek(&pc->lex, &tok) == CDB_OK && proc_is_ident_or_kw(tok.kind)) {
        cdb_lexer save = pc->lex;
        cdb_lexer_next(&pc->lex, &tok);
        if (cdb_lexer_peek(&pc->lex, &next_tok) == CDB_OK && next_tok.kind == CDB_TOK_COLON) {
            cdb_lexer_next(&pc->lex, &next_tok);
        } else {
            pc->lex = save;
        }
    }
    return proc_parse_val_to_reg(pc, pw, dst_reg);
}

static cdb_status proc_parse_ref(proc_compiler *pc, cdb_proc_writer *pw, uint8_t r_store, uint8_t r_entry) {
    int has_bracket = proc_match(pc, CDB_TOK_LBRACKET, NULL);
    cdb_status s = proc_parse_arg(pc, pw, r_store);
    if (s != CDB_OK) return s;
    if (proc_match(pc, CDB_TOK_COLON, NULL) || proc_match(pc, CDB_TOK_COMMA, NULL)) {
        s = proc_parse_arg(pc, pw, r_entry);
        if (s != CDB_OK) return s;
    }
    if (has_bracket) {
        proc_expect(pc, CDB_TOK_RBRACKET, "Expected ']' after reference", NULL);
    }
    return CDB_OK;
}

static cdb_status proc_compile_statement(proc_compiler *pc, cdb_proc_writer *pw) {
    cdb_token tok;
    uint8_t base_reg = pc->next_reg;
    uint8_t out_reg = 0;

    if (cdb_lexer_peek(&pc->lex, &tok) != CDB_OK) return CDB_E_INVALID_ARG;

    /* LET $var = ... */
    if (tok.kind == CDB_TOK_KW_LET) {
        cdb_lexer_next(&pc->lex, &tok);
        cdb_token var_tok;
        if (proc_expect(pc, CDB_TOK_PARAM, "Expected $variable after LET", &var_tok) != CDB_OK) return CDB_E_INVALID_ARG;
        proc_expect(pc, CDB_TOK_EQ, "Expected '=' in LET assignment", NULL);
        out_reg = proc_get_or_alloc_var(pc, var_tok.start, (size_t)var_tok.len);
        base_reg = pc->next_reg;

        /* If assigning a literal directly without a command */
        if (cdb_lexer_peek(&pc->lex, &tok) == CDB_OK) {
            if (tok.kind == CDB_TOK_INT || tok.kind == CDB_TOK_FLOAT || tok.kind == CDB_TOK_STRING ||
                tok.kind == CDB_TOK_KW_TRUE || tok.kind == CDB_TOK_KW_FALSE || tok.kind == CDB_TOK_KW_NULL) {
                return proc_parse_val_to_reg(pc, pw, out_reg);
            }
        }
    } else {
        out_reg = proc_alloc_temp_reg(pc);
    }

    if (cdb_lexer_peek(&pc->lex, &tok) != CDB_OK) return CDB_E_INVALID_ARG;

    /* RETURN expr */
    if (tok.kind == CDB_TOK_KW_RETURN) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t src_reg = 0;
        cdb_token ret_tok;
        if (cdb_lexer_peek(&pc->lex, &ret_tok) == CDB_OK && ret_tok.kind == CDB_TOK_PARAM) {
            cdb_lexer_next(&pc->lex, &ret_tok);
            src_reg = proc_get_or_alloc_var(pc, ret_tok.start, (size_t)ret_tok.len);
        } else if (cdb_lexer_peek(&pc->lex, &ret_tok) == CDB_OK && ret_tok.kind != CDB_TOK_SEMICOLON) {
            uint8_t r_tmp = proc_alloc_temp_reg(pc);
            proc_parse_val_to_reg(pc, pw, r_tmp);
            src_reg = r_tmp;
        }
        cdb_proc_write_return(pw, src_reg);
        return CDB_OK;
    }

    /* SAVEPOINT */
    if (cdb_token_equals_ci(&tok, "SAVEPOINT")) {
        cdb_lexer_next(&pc->lex, &tok);
        cdb_token sp_name;
        if (proc_expect(pc, CDB_TOK_IDENT, "Expected savepoint name", &sp_name) != CDB_OK) return CDB_E_INVALID_ARG;
        uint8_t sp_id = (uint8_t)(sp_name.len > 0 ? (uint8_t)((unsigned char)sp_name.start[0] % 8) : 1u);
        return cdb_proc_write_savepoint(pw, sp_id);
    }

    /* ROLLBACK */
    if (cdb_token_equals_ci(&tok, "ROLLBACK")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "TO");
        cdb_token sp_name;
        if (proc_expect(pc, CDB_TOK_IDENT, "Expected savepoint name after ROLLBACK TO", &sp_name) != CDB_OK) return CDB_E_INVALID_ARG;
        uint8_t sp_id = (uint8_t)(sp_name.len > 0 ? (uint8_t)((unsigned char)sp_name.start[0] % 8) : 1u);
        return cdb_proc_write_rollback_to(pw, sp_id);
    }

    /* RELEASE */
    if (cdb_token_equals_ci(&tok, "RELEASE")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "SAVEPOINT");
        cdb_token sp_name;
        if (proc_expect(pc, CDB_TOK_IDENT, "Expected savepoint name after RELEASE", &sp_name) != CDB_OK) return CDB_E_INVALID_ARG;
        uint8_t sp_id = (uint8_t)(sp_name.len > 0 ? (uint8_t)((unsigned char)sp_name.start[0] % 8) : 1u);
        return cdb_proc_write_release_sp(pw, sp_id);
    }

    /* COMPACT / compact(...) */
    if (cdb_token_equals_ci(&tok, "COMPACT") || cdb_token_equals_ci(&tok, "compact")) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t r_max = proc_alloc_temp_reg(pc);
        uint8_t m_args[1];
        uint8_t zero_enc[16];
        size_t zlen = encode_int_val(0, zero_enc, sizeof(zero_enc));
        cdb_proc_write_load_const(pw, r_max, zero_enc, zlen);

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            if (!proc_match(pc, CDB_TOK_RPAREN, NULL)) {
                proc_parse_arg(pc, pw, r_max);
                proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after compact(...) argument", NULL);
            }
        } else {
            if (proc_match_str(pc, "MAX")) {
                proc_parse_val_to_reg(pc, pw, r_max);
            }
        }
        m_args[0] = r_max;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_COMPACT, 1, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* RECLAIM / reclaim() */
    if (cdb_token_equals_ci(&tok, "RECLAIM") || cdb_token_equals_ci(&tok, "reclaim")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_match(pc, CDB_TOK_RPAREN, NULL);
        }
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_RECLAIM, 0, NULL);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* RECONCILE / reconcile() */
    if (cdb_token_equals_ci(&tok, "RECONCILE") || cdb_token_equals_ci(&tok, "reconcile")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_match(pc, CDB_TOK_RPAREN, NULL);
        }
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_RECONCILE, 0, NULL);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* PEER / SET PEER / peer(id) */
    if (cdb_token_equals_ci(&tok, "PEER") || cdb_token_equals_ci(&tok, "peer") || cdb_token_equals_ci(&tok, "set_peer")) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t r_peer = proc_alloc_temp_reg(pc);
        uint8_t m_args[1];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_peer);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after peer(id)", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_peer);
        }
        m_args[0] = r_peer;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_SET_PEER, 1, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (tok.kind == CDB_TOK_KW_SET && cdb_token_equals_ci(&tok, "SET")) {
        cdb_lexer save = pc->lex;
        cdb_lexer_next(&pc->lex, &tok);
        cdb_token sub_tok;
        if (cdb_lexer_peek(&pc->lex, &sub_tok) == CDB_OK && (cdb_token_equals_ci(&sub_tok, "PEER") || sub_tok.kind == CDB_TOK_KW_PEER)) {
            cdb_lexer_next(&pc->lex, &sub_tok);
            uint8_t r_peer = proc_alloc_temp_reg(pc);
            uint8_t m_args[1];
            proc_parse_val_to_reg(pc, pw, r_peer);
            m_args[0] = r_peer;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_SET_PEER, 1, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (cdb_lexer_peek(&pc->lex, &sub_tok) == CDB_OK && (cdb_token_equals_ci(&sub_tok, "RETENTION") || sub_tok.kind == CDB_TOK_KW_RETENTION)) {
            cdb_lexer_next(&pc->lex, &sub_tok);
            proc_match_str(pc, "TIMESERIES");
            proc_match_str(pc, "TS");
            uint8_t r_store = proc_alloc_temp_reg(pc);
            uint8_t r_series = proc_alloc_temp_reg(pc);
            uint8_t r_age = proc_alloc_temp_reg(pc);
            uint8_t m_args[3];
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "SERIES");
            proc_parse_val_to_reg(pc, pw, r_series);
            proc_match_str(pc, "AGE");
            proc_parse_val_to_reg(pc, pw, r_age);
            m_args[0] = r_store; m_args[1] = r_series; m_args[2] = r_age;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_RETENTION, 3, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else {
            pc->lex = save;
        }
    }

    /* SYNC / sync(...) */
    if (cdb_token_equals_ci(&tok, "SYNC") || cdb_token_equals_ci(&tok, "sync") || cdb_token_equals_ci(&tok, "sync_import")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "IMPORT");
        uint8_t r_slice = proc_alloc_temp_reg(pc);
        uint8_t m_args[1];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_slice);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after sync(...)", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_slice);
        }
        m_args[0] = r_slice;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_SYNC_IMPORT, 1, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* CHECK ROTATION / CHECK PERM / check_rotation() / check_perm(...) */
    if (cdb_token_equals_ci(&tok, "check_rotation")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) proc_match(pc, CDB_TOK_RPAREN, NULL);
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CHECK_ROTATION, 0, NULL);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (cdb_token_equals_ci(&tok, "check_perm")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '(' after check_perm", NULL);
        uint8_t r_pid = proc_alloc_temp_reg(pc);
        uint8_t r_scope = proc_alloc_temp_reg(pc);
        uint8_t r_perms = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        proc_parse_arg(pc, pw, r_pid);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_scope);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_perms);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after check_perm arguments", NULL);
        m_args[0] = r_pid; m_args[1] = r_scope; m_args[2] = r_perms;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CHECK_PERM, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (tok.kind == CDB_TOK_KW_CHECK || cdb_token_equals_ci(&tok, "CHECK")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match_str(pc, "ROTATION")) {
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CHECK_ROTATION, 0, NULL);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "PERM") || proc_match_str(pc, "PERMISSION") || proc_match_str(pc, "PERMS")) {
            uint8_t r_pid = proc_alloc_temp_reg(pc);
            uint8_t r_scope = proc_alloc_temp_reg(pc);
            uint8_t r_perms = proc_alloc_temp_reg(pc);
            uint8_t m_args[3];
            proc_parse_val_to_reg(pc, pw, r_pid);
            if (proc_match_str(pc, "SCOPE")) {
                proc_parse_val_to_reg(pc, pw, r_scope);
            } else {
                uint8_t zenc[16]; size_t zl = encode_int_val(0, zenc, sizeof(zenc));
                cdb_proc_write_load_const(pw, r_scope, zenc, zl);
            }
            if (proc_match_str(pc, "PERMS") || proc_match_str(pc, "PERM")) {
                proc_parse_val_to_reg(pc, pw, r_perms);
            } else {
                uint8_t renc[16]; size_t rl = encode_int_val(1, renc, sizeof(renc));
                cdb_proc_write_load_const(pw, r_perms, renc, rl);
            }
            m_args[0] = r_pid; m_args[1] = r_scope; m_args[2] = r_perms;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CHECK_PERM, 3, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        }
    }

    /* ROTATE / rotate(...) */
    if (cdb_token_equals_ci(&tok, "ROTATE") || cdb_token_equals_ci(&tok, "rotate") || cdb_token_equals_ci(&tok, "rotate_key")) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t r_target = proc_alloc_temp_reg(pc);
        uint8_t r_id = proc_alloc_temp_reg(pc);
        uint8_t r_uri = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_target);
            if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                proc_parse_arg(pc, pw, r_id);
                if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                    proc_parse_arg(pc, pw, r_uri);
                } else {
                    r_uri = r_id;
                    uint8_t zenc[16]; size_t zl = encode_int_val(0, zenc, sizeof(zenc));
                    r_id = proc_alloc_temp_reg(pc);
                    cdb_proc_write_load_const(pw, r_id, zenc, zl);
                }
            }
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after rotate(...)", NULL);
        } else {
            if (proc_match_str(pc, "MASTER")) {
                uint8_t zenc[16]; size_t zl = encode_int_val(0, zenc, sizeof(zenc));
                cdb_proc_write_load_const(pw, r_target, zenc, zl);
                cdb_proc_write_load_const(pw, r_id, zenc, zl);
                proc_match_str(pc, "KEY");
                proc_parse_val_to_reg(pc, pw, r_uri);
            } else if (proc_match_str(pc, "ENCLAVE")) {
                uint8_t oenc[16]; size_t ol = encode_int_val(1, oenc, sizeof(oenc));
                cdb_proc_write_load_const(pw, r_target, oenc, ol);
                proc_parse_val_to_reg(pc, pw, r_id);
                proc_match_str(pc, "KEY");
                proc_parse_val_to_reg(pc, pw, r_uri);
            }
        }
        m_args[0] = r_target; m_args[1] = r_id; m_args[2] = r_uri;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_ROTATE_KEY, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* ENCLAVE / create_enclave(...) / open_enclave(...) */
    if (cdb_token_equals_ci(&tok, "create_enclave") || cdb_token_equals_ci(&tok, "open_enclave")) {
        uint8_t mut_op = cdb_token_equals_ci(&tok, "open_enclave") ? CDB_MUT_OPEN_ENCLAVE : CDB_MUT_CREATE_ENCLAVE;
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_eid = proc_alloc_temp_reg(pc);
        uint8_t r_key = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];
        proc_parse_arg(pc, pw, r_eid);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_key);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_eid; m_args[1] = r_key;
        cdb_proc_write_call_mutation_ex(pw, out_reg, mut_op, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (tok.kind == CDB_TOK_KW_OPEN || cdb_token_equals_ci(&tok, "OPEN")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "ENCLAVE");
        uint8_t r_eid = proc_alloc_temp_reg(pc);
        uint8_t r_key = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];
        proc_parse_val_to_reg(pc, pw, r_eid);
        proc_match_str(pc, "KEY");
        proc_parse_val_to_reg(pc, pw, r_key);
        m_args[0] = r_eid; m_args[1] = r_key;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_OPEN_ENCLAVE, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* REGISTER / register_principal(...) */
    if (cdb_token_equals_ci(&tok, "register_principal") || (tok.kind == CDB_TOK_KW_REGISTER && cdb_token_equals_ci(&tok, "REGISTER"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "PRINCIPAL");
        uint8_t r_pid = proc_alloc_temp_reg(pc);
        uint8_t r_tid = proc_alloc_temp_reg(pc);
        uint8_t r_pk = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_pid);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_tid);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_pk);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_pid);
            proc_match_str(pc, "TENANT");
            proc_parse_val_to_reg(pc, pw, r_tid);
            proc_match_str(pc, "PUBKEY");
            proc_parse_val_to_reg(pc, pw, r_pk);
        }
        m_args[0] = r_pid; m_args[1] = r_tid; m_args[2] = r_pk;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_REGISTER_PRIN, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* GRANT / grant_role(...) */
    if (cdb_token_equals_ci(&tok, "grant_role") || (tok.kind == CDB_TOK_KW_GRANT && cdb_token_equals_ci(&tok, "GRANT"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "ROLE");
        uint8_t r_rid = proc_alloc_temp_reg(pc);
        uint8_t r_pid = proc_alloc_temp_reg(pc);
        uint8_t r_scope = proc_alloc_temp_reg(pc);
        uint8_t r_perms = proc_alloc_temp_reg(pc);
        uint8_t m_args[4];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_rid);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_pid);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_scope);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_perms);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_rid);
            proc_match_str(pc, "TO");
            proc_parse_val_to_reg(pc, pw, r_pid);
            if (proc_match_str(pc, "SCOPE")) {
                proc_parse_val_to_reg(pc, pw, r_scope);
            } else {
                uint8_t zenc[16]; size_t zl = encode_int_val(0, zenc, sizeof(zenc));
                cdb_proc_write_load_const(pw, r_scope, zenc, zl);
            }
            proc_match_str(pc, "PERMS");
            proc_parse_val_to_reg(pc, pw, r_perms);
        }
        m_args[0] = r_pid; m_args[1] = r_rid; m_args[2] = r_scope; m_args[3] = r_perms;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_GRANT_ROLE, 4, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* DROP / drop_trigger(...) / drop_store(...) */
    if (cdb_token_equals_ci(&tok, "drop_trigger")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_name = proc_alloc_temp_reg(pc);
        uint8_t m_args[1];
        proc_parse_arg(pc, pw, r_name);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_name;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TRIGGER_UNREG, 1, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (cdb_token_equals_ci(&tok, "drop_store")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_sid = proc_alloc_temp_reg(pc);
        uint8_t m_args[1];
        proc_parse_arg(pc, pw, r_sid);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_sid;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_DROP_STORE, 1, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (tok.kind == CDB_TOK_KW_DROP || cdb_token_equals_ci(&tok, "DROP")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match_str(pc, "TRIGGER")) {
            uint8_t r_name = proc_alloc_temp_reg(pc);
            uint8_t m_args[1];
            proc_parse_val_to_reg(pc, pw, r_name);
            m_args[0] = r_name;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TRIGGER_UNREG, 1, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "STORE")) {
            uint8_t r_sid = proc_alloc_temp_reg(pc);
            uint8_t m_args[1];
            proc_parse_val_to_reg(pc, pw, r_sid);
            m_args[0] = r_sid;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_DROP_STORE, 1, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        }
    }

    /* CREATE */
    if (tok.kind == CDB_TOK_KW_CREATE || cdb_token_equals_ci(&tok, "CREATE")) {
        cdb_lexer_next(&pc->lex, &tok);
        if (proc_match_str(pc, "ENCLAVE")) {
            uint8_t r_eid = proc_alloc_temp_reg(pc);
            uint8_t r_key = proc_alloc_temp_reg(pc);
            uint8_t m_args[2];
            proc_parse_val_to_reg(pc, pw, r_eid);
            proc_match_str(pc, "KEY");
            proc_parse_val_to_reg(pc, pw, r_key);
            m_args[0] = r_eid; m_args[1] = r_key;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CREATE_ENCLAVE, 2, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "TRIGGER")) {
            uint8_t r_name = proc_alloc_temp_reg(pc);
            uint8_t r_store = proc_alloc_temp_reg(pc);
            uint8_t r_ev = proc_alloc_temp_reg(pc);
            uint8_t r_proc = proc_alloc_temp_reg(pc);
            uint8_t r_depth = proc_alloc_temp_reg(pc);
            uint8_t m_args[5];
            proc_parse_val_to_reg(pc, pw, r_name);
            proc_match_str(pc, "ON");
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "AFTER");
            cdb_token ev_tok;
            uint64_t ev_mask = 1;
            if (cdb_lexer_peek(&pc->lex, &ev_tok) == CDB_OK) {
                if (cdb_token_equals_ci(&ev_tok, "INSERT")) ev_mask = 1;
                else if (cdb_token_equals_ci(&ev_tok, "UPDATE")) ev_mask = 2;
                else if (cdb_token_equals_ci(&ev_tok, "DELETE")) ev_mask = 4;
                else if (cdb_token_equals_ci(&ev_tok, "ALL")) ev_mask = 7;
                cdb_lexer_next(&pc->lex, &ev_tok);
            }
            uint8_t ev_enc[16]; size_t evl = encode_int_val((int64_t)ev_mask, ev_enc, sizeof(ev_enc));
            cdb_proc_write_load_const(pw, r_ev, ev_enc, evl);

            proc_match_str(pc, "EXECUTE");
            proc_parse_val_to_reg(pc, pw, r_proc);

            uint64_t max_depth = 8;
            if (proc_match_str(pc, "MAX")) {
                proc_match_str(pc, "DEPTH");
                cdb_token dtok;
                if (cdb_lexer_peek(&pc->lex, &dtok) == CDB_OK && dtok.kind == CDB_TOK_INT) {
                    cdb_lexer_next(&pc->lex, &dtok);
                    max_depth = (uint64_t)dtok.num_val.i64;
                }
            }
            uint8_t d_enc[16]; size_t dl = encode_int_val((int64_t)max_depth, d_enc, sizeof(d_enc));
            cdb_proc_write_load_const(pw, r_depth, d_enc, dl);

            m_args[0] = r_name; m_args[1] = r_store; m_args[2] = r_ev; m_args[3] = r_proc; m_args[4] = r_depth;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TRIGGER_REG, 5, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "LINK")) {
            proc_match_str(pc, "TYPE");
            uint8_t r_name = proc_alloc_temp_reg(pc);
            uint8_t r_card = proc_alloc_temp_reg(pc);
            uint8_t r_enf = proc_alloc_temp_reg(pc);
            uint8_t r_inv = proc_alloc_temp_reg(pc);
            uint8_t m_args[4];
            proc_parse_val_to_reg(pc, pw, r_name);

            uint64_t card = 3;
            if (proc_match_str(pc, "CARDINALITY")) {
                cdb_token ctok;
                if (cdb_lexer_peek(&pc->lex, &ctok) == CDB_OK) {
                    if (cdb_token_equals_ci(&ctok, "1:1")) card = 1;
                    else if (cdb_token_equals_ci(&ctok, "1:N")) card = 2;
                    else if (cdb_token_equals_ci(&ctok, "N:M")) card = 3;
                    cdb_lexer_next(&pc->lex, &ctok);
                }
            }
            uint8_t c_enc[16]; size_t cl = encode_int_val((int64_t)card, c_enc, sizeof(c_enc));
            cdb_proc_write_load_const(pw, r_card, c_enc, cl);

            uint64_t enf = proc_match_str(pc, "ENFORCED") ? 1 : 0;
            uint8_t enf_enc[16]; size_t el = encode_int_val((int64_t)enf, enf_enc, sizeof(enf_enc));
            cdb_proc_write_load_const(pw, r_enf, enf_enc, el);

            uint64_t inv = 0;
            if (proc_match_str(pc, "INVERSE")) {
                cdb_token itok;
                if (cdb_lexer_peek(&pc->lex, &itok) == CDB_OK && itok.kind == CDB_TOK_INT) {
                    cdb_lexer_next(&pc->lex, &itok);
                    inv = (uint64_t)itok.num_val.i64;
                }
            }
            uint8_t inv_enc[16]; size_t il = encode_int_val((int64_t)inv, inv_enc, sizeof(inv_enc));
            cdb_proc_write_load_const(pw, r_inv, inv_enc, il);

            m_args[0] = r_name; m_args[1] = r_card; m_args[2] = r_enf; m_args[3] = r_inv;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_LINK_DEF_TYPE, 4, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "STORE")) {
            uint8_t r_name = proc_alloc_temp_reg(pc);
            uint8_t r_model = proc_alloc_temp_reg(pc);
            uint8_t m_args[2];
            proc_parse_val_to_reg(pc, pw, r_name);
            proc_match_str(pc, "MODEL");

            uint64_t model = 2;
            cdb_token mtok;
            if (cdb_lexer_peek(&pc->lex, &mtok) == CDB_OK) {
                if (cdb_token_equals_ci(&mtok, "relational") || cdb_token_equals_ci(&mtok, "table")) model = 1;
                else if (cdb_token_equals_ci(&mtok, "document") || cdb_token_equals_ci(&mtok, "doc")) model = 2;
                else if (cdb_token_equals_ci(&mtok, "vector") || cdb_token_equals_ci(&mtok, "vec")) model = 3;
                else if (cdb_token_equals_ci(&mtok, "timeseries") || cdb_token_equals_ci(&mtok, "ts")) model = 4;
                else if (cdb_token_equals_ci(&mtok, "link") || cdb_token_equals_ci(&mtok, "links")) model = 5;
                else if (mtok.kind == CDB_TOK_INT) model = (uint64_t)mtok.num_val.i64;
                cdb_lexer_next(&pc->lex, &mtok);
            }
            uint8_t m_enc[16]; size_t ml = encode_int_val((int64_t)model, m_enc, sizeof(m_enc));
            cdb_proc_write_load_const(pw, r_model, m_enc, ml);
            m_args[0] = r_name; m_args[1] = r_model;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CREATE_STORE, 2, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        } else if (proc_match_str(pc, "SERIES")) {
            uint8_t r_store = proc_alloc_temp_reg(pc);
            uint8_t r_name = proc_alloc_temp_reg(pc);
            uint8_t m_args[2];
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_parse_val_to_reg(pc, pw, r_name);
            m_args[0] = r_store; m_args[1] = r_name;
            cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_SERIES, 2, m_args);
            pc->next_reg = base_reg;
            return CDB_OK;
        }
    }

    /* Flux create_* */
    if (cdb_token_equals_ci(&tok, "create_trigger")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_name = proc_alloc_temp_reg(pc);
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_ev = proc_alloc_temp_reg(pc);
        uint8_t r_proc = proc_alloc_temp_reg(pc);
        uint8_t r_depth = proc_alloc_temp_reg(pc);
        uint8_t m_args[5];
        proc_parse_arg(pc, pw, r_name);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_store);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_ev);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_proc);
        if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
            proc_parse_arg(pc, pw, r_depth);
        } else {
            uint8_t d_enc[16]; size_t dl = encode_int_val(8, d_enc, sizeof(d_enc));
            cdb_proc_write_load_const(pw, r_depth, d_enc, dl);
        }
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_name; m_args[1] = r_store; m_args[2] = r_ev; m_args[3] = r_proc; m_args[4] = r_depth;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TRIGGER_REG, 5, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (cdb_token_equals_ci(&tok, "create_link_type")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_name = proc_alloc_temp_reg(pc);
        uint8_t r_card = proc_alloc_temp_reg(pc);
        uint8_t r_enf = proc_alloc_temp_reg(pc);
        uint8_t r_inv = proc_alloc_temp_reg(pc);
        uint8_t m_args[4];
        proc_parse_arg(pc, pw, r_name);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_card);
        if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
            proc_parse_arg(pc, pw, r_enf);
        } else {
            uint8_t e_enc[16]; size_t el = encode_int_val(0, e_enc, sizeof(e_enc));
            cdb_proc_write_load_const(pw, r_enf, e_enc, el);
        }
        if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
            proc_parse_arg(pc, pw, r_inv);
        } else {
            uint8_t i_enc[16]; size_t il = encode_int_val(0, i_enc, sizeof(i_enc));
            cdb_proc_write_load_const(pw, r_inv, i_enc, il);
        }
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_name; m_args[1] = r_card; m_args[2] = r_enf; m_args[3] = r_inv;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_LINK_DEF_TYPE, 4, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (cdb_token_equals_ci(&tok, "create_store")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_name = proc_alloc_temp_reg(pc);
        uint8_t r_model = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];
        proc_parse_arg(pc, pw, r_name);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_model);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_name; m_args[1] = r_model;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_CREATE_STORE, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }
    if (cdb_token_equals_ci(&tok, "create_series") || cdb_token_equals_ci(&tok, "series")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_name = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];
        proc_parse_arg(pc, pw, r_store);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_name);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_store; m_args[1] = r_name;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_SERIES, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* ADD SCHEMA / add_schema(...) */
    if (cdb_token_equals_ci(&tok, "add_schema") || cdb_token_equals_ci(&tok, "ADD")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "SCHEMA");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_sch = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_sch);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_parse_val_to_reg(pc, pw, r_sch);
        }
        m_args[0] = r_store; m_args[1] = r_sch;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_ADD_SCHEMA, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* APPEND / append(...) */
    if (cdb_token_equals_ci(&tok, "append") || (tok.kind == CDB_TOK_KW_APPEND && cdb_token_equals_ci(&tok, "APPEND"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "INTO");
        proc_match_str(pc, "TIMESERIES");
        proc_match_str(pc, "TS");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_series = proc_alloc_temp_reg(pc);
        uint8_t r_ts = proc_alloc_temp_reg(pc);
        uint8_t r_val = proc_alloc_temp_reg(pc);
        uint8_t m_args[4];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_series);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_ts);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_val);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "SERIES");
            proc_parse_val_to_reg(pc, pw, r_series);
            proc_match_str(pc, "TIME");
            proc_parse_val_to_reg(pc, pw, r_ts);
            proc_match_str(pc, "VALUE");
            proc_parse_val_to_reg(pc, pw, r_val);
        }
        m_args[0] = r_store; m_args[1] = r_series; m_args[2] = r_ts; m_args[3] = r_val;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_APPEND, 4, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* PRUNE / prune(...) */
    if (cdb_token_equals_ci(&tok, "prune") || (tok.kind == CDB_TOK_KW_PRUNE && cdb_token_equals_ci(&tok, "PRUNE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "TIMESERIES");
        proc_match_str(pc, "TS");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_series = proc_alloc_temp_reg(pc);
        uint8_t r_ts = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_series);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_ts);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "SERIES");
            proc_parse_val_to_reg(pc, pw, r_series);
            proc_match_str(pc, "BEFORE");
            proc_parse_val_to_reg(pc, pw, r_ts);
        }
        m_args[0] = r_store; m_args[1] = r_series; m_args[2] = r_ts;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_DROP_BEFORE, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* RETENTION / set_retention(...) */
    if (cdb_token_equals_ci(&tok, "retention") || cdb_token_equals_ci(&tok, "set_retention")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '('", NULL);
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_series = proc_alloc_temp_reg(pc);
        uint8_t r_age = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        proc_parse_arg(pc, pw, r_store);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_series);
        proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_parse_arg(pc, pw, r_age);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        m_args[0] = r_store; m_args[1] = r_series; m_args[2] = r_age;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_RETENTION, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* APPLY RETENTION / apply_retention(...) */
    if (cdb_token_equals_ci(&tok, "apply_retention") || (tok.kind == CDB_TOK_KW_APPLY && cdb_token_equals_ci(&tok, "APPLY"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "RETENTION");
        proc_match_str(pc, "TIMESERIES");
        proc_match_str(pc, "TS");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_series = proc_alloc_temp_reg(pc);
        uint8_t r_ts = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_series);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_ts);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "SERIES");
            proc_parse_val_to_reg(pc, pw, r_series);
            proc_match_str(pc, "TIME");
            proc_parse_val_to_reg(pc, pw, r_ts);
        }
        m_args[0] = r_store; m_args[1] = r_series; m_args[2] = r_ts;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_APPLY_RET, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* DOWNSAMPLE / downsample(...) */
    if (cdb_token_equals_ci(&tok, "downsample") || (tok.kind == CDB_TOK_KW_DOWNSAMPLE && cdb_token_equals_ci(&tok, "DOWNSAMPLE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "TIMESERIES");
        proc_match_str(pc, "TS");
        proc_match_str(pc, "SOURCE");
        uint8_t r_s_store = proc_alloc_temp_reg(pc);
        uint8_t r_s_series = proc_alloc_temp_reg(pc);
        uint8_t r_d_store = proc_alloc_temp_reg(pc);
        uint8_t r_d_series = proc_alloc_temp_reg(pc);
        uint8_t r_step = proc_alloc_temp_reg(pc);
        uint8_t r_from = proc_alloc_temp_reg(pc);
        uint8_t r_to = proc_alloc_temp_reg(pc);
        uint8_t m_args[7];

        int in_paren = proc_match(pc, CDB_TOK_LPAREN, NULL);
        proc_parse_ref(pc, pw, r_s_store, r_s_series);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "INTO");
        proc_parse_ref(pc, pw, r_d_store, r_d_series);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "STEP");
        proc_parse_arg(pc, pw, r_step);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "FROM");
        proc_parse_arg(pc, pw, r_from);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "TO");
        proc_parse_arg(pc, pw, r_to);
        if (in_paren) proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);

        m_args[0] = r_s_store; m_args[1] = r_s_series; m_args[2] = r_d_store; m_args[3] = r_d_series;
        m_args[4] = r_step; m_args[5] = r_from; m_args[6] = r_to;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TS_DOWNSAMPLE, 7, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* FREE BLOB / free_blob(...) */
    if (cdb_token_equals_ci(&tok, "free_blob") || (tok.kind == CDB_TOK_KW_FREE && cdb_token_equals_ci(&tok, "FREE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "BLOB");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_page = proc_alloc_temp_reg(pc);
        uint8_t r_cnt = proc_alloc_temp_reg(pc);
        uint8_t r_len = proc_alloc_temp_reg(pc);
        uint8_t m_args[4];
        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_page);
            if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                proc_parse_arg(pc, pw, r_cnt);
            } else {
                uint8_t cenc[16]; size_t cl = encode_int_val(1, cenc, sizeof(cenc));
                cdb_proc_write_load_const(pw, r_cnt, cenc, cl);
            }
            if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                proc_parse_arg(pc, pw, r_len);
            } else {
                uint8_t lenc[16]; size_t ll = encode_int_val(0, lenc, sizeof(lenc));
                cdb_proc_write_load_const(pw, r_len, lenc, ll);
            }
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "PAGE");
            proc_parse_val_to_reg(pc, pw, r_page);
            if (proc_match_str(pc, "COUNT")) {
                proc_parse_val_to_reg(pc, pw, r_cnt);
            } else {
                uint8_t cenc[16]; size_t cl = encode_int_val(1, cenc, sizeof(cenc));
                cdb_proc_write_load_const(pw, r_cnt, cenc, cl);
            }
            if (proc_match_str(pc, "LEN")) {
                proc_parse_val_to_reg(pc, pw, r_len);
            } else {
                uint8_t lenc[16]; size_t ll = encode_int_val(0, lenc, sizeof(lenc));
                cdb_proc_write_load_const(pw, r_len, lenc, ll);
            }
        }
        m_args[0] = r_store; m_args[1] = r_page; m_args[2] = r_cnt; m_args[3] = r_len;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_BLOB_FREE, 4, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* LINK / link(...) */
    if (cdb_token_equals_ci(&tok, "link") || (tok.kind == CDB_TOK_KW_LINK && cdb_token_equals_ci(&tok, "LINK"))) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t r_type = proc_alloc_temp_reg(pc);
        uint8_t r_s_store = proc_alloc_temp_reg(pc);
        uint8_t r_s_entry = proc_alloc_temp_reg(pc);
        uint8_t r_t_store = proc_alloc_temp_reg(pc);
        uint8_t r_t_entry = proc_alloc_temp_reg(pc);
        uint8_t r_weight = proc_alloc_temp_reg(pc);
        uint8_t r_time = proc_alloc_temp_reg(pc);
        uint8_t m_args[7];

        int in_paren = proc_match(pc, CDB_TOK_LPAREN, NULL);
        proc_parse_arg(pc, pw, r_type);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "FROM");
        proc_parse_ref(pc, pw, r_s_store, r_s_entry);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "TO");
        proc_parse_ref(pc, pw, r_t_store, r_t_entry);

        if (proc_match_str(pc, "WEIGHT") || (in_paren && proc_match(pc, CDB_TOK_COMMA, NULL))) {
            proc_parse_arg(pc, pw, r_weight);
        } else {
            uint8_t wenc[16]; size_t wl = encode_f64_val(1.0, wenc, sizeof(wenc));
            cdb_proc_write_load_const(pw, r_weight, wenc, wl);
        }

        if (proc_match_str(pc, "TIME") || (in_paren && proc_match(pc, CDB_TOK_COMMA, NULL))) {
            proc_parse_arg(pc, pw, r_time);
        } else {
            uint8_t tenc[16]; size_t tl = encode_int_val(0, tenc, sizeof(tenc));
            cdb_proc_write_load_const(pw, r_time, tenc, tl);
        }
        if (in_paren) proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);

        m_args[0] = r_type; m_args[1] = r_s_store; m_args[2] = r_s_entry;
        m_args[3] = r_t_store; m_args[4] = r_t_entry; m_args[5] = r_weight; m_args[6] = r_time;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_LINK_ADD, 7, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* REINFORCE / reinforce(...) */
    if (cdb_token_equals_ci(&tok, "reinforce") || (tok.kind == CDB_TOK_KW_REINFORCE && cdb_token_equals_ci(&tok, "REINFORCE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "LINK");
        uint8_t r_type = proc_alloc_temp_reg(pc);
        uint8_t r_s_store = proc_alloc_temp_reg(pc);
        uint8_t r_s_entry = proc_alloc_temp_reg(pc);
        uint8_t r_t_store = proc_alloc_temp_reg(pc);
        uint8_t r_t_entry = proc_alloc_temp_reg(pc);
        uint8_t r_weight = proc_alloc_temp_reg(pc);
        uint8_t r_time = proc_alloc_temp_reg(pc);
        uint8_t m_args[7];

        int in_paren = proc_match(pc, CDB_TOK_LPAREN, NULL);
        proc_parse_arg(pc, pw, r_type);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "FROM");
        proc_parse_ref(pc, pw, r_s_store, r_s_entry);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "TO");
        proc_parse_ref(pc, pw, r_t_store, r_t_entry);

        if (proc_match_str(pc, "WEIGHT") || (in_paren && proc_match(pc, CDB_TOK_COMMA, NULL))) {
            proc_parse_arg(pc, pw, r_weight);
        } else {
            uint8_t wenc[16]; size_t wl = encode_f64_val(1.0, wenc, sizeof(wenc));
            cdb_proc_write_load_const(pw, r_weight, wenc, wl);
        }

        if (proc_match_str(pc, "TIME") || (in_paren && proc_match(pc, CDB_TOK_COMMA, NULL))) {
            proc_parse_arg(pc, pw, r_time);
        } else {
            uint8_t tenc[16]; size_t tl = encode_int_val(0, tenc, sizeof(tenc));
            cdb_proc_write_load_const(pw, r_time, tenc, tl);
        }
        if (in_paren) proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);

        m_args[0] = r_type; m_args[1] = r_s_store; m_args[2] = r_s_entry;
        m_args[3] = r_t_store; m_args[4] = r_t_entry; m_args[5] = r_weight; m_args[6] = r_time;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_LINK_REINFORCE, 7, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* UNLINK / unlink(...) */
    if (cdb_token_equals_ci(&tok, "unlink") || (tok.kind == CDB_TOK_KW_UNLINK && cdb_token_equals_ci(&tok, "UNLINK"))) {
        cdb_lexer_next(&pc->lex, &tok);
        uint8_t r_type = proc_alloc_temp_reg(pc);
        uint8_t r_s_store = proc_alloc_temp_reg(pc);
        uint8_t r_s_entry = proc_alloc_temp_reg(pc);
        uint8_t r_t_store = proc_alloc_temp_reg(pc);
        uint8_t r_t_entry = proc_alloc_temp_reg(pc);
        uint8_t m_args[5];

        int in_paren = proc_match(pc, CDB_TOK_LPAREN, NULL);
        proc_parse_arg(pc, pw, r_type);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "FROM");
        proc_parse_ref(pc, pw, r_s_store, r_s_entry);
        if (in_paren) proc_match(pc, CDB_TOK_COMMA, NULL);
        proc_match_str(pc, "TO");
        proc_parse_ref(pc, pw, r_t_store, r_t_entry);
        if (in_paren) proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);

        m_args[0] = r_type; m_args[1] = r_s_store; m_args[2] = r_s_entry;
        m_args[3] = r_t_store; m_args[4] = r_t_entry;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_LINK_REMOVE, 5, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* INSERT / insert(...) / table_insert(...) / vec_insert(...) */
    if (cdb_token_equals_ci(&tok, "insert") || cdb_token_equals_ci(&tok, "table_insert") || cdb_token_equals_ci(&tok, "vec_insert") ||
        (tok.kind == CDB_TOK_KW_INSERT && cdb_token_equals_ci(&tok, "INSERT"))) {
        uint8_t mut_op = CDB_MUT_DOC_INSERT;
        if (cdb_token_equals_ci(&tok, "table_insert")) mut_op = CDB_MUT_TABLE_INSERT;
        else if (cdb_token_equals_ci(&tok, "vec_insert")) mut_op = CDB_MUT_VEC_INSERT;

        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "INTO");
        if (proc_match_str(pc, "TABLE")) mut_op = CDB_MUT_TABLE_INSERT;
        else if (proc_match_str(pc, "VECTORS") || proc_match_str(pc, "VECTOR")) mut_op = CDB_MUT_VEC_INSERT;

        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_data = proc_alloc_temp_reg(pc);
        uint8_t r_sum = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];
        uint8_t argc = 2;

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_data);
            if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                proc_parse_arg(pc, pw, r_sum);
                argc = 3;
            }
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "VALUES");
            if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
                proc_parse_val_to_reg(pc, pw, r_data);
                if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                    proc_parse_val_to_reg(pc, pw, r_sum);
                    argc = 3;
                }
                proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
            } else {
                proc_parse_val_to_reg(pc, pw, r_data);
            }
        }
        m_args[0] = r_store; m_args[1] = r_data; m_args[2] = r_sum;
        cdb_proc_write_call_mutation_ex(pw, out_reg, mut_op, argc, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* UPDATE / update(...) */
    if (cdb_token_equals_ci(&tok, "update") || (tok.kind == CDB_TOK_KW_UPDATE && cdb_token_equals_ci(&tok, "UPDATE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "TABLE");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_entry = proc_alloc_temp_reg(pc);
        uint8_t r_data = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_entry);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_data);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "SET");
            proc_parse_val_to_reg(pc, pw, r_data);
            proc_match_str(pc, "WHERE");
            proc_match_str(pc, "ID");
            proc_match_str(pc, "ENTRY");
            proc_match(pc, CDB_TOK_EQ, NULL);
            proc_parse_val_to_reg(pc, pw, r_entry);
        }
        m_args[0] = r_store; m_args[1] = r_entry; m_args[2] = r_data;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_TABLE_UPDATE, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* PATCH / patch(...) */
    if (cdb_token_equals_ci(&tok, "patch") || (tok.kind == CDB_TOK_KW_PATCH && cdb_token_equals_ci(&tok, "PATCH"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "DOCUMENT");
        proc_match_str(pc, "DOC");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_entry = proc_alloc_temp_reg(pc);
        uint8_t r_data = proc_alloc_temp_reg(pc);
        uint8_t m_args[3];

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_entry);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_data);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_parse_val_to_reg(pc, pw, r_entry);
            proc_match_str(pc, "WITH");
            proc_parse_val_to_reg(pc, pw, r_data);
        }
        m_args[0] = r_store; m_args[1] = r_entry; m_args[2] = r_data;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_DOC_PATCH, 3, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* REPLACE / replace(...) */
    if (cdb_token_equals_ci(&tok, "replace") || (tok.kind == CDB_TOK_KW_REPLACE && cdb_token_equals_ci(&tok, "REPLACE"))) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "DOCUMENT");
        proc_match_str(pc, "DOC");
        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_entry = proc_alloc_temp_reg(pc);
        uint8_t r_data = proc_alloc_temp_reg(pc);
        uint8_t r_sum = proc_alloc_temp_reg(pc);
        uint8_t m_args[4];
        uint8_t argc = 3;

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_entry);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_data);
            if (proc_match(pc, CDB_TOK_COMMA, NULL)) {
                proc_parse_arg(pc, pw, r_sum);
                argc = 4;
            }
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_parse_val_to_reg(pc, pw, r_entry);
            proc_match_str(pc, "WITH");
            proc_parse_val_to_reg(pc, pw, r_data);
            if (proc_match_str(pc, "SUMMARY")) {
                proc_parse_val_to_reg(pc, pw, r_sum);
                argc = 4;
            }
        }
        m_args[0] = r_store; m_args[1] = r_entry; m_args[2] = r_data; m_args[3] = r_sum;
        cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_DOC_REPLACE, argc, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* DELETE / delete(...) */
    if (cdb_token_equals_ci(&tok, "delete") || (tok.kind == CDB_TOK_KW_DELETE && cdb_token_equals_ci(&tok, "DELETE"))) {
        uint8_t mut_op = CDB_MUT_DOC_DELETE;
        cdb_lexer_next(&pc->lex, &tok);
        proc_match_str(pc, "FROM");
        if (proc_match_str(pc, "TABLE")) mut_op = CDB_MUT_TABLE_DELETE;
        else if (proc_match_str(pc, "VECTORS") || proc_match_str(pc, "VECTOR")) mut_op = CDB_MUT_VEC_DELETE;

        uint8_t r_store = proc_alloc_temp_reg(pc);
        uint8_t r_entry = proc_alloc_temp_reg(pc);
        uint8_t m_args[2];

        if (proc_match(pc, CDB_TOK_LPAREN, NULL)) {
            proc_parse_arg(pc, pw, r_store);
            proc_match(pc, CDB_TOK_COMMA, NULL);
            proc_parse_arg(pc, pw, r_entry);
            proc_expect(pc, CDB_TOK_RPAREN, "Expected ')'", NULL);
        } else {
            proc_parse_val_to_reg(pc, pw, r_store);
            proc_match_str(pc, "WHERE");
            proc_match_str(pc, "ID");
            proc_match_str(pc, "ENTRY");
            proc_match(pc, CDB_TOK_EQ, NULL);
            proc_parse_val_to_reg(pc, pw, r_entry);
        }
        m_args[0] = r_store; m_args[1] = r_entry;
        cdb_proc_write_call_mutation_ex(pw, out_reg, mut_op, 2, m_args);
        pc->next_reg = base_reg;
        return CDB_OK;
    }

    /* Piped Flux source expressions: yield(...) | into(...) */
    if (cdb_token_equals_ci(&tok, "yield")) {
        cdb_lexer_next(&pc->lex, &tok);
        proc_expect(pc, CDB_TOK_LPAREN, "Expected '(' after yield", NULL);
        uint8_t r_val = proc_alloc_temp_reg(pc);
        proc_parse_arg(pc, pw, r_val);
        proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after yield argument", NULL);

        if (proc_match(pc, CDB_TOK_PIPE, NULL) || proc_match(pc, CDB_TOK_PIPE_FWD, NULL)) {
            cdb_token sink_tok;
            if (cdb_lexer_next(&pc->lex, &sink_tok) == CDB_OK) {
                if (cdb_token_equals_ci(&sink_tok, "into") || sink_tok.kind == CDB_TOK_KW_INTO) {
                    proc_expect(pc, CDB_TOK_LPAREN, "Expected '(' after into", NULL);
                    uint8_t r_store = proc_alloc_temp_reg(pc);
                    proc_parse_arg(pc, pw, r_store);
                    proc_expect(pc, CDB_TOK_RPAREN, "Expected ')' after into argument", NULL);

                    uint8_t m_args[2];
                    m_args[0] = r_store;
                    m_args[1] = r_val;
                    cdb_proc_write_call_mutation_ex(pw, out_reg, CDB_MUT_DOC_INSERT, 2, m_args);
                    pc->next_reg = base_reg;
                    return CDB_OK;
                }
            }
        }
    }

    /* Fallback: consume token */
    cdb_lexer_next(&pc->lex, &tok);
    return CDB_OK;
}

cdb_status cdb_compile_proc(const char *src, size_t len,
                            cdb_catalog *cat,
                            uint8_t *bc_buf, size_t bc_cap,
                            size_t *out_bc_len,
                            cdb_compile_error *err) {
    if (!src || !bc_buf || bc_cap == 0) return CDB_E_INVALID_ARG;

    proc_compiler pc;
    memset(&pc, 0, sizeof(pc));
    cdb_lexer_init(&pc.lex, src, len);
    pc.cat = cat;
    pc.err = err;
    pc.next_reg = 0;

    cdb_proc_writer pw;
    cdb_proc_writer_init(&pw, bc_buf, bc_cap, 64, 8);

    /* Optional PROCEDURE name(...) */
    cdb_token tok;
    if (proc_match(&pc, CDB_TOK_KW_PROCEDURE, NULL)) {
        if (cdb_lexer_next(&pc.lex, &tok) != CDB_OK) return CDB_E_INVALID_ARG;
        if (proc_match(&pc, CDB_TOK_LPAREN, NULL)) {
            while (!proc_match(&pc, CDB_TOK_RPAREN, NULL)) {
                cdb_token ptok;
                if (cdb_lexer_next(&pc.lex, &ptok) != CDB_OK) return CDB_E_INVALID_ARG;
                proc_match(&pc, CDB_TOK_COMMA, NULL);
            }
        }
    }

    /* Optional BEGIN ... END */
    proc_match(&pc, CDB_TOK_KW_BEGIN, NULL);
    proc_match(&pc, CDB_TOK_KW_TRANSACTION, NULL);
    proc_match(&pc, CDB_TOK_KW_TX, NULL);

    while (cdb_lexer_peek(&pc.lex, &tok) == CDB_OK && tok.kind != CDB_TOK_EOF) {
        if (tok.kind == CDB_TOK_KW_END || tok.kind == CDB_TOK_KW_COMMIT) {
            cdb_lexer_next(&pc.lex, &tok);
            proc_match(&pc, CDB_TOK_KW_TRANSACTION, NULL);
            proc_match(&pc, CDB_TOK_KW_TX, NULL);
            proc_match(&pc, CDB_TOK_SEMICOLON, NULL);
            break;
        }

        if (tok.kind == CDB_TOK_SEMICOLON) {
            cdb_lexer_next(&pc.lex, &tok);
            continue;
        }

        cdb_status st = proc_compile_statement(&pc, &pw);
        if (st != CDB_OK) {
            return st;
        }
        proc_match(&pc, CDB_TOK_SEMICOLON, NULL);
    }

    cdb_proc_write_halt(&pw);
    cdb_proc_writer_finish(&pw);

    size_t bclen = cdb_proc_writer_len(&pw);
    if (out_bc_len) *out_bc_len = bclen;
    return CDB_OK;
}
