#include "semantic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct LocalSymbol {
    size_t name_token;
    AstType type;
    size_t symbol_id;
    DataType resolved_type;
    unsigned resolved_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    struct LocalSymbol *next;
} LocalSymbol;

typedef struct {
    SemanticModel *model;
    AstProgram *program;
    LocalSymbol *locals;
    size_t current_function_token;
    size_t current_function_symbol_id;
    size_t current_owner_token;
    size_t scope_depth;
    int allocation_failed;
} Analyzer;

static size_t named_type_token(const AstProgram *program, const AstType *type);

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
        if (symbol->kind == kind &&
            same_name(symbol->source_program, symbol->name_token, name))
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

static void resolve_declared_type(const AstProgram *program, const AstType *type,
                                  SemanticSymbol *symbol) {
    symbol->resolved_type = primitive_type(program, type);
    symbol->resolved_pointer_depth = type == NULL ? 0 : type->pointer_depth;
    symbol->resolved_named_type_token = named_type_token(program, type);
    symbol->resolved_named_symbol_id = AST_SYMBOL_NONE;
    symbol->resolved_is_array = type != NULL && type->is_array;
}

static size_t resolve_named_symbol_id(const Analyzer *analyzer,
                                      const AstProgram *program, size_t token) {
    if (token == AST_TOKEN_NONE || token >= program->token_count) return AST_SYMBOL_NONE;
    const char *name = ast_program_lexeme(program, token);
    const SemanticSymbol *symbol = semantic_find_global(analyzer->model, name,
                                                         SEMANTIC_SYMBOL_STRUCT);
    if (symbol == NULL)
        symbol = semantic_find_global(analyzer->model, name, SEMANTIC_SYMBOL_ENUM);
    return symbol == NULL ? AST_SYMBOL_NONE : symbol->id;
}

static void add_global(Analyzer *analyzer, AstDeclarationNode *declaration,
                       SemanticSymbolKind kind, size_t owner_token) {
    const char *name = ast_program_lexeme(analyzer->program, declaration->name_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        int same_owner = existing->owner_token == AST_TOKEN_NONE && owner_token == AST_TOKEN_NONE;
        if (existing->owner_token != AST_TOKEN_NONE && owner_token != AST_TOKEN_NONE)
            same_owner = same_name(existing->source_program, existing->owner_token,
                                   ast_program_lexeme(analyzer->program, owner_token));
        if (existing->kind == kind && same_owner &&
            same_name(existing->source_program, existing->name_token, name)) {
            analyzer->model->duplicate_symbol_count++;
            return;
        }
    }
    AstType type = {0};
    if (kind == SEMANTIC_SYMBOL_FUNCTION) type = declaration->as.function.return_type;
    SemanticSymbol symbol = {
        .kind = kind,
        .source_program = analyzer->program,
        .name_token = declaration->name_token,
        .owner_token = owner_token,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .declared_type = type,
        .declaration = declaration,
        .node = declaration,
        .scope_depth = 0
    };
    if (owner_token != AST_TOKEN_NONE) {
        const char *owner_name = ast_program_lexeme(analyzer->program, owner_token);
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            const SemanticSymbol *owner = &analyzer->model->symbols[i];
            if (owner->kind == SEMANTIC_SYMBOL_STRUCT &&
                owner->source_program == analyzer->program &&
                same_name(owner->source_program, owner->name_token, owner_name)) {
                symbol.owner_symbol_id = owner->id;
                break;
            }
        }
    }
    resolve_declared_type(analyzer->program, &type, &symbol);
    if (append_symbol(analyzer, symbol))
        declaration->resolved_symbol_id = analyzer->model->symbol_count - 1;
}

static void add_member(Analyzer *analyzer, size_t name_token, size_t owner_token,
                       AstType type, SemanticSymbolKind kind, const void *node,
                       size_t *resolved_symbol_id) {
    SemanticSymbol symbol = {
        .kind = kind,
        .source_program = analyzer->program,
        .name_token = name_token,
        .owner_token = owner_token,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .declared_type = type,
        .node = node,
        .scope_depth = 0
    };
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *owner = &analyzer->model->symbols[i];
        if ((owner->kind == SEMANTIC_SYMBOL_STRUCT || owner->kind == SEMANTIC_SYMBOL_ENUM) &&
            owner->source_program == analyzer->program && owner->name_token == owner_token) {
            symbol.owner_symbol_id = owner->id;
            break;
        }
    }
    resolve_declared_type(analyzer->program, &type, &symbol);
    if (append_symbol(analyzer, symbol))
        *resolved_symbol_id = analyzer->model->symbol_count - 1;
}

static void collect_declarations(Analyzer *analyzer, AstProgram *program) {
    AstProgram *saved_program = analyzer->program;
    analyzer->program = program;
    for (AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->kind == AST_DECL_IMPORT) {
            AstType no_type = {0};
            size_t name = declaration->as.import_decl.path_token;
            if (name == AST_TOKEN_NONE) name = declaration->as.import_decl.path_first_token;
            add_member(analyzer, name, AST_TOKEN_NONE, no_type, SEMANTIC_SYMBOL_IMPORT,
                       declaration, &declaration->resolved_symbol_id);
        } else if (declaration->kind == AST_DECL_FUNCTION) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
        } else if (declaration->kind == AST_DECL_STRUCT) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_STRUCT, AST_TOKEN_NONE);
            for (AstField *field = declaration->as.struct_decl.fields;
                 field != NULL; field = field->next)
                add_member(analyzer, field->name_token, declaration->name_token, field->type,
                           SEMANTIC_SYMBOL_FIELD, field, &field->resolved_symbol_id);
            for (AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                add_global(analyzer, method, SEMANTIC_SYMBOL_FUNCTION, declaration->name_token);
        } else if (declaration->kind == AST_DECL_ENUM) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_ENUM, AST_TOKEN_NONE);
            for (AstField *field = declaration->as.enum_decl.fields;
                 field != NULL; field = field->next)
                add_member(analyzer, field->name_token, declaration->name_token, field->type,
                           SEMANTIC_SYMBOL_FIELD, field, &field->resolved_symbol_id);
            AstType enum_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = declaration->name_token,
                .array_length_token = AST_TOKEN_NONE
            };
            for (AstEnumValue *value = declaration->as.enum_decl.values;
                 value != NULL; value = value->next)
                add_member(analyzer, value->name_token, declaration->name_token, enum_type,
                           SEMANTIC_SYMBOL_ENUM_VALUE, value, &value->resolved_symbol_id);
        }
    }
    analyzer->program = saved_program;
}

static LocalSymbol *push_local(Analyzer *analyzer, size_t name_token, AstType type,
                               SemanticSymbolKind kind, const AstExpression *inferred) {
    LocalSymbol *local = malloc(sizeof(*local));
    if (local == NULL) {
        analyzer->allocation_failed = 1;
        return NULL;
    }
    local->name_token = name_token;
    local->type = type;
    SemanticSymbol symbol = {
        .kind = kind,
        .source_program = analyzer->program,
        .name_token = name_token,
        .owner_token = analyzer->current_function_token,
        .owner_symbol_id = analyzer->current_function_symbol_id,
        .declared_type = type,
        .node = NULL,
        .scope_depth = analyzer->scope_depth
    };
    resolve_declared_type(analyzer->program, &type, &symbol);
    if (type.kind == AST_TYPE_INFERRED && inferred != NULL) {
        symbol.resolved_type = inferred->resolved_type;
        symbol.resolved_pointer_depth = inferred->resolved_pointer_depth;
        symbol.resolved_named_type_token = inferred->resolved_named_type_token;
        symbol.resolved_named_symbol_id = inferred->resolved_named_symbol_id;
        symbol.resolved_is_array = inferred->resolved_is_array;
    }
    if (symbol.resolved_named_type_token != AST_TOKEN_NONE &&
        symbol.resolved_named_symbol_id == AST_SYMBOL_NONE)
        symbol.resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
            analyzer->program, symbol.resolved_named_type_token);
    if (!append_symbol(analyzer, symbol)) {
        free(local);
        return NULL;
    }
    local->symbol_id = analyzer->model->symbol_count - 1;
    local->resolved_type = symbol.resolved_type;
    local->resolved_pointer_depth = symbol.resolved_pointer_depth;
    local->resolved_named_type_token = symbol.resolved_named_type_token;
    local->resolved_named_symbol_id = symbol.resolved_named_symbol_id;
    local->resolved_is_array = symbol.resolved_is_array;
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
                                                         size_t name_token,
                                                         const AstProgram **source_program) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    const SemanticSymbol *symbol = semantic_find_global(analyzer->model, name,
                                                         SEMANTIC_SYMBOL_STRUCT);
    if (source_program != NULL)
        *source_program = symbol == NULL ? NULL : symbol->source_program;
    return symbol == NULL ? NULL : symbol->declaration;
}

static const AstDeclarationNode *find_enum_declaration(const Analyzer *analyzer,
                                                       size_t name_token,
                                                       const AstProgram **source_program) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    const SemanticSymbol *symbol = semantic_find_global(analyzer->model, name,
                                                         SEMANTIC_SYMBOL_ENUM);
    if (source_program != NULL)
        *source_program = symbol == NULL ? NULL : symbol->source_program;
    return symbol == NULL ? NULL : symbol->declaration;
}

static const AstField *find_field(const Analyzer *analyzer, size_t type_token,
                                  size_t field_token) {
    const AstProgram *source = NULL;
    const AstDeclarationNode *structure = find_struct_declaration(analyzer, type_token, &source);
    const char *name = ast_program_lexeme(analyzer->program, field_token);
    const AstField *fields = structure == NULL ? NULL : structure->as.struct_decl.fields;
    if (fields == NULL) {
        const AstDeclarationNode *enumeration = find_enum_declaration(analyzer, type_token,
                                                                       &source);
        fields = enumeration == NULL ? NULL : enumeration->as.enum_decl.fields;
    }
    for (const AstField *field = fields;
         field != NULL; field = field->next)
        if (same_name(source, field->name_token, name)) return field;
    return NULL;
}

static const AstField *find_field_by_symbol(const Analyzer *analyzer, size_t type_symbol_id,
                                            size_t field_token) {
    if (type_symbol_id >= analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *type = &analyzer->model->symbols[type_symbol_id];
    const AstDeclarationNode *declaration = type->declaration;
    if (declaration == NULL) return NULL;
    const AstField *fields = type->kind == SEMANTIC_SYMBOL_STRUCT
        ? declaration->as.struct_decl.fields
        : (type->kind == SEMANTIC_SYMBOL_ENUM ? declaration->as.enum_decl.fields : NULL);
    const char *name = ast_program_lexeme(analyzer->program, field_token);
    for (const AstField *field = fields; field != NULL; field = field->next)
        if (same_name(type->source_program, field->name_token, name)) return field;
    return NULL;
}

static const AstEnumValue *find_enum_value_by_symbol(const Analyzer *analyzer,
                                                     size_t type_symbol_id,
                                                     size_t value_token) {
    if (type_symbol_id >= analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *type = &analyzer->model->symbols[type_symbol_id];
    if (type->kind != SEMANTIC_SYMBOL_ENUM || type->declaration == NULL) return NULL;
    const char *name = ast_program_lexeme(analyzer->program, value_token);
    for (const AstEnumValue *value = type->declaration->as.enum_decl.values;
         value != NULL; value = value->next)
        if (same_name(type->source_program, value->name_token, name)) return value;
    return NULL;
}

static const SemanticSymbol *find_method(const Analyzer *analyzer, size_t owner_symbol_id,
                                         size_t method_token) {
    const char *name = ast_program_lexeme(analyzer->program, method_token);
    if (owner_symbol_id >= analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *owner = &analyzer->model->symbols[owner_symbol_id];
    const char *owner_name = ast_program_lexeme(owner->source_program, owner->name_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->owner_token != AST_TOKEN_NONE &&
            same_name(symbol->source_program, symbol->owner_token,
                      owner_name) &&
            same_name(symbol->source_program, symbol->name_token, name)) return symbol;
    }
    return NULL;
}

static DataType promoted_numeric(DataType left, DataType right) {
    if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (left == TYPE_FLOAT || right == TYPE_FLOAT) return TYPE_FLOAT;
    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN) return TYPE_UNKNOWN;
    return TYPE_INT;
}

static DataType builtin_result_type(const char *name) {
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strcmp") == 0 ||
        strcmp(name, "scanfInt") == 0 || strcmp(name, "sys_write") == 0 ||
        strcmp(name, "sys_read") == 0 || strcmp(name, "sys_open") == 0 ||
        strcmp(name, "sys_close") == 0 || strcmp(name, "io_strlen") == 0 ||
        strcmp(name, "io_str_to_int") == 0)
        return TYPE_INT;
    if (strcmp(name, "scanfChar") == 0) return TYPE_CHAR;
    if (strcmp(name, "strcpy") == 0 || strcmp(name, "strcat") == 0 ||
        strcmp(name, "strdup") == 0 || strcmp(name, "scanfString") == 0 ||
        strcmp(name, "io_int_to_str") == 0)
        return TYPE_STRING;
    if (strcmp(name, "free") == 0) return TYPE_VOID;
    return TYPE_UNKNOWN;
}

static int is_builtin_name(const char *name) {
    return builtin_result_type(name) != TYPE_UNKNOWN || strcmp(name, "malloc") == 0 ||
           strcmp(name, "read") == 0;
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
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
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
        TokenType name_type = expression->value_token < analyzer->program->token_count
            ? analyzer->program->tokens[expression->value_token].type : TOKEN_ERROR;
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            expression->resolved_type = TYPE_BIT;
        } else if (name_type >= TOKEN_TYPE_INT && name_type <= TOKEN_TYPE_VOID) {
            AstType type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            expression->resolved_type = primitive_type(analyzer->program, &type);
        } else if (is_builtin_name(name)) {
            expression->resolved_type = builtin_result_type(name);
            if (strcmp(name, "malloc") == 0) expression->resolved_pointer_depth = 1;
        } else {
            const LocalSymbol *local = find_local(analyzer, expression->value_token);
            if (local != NULL) {
                expression->resolved_symbol_id = local->symbol_id;
                expression->resolved_type = local->resolved_type;
                expression->resolved_pointer_depth = local->resolved_pointer_depth;
                expression->resolved_named_type_token = local->resolved_named_type_token;
                expression->resolved_named_symbol_id = local->resolved_named_symbol_id;
                expression->resolved_is_array = local->resolved_is_array;
            } else {
                const AstField *implicit_field = analyzer->current_owner_token == AST_TOKEN_NONE
                    ? NULL : find_field(analyzer, analyzer->current_owner_token,
                                        expression->value_token);
                const SemanticSymbol *structure = semantic_find_global(analyzer->model, name,
                                                                        SEMANTIC_SYMBOL_STRUCT);
                const SemanticSymbol *enumeration = semantic_find_global(analyzer->model, name,
                                                                           SEMANTIC_SYMBOL_ENUM);
                const SemanticSymbol *function = semantic_find_global(analyzer->model, name,
                                                                       SEMANTIC_SYMBOL_FUNCTION);
                if (implicit_field != NULL) {
                    expression->resolved_symbol_id = implicit_field->resolved_symbol_id;
                    expression->resolved_type = primitive_type(analyzer->program,
                                                               &implicit_field->type);
                    expression->resolved_pointer_depth = implicit_field->type.pointer_depth;
                    expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                              &implicit_field->type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        analyzer->program, expression->resolved_named_type_token);
                    expression->resolved_is_array = implicit_field->type.is_array;
                } else if (structure != NULL) {
                    expression->resolved_symbol_id = structure->id;
                    expression->resolved_named_type_token = structure->name_token;
                    expression->resolved_named_symbol_id = structure->id;
                } else if (enumeration != NULL) {
                    expression->resolved_symbol_id = enumeration->id;
                    expression->resolved_named_type_token = enumeration->name_token;
                    expression->resolved_named_symbol_id = enumeration->id;
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
            expression->resolved_named_symbol_id = expression->right->resolved_named_symbol_id;
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
    } else if (expression->kind == AST_EXPR_CAST) {
        AstType cast_type = {
            .kind = AST_TYPE_NAMED,
            .name_token = expression->value_token,
            .array_length_token = AST_TOKEN_NONE
        };
        expression->resolved_type = primitive_type(analyzer->program, &cast_type);
    } else if (expression->kind == AST_EXPR_FREE) {
        expression->resolved_type = TYPE_VOID;
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
                    expression->resolved_type = primitive_type(function->source_program,
                                                               &function->declared_type);
                    expression->resolved_pointer_depth = function->declared_type.pointer_depth;
                    expression->resolved_named_type_token = named_type_token(function->source_program,
                                                                              &function->declared_type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        function->source_program, expression->resolved_named_type_token);
                    expression->resolved_is_array = function->declared_type.is_array;
                } else if (is_builtin_name(name)) {
                    expression->resolved_type = builtin_result_type(name);
                    if (strcmp(name, "malloc") == 0)
                        expression->resolved_pointer_depth = 1;
                    if (strcmp(name, "read") == 0 && expression->arguments != NULL) {
                        expression->resolved_type = expression->arguments->resolved_type;
                        expression->resolved_named_type_token =
                            expression->arguments->resolved_named_type_token;
                        expression->resolved_named_symbol_id =
                            expression->arguments->resolved_named_symbol_id;
                    }
                } else if (expression->left->resolved_named_type_token != AST_TOKEN_NONE) {
                    expression->resolved_named_type_token =
                        expression->left->resolved_named_type_token;
                    expression->resolved_named_symbol_id =
                        expression->left->resolved_named_symbol_id;
                }
            }
        } else if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
                   expression->left->left != NULL) {
            const SemanticSymbol *method = find_method(analyzer,
                expression->left->left->resolved_named_symbol_id,
                expression->left->value_token);
            if (method != NULL) {
                expression->resolved_symbol_id = method->id;
                expression->resolved_type = primitive_type(method->source_program,
                                                           &method->declared_type);
                expression->resolved_pointer_depth = method->declared_type.pointer_depth;
                expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                          &method->declared_type);
                expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                    method->source_program, expression->resolved_named_type_token);
            }
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        expression->resolved_type = expression->left->resolved_type;
        expression->resolved_pointer_depth = expression->left->resolved_pointer_depth;
        expression->resolved_named_type_token = expression->left->resolved_named_type_token;
        expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
        if (expression->resolved_pointer_depth > 0) expression->resolved_pointer_depth--;
        expression->resolved_is_array = 0;
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL) {
        const AstEnumValue *value = NULL;
        if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
                SEMANTIC_SYMBOL_ENUM)
            value = find_enum_value_by_symbol(analyzer,
                expression->left->resolved_named_symbol_id, expression->value_token);
        if (value != NULL) {
            expression->resolved_symbol_id = value->resolved_symbol_id;
            expression->resolved_named_type_token =
                expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
            return;
        }
        const SemanticSymbol *method = find_method(analyzer,
            expression->left->resolved_named_symbol_id, expression->value_token);
        if (method != NULL) {
            expression->resolved_symbol_id = method->id;
            expression->resolved_type = primitive_type(method->source_program,
                                                       &method->declared_type);
            expression->resolved_pointer_depth = method->declared_type.pointer_depth;
            expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                      &method->declared_type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                method->source_program, expression->resolved_named_type_token);
            return;
        }
        const AstField *field = find_field_by_symbol(analyzer,
            expression->left->resolved_named_symbol_id, expression->value_token);
        if (field != NULL) {
            const SemanticSymbol *field_symbol = field->resolved_symbol_id <
                analyzer->model->symbol_count
                    ? &analyzer->model->symbols[field->resolved_symbol_id] : NULL;
            const AstProgram *field_program = field_symbol == NULL
                ? analyzer->program : field_symbol->source_program;
            expression->resolved_type = primitive_type(field_program, &field->type);
            expression->resolved_pointer_depth = field->type.pointer_depth;
            expression->resolved_named_type_token = named_type_token(field_program, &field->type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                field_program, expression->resolved_named_type_token);
            expression->resolved_is_array = field->type.is_array;
            expression->resolved_symbol_id = field->resolved_symbol_id;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        AstExpression *type_argument = expression->arguments;
        if (type_argument != NULL) {
            AstType reserved_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = type_argument->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            expression->resolved_type = primitive_type(analyzer->program, &reserved_type);
            expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                      &reserved_type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                analyzer->program, expression->resolved_named_type_token);
        }
        expression->resolved_pointer_depth = 1;
    }
    if (expression->resolved_type == TYPE_UNKNOWN &&
        expression->resolved_named_type_token == AST_TOKEN_NONE &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE &&
        expression->kind != AST_EXPR_RESERVE &&
        !(expression->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program, expression->value_token))))
        analyzer->model->unresolved_expression_count++;
}

static void analyze_statement(Analyzer *analyzer, AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        LocalSymbol *scope = analyzer->locals;
        if (statement->kind == AST_STMT_VARIABLE) {
            analyze_expression(analyzer, statement->value);
            LocalSymbol *local = push_local(analyzer, statement->name_token, statement->type,
                                            SEMANTIC_SYMBOL_LOCAL, statement->value);
            if (local != NULL) statement->resolved_symbol_id = local->symbol_id;
        } else if (statement->kind == AST_STMT_FOR) {
            analyzer->scope_depth++;
            analyze_statement(analyzer, statement->initializer);
            analyze_expression(analyzer, statement->condition);
            analyze_statement(analyzer, statement->body);
            analyze_statement(analyzer, statement->else_body);
            pop_to(analyzer, scope);
            analyzer->scope_depth--;
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
            statement->kind == AST_STMT_WHILE)
            pop_to(analyzer, scope);
    }
}

static void analyze_function(Analyzer *analyzer, AstDeclarationNode *function) {
    LocalSymbol *saved = analyzer->locals;
    size_t saved_function = analyzer->current_function_token;
    size_t saved_function_symbol = analyzer->current_function_symbol_id;
    size_t saved_owner = analyzer->current_owner_token;
    analyzer->current_function_token = function->name_token;
    analyzer->current_function_symbol_id = function->resolved_symbol_id;
    analyzer->current_owner_token = function->as.function.owner_token;
    analyzer->scope_depth++;
    for (AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        LocalSymbol *local = push_local(analyzer, parameter->name_token, parameter->type,
                                        SEMANTIC_SYMBOL_PARAMETER, NULL);
        if (local != NULL) parameter->resolved_symbol_id = local->symbol_id;
    }
    analyze_statement(analyzer, function->as.function.body);
    pop_to(analyzer, saved);
    analyzer->scope_depth--;
    analyzer->current_function_token = saved_function;
    analyzer->current_function_symbol_id = saved_function_symbol;
    analyzer->current_owner_token = saved_owner;
}

SemanticModel *semantic_analyze(AstProgram *program) {
    if (program == NULL || !program->structured_ast_complete) return NULL;
    SemanticModel *model = calloc(1, sizeof(*model));
    if (model == NULL) return NULL;
    model->program = program;
    Analyzer analyzer = {
        .model = model,
        .program = program,
        .current_function_token = AST_TOKEN_NONE,
        .current_function_symbol_id = AST_SYMBOL_NONE,
        .current_owner_token = AST_TOKEN_NONE
    };
    collect_declarations(&analyzer, program);
    for (size_t i = 0; i < program->owned_import_count; i++)
        collect_declarations(&analyzer, program->owned_imports[i]);
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->kind == AST_DECL_FUNCTION) analyze_function(&analyzer, declaration);
            else if (declaration->kind == AST_DECL_STRUCT) {
                for (AstDeclarationNode *method = declaration->as.struct_decl.methods;
                     method != NULL; method = method->next)
                    analyze_function(&analyzer, method);
            } else if (declaration->kind == AST_DECL_ENUM) {
                for (AstEnumValue *value = declaration->as.enum_decl.values;
                     value != NULL; value = value->next)
                    for (AstExpression *argument = value->arguments;
                         argument != NULL; argument = argument->next)
                        analyze_expression(&analyzer, argument);
            }
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
