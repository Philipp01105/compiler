#include "parser.h"
#include "parser_internal.h"
#include "instruction_builder.h"
#include <stdlib.h>

static int floating_type(DataType type) {
    return type == TYPE_FLOAT || type == TYPE_DOUBLE;
}

static int integral_type(DataType type) {
    return type == TYPE_INT || type == TYPE_CHAR || type == TYPE_BYTE || type == TYPE_BIT;
}

int can_implicitly_convert(DataType from, DataType to) {
    if (from == TYPE_UNKNOWN || to == TYPE_UNKNOWN || from == to) return 1;
    if (integral_type(from) && integral_type(to)) return 1;
    if (integral_type(from) && floating_type(to)) return 1;
    if (from == TYPE_FLOAT && to == TYPE_DOUBLE) return 1;
    return 0;
}

static void emit_stack_conversion(Parser *parser, DataType from, DataType to) {
    if (from == to || from == TYPE_UNKNOWN || to == TYPE_UNKNOWN) return;
    if (!floating_type(from) && !floating_type(to)) return;
    code_printf(parser, "    popq %%rax\n");
    if (from == TYPE_DOUBLE) {
        code_printf(parser, "    movq %%rax, %%xmm0\n");
        if (to == TYPE_FLOAT) code_printf(parser, "    cvtsd2ss %%xmm0, %%xmm0\n");
        else if (!floating_type(to)) code_printf(parser, "    cvttsd2si %%xmm0, %%eax\n");
    } else if (from == TYPE_FLOAT) {
        code_printf(parser, "    movd %%eax, %%xmm0\n");
        if (to == TYPE_DOUBLE) code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
        else if (!floating_type(to)) code_printf(parser, "    cvttss2si %%xmm0, %%eax\n");
    } else if (to == TYPE_DOUBLE) {
        code_printf(parser, "    cvtsi2sd %%eax, %%xmm0\n");
    } else {
        code_printf(parser, "    cvtsi2ss %%eax, %%xmm0\n");
    }
    if (floating_type(to)) code_printf(parser, "    movq %%xmm0, %%rax\n");
    code_printf(parser, "    pushq %%rax\n");
}

void convert_stack_value(Parser *parser, DataType from, DataType to) {
    if (!can_implicitly_convert(from, to)) {
        parser_error(parser, "Cannot implicitly convert %s to %s",
                     datatype_to_string(from), datatype_to_string(to));
        return;
    }
    emit_stack_conversion(parser, from, to);
}

void convert_stack_value_explicit(Parser *parser, DataType from, DataType to) {
    emit_stack_conversion(parser, from, to);
}

void emit_static_array_bounds_check(Parser *parser, const Variable *var) {
    if (var == NULL || !var->is_array || var->array_size <= 0) return;
    int label = parser->label_counter++;
    code_printf(parser, "    cmpq $0, %%rax\n");
    code_printf(parser, "    jl .L_bounds_fail_%d\n", label);
    code_printf(parser, "    cmpq $%d, %%rax\n", var->array_size);
    code_printf(parser, "    jge .L_bounds_fail_%d\n", label);
    code_printf(parser, "    jmp .L_bounds_ok_%d\n", label);
    code_printf(parser, ".L_bounds_fail_%d:\n", label);
    code_printf(parser, "    ud2\n");
    code_printf(parser, ".L_bounds_ok_%d:\n", label);
}

void generate_system_io_call(Parser *parser, const char *operation, int keep_result) {
    if (parser->target_format == TARGET_COFF) {
        if (operation[0] == 'c') {
            code_printf(parser, "    popq %%rcx\n");
        } else if (operation[0] == 'o') {
            code_printf(parser, "    popq %%r8\n");
            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rcx\n");
            code_printf(parser, "    movl %%edx, %%r10d\n");
            code_printf(parser, "    andl $3, %%edx\n");
            code_printf(parser, "    testl $64, %%r10d\n");
            code_printf(parser, "    jz .L_open_no_create_%d\n", parser->label_counter);
            code_printf(parser, "    orl $256, %%edx\n");
            code_printf(parser, ".L_open_no_create_%d:\n", parser->label_counter);
            code_printf(parser, "    testl $512, %%r10d\n");
            code_printf(parser, "    jz .L_open_no_trunc_%d\n", parser->label_counter);
            code_printf(parser, "    orl $512, %%edx\n");
            code_printf(parser, ".L_open_no_trunc_%d:\n", parser->label_counter);
            code_printf(parser, "    testl $1024, %%r10d\n");
            code_printf(parser, "    jz .L_open_no_append_%d\n", parser->label_counter);
            code_printf(parser, "    orl $8, %%edx\n");
            code_printf(parser, ".L_open_no_append_%d:\n", parser->label_counter++);
            code_printf(parser, "    orl $32768, %%edx\n");
            code_printf(parser, "    movl $384, %%r8d\n");
        } else {
            code_printf(parser, "    popq %%r8\n");
            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rcx\n");
        }
        generate_stack_align(parser);
        code_printf(parser, "    call _%s\n", operation);
        generate_stack_restore(parser);
    } else {
        if (operation[0] == 'c') {
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $3, %%rax\n");
        } else {
            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rsi\n");
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $%d, %%rax\n",
                        operation[0] == 'w' ? 1 : operation[0] == 'o' ? 2 : 0);
        }
        code_printf(parser, "    syscall\n");
    }
    if (keep_result) code_printf(parser, "    pushq %%rax\n");
    parser->expression_type = TYPE_INT;
}

void generate_function_call(Parser *parser, Function *func, const char *name, int arg_count) {
    int integer_index = 0;
    int float_index = 0;
    int stack_count = 0;
    int *stack_slots = calloc((size_t) arg_count, sizeof(*stack_slots));
    int *register_slots = calloc((size_t) arg_count, sizeof(*register_slots));
    int *register_indices = calloc((size_t) arg_count, sizeof(*register_indices));
    if (arg_count > 0 && (!stack_slots || !register_slots || !register_indices)) {
        free(stack_slots);
        free(register_slots);
        free(register_indices);
        parser_error(parser, "Out of memory while preparing function call");
        return;
    }
    const int max_integer = parser->target_format == TARGET_COFF ? 4 : 6;
    const int max_float = parser->target_format == TARGET_COFF ? 4 : 8;

    for (int i = 0; i < arg_count; i++) {
        int is_float = func->param_types[i] == TYPE_FLOAT || func->param_types[i] == TYPE_DOUBLE;
        int index;
        if (parser->target_format == TARGET_COFF) {
            index = i;
        } else if (is_float) {
            index = float_index++;
        } else {
            index = integer_index++;
        }
        int limit = is_float ? max_float : max_integer;
        if (index < limit) {
            register_slots[i] = 1;
            register_indices[i] = index;
        } else {
            stack_slots[i] = stack_count++;
        }
    }

    code_printf(parser, "    movq %%rsp, %%r11\n");
    const char **regs64 = get_arg_registers_64();
    const char **regs32 = get_arg_registers_32();
    const char **regs8 = get_arg_registers_8();
    for (int i = 0; i < arg_count; i++) {
        if (!register_slots[i]) continue;
        int source_offset = (arg_count - 1 - i) * 8;
        int reg = register_indices[i];
        DataType type = func->param_types[i];
        if (type == TYPE_FLOAT || type == TYPE_DOUBLE) {
            code_printf(parser, "    movq %d(%%r11), %%xmm%d\n", source_offset, reg);
        } else if (type == TYPE_CHAR || type == TYPE_BYTE || type == TYPE_BIT) {
            code_printf(parser, "    movb %d(%%r11), %s\n", source_offset, regs8[reg]);
        } else if (type == TYPE_STRING || func->param_is_array[i] || func->param_is_pointer[i]) {
            code_printf(parser, "    movq %d(%%r11), %s\n", source_offset, regs64[reg]);
        } else {
            code_printf(parser, "    movl %d(%%r11), %s\n", source_offset, regs32[reg]);
        }
    }

    int base_space = stack_count * 8 + (parser->target_format == TARGET_COFF ? 32 : 0);

    /*
     * Expressions are evaluated directly on the machine stack.  A call may
     * therefore have values belonging to an enclosing expression below its
     * own arguments.  Align the actual stack pointer instead of predicting
     * alignment from this call's argument count.  R15 is non-volatile on both
     * supported ABIs, so it safely carries the exact restoration point across
     * the call; its previous value remains on the expression stack.
     */
    code_printf(parser, "    pushq %%r15\n");
    code_printf(parser, "    movq %%rsp, %%r15\n");
    if (base_space > 0) code_printf(parser, "    subq $%d, %%rsp\n", base_space);
    code_printf(parser, "    andq $-16, %%rsp\n");

    int stack_base = parser->target_format == TARGET_COFF ? 32 : 0;
    for (int i = 0; i < arg_count; i++) {
        if (register_slots[i]) continue;
        int source_offset = (arg_count - 1 - i) * 8;
        int destination_offset = stack_base + stack_slots[i] * 8;
        code_printf(parser, "    movq %d(%%r11), %%rax\n", source_offset);
        code_printf(parser, "    movq %%rax, %d(%%rsp)\n", destination_offset);
    }

    code_printf(parser, "    call %s\n", name);
    code_printf(parser, "    movq %%r15, %%rsp\n");
    code_printf(parser, "    popq %%r15\n");
    if (arg_count > 0) code_printf(parser, "    addq $%d, %%rsp\n", arg_count * 8);
    free(stack_slots);
    free(register_slots);
    free(register_indices);
}

/*
 * generate_write_syscall - Emit write syscall assembly
 * @parser: Parser state
 * @buffer_reg: Register containing buffer address
 * @length_reg: Register containing length
 *
 * Generates assembly code for write(stdout, buffer, length).
 */
void generate_write_syscall(Parser *parser, const char *buffer_reg, const char *length_reg) {
    code_comment(parser, "syscall: write(stdout, buffer, length)");
    code_printf(parser, "    movq $1, %%rax\n");
    code_printf(parser, "    movq $1, %%rdi\n");
    code_printf(parser, "    movq %s, %%rsi\n", buffer_reg);
    code_printf(parser, "    movq %s, %%rdx\n", length_reg);
    code_printf(parser, "    syscall\n");
}

/*
 * generate_read_syscall - Emit read syscall assembly
 * @parser: Parser state
 * @buffer_reg: Register containing buffer address
 * @length_reg: Register containing max length
 *
 * Generates assembly code for read(stdin, buffer, length).
 */
void generate_read_syscall(Parser *parser, const char *buffer_reg, const char *length_reg) {
    code_comment(parser, "syscall: read(stdin, buffer, length)");
    code_printf(parser, "    movq $0, %%rax\n");
    code_printf(parser, "    movq $0, %%rdi\n");
    code_printf(parser, "    movq %s, %%rsi\n", buffer_reg);
    code_printf(parser, "    movq %s, %%rdx\n", length_reg);
    code_printf(parser, "    syscall\n");
}

void generate_strlen_code(Parser *parser, const char *str_ptr_reg, const char *result_reg) {
    code_comment(parser, "Calculate string length");
    code_printf(parser, "    xorq %s, %s\n", result_reg, result_reg);
    code_printf(parser, ".Lstrlen_loop_%d:\n", parser->label_counter);
    code_printf(parser, "    movb (%s,%s,1), %%cl\n", str_ptr_reg, result_reg);
    code_printf(parser, "    testb %%cl, %%cl\n");
    code_printf(parser, "    je .Lstrlen_done_%d\n", parser->label_counter);
    code_printf(parser, "    incq %s\n", result_reg);
    code_printf(parser, "    jmp .Lstrlen_loop_%d\n", parser->label_counter);
    code_printf(parser, ".Lstrlen_done_%d:\n", parser->label_counter);
    parser->label_counter++;
}

void generate_int_to_str_code(Parser *parser, const char *value_reg, const char *buffer_reg) {
    code_comment(parser, "Convert integer to string");
    int label_num = parser->label_counter++;

    code_printf(parser, "    testq %s, %s\n", value_reg, value_reg);
    code_printf(parser, "    jne .Lint2str_nonzero_%d\n", label_num);
    code_printf(parser, "    movb $48, (%s)\n", buffer_reg);
    code_printf(parser, "    movb $0, 1(%s)\n", buffer_reg);
    code_printf(parser, "    jmp .Lint2str_done_%d\n", label_num);

    code_printf(parser, ".Lint2str_nonzero_%d:\n", label_num);

    code_printf(parser, "    movq %s, %%r10\n", value_reg);
    code_printf(parser, "    xorq %%r11, %%r11\n");
    code_printf(parser, "    testq %%r10, %%r10\n");
    code_printf(parser, "    jge .Lint2str_positive_%d\n", label_num);
    code_printf(parser, "    negq %%r10\n");
    code_printf(parser, "    movq $1, %%r11\n");

    code_printf(parser, ".Lint2str_positive_%d:\n", label_num);

    code_printf(parser, "    movq %s, %%r12\n", buffer_reg);
    code_printf(parser, "    addq $20, %%r12\n");
    code_printf(parser, "    movb $0, (%%r12)\n");

    code_printf(parser, ".Lint2str_loop_%d:\n", label_num);
    code_printf(parser, "    xorq %%rdx, %%rdx\n");
    code_printf(parser, "    movq %%r10, %%rax\n");
    code_printf(parser, "    movq $10, %%rcx\n");
    code_printf(parser, "    divq %%rcx\n");
    code_printf(parser, "    addb $48, %%dl\n");
    code_printf(parser, "    decq %%r12\n");
    code_printf(parser, "    movb %%dl, (%%r12)\n");
    code_printf(parser, "    movq %%rax, %%r10\n");
    code_printf(parser, "    testq %%r10, %%r10\n");
    code_printf(parser, "    jne .Lint2str_loop_%d\n", label_num);

    code_printf(parser, "    testq %%r11, %%r11\n");
    code_printf(parser, "    je .Lint2str_no_sign_%d\n", label_num);
    code_printf(parser, "    decq %%r12\n");
    code_printf(parser, "    movb $45, (%%r12)\n");

    code_printf(parser, ".Lint2str_no_sign_%d:\n", label_num);

    code_printf(parser, "    movq %s, %%rdi\n", buffer_reg);
    code_printf(parser, "    movq %%r12, %%rsi\n");
    code_printf(parser, ".Lint2str_copy_%d:\n", label_num);
    code_printf(parser, "    movb (%%rsi), %%al\n");
    code_printf(parser, "    movb %%al, (%%rdi)\n");
    code_printf(parser, "    testb %%al, %%al\n");
    code_printf(parser, "    je .Lint2str_done_%d\n", label_num);
    code_printf(parser, "    incq %%rsi\n");
    code_printf(parser, "    incq %%rdi\n");
    code_printf(parser, "    jmp .Lint2str_copy_%d\n", label_num);

    code_printf(parser, ".Lint2str_done_%d:\n", label_num);
}

const char *get_register_for_type(DataType type, int reg_num) {
    switch (type) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
            if (reg_num == 0) return "%xmm0";
            else if (reg_num == 1) return "%xmm1";
            else return "%xmm2";

        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_BIT:
            if (reg_num == 0) return "%al";
            else if (reg_num == 1) return "%bl";
            else return "%cl";

        case TYPE_INT:
        default:
            if (reg_num == 0) return "%eax";
            else if (reg_num == 1) return "%ebx";
            else return "%ecx";
    }
}

void generate_stack_align(Parser *parser) {
    code_printf(parser, "    pushq %%r15\n");
    code_printf(parser, "    movq %%rsp, %%r15\n");
    int stack_adj = get_call_stack_space();
    if (stack_adj > 0) {
        code_printf(parser, "    subq $%d, %%rsp\n", stack_adj);
    }
    code_printf(parser, "    andq $-16, %%rsp\n");
}

void generate_stack_restore(Parser *parser) {
    (void) parser;
    code_printf(parser, "    movq %%r15, %%rsp\n");
    code_printf(parser, "    popq %%r15\n");
}

void generate_printf_call(Parser *parser) {
    if (parser->target_format == TARGET_ELF) {
        /* SysV variadic ABI: AL is an upper bound on vector arguments. */
        code_printf(parser, "    movl $1, %%eax\n");
    }
    code_printf(parser, "    call printf\n");
}

void save_nonvolatile_registers(Parser *parser) {
    emit_push(parser, "rbx");
    emit_push(parser, "r12");
    emit_push(parser, "r13");
    emit_push(parser, "r14");
    emit_push(parser, "r15");
    if (parser->target_format == TARGET_COFF) {
        emit_push(parser, "rdi");
        emit_push(parser, "rsi");
    }
    /* Both register sets contain an odd number of entries. */
    code_printf(parser, "    subq $8, %%rsp\n");
}

void restore_nonvolatile_registers(Parser *parser) {
    code_printf(parser, "    addq $8, %%rsp\n");
    if (parser->target_format == TARGET_COFF) {
        emit_pop(parser, "rsi");
        emit_pop(parser, "rdi");
    }
    emit_pop(parser, "r15");
    emit_pop(parser, "r14");
    emit_pop(parser, "r13");
    emit_pop(parser, "r12");
    emit_pop(parser, "rbx");
}

const char *get_arg_reg_64(int index) {
    return get_arg_registers_64()[index];
}

const char *get_arg_reg_32(int index) {
    return get_arg_registers_32()[index];
}

const char *get_arg_reg_8(int index) {
    return get_arg_registers_8()[index];
}
