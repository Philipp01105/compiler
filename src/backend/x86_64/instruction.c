#include "instruction.h"

#include <string.h>

static X64Operand operand(X64OperandKind kind) {
    X64Operand result = {0};
    result.kind = kind;
    return result;
}

X64Operand x64_register(const char *name) {
    X64Operand result = operand(X64_OPERAND_REGISTER);
    result.reg = name;
    return result;
}

X64Operand x64_sized_register(X64Width width, const char *name) {
    X64Operand result = x64_register(name);
    result.width = width;
    return result;
}

X64Operand x64_immediate(long long value) {
    X64Operand result = operand(X64_OPERAND_IMMEDIATE);
    result.immediate = value;
    return result;
}

X64Operand x64_memory(X64Width width, const char *base, long long displacement) {
    X64Operand result = operand(X64_OPERAND_MEMORY);
    result.width = width;
    result.base = base;
    result.scale = 1;
    result.displacement = displacement;
    return result;
}

X64Operand x64_indexed_memory(X64Width width, const char *base, const char *index,
                              unsigned scale, long long displacement) {
    X64Operand result = x64_memory(width, base, displacement);
    result.index = index;
    result.scale = scale;
    return result;
}

X64Operand x64_symbol(const char *name, size_t suffix) {
    X64Operand result = operand(X64_OPERAND_SYMBOL);
    result.symbol = name;
    result.symbol_suffix = suffix;
    result.has_symbol_suffix = 1;
    return result;
}

X64Operand x64_label(const char *name) {
    X64Operand result = operand(X64_OPERAND_SYMBOL);
    result.symbol = name;
    return result;
}

X64Operand x64_rip_memory(X64Width width, const char *name, size_t suffix) {
    X64Operand result = x64_memory(width, "rip", 0);
    result.symbol = name;
    result.symbol_suffix = suffix;
    result.has_symbol_suffix = 1;
    result.rip_relative = 1;
    return result;
}

static X64Instruction instruction(X64Opcode opcode, X64Width width) {
    X64Instruction result = {0};
    result.opcode = opcode;
    result.width = width;
    return result;
}

X64Instruction x64_instruction0(X64Opcode opcode, X64Width width) {
    return instruction(opcode, width);
}

X64Instruction x64_instruction1(X64Opcode opcode, X64Width width, X64Operand value) {
    X64Instruction result = instruction(opcode, width);
    result.operands[0] = value;
    result.operand_count = 1;
    return result;
}

X64Instruction x64_instruction2(X64Opcode opcode, X64Width width,
                                X64Operand destination, X64Operand source) {
    X64Instruction result = instruction(opcode, width);
    result.operands[0] = destination;
    result.operands[1] = source;
    result.operand_count = 2;
    return result;
}

X64Instruction x64_instruction3(X64Opcode opcode, X64Width width,
                                X64Operand destination, X64Operand source,
                                X64Operand extra) {
    X64Instruction result = x64_instruction2(opcode, width, destination, source);
    result.operands[2] = extra;
    result.operand_count = 3;
    return result;
}

X64Instruction x64_instruction_with_source(X64Instruction instruction,
                                           size_t ir_instruction,
                                           int begin_line, int begin_column,
                                           int end_line, int end_column) {
    instruction.ir_instruction = ir_instruction;
    instruction.has_source = begin_line > 0 && begin_column > 0;
    instruction.source_begin_line = begin_line;
    instruction.source_begin_column = begin_column;
    instruction.source_end_line = end_line;
    instruction.source_end_column = end_column;
    return instruction;
}

typedef enum {
    X64_SUFFIX_NONE,
    X64_SUFFIX_WIDTH,
    X64_SUFFIX_SOURCE_DESTINATION
} X64SuffixPolicy;

typedef struct {
    const char *intel_name;
    const char *att_name;
    X64SuffixPolicy att_suffix;
} X64OpcodeDescriptor;

#define OP(name, suffix) {name, name, suffix}
#define OP_NAMES(intel, att, suffix) {intel, att, suffix}
static const X64OpcodeDescriptor opcode_descriptors[X64_OP_COUNT] = {
    OP("mov", X64_SUFFIX_WIDTH),
    OP_NAMES("movsx", "movs", X64_SUFFIX_SOURCE_DESTINATION),
    OP_NAMES("movzx", "movz", X64_SUFFIX_SOURCE_DESTINATION),
    OP("movabs", X64_SUFFIX_WIDTH), OP("movd", X64_SUFFIX_NONE),
    OP("movq", X64_SUFFIX_NONE), OP("lea", X64_SUFFIX_WIDTH),
    OP("push", X64_SUFFIX_WIDTH), OP("pop", X64_SUFFIX_WIDTH),
    OP("add", X64_SUFFIX_WIDTH), OP("sub", X64_SUFFIX_WIDTH),
    OP("and", X64_SUFFIX_WIDTH), OP("or", X64_SUFFIX_WIDTH),
    OP("xor", X64_SUFFIX_WIDTH), OP("cmp", X64_SUFFIX_WIDTH),
    OP("test", X64_SUFFIX_WIDTH), OP("imul", X64_SUFFIX_WIDTH),
    OP("idiv", X64_SUFFIX_WIDTH), OP("neg", X64_SUFFIX_WIDTH),
    OP("inc", X64_SUFFIX_WIDTH), OP("dec", X64_SUFFIX_WIDTH),
    OP("cvtsi2ss", X64_SUFFIX_NONE),
    OP_NAMES("cvtsi2ss", "cvtsi2ssq", X64_SUFFIX_NONE),
    OP("cvtsi2sd", X64_SUFFIX_NONE),
    OP_NAMES("cvtsi2sd", "cvtsi2sdq", X64_SUFFIX_NONE),
    OP("cvtss2sd", X64_SUFFIX_NONE), OP("cvtsd2ss", X64_SUFFIX_NONE),
    OP("cvttss2si", X64_SUFFIX_NONE),
    OP_NAMES("cvttss2si", "cvttss2siq", X64_SUFFIX_NONE),
    OP("cvttsd2si", X64_SUFFIX_NONE),
    OP_NAMES("cvttsd2si", "cvttsd2siq", X64_SUFFIX_NONE),
    OP("addss", X64_SUFFIX_NONE), OP("addsd", X64_SUFFIX_NONE),
    OP("subss", X64_SUFFIX_NONE), OP("subsd", X64_SUFFIX_NONE),
    OP("mulss", X64_SUFFIX_NONE), OP("mulsd", X64_SUFFIX_NONE),
    OP("divss", X64_SUFFIX_NONE), OP("divsd", X64_SUFFIX_NONE),
    OP("ucomiss", X64_SUFFIX_NONE), OP("ucomisd", X64_SUFFIX_NONE),
    OP("xorps", X64_SUFFIX_NONE), OP("xorpd", X64_SUFFIX_NONE),
    OP("call", X64_SUFFIX_NONE), OP("jmp", X64_SUFFIX_NONE),
    OP("je", X64_SUFFIX_NONE), OP("jne", X64_SUFFIX_NONE),
    OP("jl", X64_SUFFIX_NONE), OP("jle", X64_SUFFIX_NONE),
    OP("jg", X64_SUFFIX_NONE), OP("jge", X64_SUFFIX_NONE),
    OP("sete", X64_SUFFIX_NONE), OP("setne", X64_SUFFIX_NONE),
    OP("setl", X64_SUFFIX_NONE), OP("setle", X64_SUFFIX_NONE),
    OP("setg", X64_SUFFIX_NONE), OP("setge", X64_SUFFIX_NONE),
    OP("seta", X64_SUFFIX_NONE), OP("setae", X64_SUFFIX_NONE),
    OP("setb", X64_SUFFIX_NONE), OP("setbe", X64_SUFFIX_NONE),
    OP("setp", X64_SUFFIX_NONE), OP("setnp", X64_SUFFIX_NONE),
    OP("ret", X64_SUFFIX_NONE), OP("cqo", X64_SUFFIX_NONE),
    OP("syscall", X64_SUFFIX_NONE), OP("ud2", X64_SUFFIX_NONE),
    OP("shl", X64_SUFFIX_WIDTH), OP("shr", X64_SUFFIX_WIDTH), OP("div", X64_SUFFIX_WIDTH)
};
#undef OP_NAMES
#undef OP

static char att_suffix(X64Width width) {
    switch (width) {
        case X64_WIDTH_BYTE: return 'b';
        case X64_WIDTH_WORD: return 'w';
        case X64_WIDTH_DWORD: return 'l';
        case X64_WIDTH_QWORD: return 'q';
        case X64_WIDTH_FLOAT: return 's';
        case X64_WIDTH_DOUBLE: return 'd';
        default: return '\0';
    }
}

static const char *intel_size(X64Width width) {
    switch (width) {
        case X64_WIDTH_BYTE: return "BYTE PTR ";
        case X64_WIDTH_WORD: return "WORD PTR ";
        case X64_WIDTH_DWORD: return "DWORD PTR ";
        case X64_WIDTH_QWORD: return "QWORD PTR ";
        default: return "";
    }
}

static int print_symbol(FILE *output, const X64Operand *value) {
    if (value->symbol == NULL) return 0;
    if (fputs(value->symbol, output) == EOF) return 0;
    return !value->has_symbol_suffix || fprintf(output, "%zu", value->symbol_suffix) >= 0;
}

static int print_intel_memory(FILE *output, const X64Operand *value) {
    if (fputs(intel_size(value->width), output) == EOF || fputc('[', output) == EOF) return 0;
    int wrote = 0;
    if (value->rip_relative) {
        if (fputs("rip + ", output) == EOF || !print_symbol(output, value)) return 0;
        wrote = 1;
    } else if (value->base != NULL) {
        if (fputs(value->base, output) == EOF) return 0;
        wrote = 1;
    }
    if (value->index != NULL) {
        if (wrote && fputs(" + ", output) == EOF) return 0;
        if (fputs(value->index, output) == EOF) return 0;
        if (value->scale > 1 && fprintf(output, " * %u", value->scale) < 0) return 0;
        wrote = 1;
    }
    if (!value->rip_relative && value->symbol != NULL) {
        if (wrote && fputs(" + ", output) == EOF) return 0;
        if (!print_symbol(output, value)) return 0;
        wrote = 1;
    }
    if (value->displacement != 0 || !wrote) {
        if (wrote && fprintf(output, value->displacement < 0 ? " - %lld" : " + %lld",
                             value->displacement < 0 ? -value->displacement : value->displacement) < 0)
            return 0;
        if (!wrote && fprintf(output, "%lld", value->displacement) < 0) return 0;
    }
    return fputc(']', output) != EOF;
}

static int print_att_memory(FILE *output, const X64Operand *value) {
    if (value->symbol != NULL && !print_symbol(output, value)) return 0;
    if (value->displacement != 0 && fprintf(output, "%lld", value->displacement) < 0) return 0;
    if (value->base == NULL && value->index == NULL) return 1;
    if (fputc('(', output) == EOF) return 0;
    if (value->base != NULL && fprintf(output, "%%%s", value->base) < 0) return 0;
    if (value->index != NULL) {
        if (fprintf(output, ",%%%s,%u", value->index, value->scale == 0 ? 1 : value->scale) < 0)
            return 0;
    }
    return fputc(')', output) != EOF;
}

static int print_operand(FILE *output, SyntaxMode syntax, const X64Operand *value) {
    switch (value->kind) {
        case X64_OPERAND_REGISTER:
            return fprintf(output, syntax == SYNTAX_ATT ? "%%%s" : "%s", value->reg) >= 0;
        case X64_OPERAND_IMMEDIATE:
            return fprintf(output, syntax == SYNTAX_ATT ? "$%lld" : "%lld",
                           value->immediate) >= 0;
        case X64_OPERAND_MEMORY:
            return syntax == SYNTAX_ATT ? print_att_memory(output, value) : print_intel_memory(output, value);
        case X64_OPERAND_SYMBOL:
            return print_symbol(output, value);
        default:
            return 0;
    }
}

int x64_print_instruction(FILE *output, SyntaxMode syntax,
                          const X64Instruction *value) {
    if (output == NULL || value == NULL || value->operand_count > 3) return 0;
    if (value->opcode >= X64_OP_COUNT) return 0;
    const X64OpcodeDescriptor *descriptor = &opcode_descriptors[value->opcode];
    const char *name = syntax == SYNTAX_ATT ? descriptor->att_name : descriptor->intel_name;
    if (syntax == SYNTAX_INTEL && value->opcode == X64_OP_MOVSX &&
        value->operand_count == 2 && value->operands[0].width == X64_WIDTH_QWORD &&
        value->operands[1].width == X64_WIDTH_DWORD)
        name = "movsxd";
    if (name == NULL || fputs("    ", output) == EOF || fputs(name, output) == EOF) return 0;
    if (syntax == SYNTAX_ATT && descriptor->att_suffix == X64_SUFFIX_SOURCE_DESTINATION) {
        char source = value->operand_count > 1 ? att_suffix(value->operands[1].width) : '\0';
        char destination = value->operand_count > 0 ? att_suffix(value->operands[0].width) : '\0';
        if (source == '\0' || destination == '\0' ||
            fputc(source, output) == EOF || fputc(destination, output) == EOF)
            return 0;
    }
    char suffix = syntax == SYNTAX_ATT && descriptor->att_suffix == X64_SUFFIX_WIDTH
                      ? att_suffix(value->width)
                      : '\0';
    if (suffix != '\0' && fputc(suffix, output) == EOF) return 0;
    if (value->operand_count != 0 && fputc(' ', output) == EOF) return 0;
    for (size_t position = 0; position < value->operand_count; position++) {
        size_t index = syntax == SYNTAX_ATT && value->operand_count > 1
                           ? value->operand_count - position - 1
                           : position;
        if (position != 0 && fputs(", ", output) == EOF) return 0;
        X64Operand operand_value = value->operands[index];
        if (syntax == SYNTAX_ATT &&
            (value->opcode == X64_OP_CALL || value->opcode == X64_OP_JMP) &&
            operand_value.kind != X64_OPERAND_SYMBOL &&
            fputc('*', output) == EOF)
            return 0;
        if (syntax == SYNTAX_INTEL &&
            (value->opcode == X64_OP_MOVSX || value->opcode == X64_OP_MOVZX) && index == 1 &&
            operand_value.kind == X64_OPERAND_MEMORY && operand_value.width == X64_WIDTH_NONE)
            operand_value.width = value->width;
        if (!print_operand(output, syntax, &operand_value)) return 0;
    }
    return fputc('\n', output) != EOF;
}
