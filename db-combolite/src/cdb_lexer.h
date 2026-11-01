/* CMBQL Text Language Tokenizer & Lexer (Spec 11.1, 11.11).
 *
 * Provides a zero-heap, streaming lexical analyzer for CMBQL queries and
 * stored procedures, with exact source position tracking (line, column),
 * dual-pipe operator support, parameter tokenization, and quoted identifier handling.
 */
#ifndef CDB_LEXER_H
#define CDB_LEXER_H

#include "combolite/cdb_common.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum cdb_token_kind {
    CDB_TOK_EOF = 0,
    CDB_TOK_ERROR,

    /* Literals & Identifiers */
    CDB_TOK_IDENT,        /* e.g. title, passages, my_var */
    CDB_TOK_PARAM,        /* $q, $0, $ebook_id */
    CDB_TOK_INT,          /* 42, 0xFF */
    CDB_TOK_FLOAT,        /* 3.14159, 1e-4 */
    CDB_TOK_STRING,       /* "hello", 'world', `field.with.dot` */

    /* Punctuation & Delimiters */
    CDB_TOK_LPAREN,       /* ( */
    CDB_TOK_RPAREN,       /* ) */
    CDB_TOK_LBRACKET,     /* [ */
    CDB_TOK_RBRACKET,     /* ] */
    CDB_TOK_LBRACE,       /* { */
    CDB_TOK_RBRACE,       /* } */
    CDB_TOK_COMMA,        /* , */
    CDB_TOK_SEMICOLON,    /* ; */
    CDB_TOK_COLON,        /* : */
    CDB_TOK_DOT,          /* . */
    CDB_TOK_PIPE,         /* | */
    CDB_TOK_PIPE_FWD,     /* |> */

    /* Operators */
    CDB_TOK_EQ,           /* = (assignment / set) */
    CDB_TOK_EQ_EQ,        /* == */
    CDB_TOK_NE,           /* != or <> */
    CDB_TOK_LT,           /* < */
    CDB_TOK_LE,           /* <= */
    CDB_TOK_GT,           /* > */
    CDB_TOK_GE,           /* >= */
    CDB_TOK_PLUS,         /* + */
    CDB_TOK_MINUS,        /* - */
    CDB_TOK_STAR,         /* * */
    CDB_TOK_SLASH,        /* / */
    CDB_TOK_PERCENT,      /* % */
    CDB_TOK_TILDE,        /* ~ */
    CDB_TOK_BANG,         /* ! */

    /* Keywords - Query & Clauses */
    CDB_TOK_KW_QUERY,
    CDB_TOK_KW_PROCEDURE,
    CDB_TOK_KW_SELECT,
    CDB_TOK_KW_FROM,
    CDB_TOK_KW_WHERE,
    CDB_TOK_KW_FILTER,
    CDB_TOK_KW_JOIN,
    CDB_TOK_KW_INNER,
    CDB_TOK_KW_LEFT,
    CDB_TOK_KW_OUTER,
    CDB_TOK_KW_ON,
    CDB_TOK_KW_GROUP,
    CDB_TOK_KW_AGGREGATE,
    CDB_TOK_KW_BY,
    CDB_TOK_KW_ORDER,
    CDB_TOK_KW_SORT,
    CDB_TOK_KW_ASC,
    CDB_TOK_KW_DESC,
    CDB_TOK_KW_NULLS,
    CDB_TOK_KW_FIRST,
    CDB_TOK_KW_LAST,
    CDB_TOK_KW_LIMIT,
    CDB_TOK_KW_OFFSET,
    CDB_TOK_KW_AS,
    CDB_TOK_KW_PROJECT,
    CDB_TOK_KW_UNNEST,
    CDB_TOK_KW_WINDOW,
    CDB_TOK_KW_ASOF_JOIN,

    /* Keywords - Logical & Membership */
    CDB_TOK_KW_AND,
    CDB_TOK_KW_OR,
    CDB_TOK_KW_NOT,
    CDB_TOK_KW_LIKE,
    CDB_TOK_KW_IN,

    /* Keywords - Literals */
    CDB_TOK_KW_TRUE,
    CDB_TOK_KW_FALSE,
    CDB_TOK_KW_NULL,

    /* Keywords - Procedures & Control Flow */
    CDB_TOK_KW_LET,
    CDB_TOK_KW_IF,
    CDB_TOK_KW_THEN,
    CDB_TOK_KW_ELSEIF,
    CDB_TOK_KW_ELSE,
    CDB_TOK_KW_END,
    CDB_TOK_KW_WHILE,
    CDB_TOK_KW_DO,
    CDB_TOK_KW_FOR,
    CDB_TOK_KW_RETURN,
    CDB_TOK_KW_CALL,
    CDB_TOK_KW_BEGIN,
    CDB_TOK_KW_COMMIT,
    CDB_TOK_KW_TRANSACTION,
    CDB_TOK_KW_TX,

    /* Keywords - Mutation & Storage Models */
    CDB_TOK_KW_INSERT,
    CDB_TOK_KW_INTO,
    CDB_TOK_KW_UPDATE,
    CDB_TOK_KW_SET,
    CDB_TOK_KW_DELETE,
    CDB_TOK_KW_LINK,
    CDB_TOK_KW_TO,
    CDB_TOK_KW_TYPE,
    CDB_TOK_KW_PROPERTIES,
    CDB_TOK_KW_UNLINK,
    CDB_TOK_KW_APPEND,
    CDB_TOK_KW_TIMESERIES,
    CDB_TOK_KW_TABLE,
    CDB_TOK_KW_POOL,
    CDB_TOK_KW_VECTOR,
    CDB_TOK_KW_VALUES,

    /* Builtin Operators */
    CDB_TOK_KW_SCAN,
    CDB_TOK_KW_TEXTSEARCH,
    CDB_TOK_KW_VECTORSEARCH,
    CDB_TOK_KW_HYBRIDRANK,
    CDB_TOK_KW_TRAVERSE,
    CDB_TOK_KW_FIELD
} cdb_token_kind;

typedef struct cdb_token {
    cdb_token_kind kind;
    const char *start;    /* Pointer into source text */
    uint32_t len;         /* Byte length of token */
    uint32_t line;        /* 1-based source line */
    uint32_t col;         /* 1-based source column */
    int is_quoted;        /* Non-zero if string or quoted identifier */
    char quote_char;      /* '"', '\'', or '`' */
    union {
        int64_t i64;
        double f64;
    } num_val;
} cdb_token;

typedef struct cdb_lexer {
    const char *src;
    size_t len;
    size_t pos;
    uint32_t line;
    uint32_t col;
    cdb_token peeked;
    int has_peeked;
} cdb_lexer;

/* Initialize a lexer over the given source text buffer. */
void cdb_lexer_init(cdb_lexer *lex, const char *src, size_t len);

/* Advance and return the next token. */
cdb_status cdb_lexer_next(cdb_lexer *lex, cdb_token *out_tok);

/* Peek at the next token without advancing the cursor. */
cdb_status cdb_lexer_peek(cdb_lexer *lex, cdb_token *out_tok);

/* Unescapes a string token's content (stripping quotes and processing \ escapes).
 * Writes up to dst_cap bytes into dst. Out_len is set to the byte length written. */
cdb_status cdb_token_unescape_string(const cdb_token *tok, char *dst, size_t dst_cap, size_t *out_len);

/* Compares a token's text with a given C string (case-sensitive or insensitive). */
int cdb_token_equals(const cdb_token *tok, const char *str);
int cdb_token_equals_ci(const cdb_token *tok, const char *str);

/* Returns a human-readable display name for the token kind. */
const char *cdb_token_kind_name(cdb_token_kind kind);

#ifdef __cplusplus
}
#endif

#endif /* CDB_LEXER_H */
