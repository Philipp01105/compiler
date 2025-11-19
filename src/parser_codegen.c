#include "parser.h"
#include "parser_internal.h"

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
    int stack_adj = get_call_stack_space();
    if (stack_adj > 0) {
        code_printf(parser, "    subq $%d, %%rsp\n", stack_adj);
    }
}

void generate_stack_restore(Parser *parser) {
    int stack_adj = get_call_stack_space();
    if (stack_adj > 0) {
        code_printf(parser, "    addq $%d, %%rsp\n", stack_adj);
    }
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
