#ifndef DMM_SEMANTIC_H
#define DMM_SEMANTIC_H

#include <stddef.h>
#include <stdio.h>

#include "ast.h"

typedef enum {
    SEMANTIC_SYMBOL_FUNCTION,
    SEMANTIC_SYMBOL_STRUCT,
    SEMANTIC_SYMBOL_ENUM,
    SEMANTIC_SYMBOL_INTERFACE,
    SEMANTIC_SYMBOL_IMPORT,
    SEMANTIC_SYMBOL_FIELD,
    SEMANTIC_SYMBOL_ENUM_VALUE,
    SEMANTIC_SYMBOL_PARAMETER,
    SEMANTIC_SYMBOL_LOCAL,
    SEMANTIC_SYMBOL_CONSTANT, SEMANTIC_SYMBOL_VARIABLE
} SemanticSymbolKind;

typedef struct {
    size_t id;
    const AstProgram *source_program;
    SemanticSymbolKind kind;
    size_t name_token;
    size_t owner_token;
    size_t owner_symbol_id;
    AstType declared_type;
    DataType resolved_type;
    unsigned resolved_pointer_depth;
    unsigned resolved_outer_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    int resolved_is_slice;
    const AstDeclarationNode *declaration;
    const void *node;
    size_t scope_depth;
} SemanticSymbol;

typedef struct {
    const AstProgram *program;
    SemanticSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    size_t *symbol_index;
    size_t symbol_index_capacity;
    size_t unresolved_expression_count;
    size_t duplicate_symbol_count;
    size_t error_count;
} SemanticModel;

SemanticModel *semantic_analyze(AstProgram * program);

int semantic_implements_interface(const SemanticModel *model, size_t interface_id,
                                  size_t struct_id);
size_t semantic_interface_method(const SemanticModel *model, size_t interface_method_id,
                                 size_t struct_id);

void semantic_model_free(SemanticModel *model);

int semantic_dump(FILE *output, const SemanticModel *model);

const SemanticSymbol *semantic_find_global(const SemanticModel *model,
                                           const char *name,
                                           SemanticSymbolKind kind);

const SemanticSymbol *semantic_find_in_package(const SemanticModel *model, const AstProgram *file,
                                               const char *name, SemanticSymbolKind kind);

#endif
