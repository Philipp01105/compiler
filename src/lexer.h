#ifndef LEXER_H
#define LEXER_H

#include "compiler_types.h"

/* Token stream management */
TokenStream *create_token_stream(void);
void free_token_stream(TokenStream *stream);
void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column);

/* Token stream navigation */
Token peek(TokenStream *stream);
Token peek_ahead(TokenStream *stream, int offset);
Token consume(TokenStream *stream);
int check(TokenStream *stream, TokenType type);
int match(TokenStream *stream, TokenType type);
int is_at_end(TokenStream *stream);

/* Tokenization */
TokenStream *tokenize_file(const char *filename, int debug_mode);
void print_tokens(TokenStream *stream);

/* Helper functions */
const char *token_type_to_string(TokenType type);

#endif