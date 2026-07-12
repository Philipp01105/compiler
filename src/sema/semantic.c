#include "semantic.h"
#include "semantic_internal.h"
#include "generics.h"
#include "core_intrinsics.h"

#include "errorHandler.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>



DataType primitive_type(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->kind != AST_TYPE_NAMED || type->name_token >= program->token_count)
        return TYPE_UNKNOWN;
    switch (program->tokens[type->name_token].type) {
        case TOKEN_TYPE_I8:
        case TOKEN_TYPE_U8:
        case TOKEN_TYPE_I16:
        case TOKEN_TYPE_U16:
        case TOKEN_TYPE_I32:
        case TOKEN_TYPE_U32:
        case TOKEN_TYPE_I64:
        case TOKEN_TYPE_U64:
        case TOKEN_TYPE_ISIZE:
        case TOKEN_TYPE_USIZE:
            return token_data_type(program->tokens[type->name_token].type);
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
        case TOKEN_TYPE_FLOAT: return TYPE_FLOAT;
        case TOKEN_TYPE_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_TYPE_STRING: return TYPE_STRING;
        case TOKEN_TYPE_VOID: return TYPE_VOID;
        case TOKEN_TYPE_NEVER: return TYPE_NEVER;
        default: return TYPE_UNKNOWN;
    }
}

static int reserved_link_name(const char *name) {
    return strncmp(name, "__dmm_", 6) == 0;
}

static int append_symbol(Analyzer *analyzer, SemanticSymbol symbol) {
    if (semantic_append_symbol(analyzer->model, symbol)) return 1;
    analyzer->allocation_failed = 1;
    return 0;
}

static void resolve_declared_type(const AstProgram *program, const AstType *type,
                                  SemanticSymbol *symbol) {
    symbol->resolved_type = primitive_type(program, type);
    symbol->resolved_borrow_kind = type == NULL ? AST_BORROW_NONE : type->borrow_kind;
    symbol->resolved_pointer_depth =
        type == NULL ? 0 : type->pointer_depth + (type->borrow_kind != AST_BORROW_NONE);
    symbol->resolved_outer_pointer_depth = type == NULL ? 0 : type->outer_pointer_depth;
    symbol->resolved_named_type_token = named_type_token(program, type);
    symbol->resolved_named_symbol_id = AST_SYMBOL_NONE;
    symbol->resolved_is_array = type != NULL && type->is_array;
    symbol->resolved_is_slice = type != NULL && type->is_slice;
}

size_t resolve_named_symbol_id(const Analyzer *analyzer,
                                      const AstProgram *program, size_t token) {
    if (token == AST_TOKEN_NONE || token >= program->token_count) return AST_SYMBOL_NONE;
    const char *name = ast_program_lexeme(program, token);
    const SemanticSymbol *symbol = scoped_find_global(analyzer->model, program, name,
                                                      SEMANTIC_SYMBOL_STRUCT);
    if (symbol == NULL)
        symbol = scoped_find_global(analyzer->model, program, name, SEMANTIC_SYMBOL_ENUM);
    if (symbol == NULL)
        symbol = scoped_find_global(analyzer->model, program, name, SEMANTIC_SYMBOL_INTERFACE);
    return symbol == NULL ? AST_SYMBOL_NONE : symbol->id;
}

void add_global(Analyzer *analyzer, AstDeclarationNode *declaration,
                       SemanticSymbolKind kind, size_t owner_token) {
    const char *name = ast_program_lexeme(analyzer->program, declaration->name_token);
    if (owner_token == AST_TOKEN_NONE && kind == SEMANTIC_SYMBOL_FUNCTION &&
        reserved_link_name(name))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                       "Function name is reserved by the runtime");
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        if (!same_package(analyzer->program, existing->source_program)) continue;
        int same_owner = existing->owner_token == AST_TOKEN_NONE && owner_token == AST_TOKEN_NONE;
        if (existing->owner_token != AST_TOKEN_NONE && owner_token != AST_TOKEN_NONE)
            same_owner = same_name(existing->source_program, existing->owner_token,
                                   ast_program_lexeme(analyzer->program, owner_token));
        if (existing->kind != SEMANTIC_SYMBOL_IMPORT && same_owner &&
            same_name(existing->source_program, existing->name_token, name)) {
            if (kind == SEMANTIC_SYMBOL_FUNCTION &&
                existing->kind == SEMANTIC_SYMBOL_FUNCTION)
                continue;
            analyzer->model->duplicate_symbol_count++;
            semantic_duplicate(analyzer, declaration->name_token, existing->source_program, existing->name_token,
                               kind == SEMANTIC_SYMBOL_FUNCTION ? "Duplicate function" : "Duplicate type declaration");
            return;
        }
    }
    AstType type = {0};
    if (kind == SEMANTIC_SYMBOL_FUNCTION) type = declaration->as.function.return_type;
    else if (kind == SEMANTIC_SYMBOL_CONSTANT || kind == SEMANTIC_SYMBOL_VARIABLE) type = declaration->as.constant.type;
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
        symbol.owner_symbol_id = resolve_named_symbol_id(analyzer, analyzer->program, owner_token);
        const char *owner_name = ast_program_lexeme(analyzer->program, owner_token);
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            const SemanticSymbol *owner = &analyzer->model->symbols[i];
            if ((owner->kind == SEMANTIC_SYMBOL_STRUCT || owner->kind == SEMANTIC_SYMBOL_INTERFACE) &&
                same_package(analyzer->program, owner->source_program) &&
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

void add_member(Analyzer *analyzer, size_t name_token, size_t owner_token,
                       AstType type, SemanticSymbolKind kind, const void *node,
                       size_t *resolved_symbol_id) {
    const char *member_name = ast_program_lexeme(analyzer->program, name_token);
    const char *owner_name = ast_program_lexeme(analyzer->program, owner_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        if (!same_package(analyzer->program, existing->source_program)) continue;
        if (existing->owner_token != AST_TOKEN_NONE &&
            same_name(existing->source_program, existing->owner_token, owner_name) &&
            same_name(existing->source_program, existing->name_token, member_name)) {
            semantic_duplicate(analyzer, name_token, existing->source_program, existing->name_token,
                               "Member is already defined");
            return;
        }
    }
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
        if ((owner->kind == SEMANTIC_SYMBOL_STRUCT ||
             owner->kind == SEMANTIC_SYMBOL_ENUM ||
             owner->kind == SEMANTIC_SYMBOL_INTERFACE) &&
            owner->source_program == analyzer->program && owner->name_token == owner_token) {
            symbol.owner_symbol_id = owner->id;
            break;
        }
    }
    resolve_declared_type(analyzer->program, &type, &symbol);
    if (append_symbol(analyzer, symbol))
        *resolved_symbol_id = analyzer->model->symbol_count - 1;
}

void collect_declarations(Analyzer *analyzer, AstProgram *program) {
    AstProgram *saved_program = analyzer->program;
    analyzer->program = program;
    for (AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->generic_parameters != NULL &&
            declaration->kind != AST_DECL_INTERFACE)
            continue;
        if (declaration->kind == AST_DECL_IMPORT) {
            continue; /* Imports are file-local package bindings, not value symbols. */
        }
        if (declaration->kind == AST_DECL_FUNCTION && declaration->generic_parameters == NULL) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
        } else if (declaration->kind == AST_DECL_CONSTANT) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_CONSTANT, AST_TOKEN_NONE);
        } else if (declaration->kind == AST_DECL_VARIABLE) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_VARIABLE, AST_TOKEN_NONE);
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
            for (AstDeclarationNode *method = declaration->as.enum_decl.methods;
                 method != NULL; method = method->next)
                add_global(analyzer, method, SEMANTIC_SYMBOL_FUNCTION, declaration->name_token);
        } else if (declaration->kind == AST_DECL_INTERFACE) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_INTERFACE, AST_TOKEN_NONE);
            for (AstDeclarationNode *method = declaration->as.interface_decl.methods;
                 method != NULL; method = method->next)
                add_global(analyzer, method, SEMANTIC_SYMBOL_FUNCTION, declaration->name_token);
        }
    }
    analyzer->program = saved_program;
}

LocalSymbol *push_local(Analyzer *analyzer, size_t name_token, AstType type,
                                SemanticSymbolKind kind, const AstExpression *inferred,
                                int is_constant) {
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
        .node = inferred,
        .scope_depth = analyzer->scope_depth
    };
    resolve_declared_type(analyzer->program, &type, &symbol);
    if (type.kind == AST_TYPE_INFERRED && inferred != NULL) {
        symbol.resolved_type = inferred->resolved_type;
        symbol.resolved_borrow_kind = inferred->resolved_borrow_kind;
        symbol.resolved_pointer_depth = inferred->resolved_pointer_depth;
        symbol.resolved_outer_pointer_depth = inferred->resolved_outer_pointer_depth;
        symbol.resolved_named_type_token = inferred->resolved_named_type_token;
        symbol.resolved_named_symbol_id = inferred->resolved_named_symbol_id;
        symbol.resolved_is_array = inferred->resolved_is_array;
        symbol.resolved_is_slice = inferred->resolved_is_slice;
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
    local->resolved_borrow_kind = symbol.resolved_borrow_kind;
    local->resolved_pointer_depth = symbol.resolved_pointer_depth;
    local->resolved_outer_pointer_depth = symbol.resolved_outer_pointer_depth;
    local->resolved_named_type_token = symbol.resolved_named_type_token;
    local->resolved_named_symbol_id = symbol.resolved_named_symbol_id;
    local->resolved_is_array = symbol.resolved_is_array;
    local->resolved_is_slice = symbol.resolved_is_slice;
    local->has_resolved_ast_type = 0;
    local->resolved_type_program = NULL;
    if (type.kind == AST_TYPE_INFERRED && inferred != NULL &&
        inferred->has_resolved_ast_type) {
        local->resolved_ast_type = inferred->resolved_ast_type;
        local->resolved_type_program = inferred->resolved_type_program;
        local->has_resolved_ast_type = 1;
    } else if (type.kind != AST_TYPE_INFERRED) {
        local->resolved_ast_type = type;
        local->resolved_type_program = analyzer->program;
        local->has_resolved_ast_type = 1;
    }
    local->is_constant = is_constant;
    local->moved = 0;
    local->initialized = inferred != NULL || kind == SEMANTIC_SYMBOL_PARAMETER;
    local->scope_depth = analyzer->scope_depth;
    local->next = analyzer->locals;
    analyzer->locals = local;
    return local;
}

LocalSymbol *find_local_by_symbol(Analyzer *analyzer, size_t symbol_id) {
    for (LocalSymbol *local = analyzer->locals; local != NULL; local = local->next)
        if (local->symbol_id == symbol_id) return local;
    return NULL;
}

void pop_to(Analyzer *analyzer, LocalSymbol *saved) {
    while (analyzer->locals != saved) {
        LocalSymbol *next = analyzer->locals->next;
        free(analyzer->locals);
        analyzer->locals = next;
    }
}

const LocalSymbol *find_local(const Analyzer *analyzer, size_t name_token) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    for (const LocalSymbol *local = analyzer->locals; local != NULL; local = local->next) {
        if (same_name(analyzer->program, local->name_token, name)) return local;
    }
    return NULL;
}

size_t named_type_token(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->kind != AST_TYPE_NAMED || type->name_token >= program->token_count)
        return AST_TOKEN_NONE;
    return program->tokens[type->name_token].type == TOKEN_IDENTIFIER
               ? type->name_token
               : AST_TOKEN_NONE;
}

static const AstDeclarationNode *find_struct_declaration(const Analyzer *analyzer,
                                                         size_t name_token,
                                                         const AstProgram **source_program) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    const SemanticSymbol *symbol = scoped_find_global(analyzer->model, analyzer->program, name,
                                                      SEMANTIC_SYMBOL_STRUCT);
    if (source_program != NULL)
        *source_program = symbol == NULL ? NULL : symbol->source_program;
    return symbol == NULL ? NULL : symbol->declaration;
}

static const AstDeclarationNode *find_enum_declaration(const Analyzer *analyzer,
                                                       size_t name_token,
                                                       const AstProgram **source_program) {
    const char *name = ast_program_lexeme(analyzer->program, name_token);
    const SemanticSymbol *symbol = scoped_find_global(analyzer->model, analyzer->program, name,
                                                      SEMANTIC_SYMBOL_ENUM);
    if (source_program != NULL)
        *source_program = symbol == NULL ? NULL : symbol->source_program;
    return symbol == NULL ? NULL : symbol->declaration;
}

const AstField *find_field(const Analyzer *analyzer, size_t type_token,
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

const AstField *find_field_by_symbol(const Analyzer *analyzer, size_t type_symbol_id,
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

const AstEnumValue *find_enum_value_by_symbol(const Analyzer *analyzer,
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

const SemanticSymbol *find_method(const Analyzer *analyzer, size_t owner_symbol_id,
                                         size_t method_token) {
    const char *name = ast_program_lexeme(analyzer->program, method_token);
    if (owner_symbol_id >= analyzer->model->symbol_count) return NULL;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->owner_symbol_id == owner_symbol_id &&
            same_name(symbol->source_program, symbol->name_token, name))
            return symbol;
    }
    return NULL;
}

DataType promoted_numeric(DataType left, DataType right) {
    if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (left == TYPE_FLOAT || right == TYPE_FLOAT) return TYPE_FLOAT;
    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN) return TYPE_UNKNOWN;
    return data_type_promoted_integer(left, right);
}

DataType builtin_result_type(const char *name) {
    const CoreIntrinsic *core = core_intrinsic_find(name);
    if (core != NULL) return core_value_type(core->result);
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strcmp") == 0 ||
        strcmp(name, "scanfInt") == 0 || strcmp(name, "sys_write") == 0 ||
        strcmp(name, "sys_read") == 0 || strcmp(name, "sys_open") == 0 ||
        strcmp(name, "sys_close") == 0 || strcmp(name, "io_strlen") == 0 ||
        strcmp(name, "io_str_to_int") == 0 || strcmp(name, "io_int_to_str") == 0)
        return TYPE_INT;
    if (strcmp(name, "scanfChar") == 0) return TYPE_CHAR;
    if (strcmp(name, "strcpy") == 0 || strcmp(name, "strcat") == 0 ||
        strcmp(name, "strdup") == 0 || strcmp(name, "scanfString") == 0)
        return TYPE_STRING;
    if (strcmp(name, "free") == 0) return TYPE_VOID;
    return TYPE_UNKNOWN;
}

int is_builtin_name(const char *name) {
    return builtin_result_type(name) != TYPE_UNKNOWN || strcmp(name, "malloc") == 0 ||
           strcmp(name, "read") == 0;
}

static int same_declared_type(const Analyzer *analyzer,
                              const AstProgram *left_program, const AstType *left,
                              const AstProgram *right_program, const AstType *right) {
    if (left->kind != right->kind || left->pointer_depth != right->pointer_depth ||
        left->outer_pointer_depth != right->outer_pointer_depth ||
        left->is_array != right->is_array || left->is_slice != right->is_slice)
        return 0;
    if (left->kind == AST_TYPE_FUNCTION) {
        const AstTypeArgument *a = left->function_parameters;
        const AstTypeArgument *b = right->function_parameters;
        for (; a && b; a = a->next, b = b->next)
            if (!same_declared_type(analyzer, left_program, &a->type,
                                    right_program, &b->type)) return 0;
        return a == NULL && b == NULL && left->function_return_type &&
               right->function_return_type &&
               same_declared_type(analyzer, left_program, left->function_return_type,
                                  right_program, right->function_return_type);
    }
    DataType left_primitive = primitive_type(left_program, left);
    DataType right_primitive = primitive_type(right_program, right);
    if (left_primitive != TYPE_UNKNOWN || right_primitive != TYPE_UNKNOWN) {
        if (left_primitive != right_primitive) return 0;
    } else {
        size_t left_symbol = resolve_named_symbol_id(analyzer, left_program,
                                                     named_type_token(left_program, left));
        size_t right_symbol = resolve_named_symbol_id(analyzer, right_program,
                                                      named_type_token(right_program, right));
        if (left_symbol == AST_SYMBOL_NONE || left_symbol != right_symbol) return 0;
    }
    if (left->is_array) {
        if (left->resolved_array_length != 0 && right->resolved_array_length != 0)
            return left->resolved_array_length == right->resolved_array_length;
        if (left->array_length_token >= left_program->token_count ||
            right->array_length_token >= right_program->token_count)
            return 0;
        return strcmp(ast_program_lexeme(left_program, left->array_length_token),
                      ast_program_lexeme(right_program, right->array_length_token)) == 0;
    }
    return 1;
}

static int same_function_signature(const Analyzer *analyzer,
                                   const SemanticSymbol *left,
                                   const SemanticSymbol *right) {
    const AstParameter *a = left->declaration->as.function.parameters;
    const AstParameter *b = right->declaration->as.function.parameters;
    for (; a != NULL && b != NULL; a = a->next, b = b->next)
        if (!same_declared_type(analyzer, left->source_program, &a->type,
                                right->source_program, &b->type))
            return 0;
    return a == NULL && b == NULL;
}

void validate_overload_sets(Analyzer *analyzer) {
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *left = &analyzer->model->symbols[i];
        if (left->kind != SEMANTIC_SYMBOL_FUNCTION || left->declaration == NULL) continue;
        const char *name = symbol_name(left);
        for (size_t j = 0; j < i; j++) {
            const SemanticSymbol *right = &analyzer->model->symbols[j];
            if (right->kind != SEMANTIC_SYMBOL_FUNCTION || right->declaration == NULL ||
                !same_package(left->source_program, right->source_program) ||
                left->owner_symbol_id != right->owner_symbol_id ||
                left->declaration->as.function.is_static !=
                right->declaration->as.function.is_static ||
                !same_name(right->source_program, right->name_token, name))
                continue;
            if (strcmp(name, "main") == 0 || same_function_signature(analyzer, left, right)) {
                analyzer->model->duplicate_symbol_count++;
                semantic_duplicate(analyzer, left->name_token, right->source_program, right->name_token,
                                   strcmp(name, "main") == 0
                                       ? "main cannot be overloaded"
                                       : "Duplicate function overload signature");
            }
        }
    }
}

static size_t builtin_arity(const char *name) {
    const CoreIntrinsic *core = core_intrinsic_find(name);
    if (core != NULL) return core->argument_count;
    if (strcmp(name, "scanfInt") == 0 || strcmp(name, "scanfChar") == 0 ||
        strcmp(name, "scanfString") == 0)
        return 0;
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strdup") == 0 ||
        strcmp(name, "malloc") == 0 || strcmp(name, "io_strlen") == 0 ||
        strcmp(name, "io_str_to_int") == 0 || strcmp(name, "sys_close") == 0)
        return 1;
    if (strcmp(name, "strcmp") == 0 || strcmp(name, "strcpy") == 0 ||
        strcmp(name, "strcat") == 0 || strcmp(name, "read") == 0)
        return 2;
    if (strcmp(name, "sys_write") == 0 || strcmp(name, "sys_read") == 0 ||
        strcmp(name, "sys_open") == 0 || strcmp(name, "io_int_to_str") == 0)
        return 3;
    return AST_TOKEN_NONE;
}

static int implicit_conversion_allowed(DataType from, unsigned from_pointers,
                                       DataType to, unsigned to_pointers) {
    if (from == TYPE_NEVER && from_pointers == 0) return 1;
    if (from_pointers != 0 || to_pointers != 0)
        return from_pointers == to_pointers && (from == to || from == TYPE_UNKNOWN ||
                                                to == TYPE_UNKNOWN);
    if (from == to || from == TYPE_UNKNOWN || to == TYPE_UNKNOWN) return 1;
    int from_integral = data_type_integral(from);
    int to_integral = data_type_integral(to);
    if (from_integral && to_integral) return 1;
    if (from_integral &&
        (to == TYPE_FLOAT || to == TYPE_DOUBLE))
        return 1;
    return from == TYPE_FLOAT && to == TYPE_DOUBLE;
}

static int expression_conversion_allowed(const AstExpression *expression,
                                         DataType to, unsigned to_pointers) {
    if (expression == NULL) return 0;
    if (implicit_conversion_allowed(expression->resolved_type,
                                    expression->resolved_pointer_depth +
                                    expression->resolved_outer_pointer_depth,
                                    to, to_pointers))
        return 1;
    int literal = expression->kind == AST_EXPR_LITERAL ||
                  (expression->kind == AST_EXPR_UNARY &&
                   expression->operator_type == TOKEN_MINUS && expression->right != NULL &&
                   expression->right->kind == AST_EXPR_LITERAL);
    return literal && expression->resolved_pointer_depth == 0 &&
           expression->resolved_outer_pointer_depth == 0 &&
           !expression->resolved_is_array && !expression->resolved_is_slice &&
           to_pointers == 0 &&
           ((expression->resolved_type == TYPE_DOUBLE && to == TYPE_FLOAT) ||
            (expression->resolved_type == TYPE_INT &&
             (to == TYPE_CHAR || to == TYPE_BYTE || to == TYPE_BIT)));
}

int expression_to_declared_type_allowed(const Analyzer *analyzer,
                                               const AstExpression *expression,
                                               const AstProgram *type_program,
                                               const AstType *type) {
    if (expression == NULL || type == NULL) return 0;
    if (type->kind == AST_TYPE_FUNCTION && !type->is_array && !type->is_slice &&
        type->pointer_depth == 0 && type->outer_pointer_depth == 0) {
        if (!expression->has_resolved_ast_type ||
            expression->resolved_ast_type.kind != AST_TYPE_FUNCTION ||
            expression->resolved_ast_type.is_array ||
            expression->resolved_ast_type.is_slice ||
            expression->resolved_ast_type.pointer_depth != 0 ||
            expression->resolved_ast_type.outer_pointer_depth != 0) return 0;
        const AstProgram *source_program = expression->resolved_type_program != NULL
                                               ? expression->resolved_type_program
                                               : analyzer->program;
        if (type->function_generic_parameters != NULL ||
            expression->resolved_ast_type.function_generic_parameters != NULL)
            return ast_polymorphic_callable_compatible(
                type_program, type, source_program,
                &expression->resolved_ast_type);
        return ast_concrete_type_equal(type_program, type, source_program,
                                       &expression->resolved_ast_type);
    }
    if (type->element_type != NULL ||
        (expression->has_resolved_ast_type &&
         expression->resolved_ast_type.element_type != NULL)) {
        if (!expression->has_resolved_ast_type ||
            !(type->is_array || type->is_slice) ||
            !(expression->resolved_ast_type.is_array ||
              expression->resolved_ast_type.is_slice))
            return 0;
        if (type->is_array &&
            (!expression->resolved_ast_type.is_array ||
             type->resolved_array_length !=
                 expression->resolved_ast_type.resolved_array_length))
            return 0;
        if (type->is_slice && expression->resolved_ast_type.outer_pointer_depth != 0)
            return 0;
        AstType target_element = ast_type_element(type);
        AstType source_element = ast_type_element(
            &expression->resolved_ast_type);
        const AstProgram *source_program =
            expression->resolved_type_program != NULL
                ? expression->resolved_type_program : analyzer->program;
        return ast_concrete_type_equal(type_program, &target_element,
                                       source_program, &source_element);
    }
    unsigned target_depth = type->pointer_depth + type->outer_pointer_depth +
                            (type->borrow_kind != AST_BORROW_NONE);
    unsigned source_depth = expression->resolved_pointer_depth +
                            expression->resolved_outer_pointer_depth;
    size_t target_name = named_type_token(type_program, type);
    int matching_borrow =
        type->borrow_kind == expression->resolved_borrow_kind ||
        (type->borrow_kind == AST_BORROW_IMMUTABLE &&
         expression->resolved_borrow_kind == AST_BORROW_MUTABLE);
    int matching_shape = target_depth == source_depth && matching_borrow;
    if (type->is_slice && !type->outer_pointer_depth)
        matching_shape = type->outer_pointer_depth == 0 &&
                         expression->resolved_outer_pointer_depth == 0 &&
                         type->pointer_depth == expression->resolved_pointer_depth &&
                         (expression->resolved_is_array || expression->resolved_is_slice);
    else
        matching_shape = matching_shape && type->is_array == expression->resolved_is_array &&
                         type->is_slice == expression->resolved_is_slice;
    if (type->is_array)
        matching_shape = matching_shape &&
                         type->pointer_depth == expression->resolved_pointer_depth &&
                         type->outer_pointer_depth == expression->resolved_outer_pointer_depth &&
                         type->resolved_array_length == expression->resolved_array_length;
    if (target_name != AST_TOKEN_NONE) {
        size_t target_symbol = resolve_named_symbol_id(analyzer, type_program, target_name);
        return target_symbol != AST_SYMBOL_NONE &&
               target_symbol == expression->resolved_named_symbol_id &&
               matching_shape;
    }
    if (expression->resolved_named_symbol_id != AST_SYMBOL_NONE) return 0;
    if (!matching_shape) return 0;
    if (type->is_array || type->is_slice)
        return
                expression->resolved_type == primitive_type(type_program, type);
    return expression_conversion_allowed(expression, primitive_type(type_program, type),
                                         target_depth);
}

int expression_assignment_allowed(const Analyzer *analyzer,
                                         const AstExpression *source,
                                         const AstExpression *target) {
    if (source == NULL || target == NULL) return 0;
    if (source->has_resolved_ast_type && target->has_resolved_ast_type &&
        (source->resolved_ast_type.kind == AST_TYPE_FUNCTION ||
         target->resolved_ast_type.kind == AST_TYPE_FUNCTION) &&
        !source->resolved_ast_type.is_array &&
        !source->resolved_ast_type.is_slice &&
        !target->resolved_ast_type.is_array &&
        !target->resolved_ast_type.is_slice &&
        source->resolved_ast_type.pointer_depth == 0 &&
        source->resolved_ast_type.outer_pointer_depth == 0 &&
        target->resolved_ast_type.pointer_depth == 0 &&
        target->resolved_ast_type.outer_pointer_depth == 0) {
        const AstProgram *source_program = source->resolved_type_program != NULL
                                               ? source->resolved_type_program : analyzer->program;
        const AstProgram *target_program = target->resolved_type_program != NULL
                                               ? target->resolved_type_program : analyzer->program;
        if (source->resolved_ast_type.function_generic_parameters != NULL ||
            target->resolved_ast_type.function_generic_parameters != NULL)
            return ast_polymorphic_callable_compatible(
                target_program, &target->resolved_ast_type,
                source_program, &source->resolved_ast_type);
        return ast_concrete_type_equal(source_program, &source->resolved_ast_type,
                                       target_program, &target->resolved_ast_type);
    }
    if ((source->has_resolved_ast_type &&
         source->resolved_ast_type.element_type != NULL) ||
        (target->has_resolved_ast_type &&
         target->resolved_ast_type.element_type != NULL)) {
        if (!source->has_resolved_ast_type || !target->has_resolved_ast_type)
            return 0;
        const AstProgram *source_program = source->resolved_type_program != NULL
                                               ? source->resolved_type_program
                                               : analyzer->program;
        const AstProgram *target_program = target->resolved_type_program != NULL
                                               ? target->resolved_type_program
                                               : analyzer->program;
        if (target->resolved_ast_type.is_slice &&
            target->resolved_ast_type.outer_pointer_depth == 0 &&
            source->resolved_ast_type.outer_pointer_depth == 0 &&
            (source->resolved_ast_type.is_array ||
             source->resolved_ast_type.is_slice)) {
            AstType source_element = ast_type_element(
                &source->resolved_ast_type);
            AstType target_element = ast_type_element(
                &target->resolved_ast_type);
            return ast_concrete_type_equal(source_program, &source_element,
                                           target_program, &target_element);
        }
        return ast_concrete_type_equal(source_program,
                                       &source->resolved_ast_type,
                                       target_program,
                                       &target->resolved_ast_type);
    }
    if (source->resolved_borrow_kind != target->resolved_borrow_kind &&
        !(target->resolved_borrow_kind == AST_BORROW_IMMUTABLE &&
          source->resolved_borrow_kind == AST_BORROW_MUTABLE))
        return 0;
    unsigned source_depth = source->resolved_pointer_depth +
                            source->resolved_outer_pointer_depth;
    unsigned target_depth = target->resolved_pointer_depth +
                            target->resolved_outer_pointer_depth;
    if (source->resolved_is_array != target->resolved_is_array ||
        source->resolved_is_slice != target->resolved_is_slice)
        return 0;
    if (source->resolved_is_array &&
        (source->resolved_pointer_depth != target->resolved_pointer_depth ||
         source->resolved_outer_pointer_depth != target->resolved_outer_pointer_depth ||
         source->resolved_array_length != target->resolved_array_length))
        return 0;
    if (target->resolved_named_symbol_id != AST_SYMBOL_NONE ||
        source->resolved_named_symbol_id != AST_SYMBOL_NONE)
        if (target->kind == AST_EXPR_INDEX &&
            target->resolved_named_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[target->resolved_named_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE &&
            !source_depth && !target_depth && !target->resolved_is_array &&
            !target->resolved_is_slice && !source->resolved_is_array && !source->resolved_is_slice &&
            semantic_implements_interface(analyzer->model, target->resolved_named_symbol_id,
                                source->resolved_named_symbol_id))
            return !semantic_expression_is_move_only(analyzer, source);
    if (target->resolved_named_symbol_id != AST_SYMBOL_NONE ||
        source->resolved_named_symbol_id != AST_SYMBOL_NONE)
        return target->resolved_named_symbol_id != AST_SYMBOL_NONE &&
               target->resolved_named_symbol_id == source->resolved_named_symbol_id &&
               target_depth == source_depth;
    (void) analyzer;
    return expression_conversion_allowed(source, target->resolved_type, target_depth);
}

int plain_numeric_expression(const AstExpression *expression) {
    return expression != NULL && expression->resolved_pointer_depth == 0 &&
           expression->resolved_outer_pointer_depth == 0 &&
           !expression->resolved_is_array && !expression->resolved_is_slice &&
           (data_type_fixed_integer(expression->resolved_type) || expression->resolved_type == TYPE_INT ||
            expression->resolved_type == TYPE_CHAR ||
            expression->resolved_type == TYPE_BYTE ||
            expression->resolved_type == TYPE_BIT ||
            expression->resolved_type == TYPE_FLOAT ||
            expression->resolved_type == TYPE_DOUBLE);
}

int integral_expression(const AstExpression *expression) {
    return plain_numeric_expression(expression) &&
           expression->resolved_type != TYPE_FLOAT &&
           expression->resolved_type != TYPE_DOUBLE;
}

static int string_expression(const AstExpression *expression) {
    return expression != NULL &&
           ((expression->resolved_type == TYPE_STRING &&
             expression->resolved_pointer_depth == 0 &&
             expression->resolved_outer_pointer_depth == 0 &&
             !expression->resolved_is_array && !expression->resolved_is_slice) ||
            (expression->resolved_type == TYPE_CHAR &&
             (expression->resolved_pointer_depth != 0 ||
              expression->resolved_outer_pointer_depth != 0 ||
              expression->resolved_is_array || expression->resolved_is_slice)));
}

int pointer_expression(const AstExpression *expression) {
    return expression != NULL && (expression->resolved_pointer_depth != 0 ||
                                  expression->resolved_outer_pointer_depth != 0 ||
                                  expression->resolved_is_array ||
                                  expression->resolved_is_slice);
}

static int constant_integer(const Analyzer *analyzer, const AstExpression *expression,
                            long long *value) {
    int negative = 0;
    if (expression != NULL && expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_MINUS) {
        negative = 1;
        expression = expression->right;
    }
    if (expression == NULL || expression->kind != AST_EXPR_LITERAL ||
        expression->value_token >= analyzer->program->token_count ||
        analyzer->program->tokens[expression->value_token].type != TOKEN_NUMBER)
        return 0;
    *value = strtoll(ast_program_lexeme(analyzer->program, expression->value_token), NULL, 10);
    if (negative) *value = -*value;
    return 1;
}

int enum_constant_expression(const Analyzer *analyzer,
                                    const AstExpression *expression) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_LITERAL) return 1;
    if (expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF || expression->kind ==
        AST_EXPR_TYPE_PROPERTY)
        return expression->folded_constant.lexeme != NULL;
    if (expression->kind == AST_EXPR_NAME)
        return same_name(analyzer->program, expression->value_token, "true") ||
               same_name(analyzer->program, expression->value_token, "false");
    return expression->kind == AST_EXPR_UNARY &&
           expression->operator_type == TOKEN_MINUS &&
           expression->right != NULL &&
           expression->right->kind == AST_EXPR_LITERAL &&
           plain_numeric_expression(expression->right);
}

size_t parameter_count(const AstDeclarationNode *function) {
    size_t count = 0;
    if (function != NULL)
        for (const AstParameter *parameter = function->as.function.parameters;
             parameter != NULL; parameter = parameter->next)
            count++;
    return count;
}

static int argument_conversion_rank(const Analyzer *analyzer,
                                    const AstExpression *argument,
                                    const AstProgram *parameter_program,
                                    const AstType *parameter) {
    if (!expression_to_declared_type_allowed(analyzer, argument,
                                             parameter_program, parameter))
        return -1;
    size_t target_named = resolve_named_symbol_id(analyzer, parameter_program,
                                                  named_type_token(parameter_program, parameter));
    int same_base = target_named != AST_SYMBOL_NONE
                        ? target_named == argument->resolved_named_symbol_id
                        : argument->resolved_named_symbol_id == AST_SYMBOL_NONE &&
                          primitive_type(parameter_program, parameter) == argument->resolved_type;
    int same_shape = parameter->pointer_depth == argument->resolved_pointer_depth &&
                     parameter->outer_pointer_depth == argument->resolved_outer_pointer_depth &&
                     parameter->is_array == argument->resolved_is_array &&
                     parameter->is_slice == argument->resolved_is_slice;
    if (same_base && same_shape) return 0;
    if (parameter->is_slice && argument->resolved_is_array && same_base &&
        parameter->pointer_depth == argument->resolved_pointer_depth &&
        parameter->outer_pointer_depth == 0 &&
        argument->resolved_outer_pointer_depth == 0)
        return 1;
    if (argument->resolved_pointer_depth == 0 &&
        argument->resolved_outer_pointer_depth == 0 &&
        !argument->resolved_is_array && !argument->resolved_is_slice &&
        parameter->pointer_depth == 0 && parameter->outer_pointer_depth == 0 &&
        !parameter->is_array && !parameter->is_slice) {
        DataType to = primitive_type(parameter_program, parameter);
        if (((argument->resolved_type == TYPE_BIT || argument->resolved_type == TYPE_BYTE ||
              argument->resolved_type == TYPE_CHAR) && to == TYPE_INT) ||
            (argument->resolved_type == TYPE_FLOAT && to == TYPE_DOUBLE))
            return 1;
    }
    return 2;
}

int contains_type_parameter(const AstProgram *unit, const AstType *type, const AstDeclarationNode *origin) {
    if (!origin || origin->kind != AST_DECL_FUNCTION) return 0;
    for (const AstGenericParameter *g = origin->generic_parameters; g; g = g->next)
        if (!strcmp(ast_program_lexeme(unit, g->name_token), ast_program_lexeme(unit, type->name_token))) return 1;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        if (contains_type_parameter(unit, &a->type, origin)) return 1;
    for (const AstTypeArgument *a = type->function_parameters; a; a = a->next)
        if (contains_type_parameter(unit, &a->type, origin)) return 1;
    if (type->function_return_type && contains_type_parameter(unit, type->function_return_type, origin)) return 1;
    return 0;
}

static int type_mentions(const AstProgram *unit, const AstType *type, const char *name) {
    if (type->kind == AST_TYPE_NAMED && !strcmp(ast_program_lexeme(unit, type->name_token), name)) return 1;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        if (type_mentions(unit, &a->type, name)) return 1;
    for (const AstTypeArgument *a = type->function_parameters; a; a = a->next)
        if (type_mentions(unit, &a->type, name)) return 1;
    if (type->function_return_type && type_mentions(unit, type->function_return_type, name)) return 1;
    return 0;
}

int viable_function(const Analyzer *analyzer, const SemanticSymbol *function,
                           const AstExpression *arguments) {
    size_t actual = 0;
    for (const AstExpression *argument = arguments; argument != NULL;
         argument = argument->next)
        actual++;
    if (function == NULL || function->declaration == NULL ||
        parameter_count(function->declaration) != actual)
        return 0;
    const AstExpression *argument = arguments;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    const AstDeclarationNode *origin = function->declaration->generic_origin;
    if (origin && origin->kind == AST_DECL_FUNCTION)
        for (const AstGenericParameter *g = origin->generic_parameters; g; g = g->next) {
            int found = 0;
            for (const AstParameter *p = origin->as.function.parameters; p; p = p->next)
                if (type_mentions(function->source_program, &p->type,
                                  ast_program_lexeme(function->source_program, g->name_token))) found = 1;
            if (!found) return 0;
        }
    const AstParameter *pattern = origin && origin->kind == AST_DECL_FUNCTION ? origin->as.function.parameters : NULL;
    for (; argument != NULL && parameter != NULL;
           argument = argument->next, parameter = parameter->next) {
        int rank = argument_conversion_rank(analyzer, argument, function->source_program, &parameter->type);
        if (rank < 0) return 0;
        if (pattern && contains_type_parameter(function->source_program, &pattern->type, origin) && rank != 0) {
            int array_to_slice = parameter->type.is_slice && argument->resolved_is_array &&
                                 parameter->type.pointer_depth == argument->resolved_pointer_depth &&
                                 primitive_type(function->source_program, &parameter->type) == argument->resolved_type
                                 &&
                                 resolve_named_symbol_id(analyzer, function->source_program,
                                                         named_type_token(function->source_program, &parameter->type))
                                 == argument->resolved_named_symbol_id;
            if (!array_to_slice) return 0;
        }
        if (pattern) pattern = pattern->next;
    }
    return argument == NULL && parameter == NULL;
}

int function_dominates(const Analyzer *analyzer,
                              const SemanticSymbol *left,
                              const SemanticSymbol *right,
                              const AstExpression *arguments) {
    const AstParameter *a = left->declaration->as.function.parameters;
    const AstParameter *b = right->declaration->as.function.parameters;
    int strictly_better = 0;
    for (const AstExpression *argument = arguments; argument != NULL;
         argument = argument->next, a = a->next, b = b->next) {
        int left_rank = argument_conversion_rank(analyzer, argument,
                                                 left->source_program, &a->type);
        int right_rank = argument_conversion_rank(analyzer, argument,
                                                  right->source_program, &b->type);
        if (left_rank > right_rank) return 0;
        if (left_rank < right_rank) strictly_better = 1;
    }
    if (!strictly_better && left->declaration->generic_origin == NULL &&
        right->declaration->generic_origin != NULL)
        return 1;
    return strictly_better;
}

const SemanticSymbol *resolve_overload(const Analyzer *analyzer,
                                              const char *name,
                                              size_t owner_symbol_id,
                                              int is_static,
                                              const AstExpression *arguments,
                                              int *ambiguous) {
    const SemanticSymbol *selected = NULL;
    size_t non_dominated = 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *candidate = &analyzer->model->symbols[i];
        if (candidate->kind != SEMANTIC_SYMBOL_FUNCTION ||
            candidate->owner_symbol_id != owner_symbol_id ||
            !(owner_symbol_id == AST_SYMBOL_NONE
                  ? symbol_matches_scope(analyzer->program, candidate, name)
                  : same_name(candidate->source_program, candidate->name_token, name)) ||
            candidate->declaration == NULL ||
            (owner_symbol_id != AST_SYMBOL_NONE &&
             candidate->declaration->as.function.is_static != is_static) ||
            !viable_function(analyzer, candidate, arguments))
            continue;
        int dominated = 0;
        for (size_t j = 0; j < analyzer->model->symbol_count && !dominated; j++) {
            const SemanticSymbol *other = &analyzer->model->symbols[j];
            if (other == candidate || other->kind != SEMANTIC_SYMBOL_FUNCTION ||
                other->owner_symbol_id != owner_symbol_id ||
                !(owner_symbol_id == AST_SYMBOL_NONE
                      ? symbol_matches_scope(analyzer->program, other, name)
                      : same_name(other->source_program, other->name_token, name)) ||
                other->declaration == NULL ||
                (owner_symbol_id != AST_SYMBOL_NONE &&
                 other->declaration->as.function.is_static != is_static) ||
                !viable_function(analyzer, other, arguments))
                continue;
            dominated = function_dominates(analyzer, other, candidate, arguments);
        }
        if (!dominated) {
            selected = candidate;
            non_dominated++;
        }
    }
    *ambiguous = non_dominated > 1;
    return non_dominated == 1 ? selected : NULL;
}


static void validate_builtin_arguments(Analyzer *analyzer,
                                       const AstExpression *expression,
                                       const char *name) {
    const AstExpression *a = expression->arguments;
    const AstExpression *b = a == NULL ? NULL : a->next;
    const AstExpression *c = b == NULL ? NULL : b->next;
    int valid = 1;
    const CoreIntrinsic *core = core_intrinsic_find(name);
    if (core != NULL) {
        const AstExpression *argument = a;
        for (size_t i = 0; i < core->argument_count; ++i, argument = argument->next) {
            if (argument == NULL) {
                valid = 0;
                break;
            }
            CoreValueKind kind = core->arguments[i];
            int plain = argument->resolved_pointer_depth == 0 &&
                        argument->resolved_outer_pointer_depth == 0 &&
                        !argument->resolved_is_array && !argument->resolved_is_slice;
            if (kind == CORE_BYTES)
                valid &= argument->resolved_type == TYPE_U8 &&
                        argument->resolved_pointer_depth == 1 &&
                        argument->resolved_outer_pointer_depth == 0 &&
                        !argument->resolved_is_array && !argument->resolved_is_slice;
            else if (kind == CORE_STRING)
                valid &= plain && argument->resolved_type == TYPE_STRING;
            else
                valid &= plain && integral_expression(argument);
        }
    } else if (strcmp(name, "strlen") == 0 || strcmp(name, "strdup") == 0 ||
               strcmp(name, "io_strlen") == 0 || strcmp(name, "io_str_to_int") == 0)
        valid = string_expression(a);
    else if (strcmp(name, "strcmp") == 0)
        valid = string_expression(a) && string_expression(b);
    else if (strcmp(name, "strcpy") == 0 || strcmp(name, "strcat") == 0)
        valid = a != NULL && a->resolved_type == TYPE_CHAR && pointer_expression(a) &&
                string_expression(b);
    else if (strcmp(name, "malloc") == 0)
        valid = integral_expression(a);
    else if (strcmp(name, "sys_close") == 0)
        valid = integral_expression(a);
    else if (strcmp(name, "sys_open") == 0)
        valid = string_expression(a) && integral_expression(b) && integral_expression(c);
    else if (strcmp(name, "sys_write") == 0 || strcmp(name, "sys_read") == 0)
        valid = integral_expression(a) &&
                (string_expression(b) || pointer_expression(b)) && integral_expression(c);
    else if (strcmp(name, "io_int_to_str") == 0)
        valid = integral_expression(a) && b != NULL && b->resolved_type == TYPE_CHAR &&
                pointer_expression(b) && integral_expression(c);
    else if (strcmp(name, "read") == 0)
        valid = integral_expression(a) && string_expression(b);
    if (!valid) {
        const char *expected = "builtin parameter types";
        if (strcmp(name, "strlen") == 0 || strcmp(name, "strdup") == 0 ||
            strcmp(name, "io_strlen") == 0 || strcmp(name, "io_str_to_int") == 0)
            expected = "(string)";
        else if (strcmp(name, "strcmp") == 0) expected = "(string, string)";
        else if (strcmp(name, "strcpy") == 0 || strcmp(name, "strcat") == 0) expected = "(*char, string)";
        else if (strcmp(name, "malloc") == 0 || strcmp(name, "sys_close") == 0) expected = "(integral)";
        else if (strcmp(name, "sys_open") == 0) expected = "(string, integral, integral)";
        else if (strcmp(name, "sys_read") == 0 || strcmp(name, "sys_write") == 0)
            expected = "(integral, string or pointer, integral)";
        else if (strcmp(name, "io_int_to_str") == 0) expected = "(integral, *char, integral)";
        else if (strcmp(name, "read") == 0) expected = "(integral, string)";
        DiagnosticText message = {0};
        diagnostic_append(&message, "Builtin argument has an incompatible type for '%s'; expected %s; supplied types:",
                          name, expected);
        for (const AstExpression *argument = expression->arguments; argument; argument = argument->next) {
            char length[32];
            snprintf(length, sizeof(length), "%zu", argument->resolved_array_length);
            diagnostic_append(&message, " '");
            diagnostic_type(&message, analyzer, argument->resolved_type, argument->resolved_named_symbol_id,
                            argument->resolved_pointer_depth, argument->resolved_outer_pointer_depth,
                            argument->resolved_is_array, argument->resolved_is_slice, length);
            diagnostic_append(&message, "'");
        }
        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                       message.failed ? "Builtin argument has an incompatible type" : message.text);
        free(message.text);
    }
}

static void validate_function_arguments(Analyzer *analyzer,
                                        const AstExpression *expression,
                                        const SemanticSymbol *function) {
    if (function == NULL || function->kind != SEMANTIC_SYMBOL_FUNCTION ||
        function->declaration == NULL)
        return;
    size_t actual = 0;
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next)
        actual++;
    size_t expected = parameter_count(function->declaration);
    if (actual != expected) {
        char message[128];
        (void) snprintf(message, sizeof(message),
                        "Function expects %zu arguments but received %zu", expected, actual);
        semantic_error(analyzer, expression->value_token,
                       ERROR_CATEGORY_SEMANTIC, ERR_SEM_WRONG_ARG_COUNT, message);
        return;
    }
    const AstExpression *argument = expression->arguments;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
           argument = argument->next, parameter = parameter->next) {
        if (!expression_to_declared_type_allowed(analyzer, argument,
                                                 function->source_program, &parameter->type))
            conversion_error(analyzer, argument, function->source_program, &parameter->type,
                             NULL, "Cannot implicitly convert argument to parameter type");
    }
}

static void validate_callable_arguments(Analyzer *analyzer,
                                        const AstExpression *expression,
                                        const AstType *callable,
                                        const AstProgram *program) {
    const AstTypeArgument *parameter = callable->function_parameters;
    const AstExpression *argument = expression->arguments;
    size_t expected = 0, actual = 0;
    for (const AstTypeArgument *p = parameter; p; p = p->next) expected++;
    for (const AstExpression *a = argument; a; a = a->next) actual++;
    if (expected != actual) {
        char message[128];
        (void) snprintf(message, sizeof(message),
                        "Callable expects %zu arguments but received %zu", expected, actual);
        semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_SEMANTIC,
                       ERR_SEM_WRONG_ARG_COUNT, message);
        return;
    }
    for (; parameter && argument; parameter = parameter->next, argument = argument->next)
        if (!expression_to_declared_type_allowed(analyzer, argument, program, &parameter->type))
            conversion_error(analyzer, argument, program, &parameter->type, NULL,
                             "Cannot implicitly convert argument to callable parameter type");
}

void validate_expression(Analyzer *analyzer, AstExpression *expression,
                                int is_callee) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_TYPE_INFO) {
        if (!is_callee)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Type metadata is compile-time only; access .name, .size or .align, or use match");
        if (expression->left) validate_expression(analyzer, expression->left, 0);
        return;
    }
    if (expression->kind == AST_EXPR_TYPE_PROPERTY) {
        validate_expression(analyzer, expression->left, 1);
        return;
    }
    if ((expression->kind == AST_EXPR_ENUM_CONSTRUCT || expression->kind == AST_EXPR_ENUM_ACCESS || expression->kind ==
         AST_EXPR_MEMBER) &&
        expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *variant = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (variant->kind == SEMANTIC_SYMBOL_ENUM_VALUE && !same_package(analyzer->program, variant->source_program) &&
            !((const AstEnumValue *) variant->node)->is_public)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                           "Enum variant is private to its defining package");
    }
    if (expression->kind == AST_EXPR_CALL && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *called = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (called->declaration && !called->declaration->is_public && !same_package(
                analyzer->program, called->source_program))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                           "Function is private to its defining package");
    }
    if (expression->kind == AST_EXPR_MEMBER && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *member = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (!same_package(analyzer->program, member->source_program) &&
            ((member->kind == SEMANTIC_SYMBOL_FIELD && !((const AstField *) member->node)->is_public) ||
             (member->kind == SEMANTIC_SYMBOL_FUNCTION && member->declaration && !member->declaration->is_public)))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                           "Member is private to its defining package");
    }
    if (expression->kind == AST_EXPR_ENUM_ACCESS) {
        if (expression->arguments != NULL)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_WRONG_ARG_COUNT, "Enum payload accessor expects no arguments");
        validate_expression(analyzer, expression->left->left, 0);
        for (AstExpression *a = expression->arguments; a; a = a->next) validate_expression(analyzer, a, 0);
        return;
    }
    if (!is_callee && expression->kind == AST_EXPR_MEMBER &&
        expression->resolved_symbol_id < analyzer->model->symbol_count &&
        analyzer->model->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
        const SemanticSymbol *variant = &analyzer->model->symbols[expression->resolved_symbol_id];
        const AstEnumValue *value = variant->node;
        if (value && value->payload_types)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Payload variant requires constructor arguments");
    }
    if (expression->token_count > 512U && !analyzer->complexity_error_reported) {
        semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                       "Expression tree exceeds 512 tokens");
        analyzer->complexity_error_reported = 1;
    }
    if (!is_callee && expression->kind == AST_EXPR_MEMBER && expression->left != NULL &&
        expression->resolved_symbol_id < analyzer->model->symbol_count &&
        analyzer->model->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION) {
        const SemanticSymbol *method = &analyzer->model->symbols[expression->resolved_symbol_id];
        const AstExpression *receiver = expression->left;
        int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
            (analyzer->model->symbols[receiver->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
             analyzer->model->symbols[receiver->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM ||
             analyzer->model->symbols[receiver->resolved_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE);
        if (type_receiver) return;
        ErrorContext *context = error_context_create(SEVERITY_ERROR,
                                                     expression->span.begin.line, expression->span.begin.column,
                                                     ERROR_CATEGORY_SEMANTIC, ERR_SEM_METHOD_REFERENCE,
                                                     analyzer->program->source_path,
                                                     "Methods cannot be used as values; call the method with parentheses");
        error_context_set_span(context, expression->span.end.line, expression->span.end.column);
        size_t candidates = 0;
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            const SemanticSymbol *symbol = &analyzer->model->symbols[i];
            if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION &&
                symbol->owner_symbol_id == method->owner_symbol_id &&
                same_name(symbol->source_program, symbol->name_token, name))
                candidates++;
        }
        if (candidates == 1 && method->declaration != NULL && parameter_count(method->declaration) == 0 &&
            method->declaration->as.function.is_static == type_receiver &&
            primitive_type(method->source_program, &method->declared_type) != TYPE_VOID) {
            error_context_set_suggestion(context, "Add '()' after the method name to call this zero-argument method");
            const AstToken *name_location = ast_program_token(analyzer->program, expression->value_token);
            if (name_location != NULL)
                error_context_set_fix(context, name_location->span.end.line, name_location->span.end.column,
                                      name_location->span.end.line, name_location->span.end.column, "()");
        } else
            error_context_set_suggestion(context,
                                         "Call the method with the required arguments and a valid receiver; method references are unsupported");
        error_report_context(global_error_handler, context);
        if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
        analyzer->model->error_count++;
        validate_expression(analyzer, expression->left, 0);
        return;
    }
    if (expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        if (expression->resolved_symbol_id >= analyzer->model->symbol_count) return;
        const SemanticSymbol *variant = &analyzer->model->symbols[expression->resolved_symbol_id];
        const AstEnumValue *value = (const AstEnumValue *) variant->node;
        const AstTypeArgument *payload = value->payload_types;
        AstExpression *argument = expression->arguments;
        for (; payload && argument; payload = payload->next, argument = argument->next) {
            if (!expression_to_declared_type_allowed(analyzer, argument, variant->source_program, &payload->type))
                conversion_error(analyzer, argument, variant->source_program, &payload->type, NULL,
                                 "Enum payload type mismatch");
            validate_expression(analyzer, argument, 0);
        }
        if (payload || argument)
            semantic_error(analyzer, expression->first_token,
                           ERROR_CATEGORY_SEMANTIC, ERR_SEM_WRONG_ARG_COUNT,
                           "Enum constructor payload argument count mismatch");
        return;
    }
    if (expression->kind == AST_EXPR_CALL) {
        if (expression->left != NULL && expression->left->has_resolved_ast_type &&
            expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION &&
            expression->left->resolved_ast_type.function_generic_parameters == NULL) {
            validate_callable_arguments(analyzer, expression,
                &expression->left->resolved_ast_type,
                expression->left->resolved_type_program != NULL
                    ? expression->left->resolved_type_program : analyzer->program);
        }
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program,
                                                  expression->left->value_token);
            TokenType token_type = analyzer->program->tokens[
                expression->left->value_token].type;
            int cast = token_type >= TOKEN_TYPE_INT && token_type <= TOKEN_TYPE_NEVER;
            if (expression->resolved_symbol_id == AST_SYMBOL_NONE &&
                !(expression->left->has_resolved_ast_type &&
                  expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION) &&
                !is_builtin_name(name) && !cast) {
                int ambiguous = 0;
                const SemanticSymbol *viable = resolve_overload(analyzer, name,
                                                                AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
                (void) viable;
                char message[MAX_TOKEN + 32];
                const SemanticSymbol *declared = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_FUNCTION);
                if (declared != NULL)
                    overload_error(analyzer, expression, name, AST_SYMBOL_NONE, 0, ambiguous);
                else {
                    (void) snprintf(message, sizeof(message), "Function '%s' not found", name);
                    semantic_error(analyzer, expression->left->value_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_UNDEFINED_FUNCTION, message);
                }
            } else if (expression->resolved_symbol_id == AST_SYMBOL_NONE &&
                       is_builtin_name(name)) {
                size_t actual = 0;
                for (AstExpression *argument = expression->arguments;
                     argument != NULL; argument = argument->next)
                    actual++;
                size_t expected = builtin_arity(name);
                if (expected != AST_TOKEN_NONE && actual != expected) {
                    char message[128];
                    (void) snprintf(message, sizeof(message),
                                    "Function expects %zu arguments but received %zu", expected, actual);
                    semantic_error(analyzer, expression->left->value_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_WRONG_ARG_COUNT, message);
                } else validate_builtin_arguments(analyzer, expression, name);
            } else if (expression->resolved_symbol_id < analyzer->model->symbol_count)
                validate_function_arguments(analyzer, expression,
                                            &analyzer->model->symbols[expression->resolved_symbol_id]);
        } else validate_expression(analyzer, expression->left, 1);
        if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
            expression->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *method =
                    &analyzer->model->symbols[expression->resolved_symbol_id];
            if (method->owner_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[method->owner_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE) {
                int uses_self = type_mentions(method->source_program,
                    &method->declaration->as.function.return_type, "Self");
                for (const AstParameter *p = method->declaration->as.function.parameters;
                     p; p = p->next)
                    if (type_mentions(method->source_program, &p->type, "Self")) uses_self = 1;
                if (uses_self)
                    semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Methods using Self cannot be called through an interface value");
            }
            const AstExpression *receiver = expression->left->left;
            int type_receiver = receiver != NULL &&
                                receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_STRUCT ||
                                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_ENUM);
            if (method->kind == SEMANTIC_SYMBOL_FUNCTION && method->declaration != NULL &&
                !method->declaration->as.function.is_static && type_receiver)
                semantic_error(analyzer, expression->value_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Instance method requires a struct value receiver");
            if (method->kind == SEMANTIC_SYMBOL_FUNCTION && method->declaration != NULL &&
                method->declaration->as.function.is_static && !type_receiver)
                semantic_error(analyzer, expression->value_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Static method requires a struct type receiver");
            validate_function_arguments(analyzer, expression,
                                        method);
        } else if (expression->left != NULL &&
                   expression->left->kind == AST_EXPR_MEMBER &&
                   expression->left->left != NULL &&
                   !(expression->left->has_resolved_ast_type &&
                     expression->left->resolved_ast_type.kind ==
                         AST_TYPE_FUNCTION)) {
            const AstExpression *receiver = expression->left->left;
            int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_STRUCT ||
                                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_ENUM);
            int ambiguous = 0;
            const char *name = ast_program_lexeme(analyzer->program,
                                                  expression->left->value_token);
            const SemanticSymbol *interface_method = find_method(analyzer,
                receiver->resolved_named_symbol_id, expression->left->value_token);
            int self_error = 0;
            if (interface_method && interface_method->owner_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[interface_method->owner_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE) {
                self_error = type_mentions(interface_method->source_program,
                    &interface_method->declaration->as.function.return_type, "Self");
                for (const AstParameter *p = interface_method->declaration->as.function.parameters;
                     p; p = p->next)
                    if (type_mentions(interface_method->source_program, &p->type, "Self")) self_error = 1;
            }
            if (self_error)
                semantic_error(analyzer, expression->left->value_token, ERROR_CATEGORY_TYPE,
                               ERR_TYPE_INVALID_OPERATION,
                               "Methods using Self cannot be called through an interface value");
            else {
                (void) resolve_overload(analyzer, name, receiver->resolved_named_symbol_id,
                                        type_receiver, expression->arguments, &ambiguous);
                overload_error(analyzer, expression, name, receiver->resolved_named_symbol_id,
                               type_receiver, ambiguous);
            }
        }
        for (AstExpression *argument = expression->arguments; argument != NULL;
             argument = argument->next)
            validate_expression(analyzer, argument, 0);
        return;
    }
    validate_expression(analyzer, expression->left, 0);
    validate_expression(analyzer, expression->right, 0);
    for (AstExpression *argument = expression->arguments; argument != NULL;
         argument = argument->next)
        validate_expression(analyzer, argument, 0);
    if (expression->kind == AST_EXPR_BINARY && expression->left && expression->right &&
        ((expression->left->has_resolved_ast_type &&
          expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION) ||
         (expression->right->has_resolved_ast_type &&
          expression->right->resolved_ast_type.kind == AST_TYPE_FUNCTION))) {
        int equality = expression->operator_type == TOKEN_EQUAL_EQUAL ||
                       expression->operator_type == TOKEN_BANG_EQUAL;
        const AstProgram *left_program = expression->left->resolved_type_program != NULL
                                             ? expression->left->resolved_type_program : analyzer->program;
        const AstProgram *right_program = expression->right->resolved_type_program != NULL
                                              ? expression->right->resolved_type_program : analyzer->program;
        if (!equality || !expression->left->has_resolved_ast_type ||
            !expression->right->has_resolved_ast_type ||
            !ast_concrete_type_equal(left_program, &expression->left->resolved_ast_type,
                                     right_program, &expression->right->resolved_ast_type))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE,
                          ERR_TYPE_INVALID_OPERATION,
                          "Callable values support only == and != with the same signature");
        return;
    }
    if (expression->kind != AST_EXPR_NAME &&
        ((expression->left != NULL && expression->left->resolved_type == TYPE_UNKNOWN &&
          expression->left->resolved_named_symbol_id == AST_SYMBOL_NONE) ||
         (expression->right != NULL && expression->right->resolved_type == TYPE_UNKNOWN &&
          expression->right->resolved_named_symbol_id == AST_SYMBOL_NONE)))
        return;
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_AMPERSAND &&
        expression->mutable_borrow && !assignable_expression(analyzer, expression->right)) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Mutable borrow requires a mutable lvalue");
    } else if (expression->kind == AST_EXPR_UNARY &&
               expression->operator_type == TOKEN_AMPERSAND &&
               expression->right != NULL &&
               expression->right->kind != AST_EXPR_NAME &&
               expression->right->kind != AST_EXPR_MEMBER &&
               expression->right->kind != AST_EXPR_INDEX &&
               !(expression->right->kind == AST_EXPR_UNARY &&
                 expression->right->operator_type == TOKEN_STAR)) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Borrow requires an addressable value");
    } else if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR &&
               expression->right != NULL && expression->right->resolved_pointer_depth == 0 &&
               expression->right->resolved_outer_pointer_depth == 0) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Dereference requires a pointer");
    } else if (expression->kind == AST_EXPR_UNARY &&
               (expression->operator_type == TOKEN_MINUS ||
                expression->operator_type == TOKEN_BANG) &&
               !plain_numeric_expression(expression->right)) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Unary numeric operator requires a numeric operand");
    } else if (expression->kind == AST_EXPR_UNARY &&
               expression->operator_type == TOKEN_MINUS && expression->right != NULL &&
               expression->right->resolved_type == TYPE_BIT) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Boolean values do not support arithmetic");
    } else if (expression->kind == AST_EXPR_BINARY && expression->left != NULL &&
               expression->right != NULL) {
        TokenType operation = expression->operator_type;
        int arithmetic = operation >= TOKEN_PLUS && operation <= TOKEN_PERCENT;
        int logical = operation == TOKEN_AMP_AMP || operation == TOKEN_PIPE_PIPE;
        int equality = operation == TOKEN_EQUAL_EQUAL || operation == TOKEN_BANG_EQUAL;
        int relational = operation >= TOKEN_LESS && operation <= TOKEN_GREATER_EQUAL;
        int string_concat = operation == TOKEN_PLUS &&
                            (expression->left->resolved_type == TYPE_STRING ||
                             expression->right->resolved_type == TYPE_STRING);
        if ((pointer_expression(expression->left) || pointer_expression(expression->right)) &&
            !equality && !string_concat)
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Pointer arithmetic is not supported");
        if (operation == TOKEN_PERCENT &&
            (expression->left->resolved_type == TYPE_FLOAT ||
             expression->left->resolved_type == TYPE_DOUBLE ||
             expression->right->resolved_type == TYPE_FLOAT ||
             expression->right->resolved_type == TYPE_DOUBLE))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Remainder requires integer operands");
        if (string_concat &&
            (!(string_expression(expression->left) ||
               plain_numeric_expression(expression->left)) ||
             !(string_expression(expression->right) ||
               plain_numeric_expression(expression->right))))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "String concatenation requires scalar operands");
        else if (arithmetic && !string_concat &&
                 (!plain_numeric_expression(expression->left) ||
                  !plain_numeric_expression(expression->right)))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Arithmetic requires numeric operands");
        else if (arithmetic && !string_concat &&
                 (expression->left->resolved_type == TYPE_BIT ||
                  expression->right->resolved_type == TYPE_BIT))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Boolean values do not support arithmetic");
        if (logical && (!plain_numeric_expression(expression->left) ||
                        !plain_numeric_expression(expression->right)))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Logical operators require numeric operands");
        if (relational && (!plain_numeric_expression(expression->left) ||
                            !plain_numeric_expression(expression->right)))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Relational comparison requires numeric operands");
        if (equality) {
            unsigned left_depth = expression->left->resolved_pointer_depth +
                                  expression->left->resolved_outer_pointer_depth;
            unsigned right_depth = expression->right->resolved_pointer_depth +
                                   expression->right->resolved_outer_pointer_depth;
            int numeric = plain_numeric_expression(expression->left) &&
                          plain_numeric_expression(expression->right);
            int strings = expression->left->resolved_type == TYPE_STRING &&
                          expression->right->resolved_type == TYPE_STRING &&
                           left_depth == 0 && right_depth == 0;
            int slices = expression->left->resolved_is_slice &&
                         expression->right->resolved_is_slice &&
                         expression->left->resolved_outer_pointer_depth == 0 &&
                         expression->right->resolved_outer_pointer_depth == 0 &&
                         expression->left->has_resolved_ast_type &&
                         expression->right->has_resolved_ast_type &&
                         ast_concrete_type_equal(
                             expression->left->resolved_type_program,
                             &expression->left->resolved_ast_type,
                             expression->right->resolved_type_program,
                             &expression->right->resolved_ast_type);
            int pointers = left_depth != 0 && left_depth == right_depth &&
                           !(expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth) &&
                           expression->left->resolved_pointer_depth == expression->right->resolved_pointer_depth &&
                           expression->left->resolved_outer_pointer_depth == expression->right->
                           resolved_outer_pointer_depth &&
                           expression->left->resolved_array_length == expression->right->resolved_array_length &&
                           expression->left->resolved_is_array == expression->right->resolved_is_array &&
                           expression->left->resolved_is_slice == expression->right->resolved_is_slice &&
                           expression->left->resolved_type == expression->right->resolved_type &&
                           expression->left->resolved_named_symbol_id ==
                           expression->right->resolved_named_symbol_id;
            int named = left_depth == 0 && right_depth == 0 &&
                        expression->left->resolved_named_symbol_id != AST_SYMBOL_NONE &&
                        expression->left->resolved_named_symbol_id ==
                        expression->right->resolved_named_symbol_id &&
                        expression->left->resolved_named_symbol_id < analyzer->model->symbol_count &&
                        analyzer->model->symbols[expression->left->resolved_named_symbol_id].kind ==
                        SEMANTIC_SYMBOL_ENUM &&
                        !analyzer->model->symbols[expression->left->resolved_named_symbol_id].declaration->as.enum_decl.
                        is_sum;
            if (!numeric && !strings && !pointers && !named && !slices)
                operand_error(analyzer, expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                              "Equality comparison requires compatible operands");
        }
    } else if (expression->kind == AST_EXPR_SUBSLICE &&
               expression->left != NULL) {
        if ((!expression->left->resolved_is_array &&
             !expression->left->resolved_is_slice) ||
            expression->left->resolved_outer_pointer_depth != 0)
            operand_error(analyzer, expression, ERROR_CATEGORY_SEMANTIC,
                          ERR_SEM_NOT_ARRAY,
                          "Subslicing requires an array or slice");
        if (expression->right != NULL &&
            !integral_expression(expression->right))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE,
                          ERR_TYPE_INVALID_OPERATION,
                          "Subslice start must be integral");
        if (expression->arguments != NULL &&
            !integral_expression(expression->arguments))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE,
                          ERR_TYPE_INVALID_OPERATION,
                          "Subslice end must be integral");
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        if (!pointer_expression(expression->left))
            operand_error(analyzer, expression, ERROR_CATEGORY_SEMANTIC, ERR_SEM_NOT_ARRAY,
                          "Indexing requires an array or pointer");
        if (!integral_expression(expression->right))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Array index must be integral");
        if (expression->resolved_type == TYPE_VOID && !expression->resolved_pointer_depth && !expression->
            resolved_outer_pointer_depth)
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Indexing requires a complete non-void element type");
    } else if (expression->kind == AST_EXPR_CAST) {
        const AstExpression *argument = expression->arguments;
        int pointer_cast = argument && argument->next == NULL &&
                           (argument->resolved_pointer_depth || argument->resolved_outer_pointer_depth) &&
                           ((!argument->resolved_is_array && !argument->resolved_is_slice) || argument->
                            resolved_outer_pointer_depth) &&
                           (expression->allocated_type.pointer_depth || expression->allocated_type.outer_pointer_depth)
                           &&
                           ((!expression->allocated_type.is_array && !expression->allocated_type.is_slice) || expression
                            ->allocated_type.outer_pointer_depth) &&
                           known_declared_type(analyzer, &expression->allocated_type);
        if (!pointer_cast && (argument == NULL || argument->next != NULL ||
                              !plain_numeric_expression(argument) ||
                              expression->resolved_type == TYPE_VOID ||
                              expression->resolved_type == TYPE_STRING ||
                              expression->resolved_type == TYPE_UNKNOWN ||
                              expression->allocated_type.pointer_depth != 0 ||
                              expression->allocated_type.outer_pointer_depth != 0 ||
                              expression->allocated_type.is_array || expression->allocated_type.is_slice))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Cast requires one numeric value and a numeric target type, or two pointer types");
    } else if (expression->kind == AST_EXPR_SLICE) {
        const AstExpression *data = expression->left, *length = expression->right;
        int typed_pointer = data != NULL &&
                            data->resolved_pointer_depth != 0 &&
                            data->resolved_outer_pointer_depth == 0 &&
                            !data->resolved_is_array &&
                            !data->resolved_is_slice &&
                            !(data->resolved_type == TYPE_VOID &&
                              data->resolved_pointer_depth == 1);
        if (data != NULL && data->has_resolved_ast_type) {
            AstType pointed = data->resolved_ast_type;
            int dereferenced = 0;
            if (pointed.borrow_kind != AST_BORROW_NONE) {
                pointed.borrow_kind = AST_BORROW_NONE;
                dereferenced = 1;
            } else if (pointed.outer_pointer_depth != 0) {
                pointed.outer_pointer_depth--;
                dereferenced = 1;
            } else if (pointed.pointer_depth != 0) {
                pointed.pointer_depth--;
                dereferenced = 1;
            }
            typed_pointer = dereferenced &&
                            !(primitive_type(
                                  data->resolved_type_program != NULL
                                      ? data->resolved_type_program
                                      : analyzer->program,
                                  &pointed) == TYPE_VOID &&
                              !pointed.is_array && !pointed.is_slice &&
                              pointed.pointer_depth == 0 &&
                              pointed.outer_pointer_depth == 0);
        }
        if (!typed_pointer || !integral_expression(length))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "slice requires a typed element pointer and an integral length");
    } else if (expression->kind == AST_EXPR_FREE) {
        const AstExpression *argument = expression->arguments;
        if (argument == NULL || argument->next != NULL ||
            ((!pointer_expression(argument) && argument->resolved_type != TYPE_STRING) ||
             ((argument->resolved_is_array || argument->resolved_is_slice) && !argument->resolved_outer_pointer_depth)))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "free requires one pointer or owned string");
    } else if (expression->kind == AST_EXPR_RESERVE) {
        const AstType *allocated = &expression->allocated_type;
        if (ast_type_contains_polymorphic_callable(allocated))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "Polymorphic callable values cannot be allocated");
        if (!known_declared_type(analyzer, allocated) ||
            (primitive_type(analyzer->program, allocated) == TYPE_VOID &&
             allocated->pointer_depth == 0 && allocated->outer_pointer_depth == 0))
            operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                          "reserve requires one complete sized non-void type");
    }
    if (expression->kind == AST_EXPR_NAME && !is_callee &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        TokenType type = analyzer->program->tokens[expression->value_token].type;
        if (expression->resolved_callable != NULL) return;
        if (strcmp(name, "true") != 0 && strcmp(name, "false") != 0 &&
            !is_builtin_name(name) && !(type >= TOKEN_TYPE_INT && type <= TOKEN_TYPE_NEVER))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_UNDEFINED_VARIABLE,
                           "Undefined variable");
    } else if (expression->kind == AST_EXPR_NAME && !is_callee &&
               expression->resolved_symbol_id < analyzer->model->symbol_count &&
               analyzer->model->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION &&
               !expression->has_resolved_ast_type) {
        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Overloaded function reference requires an expected callable type");
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        long long index = 0;
        if (constant_integer(analyzer, expression->right, &index) &&
            expression->left->has_resolved_ast_type &&
            expression->left->resolved_ast_type.is_array &&
            expression->left->resolved_ast_type.outer_pointer_depth == 0) {
            long long length = (long long)
                expression->left->resolved_ast_type.resolved_array_length;
            if (length != 0 && (index < 0 || index >= length))
                semantic_error(analyzer, expression->right->value_token,
                               ERROR_CATEGORY_TYPE,
                               ERR_TYPE_INVALID_OPERATION,
                               "Array index is outside declared bounds");
        } else if (constant_integer(analyzer, expression->right, &index) &&
                   expression->left->resolved_symbol_id <
                       analyzer->model->symbol_count) {
            const SemanticSymbol *base =
                    &analyzer->model->symbols[expression->left->resolved_symbol_id];
            if (base->declared_type.is_array &&
                base->declared_type.array_length_token < analyzer->program->token_count) {
                long long length = (long long) base->declared_type.resolved_array_length;
                if (length == 0)
                    length = strtoll(ast_program_lexeme(base->source_program,
                                                        base->declared_type.array_length_token), NULL, 10);
                if (index < 0 || index >= length)
                    semantic_error(analyzer, expression->right->value_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_INDEX_OUT_OF_BOUNDS,
                                   "Array index is outside declared bounds");
            }
        }
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL &&
               expression->resolved_symbol_id == AST_SYMBOL_NONE) {
        if (expression->left->resolved_named_symbol_id == AST_SYMBOL_NONE) {
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_NOT_STRUCT,
                           "Member access target is not a struct");
        } else {
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_FIELD_NOT_FOUND,
                           "Field or enum member not found");
        }
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL &&
               expression->resolved_symbol_id < analyzer->model->symbol_count &&
               analyzer->model->symbols[expression->resolved_symbol_id].kind ==
               SEMANTIC_SYMBOL_FIELD &&
               expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
               (analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
                SEMANTIC_SYMBOL_STRUCT ||
                analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
                SEMANTIC_SYMBOL_ENUM)) {
        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Instance field requires a value receiver");
    }
}

int statement_may_fall_through(const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_EXPRESSION && statement->expression != NULL &&
            statement->expression->resolved_type == TYPE_NEVER)
            return 0;
        if ((statement->kind == AST_STMT_VARIABLE || statement->kind == AST_STMT_ASSIGNMENT) &&
            statement->value != NULL && statement->value->resolved_type == TYPE_NEVER)
            return 0;
        if ((statement->kind == AST_STMT_IF || statement->kind == AST_STMT_WHILE ||
             statement->kind == AST_STMT_FOR) && statement->condition != NULL &&
            statement->condition->resolved_type == TYPE_NEVER)
            return 0;
        if (statement->kind == AST_STMT_MATCH && statement->match_exhaustive) {
            if (statement->is_type_match) {
                if (statement->selected_type_arm &&
                    !statement_may_fall_through(statement->selected_type_arm->body))
                    return 0;
                continue;
            }
            int any_fallthrough = statement->match_arms == NULL;
            for (const AstMatchArm *a = statement->match_arms; a; a = a->next)
                if (statement_may_fall_through(a->body)) any_fallthrough = 1;
            if (!any_fallthrough) return 0;
        }
        if (statement->kind == AST_STMT_RETURN || statement->kind == AST_STMT_BREAK ||
            statement->kind == AST_STMT_CONTINUE)
            return 0;
        if (statement->kind == AST_STMT_BLOCK && !statement_may_fall_through(statement->body))
            return 0;
        if (statement->kind == AST_STMT_IF && statement->else_body != NULL &&
            !statement_may_fall_through(statement->body) &&
            !statement_may_fall_through(statement->else_body))
            return 0;
    }
    return 1;
}

static int known_declared_type_with_binders(const Analyzer *analyzer,
                                            const AstType *type,
                                            const AstGenericParameter *binders) {
    if (type == NULL || type->kind == AST_TYPE_INFERRED ||
        primitive_type(analyzer->program, type) != TYPE_UNKNOWN)
        return 1;
    if (type->kind == AST_TYPE_NAMED) {
        const char *name = ast_program_lexeme(analyzer->program,
                                              type->name_token);
        for (const AstGenericParameter *generic = binders; generic;
             generic = generic->next)
            if (!strcmp(name, ast_program_lexeme(analyzer->program,
                                                 generic->name_token)))
                return 1;
    }
    if (type->kind == AST_TYPE_FUNCTION) {
        const AstGenericParameter *nested = type->function_generic_parameters != NULL
                                                ? type->function_generic_parameters
                                                : binders;
        if (type->function_return_type == NULL ||
            !known_declared_type_with_binders(analyzer,
                                              type->function_return_type,
                                              nested)) return 0;
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
            if (!known_declared_type_with_binders(analyzer, &parameter->type,
                                                  nested)) return 0;
        return 1;
    }
    return resolve_named_symbol_id(analyzer, analyzer->program,
                                   named_type_token(analyzer->program, type)) != AST_SYMBOL_NONE;
}

int known_declared_type(const Analyzer *analyzer, const AstType *type) {
    return known_declared_type_with_binders(analyzer, type, NULL);
}

void semantic_model_free(SemanticModel *model) {
    if (model == NULL) return;
    free(model->symbol_index);
    free(model->symbols);
    free(model);
}
