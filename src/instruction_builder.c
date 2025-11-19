#include "instruction_builder.h"
#include "parser_internal.h"
#include <stdio.h>

/* Global syntax mode - defaults to Intel for the rewrite */
static SyntaxMode current_syntax = SYNTAX_INTEL;

void set_syntax_mode(SyntaxMode mode) {
    current_syntax = mode;
}

SyntaxMode get_syntax_mode(void) {
    return current_syntax;
}

/*
 * format_register - Convert register name based on syntax mode
 * @reg: Register name without prefix (e.g., "rax", "rbx")
 * 
 * Returns: Register name with appropriate syntax:
 *   - AT&T: "%rax"
 *   - Intel: "rax"
 */
const char *format_register(const char *reg) {
    static char buffer[32];
    if (current_syntax == SYNTAX_ATT) {
        snprintf(buffer, sizeof(buffer), "%%%s", reg);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", reg);
    }
    return buffer;
}

/*
 * format_memory_ref - Format memory reference based on syntax
 * @base: Base register name
 * @offset: Offset from base
 * @buffer: Output buffer
 * @size: Size of output buffer
 * 
 * Returns: Formatted memory reference:
 *   - AT&T: "-8(%rbp)"
 *   - Intel: "[rbp-8]"
 */
const char *format_memory_ref(const char *base, int offset, char *buffer, size_t size) {
    if (current_syntax == SYNTAX_ATT) {
        if (offset == 0) {
            snprintf(buffer, size, "(%%%s)", base);
        } else {
            snprintf(buffer, size, "%d(%%%s)", offset, base);
        }
    } else {
        if (offset == 0) {
            snprintf(buffer, size, "[%s]", base);
        } else if (offset > 0) {
            snprintf(buffer, size, "[%s+%d]", base, offset);
        } else {
            snprintf(buffer, size, "[%s%d]", base, offset);
        }
    }
    return buffer;
}

/*
 * format_indexed_memory - Format indexed memory reference
 * @base: Base register
 * @index: Index register
 * @scale: Scale factor (1, 2, 4, 8)
 * @buffer: Output buffer
 * @size: Size of output buffer
 */
const char *format_indexed_memory(const char *base, const char *index, int scale, char *buffer, size_t size) {
    if (current_syntax == SYNTAX_ATT) {
        snprintf(buffer, size, "(%%%s,%%%s,%d)", base, index, scale);
    } else {
        snprintf(buffer, size, "[%s+%s*%d]", base, index, scale);
    }
    return buffer;
}

/* ========================================
 * Data Movement Instructions
 * ======================================== */

void emit_mov_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    movq %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    mov %s, %s\n", dest, src);
    }
}

void emit_mov_reg_imm(Parser *parser, const char *dest, int value) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    movl $%d, %%%s\n", value, dest);
    } else {
        code_printf(parser, "    mov %s, %d\n", dest, value);
    }
}

void emit_mov_reg_mem(Parser *parser, const char *dest, const char *base, int offset) {
    if (current_syntax == SYNTAX_ATT) {
        if (offset == 0) {
            code_printf(parser, "    movl (%%%s), %%%s\n", base, dest);
        } else {
            code_printf(parser, "    movl %d(%%%s), %%%s\n", offset, base, dest);
        }
    } else {
        if (offset == 0) {
            code_printf(parser, "    mov %s, [%s]\n", dest, base);
        } else if (offset > 0) {
            code_printf(parser, "    mov %s, [%s+%d]\n", dest, base, offset);
        } else {
            code_printf(parser, "    mov %s, [%s%d]\n", dest, base, offset);
        }
    }
}

void emit_mov_mem_reg(Parser *parser, const char *base, int offset, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        if (offset == 0) {
            code_printf(parser, "    movl %%%s, (%%%s)\n", src, base);
        } else {
            code_printf(parser, "    movl %%%s, %d(%%%s)\n", src, offset, base);
        }
    } else {
        if (offset == 0) {
            code_printf(parser, "    mov [%s], %s\n", base, src);
        } else if (offset > 0) {
            code_printf(parser, "    mov [%s+%d], %s\n", base, offset, src);
        } else {
            code_printf(parser, "    mov [%s%d], %s\n", base, offset, src);
        }
    }
}

void emit_movb_reg_mem(Parser *parser, const char *dest, const char *base, int offset) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    movb %d(%%%s), %%%s\n", offset, base, dest);
    } else {
        if (offset == 0) {
            code_printf(parser, "    mov %s, byte ptr [%s]\n", dest, base);
        } else if (offset > 0) {
            code_printf(parser, "    mov %s, byte ptr [%s+%d]\n", dest, base, offset);
        } else {
            code_printf(parser, "    mov %s, byte ptr [%s%d]\n", dest, base, offset);
        }
    }
}

void emit_movb_mem_reg(Parser *parser, const char *base, int offset, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    movb %%%s, %d(%%%s)\n", src, offset, base);
    } else {
        if (offset == 0) {
            code_printf(parser, "    mov byte ptr [%s], %s\n", base, src);
        } else if (offset > 0) {
            code_printf(parser, "    mov byte ptr [%s+%d], %s\n", base, offset, src);
        } else {
            code_printf(parser, "    mov byte ptr [%s%d], %s\n", base, offset, src);
        }
    }
}

void emit_lea(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    leaq %s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    lea %s, %s\n", dest, src);
    }
}

void emit_lea_rip_relative(Parser *parser, const char *dest, const char *label) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    leaq %s(%%rip), %%%s\n", label, dest);
    } else {
        code_printf(parser, "    lea %s, [rel %s]\n", dest, label);
    }
}

/* ========================================
 * Stack Operations
 * ======================================== */

void emit_push(Parser *parser, const char *reg) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    pushq %%%s\n", reg);
    } else {
        code_printf(parser, "    push %s\n", reg);
    }
}

void emit_pop(Parser *parser, const char *reg) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    popq %%%s\n", reg);
    } else {
        code_printf(parser, "    pop %s\n", reg);
    }
}

/* ========================================
 * Arithmetic Instructions
 * ======================================== */

void emit_add_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    addl %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    add %s, %s\n", dest, src);
    }
}

void emit_add_reg_imm(Parser *parser, const char *dest, int value) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    addq $%d, %%%s\n", value, dest);
    } else {
        code_printf(parser, "    add %s, %d\n", dest, value);
    }
}

void emit_sub_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    subl %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    sub %s, %s\n", dest, src);
    }
}

void emit_sub_reg_imm(Parser *parser, const char *dest, int value) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    subq $%d, %%%s\n", value, dest);
    } else {
        code_printf(parser, "    sub %s, %d\n", dest, value);
    }
}

void emit_imul_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    imull %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    imul %s, %s\n", dest, src);
    }
}

void emit_idiv_reg(Parser *parser, const char *divisor) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    idivl %%%s\n", divisor);
    } else {
        code_printf(parser, "    idiv %s\n", divisor);
    }
}

void emit_neg_reg(Parser *parser, const char *reg) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    negl %%%s\n", reg);
    } else {
        code_printf(parser, "    neg %s\n", reg);
    }
}

void emit_inc_reg(Parser *parser, const char *reg) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    incq %%%s\n", reg);
    } else {
        code_printf(parser, "    inc %s\n", reg);
    }
}

void emit_dec_reg(Parser *parser, const char *reg) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    decq %%%s\n", reg);
    } else {
        code_printf(parser, "    dec %s\n", reg);
    }
}

/* ========================================
 * Logical Instructions
 * ======================================== */

void emit_xor_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    xorq %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    xor %s, %s\n", dest, src);
    }
}

void emit_and_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    andl %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    and %s, %s\n", dest, src);
    }
}

void emit_or_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    orl %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    or %s, %s\n", dest, src);
    }
}

void emit_test_reg_reg(Parser *parser, const char *reg1, const char *reg2) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    testb %%%s, %%%s\n", reg2, reg1);
    } else {
        code_printf(parser, "    test %s, %s\n", reg1, reg2);
    }
}

void emit_cmp_reg_reg(Parser *parser, const char *reg1, const char *reg2) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cmpl %%%s, %%%s\n", reg2, reg1);
    } else {
        code_printf(parser, "    cmp %s, %s\n", reg1, reg2);
    }
}

void emit_cmp_reg_imm(Parser *parser, const char *reg, int value) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cmpl $%d, %%%s\n", value, reg);
    } else {
        code_printf(parser, "    cmp %s, %d\n", reg, value);
    }
}

/* ========================================
 * Conversion Instructions
 * ======================================== */

void emit_cltq(Parser *parser) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cltq\n");
    } else {
        code_printf(parser, "    cdqe\n");
    }
}

void emit_cqto(Parser *parser) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cqto\n");
    } else {
        code_printf(parser, "    cqo\n");
    }
}

void emit_cdq(Parser *parser) {
    code_printf(parser, "    cdq\n");
}

void emit_cvtsi2sd_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cvtsi2sd %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    cvtsi2sd %s, %s\n", dest, src);
    }
}

void emit_cvttsd2si_reg_reg(Parser *parser, const char *dest, const char *src) {
    if (current_syntax == SYNTAX_ATT) {
        code_printf(parser, "    cvttsd2si %%%s, %%%s\n", src, dest);
    } else {
        code_printf(parser, "    cvttsd2si %s, %s\n", dest, src);
    }
}

/* ========================================
 * Control Flow
 * ======================================== */

void emit_call(Parser *parser, const char *function) {
    code_printf(parser, "    call %s\n", function);
}

void emit_ret(Parser *parser) {
    code_printf(parser, "    ret\n");
}

void emit_leave(Parser *parser) {
    code_printf(parser, "    leave\n");
}

void emit_jmp(Parser *parser, const char *label) {
    code_printf(parser, "    jmp %s\n", label);
}

void emit_je(Parser *parser, const char *label) {
    code_printf(parser, "    je %s\n", label);
}

void emit_jne(Parser *parser, const char *label) {
    code_printf(parser, "    jne %s\n", label);
}

void emit_jg(Parser *parser, const char *label) {
    code_printf(parser, "    jg %s\n", label);
}

void emit_jge(Parser *parser, const char *label) {
    code_printf(parser, "    jge %s\n", label);
}

void emit_jl(Parser *parser, const char *label) {
    code_printf(parser, "    jl %s\n", label);
}

void emit_jle(Parser *parser, const char *label) {
    code_printf(parser, "    jle %s\n", label);
}

/* ========================================
 * Special Instructions
 * ======================================== */

void emit_syscall(Parser *parser) {
    code_printf(parser, "    syscall\n");
}

void emit_nop(Parser *parser) {
    code_printf(parser, "    nop\n");
}
