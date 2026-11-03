/* CMBQL Text Language Tokenizer & Lexer (Spec 11.1, 11.11).
 *
 * Zero-heap streaming lexical analyzer for CMBQL.
 */
#include "cdb_lexer.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct kw_entry {
    const char *name;
    cdb_token_kind kind;
} kw_entry;

static const kw_entry KEYWORDS[] = {
    {"QUERY", CDB_TOK_KW_QUERY},
    {"PROCEDURE", CDB_TOK_KW_PROCEDURE},
    {"SELECT", CDB_TOK_KW_SELECT},
    {"FROM", CDB_TOK_KW_FROM},
    {"WHERE", CDB_TOK_KW_WHERE},
    {"FILTER", CDB_TOK_KW_FILTER},
    {"JOIN", CDB_TOK_KW_JOIN},
    {"INNER", CDB_TOK_KW_INNER},
    {"LEFT", CDB_TOK_KW_LEFT},
    {"OUTER", CDB_TOK_KW_OUTER},
    {"ON", CDB_TOK_KW_ON},
    {"GROUP", CDB_TOK_KW_GROUP},
    {"AGGREGATE", CDB_TOK_KW_AGGREGATE},
    {"BY", CDB_TOK_KW_BY},
    {"ORDER", CDB_TOK_KW_ORDER},
    {"SORT", CDB_TOK_KW_SORT},
    {"ASC", CDB_TOK_KW_ASC},
    {"DESC", CDB_TOK_KW_DESC},
    {"NULLS", CDB_TOK_KW_NULLS},
    {"FIRST", CDB_TOK_KW_FIRST},
    {"LAST", CDB_TOK_KW_LAST},
    {"LIMIT", CDB_TOK_KW_LIMIT},
    {"OFFSET", CDB_TOK_KW_OFFSET},
    {"AS", CDB_TOK_KW_AS},
    {"PROJECT", CDB_TOK_KW_PROJECT},
    {"UNNEST", CDB_TOK_KW_UNNEST},
    {"WINDOW", CDB_TOK_KW_WINDOW},
    {"ASOF_JOIN", CDB_TOK_KW_ASOF_JOIN},
    {"AND", CDB_TOK_KW_AND},
    {"OR", CDB_TOK_KW_OR},
    {"NOT", CDB_TOK_KW_NOT},
    {"LIKE", CDB_TOK_KW_LIKE},
    {"IN", CDB_TOK_KW_IN},
    {"TRUE", CDB_TOK_KW_TRUE},
    {"FALSE", CDB_TOK_KW_FALSE},
    {"NULL", CDB_TOK_KW_NULL},
    {"LET", CDB_TOK_KW_LET},
    {"IF", CDB_TOK_KW_IF},
    {"THEN", CDB_TOK_KW_THEN},
    {"ELSEIF", CDB_TOK_KW_ELSEIF},
    {"ELSE", CDB_TOK_KW_ELSE},
    {"END", CDB_TOK_KW_END},
    {"WHILE", CDB_TOK_KW_WHILE},
    {"DO", CDB_TOK_KW_DO},
    {"FOR", CDB_TOK_KW_FOR},
    {"RETURN", CDB_TOK_KW_RETURN},
    {"CALL", CDB_TOK_KW_CALL},
    {"BEGIN", CDB_TOK_KW_BEGIN},
    {"COMMIT", CDB_TOK_KW_COMMIT},
    {"TRANSACTION", CDB_TOK_KW_TRANSACTION},
    {"TX", CDB_TOK_KW_TX},
    {"INSERT", CDB_TOK_KW_INSERT},
    {"INTO", CDB_TOK_KW_INTO},
    {"UPDATE", CDB_TOK_KW_UPDATE},
    {"SET", CDB_TOK_KW_SET},
    {"DELETE", CDB_TOK_KW_DELETE},
    {"LINK", CDB_TOK_KW_LINK},
    {"TO", CDB_TOK_KW_TO},
    {"TYPE", CDB_TOK_KW_TYPE},
    {"PROPERTIES", CDB_TOK_KW_PROPERTIES},
    {"UNLINK", CDB_TOK_KW_UNLINK},
    {"APPEND", CDB_TOK_KW_APPEND},
    {"TIMESERIES", CDB_TOK_KW_TIMESERIES},
    {"TABLE", CDB_TOK_KW_TABLE},
    {"POOL", CDB_TOK_KW_POOL},
    {"VECTOR", CDB_TOK_KW_VECTOR},
    {"VALUES", CDB_TOK_KW_VALUES},
    {"CREATE", CDB_TOK_KW_CREATE},
    {"DROP", CDB_TOK_KW_DROP},
    {"ENCLAVE", CDB_TOK_KW_ENCLAVE},
    {"OPEN", CDB_TOK_KW_OPEN},
    {"ROTATE", CDB_TOK_KW_ROTATE},
    {"MASTER", CDB_TOK_KW_MASTER},
    {"KEY", CDB_TOK_KW_KEY},
    {"CHECK", CDB_TOK_KW_CHECK},
    {"ROTATION", CDB_TOK_KW_ROTATION},
    {"REGISTER", CDB_TOK_KW_REGISTER},
    {"PRINCIPAL", CDB_TOK_KW_PRINCIPAL},
    {"TENANT", CDB_TOK_KW_TENANT},
    {"PUBKEY", CDB_TOK_KW_PUBKEY},
    {"GRANT", CDB_TOK_KW_GRANT},
    {"ROLE", CDB_TOK_KW_ROLE},
    {"PERMS", CDB_TOK_KW_PERMS},
    {"PERM", CDB_TOK_KW_PERM},
    {"RECONCILE", CDB_TOK_KW_RECONCILE},
    {"COMPACT", CDB_TOK_KW_COMPACT},
    {"RECLAIM", CDB_TOK_KW_RECLAIM},
    {"TRIGGER", CDB_TOK_KW_TRIGGER},
    {"SAVEPOINT", CDB_TOK_KW_SAVEPOINT},
    {"ROLLBACK", CDB_TOK_KW_ROLLBACK},
    {"RELEASE", CDB_TOK_KW_RELEASE},
    {"DURABILITY", CDB_TOK_KW_DURABILITY},
    {"STORE", CDB_TOK_KW_STORE},
    {"MODEL", CDB_TOK_KW_MODEL},
    {"SCHEMA", CDB_TOK_KW_SCHEMA},
    {"REPLACE", CDB_TOK_KW_REPLACE},
    {"PATCH", CDB_TOK_KW_PATCH},
    {"SUMMARY", CDB_TOK_KW_SUMMARY},
    {"WEIGHT", CDB_TOK_KW_WEIGHT},
    {"TIME", CDB_TOK_KW_TIME},
    {"REINFORCE", CDB_TOK_KW_REINFORCE},
    {"SERIES", CDB_TOK_KW_SERIES},
    {"TAGS", CDB_TOK_KW_TAGS},
    {"BEFORE", CDB_TOK_KW_BEFORE},
    {"PRUNE", CDB_TOK_KW_PRUNE},
    {"RETENTION", CDB_TOK_KW_RETENTION},
    {"AGE", CDB_TOK_KW_AGE},
    {"DOWNSAMPLE", CDB_TOK_KW_DOWNSAMPLE},
    {"STEP", CDB_TOK_KW_STEP},
    {"SOURCE", CDB_TOK_KW_SOURCE},
    {"BLOB", CDB_TOK_KW_BLOB},
    {"FREE", CDB_TOK_KW_FREE},
    {"REF", CDB_TOK_KW_REF},
    {"PEER", CDB_TOK_KW_PEER},
    {"SYNC", CDB_TOK_KW_SYNC},
    {"SINCE", CDB_TOK_KW_SINCE},
    {"EXPORT", CDB_TOK_KW_EXPORT},
    {"IMPORT", CDB_TOK_KW_IMPORT},
    {"YIELD", CDB_TOK_KW_YIELD},
    {"ACTION", CDB_TOK_KW_ACTION},
    {"DATA", CDB_TOK_KW_DATA},
    {"SEARCH", CDB_TOK_KW_SEARCH},
    {"CARDINALITY", CDB_TOK_KW_CARDINALITY},
    {"ENFORCED", CDB_TOK_KW_ENFORCED},
    {"FIND", CDB_TOK_KW_FIND},
    {"APPLY", CDB_TOK_KW_APPLY},
    {"SCAN", CDB_TOK_KW_SCAN},
    {"TEXTSEARCH", CDB_TOK_KW_TEXTSEARCH},
    {"VECTORSEARCH", CDB_TOK_KW_VECTORSEARCH},
    {"HYBRIDRANK", CDB_TOK_KW_HYBRIDRANK},
    {"TRAVERSE", CDB_TOK_KW_TRAVERSE},
    {"FIELD", CDB_TOK_KW_FIELD}
};

static int cdb_ci_char_eq(char a, char b) {
    if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
    return a == b;
}

static int cdb_ci_strn_eq(const char *s, const char *kw, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (!cdb_ci_char_eq(s[i], kw[i])) return 0;
    }
    return kw[len] == '\0';
}

void cdb_lexer_init(cdb_lexer *lex, const char *src, size_t len) {
    lex->src = src;
    lex->len = len;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->has_peeked = 0;
    memset(&lex->peeked, 0, sizeof(lex->peeked));
}

static char lexer_peek_char(const cdb_lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    return lex->src[lex->pos];
}

static char lexer_peek_next_char(const cdb_lexer *lex) {
    if (lex->pos + 1 >= lex->len) return '\0';
    return lex->src[lex->pos + 1];
}

static char lexer_advance_char(cdb_lexer *lex) {
    if (lex->pos >= lex->len) return '\0';
    char c = lex->src[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

static void skip_whitespace_and_comments(cdb_lexer *lex) {
    while (lex->pos < lex->len) {
        char c = lexer_peek_char(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lexer_advance_char(lex);
        } else if (c == '-' && lexer_peek_next_char(lex) == '-') {
            /* Single-line comment */
            lexer_advance_char(lex);
            lexer_advance_char(lex);
            while (lex->pos < lex->len && lexer_peek_char(lex) != '\n') {
                lexer_advance_char(lex);
            }
        } else if (c == '/' && lexer_peek_next_char(lex) == '*') {
            /* Block comment */
            lexer_advance_char(lex);
            lexer_advance_char(lex);
            while (lex->pos < lex->len) {
                if (lexer_peek_char(lex) == '*' && lexer_peek_next_char(lex) == '/') {
                    lexer_advance_char(lex);
                    lexer_advance_char(lex);
                    break;
                }
                lexer_advance_char(lex);
            }
        } else {
            break;
        }
    }
}

cdb_status cdb_lexer_next(cdb_lexer *lex, cdb_token *out_tok) {
    if (lex->has_peeked) {
        *out_tok = lex->peeked;
        lex->has_peeked = 0;
        return CDB_OK;
    }

    skip_whitespace_and_comments(lex);

    memset(out_tok, 0, sizeof(*out_tok));
    out_tok->line = lex->line;
    out_tok->col = lex->col;
    out_tok->start = &lex->src[lex->pos];

    if (lex->pos >= lex->len) {
        out_tok->kind = CDB_TOK_EOF;
        out_tok->len = 0;
        return CDB_OK;
    }

    char c = lexer_peek_char(lex);

    /* Parameter: $ident */
    if (c == '$') {
        lexer_advance_char(lex);
        while (lex->pos < lex->len) {
            char nc = lexer_peek_char(lex);
            if (isalnum((unsigned char)nc) || nc == '_') {
                lexer_advance_char(lex);
            } else {
                break;
            }
        }
        out_tok->kind = CDB_TOK_PARAM;
        out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);
        return CDB_OK;
    }

    /* String literal: '...', "...", `...` */
    if (c == '"' || c == '\'' || c == '`') {
        char quote = lexer_advance_char(lex);
        out_tok->is_quoted = 1;
        out_tok->quote_char = quote;
        int escaped = 0;

        while (lex->pos < lex->len) {
            char sc = lexer_advance_char(lex);
            if (escaped) {
                escaped = 0;
            } else if (sc == '\\') {
                escaped = 1;
            } else if (sc == quote) {
                break;
            }
        }
        out_tok->kind = CDB_TOK_STRING;
        out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);
        return CDB_OK;
    }

    /* Number literal (hex or decimal or float) */
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lexer_peek_next_char(lex)))) {
        int is_float = 0;
        if (c == '0' && (lexer_peek_next_char(lex) == 'x' || lexer_peek_next_char(lex) == 'X')) {
            /* Hex integer */
            lexer_advance_char(lex);
            lexer_advance_char(lex);
            while (lex->pos < lex->len && isxdigit((unsigned char)lexer_peek_char(lex))) {
                lexer_advance_char(lex);
            }
            out_tok->kind = CDB_TOK_INT;
            out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);
            char tmp[64];
            size_t copy_len = out_tok->len < sizeof(tmp) - 1 ? out_tok->len : sizeof(tmp) - 1;
            memcpy(tmp, out_tok->start, copy_len);
            tmp[copy_len] = '\0';
            out_tok->num_val.i64 = (int64_t)strtoull(tmp, NULL, 16);
            return CDB_OK;
        }

        while (lex->pos < lex->len && isdigit((unsigned char)lexer_peek_char(lex))) {
            lexer_advance_char(lex);
        }
        if (lexer_peek_char(lex) == '.' && isdigit((unsigned char)lexer_peek_next_char(lex))) {
            is_float = 1;
            lexer_advance_char(lex); /* '.' */
            while (lex->pos < lex->len && isdigit((unsigned char)lexer_peek_char(lex))) {
                lexer_advance_char(lex);
            }
        }
        if (lexer_peek_char(lex) == 'e' || lexer_peek_char(lex) == 'E') {
            is_float = 1;
            lexer_advance_char(lex);
            if (lexer_peek_char(lex) == '+' || lexer_peek_char(lex) == '-') {
                lexer_advance_char(lex);
            }
            while (lex->pos < lex->len && isdigit((unsigned char)lexer_peek_char(lex))) {
                lexer_advance_char(lex);
            }
        }
        out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);
        char tmp[64];
        size_t copy_len = out_tok->len < sizeof(tmp) - 1 ? out_tok->len : sizeof(tmp) - 1;
        memcpy(tmp, out_tok->start, copy_len);
        tmp[copy_len] = '\0';

        if (is_float) {
            out_tok->kind = CDB_TOK_FLOAT;
            out_tok->num_val.f64 = strtod(tmp, NULL);
        } else {
            out_tok->kind = CDB_TOK_INT;
            out_tok->num_val.i64 = (int64_t)strtoll(tmp, NULL, 10);
        }
        return CDB_OK;
    }

    /* Identifier or keyword */
    if (isalpha((unsigned char)c) || c == '_') {
        lexer_advance_char(lex);
        while (lex->pos < lex->len) {
            char ic = lexer_peek_char(lex);
            if (isalnum((unsigned char)ic) || ic == '_') {
                lexer_advance_char(lex);
            } else {
                break;
            }
        }
        out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);

        /* Keyword lookup */
        size_t n_kw = sizeof(KEYWORDS) / sizeof(KEYWORDS[0]);
        for (size_t i = 0; i < n_kw; ++i) {
            if (cdb_ci_strn_eq(out_tok->start, KEYWORDS[i].name, (size_t)out_tok->len)) {
                out_tok->kind = KEYWORDS[i].kind;
                return CDB_OK;
            }
        }
        out_tok->kind = CDB_TOK_IDENT;
        return CDB_OK;
    }

    /* Punctuation & multi-char operators */
    lexer_advance_char(lex);
    switch (c) {
        case '(': out_tok->kind = CDB_TOK_LPAREN; break;
        case ')': out_tok->kind = CDB_TOK_RPAREN; break;
        case '[': out_tok->kind = CDB_TOK_LBRACKET; break;
        case ']': out_tok->kind = CDB_TOK_RBRACKET; break;
        case '{': out_tok->kind = CDB_TOK_LBRACE; break;
        case '}': out_tok->kind = CDB_TOK_RBRACE; break;
        case ',': out_tok->kind = CDB_TOK_COMMA; break;
        case ';': out_tok->kind = CDB_TOK_SEMICOLON; break;
        case ':': out_tok->kind = CDB_TOK_COLON; break;
        case '.': out_tok->kind = CDB_TOK_DOT; break;
        case '+': out_tok->kind = CDB_TOK_PLUS; break;
        case '-': out_tok->kind = CDB_TOK_MINUS; break;
        case '*': out_tok->kind = CDB_TOK_STAR; break;
        case '/': out_tok->kind = CDB_TOK_SLASH; break;
        case '%': out_tok->kind = CDB_TOK_PERCENT; break;
        case '~': out_tok->kind = CDB_TOK_TILDE; break;

        case '|':
            if (lexer_peek_char(lex) == '>') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_PIPE_FWD;
            } else {
                out_tok->kind = CDB_TOK_PIPE;
            }
            break;

        case '=':
            if (lexer_peek_char(lex) == '=') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_EQ_EQ;
            } else {
                out_tok->kind = CDB_TOK_EQ;
            }
            break;

        case '!':
            if (lexer_peek_char(lex) == '=') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_NE;
            } else {
                out_tok->kind = CDB_TOK_BANG;
            }
            break;

        case '<':
            if (lexer_peek_char(lex) == '=') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_LE;
            } else if (lexer_peek_char(lex) == '>') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_NE;
            } else {
                out_tok->kind = CDB_TOK_LT;
            }
            break;

        case '>':
            if (lexer_peek_char(lex) == '=') {
                lexer_advance_char(lex);
                out_tok->kind = CDB_TOK_GE;
            } else {
                out_tok->kind = CDB_TOK_GT;
            }
            break;

        default:
            out_tok->kind = CDB_TOK_ERROR;
            break;
    }

    out_tok->len = (uint32_t)(&lex->src[lex->pos] - out_tok->start);
    return CDB_OK;
}

cdb_status cdb_lexer_peek(cdb_lexer *lex, cdb_token *out_tok) {
    if (!lex->has_peeked) {
        cdb_status st = cdb_lexer_next(lex, &lex->peeked);
        if (st != CDB_OK) return st;
        lex->has_peeked = 1;
    }
    *out_tok = lex->peeked;
    return CDB_OK;
}

cdb_status cdb_token_unescape_string(const cdb_token *tok, char *dst, size_t dst_cap, size_t *out_len) {
    if (!tok || !dst || dst_cap == 0) return CDB_E_INVALID_ARG;
    if (tok->kind != CDB_TOK_STRING && !tok->is_quoted) {
        /* Direct copy */
        size_t copy_len = (size_t)tok->len < dst_cap - 1 ? (size_t)tok->len : dst_cap - 1;
        memcpy(dst, tok->start, copy_len);
        dst[copy_len] = '\0';
        if (out_len) *out_len = copy_len;
        return CDB_OK;
    }

    if (tok->len < 2) {
        dst[0] = '\0';
        if (out_len) *out_len = 0;
        return CDB_OK;
    }

    const char *p = tok->start + 1; /* Skip opening quote */
    const char *end = tok->start + tok->len;
    if (*(end - 1) == tok->quote_char) {
        end--; /* Skip closing quote */
    }

    size_t out_idx = 0;
    while (p < end && out_idx + 1 < dst_cap) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n': dst[out_idx++] = '\n'; break;
                case 'r': dst[out_idx++] = '\r'; break;
                case 't': dst[out_idx++] = '\t'; break;
                case '\\': dst[out_idx++] = '\\'; break;
                case '\'': dst[out_idx++] = '\''; break;
                case '"': dst[out_idx++] = '"'; break;
                case '`': dst[out_idx++] = '`'; break;
                case '0': dst[out_idx++] = '\0'; break;
                default: dst[out_idx++] = *p; break;
            }
            p++;
        } else {
            dst[out_idx++] = *p++;
        }
    }
    dst[out_idx] = '\0';
    if (out_len) *out_len = out_idx;
    return CDB_OK;
}

int cdb_token_equals(const cdb_token *tok, const char *str) {
    if (!tok || !str) return 0;
    size_t slen = strlen(str);
    if ((size_t)tok->len != slen) return 0;
    return memcmp(tok->start, str, slen) == 0;
}

int cdb_token_equals_ci(const cdb_token *tok, const char *str) {
    if (!tok || !str) return 0;
    size_t slen = strlen(str);
    if ((size_t)tok->len != slen) return 0;
    for (size_t i = 0; i < slen; ++i) {
        if (!cdb_ci_char_eq(tok->start[i], str[i])) return 0;
    }
    return 1;
}

const char *cdb_token_kind_name(cdb_token_kind kind) {
    switch (kind) {
        case CDB_TOK_EOF: return "EOF";
        case CDB_TOK_ERROR: return "ERROR";
        case CDB_TOK_IDENT: return "IDENT";
        case CDB_TOK_PARAM: return "PARAM";
        case CDB_TOK_INT: return "INT";
        case CDB_TOK_FLOAT: return "FLOAT";
        case CDB_TOK_STRING: return "STRING";
        case CDB_TOK_LPAREN: return "(";
        case CDB_TOK_RPAREN: return ")";
        case CDB_TOK_LBRACKET: return "[";
        case CDB_TOK_RBRACKET: return "]";
        case CDB_TOK_LBRACE: return "{";
        case CDB_TOK_RBRACE: return "}";
        case CDB_TOK_COMMA: return ",";
        case CDB_TOK_SEMICOLON: return ";";
        case CDB_TOK_COLON: return ":";
        case CDB_TOK_DOT: return ".";
        case CDB_TOK_PIPE: return "|";
        case CDB_TOK_PIPE_FWD: return "|>";
        case CDB_TOK_EQ: return "=";
        case CDB_TOK_EQ_EQ: return "==";
        case CDB_TOK_NE: return "!=";
        case CDB_TOK_LT: return "<";
        case CDB_TOK_LE: return "<=";
        case CDB_TOK_GT: return ">";
        case CDB_TOK_GE: return ">=";
        case CDB_TOK_PLUS: return "+";
        case CDB_TOK_MINUS: return "-";
        case CDB_TOK_STAR: return "*";
        case CDB_TOK_SLASH: return "/";
        case CDB_TOK_PERCENT: return "%";
        case CDB_TOK_TILDE: return "~";
        case CDB_TOK_BANG: return "!";
        default: return "KEYWORD";
    }
}
