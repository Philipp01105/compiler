#include "semantic_internal.h"

#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int same_name(const AstProgram *program, size_t token, const char *name) {
    if (program == NULL || name == NULL || token >= program->token_count) return 0;
    const char *lexeme = program->tokens[token].lexeme;
    return lexeme == name || strcmp(lexeme, name) == 0;
}

int same_package(const AstProgram *left, const AstProgram *right) {
    return left->package && right->package ? left->package == right->package : left == right;
}

const DmmPackage *lookup_package(const AstProgram *file, const char **name) {
    const char *canonical = strstr(*name, "::");
    if (canonical && file->module) {
        for (DmmPackage *package = file->module->graph->packages; package; package = package->next)
            if (strlen(package->path) == (size_t) (canonical - *name) &&
                strncmp(package->path, *name, (size_t) (canonical - *name)) == 0) {
                *name = canonical + 2;
                return package;
            }
    }
    const char *dot = strchr(*name, '.');
    if (dot) {
        for (AstDeclarationNode *declaration = file->root; declaration; declaration = declaration->next)
            if (declaration->kind == AST_DECL_IMPORT)
                for (AstImportPath *path = declaration->as.import_decl.paths; path; path = path->next)
                    if (path->alias && path->resolved_program && strlen(path->alias) == (size_t) (dot - *name) &&
                        strncmp(path->alias, *name, (size_t) (dot - *name)) == 0) {
                        *name = dot + 1;
                        return path->resolved_program->package;
                    }
        return NULL;
    }
    return file->package;
}

static uintptr_t scope_identity(const AstProgram *program) {
    return (uintptr_t) (program->package ? (const void *) program->package : (const void *) program);
}

static uint64_t symbol_hash(const char *name, SemanticSymbolKind kind, uintptr_t scope) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; name[i] != '\0'; i++) {
        hash ^= (unsigned char) name[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= (uint64_t) kind + UINT64_C(0x9e3779b97f4a7c15);
    hash *= UINT64_C(1099511628211);
    hash ^= (uint64_t) scope;
    hash *= UINT64_C(1099511628211);
    return hash;
}

const char *symbol_name(const SemanticSymbol *symbol) {
    return ast_program_lexeme(symbol->source_program, symbol->name_token);
}

int symbol_matches_scope(const AstProgram *file, const SemanticSymbol *symbol, const char *name) {
    const char *plain = name;
    const DmmPackage *package = lookup_package(file, &plain);
    if ((package != NULL
             ? symbol->source_program->package != package
             : (strchr(name, '.') != NULL || strstr(name, "::") != NULL ||
                !same_package(file, symbol->source_program))) ||
        symbol->owner_symbol_id != AST_SYMBOL_NONE)
        return 0;
    return same_name(symbol->source_program, symbol->name_token, plain);
}

static int index_symbol(SemanticModel *model, size_t symbol_id) {
    const SemanticSymbol *symbol = &model->symbols[symbol_id];
    if (symbol->owner_symbol_id != AST_SYMBOL_NONE) return 1;
    const char *name = symbol_name(symbol);
    size_t mask = model->symbol_index_capacity - 1U;
    size_t slot = (size_t) symbol_hash(name, symbol->kind, scope_identity(symbol->source_program)) & mask;
    while (model->symbol_index[slot] != 0) {
        const SemanticSymbol *existing = &model->symbols[model->symbol_index[slot] - 1U];
        if (existing->kind == symbol->kind && same_package(existing->source_program, symbol->source_program) &&
            same_name(existing->source_program, existing->name_token, name))
            return 1;
        slot = (slot + 1U) & mask;
    }
    model->symbol_index[slot] = symbol_id + 1U;
    return 1;
}

static int grow_symbol_index(SemanticModel *model) {
    size_t capacity = model->symbol_index_capacity == 0 ? 64U : model->symbol_index_capacity * 2U;
    if (capacity < model->symbol_index_capacity || capacity > SIZE_MAX / sizeof(*model->symbol_index)) return 0;
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

int semantic_append_symbol(SemanticModel *model, SemanticSymbol symbol) {
    if (model->symbol_index_capacity == 0 || model->symbol_count + 1U >= model->symbol_index_capacity / 2U)
        if (!grow_symbol_index(model)) return 0;
    if (model->symbol_count == model->symbol_capacity) {
        size_t capacity = model->symbol_capacity == 0 ? 32U : model->symbol_capacity * 2U;
        if (capacity < model->symbol_capacity || capacity > SIZE_MAX / sizeof(*model->symbols)) return 0;
        SemanticSymbol *symbols = realloc(model->symbols, capacity * sizeof(*symbols));
        if (symbols == NULL) return 0;
        model->symbols = symbols;
        model->symbol_capacity = capacity;
    }
    symbol.id = model->symbol_count;
    model->symbols[model->symbol_count++] = symbol;
    return index_symbol(model, symbol.id);
}

unsigned semantic_symbol_type_properties(const SemanticModel *model,
                                         size_t type_symbol_id) {
    if (model == NULL || type_symbol_id >= model->symbol_count)
        return SEMANTIC_TYPE_COPYABLE;
    const SemanticSymbol *symbol = &model->symbols[type_symbol_id];
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE)
        return SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP;
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT &&
        symbol->kind != SEMANTIC_SYMBOL_ENUM)
        return SEMANTIC_TYPE_COPYABLE;
    return symbol->type_properties;
}

static const SemanticSymbol *indexed_find(const SemanticModel *model, const AstProgram *file,
                                          const DmmPackage *package, const char *name, SemanticSymbolKind kind) {
    if (model->symbol_index_capacity == 0) return NULL;
    size_t mask = model->symbol_index_capacity - 1U;
    uintptr_t scope = (uintptr_t) (package != NULL ? (const void *) package : (const void *) file);
    size_t slot = (size_t) symbol_hash(name, kind, scope) & mask;
    while (model->symbol_index[slot] != 0) {
        const SemanticSymbol *symbol = &model->symbols[model->symbol_index[slot] - 1U];
        int same_scope = package != NULL ? symbol->source_program->package == package
                                         : same_package(file, symbol->source_program);
        if (symbol->kind == kind && same_scope &&
            same_name(symbol->source_program, symbol->name_token, name))
            return symbol;
        slot = (slot + 1U) & mask;
    }
    return NULL;
}

const SemanticSymbol *scoped_find_global(const SemanticModel *model, const AstProgram *file,
                                         const char *name, SemanticSymbolKind kind) {
    if (model == NULL || file == NULL || name == NULL) return NULL;
    const char *plain = name;
    const DmmPackage *package = lookup_package(file, &plain);
    const int qualified = strchr(name, '.') != NULL || strstr(name, "::") != NULL;
    if (qualified && package == NULL) return NULL;
    const SemanticSymbol *symbol = indexed_find(model, file, package, plain, kind);
    if (symbol != NULL && strstr(name, "::") == NULL && !same_package(file, symbol->source_program) &&
        symbol->declaration != NULL && !symbol->declaration->is_public) {
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_SEMANTIC,
                     ERR_PACKAGE_PRIVATE, file->source_path, "Symbol '%s' is private to package '%s'", name,
                     symbol->source_program->module_identity);
        ((SemanticModel *) model)->error_count++;
        return NULL;
    }
    return symbol;
}

const SemanticSymbol *semantic_find_global(const SemanticModel *model, const char *name, SemanticSymbolKind kind) {
    return model ? scoped_find_global(model, model->program, name, kind) : NULL;
}

const SemanticSymbol *semantic_find_in_package(const SemanticModel *model, const AstProgram *file, const char *name,
                                               SemanticSymbolKind kind) {
    return scoped_find_global(model, file, name, kind);
}

static int dump_quoted(FILE *output, const char *text) {
    if (fputc('"', output) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *) (text == NULL ? "" : text); *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', output) == EOF || fputc(*p, output) == EOF) return 0;
        } else if (*p == '\n') {
            if (fputs("\\n", output) == EOF) return 0;
        } else if (*p == '\r') {
            if (fputs("\\r", output) == EOF) return 0;
        } else if (*p == '\t') {
            if (fputs("\\t", output) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(output, "\\x%02x", *p) < 0) return 0;
        } else if (fputc(*p, output) == EOF) return 0;
    }
    return fputc('"', output) != EOF;
}

static const char *symbol_kind_name(SemanticSymbolKind kind) {
    static const char *names[] = {
        "function", "struct", "enum", "interface", "import", "field", "enum-value",
        "parameter", "local", "constant", "variable"
    };
    return kind >= SEMANTIC_SYMBOL_FUNCTION && kind <= SEMANTIC_SYMBOL_VARIABLE ? names[kind] : "invalid";
}

static const char *resolved_type_name(DataType type) {
    static const char *names[] = {DMM_TYPE_NAMES};
    return type >= TYPE_INT && type <= TYPE_UNKNOWN ? names[type] : "invalid";
}

int semantic_dump(FILE *output, const SemanticModel *model) {
    if (output == NULL || model == NULL) return 0;
    size_t indexed = 0;
    for (size_t i = 0; i < model->symbol_index_capacity; i++) indexed += model->symbol_index[i] != 0;
    if (fputs("dmm-symbols-v1\nmodule path=", output) == EOF ||
        !dump_quoted(output, model->program->source_path) ||
        fprintf(output,
                " symbols=%zu index-capacity=%zu indexed=%zu unresolved-expressions=%zu duplicates=%zu errors=%zu\n",
                model->symbol_count, model->symbol_index_capacity, indexed, model->unresolved_expression_count,
                model->duplicate_symbol_count, model->error_count) < 0)
        return 0;
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if (fprintf(output, "symbol #%zu kind=%s name=", symbol->id, symbol_kind_name(symbol->kind)) < 0 ||
            !dump_quoted(output, symbol_name(symbol)) || fputs(" source=", output) == EOF ||
            !dump_quoted(output, symbol->source_program->source_path) ||
            fprintf(output, " token=%zu owner=", symbol->name_token) < 0)
            return 0;
        if (symbol->owner_symbol_id == AST_SYMBOL_NONE) {
            if (fputc('-', output) == EOF) return 0;
        } else if (fprintf(output, "%zu", symbol->owner_symbol_id) < 0) return 0;
        if (fprintf(output, " scope=%zu type=%s pointers=%u outer-pointers=%u named=",
                    symbol->scope_depth, resolved_type_name(symbol->resolved_type),
                    symbol->resolved_pointer_depth, symbol->resolved_outer_pointer_depth) < 0)
            return 0;
        if (symbol->resolved_named_symbol_id == AST_SYMBOL_NONE) {
            if (fputc('-', output) == EOF) return 0;
        } else if (fprintf(output, "%zu", symbol->resolved_named_symbol_id) < 0) return 0;
        if (fprintf(output, " array=%d slice=%d declaration=%d properties=%s%s\n",
                    symbol->resolved_is_array, symbol->resolved_is_slice,
                    symbol->declaration != NULL,
                    (symbol->type_properties & SEMANTIC_TYPE_MOVE_ONLY)
                        ? "MOVE_ONLY" : "COPYABLE",
                    (symbol->type_properties & SEMANTIC_TYPE_NEEDS_DROP)
                        ? "|NEEDS_DROP" : "") < 0)
            return 0;
    }
    return !ferror(output);
}
