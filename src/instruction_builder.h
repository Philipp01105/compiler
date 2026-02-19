#ifndef INSTRUCTION_BUILDER_H
#define INSTRUCTION_BUILDER_H

#include <stddef.h>
#include "parser.h"

/*
 * Instruction Builder - Assembly Generation Abstraction Layer
 * 
 * This module provides a clean abstraction for generating assembly instructions
 * in either AT&T or Intel syntax. It supports both Linux (ELF) and Windows (COFF)
 * target platforms.
 * 
 * Key Features:
 * - Automatic syntax selection based on target platform
 * - Type-safe instruction generation
 * - Memory addressing helpers
 * - Register and immediate operand support
 */

/* Data movement instructions */
void emit_mov_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_mov_reg_imm(Parser *parser, const char *dest, int value);

void emit_mov_reg_mem(Parser *parser, const char *dest, const char *base, int offset);

void emit_mov_mem_reg(Parser *parser, const char *base, int offset, const char *src);

void emit_movb_reg_mem(Parser *parser, const char *dest, const char *base, int offset);

void emit_movb_mem_reg(Parser *parser, const char *base, int offset, const char *src);

void emit_lea(Parser *parser, const char *dest, const char *src);

void emit_lea_rip_relative(Parser *parser, const char *dest, const char *label);

/* Stack operations */
void emit_push(Parser *parser, const char *reg);

void emit_pop(Parser *parser, const char *reg);

/* Arithmetic instructions */
void emit_add_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_add_reg_imm(Parser *parser, const char *dest, int value);

void emit_sub_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_sub_reg_imm(Parser *parser, const char *dest, int value);

void emit_imul_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_idiv_reg(Parser *parser, const char *divisor);

void emit_neg_reg(Parser *parser, const char *reg);

void emit_inc_reg(Parser *parser, const char *reg);

void emit_dec_reg(Parser *parser, const char *reg);

/* Logical instructions */
void emit_xor_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_and_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_or_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_test_reg_reg(Parser *parser, const char *reg1, const char *reg2);

void emit_cmp_reg_reg(Parser *parser, const char *reg1, const char *reg2);

void emit_cmp_reg_imm(Parser *parser, const char *reg, int value);

/* Conversion instructions */
void emit_cltq(Parser *parser);

void emit_cqto(Parser *parser);

void emit_cdq(Parser *parser);

void emit_cvtsi2sd_reg_reg(Parser *parser, const char *dest, const char *src);

void emit_cvttsd2si_reg_reg(Parser *parser, const char *dest, const char *src);

/* Control flow */
void emit_call(Parser *parser, const char *function);

void emit_ret(Parser *parser);

void emit_leave(Parser *parser);

void emit_jmp(Parser *parser, const char *label);

void emit_je(Parser *parser, const char *label);

void emit_jne(Parser *parser, const char *label);

void emit_jg(Parser *parser, const char *label);

void emit_jge(Parser *parser, const char *label);

void emit_jl(Parser *parser, const char *label);

void emit_jle(Parser *parser, const char *label);

/* Special instructions */
void emit_syscall(Parser *parser);

void emit_nop(Parser *parser);

#endif /* INSTRUCTION_BUILDER_H */
