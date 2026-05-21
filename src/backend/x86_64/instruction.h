#ifndef DMM_X86_64_INSTRUCTION_H
#define DMM_X86_64_INSTRUCTION_H

#include "language_types.h"

#include <stddef.h>
#include <stdio.h>

typedef enum {
    X64_WIDTH_NONE,
    X64_WIDTH_BYTE,
    X64_WIDTH_WORD,
    X64_WIDTH_DWORD,
    X64_WIDTH_QWORD,
    X64_WIDTH_FLOAT,
    X64_WIDTH_DOUBLE
} X64Width;

typedef enum {
    X64_OP_MOV,
    X64_OP_MOVSX,
    X64_OP_MOVZX,
    X64_OP_MOVABS,
    X64_OP_MOVD,
    X64_OP_MOVQ,
    X64_OP_LEA,
    X64_OP_PUSH,
    X64_OP_POP,
    X64_OP_ADD,
    X64_OP_SUB,
    X64_OP_AND,
    X64_OP_OR,
    X64_OP_XOR,
    X64_OP_CMP,
    X64_OP_TEST,
    X64_OP_IMUL,
    X64_OP_IDIV,
    X64_OP_NEG,
    X64_OP_INC,
    X64_OP_DEC,
    X64_OP_CVTSI2SS,
    X64_OP_CVTSI2SSQ,
    X64_OP_CVTSI2SD,
    X64_OP_CVTSI2SDQ,
    X64_OP_CVTSS2SD,
    X64_OP_CVTSD2SS,
    X64_OP_CVTTSS2SI,
    X64_OP_CVTTSS2SIQ,
    X64_OP_CVTTSD2SI,
    X64_OP_CVTTSD2SIQ,
    X64_OP_ADDSS,
    X64_OP_ADDSD,
    X64_OP_SUBSS,
    X64_OP_SUBSD,
    X64_OP_MULSS,
    X64_OP_MULSD,
    X64_OP_DIVSS,
    X64_OP_DIVSD,
    X64_OP_UCOMISS,
    X64_OP_UCOMISD,
    X64_OP_XORPS,
    X64_OP_XORPD,
    X64_OP_CALL,
    X64_OP_JMP,
    X64_OP_JE,
    X64_OP_JNE,
    X64_OP_JL,
    X64_OP_JLE,
    X64_OP_JG,
    X64_OP_JGE,
    X64_OP_SETE,
    X64_OP_SETNE,
    X64_OP_SETL,
    X64_OP_SETLE,
    X64_OP_SETG,
    X64_OP_SETGE,
    X64_OP_SETA,
    X64_OP_SETAE,
    X64_OP_SETB,
    X64_OP_SETBE,
    X64_OP_SETP,
    X64_OP_SETNP,
    X64_OP_RET,
    X64_OP_CQO,
    X64_OP_SYSCALL,
    X64_OP_UD2,
    X64_OP_SHL,
    X64_OP_SHR,
    X64_OP_DIV,
    X64_OP_COUNT
} X64Opcode;

typedef enum {
    X64_OPERAND_NONE,
    X64_OPERAND_REGISTER,
    X64_OPERAND_IMMEDIATE,
    X64_OPERAND_MEMORY,
    X64_OPERAND_SYMBOL
} X64OperandKind;

typedef struct {
    X64OperandKind kind;
    X64Width width;
    const char *reg;
    const char *base;
    const char *index;
    unsigned scale;
    long long displacement;
    const char *symbol;
    size_t symbol_suffix;
    int has_symbol_suffix;
    long long immediate;
    int rip_relative;
} X64Operand;

typedef struct {
    X64Opcode opcode;
    X64Width width;
    X64Operand operands[3];
    size_t operand_count;
    size_t ir_instruction;
    int has_source;
    int source_begin_line;
    int source_begin_column;
    int source_end_line;
    int source_end_column;
} X64Instruction;

X64Operand x64_register(const char *name);
X64Operand x64_sized_register(X64Width width, const char *name);
X64Operand x64_immediate(long long value);
X64Operand x64_memory(X64Width width, const char *base, long long displacement);
X64Operand x64_indexed_memory(X64Width width, const char *base, const char *index,
                              unsigned scale, long long displacement);
X64Operand x64_symbol(const char *name, size_t suffix);
X64Operand x64_label(const char *name);
X64Operand x64_rip_memory(X64Width width, const char *name, size_t suffix);

X64Instruction x64_instruction0(X64Opcode opcode, X64Width width);
X64Instruction x64_instruction1(X64Opcode opcode, X64Width width, X64Operand operand);
X64Instruction x64_instruction2(X64Opcode opcode, X64Width width,
                                 X64Operand destination, X64Operand source);
X64Instruction x64_instruction3(X64Opcode opcode, X64Width width,
                                 X64Operand destination, X64Operand source,
                                 X64Operand extra);
X64Instruction x64_instruction_with_source(X64Instruction instruction,
                                           size_t ir_instruction,
                                           int begin_line, int begin_column,
                                           int end_line, int end_column);

int x64_print_instruction(FILE *output, SyntaxMode syntax,
                          const X64Instruction *instruction);

#endif
