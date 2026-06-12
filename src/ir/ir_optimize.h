#ifndef DMM_IR_OPTIMIZE_H
#define DMM_IR_OPTIMIZE_H
#include "ir.h"

typedef struct {
    size_t constants_folded, constants_propagated, copies_propagated;
    size_t dead_instructions, dead_stores, branches_folded, blocks_removed;
    size_t addresses_simplified;
} IrOptimizationStats;

/* Requires valid typed IR; validates again after optimization. */
int ir_optimize_module(IrModule * module, IrOptimizationStats * stats);
#endif
