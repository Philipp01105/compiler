#include "errorHandler.h"
#include "lexer.h"

#include <assert.h>
#include <string.h>

static TokenStream *lex(const char *source, size_t length) {
    return tokenize_source(source, length, "<unit>");
}

int main(void) {
    ErrorHandler *handler = error_handler_init();
    assert(handler != NULL);
    error_handler_set_buffered(handler, 1);
    error_handler_set_global(handler);

    const char valid[] = "// comment\r\nfunc main()->void{\tprintln(\"Gr\xC3\xBC\xC3\x9F" "e\\n\");}";
    TokenStream *tokens = lex(valid, sizeof(valid) - 1);
    assert(tokens != NULL && !tokens->has_error);
    assert(tokens->count > 1);
    assert(tokens->tokens[tokens->count - 1].type == TOKEN_EOF);
    free_token_stream(tokens);

    const char repeated[] = "var repeated:int; repeated = repeated + repeated;";
    tokens = lex(repeated, sizeof(repeated) - 1U);
    assert(tokens != NULL && !tokens->has_error);
    const char *identity = tokens->tokens[1].value;
    assert(identity != NULL);
    assert(tokens->tokens[5].value == identity);
    assert(tokens->tokens[7].value == identity);
    assert(tokens->tokens[9].value == identity);
    assert(sizeof(Token) < MAX_TOKEN);
    free_token_stream(tokens);

    tokens = lex("", 0);
    assert(tokens != NULL && !tokens->has_error && tokens->count == 1);
    assert(tokens->tokens[0].type == TOKEN_EOF);
    free_token_stream(tokens);

    const char no_newline[] = "func main()->void{}";
    tokens = lex(no_newline, sizeof(no_newline) - 1);
    assert(tokens != NULL && !tokens->has_error);
    free_token_stream(tokens);

    const char escaped_quotes[] = "'\\\"' '\\''";
    tokens = lex(escaped_quotes, sizeof(escaped_quotes) - 1);
    assert(tokens != NULL && !tokens->has_error);
    assert(tokens->count == 3);
    assert(tokens->tokens[0].type == TOKEN_CHAR_LITERAL);
    assert(tokens->tokens[0].value[0] == '"');
    assert(tokens->tokens[1].type == TOKEN_CHAR_LITERAL);
    assert(tokens->tokens[1].value[0] == '\'');
    assert(tokens->tokens[0].end_column == tokens->tokens[0].column + 4);
    assert(tokens->tokens[1].end_column == tokens->tokens[1].column + 4);
    free_token_stream(tokens);

    const char postfix_casts[] = "3.(int) 3.5.(float) \"\\n\"";
    tokens = lex(postfix_casts, sizeof(postfix_casts)-1);
    assert(tokens != NULL && !tokens->has_error && tokens->count == 12);
    assert(tokens->tokens[0].type == TOKEN_NUMBER);
    assert(tokens->tokens[1].type == TOKEN_DOT);
    assert(tokens->tokens[5].type == TOKEN_FLOAT_LITERAL);
    assert(tokens->tokens[6].type == TOKEN_DOT);
    assert(tokens->tokens[10].type == TOKEN_STRING_LITERAL);
    assert(tokens->tokens[10].end_column == tokens->tokens[10].column + 4);
    free_token_stream(tokens);

    const char bad_escape[] = "\"bad\\q\"";
    tokens = lex(bad_escape, sizeof(bad_escape) - 1);
    assert(tokens != NULL && tokens->has_error);
    free_token_stream(tokens);
    error_handler_reset(handler);

    const char unclosed[] = "\"missing";
    tokens = lex(unclosed, sizeof(unclosed) - 1);
    assert(tokens != NULL && tokens->has_error);
    free_token_stream(tokens);

    error_handler_free(handler);
    error_handler_set_global(NULL);
    return 0;
}
