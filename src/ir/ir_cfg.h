#ifndef DMM_IR_CFG_H
#define DMM_IR_CFG_H
#include "ir.h"

typedef struct {
    size_t begin, end;
    size_t successor[2];
    size_t predecessor;
    size_t predecessor_count;
    int reachable;
} IrCfgBlock;

typedef struct {
    size_t block, next;
} IrCfgEdge;

typedef struct {
    IrCfgBlock *blocks;
    IrCfgEdge *edges;
    size_t count;
    size_t *owner;
    size_t *labels;
    size_t *definitions;
} IrControlFlowGraph;

int ir_cfg_build(const IrFunction *function, IrControlFlowGraph *graph);
void ir_cfg_free(IrControlFlowGraph *graph);
int ir_opcode_is_terminator(IrOpcode opcode);

int ir_verify_control_flow(const IrFunction *function, int implicit_void_return);
#endif
