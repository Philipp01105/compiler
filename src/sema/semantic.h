#ifndef DMM_SEMANTIC_H
#define DMM_SEMANTIC_H

#include <stddef.h>

#include "ast.h"

typedef enum {
    SEMANTIC_SYMBOL_FUNCTION,
    SEMANTIC_SYMBOL_STRUCT,
    SEMANTIC_SYMBOL_ENUM,
    SEMANTIC_SYMBOL_PARAMETER,
    SEMANTIC_SYMBOL_LOCAL
} SemanticSymbolKind;

typedef struct {
    size_t id;
    SemanticSymbolKind kind;
    size_t name_token;
    size_t owner_token;
    AstType declared_type;
    const AstDeclarationNode *declaration;
    size_t scope_depth;
} SemanticSymbol;

typedef struct {
    const AstProgram *program;
    SemanticSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    size_t unresolved_expression_count;
    size_t duplicate_symbol_count;
} SemanticModel;

SemanticModel *semantic_analyze(AstProgram *program);
void semantic_model_free(SemanticModel *model);
const SemanticSymbol *semantic_find_global(const SemanticModel *model,
                                           const char *name,
                                           SemanticSymbolKind kind);

#endif
