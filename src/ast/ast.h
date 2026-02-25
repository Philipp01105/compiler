#ifndef DMM_AST_H
#define DMM_AST_H

#include <stddef.h>
#include "language_types.h"

typedef struct {
    int line;
    int column;
} AstSourceLocation;

typedef struct {
    AstSourceLocation begin;
    AstSourceLocation end;
} AstSourceSpan;

typedef enum {
    AST_DECL_IMPORT,
    AST_DECL_STRUCT,
    AST_DECL_ENUM,
    AST_DECL_FUNCTION,
    AST_DECL_INVALID
} AstDeclarationKind;

/*
 * A lossless token leaf. Tokens are copied into the AST so the lexer stream can
 * be destroyed as soon as frontend parsing finishes. The backend never owns or
 * observes the lexer's transient storage.
 */
typedef struct {
    TokenType type;
    char lexeme[MAX_TOKEN];
    AstSourceSpan span;
} AstToken;

typedef struct {
    AstDeclarationKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
} AstDeclaration;

typedef struct AstProgram {
    char *source_path;
    AstToken *tokens;
    size_t token_count;
    AstDeclaration *declarations;
    size_t declaration_count;
} AstProgram;

void ast_program_free(AstProgram *program);

const char *ast_declaration_kind_name(AstDeclarationKind kind);

#endif
