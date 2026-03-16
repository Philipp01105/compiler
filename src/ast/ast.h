#ifndef DMM_AST_H
#define DMM_AST_H

#include <stddef.h>
#include "language_types.h"

#define AST_TOKEN_NONE ((size_t)-1)
#define AST_SYMBOL_NONE ((size_t)-1)

typedef struct { int line; int column; } AstSourceLocation;
typedef struct { AstSourceLocation begin; AstSourceLocation end; } AstSourceSpan;

typedef enum {
    AST_DECL_IMPORT, AST_DECL_STRUCT, AST_DECL_ENUM, AST_DECL_FUNCTION, AST_DECL_INVALID
} AstDeclarationKind;
typedef enum { AST_TYPE_INFERRED, AST_TYPE_NAMED } AstTypeKind;
typedef enum {
    AST_EXPR_ERROR, AST_EXPR_LITERAL, AST_EXPR_NAME, AST_EXPR_UNARY,
    AST_EXPR_BINARY, AST_EXPR_CALL, AST_EXPR_INDEX, AST_EXPR_MEMBER, AST_EXPR_RESERVE
} AstExpressionKind;
typedef enum {
    AST_STMT_ERROR, AST_STMT_BLOCK, AST_STMT_VARIABLE, AST_STMT_EXPRESSION,
    AST_STMT_ASSIGNMENT, AST_STMT_IF, AST_STMT_WHILE, AST_STMT_FOR,
    AST_STMT_RETURN, AST_STMT_BREAK, AST_STMT_CONTINUE, AST_STMT_PRINT
} AstStatementKind;

/* Lossless leaves remain temporarily available to the compatibility emitter. */
typedef struct {
    TokenType type;
    char lexeme[MAX_TOKEN];
    AstSourceSpan span;
} AstToken;

typedef struct AstType {
    AstTypeKind kind;
    AstSourceSpan span;
    size_t name_token;
    unsigned pointer_depth;
    int is_array;
    size_t array_length_token;
} AstType;

typedef struct AstExpression AstExpression;
typedef struct AstStatement AstStatement;
typedef struct AstParameter AstParameter;
typedef struct AstField AstField;
typedef struct AstEnumValue AstEnumValue;
typedef struct AstDeclarationNode AstDeclarationNode;

struct AstExpression {
    AstExpressionKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
    size_t value_token;
    TokenType operator_type;
    AstExpression *left;
    AstExpression *right;
    AstExpression *arguments;
    AstExpression *next;
    DataType resolved_type;
    unsigned resolved_pointer_depth;
    size_t resolved_named_type_token;
    int resolved_is_array;
    size_t resolved_symbol_id;
};

struct AstStatement {
    AstStatementKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
    size_t name_token;
    TokenType assignment_operator;
    AstType type;
    AstExpression *expression;
    AstExpression *value;
    AstExpression *condition;
    AstExpression *update;
    AstStatement *body;
    AstStatement *else_body;
    AstStatement *initializer;
    AstStatement *next;
    int is_gc;
    int print_newline;
};

struct AstParameter {
    AstSourceSpan span;
    size_t name_token;
    AstType type;
    int is_array;
    AstParameter *next;
};

struct AstField {
    AstSourceSpan span;
    size_t name_token;
    AstType type;
    AstField *next;
};

struct AstEnumValue {
    AstSourceSpan span;
    size_t name_token;
    AstExpression *arguments;
    AstEnumValue *next;
};

struct AstDeclarationNode {
    AstDeclarationKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
    size_t name_token;
    AstDeclarationNode *next;
    union {
        struct { size_t path_token; size_t path_first_token; size_t path_token_count; } import_decl;
        struct {
            AstParameter *parameters;
            AstType return_type;
            AstStatement *body;
            int is_static;
            size_t owner_token;
        } function;
        struct { AstField *fields; AstDeclarationNode *methods; } struct_decl;
        struct { AstField *fields; AstEnumValue *values; } enum_decl;
    } as;
};

/* Top-level token index retained until the old emitter is retired. */
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
    AstDeclarationNode *root;
    size_t structured_declaration_count;
    int structured_ast_complete;
    size_t structured_error_token;
    void *arena;
} AstProgram;

void ast_program_free(AstProgram *program);
void *ast_program_alloc(AstProgram *program, size_t size);
const AstToken *ast_program_token(const AstProgram *program, size_t index);
const char *ast_program_lexeme(const AstProgram *program, size_t index);
const char *ast_declaration_kind_name(AstDeclarationKind kind);

#endif
