#include "ast.h"

#include <stdint.h>
#include <stdlib.h>

typedef union { long double floating; void *pointer; int64_t integer; } AstArenaAlignment;

typedef struct AstArenaBlock {
    struct AstArenaBlock *next;
    size_t used;
    size_t capacity;
    AstArenaAlignment alignment;
    unsigned char data[];
} AstArenaBlock;

void *ast_program_alloc(AstProgram *program, size_t size) {
    if (program == NULL || size == 0) return NULL;
    const size_t alignment = _Alignof(AstArenaAlignment);
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
    free(program->module_identity);
    for (size_t i = 0; i < program->owned_import_count; i++)
        ast_program_free(program->owned_imports[i]);
    free(program->owned_imports);
    for (size_t i = 0; i < program->loaded_source_count; i++) free(program->loaded_source_paths[i]);
    free(program->loaded_source_paths);
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
        case AST_DECL_CONSTANT: return "constant";
        case AST_DECL_VARIABLE: return "variable";
        case AST_DECL_INTERFACE: return "interface";
        case AST_DECL_INVALID: return "invalid";
        case AST_DECL_EXTERN: return "extern";
    }
    return "invalid";
}

AstType ast_type_element(const AstType *type) {
    AstType result = {0};
    result.kind = AST_TYPE_INFERRED;
    result.name_token = AST_TOKEN_NONE;
    result.array_length_token = AST_TOKEN_NONE;
    if (type == NULL) return result;
    if (type->element_type != NULL) return *type->element_type;
    result = *type;
    result.is_array = 0;
    result.is_slice = 0;
    result.array_length_token = AST_TOKEN_NONE;
    result.resolved_array_length = 0;
    result.element_type = NULL;
    return result;
}

int ast_type_contains_polymorphic_callable(const AstType *type) {
    if (type == NULL) return 0;
    if (type->kind == AST_TYPE_FUNCTION && type->function_generic_parameters != NULL) return 1;
    if (ast_type_contains_polymorphic_callable(type->element_type)) return 1;
    for (const AstTypeArgument *argument = type->arguments; argument; argument = argument->next)
        if (ast_type_contains_polymorphic_callable(&argument->type)) return 1;
    for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
        if (ast_type_contains_polymorphic_callable(&parameter->type)) return 1;
    return ast_type_contains_polymorphic_callable(type->function_return_type);
}

static int valid_token(const AstProgram *program, size_t token) {
    return token != AST_TOKEN_NONE && token < program->token_count;
}

static int valid_type_depth(const AstProgram *program, const AstType *type, int allow_inferred, unsigned depth) {
    if (depth > 512) return 0;
    if (type->is_native_function && type->kind != AST_TYPE_FUNCTION) return 0;
    if (type->kind == AST_TYPE_INFERRED) return allow_inferred;
    if (type->kind == AST_TYPE_FUNCTION) {
        if (!valid_token(program, type->name_token) || type->function_return_type == NULL ||
            !valid_type_depth(program, type->function_return_type, 0, depth + 1)) return 0;
        unsigned parameter_count = 0, generic_count = 0;
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
            if (++parameter_count > 16 || !valid_type_depth(program, &parameter->type, 0, depth + 1)) return 0;
        for (const AstGenericParameter *parameter = type->function_generic_parameters; parameter;
             parameter = parameter->next)
            if (++generic_count > 16 || !valid_token(program, parameter->name_token)) return 0;
    } else if ((type->kind != AST_TYPE_NAMED && type->kind != AST_TYPE_FUTURE &&
                type->kind != AST_TYPE_JOIN && type->kind != AST_TYPE_EXECUTOR) ||
               !valid_token(program, type->name_token)) return 0;
    if ((type->kind == AST_TYPE_FUTURE || type->kind == AST_TYPE_JOIN) &&
        (type->arguments == NULL || type->arguments->next != NULL)) return 0;
    if (type->is_array && !valid_token(program, type->array_length_token)) return 0;
    if (type->element_type != NULL &&
        !valid_type_depth(program, type->element_type, 0, depth + 1))
        return 0;
    unsigned count = 0;
    for (const AstTypeArgument *argument = type->arguments; argument; argument = argument->next)
        if (++count > 16 || !valid_type_depth(program, &argument->type, 0, depth + 1)) return 0;
    return 1;
}

static int valid_type(const AstProgram *program, const AstType *type, int allow_inferred) {
    return valid_type_depth(program, type, allow_inferred, 0);
}

static int valid_statement(const AstProgram *program, const AstStatement *statement);

static int valid_expression(const AstProgram *program, const AstExpression *expression) {
    if (expression == NULL || expression->first_token >= program->token_count ||
        expression->token_count == 0 ||
        expression->token_count > program->token_count - expression->first_token)
        return 0;
    switch (expression->kind) {
        case AST_EXPR_CONTROL:
            if (expression->control == NULL ||
                !valid_statement(program, expression->control)) return 0;
            break;
        case AST_EXPR_LITERAL:
        case AST_EXPR_NAME:
            if (!valid_token(program, expression->value_token)) return 0;
            break;
        case AST_EXPR_UNARY:
        case AST_EXPR_AWAIT:
            if (!valid_expression(program, expression->right)) return 0;
            break;
        case AST_EXPR_PROPAGATE:
            if (!valid_expression(program, expression->left) ||
                !valid_token(program, expression->value_token)) return 0;
            break;
        case AST_EXPR_BINARY:
        case AST_EXPR_SLICE:
        case AST_EXPR_INDEX:
            if (!valid_expression(program, expression->left) ||
                !valid_expression(program, expression->right))
                return 0;
            break;
        case AST_EXPR_SUBSLICE:
            if (!valid_expression(program, expression->left) ||
                (expression->right != NULL &&
                 !valid_expression(program, expression->right)) ||
                (expression->arguments != NULL &&
                 (!valid_expression(program, expression->arguments) ||
                  expression->arguments->next != NULL)))
                return 0;
            break;
        case AST_EXPR_ENUM_CONSTRUCT:
        case AST_EXPR_ENUM_ACCESS:
        case AST_EXPR_CALL:
            if (!valid_expression(program, expression->left)) return 0;
            break;
        case AST_EXPR_ARRAY_LITERAL:
            if (expression->arguments == NULL) return 0;
            for (const AstExpression *element = expression->arguments;
                 element != NULL; element = element->next)
                if (!valid_expression(program, element)) return 0;
            if (expression->right != NULL &&
                !valid_expression(program, expression->right))
                return 0;
            break;
        case AST_EXPR_MEMBER:
        case AST_EXPR_SLICE_LENGTH:
        case AST_EXPR_SLICE_DATA:
        case AST_EXPR_TYPE_PROPERTY:
            if (!valid_expression(program, expression->left) ||
                !valid_token(program, expression->value_token))
                return 0;
            break;
        case AST_EXPR_RESERVE:
        case AST_EXPR_SIZEOF:
        case AST_EXPR_ALIGNOF:
        case AST_EXPR_TYPE_INFO:
            if (!valid_token(program, expression->value_token) ||
                !valid_type(program, &expression->allocated_type, 0))
                return 0;
            break;
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
            statement->token_count > program->token_count - statement->first_token)
            return 0;
        switch (statement->kind) {
            case AST_STMT_BLOCK:
                if ((statement->body != NULL && !valid_statement(program, statement->body)) ||
                    (statement->result != NULL && !valid_expression(program, statement->result))) return 0;
                break;
            case AST_STMT_VARIABLE:
                if (!valid_token(program, statement->name_token) ||
                    !valid_type(program, &statement->type, 1) ||
                    (statement->value != NULL &&
                     !valid_expression(program, statement->value)))
                    return 0;
                break;
            case AST_STMT_EXPRESSION:
                if (!valid_expression(program, statement->expression)) return 0;
                break;
            case AST_STMT_ASSIGNMENT:
                if (!valid_expression(program, statement->expression) ||
                    ((statement->assignment_operator != TOKEN_PLUS_PLUS &&
                      statement->assignment_operator != TOKEN_MINUS_MINUS) &&
                     !valid_expression(program, statement->value)))
                    return 0;
                break;
            case AST_STMT_IF:
                if (!valid_expression(program, statement->condition) || statement->body == NULL ||
                    !valid_statement(program, statement->body) ||
                    (statement->else_body != NULL &&
                     !valid_statement(program, statement->else_body)))
                    return 0;
                break;
            case AST_STMT_WHILE:
                if (!valid_expression(program, statement->condition) || statement->body == NULL ||
                    !valid_statement(program, statement->body))
                    return 0;
                break;
            case AST_STMT_FOR:
                if (statement->body == NULL || !valid_statement(program, statement->body) ||
                    (statement->initializer != NULL &&
                     !valid_statement(program, statement->initializer)) ||
                    (statement->condition != NULL &&
                     !valid_expression(program, statement->condition)) ||
                    (statement->else_body != NULL &&
                     !valid_statement(program, statement->else_body)))
                    return 0;
                break;
            case AST_STMT_RETURN:
                if (statement->value != NULL && !valid_expression(program, statement->value)) return 0;
                break;
            case AST_STMT_DEFER:
                if ((statement->expression == NULL) == (statement->body == NULL) ||
                    (statement->expression != NULL &&
                     !valid_expression(program, statement->expression)) ||
                    (statement->body != NULL &&
                     !valid_statement(program, statement->body)))
                    return 0;
                break;
            case AST_STMT_MATCH:
                if (!valid_expression(program, statement->value) || !statement->match_arms) return 0;
                for (const AstMatchArm *a = statement->match_arms; a; a = a->next) {
                    if (!valid_token(program, a->variant_token) || !valid_statement(program, a->body)) return 0;
                    if (a->is_type_pattern && !valid_type(program, &a->type, 0)) return 0;
                    for (const AstParameter *p = a->bindings; p; p = p->next)
                        if (!valid_token(program, p->name_token)) return 0;
                }
                break;
            case AST_STMT_BREAK:
            case AST_STMT_CONTINUE:
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
            !valid_type(program, &field->type, 0))
            return 0;
    return 1;
}

static int valid_function_declaration(const AstProgram *program,
                                      const AstDeclarationNode *declaration) {
    if (!valid_token(program, declaration->name_token) ||
        !valid_type(program, &declaration->as.function.return_type, 0) ||
        (declaration->is_native
             ? declaration->as.function.body != NULL || declaration->as.function.is_async ||
               declaration->generic_parameters != NULL ||
               !valid_token(program, declaration->native_abi_token) ||
               !valid_token(program, declaration->native_library_token) ||
               !valid_token(program, declaration->native_name_token)
             : declaration->as.function.body == NULL ||
               !valid_statement(program, declaration->as.function.body)))
        return 0;
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        if (!valid_token(program, parameter->name_token) ||
            !valid_type(program, &parameter->type, 0))
            return 0;
    return 1;
}

static int valid_declarations(const AstProgram *program) {
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->first_token >= program->token_count || declaration->token_count == 0 ||
            declaration->token_count > program->token_count - declaration->first_token)
            return 0;
        if (declaration->is_native &&
            ((declaration->kind != AST_DECL_STRUCT && declaration->kind != AST_DECL_FUNCTION) ||
             !valid_token(program, declaration->native_abi_token))) return 0;
        if (declaration->is_native_export &&
            (declaration->kind != AST_DECL_FUNCTION || declaration->is_native ||
             !valid_token(program, declaration->native_abi_token))) return 0;
        if ((declaration->is_native_union || declaration->native_pack || declaration->native_alignment) &&
            (declaration->kind != AST_DECL_STRUCT || !declaration->is_native)) return 0;
        if (declaration->kind == AST_DECL_IMPORT) {
            if (declaration->as.import_decl.paths == NULL) return 0;
            for (const AstImportPath *path = declaration->as.import_decl.paths; path != NULL; path = path->next) {
                if (path->path_token != AST_TOKEN_NONE && !valid_token(program, path->path_token)) return 0;
                if (path->path_token_count == 0 || path->path_first_token >= program->token_count ||
                    path->path_token_count > program->token_count - path->path_first_token)
                    return 0;
            }
        } else if (declaration->kind == AST_DECL_FUNCTION) {
            if (!valid_function_declaration(program, declaration)) return 0;
        } else if (declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE) {
            if (!valid_token(program, declaration->name_token) ||
                !valid_type(program, &declaration->as.constant.type, 1) ||
                ((declaration->kind == AST_DECL_CONSTANT || declaration->as.constant.value) &&
                 !valid_expression(program, declaration->as.constant.value)))
                return 0;
        } else if (declaration->kind == AST_DECL_STRUCT) {
            if (declaration->is_native &&
                (declaration->generic_parameters || declaration->as.struct_decl.methods ||
                 declaration->as.struct_decl.destructor ||
                 declaration->native_library_token != AST_TOKEN_NONE ||
                 (declaration->is_opaque && declaration->as.struct_decl.fields))) return 0;
            if (!valid_token(program, declaration->name_token) ||
                !valid_fields(program, declaration->as.struct_decl.fields) ||
                (declaration->as.struct_decl.destructor != NULL &&
                 !valid_statement(program, declaration->as.struct_decl.destructor)))
                return 0;
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                if (method->kind != AST_DECL_FUNCTION ||
                    !valid_function_declaration(program, method))
                    return 0;
        } else if (declaration->kind == AST_DECL_ENUM) {
            if (!valid_token(program, declaration->name_token) ||
                !valid_fields(program, declaration->as.enum_decl.fields))
                return 0;
            for (const AstEnumValue *value = declaration->as.enum_decl.values;
                 value != NULL; value = value->next) {
                if (!valid_token(program, value->name_token)) return 0;
                for (const AstExpression *argument = value->arguments;
                     argument != NULL; argument = argument->next)
                    if (!valid_expression(program, argument)) return 0;
            }
            for (const AstDeclarationNode *method = declaration->as.enum_decl.methods;
                 method != NULL; method = method->next)
                if (method->kind != AST_DECL_FUNCTION ||
                    !valid_function_declaration(program, method))
                    return 0;
        } else if (declaration->kind == AST_DECL_INTERFACE) {
            if (!valid_token(program, declaration->name_token)) return 0;
            const AstDeclarationNode *methods = declaration->as.interface_decl.methods;
            for (; methods; methods = methods->next)
                if (!valid_function_declaration(program, methods)) return 0;
        } else {
            return 0;
        }
    }
    return 1;
}

int ast_validate_program(const AstProgram *program) {
    if (program == NULL || !program->structured_ast_complete || program->tokens == NULL ||
        program->token_count == 0 || !valid_declarations(program))
        return 0;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (!ast_validate_program(program->owned_imports[i])) return 0;
    return 1;
}
