#include "errorHandler.h"
#include "lexer.h"

#include <assert.h>
#include <limits.h>
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
    tokens->current = 1;
    assert(peek_ahead(tokens, -1).type == tokens->tokens[0].type);
    tokens->current = 0;
    assert(peek_ahead(tokens, -1).type == TOKEN_EOF);
    assert(peek_ahead(tokens, INT_MIN).type == TOKEN_EOF);
    free_token_stream(tokens);

    const char propagation[] = "value??";
    tokens = lex(propagation, sizeof(propagation) - 1U);
    assert(tokens != NULL && !tokens->has_error && tokens->count == 4);
    assert(tokens->tokens[1].type == TOKEN_QUESTION);
    assert(tokens->tokens[2].type == TOKEN_QUESTION);
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

    const char block[] = "/* documentation\n second line */func";
    tokens = lex(block, sizeof(block) - 1);
    assert(tokens != NULL && !tokens->has_error && tokens->tokens[0].type == TOKEN_KEYWORD_FUNC);
    assert(tokens->tokens[0].line == 2 && tokens->tokens[0].column == 16);
    free_token_stream(tokens);
    tokens = lex("/* unfinished", 13);
    assert(tokens != NULL && tokens->has_error);
    free_token_stream(tokens);

    const char postfix_casts[] = "3.(int) 3.5.(float) \"\\n\"";
    tokens = lex(postfix_casts, sizeof(postfix_casts) - 1);
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

    error_handler_reset(handler);
    const char multiple_chars[] = "'ab' 'x'";
    tokens = lex(multiple_chars, sizeof(multiple_chars) - 1);
    assert(tokens != NULL && tokens->has_error && handler->error_count == 1);
    assert(handler->buffer[0]->error_code == ERR_LEX_INVALID_CHAR_LITERAL);
    assert(tokens->tokens[0].type == TOKEN_CHAR_LITERAL && tokens->tokens[0].value[0] == 'x');
    free_token_stream(tokens);

    error_handler_reset(handler);
    const char escaped_single_quote[] = "\"\\'\"";
    tokens = lex(escaped_single_quote, sizeof(escaped_single_quote) - 1);
    assert(tokens != NULL && !tokens->has_error);
    free_token_stream(tokens);

    error_handler_reset(handler);
    const char unicode_unknown[] = "\xCE\xB1 $";
    tokens = lex(unicode_unknown, sizeof(unicode_unknown) - 1);
    assert(tokens != NULL && tokens->has_error && handler->error_count == 2);
    assert(handler->buffer[1]->column == 3);
    free_token_stream(tokens);

    error_handler_reset(handler);
    const char unicode_char[] = "'\xCE\xB1' $";
    tokens = lex(unicode_char, sizeof(unicode_char) - 1);
    assert(tokens != NULL && tokens->has_error && handler->error_count == 2);
    assert(handler->buffer[0]->error_code == ERR_LEX_INVALID_CHAR_LITERAL);
    assert(handler->buffer[1]->column == 5);
    free_token_stream(tokens);

    error_handler_reset(handler);
    FILE *json = tmpfile();
    assert(json != NULL);
    handler->output_stream = json;
    error_handler_set_json_output(handler, 1);
    ErrorContext *invalid_utf8 = error_context_create(SEVERITY_ERROR, 1, 1,
                                                      ERROR_CATEGORY_LEXER, ERR_LEX_UNKNOWN_CHAR, "<unit>",
                                                      "Invalid byte \xFF");
    error_context_set_source_line(invalid_utf8, "\xE0\x80 \xF4\x90\x80\x80 \xCE\xB1");
    error_report_context(handler, invalid_utf8);
    error_handler_flush(handler);
    rewind(json);
    char output[4096] = {0};
    size_t size = fread(output, 1, sizeof(output) - 1, json);
    assert(size > 0 && strstr(output, "\\uFFFD") != NULL);
    assert(strstr(output, "\xCE\xB1") != NULL);
    assert(strchr(output, (char) 0xFF) == NULL && strchr(output, (char) 0xE0) == NULL);
    fclose(json);
    handler->output_stream = stderr;

    error_handler_free(handler);
    error_handler_set_global(NULL);
    return 0;
}
