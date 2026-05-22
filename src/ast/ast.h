#ifndef DMM_AST_H
#define DMM_AST_H

#include <stddef.h>
#include <stdio.h>
#include "language_types.h"
#include "string_interner.h"

#define AST_TOKEN_NONE ((size_t)-1)
#define AST_SYMBOL_NONE ((size_t)-1)

typedef struct { int line; int column; } AstSourceLocation;
typedef struct { AstSourceLocation begin; AstSourceLocation end; } AstSourceSpan;

typedef enum {
    AST_DECL_IMPORT, AST_DECL_STRUCT, AST_DECL_ENUM, AST_DECL_FUNCTION,
    AST_DECL_CONSTANT, AST_DECL_TRAIT, AST_DECL_IMPL, AST_DECL_INVALID
} AstDeclarationKind;
typedef enum { AST_TYPE_INFERRED, AST_TYPE_NAMED } AstTypeKind;
typedef enum {
    AST_EXPR_ERROR, AST_EXPR_LITERAL, AST_EXPR_NAME, AST_EXPR_UNARY,
    AST_EXPR_BINARY, AST_EXPR_CALL, AST_EXPR_INDEX, AST_EXPR_MEMBER,
    AST_EXPR_SLICE_LENGTH, AST_EXPR_RESERVE, AST_EXPR_CAST, AST_EXPR_FREE, AST_EXPR_ENUM_CONSTRUCT
} AstExpressionKind;
typedef enum {
    AST_STMT_ERROR, AST_STMT_BLOCK, AST_STMT_VARIABLE, AST_STMT_EXPRESSION,
    AST_STMT_ASSIGNMENT, AST_STMT_IF, AST_STMT_WHILE, AST_STMT_FOR,
    AST_STMT_RETURN, AST_STMT_BREAK, AST_STMT_CONTINUE, AST_STMT_MATCH
} AstStatementKind;

/* Lossless token leaves reference source spellings owned by the module interner. */
typedef struct {
    TokenType type;
    const char *lexeme;
    AstSourceSpan span;
} AstToken;

typedef struct AstTypeArgument AstTypeArgument;
typedef struct AstGenericParameter AstGenericParameter;

typedef struct AstType {
    AstTypeKind kind;
    AstSourceSpan span;
    size_t name_token;
    unsigned pointer_depth;
    unsigned outer_pointer_depth;
    int is_array;
    int is_slice;
    size_t array_length_token;
    size_t resolved_array_length;
    AstTypeArgument *arguments;
} AstType;

struct AstTypeArgument { AstType type; AstTypeArgument *next; };
typedef struct AstTraitBound { size_t name_token; struct AstTraitBound *next; } AstTraitBound;
struct AstGenericParameter { size_t name_token; AstTraitBound *bounds; AstGenericParameter *next; };

typedef struct AstExpression AstExpression;
typedef struct AstStatement AstStatement;
typedef struct AstParameter AstParameter;
typedef struct AstField AstField;
typedef struct AstEnumValue AstEnumValue;
typedef struct AstDeclarationNode AstDeclarationNode;
typedef struct AstProgram AstProgram;
typedef struct AstMatchArm AstMatchArm;

typedef struct AstImportPath {
    AstSourceSpan span;
    size_t path_token;
    size_t path_first_token;
    size_t path_token_count;
    size_t resolved_symbol_id;
    AstProgram *resolved_program;
    struct AstImportPath *next;
} AstImportPath;

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
    AstType allocated_type;
    /* Sema-owned folded constant spelling, separate from the source tree. */
    AstToken folded_constant;
    DataType resolved_type;
    unsigned resolved_pointer_depth;
    unsigned resolved_outer_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    int resolved_is_slice;
    size_t resolved_array_length;
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
    int is_const;
    AstExpression *expression;
    AstExpression *value;
    AstExpression *condition;
    AstExpression *update;
    AstStatement *body;
    AstStatement *else_body;
    AstStatement *initializer;
    AstStatement *next;
    size_t resolved_symbol_id;
    AstMatchArm *match_arms;
    int match_exhaustive;
};

struct AstMatchArm {
    size_t variant_token;
    size_t resolved_variant_symbol;
    AstParameter *bindings;
    AstStatement *body;
    AstSourceSpan span;
    int wildcard;
    AstMatchArm *next;
};

struct AstParameter {
    AstSourceSpan span;
    size_t name_token;
    AstType type;
    size_t resolved_symbol_id;
    AstParameter *next;
};

struct AstField {
    AstSourceSpan span;
    size_t name_token;
    AstType type;
    size_t resolved_symbol_id;
    AstField *next;
};

struct AstEnumValue {
    AstSourceSpan span;
    size_t name_token;
    AstExpression *arguments;
    AstTypeArgument *payload_types;
    size_t resolved_symbol_id;
    AstEnumValue *next;
};

struct AstDeclarationNode {
    AstDeclarationKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
    size_t name_token;
    size_t resolved_symbol_id;
    AstDeclarationNode *next;
    AstGenericParameter *generic_parameters;
    const AstDeclarationNode *generic_origin;
    AstTypeArgument *specialization_arguments;
    const char *specialization_identity;
    int semantic_body_checked;
    union {
        struct {
            AstImportPath *paths;
        } import_decl;
        struct {
            AstParameter *parameters;
            AstType return_type;
            AstStatement *body;
            int is_static;
            size_t owner_token;
        } function;
        struct { AstField *fields; AstDeclarationNode *methods; } struct_decl;
        struct { AstField *fields; AstEnumValue *values; int is_sum; } enum_decl;
        struct { AstType type; AstExpression *value; } constant;
        struct { AstDeclarationNode *methods; } trait_decl;
        struct { size_t trait_token; AstType for_type; AstDeclarationNode *methods; int attached; } impl_decl;
    } as;
};

/* Compact top-level index used by tooling alongside the structured tree. */
typedef struct {
    AstDeclarationKind kind;
    AstSourceSpan span;
    size_t first_token;
    size_t token_count;
} AstDeclaration;

struct AstProgram {
    char *source_path;
    char *module_identity;
    AstToken *tokens;
    size_t token_count;
    AstDeclaration *declarations;
    size_t declaration_count;
    AstDeclarationNode *root;
    size_t structured_declaration_count;
    int structured_ast_complete;
    size_t structured_error_token;
    void *arena;
    StringInterner *strings;
    int owns_strings;
    AstProgram **owned_imports;
    size_t owned_import_count;
    size_t owned_import_capacity;
    /* Files opened by import resolution, including units rejected by the lexer. */
    char **loaded_source_paths;
    size_t loaded_source_count;
    size_t loaded_source_capacity;
};

void ast_program_free(AstProgram *program);
void *ast_program_alloc(AstProgram *program, size_t size);
const AstToken *ast_program_token(const AstProgram *program, size_t index);
const char *ast_program_lexeme(const AstProgram *program, size_t index);
const char *ast_declaration_kind_name(AstDeclarationKind kind);
int ast_validate_program(const AstProgram *program);
int ast_dump(FILE *output, const AstProgram *program);

#endif
