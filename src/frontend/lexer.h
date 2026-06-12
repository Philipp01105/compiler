#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>
#include "token.h"

/* Token stream management */
TokenStream *create_token_stream(void);

TokenStream *create_token_stream_with_interner(StringInterner * interner);

void free_token_stream(TokenStream * stream);

void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column);

/* Token stream navigation */
Token peek(const TokenStream *stream);

Token peek_ahead(const TokenStream *stream, int offset);

Token consume(TokenStream * stream);

int check(const TokenStream *stream, TokenType type);

int match(TokenStream *stream, TokenType type);

int is_at_end(const TokenStream *stream);

/* Tokenization */
TokenStream *tokenize_file(const char *filename, int debug_mode);

TokenStream *tokenize_file_with_interner(const char *filename, int debug_mode,
                                         StringInterner *interner);

/* Tokenize an in-memory byte sequence. The buffer need not be NUL-terminated. */
TokenStream *tokenize_source(const char *source, size_t length, const char *filename);

TokenStream *tokenize_source_with_interner(const char *source, size_t length,
                                           const char *filename,
                                           StringInterner *interner);

void print_tokens(const TokenStream *stream);

/* Helper functions */
const char *token_type_to_string(TokenType type);

#endif
