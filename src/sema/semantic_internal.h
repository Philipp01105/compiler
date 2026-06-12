#ifndef DMM_SEMANTIC_INTERNAL_H
#define DMM_SEMANTIC_INTERNAL_H

#include "semantic.h"

int same_name(const AstProgram *program, size_t token, const char *name);
int same_package(const AstProgram *left, const AstProgram *right);
const char *symbol_name(const SemanticSymbol *symbol);
int symbol_matches_scope(const AstProgram *file, const SemanticSymbol *symbol, const char *name);
const DmmPackage *lookup_package(const AstProgram *file, const char **name);
const SemanticSymbol *scoped_find_global(const SemanticModel *model, const AstProgram *file,
                                         const char *name, SemanticSymbolKind kind);
int semantic_append_symbol(SemanticModel *model, SemanticSymbol symbol);

#endif
