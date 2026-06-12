#include "instruction.h"

#include <stdio.h>
#include <string.h>

static int render(const X64Instruction *instruction, SyntaxMode syntax,
                  const char *expected) {
    FILE *stream = tmpfile();
    if (stream == NULL || !x64_print_instruction(stream, syntax, instruction)) return 0;
    rewind(stream);
    char buffer[256] = {0};
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, stream);
    fclose(stream);
    buffer[length] = '\0';
    return strcmp(buffer, expected) == 0;
}

int main(void) {
    X64Instruction load = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                           x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rbp", -24));
    if (!render(&load, SYNTAX_INTEL, "    mov rax, QWORD PTR [rbp - 24]\n")) return 1;
    if (!render(&load, SYNTAX_ATT, "    movq -24(%rbp), %rax\n")) return 2;

    X64Instruction indexed = x64_instruction2(X64_OP_LEA, X64_WIDTH_QWORD,
                                              x64_register("rbx"),
                                              x64_indexed_memory(X64_WIDTH_NONE, "rax", "rcx", 8, 16));
    if (!render(&indexed, SYNTAX_INTEL, "    lea rbx, [rax + rcx * 8 + 16]\n")) return 3;
    if (!render(&indexed, SYNTAX_ATT, "    leaq 16(%rax,%rcx,8), %rbx\n")) return 4;

    X64Instruction address = x64_instruction2(X64_OP_LEA, X64_WIDTH_QWORD,
                                              x64_register("rdi"), x64_rip_memory(X64_WIDTH_NONE, ".Ltext_", 7));
    if (!render(&address, SYNTAX_INTEL, "    lea rdi, [rip + .Ltext_7]\n")) return 5;
    if (!render(&address, SYNTAX_ATT, "    leaq .Ltext_7(%rip), %rdi\n")) return 6;

    X64Instruction extend = x64_instruction2(X64_OP_MOVSX, X64_WIDTH_QWORD,
                                             x64_sized_register(X64_WIDTH_QWORD, "rax"),
                                             x64_sized_register(X64_WIDTH_DWORD, "eax"));
    if (!render(&extend, SYNTAX_INTEL, "    movsxd rax, eax\n")) return 7;
    if (!render(&extend, SYNTAX_ATT, "    movslq %eax, %rax\n")) return 8;

    X64Instruction floating = x64_instruction2(X64_OP_ADDSD, X64_WIDTH_NONE,
                                               x64_register("xmm2"), x64_register("xmm1"));
    if (!render(&floating, SYNTAX_INTEL, "    addsd xmm2, xmm1\n")) return 9;
    if (!render(&floating, SYNTAX_ATT, "    addsd %xmm1, %xmm2\n")) return 10;

    X64Instruction conversion = x64_instruction2(X64_OP_CVTSI2SSQ, X64_WIDTH_NONE,
                                                 x64_register("xmm0"), x64_register("rax"));
    if (!render(&conversion, SYNTAX_INTEL, "    cvtsi2ss xmm0, rax\n")) return 11;
    if (!render(&conversion, SYNTAX_ATT, "    cvtsi2ssq %rax, %xmm0\n")) return 12;

    X64Instruction condition = x64_instruction1(X64_OP_SETNP, X64_WIDTH_NONE,
                                                x64_register("al"));
    if (!render(&condition, SYNTAX_INTEL, "    setnp al\n")) return 13;
    if (!render(&condition, SYNTAX_ATT, "    setnp %al\n")) return 14;

    X64Instruction branch = x64_instruction1(X64_OP_JNE, X64_WIDTH_NONE,
                                             x64_label(".Lnext"));
    if (!render(&branch, SYNTAX_INTEL, "    jne .Lnext\n")) return 15;
    if (!render(&branch, SYNTAX_ATT, "    jne .Lnext\n")) return 16;

    X64Instruction byte_store = x64_instruction2(X64_OP_MOV, X64_WIDTH_BYTE,
                                                 x64_indexed_memory(X64_WIDTH_BYTE, "rsp", "rax", 1, 0),
                                                 x64_immediate(0));
    if (!render(&byte_store, SYNTAX_INTEL,
                "    mov BYTE PTR [rsp + rax], 0\n"))
        return 17;
    if (!render(&byte_store, SYNTAX_ATT, "    movb $0, (%rsp,%rax,1)\n")) return 18;
    X64Instruction mapped = x64_instruction_with_source(load, 7, 3, 5, 3, 12);
    if (!mapped.has_source || mapped.ir_instruction != 7 || mapped.source_begin_line != 3 ||
        mapped.source_begin_column != 5 || mapped.source_end_line != 3 ||
        mapped.source_end_column != 12)
        return 19;
    if (!render(&mapped, SYNTAX_INTEL, "    mov rax, QWORD PTR [rbp - 24]\n")) return 20;
    return 0;
}
