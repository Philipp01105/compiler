#include "semantic.h"
#include "semantic_internal.h"
#include "generics.h"
#include "core_intrinsics.h"

#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


static void analyze_constant_declaration(Analyzer * analyzer, AstDeclarationNode * declaration);

static void semantic_error_at(Analyzer *analyzer, size_t token, size_t last_token, char category, int code,
                              const char *message) {
    const AstToken *location = ast_program_token(analyzer->program, token);
    const AstToken *end = ast_program_token(analyzer->program, last_token);
    char named_message[1024];
    if (location != NULL && ((category == ERROR_CATEGORY_TYPE && code == ERR_TYPE_UNKNOWN) ||
                             (category == ERROR_CATEGORY_SEMANTIC &&
                              (code == ERR_SEM_UNDEFINED_VARIABLE || code == ERR_SEM_DUPLICATE_DEFINITION ||
                               code == ERR_SEM_FIELD_NOT_FOUND)))) {
        snprintf(named_message, sizeof(named_message), "%s '%s'", message, location->lexeme);
        message = named_message;
    }
    ErrorContext *context = error_context_create(SEVERITY_ERROR,
                                                 location == NULL ? 0 : location->span.begin.line,
                                                 location == NULL ? 0 : location->span.begin.column,
                                                 category, code, analyzer->program->source_path,
                                                 message);
    if (location != NULL) {
        error_context_set_span(context, end == NULL ? location->span.end.line : end->span.end.line,
                               end == NULL ? location->span.end.column : end->span.end.column);
        error_context_set_token(context, location->lexeme);
    }
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
    analyzer->model->error_count++;
}

void semantic_error(Analyzer *analyzer, size_t token, char category, int code,
                           const char *message) {
    semantic_error_at(analyzer, token, token, category, code, message);
}


void semantic_duplicate(Analyzer *analyzer, size_t token,
                               const AstProgram *previous_program, size_t previous_token,
                               const char *message) {
    const AstToken *location = ast_program_token(analyzer->program, token);
    const AstToken *previous = ast_program_token(previous_program, previous_token);
    char detail[1024];
    snprintf(detail, sizeof(detail), "%s '%s'", message, ast_program_lexeme(analyzer->program, token));
    ErrorContext *context = error_context_create(SEVERITY_ERROR,
                                                 location == NULL ? 0 : location->span.begin.line,
                                                 location == NULL ? 0 : location->span.begin.column,
                                                 ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                                 analyzer->program->source_path, detail);
    if (location != NULL) error_context_set_span(context, location->span.end.line, location->span.end.column);
    if (previous != NULL) {
        ErrorContext *note = error_context_create(SEVERITY_INFO, previous->span.begin.line, previous->span.begin.column,
                                                  ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                                  previous_program->source_path,
                                                  "Previous declaration is here");
        error_context_set_span(note, previous->span.end.line, previous->span.end.column);
        error_context_add_child(context, note);
    }
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
    analyzer->model->error_count++;
}

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
    symbol->resolved_pointer_depth = type == NULL ? 0 : type->pointer_depth;
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
        if (declaration->generic_parameters != NULL) continue;
        if (declaration->kind == AST_DECL_IMPORT) {
            continue; /* Imports are file-local package bindings, not value symbols. */
        } else if (declaration->kind == AST_DECL_FUNCTION && declaration->generic_parameters == NULL) {
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
        } else if (declaration->kind == AST_DECL_INTERFACE) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_INTERFACE, AST_TOKEN_NONE);
            for (AstDeclarationNode *method = declaration->as.interface_decl.methods;
                 method != NULL; method = method->next)
                add_global(analyzer, method, SEMANTIC_SYMBOL_FUNCTION, declaration->name_token);
        }
    }
    analyzer->program = saved_program;
}

static LocalSymbol *push_local(Analyzer *analyzer, size_t name_token, AstType type,
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
    local->resolved_pointer_depth = symbol.resolved_pointer_depth;
    local->resolved_outer_pointer_depth = symbol.resolved_outer_pointer_depth;
    local->resolved_named_type_token = symbol.resolved_named_type_token;
    local->resolved_named_symbol_id = symbol.resolved_named_symbol_id;
    local->resolved_is_array = symbol.resolved_is_array;
    local->resolved_is_slice = symbol.resolved_is_slice;
    local->is_constant = is_constant;
    local->scope_depth = analyzer->scope_depth;
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
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->owner_symbol_id == owner_symbol_id &&
            same_name(symbol->source_program, symbol->name_token, name))
            return symbol;
    }
    return NULL;
}

static DataType promoted_numeric(DataType left, DataType right) {
    if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
    if (left == TYPE_FLOAT || right == TYPE_FLOAT) return TYPE_FLOAT;
    if (left == TYPE_UNKNOWN || right == TYPE_UNKNOWN) return TYPE_UNKNOWN;
    return data_type_promoted_integer(left, right);
}

static DataType builtin_result_type(const char *name) {
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

static int is_builtin_name(const char *name) {
    return builtin_result_type(name) != TYPE_UNKNOWN || strcmp(name, "malloc") == 0 ||
           strcmp(name, "read") == 0;
}

static int same_declared_type(const Analyzer *analyzer,
                              const AstProgram *left_program, const AstType *left,
                              const AstProgram *right_program, const AstType *right) {
    if (left->pointer_depth != right->pointer_depth ||
        left->outer_pointer_depth != right->outer_pointer_depth ||
        left->is_array != right->is_array || left->is_slice != right->is_slice)
        return 0;
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

static void validate_overload_sets(Analyzer *analyzer) {
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
    unsigned target_depth = type->pointer_depth + type->outer_pointer_depth;
    unsigned source_depth = expression->resolved_pointer_depth +
                            expression->resolved_outer_pointer_depth;
    size_t target_name = named_type_token(type_program, type);
    int matching_shape = target_depth == source_depth;
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

static int expression_assignment_allowed(const Analyzer *analyzer,
                                         const AstExpression *source,
                                         const AstExpression *target) {
    if (source == NULL || target == NULL) return 0;
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
                                source->resolved_named_symbol_id)) return 1;
    if (target->resolved_named_symbol_id != AST_SYMBOL_NONE ||
        source->resolved_named_symbol_id != AST_SYMBOL_NONE)
        return target->resolved_named_symbol_id != AST_SYMBOL_NONE &&
               target->resolved_named_symbol_id == source->resolved_named_symbol_id &&
               target_depth == source_depth;
    (void) analyzer;
    return expression_conversion_allowed(source, target->resolved_type, target_depth);
}

static int plain_numeric_expression(const AstExpression *expression) {
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

static int pointer_expression(const AstExpression *expression) {
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

static int enum_constant_expression(const Analyzer *analyzer,
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
    return 0;
}

static int type_mentions(const AstProgram *unit, const AstType *type, const char *name) {
    if (!strcmp(ast_program_lexeme(unit, type->name_token), name)) return 1;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        if (type_mentions(unit, &a->type, name)) return 1;
    return 0;
}

static int viable_function(const Analyzer *analyzer, const SemanticSymbol *function,
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

static const SemanticSymbol *resolve_overload(const Analyzer *analyzer,
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

typedef struct {
    char *text;
    size_t used;
    int failed;
} DiagnosticText;

static void diagnostic_append(DiagnosticText *text, const char *format, ...) {
    if (text->failed) return;
    va_list arguments, copy;
    va_start(arguments, format);
    va_copy(copy, arguments);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || (size_t) count > SIZE_MAX - text->used - 1) text->failed = 1;
    else {
        size_t needed = text->used + (size_t) count + 1;
        char *grown = realloc(text->text, needed);
        if (grown == NULL) text->failed = 1;
        else {
            text->text = grown;
            (void) vsnprintf(text->text + text->used, needed - text->used, format, arguments);
            text->used += (size_t) count;
        }
    }
    va_end(arguments);
}

static void diagnostic_type(DiagnosticText *text, const Analyzer *analyzer,
                            DataType primitive, size_t nominal, unsigned pointers,
                            unsigned outer, int array, int slice, const char *length) {
    static const char *names[] = {DMM_TYPE_NAMES};
    for (unsigned i = 0; i < outer; i++) diagnostic_append(text, "*");
    if (outer != 0 && (array || slice)) diagnostic_append(text, "(");
    for (unsigned i = 0; i < pointers; i++) diagnostic_append(text, "*");
    if (nominal < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[nominal];
        const AstDeclarationNode *declaration = symbol->declaration;
        if (declaration && declaration->generic_origin) {
            diagnostic_append(text, "%s<",
                              ast_program_lexeme(symbol->source_program, declaration->generic_origin->name_token));
            for (const AstTypeArgument *argument = declaration->specialization_arguments; argument;
                 argument = argument->next) {
                const AstType *type = &argument->type;
                char size[32];
                snprintf(size, sizeof(size), "%zu", type->resolved_array_length);
                diagnostic_type(text, analyzer, primitive_type(symbol->source_program, type),
                                resolve_named_symbol_id(analyzer, symbol->source_program,
                                                        named_type_token(symbol->source_program, type)),
                                type->pointer_depth, type->outer_pointer_depth, type->is_array, type->is_slice, size);
                if (argument->next) diagnostic_append(text, ",");
            }
            diagnostic_append(text, ">");
        } else diagnostic_append(text, "%s", ast_program_lexeme(symbol->source_program, symbol->name_token));
    } else diagnostic_append(text, "%s", primitive <= TYPE_UNKNOWN ? names[primitive] : "unknown");
    if (slice) diagnostic_append(text, "[]");
    else if (array) diagnostic_append(text, "[%s]", length == NULL ? "?" : length);
    if (outer != 0 && (array || slice)) diagnostic_append(text, ")");
}

/* Preserve the rule text while showing every supplied operand's complete shape. */
static void operand_error(Analyzer *analyzer, const AstExpression *expression,
                          char category, int code, const char *reason) {
    DiagnosticText message = {0};
    diagnostic_append(&message, "%s; supplied types:", reason);
    const AstExpression *operands[2] = {expression->left, expression->right};
    if (operands[0] == NULL && operands[1] == NULL) operands[0] = expression;
    if (expression->kind == AST_EXPR_CAST || expression->kind == AST_EXPR_FREE)
        operands[0] = expression->arguments;
    for (size_t i = 0; i < 2; i++) {
        const AstExpression *operand = operands[i];
        if (operand == NULL) continue;
        char length[32];
        snprintf(length, sizeof(length), "%zu", operand->resolved_array_length);
        diagnostic_append(&message, " '");
        diagnostic_type(&message, analyzer, operand->resolved_type,
                        operand->resolved_named_symbol_id, operand->resolved_pointer_depth,
                        operand->resolved_outer_pointer_depth, operand->resolved_is_array,
                        operand->resolved_is_slice, length);
        diagnostic_append(&message, "'");
    }
    semantic_error_at(analyzer, expression->first_token,
                      expression->token_count == 0
                          ? expression->first_token
                          : expression->first_token + expression->token_count - 1,
                      category, code, message.failed ? reason : message.text);
    free(message.text);
}

static void conversion_error(Analyzer *analyzer, const AstExpression *value,
                             const AstProgram *expected_program, const AstType *expected,
                             const AstExpression *expected_value, const char *reason) {
    // An unresolved value already has a name/type diagnostic; avoid a follow-up conversion error.
    if (value->resolved_type == TYPE_UNKNOWN && value->resolved_named_symbol_id == AST_SYMBOL_NONE) return;
    DiagnosticText message = {0};
    diagnostic_append(&message, "%s; got '", reason);
    const char *length = NULL;
    if (value->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[value->resolved_symbol_id];
        length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
    }
    diagnostic_type(&message, analyzer, value->resolved_type, value->resolved_named_symbol_id,
                    value->resolved_pointer_depth, value->resolved_outer_pointer_depth,
                    value->resolved_is_array, value->resolved_is_slice, length);
    diagnostic_append(&message, "', expected '");
    if (expected_value != NULL) {
        const char *expected_length = NULL;
        if (expected_value->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *symbol = &analyzer->model->symbols[expected_value->resolved_symbol_id];
            expected_length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
        }
        if (expected_value->resolved_type == TYPE_UNKNOWN && expected_value->resolved_named_symbol_id ==
            AST_SYMBOL_NONE) {
            free(message.text);
            return;
        }
        diagnostic_type(&message, analyzer, expected_value->resolved_type, expected_value->resolved_named_symbol_id,
                        expected_value->resolved_pointer_depth, expected_value->resolved_outer_pointer_depth,
                        expected_value->resolved_is_array, expected_value->resolved_is_slice, expected_length);
    } else
        diagnostic_type(&message, analyzer, primitive_type(expected_program, expected),
                        resolve_named_symbol_id(analyzer, expected_program,
                                                named_type_token(expected_program, expected)),
                        expected->pointer_depth, expected->outer_pointer_depth, expected->is_array, expected->is_slice,
                        ast_program_lexeme(expected_program, expected->array_length_token));
    diagnostic_append(&message, "'");
    semantic_error_at(analyzer, value->first_token,
                      value->token_count == 0 ? value->first_token : value->first_token + value->token_count - 1,
                      ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                      message.failed ? reason : message.text);
    free(message.text);
}

static void overload_error(Analyzer *analyzer, const AstExpression *call,
                           const char *name, size_t owner, int is_static, int ambiguous) {
    DiagnosticText message = {0};
    diagnostic_append(&message, owner == AST_SYMBOL_NONE
                                    ? (ambiguous
                                           ? "Call to '%s' is ambiguous"
                                           : "No overload of '%s' matches the supplied arguments")
                                    : (ambiguous
                                           ? "Method call to '%s' is ambiguous"
                                           : "No method overload matches the supplied arguments for '%s'"), name);
    diagnostic_append(&message, "; supplied types: (");
    size_t index = 0;
    for (const AstExpression *argument = call->arguments; argument != NULL; argument = argument->next) {
        if (index++ != 0) diagnostic_append(&message, ", ");
        const char *length = NULL;
        if (argument->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *symbol = &analyzer->model->symbols[argument->resolved_symbol_id];
            if (symbol->declared_type.is_array)
                length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
        }
        diagnostic_type(&message, analyzer, argument->resolved_type, argument->resolved_named_symbol_id,
                        argument->resolved_pointer_depth, argument->resolved_outer_pointer_depth,
                        argument->resolved_is_array, argument->resolved_is_slice, length);
    }
    diagnostic_append(&message, ambiguous ? "); viable signatures: " : "); available signatures: ");
    index = 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_FUNCTION || symbol->owner_symbol_id != owner ||
            !same_name(symbol->source_program, symbol->name_token, name) || symbol->declaration == NULL ||
            (owner != AST_SYMBOL_NONE && symbol->declaration->as.function.is_static != is_static) ||
            (ambiguous && !viable_function(analyzer, symbol, call->arguments)))
            continue;
        if (index++ != 0) diagnostic_append(&message, "; ");
        if (owner < analyzer->model->symbol_count) {
            const SemanticSymbol *owner_symbol = &analyzer->model->symbols[owner];
            diagnostic_append(&message, "%s.",
                              ast_program_lexeme(owner_symbol->source_program, owner_symbol->name_token));
        }
        diagnostic_append(&message, "%s(", name);
        size_t parameter_index = 0;
        for (const AstParameter *parameter = symbol->declaration->as.function.parameters;
             parameter != NULL; parameter = parameter->next) {
            if (parameter_index++ != 0) diagnostic_append(&message, ", ");
            const AstType *type = &parameter->type;
            diagnostic_type(&message, analyzer, primitive_type(symbol->source_program, type),
                            resolve_named_symbol_id(analyzer, symbol->source_program,
                                                    named_type_token(symbol->source_program, type)),
                            type->pointer_depth, type->outer_pointer_depth, type->is_array, type->is_slice,
                            type->is_array
                                ? ast_program_lexeme(symbol->source_program, type->array_length_token)
                                : NULL);
        }
        diagnostic_append(&message, ")");
    }
    if (index == 0) diagnostic_append(&message, "<none>");
    semantic_error(analyzer, call->left->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                   message.failed ? "Could not format overload diagnostic" : message.text);
    free(message.text);
}

static int assignable_expression(const Analyzer *analyzer,
                                 const AstExpression *expression);

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

static void validate_expression(Analyzer *analyzer, AstExpression *expression,
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
        const AstExpression *receiver = expression->left;
        int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                            analyzer->model->symbols[receiver->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT;
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
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program,
                                                  expression->left->value_token);
            TokenType token_type = analyzer->program->tokens[
                expression->left->value_token].type;
            int cast = token_type >= TOKEN_TYPE_INT && token_type <= TOKEN_TYPE_VOID;
            if (expression->resolved_symbol_id == AST_SYMBOL_NONE &&
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
                   expression->left->left != NULL) {
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
    if (expression->kind != AST_EXPR_NAME &&
        ((expression->left != NULL && expression->left->resolved_type == TYPE_UNKNOWN &&
          expression->left->resolved_named_symbol_id == AST_SYMBOL_NONE) ||
         (expression->right != NULL && expression->right->resolved_type == TYPE_UNKNOWN &&
          expression->right->resolved_named_symbol_id == AST_SYMBOL_NONE)))
        return;
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_AMPERSAND &&
        !assignable_expression(analyzer, expression->right)) {
        operand_error(analyzer, expression, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                      "Address-of requires a mutable lvalue");
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
            if (!numeric && !strings && !pointers && !named)
                operand_error(analyzer, expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                              "Equality comparison requires compatible operands");
        }
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
        if (!data || !data->resolved_pointer_depth || data->resolved_outer_pointer_depth || data->resolved_is_array ||
            data->resolved_is_slice ||
            (data->resolved_type == TYPE_VOID && data->resolved_pointer_depth == 1) || !integral_expression(length))
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
        if (strcmp(name, "true") != 0 && strcmp(name, "false") != 0 &&
            !is_builtin_name(name) && !(type >= TOKEN_TYPE_INT && type <= TOKEN_TYPE_VOID))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_UNDEFINED_VARIABLE,
                           "Undefined variable");
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        long long index = 0;
        if (constant_integer(analyzer, expression->right, &index) &&
            expression->left->resolved_symbol_id < analyzer->model->symbol_count) {
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

static int statement_always_returns(const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_MATCH && statement->match_exhaustive) {
            if (statement->is_type_match) {
                if (statement->selected_type_arm && statement_always_returns(statement->selected_type_arm->body)) return
                        1;
                continue;
            }
            int all = statement->match_arms != NULL;
            for (const AstMatchArm *a = statement->match_arms; a; a = a->next)
                if (!statement_always_returns(a->body)) all = 0;
            if (all) return 1;
        }
        if (statement->kind == AST_STMT_RETURN) return 1;
        if (statement->kind == AST_STMT_BLOCK && statement_always_returns(statement->body))
            return 1;
        if (statement->kind == AST_STMT_IF && statement->else_body != NULL &&
            statement_always_returns(statement->body) &&
            statement_always_returns(statement->else_body))
            return 1;
    }
    return 0;
}

int known_declared_type(const Analyzer *analyzer, const AstType *type) {
    if (type == NULL || type->kind == AST_TYPE_INFERRED ||
        primitive_type(analyzer->program, type) != TYPE_UNKNOWN)
        return 1;
    return resolve_named_symbol_id(analyzer, analyzer->program,
                                   named_type_token(analyzer->program, type)) != AST_SYMBOL_NONE;
}

static int aggregate_reaches(const Analyzer *analyzer, size_t current_symbol,
                             size_t target_symbol, size_t depth) {
    if (current_symbol >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count)
        return 1;
    const SemanticSymbol *current = &analyzer->model->symbols[current_symbol];
    if (current->kind == SEMANTIC_SYMBOL_ENUM && current->declaration != NULL) {
        for (const AstEnumValue *v = current->declaration->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                if (p->type.pointer_depth || p->type.outer_pointer_depth || p->type.is_slice) continue;
                size_t child = resolve_named_symbol_id(analyzer, current->source_program,
                                                       named_type_token(current->source_program, &p->type));
                if (child == target_symbol || (child != AST_SYMBOL_NONE && aggregate_reaches(
                                                   analyzer, child, target_symbol, depth + 1))) return 1;
            }
        return 0;
    }
    if (current->kind != SEMANTIC_SYMBOL_STRUCT || current->declaration == NULL) return 0;
    for (const AstField *field = current->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        if (field->type.pointer_depth != 0 || field->type.outer_pointer_depth != 0 ||
            field->type.is_slice)
            continue;
        size_t named = named_type_token(current->source_program, &field->type);
        size_t child = resolve_named_symbol_id(analyzer, current->source_program, named);
        if (child == target_symbol ||
            (child != AST_SYMBOL_NONE &&
             aggregate_reaches(analyzer, child, target_symbol, depth + 1U)))
            return 1;
    }
    return 0;
}

static size_t semantic_symbol_slots(const Analyzer *analyzer, size_t symbol_id,
                                    size_t depth) {
    if (symbol_id >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count)
        return SIZE_MAX;
    const SemanticSymbol *symbol = &analyzer->model->symbols[symbol_id];
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE) {
        size_t largest = 1;
        for (size_t i = 0; i < analyzer->model->symbol_count; i++)
            if (semantic_implements_interface(analyzer->model, symbol_id, i)) {
                size_t slots = semantic_symbol_slots(analyzer, i, depth + 1);
                if (slots > largest) largest = slots;
            }
        return largest == SIZE_MAX ? SIZE_MAX : largest + 1;
    }
    if (symbol->kind == SEMANTIC_SYMBOL_ENUM && symbol->declaration) {
        size_t largest = 0;
        for (const AstEnumValue *v = symbol->declaration->as.enum_decl.values; v; v = v->next) {
            size_t payload = 0;
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                size_t slots = p->type.is_slice && !p->type.outer_pointer_depth ? 2 : 1;
                if (!p->type.pointer_depth && !p->type.outer_pointer_depth && !p->type.is_slice) {
                    size_t child = resolve_named_symbol_id(analyzer, symbol->source_program,
                                                           named_type_token(symbol->source_program, &p->type));
                    if (child != AST_SYMBOL_NONE) slots = semantic_symbol_slots(analyzer, child, depth + 1);
                    if (p->type.is_array) {
                        size_t length = p->type.resolved_array_length;
                        if (!length) length = (size_t) strtoull(
                                         ast_program_lexeme(symbol->source_program, p->type.array_length_token), NULL,
                                         10);
                        if (slots == SIZE_MAX || (length && slots > SIZE_MAX / length)) return SIZE_MAX;
                        slots *= length;
                    }
                }
                if (slots > SIZE_MAX - payload) return SIZE_MAX;
                payload += slots;
            }
            if (payload > largest) largest = payload;
        }
        return largest == SIZE_MAX ? SIZE_MAX : largest + 1;
    }
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL) return 1;
    size_t slots = 0;
    for (const AstField *field = symbol->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        size_t field_slots = field->type.is_slice && !field->type.outer_pointer_depth ? 2 : 1;
        if (field->type.pointer_depth == 0 && field->type.outer_pointer_depth == 0 &&
            !field->type.is_slice) {
            size_t named = named_type_token(symbol->source_program, &field->type);
            size_t child = resolve_named_symbol_id(analyzer, symbol->source_program, named);
            if (child != AST_SYMBOL_NONE)
                field_slots = semantic_symbol_slots(analyzer, child, depth + 1U);
        }
        if (field->type.is_array &&
            field->type.array_length_token < symbol->source_program->token_count) {
            size_t length = field->type.resolved_array_length;
            if (length == 0)
                length = (size_t) strtoull(ast_program_lexeme(symbol->source_program,
                                                              field->type.array_length_token), NULL, 10);
            if (field_slots != 0 && length > SIZE_MAX / field_slots) return SIZE_MAX;
            field_slots *= length;
        }
        if (slots > SIZE_MAX - field_slots) return SIZE_MAX;
        slots += field_slots;
    }
    return slots == 0 ? 1 : slots;
}

static size_t semantic_type_slots(const Analyzer *analyzer, const AstProgram *program,
                                  const AstType *type,
                                  const AstExpression *inferred) {
    if (type->kind == AST_TYPE_INFERRED && inferred && inferred->resolved_is_slice && !inferred->
        resolved_outer_pointer_depth) return 2;
    if (type->outer_pointer_depth != 0 ||
        (type->pointer_depth != 0 && !type->is_array && !type->is_slice) || type->is_slice ||
        (type->kind == AST_TYPE_INFERRED && inferred != NULL &&
         (inferred->resolved_pointer_depth != 0 ||
          inferred->resolved_outer_pointer_depth != 0)))
        return type->is_slice && !type->outer_pointer_depth ? 2U : 1U;
    size_t slots = 1;
    size_t named_symbol = type->kind == AST_TYPE_INFERRED && inferred != NULL
                              ? inferred->resolved_named_symbol_id
                              : resolve_named_symbol_id(analyzer, program, named_type_token(program, type));
    if (type->pointer_depth != 0) slots = 1;
    else if (named_symbol != AST_SYMBOL_NONE)
        slots = semantic_symbol_slots(analyzer, named_symbol, 0);
    if (type->is_array && type->array_length_token < program->token_count) {
        size_t length = type->resolved_array_length;
        if (length == 0)
            length = (size_t) strtoull(ast_program_lexeme(program,
                                                          type->array_length_token), NULL, 10);
        if (slots != 0 && length > SIZE_MAX / slots) return SIZE_MAX;
        slots *= length;
    }
    return slots;
}

static int assignable_expression(const Analyzer *analyzer,
                                 const AstExpression *expression) {
    if (expression != NULL && expression->kind == AST_EXPR_MEMBER &&
        expression->left != NULL && expression->left->resolved_is_slice &&
        same_name(analyzer->program, expression->value_token, "length"))
        return 0;
    return expression != NULL &&
           !expression_is_constant_symbol(analyzer, expression) &&
           (expression->kind == AST_EXPR_NAME || expression->kind == AST_EXPR_INDEX ||
            expression->kind == AST_EXPR_MEMBER ||
            (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR));
}

void validate_array_shape(Analyzer *analyzer, AstType *type) {
    if (type == NULL || !type->is_array) return;
    if (type->array_length_token >= analyzer->program->token_count) {
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be a compile-time integer");
        return;
    }
    const char *text = ast_program_lexeme(analyzer->program, type->array_length_token);
    if (analyzer->program->tokens[type->array_length_token].type == TOKEN_IDENTIFIER) {
        const LocalSymbol *local = find_local(analyzer, type->array_length_token);
        const SemanticSymbol *symbol = local != NULL
                                           ? &analyzer->model->symbols[local->symbol_id]
                                           : scoped_find_global(analyzer->model, analyzer->program, text,
                                                                SEMANTIC_SYMBOL_CONSTANT);
        if (symbol && symbol->kind == SEMANTIC_SYMBOL_CONSTANT && symbol->declaration && !symbol->declaration->
            semantic_body_checked) {
            size_t id = symbol->id;
            AstProgram *saved = analyzer->program;
            analyzer->program = (AstProgram *) symbol->source_program;
            analyze_constant_declaration(analyzer, (AstDeclarationNode *) symbol->declaration);
            analyzer->program = saved;
            symbol = &analyzer->model->symbols[id];
        }
        const AstExpression *value = symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_CONSTANT
                                         ? constant_initializer(symbol)
                                         : NULL;
        if (value == NULL || !data_type_integral(value->resolved_type) || value->folded_constant.lexeme == NULL) {
            semantic_error(analyzer, type->array_length_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Array length must be a visible evaluated integral constant");
            return;
        }
        text = value->folded_constant.lexeme;
    }
    unsigned long long length = strtoull(text, NULL, 10);
    if (length == 0 || length > 1048576ULL)
        semantic_error(analyzer, type->array_length_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be positive and fit local storage");
    else type->resolved_array_length = (size_t) length;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FIELD && symbol->source_program == analyzer->program &&
            &((const AstField *) symbol->node)->type == type)
            symbol->declared_type = *type;
    }
}

static size_t layout_size(Analyzer *analyzer, AstType *type, size_t depth) {
    if (depth > analyzer->model->symbol_count + 64 || type->kind == AST_TYPE_INFERRED) return 0;
    if (type->outer_pointer_depth) return 8;
    if (type->is_slice) return 16;
    if (type->is_array) {
        validate_array_shape(analyzer, type);
        if (!type->resolved_array_length) return 0;
        AstType element = *type;
        element.is_array = 0;
        size_t size = layout_size(analyzer, &element, depth + 1);
        if (!size || size > SIZE_MAX - 7) return 0;
        size = (size + 7) & ~(size_t) 7;
        return type->resolved_array_length > SIZE_MAX / size ? 0 : size * type->resolved_array_length;
    }
    if (type->pointer_depth) return 8;
    DataType primitive = primitive_type(analyzer->program, type);
    if (primitive != TYPE_UNKNOWN) return primitive == TYPE_VOID ? 0 : data_type_bytes(primitive);
    size_t id = resolve_named_symbol_id(analyzer, analyzer->program, type->name_token);
    if (id == AST_SYMBOL_NONE) return 0;
    const SemanticSymbol *symbol = &analyzer->model->symbols[id];
    AstProgram *unit = (AstProgram *) symbol->source_program;
    AstDeclarationNode *declaration = (AstDeclarationNode *) symbol->declaration;
    if (!declaration) return 0;
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE) {
        size_t slots = semantic_symbol_slots(analyzer, id, depth + 1);
        return slots == SIZE_MAX || slots > SIZE_MAX / 8 ? 0 : slots * 8;
    }
    AstProgram *saved = analyzer->program;
    analyzer->program = unit;
    size_t size = 0;
    int valid = 1;
    if (declaration->kind == AST_DECL_STRUCT) {
        for (AstField *field = declaration->as.struct_decl.fields; field; field = field->next) {
            size_t bytes = layout_size(analyzer, &field->type, depth + 1);
            if (!bytes || bytes > SIZE_MAX - 7) {
                valid = 0;
                break;
            }
            bytes = (bytes + 7) & ~(size_t) 7;
            if (bytes > SIZE_MAX - size) {
                valid = 0;
                break;
            }
            size += bytes;
        }
        if (!size) size = 8;
    } else if (declaration->kind == AST_DECL_ENUM) {
        size_t largest = 0;
        for (AstEnumValue *variant = declaration->as.enum_decl.values; variant; variant = variant->next) {
            size_t payload = 0;
            for (AstTypeArgument *argument = variant->payload_types; argument; argument = argument->next) {
                size_t bytes = layout_size(analyzer, &argument->type, depth + 1);
                if (!bytes || bytes > SIZE_MAX - 7) {
                    valid = 0;
                    break;
                }
                bytes = (bytes + 7) & ~(size_t) 7;
                if (bytes > SIZE_MAX - payload) {
                    valid = 0;
                    break;
                }
                payload += bytes;
            }
            if (payload > largest) largest = payload;
        }
        if (largest > SIZE_MAX - 8) valid = 0;
        else size = largest + 8;
    } else valid = 0;
    analyzer->program = saved;
    return valid ? size : 0;
}

static void metadata_name(DiagnosticText *text, const Analyzer *analyzer, const AstProgram *unit, const AstType *type,
                          unsigned depth) {
    if (depth > 64) {
        text->failed = 1;
        return;
    }
    for (unsigned i = 0; i < type->outer_pointer_depth; i++) diagnostic_append(text, "*");
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, "(");
    for (unsigned i = 0; i < type->pointer_depth; i++) diagnostic_append(text, "*");
    size_t id = resolve_named_symbol_id(analyzer, unit, named_type_token(unit, type));
    if (id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[id];
        const AstDeclarationNode *decl = symbol->declaration;
        const AstProgram *owner = symbol->source_program;
        diagnostic_append(text, "%s.%s", owner->module_identity,
                          ast_program_lexeme(owner, decl && decl->generic_origin
                                                        ? decl->generic_origin->name_token
                                                        : symbol->name_token));
        if (decl && decl->specialization_arguments) {
            diagnostic_append(text, "<");
            for (const AstTypeArgument *a = decl->specialization_arguments; a; a = a->next) {
                metadata_name(text, analyzer, owner, &a->type, depth + 1);
                if (a->next) diagnostic_append(text, ",");
            }
            diagnostic_append(text, ">");
        }
    } else diagnostic_append(text, "%s", ast_program_lexeme(unit, type->name_token));
    if (type->is_slice) diagnostic_append(text, "[]");
    if (type->is_array) diagnostic_append(text, "[%zu]", type->resolved_array_length);
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, ")");
}

static void analyze_expression(Analyzer *analyzer, AstExpression *expression) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_TYPE_INFO && !expression->left &&
        !expression->allocated_type.pointer_depth && !expression->allocated_type.outer_pointer_depth &&
        !expression->allocated_type.is_array && !expression->allocated_type.is_slice && !expression->allocated_type.
        arguments) {
        const SemanticSymbol *global = scoped_find_global(analyzer->model, analyzer->program,
                                                          ast_program_lexeme(
                                                              analyzer->program, expression->value_token),
                                                          SEMANTIC_SYMBOL_VARIABLE);
        if (!global)
            global = scoped_find_global(analyzer->model, analyzer->program,
                                        ast_program_lexeme(analyzer->program, expression->value_token),
                                        SEMANTIC_SYMBOL_CONSTANT);
        if (global || find_local(analyzer, expression->value_token)) {
            expression->kind = AST_EXPR_NAME;
            expression->allocated_type = (AstType)
            {
                .kind = AST_TYPE_INFERRED,.name_token = AST_TOKEN_NONE
            };
        }
    }
    if (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind == AST_EXPR_NAME &&
        !find_local(analyzer, expression->left->value_token)) {
        const char *alias = ast_program_lexeme(analyzer->program, expression->left->value_token);
        for (AstDeclarationNode *d = analyzer->program->root; d; d = d->next)
            if (d->kind == AST_DECL_IMPORT)
                for (AstImportPath *p = d->as.import_decl.paths; p; p = p->next)
                    if (p->alias && !strcmp(alias, p->alias)) {
                        char qualified[2048];
                        snprintf(qualified, sizeof(qualified), "%s.%s", alias,
                                 ast_program_lexeme(analyzer->program, expression->value_token));
                        expression->value_token = concrete_token(analyzer, TOKEN_IDENTIFIER, qualified);
                        expression->kind = AST_EXPR_NAME;
                        expression->left = NULL;
                    }
    }
    analyze_expression(analyzer, expression->left);
    analyze_expression(analyzer, expression->right);
    for (AstExpression *argument = expression->arguments; argument != NULL; argument = argument->next)
        analyze_expression(analyzer, argument);

    if (expression->kind == AST_EXPR_ENUM_ACCESS) return;

    if (expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        const AstExpression *left = expression->left;
        if (left && left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
            expression->resolved_symbol_id = left->resolved_symbol_id;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        } else if (left) {
            const AstEnumValue *variant = find_enum_value_by_symbol(analyzer, left->resolved_named_symbol_id,
                                                                    expression->value_token);
            expression->resolved_symbol_id = variant ? variant->resolved_symbol_id : AST_SYMBOL_NONE;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        }
        return;
    }
    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_pointer_depth = 0;
    expression->resolved_outer_pointer_depth = 0;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_is_array = 0;
    expression->resolved_is_slice = 0;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    if (expression->kind == AST_EXPR_MEMBER && expression->left && same_name(
            analyzer->program, expression->value_token, "type")) {
        if (expression->left->kind == AST_EXPR_TYPE_INFO ||
            (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
             (analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
              analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM)))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Types expose name, size and align directly; .type is for values");
        expression->kind = AST_EXPR_TYPE_INFO;
        expression->allocated_type = inferred_argument_type(analyzer, expression->left);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Value type metadata requires a known static type");
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_TYPE_INFO) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Type metadata requires a known type");
        expression->resolved_type = TYPE_VOID; /* Compile-time entity, never an IR value. */
    } else if (expression->kind == AST_EXPR_TYPE_PROPERTY ||
               (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind ==
                AST_EXPR_TYPE_INFO)) {
        expression->kind = AST_EXPR_TYPE_PROPERTY;
        const char *property = ast_program_lexeme(analyzer->program, expression->value_token);
        AstType *type = &expression->left->allocated_type;
        if (!strcmp(property, "name")) {
            DiagnosticText text = {0};
            metadata_name(&text, analyzer, analyzer->program, type, 0);
            if (text.failed) analyzer->allocation_failed = 1;
            else expression->folded_constant = (AstToken)
            {
                .type = TOKEN_STRING_LITERAL,
                .lexeme = string_interner_intern(analyzer->program->strings, text.text),.span = expression->span
            };
            free(text.text);
            expression->resolved_type = TYPE_STRING;
        } else if (!strcmp(property, "size") || !strcmp(property, "align")) {
            DataType primitive = primitive_type(analyzer->program, type);
            int is_void = primitive == TYPE_VOID && !type->pointer_depth && !type->outer_pointer_depth && !type->
                          is_array && !type->is_slice;
            size_t size = is_void ? 0 : layout_size(analyzer, type, 0);
            if ((!size && !is_void) || size > INT64_MAX)
                semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Type metadata layout requires a complete, non-recursive sized type");
            size_t align = is_void
                               ? 1
                               : (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->
                                  is_slice && primitive != TYPE_UNKNOWN)
                                     ? data_type_bytes(primitive)
                                     : 8;
            char text[32];
            snprintf(text, sizeof(text), "%zu", !strcmp(property, "size") ? size : align);
            expression->folded_constant = (AstToken)
            {
                .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
                span = expression->span
            };
            expression->resolved_type = TYPE_USIZE;
        } else semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_FIELD_NOT_FOUND,
                              "Unknown type metadata property; expected name, size or align");
    } else if (expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        size_t size = layout_size(analyzer, &expression->allocated_type, 0);
        expression->resolved_type = TYPE_USIZE;
        if (!size || size > (size_t) INT64_MAX) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Layout query requires a complete, non-recursive, sized non-void type");
            return;
        }
        const AstType *type = &expression->allocated_type;
        DataType primitive = primitive_type(analyzer->program, type);
        size_t alignment = (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->is_slice &&
                            primitive != TYPE_UNKNOWN)
                               ? data_type_bytes(primitive)
                               : 8;
        char text[32];
        snprintf(text, sizeof(text), "%zu", expression->kind == AST_EXPR_SIZEOF ? size : alignment);
        expression->folded_constant = (AstToken)
        {
            .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
            span = expression->span
        };
    } else if (expression->kind == AST_EXPR_SLICE) {
        const AstExpression *data = expression->left;
        if (data) {
            expression->resolved_type = data->resolved_type;
            expression->resolved_named_type_token = data->resolved_named_type_token;
            expression->resolved_named_symbol_id = data->resolved_named_symbol_id;
            expression->resolved_pointer_depth = data->resolved_pointer_depth ? data->resolved_pointer_depth - 1 : 0;
            expression->resolved_is_slice = 1;
        }
    } else if (expression->kind == AST_EXPR_LITERAL) {
        TokenType token = expression->value_token < analyzer->program->token_count
                              ? analyzer->program->tokens[expression->value_token].type
                              : TOKEN_ERROR;
        if (token == TOKEN_NUMBER) {
            unsigned long long value = strtoull(ast_program_lexeme(analyzer->program, expression->value_token), NULL,
                                                10);
            expression->resolved_type = value > INT64_MAX ? TYPE_U64 : value > INT32_MAX ? TYPE_I64 : TYPE_INT;
        } else if (token == TOKEN_FLOAT_LITERAL) expression->resolved_type = TYPE_DOUBLE;
        else if (token == TOKEN_CHAR_LITERAL) expression->resolved_type = TYPE_CHAR;
        else if (token == TOKEN_STRING_LITERAL) expression->resolved_type = TYPE_STRING;
    } else if (expression->kind == AST_EXPR_NAME) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        TokenType name_type = expression->value_token < analyzer->program->token_count
                                  ? analyzer->program->tokens[expression->value_token].type
                                  : TOKEN_ERROR;
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
            const CoreIntrinsic *core = core_intrinsic_find(name);
            if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
        } else {
            const LocalSymbol *local = find_local(analyzer, expression->value_token);
            if (local != NULL) {
                expression->resolved_symbol_id = local->symbol_id;
                expression->resolved_type = local->resolved_type;
                expression->resolved_pointer_depth = local->resolved_pointer_depth;
                expression->resolved_outer_pointer_depth = local->resolved_outer_pointer_depth;
                expression->resolved_named_type_token = local->resolved_named_type_token;
                expression->resolved_named_symbol_id = local->resolved_named_symbol_id;
                expression->resolved_is_array = local->resolved_is_array;
                expression->resolved_is_slice = local->resolved_is_slice;
            } else {
                const AstField *implicit_field = analyzer->current_owner_token == AST_TOKEN_NONE
                                                     ? NULL
                                                     : find_field(analyzer, analyzer->current_owner_token,
                                                                  expression->value_token);
                const SemanticSymbol *structure = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                     SEMANTIC_SYMBOL_STRUCT);
                const SemanticSymbol *enumeration = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                       SEMANTIC_SYMBOL_ENUM);
                const SemanticSymbol *function = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_FUNCTION);
                const SemanticSymbol *constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_CONSTANT);
                if (!constant) constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                             SEMANTIC_SYMBOL_VARIABLE);
                if (implicit_field != NULL) {
                    expression->resolved_symbol_id = implicit_field->resolved_symbol_id;
                    expression->resolved_type = primitive_type(analyzer->program,
                                                               &implicit_field->type);
                    expression->resolved_pointer_depth = implicit_field->type.pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            implicit_field->type.outer_pointer_depth;
                    expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                             &implicit_field->type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        analyzer->program, expression->resolved_named_type_token);
                    expression->resolved_is_array = implicit_field->type.is_array;
                    expression->resolved_is_slice = implicit_field->type.is_slice;
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
                    expression->resolved_type = primitive_type(function->source_program,
                                                               &function->declared_type);
                } else if (constant != NULL) {
                    size_t constant_id = constant->id;
                    if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && !constant->declaration->
                        semantic_body_checked) {
                        AstProgram *saved = analyzer->program;
                        analyzer->program = (AstProgram *) constant->source_program;
                        analyze_constant_declaration(analyzer, (AstDeclarationNode *) constant->declaration);
                        analyzer->program = saved;
                        constant = &analyzer->model->symbols[constant_id];
                    } else if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && constant->
                               declaration->semantic_body_checked == 2) {
                        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE,
                                       ERR_TYPE_INVALID_OPERATION, "Constant initializer contains a dependency cycle");
                    }
                    expression->resolved_symbol_id = constant->id;
                    expression->resolved_type = constant->resolved_type;
                    expression->resolved_pointer_depth = constant->resolved_pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            constant->resolved_outer_pointer_depth;
                    expression->resolved_named_type_token =
                            constant->resolved_named_type_token;
                    expression->resolved_named_symbol_id = constant->resolved_named_symbol_id;
                    expression->resolved_is_array = constant->resolved_is_array;
                    expression->resolved_is_slice = constant->resolved_is_slice;
                }
            }
        }
    } else if (expression->kind == AST_EXPR_UNARY) {
        if (expression->operator_type == TOKEN_BANG) expression->resolved_type = TYPE_BIT;
        else if (expression->right != NULL) {
            expression->resolved_type = expression->right->resolved_type;
            expression->resolved_pointer_depth = expression->right->resolved_pointer_depth;
            expression->resolved_outer_pointer_depth =
                    expression->right->resolved_outer_pointer_depth;
            expression->resolved_named_type_token = expression->right->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->right->resolved_named_symbol_id;
            expression->resolved_is_array = expression->right->resolved_is_array;
            expression->resolved_is_slice = expression->right->resolved_is_slice;
            if (expression->operator_type == TOKEN_AMPERSAND) {
                if (expression->resolved_is_array || expression->resolved_is_slice)
                    expression->resolved_outer_pointer_depth++;
                else expression->resolved_pointer_depth++;
            } else if (expression->operator_type == TOKEN_STAR) {
                if (expression->resolved_outer_pointer_depth > 0)
                    expression->resolved_outer_pointer_depth--;
                else if (expression->resolved_pointer_depth > 0)
                    expression->resolved_pointer_depth--;
            }
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
            else if (expression->left->resolved_type == TYPE_FLOAT &&
                     expression->right->resolved_type == TYPE_DOUBLE &&
                     expression->right->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else if (expression->right->resolved_type == TYPE_FLOAT &&
                     expression->left->resolved_type == TYPE_DOUBLE &&
                     expression->left->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else
                expression->resolved_type = promoted_numeric(expression->left->resolved_type,
                                                             expression->right->resolved_type);
        }
    } else if (expression->kind == AST_EXPR_CAST) {
        const AstType *cast_type = &expression->allocated_type;
        expression->resolved_type = primitive_type(analyzer->program, cast_type);
        expression->resolved_pointer_depth = cast_type->pointer_depth;
        expression->resolved_outer_pointer_depth = cast_type->outer_pointer_depth;
        expression->resolved_named_type_token = named_type_token(analyzer->program, cast_type);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer, analyzer->program,
                                                                       expression->resolved_named_type_token);
        expression->resolved_is_array = cast_type->is_array;
        expression->resolved_is_slice = cast_type->is_slice;
    } else if (expression->kind == AST_EXPR_FREE) {
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_CALL) {
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program, expression->left->value_token);
            TokenType callee_type = expression->left->value_token < analyzer->program->token_count
                                        ? analyzer->program->tokens[expression->left->value_token].type
                                        : TOKEN_ERROR;
            AstType cast_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->left->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            if (callee_type >= TOKEN_TYPE_INT && callee_type <= TOKEN_TYPE_VOID) {
                expression->resolved_type = primitive_type(analyzer->program, &cast_type);
            } else {
                int ambiguous = 0;
                const SemanticSymbol *function;
                if (expression->left->explicit_type_arguments) function = explicit_generic_function(
                                                                   analyzer, name, expression);
                else {
                    instantiate_generic_candidates(analyzer, name, expression->arguments);
                    function = resolve_overload(analyzer, name, AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
                }
                (void) ambiguous;
                if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    expression->resolved_type = primitive_type(function->source_program,
                                                               &function->declared_type);
                    expression->resolved_pointer_depth = function->declared_type.pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            function->declared_type.outer_pointer_depth;
                    expression->resolved_named_type_token = named_type_token(function->source_program,
                                                                             &function->declared_type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        function->source_program, expression->resolved_named_type_token);
                    expression->resolved_is_array = function->declared_type.is_array;
                    expression->resolved_is_slice = function->declared_type.is_slice;
                } else if (is_builtin_name(name)) {
                    expression->resolved_type = builtin_result_type(name);
                    if (strcmp(name, "malloc") == 0)
                        expression->resolved_pointer_depth = 1;
                    const CoreIntrinsic *core = core_intrinsic_find(name);
                    if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
                    if (strcmp(name, "read") == 0 && expression->arguments != NULL &&
                        expression->arguments->next != NULL) {
                        const AstExpression *format = expression->arguments->next;
                        const char *format_text = format->kind == AST_EXPR_LITERAL
                                                      ? ast_program_lexeme(analyzer->program, format->value_token)
                                                      : "";
                        expression->resolved_type = strcmp(format_text, "%c") == 0
                                                        ? TYPE_CHAR
                                                        : TYPE_INT;
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
            const AstExpression *receiver = expression->left->left;
            int enum_type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                     analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                     SEMANTIC_SYMBOL_ENUM;
            const AstEnumValue *access_variant = !enum_type_receiver &&
                                                 receiver->resolved_pointer_depth == 0 && !receiver->resolved_is_array
                                                 && !receiver->resolved_is_slice
                                                     ? find_enum_value_by_symbol(
                                                         analyzer, receiver->resolved_named_symbol_id,
                                                         expression->left->value_token)
                                                     : NULL;
            if (access_variant != NULL) {
                const SemanticSymbol *variant = &analyzer->model->symbols[access_variant->resolved_symbol_id];
                const AstTypeArgument *payload = access_variant->payload_types;
                expression->kind = AST_EXPR_ENUM_ACCESS;
                expression->resolved_symbol_id = variant->id;
                if (!payload || payload->next) {
                    semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Enum payload accessor requires a variant with exactly one payload");
                    return;
                }
                expression->resolved_type = primitive_type(variant->source_program, &payload->type);
                expression->resolved_pointer_depth = payload->type.pointer_depth;
                expression->resolved_outer_pointer_depth = payload->type.outer_pointer_depth;
                expression->resolved_named_type_token = named_type_token(variant->source_program, &payload->type);
                expression->resolved_named_symbol_id = resolve_named_symbol_id(
                    analyzer, variant->source_program, expression->resolved_named_type_token);
                expression->resolved_is_array = payload->type.is_array;
                expression->resolved_is_slice = payload->type.is_slice;
                return;
            }
            if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
                expression->resolved_symbol_id = expression->left->resolved_symbol_id;
                expression->resolved_named_symbol_id = receiver->resolved_named_symbol_id;
                expression->resolved_named_type_token = receiver->resolved_named_type_token;
                return;
            }
            int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_STRUCT ||
                                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_ENUM);
            int ambiguous = 0;
            const SemanticSymbol *method = resolve_overload(analyzer,
                                                            ast_program_lexeme(
                                                                analyzer->program, expression->left->value_token),
                                                            receiver->resolved_named_symbol_id, type_receiver,
                                                            expression->arguments, &ambiguous);
            (void) ambiguous;
            if (method != NULL) {
                expression->resolved_symbol_id = method->id;
                expression->resolved_type = primitive_type(method->source_program,
                                                           &method->declared_type);
                expression->resolved_pointer_depth = method->declared_type.pointer_depth;
                expression->resolved_outer_pointer_depth =
                        method->declared_type.outer_pointer_depth;
                expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                         &method->declared_type);
                expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                               method->source_program,
                                                                               expression->resolved_named_type_token);
                expression->resolved_is_array = method->declared_type.is_array;
                expression->resolved_is_slice = method->declared_type.is_slice;
            }
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        expression->resolved_type = expression->left->resolved_type;
        expression->resolved_pointer_depth = expression->left->resolved_pointer_depth;
        expression->resolved_outer_pointer_depth =
                expression->left->resolved_outer_pointer_depth;
        expression->resolved_named_type_token = expression->left->resolved_named_type_token;
        expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
        expression->resolved_is_array = expression->left->resolved_is_array;
        expression->resolved_is_slice = expression->left->resolved_is_slice;
        if (expression->resolved_outer_pointer_depth > 0)
            expression->resolved_outer_pointer_depth--;
        else if (expression->resolved_is_array || expression->resolved_is_slice) {
            expression->resolved_is_array = 0;
            expression->resolved_is_slice = 0;
        } else if (expression->resolved_pointer_depth > 0)
            expression->resolved_pointer_depth--;
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL) {
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "length")) {
            expression->kind = AST_EXPR_SLICE_LENGTH;
            expression->resolved_type = TYPE_USIZE;
            return;
        }
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "data")) {
            expression->kind = AST_EXPR_SLICE_DATA;
            expression->resolved_type = expression->left->resolved_type;
            expression->resolved_pointer_depth = expression->left->resolved_pointer_depth + 1;
            expression->resolved_named_type_token = expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
            return;
        }
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
            const SemanticSymbol *enum_symbol = &analyzer->model->symbols[expression->resolved_named_symbol_id];
            if (enum_symbol->declaration->as.enum_decl.is_sum && value->payload_types == NULL)
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
            return;
        }
        const SemanticSymbol *method = find_method(analyzer,
                                                   expression->left->resolved_named_symbol_id, expression->value_token);
        if (method != NULL) {
            expression->resolved_symbol_id = method->id;
            expression->resolved_type = primitive_type(method->source_program,
                                                       &method->declared_type);
            expression->resolved_pointer_depth = method->declared_type.pointer_depth;
            expression->resolved_outer_pointer_depth =
                    method->declared_type.outer_pointer_depth;
            expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                     &method->declared_type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                           method->source_program,
                                                                           expression->resolved_named_type_token);
            return;
        }
        const AstField *field = find_field_by_symbol(analyzer,
                                                     expression->left->resolved_named_symbol_id,
                                                     expression->value_token);
        if (field != NULL) {
            const SemanticSymbol *field_symbol = field->resolved_symbol_id <
                                                 analyzer->model->symbol_count
                                                     ? &analyzer->model->symbols[field->resolved_symbol_id]
                                                     : NULL;
            const AstProgram *field_program = field_symbol == NULL
                                                  ? analyzer->program
                                                  : field_symbol->source_program;
            expression->resolved_type = primitive_type(field_program, &field->type);
            expression->resolved_pointer_depth = field->type.pointer_depth;
            expression->resolved_outer_pointer_depth = field->type.outer_pointer_depth;
            expression->resolved_named_type_token = named_type_token(field_program, &field->type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                           field_program,
                                                                           expression->resolved_named_type_token);
            expression->resolved_is_array = field->type.is_array;
            expression->resolved_is_slice = field->type.is_slice;
            expression->resolved_symbol_id = field->resolved_symbol_id;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        validate_array_shape(analyzer, &expression->allocated_type);
        const AstType *reserved_type = &expression->allocated_type;
        expression->resolved_type = primitive_type(analyzer->program, reserved_type);
        expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                 reserved_type);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                       analyzer->program,
                                                                       expression->resolved_named_type_token);
        expression->resolved_pointer_depth = reserved_type->pointer_depth;
        expression->resolved_outer_pointer_depth = reserved_type->outer_pointer_depth;
        expression->resolved_is_array = reserved_type->is_array;
        expression->resolved_is_slice = reserved_type->is_slice;
        if (reserved_type->is_array || reserved_type->is_slice)
            expression->resolved_outer_pointer_depth++;
        else expression->resolved_pointer_depth++;
    }
    if (expression->kind == AST_EXPR_UNARY && expression->right != NULL)
        expression->resolved_array_length = expression->right->resolved_array_length;
    else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL)
        expression->resolved_array_length = expression->resolved_is_array ? expression->left->resolved_array_length : 0;
    else if (expression->kind == AST_EXPR_RESERVE)
        expression->resolved_array_length = expression->allocated_type.resolved_array_length;
    else if (expression->resolved_symbol_id < analyzer->model->symbol_count)
        expression->resolved_array_length = analyzer->model->symbols[expression->resolved_symbol_id].declared_type.
                resolved_array_length;
    if (expression->resolved_type == TYPE_UNKNOWN &&
        expression->resolved_named_type_token == AST_TOKEN_NONE &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE &&
        expression->kind != AST_EXPR_RESERVE &&
        !(expression->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program, expression->value_token))) &&
        !(expression->kind == AST_EXPR_CALL && expression->left != NULL &&
          expression->left->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program,
                                             expression->left->value_token))))
        analyzer->model->unresolved_expression_count++;
}

static void analyze_statement(Analyzer *analyzer, AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_MATCH) {
            analyze_expression(analyzer, statement->value);
            if (statement->value && statement->value->kind == AST_EXPR_TYPE_INFO) {
                validate_expression(analyzer, statement->value, 1);
                statement->is_type_match = 1;
                statement->selected_type_arm = NULL;
                int wildcard = 0;
                for (AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                    if (wildcard) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                 ERR_SEM_INVALID_DECLARATION,
                                                 "Unreachable type match arm after wildcard");
                    if (arm->bindings) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                      ERR_SEM_INVALID_DECLARATION,
                                                      "Type match cannot bind enum payloads");
                    if (arm->wildcard) {
                        wildcard = 1;
                        if (!statement->selected_type_arm) statement->selected_type_arm = arm;
                        continue;
                    }
                    if (!arm->is_type_pattern) {
                        semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_PARSER, ERR_PARSE_INVALID_SYNTAX,
                                       "Type match requires case Type -> statement");
                        continue;
                    }
                    normalize_generic_type(analyzer, &arm->type, 0);
                    if (!known_declared_type(analyzer, &arm->type))
                        semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown type match pattern");
                    for (AstMatchArm *previous = statement->match_arms; previous != arm; previous = previous->next)
                        if (previous->is_type_pattern && !previous->wildcard && ast_concrete_type_equal(
                                analyzer->program, &previous->type, analyzer->program, &arm->type))
                            semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                           ERR_SEM_DUPLICATE_DEFINITION, "Duplicate type match case");
                    if (!statement->selected_type_arm && ast_concrete_type_equal(
                            analyzer->program, &statement->value->allocated_type, analyzer->program,
                            &arm->type)) statement->selected_type_arm = arm;
                }
                statement->match_exhaustive = statement->selected_type_arm != NULL;
                if (!statement->match_exhaustive)
                    semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_SEMANTIC,
                                   ERR_SEM_INVALID_DECLARATION, "Type match has no matching case; add a wildcard");
                else {
                    LocalSymbol *saved = analyzer->locals;
                    analyzer->scope_depth++;
                    normalize_statement_types(analyzer, statement->selected_type_arm->body);
                    analyze_statement(analyzer, statement->selected_type_arm->body);
                    analyzer->scope_depth--;
                    pop_to(analyzer, saved);
                }
                continue;
            }
            validate_expression(analyzer, statement->value, 0);
            size_t nominal = statement->value ? statement->value->resolved_named_symbol_id : AST_SYMBOL_NONE;
            const SemanticSymbol *enum_symbol = nominal < analyzer->model->symbol_count
                                                    ? &analyzer->model->symbols[nominal]
                                                    : NULL;
            if (!enum_symbol || enum_symbol->kind != SEMANTIC_SYMBOL_ENUM || pointer_expression(statement->value)) {
                semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "match requires an enum value");
                continue;
            }
            const AstDeclarationNode *enumeration = enum_symbol->declaration;
            const AstProgram *enum_unit = enum_symbol->source_program;
            size_t variants = 0, covered = 0;
            int wildcard = 0;
            for (const AstEnumValue *v = enumeration->as.enum_decl.values; v; v = v->next) variants++;
            for (AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                if (arm->is_type_pattern) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_TYPE,
                                                         ERR_TYPE_INVALID_OPERATION,
                                                         "Type cases require a type metadata match, not an enum value");
                if (wildcard || covered == variants)
                    semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                                   "Unreachable match arm");
                LocalSymbol *saved = analyzer->locals;
                analyzer->scope_depth++;
                if (arm->wildcard) {
                    wildcard = 1;
                    if (arm->bindings) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_PARSER,
                                                      ERR_PARSE_INVALID_SYNTAX,
                                                      "Wildcard pattern cannot bind payloads");
                } else {
                    const AstEnumValue *v = find_enum_value_by_symbol(analyzer, nominal, arm->variant_token);
                    if (!v) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                           ERR_SEM_FIELD_NOT_FOUND, "Unknown enum match variant");
                    else {
                        if (!same_package(analyzer->program, enum_unit) && !v->is_public)
                            semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                                           "Enum variant is private to its defining package");
                        arm->resolved_variant_symbol = v->resolved_symbol_id;
                        int duplicate = 0;
                        for (AstMatchArm *previous = statement->match_arms; previous != arm; previous = previous->next)
                            if (previous->resolved_variant_symbol == arm->resolved_variant_symbol) duplicate = 1;
                        if (duplicate) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                      ERR_SEM_DUPLICATE_DEFINITION, "Duplicate match variant");
                        else covered++;
                        const AstTypeArgument *p = v->payload_types;
                        AstParameter *binding = arm->bindings;
                        for (; p && binding; p = p->next, binding = binding->next) {
                            binding->type = argument_type_copy(analyzer, enum_unit, p->type);
                            validate_array_shape(analyzer, &binding->type);
                            for (const LocalSymbol *existing = analyzer->locals; existing != saved;
                                 existing = existing->next)
                                if (same_name(analyzer->program, existing->name_token,
                                              ast_program_lexeme(analyzer->program, binding->name_token)))
                                    semantic_error(analyzer, binding->name_token, ERROR_CATEGORY_SEMANTIC,
                                                   ERR_SEM_DUPLICATE_DEFINITION, "Duplicate pattern binding");
                            LocalSymbol *local = push_local(analyzer, binding->name_token, binding->type,
                                                            SEMANTIC_SYMBOL_LOCAL, NULL, 0);
                            if (local) binding->resolved_symbol_id = local->symbol_id;
                        }
                        if (p || binding) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                         ERR_SEM_WRONG_ARG_COUNT,
                                                         "Match pattern payload binding count mismatch");
                    }
                }
                normalize_statement_types(analyzer, arm->body);
                analyze_statement(analyzer, arm->body);
                analyzer->scope_depth--;
                pop_to(analyzer, saved);
            }
            statement->match_exhaustive = wildcard || covered == variants;
            if (!statement->match_exhaustive) semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_SEMANTIC,
                                                             ERR_SEM_INVALID_DECLARATION,
                                                             "Non-exhaustive enum match requires every variant or a wildcard");
            continue;
        }

        LocalSymbol *scope = analyzer->locals;
        if (statement->kind == AST_STMT_VARIABLE) {
            size_t errors_before = analyzer->model->error_count;
            validate_array_shape(analyzer, &statement->type);
            analyze_expression(analyzer, statement->value);
            validate_expression(analyzer, statement->value, 0);
            if (!known_declared_type(analyzer, &statement->type))
                semantic_error(analyzer, statement->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                               "Unknown variable type");
            if (statement->type.kind == AST_TYPE_INFERRED && statement->value == NULL)
                semantic_error(analyzer, statement->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                               "Inferred variable requires an initializer");
            if (statement->is_const && statement->value == NULL)
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Constant requires an initializer");
            if (statement->is_const && statement->value != NULL &&
                !constant_expression_allowed(analyzer, statement->value))
                semantic_error(analyzer, statement->value->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Constant initializer is not a constant expression");
            if (statement->is_const &&
                (statement->type.pointer_depth != 0 ||
                 statement->type.outer_pointer_depth != 0 || statement->type.is_array ||
                 statement->type.is_slice ||
                 (statement->value != NULL &&
                  statement->value->resolved_named_symbol_id != AST_SYMBOL_NONE)))
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Constants require a primitive or string type");
            if (primitive_type(analyzer->program, &statement->type) == TYPE_VOID &&
                statement->type.pointer_depth == 0)
                semantic_error(analyzer, statement->type.name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Variable cannot have type void");
            if (statement->type.kind != AST_TYPE_INFERRED && statement->value != NULL) {
                if (!expression_to_declared_type_allowed(analyzer, statement->value,
                                                         analyzer->program,
                                                         &statement->type))
                    conversion_error(analyzer, statement->value, analyzer->program, &statement->type,
                                     NULL, "Cannot implicitly convert initializer");
            }
            if (statement->type.is_array && statement->type.outer_pointer_depth == 0 && statement->value != NULL)
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Arrays cannot be initialized by assignment");
            if (statement->is_const && statement->value != NULL &&
                analyzer->model->error_count == errors_before &&
                constant_expression_allowed(analyzer, statement->value))
                (void) fold_constant(analyzer, statement->value,
                                     statement->type.kind == AST_TYPE_INFERRED
                                         ? statement->value->resolved_type
                                         : primitive_type(analyzer->program, &statement->type));
            if (same_name(analyzer->program, statement->name_token, "true") ||
                same_name(analyzer->program, statement->name_token, "false"))
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Reserved name cannot be declared");
            if (statement->type.is_array &&
                statement->type.array_length_token < analyzer->program->token_count) {
                unsigned long long length = statement->type.resolved_array_length;
                if (length == 0 || length > 1048576ULL)
                    semantic_error(analyzer, statement->type.array_length_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Array length must be positive and fit local storage");
            }
            size_t slots = semantic_type_slots(analyzer, analyzer->program,
                                               &statement->type, statement->value);
            size_t storage_limit = 8U * 1024U * 1024U;
            if (slots == SIZE_MAX || slots > storage_limit / 8U || analyzer->local_storage >
                storage_limit - slots * 8U) {
                if (!analyzer->storage_error_reported)
                    semantic_error(analyzer, statement->name_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_STORAGE_LIMIT,
                                   "Function local storage exceeds supported limit");
                analyzer->storage_error_reported = 1;
            } else analyzer->local_storage += slots * 8U;
            for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
                 existing = existing->next) {
                if (existing->scope_depth == analyzer->scope_depth &&
                    same_name(analyzer->program, existing->name_token,
                              ast_program_lexeme(analyzer->program, statement->name_token))) {
                    semantic_duplicate(analyzer, statement->name_token, analyzer->program, existing->name_token,
                                       "Duplicate variable");
                    break;
                }
            }
            LocalSymbol *local = push_local(analyzer, statement->name_token, statement->type,
                                            statement->is_const ? SEMANTIC_SYMBOL_CONSTANT : SEMANTIC_SYMBOL_LOCAL,
                                            statement->value, statement->is_const);
            if (local != NULL) statement->resolved_symbol_id = local->symbol_id;
        } else if (statement->kind == AST_STMT_FOR) {
            analyzer->scope_depth++;
            analyze_statement(analyzer, statement->initializer);
            analyze_expression(analyzer, statement->condition);
            validate_expression(analyzer, statement->condition, 0);
            if (statement->condition != NULL &&
                !plain_numeric_expression(statement->condition))
                operand_error(analyzer, statement->condition,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Condition requires a numeric or bit expression");
            analyzer->loop_depth++;
            analyze_statement(analyzer, statement->body);
            analyzer->loop_depth--;
            analyze_statement(analyzer, statement->else_body);
            pop_to(analyzer, scope);
            analyzer->scope_depth--;
        } else {
            analyze_expression(analyzer, statement->expression);
            analyze_expression(analyzer, statement->value);
            analyze_expression(analyzer, statement->condition);
            analyze_expression(analyzer, statement->update);
            validate_expression(analyzer, statement->expression, 0);
            validate_expression(analyzer, statement->value, 0);
            validate_expression(analyzer, statement->condition, 0);
            validate_expression(analyzer, statement->update, 0);
            if ((statement->kind == AST_STMT_IF || statement->kind == AST_STMT_WHILE) &&
                statement->condition != NULL &&
                !plain_numeric_expression(statement->condition))
                operand_error(analyzer, statement->condition,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Condition requires a numeric or bit expression");
            if (statement->kind == AST_STMT_ASSIGNMENT &&
                !assignable_expression(analyzer, statement->expression))
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Unexpected statement: Assignment requires an assignable target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->value != NULL &&
                !expression_assignment_allowed(analyzer, statement->value,
                                               statement->expression))
                conversion_error(analyzer, statement->value, NULL, NULL, statement->expression,
                                 "Cannot implicitly convert assigned value");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_is_array &&
                statement->expression->resolved_outer_pointer_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Arrays cannot be assigned as values");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_named_symbol_id != AST_SYMBOL_NONE &&
                statement->expression->resolved_named_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[
                    statement->expression->resolved_named_symbol_id].kind ==
                SEMANTIC_SYMBOL_STRUCT &&
                statement->assignment_operator != TOKEN_EQUAL)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Structures only support simple assignment");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->assignment_operator != TOKEN_EQUAL &&
                !plain_numeric_expression(statement->expression))
                operand_error(analyzer, statement->expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Compound assignment requires a numeric target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_type == TYPE_BIT &&
                statement->assignment_operator != TOKEN_EQUAL)
                operand_error(analyzer, statement->expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Boolean values do not support arithmetic assignment");
            if (statement->kind == AST_STMT_BREAK && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_BREAK_OUTSIDE_LOOP, "Break used outside a loop");
            if (statement->kind == AST_STMT_CONTINUE && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_CONTINUE_OUTSIDE_LOOP, "Continue used outside a loop");
            if (statement->kind == AST_STMT_RETURN && analyzer->current_function != NULL) {
                DataType expected = primitive_type(analyzer->program,
                                                   &analyzer->current_function->as.function.return_type);
                if (expected == TYPE_VOID && statement->value != NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Void function cannot return a value");
                else if (expected != TYPE_VOID && statement->value == NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Function must return a value");
                else if (statement->value != NULL &&
                         !expression_to_declared_type_allowed(analyzer, statement->value,
                                                              analyzer->program,
                                                              &analyzer->current_function->as.function.return_type))
                    conversion_error(analyzer, statement->value, analyzer->program,
                                     &analyzer->current_function->as.function.return_type,
                                     NULL, "Cannot implicitly convert returned value");
            }
            analyze_statement(analyzer, statement->initializer);
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth++;
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE)
                analyzer->scope_depth++;
            analyze_statement(analyzer, statement->body);
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE)
                analyzer->scope_depth--;
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth--;
            analyze_statement(analyzer, statement->else_body);
        }
        if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
            statement->kind == AST_STMT_WHILE)
            pop_to(analyzer, scope);
    }
}

static void analyze_function(Analyzer *analyzer, AstDeclarationNode *function) {
    if (function->generic_parameters != NULL || function->semantic_body_checked) return;
    function->semantic_body_checked = 1;
    LocalSymbol *saved = analyzer->locals;
    size_t saved_function = analyzer->current_function_token;
    size_t saved_function_symbol = analyzer->current_function_symbol_id;
    size_t saved_owner = analyzer->current_owner_token;
    const AstDeclarationNode *saved_declaration = analyzer->current_function;
    analyzer->current_function_token = function->name_token;
    analyzer->current_function_symbol_id = function->resolved_symbol_id;
    analyzer->current_owner_token = function->as.function.owner_token;
    analyzer->current_function = function;
    analyzer->local_storage = 0;
    analyzer->storage_error_reported = 0;
    analyzer->complexity_error_reported = 0;
    if (!known_declared_type(analyzer, &function->as.function.return_type))
        semantic_error(analyzer, function->as.function.return_type.name_token,
                       ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown function return type");
    validate_array_shape(analyzer, &function->as.function.return_type);
    if (function->as.function.return_type.is_array &&
        function->as.function.return_type.outer_pointer_depth == 0)
        semantic_error(analyzer, function->as.function.return_type.name_token,
                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Array return types are not supported; return a slice instead");
    analyzer->scope_depth++;
    for (AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        if (!known_declared_type(analyzer, &parameter->type))
            semantic_error(analyzer, parameter->type.name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown parameter type");
        validate_array_shape(analyzer, &parameter->type);
        if (primitive_type(analyzer->program, &parameter->type) == TYPE_VOID &&
            parameter->type.pointer_depth == 0)
            semantic_error(analyzer, parameter->type.name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Parameter cannot have type void");
        for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
             existing = existing->next)
            if (existing->scope_depth == analyzer->scope_depth &&
                same_name(analyzer->program, existing->name_token,
                          ast_program_lexeme(analyzer->program,
                                             parameter->name_token))) {
                semantic_duplicate(analyzer, parameter->name_token, analyzer->program, existing->name_token,
                                   "Duplicate parameter");
                break;
            }
        if (same_name(analyzer->program, parameter->name_token, "true") ||
            same_name(analyzer->program, parameter->name_token, "false"))
            semantic_error(analyzer, parameter->name_token,
                           ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                           "Reserved name cannot be declared");
        LocalSymbol *local = push_local(analyzer, parameter->name_token, parameter->type,
                                        SEMANTIC_SYMBOL_PARAMETER, NULL, 0);
        if (local != NULL) parameter->resolved_symbol_id = local->symbol_id;
    }
    analyze_statement(analyzer, function->as.function.body);
    DataType return_type = primitive_type(analyzer->program,
                                          &function->as.function.return_type);
    if (return_type != TYPE_VOID &&
        !statement_always_returns(function->as.function.body))
        semantic_error(analyzer, function->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Function does not return on all paths");
    pop_to(analyzer, saved);
    analyzer->scope_depth--;
    analyzer->current_function_token = saved_function;
    analyzer->current_function_symbol_id = saved_function_symbol;
    analyzer->current_owner_token = saved_owner;
    analyzer->current_function = saved_declaration;
}

static void analyze_constant_declaration(Analyzer *analyzer,
                                         AstDeclarationNode *declaration) {
    if (declaration->semantic_body_checked) return;
    declaration->semantic_body_checked = 2;
    size_t errors_before = analyzer->model->error_count;
    AstExpression *value = declaration->as.constant.value;
    AstType *type = &declaration->as.constant.type;
    if (declaration->kind == AST_DECL_VARIABLE && !value) {
        if (type->kind == AST_TYPE_INFERRED || !known_declared_type(analyzer, type) ||
            (primitive_type(analyzer->program, type) == TYPE_VOID && !type->pointer_depth && !type->
             outer_pointer_depth))
            semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Package variable needs a concrete non-void type or a constant initializer");
        declaration->semantic_body_checked = 1;
        return;
    }
    analyze_expression(analyzer, value);
    validate_expression(analyzer, value, 0);
    if (value == NULL || !constant_expression_allowed(analyzer, value))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Constant initializer is not a constant expression");
    if (!known_declared_type(analyzer, type))
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                       "Unknown constant type");
    if (type->pointer_depth != 0 || type->outer_pointer_depth != 0 ||
        type->is_array || type->is_slice ||
        (value != NULL && value->resolved_named_symbol_id != AST_SYMBOL_NONE))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Constants require a primitive or string type");
    if (type->kind != AST_TYPE_INFERRED && value != NULL &&
        !expression_to_declared_type_allowed(analyzer, value, analyzer->program, type))
        conversion_error(analyzer, value, analyzer->program, type,
                         NULL, "Cannot implicitly convert constant initializer");
    if (value != NULL && analyzer->model->error_count == errors_before &&
        constant_expression_allowed(analyzer, value))
        (void) fold_constant(analyzer, value,
                             type->kind == AST_TYPE_INFERRED
                                 ? value->resolved_type
                                 : primitive_type(analyzer->program, type));
    if (declaration->resolved_symbol_id < analyzer->model->symbol_count && value != NULL &&
        type->kind == AST_TYPE_INFERRED) {
        SemanticSymbol *symbol =
                &analyzer->model->symbols[declaration->resolved_symbol_id];
        symbol->resolved_type = value->resolved_type;
        symbol->resolved_pointer_depth = value->resolved_pointer_depth;
        symbol->resolved_outer_pointer_depth = value->resolved_outer_pointer_depth;
        symbol->resolved_named_type_token = value->resolved_named_type_token;
        symbol->resolved_named_symbol_id = value->resolved_named_symbol_id;
        symbol->resolved_is_array = value->resolved_is_array;
        symbol->resolved_is_slice = value->resolved_is_slice;
    }
    declaration->semantic_body_checked = 1;
}

static void analyze_unit_constants(Analyzer *analyzer, AstProgram *root,
                                   AstProgram *unit, unsigned char *states) {
    size_t index = 0;
    if (unit != root) {
        for (index = 1; index <= root->owned_import_count; index++)
            if (root->owned_imports[index - 1] == unit) break;
        if (index > root->owned_import_count) return;
    }
    if (states[index] != 0) return;
    states[index] = 1;
    for (AstDeclarationNode *declaration = unit->root; declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_IMPORT)
            for (AstImportPath *path = declaration->as.import_decl.paths; path != NULL; path = path->next)
                if (path->resolved_program != NULL)
                    analyze_unit_constants(analyzer, root, path->resolved_program, states);
    analyzer->program = unit;
    for (AstDeclarationNode *declaration = unit->root; declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE)
            analyze_constant_declaration(analyzer, declaration);
    states[index] = 2;
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
    for (size_t i = 0; i <= program->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? program : program->owned_imports[i - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            d->semantic_body_checked = 0;
            if (d->generic_parameters) continue;
            if (d->kind == AST_DECL_FUNCTION) normalize_function_types(&analyzer, d);
            else if (d->kind == AST_DECL_VARIABLE || d->kind == AST_DECL_CONSTANT) normalize_generic_type(
                &analyzer, &d->as.constant.type, 0);
            else if (d->kind == AST_DECL_INTERFACE) {
                for (AstDeclarationNode *m = d->as.interface_decl.methods; m; m = m->next)
                    normalize_function_types(&analyzer, m);
            }
            else if (d->kind == AST_DECL_ENUM) {
                for (AstEnumValue *v = d->as.enum_decl.values; v; v = v->next)
                    for (AstTypeArgument *p = v->payload_types; p; p = p->next)
                        normalize_generic_type(&analyzer, &p->type, 0);
            } else if (d->kind == AST_DECL_STRUCT) {
                for (AstField *f = d->as.struct_decl.fields; f; f = f->next)
                    normalize_generic_type(&analyzer, &f->type, 0);
                AstType self_type = {.kind = AST_TYPE_NAMED, .name_token = d->name_token,
                                     .array_length_token = AST_TOKEN_NONE};
                for (AstDeclarationNode *m = d->as.struct_decl.methods; m; m = m->next) {
                    m->semantic_body_checked = 0;
                    replace_self_type(&analyzer, &m->as.function.return_type, &self_type);
                    for (AstParameter *p = m->as.function.parameters; p; p = p->next)
                        replace_self_type(&analyzer, &p->type, &self_type);
                    replace_self_statement(&analyzer, m->as.function.body, &self_type);
                    normalize_function_types(&analyzer, m);
                }
            }
        }
    }
    analyzer.program = program;
    prepare_interfaces(&analyzer, program);
    collect_declarations(&analyzer, program);
    for (size_t i = 0; i < program->owned_import_count; i++)
        collect_declarations(&analyzer, program->owned_imports[i]);
    analyzer.program = program;
    unsigned char *constant_states = calloc(program->owned_import_count + 1, 1);
    if (constant_states == NULL) {
        semantic_model_free(model);
        return NULL;
    }
    analyze_unit_constants(&analyzer, program, program, constant_states);
    for (size_t i = 0; i < program->owned_import_count; i++)
        analyze_unit_constants(&analyzer, program, program->owned_imports[i], constant_states);
    free(constant_states);
    /* Complete declaration type shapes before bodies use forward declarations. */
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        analyzer.program = (AstProgram *) symbol->source_program;
        if (symbol->kind == SEMANTIC_SYMBOL_FIELD) {
            AstField *field = (AstField *) symbol->node;
            validate_array_shape(&analyzer, &field->type);
        } else if (symbol->kind == SEMANTIC_SYMBOL_VARIABLE) {
            AstDeclarationNode *variable = (AstDeclarationNode *) symbol->declaration;
            validate_array_shape(&analyzer, &variable->as.constant.type);
            symbol->declared_type = variable->as.constant.type;
        } else if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration != NULL) {
            AstDeclarationNode *function = (AstDeclarationNode *) symbol->declaration;
            validate_array_shape(&analyzer, &function->as.function.return_type);
            symbol->declared_type = function->as.function.return_type;
            for (AstParameter *parameter = function->as.function.parameters;
                 parameter != NULL; parameter = parameter->next)
                validate_array_shape(&analyzer, &parameter->type);
        }
    }
    analyzer.program = program;
    validate_overload_sets(&analyzer);
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->generic_parameters != NULL) continue;
            if (declaration->kind == AST_DECL_FUNCTION)
                analyze_function(&analyzer, declaration);
            else if (declaration->kind == AST_DECL_STRUCT) {
                for (AstField *field = declaration->as.struct_decl.fields;
                     field != NULL; field = field->next) {
                    if (!known_declared_type(&analyzer, &field->type))
                        semantic_error(&analyzer, field->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown field type");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Field cannot have type void");
                    validate_array_shape(&analyzer, &field->type);
                }
                for (AstDeclarationNode *method = declaration->as.struct_decl.methods;
                     method != NULL; method = method->next)
                    analyze_function(&analyzer, method);
            } else if (declaration->kind == AST_DECL_ENUM) {
                for (AstField *field = declaration->as.enum_decl.fields;
                     field != NULL; field = field->next) {
                    if (!known_declared_type(&analyzer, &field->type))
                        semantic_error(&analyzer, field->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown enum field type");
                    if (field->type.is_slice)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Slice fields are not supported");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum field cannot have type void");
                    if (primitive_type(unit, &field->type) == TYPE_UNKNOWN ||
                        field->type.pointer_depth != 0 || field->type.is_array)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum fields require scalar primitive types");
                    validate_array_shape(&analyzer, &field->type);
                }
                size_t field_count = 0;
                for (AstField *field = declaration->as.enum_decl.fields;
                     field != NULL; field = field->next)
                    field_count++;
                for (AstEnumValue *value = declaration->as.enum_decl.values;
                     value != NULL; value = value->next) {
                    for (AstTypeArgument *p = value->payload_types; p; p = p->next) {
                        if (!known_declared_type(&analyzer, &p->type) ||
                            (primitive_type(unit, &p->type) == TYPE_VOID && !p->type.pointer_depth))
                            semantic_error(&analyzer, p->type.name_token, ERROR_CATEGORY_TYPE,
                                           ERR_TYPE_INVALID_OPERATION,
                                           "Enum payload requires a complete non-void concrete type");
                        validate_array_shape(&analyzer, &p->type);
                    }

                    size_t argument_count = 0;
                    for (AstExpression *argument = value->arguments;
                         argument != NULL; argument = argument->next) {
                        analyze_expression(&analyzer, argument);
                        validate_expression(&analyzer, argument, 0);
                        argument_count++;
                    }
                    if (argument_count != field_count) {
                        char message[128];
                        (void) snprintf(message, sizeof(message),
                                        "Enum value expects %zu arguments but received %zu",
                                        field_count, argument_count);
                        semantic_error(&analyzer, value->name_token,
                                       ERROR_CATEGORY_SEMANTIC, ERR_SEM_WRONG_ARG_COUNT, message);
                    } else {
                        AstExpression *argument = value->arguments;
                        AstField *field = declaration->as.enum_decl.fields;
                        for (; argument != NULL && field != NULL;
                               argument = argument->next, field = field->next) {
                            if (!enum_constant_expression(&analyzer, argument))
                                semantic_error(&analyzer, argument->first_token,
                                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                               "Enum arguments must be compile-time scalar constants");
                            if (!expression_to_declared_type_allowed(&analyzer, argument,
                                                                     unit, &field->type))
                                conversion_error(&analyzer, argument, unit, &field->type, NULL,
                                                 "Cannot implicitly convert enum argument");
                        }
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration != NULL) {
            if (symbol->owner_symbol_id < model->symbol_count &&
                model->symbols[symbol->owner_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE)
                continue;
            analyzer.program = (AstProgram *) symbol->source_program;
            analyze_function(&analyzer, (AstDeclarationNode *) symbol->declaration);
        }
    }
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if ((symbol->kind != SEMANTIC_SYMBOL_STRUCT && symbol->kind != SEMANTIC_SYMBOL_ENUM) || !symbol->declaration)
            continue;
        analyzer.program = (AstProgram *) symbol->source_program;
        if (aggregate_reaches(&analyzer, symbol->id, symbol->id, 0))
            semantic_error(&analyzer, symbol->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           symbol->kind == SEMANTIC_SYMBOL_ENUM
                               ? "Recursive enum payload requires a pointer"
                               : "Recursive structure requires a pointer field");
        const AstDeclarationNode *declaration = symbol->declaration;
        if (declaration->generic_origin) {
            AstType arguments[DMM_MAX_TYPE_PARAMETERS];
            size_t count = 0;
            for (const AstTypeArgument *p = declaration->specialization_arguments; p && count < DMM_MAX_TYPE_PARAMETERS;
                 p = p->next)
                arguments[count++] = p->type;
            if (!generic_bounds_satisfied(&analyzer, symbol->source_program, declaration->generic_origin, arguments))
                semantic_error(&analyzer, symbol->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                               "Generic aggregate type arguments do not satisfy interface bounds");
        }
        if (symbol->kind == SEMANTIC_SYMBOL_ENUM)
            for (AstEnumValue *v = declaration->as.enum_decl.values; v; v = v->next)
                for (AstTypeArgument *p = v->payload_types; p; p = p->next) {
                    validate_array_shape(&analyzer, &p->type);
                    if (!known_declared_type(&analyzer, &p->type) ||
                        (primitive_type(analyzer.program, &p->type) == TYPE_VOID && !p->type.pointer_depth))
                        semantic_error(&analyzer, p->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum payload requires a complete non-void concrete type");
                }
    }
    const SemanticSymbol *main_symbol = semantic_find_global(model, "main",
                                                             SEMANTIC_SYMBOL_FUNCTION);
    if (program->executable_build && program->package_name && strcmp(program->package_name, "main")) {
        analyzer.program = program;
        semantic_error(&analyzer, program->package_token, ERROR_CATEGORY_COMPILER, ERR_COMP_NO_MAIN_FUNCTION,
                       "Executable builds require package main");
    } else if (program->package_name && strcmp(program->package_name, "main")) {
        /* Library packages have no entry-point requirement. */
    } else if (main_symbol == NULL || main_symbol->declaration == NULL) {
        analyzer.program = program;
        semantic_error(&analyzer, AST_TOKEN_NONE, ERROR_CATEGORY_COMPILER, ERR_COMP_NO_MAIN_FUNCTION,
                       "Program must define main: func main() -> int or func main() -> void");
    } else {
        analyzer.program = (AstProgram *) main_symbol->source_program;
        const AstDeclarationNode *main_declaration = main_symbol->declaration;
        DataType main_type = primitive_type(main_symbol->source_program,
                                            &main_declaration->as.function.return_type);
        if (main_declaration->as.function.parameters != NULL ||
            main_declaration->as.function.return_type.pointer_depth != 0 ||
            main_declaration->as.function.return_type.outer_pointer_depth != 0 ||
            main_declaration->as.function.return_type.is_array || main_declaration->as.function.return_type.is_slice ||
            (main_type != TYPE_VOID && main_type != TYPE_INT))
            semantic_error(&analyzer, main_symbol->name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                           "main must have no parameters and return void or int");
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
    free(model->symbol_index);
    free(model->symbols);
    free(model);
}
