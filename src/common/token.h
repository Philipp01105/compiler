#ifndef DMM_TOKEN_H
#define DMM_TOKEN_H

#include "language_types.h"
#include "string_interner.h"

typedef struct {
    TokenType type;
    const char *value;
    int line;
    int column;
} Token;

typedef struct {
    Token *tokens;
    int count;
    int current;
    int capacity;
    int has_error;
    StringInterner *strings;
    int owns_strings;
} TokenStream;

#endif
