#include "ast.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct AstArenaBlock {
    struct AstArenaBlock *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
} AstArenaBlock;

void *ast_program_alloc(AstProgram *program, size_t size) {
    if (program == NULL || size == 0) return NULL;
    const size_t alignment = _Alignof(max_align_t);
    if (size > SIZE_MAX - (alignment - 1)) return NULL;
    size = (size + alignment - 1) & ~(alignment - 1);
    AstArenaBlock *block = program->arena;
    if (block == NULL || size > block->capacity - block->used) {
        size_t capacity = 64U * 1024U;
        if (capacity < size) capacity = size;
        if (capacity > SIZE_MAX - sizeof(*block)) return NULL;
        AstArenaBlock *created = calloc(1, sizeof(*created) + capacity);
        if (created == NULL) return NULL;
        created->capacity = capacity;
        created->next = block;
        program->arena = created;
        block = created;
    }
    void *result = block->data + block->used;
    block->used += size;
    return result;
}

void ast_program_free(AstProgram *program) {
    if (program == NULL) return;
    AstArenaBlock *block = program->arena;
    while (block != NULL) {
        AstArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    free(program->source_path);
    for (size_t i = 0; i < program->owned_import_count; i++)
        ast_program_free(program->owned_imports[i]);
    free(program->owned_imports);
    free(program->tokens);
    free(program->declarations);
    if (program->owns_strings) string_interner_free(program->strings);
    free(program);
}

const AstToken *ast_program_token(const AstProgram *program, size_t index) {
    if (program == NULL || index >= program->token_count) return NULL;
    return &program->tokens[index];
}

const char *ast_program_lexeme(const AstProgram *program, size_t index) {
    const AstToken *token = ast_program_token(program, index);
    return token == NULL ? "" : token->lexeme;
}

const char *ast_declaration_kind_name(AstDeclarationKind kind) {
    switch (kind) {
        case AST_DECL_IMPORT: return "import";
        case AST_DECL_STRUCT: return "struct";
        case AST_DECL_ENUM: return "enum";
        case AST_DECL_FUNCTION: return "function";
        case AST_DECL_INVALID: return "invalid";
    }
    return "invalid";
}

static int valid_token(const AstProgram *program, size_t token) {
    return token != AST_TOKEN_NONE && token < program->token_count;
}

static int valid_type(const AstProgram *program, const AstType *type, int allow_inferred) {
    if (type->kind == AST_TYPE_INFERRED) return allow_inferred;
    if (type->kind != AST_TYPE_NAMED || !valid_token(program, type->name_token)) return 0;
    if (type->is_array && !valid_token(program, type->array_length_token)) return 0;
    return 1;
}

static int valid_expression(const AstProgram *program, const AstExpression *expression) {
    if (expression == NULL || expression->first_token >= program->token_count ||
        expression->token_count == 0 ||
        expression->token_count > program->token_count - expression->first_token)
        return 0;
    switch (expression->kind) {
        case AST_EXPR_LITERAL:
        case AST_EXPR_NAME:
            if (!valid_token(program, expression->value_token)) return 0;
            break;
        case AST_EXPR_UNARY:
            if (!valid_expression(program, expression->right)) return 0;
            break;
        case AST_EXPR_BINARY:
        case AST_EXPR_INDEX:
            if (!valid_expression(program, expression->left) ||
                !valid_expression(program, expression->right)) return 0;
            break;
        case AST_EXPR_CALL:
            if (!valid_expression(program, expression->left)) return 0;
            break;
        case AST_EXPR_MEMBER:
            if (!valid_expression(program, expression->left) ||
                !valid_token(program, expression->value_token)) return 0;
            break;
        case AST_EXPR_RESERVE:
        case AST_EXPR_CAST:
        case AST_EXPR_FREE:
            if (!valid_token(program, expression->value_token) || expression->arguments == NULL)
                return 0;
            break;
        case AST_EXPR_ERROR:
            return 0;
    }
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next)
        if (!valid_expression(program, argument)) return 0;
    return 1;
}

static int valid_statement(const AstProgram *program, const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->first_token >= program->token_count || statement->token_count == 0 ||
            statement->token_count > program->token_count - statement->first_token) return 0;
        switch (statement->kind) {
            case AST_STMT_BLOCK:
                if (statement->body != NULL && !valid_statement(program, statement->body)) return 0;
                break;
            case AST_STMT_VARIABLE:
                if (!valid_token(program, statement->name_token) ||
                    !valid_type(program, &statement->type, 1) ||
                    (statement->value != NULL &&
                     !valid_expression(program, statement->value))) return 0;
                break;
            case AST_STMT_EXPRESSION:
                if (!valid_expression(program, statement->expression)) return 0;
                break;
            case AST_STMT_ASSIGNMENT:
                if (!valid_expression(program, statement->expression) ||
                    ((statement->assignment_operator != TOKEN_PLUS_PLUS &&
                      statement->assignment_operator != TOKEN_MINUS_MINUS) &&
                     !valid_expression(program, statement->value))) return 0;
                break;
            case AST_STMT_IF:
                if (!valid_expression(program, statement->condition) || statement->body == NULL ||
                    !valid_statement(program, statement->body) ||
                    (statement->else_body != NULL &&
                     !valid_statement(program, statement->else_body))) return 0;
                break;
            case AST_STMT_WHILE:
                if (!valid_expression(program, statement->condition) || statement->body == NULL ||
                    !valid_statement(program, statement->body)) return 0;
                break;
            case AST_STMT_FOR:
                if (statement->body == NULL || !valid_statement(program, statement->body) ||
                    (statement->initializer != NULL &&
                     !valid_statement(program, statement->initializer)) ||
                    (statement->condition != NULL &&
                     !valid_expression(program, statement->condition)) ||
                    (statement->else_body != NULL &&
                     !valid_statement(program, statement->else_body))) return 0;
                break;
            case AST_STMT_RETURN:
                if (statement->value != NULL && !valid_expression(program, statement->value)) return 0;
                break;
            case AST_STMT_BREAK:
            case AST_STMT_CONTINUE:
                break;
            case AST_STMT_PRINT:
                if (statement->value != NULL && !valid_expression(program, statement->value)) return 0;
                break;
            case AST_STMT_ERROR:
                return 0;
        }
    }
    return 1;
}

static int valid_fields(const AstProgram *program, const AstField *field) {
    for (; field != NULL; field = field->next)
        if (!valid_token(program, field->name_token) ||
            !valid_type(program, &field->type, 0)) return 0;
    return 1;
}

static int valid_function_declaration(const AstProgram *program,
                                      const AstDeclarationNode *declaration) {
    if (!valid_token(program, declaration->name_token) ||
        !valid_type(program, &declaration->as.function.return_type, 0) ||
        declaration->as.function.body == NULL ||
        !valid_statement(program, declaration->as.function.body)) return 0;
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        if (!valid_token(program, parameter->name_token) ||
            !valid_type(program, &parameter->type, 0)) return 0;
    return 1;
}

static int valid_declarations(const AstProgram *program) {
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->first_token >= program->token_count || declaration->token_count == 0 ||
            declaration->token_count > program->token_count - declaration->first_token) return 0;
        if (declaration->kind == AST_DECL_IMPORT) {
            if (declaration->as.import_decl.path_token == AST_TOKEN_NONE &&
                declaration->as.import_decl.path_token_count == 0) return 0;
        } else if (declaration->kind == AST_DECL_FUNCTION) {
            if (!valid_function_declaration(program, declaration)) return 0;
        } else if (declaration->kind == AST_DECL_STRUCT) {
            if (!valid_token(program, declaration->name_token) ||
                !valid_fields(program, declaration->as.struct_decl.fields)) return 0;
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                if (method->kind != AST_DECL_FUNCTION ||
                    !valid_function_declaration(program, method)) return 0;
        } else if (declaration->kind == AST_DECL_ENUM) {
            if (!valid_token(program, declaration->name_token) ||
                !valid_fields(program, declaration->as.enum_decl.fields)) return 0;
            for (const AstEnumValue *value = declaration->as.enum_decl.values;
                 value != NULL; value = value->next) {
                if (!valid_token(program, value->name_token)) return 0;
                for (const AstExpression *argument = value->arguments;
                     argument != NULL; argument = argument->next)
                    if (!valid_expression(program, argument)) return 0;
            }
        } else {
            return 0;
        }
    }
    return 1;
}

int ast_validate_program(const AstProgram *program) {
    if (program == NULL || !program->structured_ast_complete || program->tokens == NULL ||
        program->token_count == 0 || !valid_declarations(program)) return 0;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (!ast_validate_program(program->owned_imports[i])) return 0;
    return 1;
}
