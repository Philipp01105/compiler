#ifndef DMM_AST_OPTIMIZE_H
#define DMM_AST_OPTIMIZE_H

#include "ast.h"

typedef struct {
    size_t constant_branches;
    size_t constant_loops;
    size_t short_circuits;
    size_t unreachable_statements;
} AstOptimizationStats;

/* Requires completed semantic analysis. Mutates only typed function bodies. */
void ast_optimize_program(AstProgram *program, AstOptimizationStats *stats);

#endif
