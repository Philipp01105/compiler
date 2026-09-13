#include "ir_cfg.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t begin, end;
    size_t successor[2];
    size_t predecessor;
    size_t predecessor_count;
    int reachable;
} Block;

typedef struct { size_t block, next; } Edge;

static int terminator(IrOpcode opcode) {
    return opcode == IR_OP_BRANCH || opcode == IR_OP_JUMP || opcode == IR_OP_RETURN;
}

static int available(size_t value, size_t use_block, size_t use_index,
                     const size_t *definitions, const size_t *instruction_blocks,
                     const Block *blocks, const uint64_t *dominators, size_t words) {
    size_t definition = definitions[value];
    if (definition == IR_VALUE_NONE) return 0;
    size_t owner = instruction_blocks[definition];
    if (owner == use_block) return definition < use_index;
    /* Dominance is defined on paths from the function entry. */
    if (!blocks[use_block].reachable) return 1;
    return (dominators[use_block * words + owner / 64] &
            (UINT64_C(1) << (owner % 64))) != 0;
}

/* Called after opcode/type/value/label validation, so operand indices are safe. */
int ir_verify_control_flow(const IrFunction *function, int implicit_void_return) {
    size_t n = function->instruction_count;
    if (n == 0) return implicit_void_return;
    if (n > SIZE_MAX / sizeof(Block) || n > SIZE_MAX / sizeof(size_t) ||
        n > SIZE_MAX / (2 * sizeof(Edge)) ||
        function->next_label > SIZE_MAX / sizeof(size_t) ||
        function->next_value > SIZE_MAX / sizeof(size_t)) return 0;
    Block *blocks = calloc(n, sizeof(*blocks));
    Edge *edges = malloc(2 * n * sizeof(*edges));
    size_t *instruction_blocks = malloc(n * sizeof(*instruction_blocks));
    size_t *labels = malloc(function->next_label * sizeof(*labels));
    size_t *definitions = malloc(function->next_value * sizeof(*definitions));
    size_t *queue = malloc(n * sizeof(*queue));
    uint64_t *dominators = NULL, *row = NULL;
    int valid = 0;
    if (blocks == NULL || edges == NULL || instruction_blocks == NULL || queue == NULL ||
        (function->next_label != 0 && labels == NULL) ||
        (function->next_value != 0 && definitions == NULL)) goto done;
    for (size_t i = 0; i < function->next_label; i++) labels[i] = IR_VALUE_NONE;
    for (size_t i = 0; i < function->next_value; i++) definitions[i] = IR_VALUE_NONE;
    size_t count = 0;
    int phi_region = 0;
    for (size_t i = 0; i < n; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        if (i == 0 || instruction->opcode == IR_OP_LABEL) {
            if (count != 0) blocks[count - 1].end = i;
            blocks[count].begin = i;
            blocks[count].predecessor = IR_VALUE_NONE;
            blocks[count].successor[0] = blocks[count].successor[1] = IR_VALUE_NONE;
            count++;
            phi_region = instruction->opcode == IR_OP_LABEL;
        } else if (terminator(function->instructions[i - 1].opcode)) goto done;
        if (instruction->opcode == IR_OP_PHI) {
            if (!phi_region) goto done;
        } else if (instruction->opcode != IR_OP_LABEL) phi_region = 0;
        instruction_blocks[i] = count - 1;
        if (instruction->opcode == IR_OP_LABEL) labels[instruction->target_a] = count - 1;
        if (instruction->result != IR_VALUE_NONE) definitions[instruction->result] = i;
    }
    blocks[count - 1].end = n;
    size_t edge_count = 0;
    for (size_t b = 0; b < count; b++) {
        const IrInstruction *last = &function->instructions[blocks[b].end - 1];
        if (last->opcode == IR_OP_BRANCH || last->opcode == IR_OP_JUMP)
            blocks[b].successor[0] = labels[last->target_a];
        else if (last->opcode != IR_OP_RETURN && b + 1 < count)
            blocks[b].successor[0] = b + 1;
        if (last->opcode == IR_OP_BRANCH && last->target_b != last->target_a)
            blocks[b].successor[1] = labels[last->target_b];
        for (size_t s = 0; s < 2; s++) {
            size_t target = blocks[b].successor[s];
            if (target == IR_VALUE_NONE) continue;
            edges[edge_count] = (Edge) {b, blocks[target].predecessor};
            blocks[target].predecessor = edge_count++;
            blocks[target].predecessor_count++;
        }
    }
    size_t queued = 1;
    queue[0] = 0; blocks[0].reachable = 1;
    for (size_t q = 0; q < queued; q++)
        for (size_t s = 0; s < 2; s++) {
            size_t target = blocks[queue[q]].successor[s];
            if (target != IR_VALUE_NONE && !blocks[target].reachable) {
                blocks[target].reachable = 1; queue[queued++] = target;
            }
        }
    size_t words = (count + 63) / 64;
    if (blocks[count - 1].reachable && !implicit_void_return &&
        !terminator(function->instructions[n - 1].opcode)) goto done;
    if (words > SIZE_MAX / sizeof(uint64_t) || count > SIZE_MAX / (words * sizeof(uint64_t))) goto done;
    dominators = calloc(count * words, sizeof(*dominators));
    row = malloc(words * sizeof(*row));
    if (dominators == NULL || row == NULL) goto done;
    for (size_t b = 0; b < count; b++) {
        if (b == 0) dominators[0] = 1;
        else if (blocks[b].reachable)
            for (size_t q = 0; q < queued; q++)
                dominators[b * words + queue[q] / 64] |= UINT64_C(1) << (queue[q] % 64);
    }
    int changed;
    do {
        changed = 0;
        for (size_t q = 1; q < queued; q++) {
            size_t b = queue[q];
            for (size_t w = 0; w < words; w++) row[w] = UINT64_MAX;
            for (size_t e = blocks[b].predecessor; e != IR_VALUE_NONE; e = edges[e].next)
                if (blocks[edges[e].block].reachable)
                    for (size_t w = 0; w < words; w++) row[w] &= dominators[edges[e].block * words + w];
            row[b / 64] |= UINT64_C(1) << (b % 64);
            if (memcmp(row, dominators + b * words, words * sizeof(*row)) != 0) {
                memcpy(dominators + b * words, row, words * sizeof(*row)); changed = 1;
            }
        }
    } while (changed);
    for (size_t i = 0; i < n; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        size_t b = instruction_blocks[i];
#define AVAILABLE(v) do { if ((v) >= function->next_value || !available((v), b, i, definitions, instruction_blocks, blocks, dominators, words)) goto done; } while (0)
        if (instruction->opcode == IR_OP_PHI) {
            size_t incoming[2] = {labels[instruction->target_a], labels[instruction->target_b]};
            size_t values[2] = {instruction->operand_a, instruction->operand_b};
            if (blocks[b].predecessor_count != 2) goto done;
            for (size_t a = 0; a < 2; a++) {
                size_t pred = incoming[a];
                if (pred == IR_VALUE_NONE || blocks[pred].successor[0] != b ||
                    function->instructions[blocks[pred].end - 1].opcode != IR_OP_JUMP ||
                    !available(values[a], pred, blocks[pred].end, definitions,
                               instruction_blocks, blocks, dominators, words)) goto done;
            }
        } else {
            if (instruction->operand_a != IR_VALUE_NONE) AVAILABLE(instruction->operand_a);
            if (instruction->operand_b != IR_VALUE_NONE) AVAILABLE(instruction->operand_b);
            if (instruction->opcode == IR_OP_CALL)
                for (size_t a = 0; a < instruction->argument_count; a++)
                    AVAILABLE(function->arguments[instruction->first_argument + a]);
        }
#undef AVAILABLE
    }
    valid = 1;
done:
    free(blocks); free(edges); free(instruction_blocks); free(labels); free(definitions);
    free(queue); free(dominators); free(row);
    return valid;
}
