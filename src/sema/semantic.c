#include "semantic.h"

#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct LocalSymbol {
    size_t name_token;
    AstType type;
    size_t symbol_id;
    DataType resolved_type;
    unsigned resolved_pointer_depth;
    unsigned resolved_outer_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    int resolved_is_slice;
    int is_constant;
    size_t scope_depth;
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
    unsigned loop_depth;
    const AstDeclarationNode *current_function;
    size_t local_storage;
    int storage_error_reported;
    int complexity_error_reported;
    int allocation_failed;
} Analyzer;

static int known_declared_type(const Analyzer *analyzer, const AstType *type);

static void semantic_error(Analyzer *analyzer, size_t token, int code,
                           const char *message) {
    const AstToken *location = ast_program_token(analyzer->program, token);
    error_report(global_error_handler, SEVERITY_ERROR,
                 location == NULL ? 0 : location->span.begin.line,
                 location == NULL ? 0 : location->span.begin.column,
                 ERROR_CATEGORY_SEMANTIC, code, analyzer->program->source_path,
                 "%s", message);
    analyzer->model->error_count++;
}

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
    if (token >= program->token_count) return 0;
    const char *lexeme = program->tokens[token].lexeme;
    return lexeme == name || strcmp(lexeme, name) == 0;
}

static int reserved_link_name(const char *name) {
    return strncmp(name, "__dmm_", 6) == 0;
}

static uint64_t symbol_hash(const char *name, SemanticSymbolKind kind) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; name[i] != '\0'; i++) {
        hash ^= (unsigned char) name[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint64_t) kind + UINT64_C(0x9e3779b97f4a7c15);
    hash *= UINT64_C(1099511628211);
    return hash;
}

static const char *symbol_name(const SemanticSymbol *symbol) {
    return ast_program_lexeme(symbol->source_program, symbol->name_token);
}

static int index_symbol(SemanticModel *model, size_t symbol_id) {
    const SemanticSymbol *symbol = &model->symbols[symbol_id];
    const char *name = symbol_name(symbol);
    size_t mask = model->symbol_index_capacity - 1U;
    size_t slot = (size_t) symbol_hash(name, symbol->kind) & mask;
    while (model->symbol_index[slot] != 0) {
        const SemanticSymbol *existing = &model->symbols[model->symbol_index[slot] - 1U];
        if (existing->kind == symbol->kind && same_name(existing->source_program,
                                                        existing->name_token, name))
            return 1;
        slot = (slot + 1U) & mask;
    }
    model->symbol_index[slot] = symbol_id + 1U;
    return 1;
}

static int grow_symbol_index(SemanticModel *model) {
    size_t capacity = model->symbol_index_capacity == 0
        ? 64U : model->symbol_index_capacity * 2U;
    if (capacity < model->symbol_index_capacity ||
        capacity > SIZE_MAX / sizeof(*model->symbol_index)) return 0;
    size_t *previous = model->symbol_index;
    model->symbol_index = calloc(capacity, sizeof(*model->symbol_index));
    if (model->symbol_index == NULL) {
        model->symbol_index = previous;
        return 0;
    }
    model->symbol_index_capacity = capacity;
    for (size_t i = 0; i < model->symbol_count; i++) (void) index_symbol(model, i);
    free(previous);
    return 1;
}

const SemanticSymbol *semantic_find_global(const SemanticModel *model,
                                           const char *name,
                                           SemanticSymbolKind kind) {
    if (model == NULL || name == NULL || model->symbol_index_capacity == 0) return NULL;
    size_t mask = model->symbol_index_capacity - 1U;
    size_t slot = (size_t) symbol_hash(name, kind) & mask;
    while (model->symbol_index[slot] != 0) {
        const SemanticSymbol *symbol = &model->symbols[model->symbol_index[slot] - 1U];
        if (symbol->kind == kind && same_name(symbol->source_program, symbol->name_token, name))
            return symbol;
        slot = (slot + 1U) & mask;
    }
    return NULL;
}

static int append_symbol(Analyzer *analyzer, SemanticSymbol symbol) {
    SemanticModel *model = analyzer->model;
    if (model->symbol_index_capacity == 0 ||
        model->symbol_count + 1U >= model->symbol_index_capacity / 2U) {
        if (!grow_symbol_index(model)) {
            analyzer->allocation_failed = 1;
            return 0;
        }
    }
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
    (void) index_symbol(model, symbol.id);
    return 1;
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
    if (owner_token == AST_TOKEN_NONE && kind == SEMANTIC_SYMBOL_FUNCTION &&
        reserved_link_name(name))
        semantic_error(analyzer, declaration->name_token, ERR_PARSE_INVALID_DECLARATION,
                       "Function name is reserved by the runtime");
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        int same_owner = existing->owner_token == AST_TOKEN_NONE && owner_token == AST_TOKEN_NONE;
        if (existing->owner_token != AST_TOKEN_NONE && owner_token != AST_TOKEN_NONE)
            same_owner = same_name(existing->source_program, existing->owner_token,
                                   ast_program_lexeme(analyzer->program, owner_token));
        if (existing->kind != SEMANTIC_SYMBOL_IMPORT && same_owner &&
            same_name(existing->source_program, existing->name_token, name)) {
            if (kind == SEMANTIC_SYMBOL_FUNCTION &&
                existing->kind == SEMANTIC_SYMBOL_FUNCTION) continue;
            analyzer->model->duplicate_symbol_count++;
            semantic_error(analyzer, declaration->name_token,
                           ERR_PARSE_DUPLICATE_DEFINITION,
                           kind == SEMANTIC_SYMBOL_FUNCTION ? "Duplicate function" :
                                                              "Duplicate type declaration");
            return;
        }
    }
    AstType type = {0};
    if (kind == SEMANTIC_SYMBOL_FUNCTION) type = declaration->as.function.return_type;
    else if (kind == SEMANTIC_SYMBOL_CONSTANT) type = declaration->as.constant.type;
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
    const char *member_name = ast_program_lexeme(analyzer->program, name_token);
    const char *owner_name = ast_program_lexeme(analyzer->program, owner_token);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *existing = &analyzer->model->symbols[i];
        if (existing->owner_token != AST_TOKEN_NONE &&
            same_name(existing->source_program, existing->owner_token, owner_name) &&
            same_name(existing->source_program, existing->name_token, member_name)) {
            semantic_error(analyzer, name_token, ERR_PARSE_DUPLICATE_DEFINITION,
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
        if (declaration->kind == AST_DECL_IMPORT) {
            AstType no_type = {0};
            size_t name = declaration->as.import_decl.path_token;
            if (name == AST_TOKEN_NONE) name = declaration->as.import_decl.path_first_token;
            add_member(analyzer, name, AST_TOKEN_NONE, no_type, SEMANTIC_SYMBOL_IMPORT,
                       declaration, &declaration->resolved_symbol_id);
        } else if (declaration->kind == AST_DECL_FUNCTION) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
        } else if (declaration->kind == AST_DECL_CONSTANT) {
            add_global(analyzer, declaration, SEMANTIC_SYMBOL_CONSTANT, AST_TOKEN_NONE);
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
        left->is_array != right->is_array || left->is_slice != right->is_slice) return 0;
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
        if (left->array_length_token >= left_program->token_count ||
            right->array_length_token >= right_program->token_count) return 0;
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
                               right->source_program, &b->type)) return 0;
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
                left->owner_symbol_id != right->owner_symbol_id ||
                left->declaration->as.function.is_static !=
                    right->declaration->as.function.is_static ||
                !same_name(right->source_program, right->name_token, name)) continue;
            if (strcmp(name, "main") == 0 || same_function_signature(analyzer, left, right)) {
                analyzer->model->duplicate_symbol_count++;
                semantic_error(analyzer, left->name_token, ERR_PARSE_DUPLICATE_DEFINITION,
                    strcmp(name, "main") == 0 ? "main cannot be overloaded" :
                                                "Duplicate function overload signature");
            }
        }
    }
}

static size_t builtin_arity(const char *name) {
    if (strcmp(name, "scanfInt") == 0 || strcmp(name, "scanfChar") == 0 ||
        strcmp(name, "scanfString") == 0) return 0;
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strdup") == 0 ||
        strcmp(name, "malloc") == 0 || strcmp(name, "io_strlen") == 0 ||
        strcmp(name, "io_str_to_int") == 0 || strcmp(name, "sys_close") == 0)
        return 1;
    if (strcmp(name, "strcmp") == 0 || strcmp(name, "strcpy") == 0 ||
        strcmp(name, "strcat") == 0 || strcmp(name, "read") == 0) return 2;
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
    int from_integral = from == TYPE_INT || from == TYPE_CHAR || from == TYPE_BYTE ||
                        from == TYPE_BIT;
    int to_integral = to == TYPE_INT || to == TYPE_CHAR || to == TYPE_BYTE || to == TYPE_BIT;
    if (from_integral && to_integral) return 1;
    if ((from == TYPE_INT || from == TYPE_CHAR || from == TYPE_BYTE || from == TYPE_BIT) &&
        (to == TYPE_FLOAT || to == TYPE_DOUBLE)) return 1;
    return from == TYPE_FLOAT && to == TYPE_DOUBLE;
}

static int expression_conversion_allowed(const AstExpression *expression,
                                         DataType to, unsigned to_pointers) {
    if (expression == NULL) return 0;
    if (implicit_conversion_allowed(expression->resolved_type,
                                    expression->resolved_pointer_depth +
                                        expression->resolved_outer_pointer_depth,
                                    to, to_pointers)) return 1;
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

static int expression_to_declared_type_allowed(const Analyzer *analyzer,
                                                const AstExpression *expression,
                                                const AstProgram *type_program,
                                                const AstType *type) {
    if (expression == NULL || type == NULL) return 0;
    unsigned target_depth = type->pointer_depth + type->outer_pointer_depth;
    unsigned source_depth = expression->resolved_pointer_depth +
                            expression->resolved_outer_pointer_depth;
    size_t target_name = named_type_token(type_program, type);
    int matching_shape = target_depth == source_depth;
    if (type->is_slice)
        matching_shape = type->outer_pointer_depth == 0 &&
            expression->resolved_outer_pointer_depth == 0 &&
            type->pointer_depth == expression->resolved_pointer_depth &&
            (expression->resolved_is_array || expression->resolved_is_slice);
    else
        matching_shape = matching_shape && type->is_array == expression->resolved_is_array &&
                         type->is_slice == expression->resolved_is_slice;
    if (target_name != AST_TOKEN_NONE) {
        size_t target_symbol = resolve_named_symbol_id(analyzer, type_program, target_name);
        return target_symbol != AST_SYMBOL_NONE &&
               target_symbol == expression->resolved_named_symbol_id &&
               matching_shape;
    }
    if (expression->resolved_named_symbol_id != AST_SYMBOL_NONE) return 0;
    if (!matching_shape) return 0;
    if (type->is_array || type->is_slice) return
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
        source->resolved_is_slice != target->resolved_is_slice) return 0;
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
           (expression->resolved_type == TYPE_INT ||
            expression->resolved_type == TYPE_CHAR ||
            expression->resolved_type == TYPE_BYTE ||
            expression->resolved_type == TYPE_BIT ||
            expression->resolved_type == TYPE_FLOAT ||
            expression->resolved_type == TYPE_DOUBLE);
}

static int integral_expression(const AstExpression *expression) {
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
        analyzer->program->tokens[expression->value_token].type != TOKEN_NUMBER) return 0;
    *value = strtoll(ast_program_lexeme(analyzer->program, expression->value_token), NULL, 10);
    if (negative) *value = -*value;
    return 1;
}

static int enum_constant_expression(const Analyzer *analyzer,
                                    const AstExpression *expression) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_LITERAL) return 1;
    if (expression->kind == AST_EXPR_NAME)
        return same_name(analyzer->program, expression->value_token, "true") ||
               same_name(analyzer->program, expression->value_token, "false");
    return expression->kind == AST_EXPR_UNARY &&
           expression->operator_type == TOKEN_MINUS &&
           expression->right != NULL &&
           expression->right->kind == AST_EXPR_LITERAL &&
           plain_numeric_expression(expression->right);
}

static size_t parameter_count(const AstDeclarationNode *function) {
    size_t count = 0;
    if (function != NULL)
        for (const AstParameter *parameter = function->as.function.parameters;
             parameter != NULL; parameter = parameter->next) count++;
    return count;
}

static int argument_conversion_rank(const Analyzer *analyzer,
                                    const AstExpression *argument,
                                    const AstProgram *parameter_program,
                                    const AstType *parameter) {
    if (!expression_to_declared_type_allowed(analyzer, argument,
                                             parameter_program, parameter)) return -1;
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
        argument->resolved_outer_pointer_depth == 0) return 1;
    if (argument->resolved_pointer_depth == 0 &&
        argument->resolved_outer_pointer_depth == 0 &&
        !argument->resolved_is_array && !argument->resolved_is_slice &&
        parameter->pointer_depth == 0 && parameter->outer_pointer_depth == 0 &&
        !parameter->is_array && !parameter->is_slice) {
        DataType to = primitive_type(parameter_program, parameter);
        if (((argument->resolved_type == TYPE_BIT || argument->resolved_type == TYPE_BYTE ||
              argument->resolved_type == TYPE_CHAR) && to == TYPE_INT) ||
            (argument->resolved_type == TYPE_FLOAT && to == TYPE_DOUBLE)) return 1;
    }
    return 2;
}

static int viable_function(const Analyzer *analyzer, const SemanticSymbol *function,
                           const AstExpression *arguments) {
    size_t actual = 0;
    for (const AstExpression *argument = arguments; argument != NULL;
         argument = argument->next) actual++;
    if (function == NULL || function->declaration == NULL ||
        parameter_count(function->declaration) != actual) return 0;
    const AstExpression *argument = arguments;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
         argument = argument->next, parameter = parameter->next)
        if (argument_conversion_rank(analyzer, argument, function->source_program,
                                     &parameter->type) < 0) return 0;
    return argument == NULL && parameter == NULL;
}

static int function_dominates(const Analyzer *analyzer,
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
            !same_name(candidate->source_program, candidate->name_token, name) ||
            candidate->declaration == NULL ||
            (owner_symbol_id != AST_SYMBOL_NONE &&
             candidate->declaration->as.function.is_static != is_static) ||
            !viable_function(analyzer, candidate, arguments)) continue;
        int dominated = 0;
        for (size_t j = 0; j < analyzer->model->symbol_count && !dominated; j++) {
            const SemanticSymbol *other = &analyzer->model->symbols[j];
            if (other == candidate || other->kind != SEMANTIC_SYMBOL_FUNCTION ||
                other->owner_symbol_id != owner_symbol_id ||
                !same_name(other->source_program, other->name_token, name) ||
                other->declaration == NULL ||
                (owner_symbol_id != AST_SYMBOL_NONE &&
                 other->declaration->as.function.is_static != is_static) ||
                !viable_function(analyzer, other, arguments)) continue;
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

static int assignable_expression(const Analyzer *analyzer,
                                 const AstExpression *expression);

static int expression_is_constant_symbol(const Analyzer *analyzer,
                                         const AstExpression *expression) {
    return expression != NULL && expression->resolved_symbol_id < analyzer->model->symbol_count &&
           analyzer->model->symbols[expression->resolved_symbol_id].kind ==
               SEMANTIC_SYMBOL_CONSTANT;
}

static int constant_expression_allowed(const Analyzer *analyzer,
                                       const AstExpression *expression) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_LITERAL) return 1;
    if (expression->kind == AST_EXPR_NAME)
        return same_name(analyzer->program, expression->value_token, "true") ||
               same_name(analyzer->program, expression->value_token, "false") ||
               expression_is_constant_symbol(analyzer, expression);
    if (expression->kind == AST_EXPR_CAST)
        return expression->arguments != NULL && expression->arguments->next == NULL &&
               constant_expression_allowed(analyzer, expression->arguments);
    if (expression->kind == AST_EXPR_UNARY)
        return (expression->operator_type == TOKEN_MINUS ||
                expression->operator_type == TOKEN_BANG) &&
               constant_expression_allowed(analyzer, expression->right);
    if (expression->kind == AST_EXPR_BINARY)
        return constant_expression_allowed(analyzer, expression->left) &&
               constant_expression_allowed(analyzer, expression->right);
    return 0;
}

static void validate_builtin_arguments(Analyzer *analyzer,
                                       const AstExpression *expression,
                                       const char *name) {
    const AstExpression *a = expression->arguments;
    const AstExpression *b = a == NULL ? NULL : a->next;
    const AstExpression *c = b == NULL ? NULL : b->next;
    int valid = 1;
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strdup") == 0 ||
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
    if (!valid)
        semantic_error(analyzer, expression->value_token, ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Builtin argument has an incompatible type");
}

static void validate_function_arguments(Analyzer *analyzer,
                                        const AstExpression *expression,
                                        const SemanticSymbol *function) {
    if (function == NULL || function->kind != SEMANTIC_SYMBOL_FUNCTION ||
        function->declaration == NULL) return;
    size_t actual = 0;
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) actual++;
    size_t expected = parameter_count(function->declaration);
    if (actual != expected) {
        char message[128];
        (void) snprintf(message, sizeof(message),
            "Function expects %zu arguments but received %zu", expected, actual);
        semantic_error(analyzer, expression->value_token,
                       ERR_SEM_WRONG_ARG_COUNT, message);
        return;
    }
    const AstExpression *argument = expression->arguments;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
         argument = argument->next, parameter = parameter->next) {
        if (!expression_to_declared_type_allowed(analyzer, argument,
                function->source_program, &parameter->type))
            semantic_error(analyzer, argument->value_token,
                ERR_TYPE_INCOMPATIBLE_TYPES,
                "Cannot implicitly convert argument to parameter type");
    }
}

static void validate_expression(Analyzer *analyzer, AstExpression *expression,
                                int is_callee) {
    if (expression == NULL) return;
    if (expression->token_count > 512U && !analyzer->complexity_error_reported) {
        semantic_error(analyzer, expression->first_token, ERR_PARSE_TOO_MANY_ERRORS,
                       "Expression tree exceeds 512 tokens");
        analyzer->complexity_error_reported = 1;
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
                const SemanticSymbol *declared = semantic_find_global(analyzer->model, name,
                    SEMANTIC_SYMBOL_FUNCTION);
                (void) snprintf(message, sizeof(message), ambiguous
                    ? "Call to '%s' is ambiguous"
                    : declared != NULL ? "No overload of '%s' matches the supplied arguments"
                                       : "Function '%s' not found", name);
                semantic_error(analyzer, expression->left->value_token,
                               ambiguous || declared != NULL ? ERR_TYPE_INCOMPATIBLE_TYPES
                                                            : ERR_SEM_UNDEFINED_FUNCTION,
                               message);
            } else if (expression->resolved_symbol_id == AST_SYMBOL_NONE &&
                       is_builtin_name(name)) {
                size_t actual = 0;
                for (AstExpression *argument = expression->arguments;
                     argument != NULL; argument = argument->next) actual++;
                size_t expected = builtin_arity(name);
                if (expected != AST_TOKEN_NONE && actual != expected) {
                    char message[128];
                    (void) snprintf(message, sizeof(message),
                        "Function expects %zu arguments but received %zu", expected, actual);
                    semantic_error(analyzer, expression->left->value_token,
                                   ERR_SEM_WRONG_ARG_COUNT, message);
                } else validate_builtin_arguments(analyzer, expression, name);
            } else if (expression->resolved_symbol_id < analyzer->model->symbol_count)
                validate_function_arguments(analyzer, expression,
                    &analyzer->model->symbols[expression->resolved_symbol_id]);
        } else validate_expression(analyzer, expression->left, 1);
        if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
            expression->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *method =
                &analyzer->model->symbols[expression->resolved_symbol_id];
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
                               ERR_TYPE_INVALID_OPERATION,
                               "Instance method requires a struct value receiver");
            if (method->kind == SEMANTIC_SYMBOL_FUNCTION && method->declaration != NULL &&
                method->declaration->as.function.is_static && !type_receiver)
                semantic_error(analyzer, expression->value_token,
                               ERR_TYPE_INVALID_OPERATION,
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
            (void) resolve_overload(analyzer, name, receiver->resolved_named_symbol_id,
                                    type_receiver, expression->arguments, &ambiguous);
            semantic_error(analyzer, expression->left->value_token,
                           ERR_TYPE_INCOMPATIBLE_TYPES,
                           ambiguous ? "Method call is ambiguous" :
                                       "No method overload matches the supplied arguments");
        }
        for (AstExpression *argument = expression->arguments; argument != NULL;
             argument = argument->next) validate_expression(analyzer, argument, 0);
        return;
    }
    validate_expression(analyzer, expression->left, 0);
    validate_expression(analyzer, expression->right, 0);
    for (AstExpression *argument = expression->arguments; argument != NULL;
         argument = argument->next) validate_expression(analyzer, argument, 0);
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_AMPERSAND &&
        (expression->right == NULL ||
         (expression->right->kind != AST_EXPR_NAME &&
          expression->right->kind != AST_EXPR_INDEX) ||
         expression_is_constant_symbol(analyzer, expression->right))) {
        semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                       "Address-of requires a mutable lvalue");
    } else if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR &&
        expression->right != NULL && expression->right->resolved_pointer_depth == 0 &&
        expression->right->resolved_outer_pointer_depth == 0) {
        semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                       "Dereference requires a pointer");
    } else if (expression->kind == AST_EXPR_UNARY &&
               (expression->operator_type == TOKEN_MINUS ||
                expression->operator_type == TOKEN_BANG) &&
               !plain_numeric_expression(expression->right)) {
        semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                       "Unary numeric operator requires a numeric operand");
    } else if (expression->kind == AST_EXPR_UNARY &&
               expression->operator_type == TOKEN_MINUS && expression->right != NULL &&
               expression->right->resolved_type == TYPE_BIT) {
        semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
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
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Pointer arithmetic is not supported");
        if (operation == TOKEN_PERCENT &&
            (expression->left->resolved_type == TYPE_FLOAT ||
             expression->left->resolved_type == TYPE_DOUBLE ||
             expression->right->resolved_type == TYPE_FLOAT ||
             expression->right->resolved_type == TYPE_DOUBLE))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Remainder requires integer operands");
        if (string_concat &&
            (!(string_expression(expression->left) ||
               plain_numeric_expression(expression->left)) ||
             !(string_expression(expression->right) ||
               plain_numeric_expression(expression->right))))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "String concatenation requires scalar operands");
        else if (arithmetic && !string_concat &&
                 (!plain_numeric_expression(expression->left) ||
                  !plain_numeric_expression(expression->right)))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Arithmetic requires numeric operands");
        else if (arithmetic && !string_concat &&
                 (expression->left->resolved_type == TYPE_BIT ||
                  expression->right->resolved_type == TYPE_BIT))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Boolean values do not support arithmetic");
        if (logical && (!plain_numeric_expression(expression->left) ||
                        !plain_numeric_expression(expression->right)))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Logical operators require numeric operands");
        if (relational && (!plain_numeric_expression(expression->left) ||
                           !plain_numeric_expression(expression->right)))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
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
                    SEMANTIC_SYMBOL_ENUM;
            if (!numeric && !strings && !pointers && !named)
                semantic_error(analyzer, expression->first_token,
                               ERR_TYPE_INCOMPATIBLE_TYPES,
                               "Equality comparison requires compatible operands");
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        if (!pointer_expression(expression->left))
            semantic_error(analyzer, expression->first_token, ERR_SEM_NOT_ARRAY,
                           "Indexing requires an array or pointer");
        if (!integral_expression(expression->right))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Array index must be integral");
    } else if (expression->kind == AST_EXPR_CAST) {
        const AstExpression *argument = expression->arguments;
        if (argument == NULL || argument->next != NULL ||
            !plain_numeric_expression(argument) ||
            expression->resolved_type == TYPE_VOID ||
            expression->resolved_type == TYPE_STRING)
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "Cast requires one numeric value and a numeric target type");
    } else if (expression->kind == AST_EXPR_FREE) {
        const AstExpression *argument = expression->arguments;
        if (argument == NULL || argument->next != NULL ||
            (!pointer_expression(argument) && argument->resolved_type != TYPE_STRING))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "free requires one pointer or owned string");
    } else if (expression->kind == AST_EXPR_RESERVE) {
        const AstType *allocated = &expression->allocated_type;
        if (!known_declared_type(analyzer, allocated) || allocated->is_slice ||
            (primitive_type(analyzer->program, allocated) == TYPE_VOID &&
             allocated->pointer_depth == 0 && allocated->outer_pointer_depth == 0))
            semantic_error(analyzer, expression->first_token, ERR_TYPE_INVALID_OPERATION,
                           "reserve requires one complete sized non-void type");
    }
    if (expression->kind == AST_EXPR_NAME && !is_callee &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        TokenType type = analyzer->program->tokens[expression->value_token].type;
        if (strcmp(name, "true") != 0 && strcmp(name, "false") != 0 &&
            !is_builtin_name(name) && !(type >= TOKEN_TYPE_INT && type <= TOKEN_TYPE_VOID))
            semantic_error(analyzer, expression->value_token, ERR_SEM_UNDEFINED_VARIABLE,
                           "Undefined variable");
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        long long index = 0;
        if (constant_integer(analyzer, expression->right, &index) &&
            expression->left->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *base =
                &analyzer->model->symbols[expression->left->resolved_symbol_id];
            if (base->declared_type.is_array &&
                base->declared_type.array_length_token < analyzer->program->token_count) {
                long long length = strtoll(ast_program_lexeme(analyzer->program,
                    base->declared_type.array_length_token), NULL, 10);
                if (index < 0 || index >= length)
                    semantic_error(analyzer, expression->right->value_token,
                                   ERR_SEM_NOT_ARRAY,
                                   "Array index is outside declared bounds");
            }
        }
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL &&
               expression->resolved_symbol_id == AST_SYMBOL_NONE) {
        if (expression->left->resolved_named_symbol_id == AST_SYMBOL_NONE) {
            semantic_error(analyzer, expression->value_token, ERR_SEM_NOT_STRUCT,
                           "Member access target is not a struct");
        } else {
            semantic_error(analyzer, expression->value_token, ERR_SEM_FIELD_NOT_FOUND,
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
        semantic_error(analyzer, expression->value_token, ERR_TYPE_INVALID_OPERATION,
                       "Instance field requires a value receiver");
    }
}

static int statement_always_returns(const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_RETURN) return 1;
        if (statement->kind == AST_STMT_BLOCK && statement_always_returns(statement->body))
            return 1;
        if (statement->kind == AST_STMT_IF && statement->else_body != NULL &&
            statement_always_returns(statement->body) &&
            statement_always_returns(statement->else_body)) return 1;
    }
    return 0;
}

static int known_declared_type(const Analyzer *analyzer, const AstType *type) {
    if (type == NULL || type->kind == AST_TYPE_INFERRED ||
        primitive_type(analyzer->program, type) != TYPE_UNKNOWN) return 1;
    return resolve_named_symbol_id(analyzer, analyzer->program,
                                   named_type_token(analyzer->program, type)) != AST_SYMBOL_NONE;
}

static int aggregate_reaches(const Analyzer *analyzer, size_t current_symbol,
                             size_t target_symbol, size_t depth) {
    if (current_symbol >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count) return 1;
    const SemanticSymbol *current = &analyzer->model->symbols[current_symbol];
    if (current->kind != SEMANTIC_SYMBOL_STRUCT || current->declaration == NULL) return 0;
    for (const AstField *field = current->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        if (field->type.pointer_depth != 0 || field->type.outer_pointer_depth != 0 ||
            field->type.is_slice) continue;
        size_t named = named_type_token(current->source_program, &field->type);
        size_t child = resolve_named_symbol_id(analyzer, current->source_program, named);
        if (child == target_symbol ||
            (child != AST_SYMBOL_NONE &&
             aggregate_reaches(analyzer, child, target_symbol, depth + 1U))) return 1;
    }
    return 0;
}

static size_t semantic_symbol_slots(const Analyzer *analyzer, size_t symbol_id,
                                    size_t depth) {
    if (symbol_id >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count) return SIZE_MAX;
    const SemanticSymbol *symbol = &analyzer->model->symbols[symbol_id];
    if (symbol->kind == SEMANTIC_SYMBOL_ENUM) return 1;
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL) return 1;
    size_t slots = 0;
    for (const AstField *field = symbol->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        size_t field_slots = 1;
        if (field->type.pointer_depth == 0 && field->type.outer_pointer_depth == 0 &&
            !field->type.is_slice) {
            size_t named = named_type_token(symbol->source_program, &field->type);
            size_t child = resolve_named_symbol_id(analyzer, symbol->source_program, named);
            if (child != AST_SYMBOL_NONE)
                field_slots = semantic_symbol_slots(analyzer, child, depth + 1U);
        }
        if (field->type.is_array &&
            field->type.array_length_token < symbol->source_program->token_count) {
            size_t length = (size_t) strtoull(ast_program_lexeme(symbol->source_program,
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
    if (type->outer_pointer_depth != 0 ||
        (type->pointer_depth != 0 && !type->is_array && !type->is_slice) || type->is_slice ||
        (type->kind == AST_TYPE_INFERRED && inferred != NULL &&
         (inferred->resolved_pointer_depth != 0 ||
          inferred->resolved_outer_pointer_depth != 0))) return type->is_slice ? 2U : 1U;
    size_t slots = 1;
    size_t named_symbol = type->kind == AST_TYPE_INFERRED && inferred != NULL
        ? inferred->resolved_named_symbol_id
        : resolve_named_symbol_id(analyzer, program, named_type_token(program, type));
    if (type->pointer_depth != 0) slots = 1;
    else if (named_symbol != AST_SYMBOL_NONE)
        slots = semantic_symbol_slots(analyzer, named_symbol, 0);
    if (type->is_array && type->array_length_token < program->token_count) {
        size_t length = (size_t) strtoull(ast_program_lexeme(program,
            type->array_length_token), NULL, 10);
        if (slots != 0 && length > SIZE_MAX / slots) return SIZE_MAX;
        slots *= length;
    }
    return slots;
}

static int assignable_expression(const Analyzer *analyzer,
                                 const AstExpression *expression) {
    return expression != NULL &&
        !expression_is_constant_symbol(analyzer, expression) &&
        (expression->kind == AST_EXPR_NAME || expression->kind == AST_EXPR_INDEX ||
         expression->kind == AST_EXPR_MEMBER ||
         (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR));
}

static void validate_array_shape(Analyzer *analyzer, const AstType *type) {
    if (type == NULL || !type->is_array) return;
    if (type->array_length_token >= analyzer->program->token_count) {
        semantic_error(analyzer, type->name_token, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be a compile-time integer");
        return;
    }
    unsigned long long length = strtoull(ast_program_lexeme(analyzer->program,
        type->array_length_token), NULL, 10);
    if (length == 0 || length > 1048576ULL)
        semantic_error(analyzer, type->array_length_token, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be positive and fit local storage");
}

static void analyze_expression(Analyzer *analyzer, AstExpression *expression) {
    if (expression == NULL) return;
    analyze_expression(analyzer, expression->left);
    analyze_expression(analyzer, expression->right);
    for (AstExpression *argument = expression->arguments; argument != NULL; argument = argument->next)
        analyze_expression(analyzer, argument);

    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_pointer_depth = 0;
    expression->resolved_outer_pointer_depth = 0;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_is_array = 0;
    expression->resolved_is_slice = 0;
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
                expression->resolved_outer_pointer_depth = local->resolved_outer_pointer_depth;
                expression->resolved_named_type_token = local->resolved_named_type_token;
                expression->resolved_named_symbol_id = local->resolved_named_symbol_id;
                expression->resolved_is_array = local->resolved_is_array;
                expression->resolved_is_slice = local->resolved_is_slice;
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
                const SemanticSymbol *constant = semantic_find_global(analyzer->model, name,
                                                                        SEMANTIC_SYMBOL_CONSTANT);
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
            }
            else if (expression->operator_type == TOKEN_STAR) {
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
                int ambiguous = 0;
                const SemanticSymbol *function = resolve_overload(analyzer, name,
                    AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
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
                    if (strcmp(name, "read") == 0 && expression->arguments != NULL &&
                        expression->arguments->next != NULL) {
                        const AstExpression *format = expression->arguments->next;
                        const char *format_text = format->kind == AST_EXPR_LITERAL
                            ? ast_program_lexeme(analyzer->program, format->value_token) : "";
                        expression->resolved_type = strcmp(format_text, "%c") == 0
                            ? TYPE_CHAR : TYPE_INT;
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
            int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                     SEMANTIC_SYMBOL_STRUCT ||
                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                     SEMANTIC_SYMBOL_ENUM);
            int ambiguous = 0;
            const SemanticSymbol *method = resolve_overload(analyzer,
                ast_program_lexeme(analyzer->program, expression->left->value_token),
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
                    method->source_program, expression->resolved_named_type_token);
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
        if (expression->left->resolved_is_slice &&
            same_name(analyzer->program, expression->value_token, "length")) {
            expression->kind = AST_EXPR_SLICE_LENGTH;
            expression->resolved_type = TYPE_INT;
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
            expression->resolved_outer_pointer_depth = field->type.outer_pointer_depth;
            expression->resolved_named_type_token = named_type_token(field_program, &field->type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                field_program, expression->resolved_named_type_token);
            expression->resolved_is_array = field->type.is_array;
            expression->resolved_is_slice = field->type.is_slice;
            expression->resolved_symbol_id = field->resolved_symbol_id;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        const AstType *reserved_type = &expression->allocated_type;
        expression->resolved_type = primitive_type(analyzer->program, reserved_type);
        expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                  reserved_type);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
            analyzer->program, expression->resolved_named_type_token);
        expression->resolved_pointer_depth = reserved_type->pointer_depth;
        expression->resolved_outer_pointer_depth = reserved_type->outer_pointer_depth;
        expression->resolved_is_array = reserved_type->is_array;
        expression->resolved_is_slice = reserved_type->is_slice;
        if (reserved_type->is_array || reserved_type->is_slice)
            expression->resolved_outer_pointer_depth++;
        else expression->resolved_pointer_depth++;
    }
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
        LocalSymbol *scope = analyzer->locals;
        if (statement->kind == AST_STMT_VARIABLE) {
            analyze_expression(analyzer, statement->value);
            validate_expression(analyzer, statement->value, 0);
            if (!known_declared_type(analyzer, &statement->type))
                semantic_error(analyzer, statement->type.name_token, ERR_TYPE_UNKNOWN,
                               "Unknown variable type");
            if (statement->type.kind == AST_TYPE_INFERRED && statement->value == NULL)
                semantic_error(analyzer, statement->name_token, ERR_TYPE_UNKNOWN,
                               "Inferred variable requires an initializer");
            if (statement->type.is_slice)
                semantic_error(analyzer, statement->type.name_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Slice types are only valid for function parameters");
            if (statement->is_const && statement->value == NULL)
                semantic_error(analyzer, statement->name_token,
                               ERR_PARSE_INVALID_DECLARATION,
                               "Constant requires an initializer");
            if (statement->is_const && statement->value != NULL &&
                !constant_expression_allowed(analyzer, statement->value))
                semantic_error(analyzer, statement->value->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Constant initializer is not a constant expression");
            if (statement->is_const &&
                (statement->type.pointer_depth != 0 ||
                 statement->type.outer_pointer_depth != 0 || statement->type.is_array ||
                 statement->type.is_slice ||
                 (statement->value != NULL &&
                  statement->value->resolved_named_symbol_id != AST_SYMBOL_NONE)))
                semantic_error(analyzer, statement->name_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Constants require a primitive or string type");
            if (primitive_type(analyzer->program, &statement->type) == TYPE_VOID &&
                statement->type.pointer_depth == 0)
                semantic_error(analyzer, statement->type.name_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Variable cannot have type void");
            if (statement->type.kind != AST_TYPE_INFERRED && statement->value != NULL) {
                if (!expression_to_declared_type_allowed(analyzer, statement->value,
                                                         analyzer->program,
                                                         &statement->type))
                    semantic_error(analyzer, statement->name_token,
                                   ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Cannot implicitly convert initializer");
            }
            if (statement->type.is_array && statement->value != NULL)
                semantic_error(analyzer, statement->name_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Arrays cannot be initialized by assignment");
            if (same_name(analyzer->program, statement->name_token, "true") ||
                same_name(analyzer->program, statement->name_token, "false"))
                semantic_error(analyzer, statement->name_token,
                               ERR_PARSE_INVALID_DECLARATION,
                               "Reserved name cannot be declared");
            if (statement->type.is_array &&
                statement->type.array_length_token < analyzer->program->token_count) {
                unsigned long long length = strtoull(ast_program_lexeme(analyzer->program,
                    statement->type.array_length_token), NULL, 10);
                if (length == 0 || length > 1048576ULL)
                    semantic_error(analyzer, statement->type.array_length_token,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Array length must be positive and fit local storage");
            }
            size_t slots = semantic_type_slots(analyzer, analyzer->program,
                                               &statement->type, statement->value);
            size_t storage_limit = 8U * 1024U * 1024U;
            if (slots == SIZE_MAX || slots > storage_limit / 8U || analyzer->local_storage >
                storage_limit - slots * 8U) {
                if (!analyzer->storage_error_reported)
                    semantic_error(analyzer, statement->name_token,
                                   ERR_CODEGEN_TOO_MANY_VARIABLES,
                                   "Function local storage exceeds supported limit");
                analyzer->storage_error_reported = 1;
            } else analyzer->local_storage += slots * 8U;
            for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
                 existing = existing->next) {
                if (existing->scope_depth == analyzer->scope_depth &&
                    same_name(analyzer->program, existing->name_token,
                              ast_program_lexeme(analyzer->program, statement->name_token))) {
                    semantic_error(analyzer, statement->name_token,
                                   ERR_PARSE_DUPLICATE_DEFINITION, "Duplicate variable");
                    break;
                }
            }
            LocalSymbol *local = push_local(analyzer, statement->name_token, statement->type,
                                            statement->is_const ? SEMANTIC_SYMBOL_CONSTANT :
                                                                  SEMANTIC_SYMBOL_LOCAL,
                                            statement->value, statement->is_const);
            if (local != NULL) statement->resolved_symbol_id = local->symbol_id;
        } else if (statement->kind == AST_STMT_FOR) {
            analyzer->scope_depth++;
            analyze_statement(analyzer, statement->initializer);
            analyze_expression(analyzer, statement->condition);
            validate_expression(analyzer, statement->condition, 0);
            if (statement->condition != NULL &&
                !plain_numeric_expression(statement->condition))
                semantic_error(analyzer, statement->condition->first_token,
                               ERR_TYPE_INVALID_OPERATION,
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
                semantic_error(analyzer, statement->condition->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Condition requires a numeric or bit expression");
            if (statement->kind == AST_STMT_ASSIGNMENT &&
                !assignable_expression(analyzer, statement->expression))
                semantic_error(analyzer, statement->first_token,
                               ERR_PARSE_UNEXPECTED_TOKEN,
                               "Unexpected statement: Assignment requires an assignable target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->value != NULL &&
                !expression_assignment_allowed(analyzer, statement->value,
                                               statement->expression))
                semantic_error(analyzer, statement->first_token,
                               ERR_TYPE_INCOMPATIBLE_TYPES,
                               "Cannot implicitly convert assigned value");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_is_array)
                semantic_error(analyzer, statement->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Arrays cannot be assigned as values");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_named_symbol_id != AST_SYMBOL_NONE &&
                statement->expression->resolved_named_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[
                    statement->expression->resolved_named_symbol_id].kind ==
                        SEMANTIC_SYMBOL_STRUCT &&
                statement->assignment_operator != TOKEN_EQUAL)
                semantic_error(analyzer, statement->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Structures only support simple assignment");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->assignment_operator != TOKEN_EQUAL &&
                !plain_numeric_expression(statement->expression))
                semantic_error(analyzer, statement->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Compound assignment requires a numeric target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_type == TYPE_BIT &&
                statement->assignment_operator != TOKEN_EQUAL)
                semantic_error(analyzer, statement->first_token,
                               ERR_TYPE_INVALID_OPERATION,
                               "Boolean values do not support arithmetic assignment");
            if (statement->kind == AST_STMT_BREAK && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERR_SEM_BREAK_OUTSIDE_LOOP, "Break used outside a loop");
            if (statement->kind == AST_STMT_CONTINUE && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERR_SEM_CONTINUE_OUTSIDE_LOOP, "Continue used outside a loop");
            if (statement->kind == AST_STMT_RETURN && analyzer->current_function != NULL) {
                DataType expected = primitive_type(analyzer->program,
                    &analyzer->current_function->as.function.return_type);
                if (expected == TYPE_VOID && statement->value != NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Void function cannot return a value");
                else if (expected != TYPE_VOID && statement->value == NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Function must return a value");
                else if (statement->value != NULL &&
                         !expression_to_declared_type_allowed(analyzer, statement->value,
                            analyzer->program,
                            &analyzer->current_function->as.function.return_type))
                    semantic_error(analyzer, statement->first_token,
                                   ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Cannot implicitly convert returned value");
            }
            analyze_statement(analyzer, statement->initializer);
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth++;
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE) analyzer->scope_depth++;
            analyze_statement(analyzer, statement->body);
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE) analyzer->scope_depth--;
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth--;
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
                       ERR_TYPE_UNKNOWN, "Unknown function return type");
    if (function->as.function.return_type.is_array ||
        function->as.function.return_type.is_slice)
        semantic_error(analyzer, function->as.function.return_type.name_token,
                       ERR_TYPE_INVALID_OPERATION,
                       "Array and slice return types are not supported");
    analyzer->scope_depth++;
    for (AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        if (!known_declared_type(analyzer, &parameter->type))
            semantic_error(analyzer, parameter->type.name_token,
                           ERR_TYPE_UNKNOWN, "Unknown parameter type");
        if (parameter->type.is_array)
            semantic_error(analyzer, parameter->type.array_length_token,
                           ERR_TYPE_INVALID_OPERATION,
                           "Array parameters use an unsized slice type T[]");
        if (primitive_type(analyzer->program, &parameter->type) == TYPE_VOID &&
            parameter->type.pointer_depth == 0)
            semantic_error(analyzer, parameter->type.name_token,
                           ERR_TYPE_INVALID_OPERATION,
                           "Parameter cannot have type void");
        for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
             existing = existing->next)
            if (existing->scope_depth == analyzer->scope_depth &&
                same_name(analyzer->program, existing->name_token,
                          ast_program_lexeme(analyzer->program,
                                             parameter->name_token))) {
                semantic_error(analyzer, parameter->name_token,
                               ERR_PARSE_DUPLICATE_DEFINITION,
                               "Duplicate parameter");
                break;
            }
        if (same_name(analyzer->program, parameter->name_token, "true") ||
            same_name(analyzer->program, parameter->name_token, "false"))
            semantic_error(analyzer, parameter->name_token,
                           ERR_PARSE_INVALID_DECLARATION,
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
        semantic_error(analyzer, function->name_token, ERR_TYPE_INCOMPATIBLE_TYPES,
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
    AstExpression *value = declaration->as.constant.value;
    AstType *type = &declaration->as.constant.type;
    analyze_expression(analyzer, value);
    validate_expression(analyzer, value, 0);
    if (value == NULL || !constant_expression_allowed(analyzer, value))
        semantic_error(analyzer, declaration->name_token, ERR_TYPE_INVALID_OPERATION,
                       "Constant initializer is not a constant expression");
    if (!known_declared_type(analyzer, type))
        semantic_error(analyzer, type->name_token, ERR_TYPE_UNKNOWN,
                       "Unknown constant type");
    if (type->pointer_depth != 0 || type->outer_pointer_depth != 0 ||
        type->is_array || type->is_slice ||
        (value != NULL && value->resolved_named_symbol_id != AST_SYMBOL_NONE))
        semantic_error(analyzer, declaration->name_token, ERR_TYPE_INVALID_OPERATION,
                       "Constants require a primitive or string type");
    if (type->kind != AST_TYPE_INFERRED && value != NULL &&
        !expression_to_declared_type_allowed(analyzer, value, analyzer->program, type))
        semantic_error(analyzer, declaration->name_token, ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Cannot implicitly convert constant initializer");
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
    analyzer.program = program;
    validate_overload_sets(&analyzer);
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL) continue;
        analyzer.program = (AstProgram *) symbol->source_program;
        if (aggregate_reaches(&analyzer, symbol->id, symbol->id, 0))
            semantic_error(&analyzer, symbol->name_token, ERR_TYPE_INVALID_OPERATION,
                           "Recursive structure requires a pointer field");
    }
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root;
             declaration != NULL; declaration = declaration->next)
            if (declaration->kind == AST_DECL_CONSTANT)
                analyze_constant_declaration(&analyzer, declaration);
    }
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->kind == AST_DECL_FUNCTION)
                analyze_function(&analyzer, declaration);
            else if (declaration->kind == AST_DECL_STRUCT) {
                for (AstField *field = declaration->as.struct_decl.fields;
                     field != NULL; field = field->next) {
                    if (!known_declared_type(&analyzer, &field->type))
                        semantic_error(&analyzer, field->type.name_token, ERR_TYPE_UNKNOWN,
                                       "Unknown field type");
                    if (field->type.is_slice)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Slice fields are not supported");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERR_TYPE_INVALID_OPERATION,
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
                        semantic_error(&analyzer, field->type.name_token, ERR_TYPE_UNKNOWN,
                                       "Unknown enum field type");
                    if (field->type.is_slice)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Slice fields are not supported");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Enum field cannot have type void");
                    if (primitive_type(unit, &field->type) == TYPE_UNKNOWN ||
                        field->type.pointer_depth != 0 || field->type.is_array)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Enum fields require scalar primitive types");
                    validate_array_shape(&analyzer, &field->type);
                }
                size_t field_count = 0;
                for (AstField *field = declaration->as.enum_decl.fields;
                     field != NULL; field = field->next) field_count++;
                for (AstEnumValue *value = declaration->as.enum_decl.values;
                     value != NULL; value = value->next) {
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
                                       ERR_SEM_WRONG_ARG_COUNT, message);
                    } else {
                        AstExpression *argument = value->arguments;
                        AstField *field = declaration->as.enum_decl.fields;
                        for (; argument != NULL && field != NULL;
                             argument = argument->next, field = field->next) {
                            if (!enum_constant_expression(&analyzer, argument))
                                semantic_error(&analyzer, argument->first_token,
                                    ERR_TYPE_INVALID_OPERATION,
                                    "Enum arguments must be compile-time scalar constants");
                            if (!expression_to_declared_type_allowed(&analyzer, argument,
                                                                    unit, &field->type))
                                semantic_error(&analyzer, argument->value_token,
                                    ERR_TYPE_INCOMPATIBLE_TYPES,
                                    "Cannot implicitly convert enum argument");
                        }
                    }
                }
            }
        }
    }
    const SemanticSymbol *main_symbol = semantic_find_global(model, "main",
                                                              SEMANTIC_SYMBOL_FUNCTION);
    if (main_symbol == NULL || main_symbol->declaration == NULL) {
        analyzer.program = program;
        semantic_error(&analyzer, AST_TOKEN_NONE, ERR_SEM_UNDEFINED_FUNCTION,
                       "Program must define main");
    } else {
        analyzer.program = (AstProgram *) main_symbol->source_program;
        const AstDeclarationNode *main_declaration = main_symbol->declaration;
        DataType main_type = primitive_type(main_symbol->source_program,
            &main_declaration->as.function.return_type);
        if (main_declaration->as.function.parameters != NULL ||
            main_declaration->as.function.return_type.pointer_depth != 0 ||
            (main_type != TYPE_VOID && main_type != TYPE_INT))
            semantic_error(&analyzer, main_symbol->name_token,
                           ERR_TYPE_INCOMPATIBLE_TYPES,
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
