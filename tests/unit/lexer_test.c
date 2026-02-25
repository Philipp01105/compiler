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
