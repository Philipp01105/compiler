#include "ir_cfg.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int ir_opcode_is_terminator(IrOpcode opcode) {
    return opcode == IR_OP_BRANCH || opcode == IR_OP_JUMP || opcode == IR_OP_TRAP || opcode == IR_OP_RETURN;
}

void ir_cfg_free(IrControlFlowGraph *graph) {
    if (graph == NULL) return;
    free(graph->blocks);
    free(graph->edges);
    free(graph->owner);
    free(graph->labels);
    free(graph->definitions);
    memset(graph, 0, sizeof(*graph));
}

int ir_cfg_build(const IrFunction *function, IrControlFlowGraph *graph) {
    if (function == NULL || graph == NULL) return 0;
    memset(graph, 0, sizeof(*graph));
    size_t n = function->instruction_count;
    if (n == 0) return 1;
    if (n > SIZE_MAX / sizeof(*graph->blocks) || n > SIZE_MAX / (2U * sizeof(*graph->edges)) ||
        n > SIZE_MAX / sizeof(*graph->owner) || function->next_label > SIZE_MAX / sizeof(*graph->labels) ||
        function->next_value > SIZE_MAX / sizeof(*graph->definitions))
        return 0;
    graph->blocks = calloc(n, sizeof(*graph->blocks));
    graph->edges = malloc(2U * n * sizeof(*graph->edges));
    graph->owner = malloc(n * sizeof(*graph->owner));
    graph->labels = malloc(function->next_label * sizeof(*graph->labels));
    graph->definitions = malloc(function->next_value * sizeof(*graph->definitions));
    if (graph->blocks == NULL || graph->edges == NULL || graph->owner == NULL ||
        (function->next_label != 0 && graph->labels == NULL) ||
        (function->next_value != 0 && graph->definitions == NULL)) {
        ir_cfg_free(graph);
        return 0;
    }
    for (size_t i = 0; i < function->next_label; i++) graph->labels[i] = IR_VALUE_NONE;
    for (size_t i = 0; i < function->next_value; i++) graph->definitions[i] = IR_VALUE_NONE;

    int phi_region = 0;
    for (size_t i = 0; i < n; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        if (i == 0 || instruction->opcode == IR_OP_LABEL) {
            if (graph->count != 0) graph->blocks[graph->count - 1].end = i;
            IrCfgBlock *block = &graph->blocks[graph->count++];
            block->begin = i;
            block->predecessor = block->successor[0] = block->successor[1] = IR_VALUE_NONE;
            phi_region = instruction->opcode == IR_OP_LABEL;
        } else if (ir_opcode_is_terminator(function->instructions[i - 1].opcode)) goto fail;
        if (instruction->opcode == IR_OP_PHI) {
            if (!phi_region) goto fail;
        } else if (instruction->opcode != IR_OP_LABEL) phi_region = 0;
        graph->owner[i] = graph->count - 1;
        if (instruction->opcode == IR_OP_LABEL) graph->labels[instruction->target_a] = graph->count - 1;
        if (instruction->result != IR_VALUE_NONE) graph->definitions[instruction->result] = i;
    }
    graph->blocks[graph->count - 1].end = n;

    size_t edge_count = 0;
    for (size_t b = 0; b < graph->count; b++) {
        IrCfgBlock *block = &graph->blocks[b];
        const IrInstruction *last = &function->instructions[block->end - 1];
        if (last->opcode == IR_OP_BRANCH || last->opcode == IR_OP_JUMP)
            block->successor[0] = graph->labels[last->target_a];
        else if (!ir_opcode_is_terminator(last->opcode) && b + 1 < graph->count)
            block->successor[0] = b + 1;
        if (last->opcode == IR_OP_BRANCH && last->target_b != last->target_a)
            block->successor[1] = graph->labels[last->target_b];
        for (size_t s = 0; s < 2; s++) {
            size_t target = block->successor[s];
            if (target == IR_VALUE_NONE) continue;
            graph->edges[edge_count] = (IrCfgEdge){b, graph->blocks[target].predecessor};
            graph->blocks[target].predecessor = edge_count++;
            graph->blocks[target].predecessor_count++;
        }
    }

    size_t *queue = malloc(graph->count * sizeof(*queue));
    if (queue == NULL) goto fail;
    size_t queued = 1;
    queue[0] = 0;
    graph->blocks[0].reachable = 1;
    for (size_t q = 0; q < queued; q++)
        for (size_t s = 0; s < 2; s++) {
            size_t target = graph->blocks[queue[q]].successor[s];
            if (target != IR_VALUE_NONE && !graph->blocks[target].reachable) {
                graph->blocks[target].reachable = 1;
                queue[queued++] = target;
            }
        }
    free(queue);
    return 1;

fail:
    ir_cfg_free(graph);
    return 0;
}

static int value_available(size_t value, size_t use_block, size_t use_index,
                           const IrControlFlowGraph *graph, const uint64_t *dominators, size_t words) {
    size_t definition = graph->definitions[value];
    if (definition == IR_VALUE_NONE) return 0;
    size_t owner = graph->owner[definition];
    if (owner == use_block) return definition < use_index;
    if (!graph->blocks[use_block].reachable) return 1;
    return (dominators[use_block * words + owner / 64] & (UINT64_C(1) << (owner % 64))) != 0;
}

/* Called after opcode/type/value/label validation, so operand indices are safe. */
int ir_verify_control_flow(const IrFunction *function, int implicit_void_return) {
    size_t n = function->instruction_count;
    if (n == 0) return implicit_void_return;
    IrControlFlowGraph graph;
    if (!ir_cfg_build(function, &graph)) return 0;
    size_t count = graph.count;
    size_t words = (count + 63) / 64;
    uint64_t *dominators = NULL;
    uint64_t *row = NULL;
    int valid = 0;
    if (graph.blocks[count - 1].reachable && !implicit_void_return &&
        !ir_opcode_is_terminator(function->instructions[n - 1].opcode))
        goto done;
    if (words > SIZE_MAX / sizeof(*dominators) || count > SIZE_MAX / (words * sizeof(*dominators))) goto done;
    dominators = calloc(count * words, sizeof(*dominators));
    row = malloc(words * sizeof(*row));
    if (dominators == NULL || row == NULL) goto done;
    for (size_t b = 0; b < count; b++) {
        if (b == 0) dominators[0] = 1;
        else if (graph.blocks[b].reachable)
            for (size_t q = 0; q < count; q++)
                if (graph.blocks[q].reachable)
                    dominators[b * words + q / 64] |= UINT64_C(1) << (q % 64);
    }
    int changed;
    do {
        changed = 0;
        for (size_t b = 1; b < count; b++) {
            if (!graph.blocks[b].reachable) continue;
            for (size_t w = 0; w < words; w++) row[w] = UINT64_MAX;
            for (size_t e = graph.blocks[b].predecessor; e != IR_VALUE_NONE; e = graph.edges[e].next)
                if (graph.blocks[graph.edges[e].block].reachable)
                    for (size_t w = 0; w < words; w++)
                        row[w] &= dominators[graph.edges[e].block * words + w];
            row[b / 64] |= UINT64_C(1) << (b % 64);
            if (memcmp(row, dominators + b * words, words * sizeof(*row)) != 0) {
                memcpy(dominators + b * words, row, words * sizeof(*row));
                changed = 1;
            }
        }
    } while (changed);
    for (size_t i = 0; i < n; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        size_t b = graph.owner[i];
#define AVAILABLE(v) do { if ((v) >= function->next_value || !value_available((v), b, i, &graph, dominators, words)) goto done; } while (0)
        if (instruction->opcode == IR_OP_PHI) {
            size_t incoming[2] = {graph.labels[instruction->target_a], graph.labels[instruction->target_b]};
            size_t values[2] = {instruction->operand_a, instruction->operand_b};
            if (graph.blocks[b].predecessor_count != 2) goto done;
            for (size_t a = 0; a < 2; a++) {
                size_t pred = incoming[a];
                if (pred == IR_VALUE_NONE || graph.blocks[pred].successor[0] != b ||
                    function->instructions[graph.blocks[pred].end - 1].opcode != IR_OP_JUMP ||
                    !value_available(values[a], pred, graph.blocks[pred].end, &graph, dominators, words))
                    goto done;
            }
        } else {
            if (instruction->operand_a != IR_VALUE_NONE) AVAILABLE(instruction->operand_a);
            if (instruction->operand_b != IR_VALUE_NONE) AVAILABLE(instruction->operand_b);
            if (instruction->opcode == IR_OP_CALL || instruction->opcode == IR_OP_ENUM_CONSTRUCT)
                for (size_t a = 0; a < instruction->argument_count; a++)
                    AVAILABLE(function->arguments[instruction->first_argument + a]);
        }
#undef AVAILABLE
    }
    valid = 1;
done:
    free(dominators);
    free(row);
    ir_cfg_free(&graph);
    return valid;
}
