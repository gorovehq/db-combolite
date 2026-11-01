#include "unity.h"
#include "cdb_lexer.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_lexer_keywords_and_case(void) {
    const char *src = "SELECT select From WHERE filter Join INNER LEFT OUTER ON GROUP BY ORDER SORT ASC DESC LIMIT OFFSET";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_SELECT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_SELECT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_FROM, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_WHERE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_FILTER, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_JOIN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_INNER, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_LEFT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_OUTER, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_ON, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_GROUP, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_BY, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_ORDER, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_SORT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_ASC, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_DESC, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_LIMIT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_OFFSET, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_EOF, tok.kind);
}

static void test_lexer_symbols_and_pipes(void) {
    const char *src = "| |> == != <= >= < > = + - * / % ( ) [ ] { } , ; : .";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PIPE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PIPE_FWD, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_EQ_EQ, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_NE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_GE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_GT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_EQ, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PLUS, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_MINUS, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_STAR, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_SLASH, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PERCENT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LPAREN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_RPAREN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LBRACKET, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_RBRACKET, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LBRACE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_RBRACE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_COMMA, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_SEMICOLON, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_COLON, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_DOT, tok.kind);
}

static void test_lexer_numbers_and_params(void) {
    const char *src = "1234 0x2A 3.1415 1.5e-3 $q $0 $param_name";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_INT, tok.kind);
    TEST_ASSERT_EQUAL_INT64(1234, tok.num_val.i64);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_INT, tok.kind);
    TEST_ASSERT_EQUAL_INT64(42, tok.num_val.i64);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_FLOAT, tok.kind);
    TEST_ASSERT_FLOAT_WITHIN(1e-4, 3.1415, (float)tok.num_val.f64);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_FLOAT, tok.kind);
    TEST_ASSERT_FLOAT_WITHIN(1e-5, 0.0015, (float)tok.num_val.f64);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PARAM, tok.kind);
    TEST_ASSERT_TRUE(cdb_token_equals(&tok, "$q"));

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PARAM, tok.kind);
    TEST_ASSERT_TRUE(cdb_token_equals(&tok, "$0"));

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PARAM, tok.kind);
    TEST_ASSERT_TRUE(cdb_token_equals(&tok, "$param_name"));
}

static void test_lexer_strings_and_unescape(void) {
    const char *src = "\"hello\\nworld\" 'single\\'quote' `sensor.temp.celsius`";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;
    char buf[128];
    size_t unesc_len = 0;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_STRING, tok.kind);
    TEST_ASSERT_TRUE(tok.is_quoted);
    TEST_ASSERT_EQUAL('"', tok.quote_char);
    TEST_ASSERT_EQUAL(CDB_OK, cdb_token_unescape_string(&tok, buf, sizeof(buf), &unesc_len));
    TEST_ASSERT_EQUAL_STRING("hello\nworld", buf);
    TEST_ASSERT_EQUAL(11, unesc_len);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_STRING, tok.kind);
    TEST_ASSERT_TRUE(tok.is_quoted);
    TEST_ASSERT_EQUAL('\'', tok.quote_char);
    TEST_ASSERT_EQUAL(CDB_OK, cdb_token_unescape_string(&tok, buf, sizeof(buf), &unesc_len));
    TEST_ASSERT_EQUAL_STRING("single'quote", buf);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_STRING, tok.kind);
    TEST_ASSERT_TRUE(tok.is_quoted);
    TEST_ASSERT_EQUAL('`', tok.quote_char);
    TEST_ASSERT_EQUAL(CDB_OK, cdb_token_unescape_string(&tok, buf, sizeof(buf), &unesc_len));
    TEST_ASSERT_EQUAL_STRING("sensor.temp.celsius", buf);
}

static void test_lexer_comments_and_positions(void) {
    const char *src =
        "-- This is a single-line comment\n"
        "/* Multi-line\n"
        "   comment block */\n"
        "SELECT 42;";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_SELECT, tok.kind);
    TEST_ASSERT_EQUAL_UINT32(4, tok.line);
    TEST_ASSERT_EQUAL_UINT32(1, tok.col);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_INT, tok.kind);
    TEST_ASSERT_EQUAL_UINT32(4, tok.line);
    TEST_ASSERT_EQUAL_UINT32(8, tok.col);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_SEMICOLON, tok.kind);
}

static void test_lexer_pipeline_stream(void) {
    const char *src = "Scan(\"telemetry\") | filter(temp > 85.0) | window(field: \"ts\", size: 60000) | limit(10);";
    cdb_lexer lex;
    cdb_lexer_init(&lex, src, strlen(src));
    cdb_token tok;

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_SCAN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LPAREN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_STRING, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_RPAREN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_PIPE, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_KW_FILTER, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_LPAREN, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_IDENT, tok.kind);
    TEST_ASSERT_TRUE(cdb_token_equals(&tok, "temp"));

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_GT, tok.kind);

    TEST_ASSERT_EQUAL(CDB_OK, cdb_lexer_next(&lex, &tok));
    TEST_ASSERT_EQUAL(CDB_TOK_FLOAT, tok.kind);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lexer_keywords_and_case);
    RUN_TEST(test_lexer_symbols_and_pipes);
    RUN_TEST(test_lexer_numbers_and_params);
    RUN_TEST(test_lexer_strings_and_unescape);
    RUN_TEST(test_lexer_comments_and_positions);
    RUN_TEST(test_lexer_pipeline_stream);
    return UNITY_END();
}
