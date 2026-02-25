#ifndef DMM_TOKEN_H
#define DMM_TOKEN_H

#include "language_types.h"

typedef struct {
    TokenType type;
    char value[MAX_TOKEN];
    int line;
    int column;
} Token;

typedef struct {
    Token *tokens;
    int count;
    int current;
    int capacity;
    int has_error;
} TokenStream;

#endif
