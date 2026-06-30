#include "ir_emitter_internal.h"

#include <stdint.h>
#include <stdio.h>

void load_floating_value(Emitter *emitter, size_t value, DataType target,
                                unsigned xmm) {
    const IrInstruction *source = producer(emitter->function, value);
    write_value_load(emitter, "rax", value);
    convert_rax(emitter, source->type, target);
    char destination[16];
    (void) snprintf(destination, sizeof(destination), "xmm%u", xmm);
    write_x64_2(emitter, target == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ,
                X64_WIDTH_NONE, x64_register(destination),
                x64_register(target == TYPE_FLOAT ? "eax" : "rax"));
}

static void store_floating_result(Emitter *emitter, DataType type, unsigned xmm,
                                  size_t result) {
    char source[16];
    (void) snprintf(source, sizeof(source), "xmm%u", xmm);
    write_x64_2(emitter, type == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ,
                X64_WIDTH_NONE, x64_register(type == TYPE_FLOAT ? "eax" : "rax"),
                x64_register(source));
    write_value_store(emitter, "rax", result);
}

static int emit_floating_binary(Emitter *emitter, const IrInstruction *instruction,
                                DataType operation_type) {
    load_floating_value(emitter, instruction->operand_a, operation_type, 2);
    load_floating_value(emitter, instruction->operand_b, operation_type, 1);
    if (instruction->operator_type >= TOKEN_PLUS &&
        instruction->operator_type <= TOKEN_SLASH) {
        static const X64Opcode float_operations[] = {
            X64_OP_ADDSS, X64_OP_SUBSS, X64_OP_MULSS, X64_OP_DIVSS
        };
        static const X64Opcode double_operations[] = {
            X64_OP_ADDSD, X64_OP_SUBSD, X64_OP_MULSD, X64_OP_DIVSD
        };
        size_t operation = (size_t) (instruction->operator_type - TOKEN_PLUS);
        write_x64_2(emitter, operation_type == TYPE_FLOAT
                                 ? float_operations[operation]
                                 : double_operations[operation],
                    X64_WIDTH_NONE, x64_register("xmm2"), x64_register("xmm1"));
        store_floating_result(emitter, instruction->type, 2, instruction->result);
        return 1;
    }
    if (instruction->operator_type < TOKEN_EQUAL_EQUAL ||
        instruction->operator_type > TOKEN_GREATER_EQUAL)
        return 0;
    write_x64_2(emitter, operation_type == TYPE_FLOAT ? X64_OP_UCOMISS : X64_OP_UCOMISD,
                X64_WIDTH_NONE, x64_register("xmm2"), x64_register("xmm1"));
    X64Opcode condition = X64_OP_SETE;
    int require_ordered = 0;
    int include_unordered = 0;
    if (instruction->operator_type == TOKEN_BANG_EQUAL) {
        condition = X64_OP_SETNE;
        include_unordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS) {
        condition = X64_OP_SETB;
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS_EQUAL) {
        condition = X64_OP_SETBE;
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_GREATER) condition = X64_OP_SETA;
    else if (instruction->operator_type == TOKEN_GREATER_EQUAL) condition = X64_OP_SETAE;
    else require_ordered = 1;
    write_x64_1(emitter, condition, X64_WIDTH_NONE, x64_register("al"));
    if (require_ordered || include_unordered) {
        write_x64_1(emitter, include_unordered ? X64_OP_SETP : X64_OP_SETNP,
                    X64_WIDTH_NONE, x64_register("dl"));
        write_x64_2(emitter, include_unordered ? X64_OP_OR : X64_OP_AND,
                    X64_WIDTH_BYTE, x64_register("al"), x64_register("dl"));
    }
    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                x64_sized_register(X64_WIDTH_BYTE, "al"));
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}


int emit_binary(Emitter *emitter, const IrInstruction *instruction) {
    const IrInstruction *left = producer(emitter->function, instruction->operand_a);
    const IrInstruction *right = producer(emitter->function, instruction->operand_b);
    if (left != NULL && right != NULL && instruction->type == TYPE_STRING &&
        instruction->operator_type == TOKEN_PLUS)
        return emit_string_concat(emitter, instruction);
    if (left != NULL && right != NULL && left->type == TYPE_STRING &&
        right->type == TYPE_STRING) {
        return emit_string_compare(emitter, instruction);
    }
    if (left != NULL && right != NULL &&
        (is_floating(left->type) || is_floating(right->type))) {
        DataType operation_type = is_floating(instruction->type)
                                      ? instruction->type
                                      : (left->type == TYPE_DOUBLE || right->type == TYPE_DOUBLE
                                             ? TYPE_DOUBLE
                                             : TYPE_FLOAT);
        return emit_floating_binary(emitter, instruction, operation_type);
    }
    DataType integer_type = left != NULL && right != NULL
                                ? data_type_promoted_integer(left->type, right->type)
                                : TYPE_INT;
    int fixed = data_type_fixed_integer(integer_type) && !is_pointer_value(left) && !is_pointer_value(right);
    write_value_load(emitter, "rax", instruction->operand_b);
    if (fixed) normalize_integral_parameter(emitter, integer_type);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rcx"), x64_register("rax"));
    write_value_load(emitter, "rax", instruction->operand_a);
    if (fixed) normalize_integral_parameter(emitter, integer_type);
    switch (instruction->operator_type) {
        case TOKEN_PLUS:
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_MINUS:
            write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_STAR:
            write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            if (fixed && data_type_unsigned(integer_type)) {
                write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD, x64_register("edx"), x64_register("edx"));
                write_x64_1(emitter, X64_OP_DIV, X64_WIDTH_QWORD, x64_register("rcx"));
            } else {
                write_x64_0(emitter, X64_OP_CQO);
                write_x64_1(emitter, X64_OP_IDIV, X64_WIDTH_QWORD, x64_register("rcx"));
            }
            if (instruction->operator_type == TOKEN_PERCENT)
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rdx"));
            break;
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL: {
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            X64Opcode condition = X64_OP_SETE;
            if (instruction->operator_type == TOKEN_BANG_EQUAL) condition = X64_OP_SETNE;
            else if (instruction->operator_type == TOKEN_LESS)
                condition = fixed && data_type_unsigned(integer_type)
                                ? X64_OP_SETB
                                : X64_OP_SETL;
            else if (instruction->operator_type == TOKEN_LESS_EQUAL)
                condition = fixed && data_type_unsigned(integer_type) ? X64_OP_SETBE : X64_OP_SETLE;
            else if (instruction->operator_type == TOKEN_GREATER)
                condition = fixed && data_type_unsigned(integer_type)
                                ? X64_OP_SETA
                                : X64_OP_SETG;
            else if (instruction->operator_type == TOKEN_GREATER_EQUAL)
                condition = fixed && data_type_unsigned(integer_type) ? X64_OP_SETAE : X64_OP_SETGE;
            write_x64_1(emitter, condition, X64_WIDTH_NONE, x64_register("al"));
            write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                        x64_sized_register(X64_WIDTH_QWORD, "rax"),
                        x64_sized_register(X64_WIDTH_BYTE, "al"));
            break;
        }
        default: return 0;
    }
    if (fixed && data_type_fixed_integer(instruction->type)) normalize_integral_parameter(emitter, instruction->type);
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}


/* Values use an eight-byte virtual slot. Floating-point values are kept as
   their IEEE bit pattern so conversions happen only at typed IR boundaries. */
void convert_rax(Emitter *emitter, DataType from, DataType to) {
    if (!is_numeric(from) || !is_numeric(to)) return;
    if (is_integral(from) && is_integral(to)) {
        if (data_type_fixed_integer(to) || (data_type_fixed_integer(from) && to == TYPE_INT))
            normalize_integral_parameter(emitter, to);
        return;
    }
    if (from == to) return;
    if (data_type_unsigned(from) && data_type_bytes(from) == 8 && is_floating(to)) {
        size_t sequence = emitter->bounds_sequence++;
        char ordinary[96], done[96];
        snprintf(ordinary, sizeof(ordinary), ".LIR_u64_float_%zu_%zu", emitter->function_index, sequence);
        snprintf(done, sizeof(done), ".LIR_u64_float_done_%zu_%zu", emitter->function_index, sequence);
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_JGE, X64_WIDTH_NONE, x64_label(ordinary));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r11"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_AND, X64_WIDTH_QWORD, x64_register("r11"), x64_immediate(1));
        write_x64_2(emitter, X64_OP_SHR, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(1));
        write_x64_2(emitter, X64_OP_OR, X64_WIDTH_QWORD, x64_register("rax"), x64_register("r11"));
        write_x64_2(emitter, to == TYPE_FLOAT ? X64_OP_CVTSI2SSQ : X64_OP_CVTSI2SDQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, to == TYPE_FLOAT ? X64_OP_ADDSS : X64_OP_ADDSD, X64_WIDTH_NONE, x64_register("xmm0"),
                    x64_register("xmm0"));
        write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(done));
        write_labelf(emitter, "%s:\n", ordinary);
        write_x64_2(emitter, to == TYPE_FLOAT ? X64_OP_CVTSI2SSQ : X64_OP_CVTSI2SDQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_labelf(emitter, "%s:\n", done);
        write_x64_2(emitter, to == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register(to == TYPE_FLOAT ? "eax" : "rax"), x64_register("xmm0"));
        return;
    }
    if (is_floating(from) && data_type_unsigned(to) && data_type_bytes(to) == 8) {
        size_t sequence = emitter->bounds_sequence++;
        char done[96];
        snprintf(done, sizeof(done), ".LIR_float_u64_done_%zu_%zu", emitter->function_index, sequence);
        write_x64_2(emitter, from == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE, x64_register("xmm0"),
                    x64_register(from == TYPE_FLOAT ? "eax" : "rax"));
        write_x64_2(emitter, from == TYPE_FLOAT ? X64_OP_CVTTSS2SIQ : X64_OP_CVTTSD2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_JGE, X64_WIDTH_NONE, x64_label(done));
        write_immediate(emitter, "r11", from == TYPE_FLOAT ? INT64_C(0x5f000000) : INT64_C(0x43e0000000000000));
        write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("r11"));
        write_x64_2(emitter, from == TYPE_FLOAT ? X64_OP_SUBSS : X64_OP_SUBSD, X64_WIDTH_NONE, x64_register("xmm0"),
                    x64_memory(from == TYPE_FLOAT ? X64_WIDTH_DWORD : X64_WIDTH_QWORD, "rsp", 0));
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(8));
        write_x64_2(emitter, from == TYPE_FLOAT ? X64_OP_CVTTSS2SIQ : X64_OP_CVTTSD2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
        write_immediate(emitter, "r11", (long long) UINT64_C(0x8000000000000000));
        write_x64_2(emitter, X64_OP_OR, X64_WIDTH_QWORD, x64_register("rax"), x64_register("r11"));
        write_labelf(emitter, "%s:\n", done);
        return;
    }
    if (is_integral(from) && to == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_CVTSI2SDQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (is_integral(from) && to == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_CVTSI2SSQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("eax"), x64_register("xmm0"));
    } else if (from == TYPE_FLOAT && to == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_CVTSS2SD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm0"));
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (from == TYPE_DOUBLE && to == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_CVTSD2SS, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm0"));
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("eax"), x64_register("xmm0"));
    } else if (from == TYPE_FLOAT && is_integral(to)) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_CVTTSS2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (from == TYPE_DOUBLE && is_integral(to)) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_CVTTSD2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    }
    if (data_type_fixed_integer(to)) normalize_integral_parameter(emitter, to);
}

void normalize_truth_rax(Emitter *emitter, DataType type) {
    if (type == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_XORPS, X64_WIDTH_NONE,
                    x64_register("xmm1"), x64_register("xmm1"));
        write_x64_2(emitter, X64_OP_UCOMISS, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm1"));
    } else if (type == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_XORPD, X64_WIDTH_NONE,
                    x64_register("xmm1"), x64_register("xmm1"));
        write_x64_2(emitter, X64_OP_UCOMISD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm1"));
    } else {
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
        write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                    x64_sized_register(X64_WIDTH_QWORD, "rax"),
                    x64_sized_register(X64_WIDTH_BYTE, "al"));
        return;
    }
    write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
    write_x64_1(emitter, X64_OP_SETP, X64_WIDTH_NONE, x64_register("dl"));
    write_x64_2(emitter, X64_OP_OR, X64_WIDTH_BYTE,
                x64_register("al"), x64_register("dl"));
    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                x64_sized_register(X64_WIDTH_BYTE, "al"));
}

