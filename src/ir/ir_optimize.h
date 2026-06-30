#ifndef DMM_IR_OPTIMIZE_H
#define DMM_IR_OPTIMIZE_H
#include "ir.h"
#include <stdio.h>

typedef struct {
    size_t constants_folded, constants_propagated, copies_propagated;
    size_t dead_instructions, dead_stores, branches_folded, blocks_removed;
    size_t addresses_simplified;
    size_t common_expressions, bounds_checks_reused, jumps_threaded;
    size_t dead_functions;
    size_t loop_invariants_hoisted;
} IrOptimizationStats;

/* Requires valid typed IR; validates again after optimization. */
int ir_optimize_module(IrModule * module, IrOptimizationStats * stats);

/* Writes a validated snapshot after every pass type and fixed-point iteration. */
int ir_optimize_module_traced(IrModule *module, IrOptimizationStats *stats, FILE *trace);
#endif
