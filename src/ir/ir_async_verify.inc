/* Exactly-once consuming effects on owned SSA temporaries. Definitions reset
   the state on loop re-entry; joins retain consumption on either path. Local
   cleanup is guarded by explicit drop flags and is checked by source flow. */
static int verify_async_effects(const IrModule *module, const IrFunction *function) {
    if (!function->is_async) return function->async_state_count == 0;
    if (function->async_frame_pinned != 1 || function->future_type_id >= module->type_count ||
        module->types[function->future_type_id].kind != IR_TYPE_FUTURE ||
        module->types[function->future_type_id].element_type != function->return_type_id) return 0;
    unsigned char *suspensions=calloc(function->async_state_count+1,1);
    if(!suspensions) return 0;
    int cancel_entry=0, transitions_valid=1;
    for(size_t i=0;i<function->instruction_count;i++) {
        const IrInstruction *in=&function->instructions[i];
        if(in->opcode==IR_OP_LABEL && in->target_a==function->async_cancel_entry) cancel_entry=1;
        if(in->opcode!=IR_OP_AWAIT && in->opcode!=IR_OP_CANCEL_AWAIT && in->opcode!=IR_OP_CANCEL_DROP) continue;
        if(!in->target_a || in->target_a>function->async_state_count || suspensions[in->target_a]) {transitions_valid=0; break;}
        suspensions[in->target_a]=1;
        if(in->opcode==IR_OP_AWAIT && !in->async_cleanup) {
            int edge=0;
            for(size_t j=0;j<function->instruction_count;j++)
                if(function->instructions[j].opcode==IR_OP_LABEL && function->instructions[j].target_a==in->target_b &&
                   j+1<function->instruction_count && function->instructions[j+1].opcode==IR_OP_CANCEL_AWAIT &&
                   function->instructions[j+1].operand_a==in->operand_a) edge=1;
            if(!edge) transitions_valid=0;
        }
    }
    free(suspensions);
    if(!transitions_valid || !cancel_entry) return 0;
    IrControlFlowGraph graph;
    if (!ir_cfg_build(function, &graph)) return 0;
    size_t count = function->next_value;
    if (count == 0 || graph.count == 0) { ir_cfg_free(&graph); return 1; }
    if (graph.count > SIZE_MAX / count) { ir_cfg_free(&graph); return 0; }
    unsigned char *entry = calloc(graph.count * count, 1);
    unsigned char *state = calloc(count, 1);
    if (entry == NULL || state == NULL) {
        free(entry); free(state); ir_cfg_free(&graph); return 0;
    }
    int changed = 1, valid = 1;
    while (changed && valid) {
        changed = 0;
        for (size_t b = 0; b < graph.count && valid; b++) {
            if (!graph.blocks[b].reachable) continue;
            memcpy(state, entry + b * count, count);
            for (size_t i = graph.blocks[b].begin; i < graph.blocks[b].end; i++) {
                const IrInstruction *in = &function->instructions[i];
                if (in->result != IR_VALUE_NONE) state[in->result] = 0;
                if (in->opcode == IR_OP_AWAIT || in->opcode == IR_OP_CANCEL_AWAIT ||
                    (in->opcode==IR_OP_EXECUTOR && (in->async_operation==ASYNC_SPAWN || in->async_operation==ASYNC_CANCEL || in->async_operation==ASYNC_BLOCK_ON || in->async_operation==ASYNC_COMPLETE || in->async_operation==ASYNC_CANCEL_COMPLETE)) ||
                    (in->opcode == IR_OP_DROP && in->operand_a != IR_VALUE_NONE)) {
                    if (state[in->operand_a]) { valid = 0; break; }
                    state[in->operand_a] = 1;
                }
            }
            for (size_t edge = 0; edge < 2; edge++) {
                size_t next = graph.blocks[b].successor[edge];
                if (next == IR_VALUE_NONE) continue;
                for (size_t v = 0; v < count; v++)
                    if (state[v] && !entry[next * count + v]) {
                        entry[next * count + v] = 1;
                        changed = 1;
                    }
            }
        }
    }
    free(entry); free(state); ir_cfg_free(&graph);
    return valid;
}
