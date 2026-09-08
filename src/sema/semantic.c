#include "semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct LocalSymbol {
    size_t name_token;
    AstType type;
    size_t symbol_id;
    struct LocalSymbol *next;
} LocalSymbol;

typedef struct {
    SemanticModel *model;
    AstProgram *program;
    LocalSymbol *locals;
    size_t current_function_token;
    size_t scope_depth;
    int allocation_failed;
} Analyzer;

static DataType primitive_type(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->kind != AST_TYPE_NAMED || type->name_token >= program->token_count)
        return TYPE_UNKNOWN;
    switch (program->tokens[type->name_token].type) {
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
        case TOKEN_TYPE_FLOAT: return TYPE_FLOAT;
        case TOKEN_TYPE_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_TYPE_STRING: return TYPE_STRING;
        case TOKEN_TYPE_VOID: return TYPE_VOID;
        default: return TYPE_UNKNOWN;
    }
}

static int same_name(const AstProgram *program, size_t token, const char *name) {
    return token < program->token_count && strcmp(program->tokens[token].lexeme, name) == 0;
}

const SemanticSymbol *semantic_find_global(const SemanticModel *model,
                                           const char *name,
                                           SemanticSymbolKind kind) {
    if (model == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind == kind && same_name(model->program, symbol->name_token, name))
            return symbol;
    }
    return NULL;
}

static int append_symbol(Analyzer *analyzer, SemanticSymbol symbol) {
    SemanticModel *model = analyzer->model;
    if (model->symbol_count == model->symbol_capacity) {
        size_t capacity = model->symbol_capacity == 0 ? 32 : model->symbol_capacity * 2;
        if (capacity < model->symbol_capacity || capacity > SIZE_MAX / sizeof(*model->symbols)) {
            analyzer->allocation_failed = 1;
            return 0;
        }
        SemanticSymbol *symbols = realloc(model->symbols, capacity * sizeof(*symbols));
        if (symbols == NULL) {
            analyzer->allocation_failed = 1;
            return 0;
        }
        model->symbols = symbols;
        model->symbol_capacity = capacity;
    }
    symbol.id = model->symbol_count;
    model->symbols[model->symbol_count++] = symbol;
    return 1;
}

static void add_global(Analyzer *analyzer, const AstDeclarationNode *declaration,
                       SemanticSymbolKind kind, size_t owner_token) {
    const char *name = ast_program_lexeme(analyzer->program, declaration->name_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        if (existing->kind == kind && existing->owner_token == owner_token &&
            same_name(analyzer->program, existing->name_token, name)) {
            analyzer->model->duplicate_symbol_count++;
            return;
        }
    }
    AstType type = {0};
    if (kind == SEMANTIC_SYMBOL_FUNCTION) type = declaration->as.function.return_type;
    SemanticSymbol symbol = {
        .kind = kind,
        .name_token = declaration->name_token,
        .owner_token = owner_token,
        .declared_type = type,
        .declaration = declaration,
        .scope_depth = 0
    };
    (void) append_symbol(analyzer, symbol);
}

static void collect_declarations(Analyzer *analyzer) {
    for (const AstDeclarationNode *declaration = analyzer->program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->kind == AST_DECL_FUNCTION) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
        } else if (declaration->kind == AST_DECL_STRUCT) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_STRUCT, AST_TOKEN_NONE);
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                add_global(analyzer, method, SEMANTIC_SYMBOL_FUNCTION, declaration->name_token);
        } else if (declaration->kind == AST_DECL_ENUM) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_ENUM, AST_TOKEN_NONE);
        }
    }
}

static LocalSymbol *push_local(Analyzer *analyzer, size_t name_token, AstType type,
                               SemanticSymbolKind kind) {
    LocalSymbol *local = malloc(sizeof(*local));
    if (local == NULL) {
        analyzer->allocation_failed = 1;
        return NULL;
    }
    local->name_token = name_token;
    local->type = type;
    SemanticSymbol symbol = {
        .kind = kind,
        .name_token = name_token,
        .owner_token = analyzer->current_function_token,
        .declared_type = type,
        .scope_depth = analyzer->scope_depth
    };
    if (!append_symbol(analyzer, symbol)) {
        free(local);
        return NULL;
    }
    local->symbol_id = analyzer->model->symbol_count - 1;
    local->next = analyzer->locals;
    analyzer->locals = local;
    return local;
}

static void pop_to(Analyzer *analyzer, LocalSymbol *saved) {
    while (analyzer->locals != saved) {
        LocalSymbol *next = analyzer->locals->next;
        free(analyzer->locals);
        analyzer->locals = next;
    }
}

static const LocalSymbol *find_local(const Analyzer *analyzer, size_t name_token) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    for (const LocalSymbol *local = analyzer->locals; local != NULL; local = local->next) {
        if (same_name(analyzer->program, local->name_token, name)) return local;
    }
    return NULL;
}

static size_t named_type_token(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->kind != AST_TYPE_NAMED || type->name_token >= program->token_count)
        return AST_TOKEN_NONE;
    return program->tokens[type->name_token].type == TOKEN_IDENTIFIER
        ? type->name_token : AST_TOKEN_NONE;
}

static const AstDeclarationNode *find_struct_declaration(const Analyzer *analyzer,
                                                         size_t name_token) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    const SemanticSymbol *symbol = semantic_find_global(analyzer->model, name,
                                                         SEMANTIC_SYMBOL_STRUCT);
    return symbol == NULL ? NULL : symbol->declaration;
}

static const AstField *find_field(const Analyzer *analyzer, size_t type_token,
                                  size_t field_token) {
    const AstDeclarationNode *structure = find_struct_declaration(analyzer, type_token);
    const char *name = ast_program_lexeme(analyzer->program, field_token);
    if (structure == NULL) return NULL;
    for (const AstField *field = structure->as.struct_decl.fields;
         field != NULL; field = field->next)
        if (same_name(analyzer->program, field->name_token, name)) return field;
    return NULL;
}

static const SemanticSymbol *find_method(const Analyzer *analyzer, size_t owner_token,
                                         size_t method_token) {
    const char *name = ast_program_lexeme(analyzer->program, method_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->owner_token != AST_TOKEN_NONE &&
            same_name(analyzer->program, symbol->owner_token,
                      ast_program_lexeme(analyzer->program, owner_token)) &&
            same_name(analyzer->program, symbol->name_token, name)) return symbol;
    }
    return NULL;
}

static DataType promoted_numeric(DataType left, DataType right) {
    if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (left == TYPE_FLOAT || right == TYPE_FLOAT) return TYPE_FLOAT;
    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN) return TYPE_UNKNOWN;
    return TYPE_INT;
}

static void analyze_expression(Analyzer *analyzer, AstExpression *expression) {
    if (expression == NULL) return;
    analyze_expression(analyzer, expression->left);
    analyze_expression(analyzer, expression->right);
    for (AstExpression *argument = expression->arguments; argument != NULL; argument = argument->next)
        analyze_expression(analyzer, argument);

    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_pointer_depth = 0;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_is_array = 0;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    if (expression->kind == AST_EXPR_LITERAL) {
        TokenType token = expression->value_token < analyzer->program->token_count
            ? analyzer->program->tokens[expression->value_token].type : TOKEN_ERROR;
        if (token == TOKEN_NUMBER) expression->resolved_type = TYPE_INT;
        else if (token == TOKEN_FLOAT_LITERAL) expression->resolved_type = TYPE_DOUBLE;
        else if (token == TOKEN_CHAR_LITERAL) expression->resolved_type = TYPE_CHAR;
        else if (token == TOKEN_STRING_LITERAL) expression->resolved_type = TYPE_STRING;
    } else if (expression->kind == AST_EXPR_NAME) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            expression->resolved_type = TYPE_BIT;
        } else {
            const LocalSymbol *local = find_local(analyzer, expression->value_token);
            if (local != NULL) {
                const AstType *type = &local->type;
                expression->resolved_symbol_id = local->symbol_id;
                expression->resolved_type = primitive_type(analyzer->program, type);
                expression->resolved_pointer_depth = type->pointer_depth;
                expression->resolved_named_type_token = named_type_token(analyzer->program, type);
                expression->resolved_is_array = type->is_array;
            } else {
                const SemanticSymbol *structure = semantic_find_global(analyzer->model, name,
                                                                        SEMANTIC_SYMBOL_STRUCT);
                const SemanticSymbol *function = semantic_find_global(analyzer->model, name,
                                                                       SEMANTIC_SYMBOL_FUNCTION);
                if (structure != NULL) {
                    expression->resolved_symbol_id = structure->id;
                    expression->resolved_named_type_token = structure->name_token;
                } else if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    expression->resolved_type = primitive_type(analyzer->program,
                                                               &function->declared_type);
                }
            }
        }
    } else if (expression->kind == AST_EXPR_UNARY) {
        if (expression->operator_type == TOKEN_BANG) expression->resolved_type = TYPE_BIT;
        else if (expression->right != NULL) {
            expression->resolved_type = expression->right->resolved_type;
            expression->resolved_pointer_depth = expression->right->resolved_pointer_depth;
            expression->resolved_named_type_token = expression->right->resolved_named_type_token;
            expression->resolved_is_array = expression->right->resolved_is_array;
            if (expression->operator_type == TOKEN_AMPERSAND) expression->resolved_pointer_depth++;
            else if (expression->operator_type == TOKEN_STAR && expression->resolved_pointer_depth > 0)
                expression->resolved_pointer_depth--;
        }
    } else if (expression->kind == AST_EXPR_BINARY) {
        TokenType operation = expression->operator_type;
        if ((operation >= TOKEN_EQUAL_EQUAL && operation <= TOKEN_GREATER_EQUAL) ||
            operation == TOKEN_AMP_AMP || operation == TOKEN_PIPE_PIPE) {
            expression->resolved_type = TYPE_BIT;
        } else if (expression->left != NULL && expression->right != NULL) {
            if (operation == TOKEN_PLUS && (expression->left->resolved_type == TYPE_STRING ||
                                            expression->right->resolved_type == TYPE_STRING))
                expression->resolved_type = TYPE_STRING;
            else expression->resolved_type = promoted_numeric(expression->left->resolved_type,
                                                                expression->right->resolved_type);
        }
    } else if (expression->kind == AST_EXPR_CALL) {
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program, expression->left->value_token);
            TokenType callee_type = expression->left->value_token < analyzer->program->token_count
                ? analyzer->program->tokens[expression->left->value_token].type : TOKEN_ERROR;
            AstType cast_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->left->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            if (callee_type >= TOKEN_TYPE_INT && callee_type <= TOKEN_TYPE_VOID) {
                expression->resolved_type = primitive_type(analyzer->program, &cast_type);
            } else {
                const SemanticSymbol *function = semantic_find_global(analyzer->model, name,
                                                                       SEMANTIC_SYMBOL_FUNCTION);
                if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    expression->resolved_type = primitive_type(analyzer->program, &function->declared_type);
                    expression->resolved_pointer_depth = function->declared_type.pointer_depth;
                    expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                              &function->declared_type);
                    expression->resolved_is_array = function->declared_type.is_array;
                }
            }
        } else if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
                   expression->left->left != NULL) {
            const SemanticSymbol *method = find_method(analyzer,
                expression->left->left->resolved_named_type_token,
                expression->left->value_token);
            if (method != NULL) {
                expression->resolved_type = primitive_type(analyzer->program, &method->declared_type);
                expression->resolved_pointer_depth = method->declared_type.pointer_depth;
                expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                          &method->declared_type);
            }
        } else if (expression->left != NULL && expression->left->kind == AST_EXPR_RESERVE &&
                   expression->arguments != NULL) {
            AstExpression *type_argument = expression->arguments;
            AstType reserved_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = type_argument->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            expression->resolved_type = primitive_type(analyzer->program, &reserved_type);
            expression->resolved_named_type_token = named_type_token(analyzer->program, &reserved_type);
            expression->resolved_pointer_depth = 1;
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        expression->resolved_type = expression->left->resolved_type;
        expression->resolved_pointer_depth = expression->left->resolved_pointer_depth;
        expression->resolved_named_type_token = expression->left->resolved_named_type_token;
        if (expression->resolved_pointer_depth > 0) expression->resolved_pointer_depth--;
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL) {
        const AstField *field = find_field(analyzer, expression->left->resolved_named_type_token,
                                           expression->value_token);
        if (field != NULL) {
            expression->resolved_type = primitive_type(analyzer->program, &field->type);
            expression->resolved_pointer_depth = field->type.pointer_depth;
            expression->resolved_named_type_token = named_type_token(analyzer->program, &field->type);
            expression->resolved_is_array = field->type.is_array;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        expression->resolved_pointer_depth = 1;
    }
    if (expression->resolved_type == TYPE_UNKNOWN && expression->kind != AST_EXPR_MEMBER &&
        expression->kind != AST_EXPR_RESERVE)
        analyzer->model->unresolved_expression_count++;
}

static void analyze_statement(Analyzer *analyzer, AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        LocalSymbol *scope = analyzer->locals;
        if (statement->kind == AST_STMT_VARIABLE) {
            analyze_expression(analyzer, statement->value);
            (void) push_local(analyzer, statement->name_token, statement->type,
                              SEMANTIC_SYMBOL_LOCAL);
        } else {
            analyze_expression(analyzer, statement->expression);
            analyze_expression(analyzer, statement->value);
            analyze_expression(analyzer, statement->condition);
            analyze_expression(analyzer, statement->update);
            analyze_statement(analyzer, statement->initializer);
            analyze_statement(analyzer, statement->body);
            analyze_statement(analyzer, statement->else_body);
        }
        if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
            statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR)
            pop_to(analyzer, scope);
    }
}

static void analyze_function(Analyzer *analyzer, AstDeclarationNode *function) {
    LocalSymbol *saved = analyzer->locals;
    size_t saved_function = analyzer->current_function_token;
    analyzer->current_function_token = function->name_token;
    analyzer->scope_depth++;
    for (AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        (void) push_local(analyzer, parameter->name_token, parameter->type,
                          SEMANTIC_SYMBOL_PARAMETER);
    analyze_statement(analyzer, function->as.function.body);
    pop_to(analyzer, saved);
    analyzer->scope_depth--;
    analyzer->current_function_token = saved_function;
}

SemanticModel *semantic_analyze(AstProgram *program) {
    if (program == NULL || !program->structured_ast_complete) return NULL;
    SemanticModel *model = calloc(1, sizeof(*model));
    if (model == NULL) return NULL;
    model->program = program;
    Analyzer analyzer = {
        .model = model,
        .program = program,
        .current_function_token = AST_TOKEN_NONE
    };
    collect_declarations(&analyzer);
    for (AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->kind == AST_DECL_FUNCTION) analyze_function(&analyzer, declaration);
        else if (declaration->kind == AST_DECL_STRUCT) {
            for (AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                analyze_function(&analyzer, method);
        }
    }
    pop_to(&analyzer, NULL);
    if (analyzer.allocation_failed) {
        semantic_model_free(model);
        return NULL;
    }
    return model;
}

void semantic_model_free(SemanticModel *model) {
    if (model == NULL) return;
    free(model->symbols);
    free(model);
}
