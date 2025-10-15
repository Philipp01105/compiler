#ifndef LEXER_H
#define LEXER_H

#include "compiler_types.h"

// Lexer Functions
TokenStream *create_token_stream(int initial_capacity);
void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column);
TokenStream *tokenize_source(const char *source);
void free_token_stream(TokenStream *stream);

// Token Stream Navigation
Token peek(TokenStream *stream);
Token peek_ahead(TokenStream *stream, int offset);
Token consume(TokenStream *stream);
int match(TokenStream *stream, TokenType type);
int check(TokenStream *stream, TokenType type);
int is_at_end(TokenStream *stream);

// Utility Functions
const char *token_type_to_string(TokenType type);
void print_token(Token token);
void print_all_tokens(TokenStream *stream);

#endif