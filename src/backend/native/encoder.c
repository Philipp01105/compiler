#include "encoder.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

typedef struct {
    int number, bits, xmm;
} Register;

typedef struct {
    unsigned char bytes[32];
    size_t size, fixup;
    const X64Operand *symbol;
    int rex, force_rex, bad;
} Encoding;

static Register reg(const char *name) {
    static const char *const names[4][16] = {
        {
            "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b",
            "r15b"
        },
        {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"},
        {
            "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d",
            "r14d", "r15d"
        },
        {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"}
    };
    Register result = {-1, 0, 0};
    if (!name) return result;
    for (int w = 0; w < 4; ++w)
        for (int n = 0; n < 16; ++n)
            if (!strcmp(name, names[w][n])) {
                result.number = n;
                result.bits = 8 << w;
                return result;
            }
    for (int n = 0; n < 16; ++n) {
        char text[8];
        snprintf(text, sizeof(text), "xmm%d", n);
        if (!strcmp(name, text)) {
            result.number = n;
            result.bits = 128;
            result.xmm = 1;
            return result;
        }
    }
    return result;
}

static int bits(X64Width width) {
    switch (width) {
        case X64_WIDTH_BYTE: return 8;
        case X64_WIDTH_WORD: return 16;
        case X64_WIDTH_DWORD:
        case X64_WIDTH_FLOAT: return 32;
        case X64_WIDTH_QWORD:
        case X64_WIDTH_DOUBLE: return 64;
        default: return 0;
    }
}

static void byte(Encoding *e, unsigned value) {
    if (e->size == sizeof(e->bytes)) {
        e->bad = 1;
        return;
    }
    e->bytes[e->size++] = (unsigned char) value;
}

static void integer(Encoding *e, uint64_t value, size_t count) {
    for (size_t n = 0; n < count; ++n) byte(e, (unsigned) (value >> (n * 8)) & 255u);
}

static int signed32(long long value) { return value >= INT32_MIN && value <= INT32_MAX; }

static int register_number(Encoding *e, const X64Operand *operand) {
    Register r = reg(operand->reg);
    if (operand->kind != X64_OPERAND_REGISTER || r.number < 0) e->bad = 1;
    if (r.bits == 8 && r.number >= 4) e->force_rex = 1;
    return r.number < 0 ? 0 : r.number;
}

/* Construct ModRM/SIB separately, so legacy prefixes always precede REX. */
static void modrm(Encoding *e, int field, const X64Operand *rm) {
    if (field >= 8) e->rex |= 4;
    if (rm->kind == X64_OPERAND_REGISTER) {
        int n = register_number(e, rm);
        if (n >= 8)e->rex |= 1;
        byte(e, 0xc0u | ((unsigned) field & 7u) * 8u | ((unsigned) n & 7u));
        return;
    }
    if (rm->kind != X64_OPERAND_MEMORY) {
        e->bad = 1;
        return;
    }
    if (rm->rip_relative) {
        if (!rm->symbol || !rm->symbol[0]) e->bad=1;
        if (rm->index || !signed32(rm->displacement))e->bad = 1;
        byte(e, ((unsigned) field & 7u) * 8u | 5u);
        e->fixup = e->size;
        e->symbol = rm;
        integer(e, (uint64_t) rm->displacement, 4);
        return;
    }
    Register base = reg(rm->base), index = reg(rm->index);
    if ((rm->base && (base.bits != 64 || base.xmm)) ||
        (rm->index && (index.bits != 64 || index.xmm || index.number == 4)) ||
        !signed32(rm->displacement)) {
        e->bad = 1;
        return;
    }
    int b = base.number, i = index.number, mod = 0;
    if (b >= 8)e->rex |= 1;
    if (i >= 8)e->rex |= 2;
    if (b < 0)mod = 0;
    else if (rm->displacement == 0 && (b & 7) != 5)mod = 0;
    else if (rm->displacement >= -128 && rm->displacement <= 127)mod = 1;
    else mod = 2;
    int sib = i >= 0 || b < 0 || (b & 7) == 4;
    byte(e, (unsigned) mod * 64u + ((unsigned) field & 7u) * 8u + (sib ? 4u : ((unsigned) b & 7u)));
    if (sib) {
        unsigned scale = rm->scale ? rm->scale : 1, shift = 0;
        if (scale == 2)shift = 1;
        else if (scale == 4)shift = 2;
        else if (scale == 8)shift = 3;
        else if (scale != 1)e->bad = 1;
        byte(e, shift * 64u + (i < 0 ? 4u : ((unsigned) i & 7u)) * 8u + (b < 0 ? 5u : ((unsigned) b & 7u)));
    }
    if (b < 0 || mod == 2)integer(e, (uint64_t) rm->displacement, 4);
    else if (mod == 1)byte(e, (unsigned) rm->displacement);
}

static void operation(Encoding *e, unsigned prefix, int wide, unsigned op1, unsigned op2,
                      int field, const X64Operand *rm) {
    Encoding tail = {0};
    modrm(&tail, field, rm);
    if (prefix)byte(e, prefix);
    int rex = tail.rex | (wide ? 8 : 0);
    if (rex || tail.force_rex || e->force_rex)byte(e, 0x40u | (unsigned) rex);
    byte(e, op1);
    if (op2)byte(e, op2);
    if (tail.symbol) {
        e->fixup = e->size + tail.fixup;
        e->symbol = tail.symbol;
    }
    for (size_t n = 0; n < tail.size; ++n)byte(e, tail.bytes[n]);
    e->bad |= tail.bad;
}

static int condition(X64Opcode op) {
    switch (op) {
        case X64_OP_JE:
        case X64_OP_SETE: return 4;
        case X64_OP_JNE:
        case X64_OP_SETNE: return 5;
        case X64_OP_JL:
        case X64_OP_SETL: return 12;
        case X64_OP_JLE:
        case X64_OP_SETLE: return 14;
        case X64_OP_JG:
        case X64_OP_SETG: return 15;
        case X64_OP_JGE:
        case X64_OP_SETGE: return 13;
        case X64_OP_SETA: return 7;
        case X64_OP_SETAE: return 3;
        case X64_OP_SETB: return 2;
        case X64_OP_SETBE: return 6;
        case X64_OP_SETP: return 10;
        case X64_OP_SETNP: return 11;
        default: return -1;
    }
}

int native_encode(NativeObject *object, const X64Instruction *in) {
    if (!object || !in || object->failed)return 0;
    Encoding e = {0};
    const X64Operand *a = &in->operands[0], *b = &in->operands[1];
    Register ar = reg(a->reg), br = reg(b->reg);
    int w = bits(in->width);
    if (!w)w = a->kind == X64_OPERAND_MEMORY ? bits(a->width) : ar.bits == 128 ? bits(a->width) : ar.bits;
    unsigned prefix = w == 16 ? 0x66u : 0u;
    int wide = w == 64;
    X64Opcode op = in->opcode;
    for (size_t n=0; n<in->operand_count && n<3; ++n) {
        const X64Operand *operand=&in->operands[n];
        if (operand->kind==X64_OPERAND_REGISTER && reg(operand->reg).number<0) e.bad=1;
    }
    if (op==X64_OP_MOV || op==X64_OP_MOVABS || op==X64_OP_MOVSX || op==X64_OP_MOVZX ||
        op==X64_OP_LEA || (op>=X64_OP_ADD && op<=X64_OP_DEC)) {
        if (w!=8 && w!=16 && w!=32 && w!=64) e.bad=1;
        if (ar.xmm || br.xmm) e.bad=1;
        if (a->kind==X64_OPERAND_REGISTER && ar.bits!=w) e.bad=1;
        if (op!=X64_OP_MOVSX && op!=X64_OP_MOVZX && b->kind==X64_OPERAND_REGISTER && br.bits!=w) e.bad=1;
    }
    if (op>=X64_OP_SETE && op<=X64_OP_SETNP && a->kind==X64_OPERAND_REGISTER && ar.bits!=8) e.bad=1;
    if (op>=X64_OP_CVTSI2SS && op<=X64_OP_XORPD) {
        int to_integer=op>=X64_OP_CVTTSS2SI && op<=X64_OP_CVTTSD2SIQ;
        int from_integer=op>=X64_OP_CVTSI2SS && op<=X64_OP_CVTSI2SDQ;
        if (a->kind!=X64_OPERAND_REGISTER || (to_integer ? ar.xmm : !ar.xmm)) e.bad=1;
        if (b->kind==X64_OPERAND_REGISTER && (from_integer ? br.xmm : !br.xmm)) e.bad=1;
    }
    size_t expected = 2;
    if (op == X64_OP_RET || op == X64_OP_CQO || op == X64_OP_SYSCALL || op == X64_OP_UD2)expected = 0;
    else if (op == X64_OP_PUSH || op == X64_OP_POP || op == X64_OP_IDIV || op == X64_OP_NEG || op == X64_OP_INC || op ==
             X64_OP_DEC ||
             (op >= X64_OP_CALL && op <= X64_OP_SETNP))
        expected = 1;
    else if (op == X64_OP_IMUL && in->operand_count == 3)expected = 3;
    if(op==X64_OP_DIV)expected=1;
    if (in->operand_count != expected || op < 0 || op >= X64_OP_COUNT)e.bad = 1;
    if (op == X64_OP_RET)byte(&e, 0xc3);
    else if (op == X64_OP_CQO) {
        byte(&e, 0x48);
        byte(&e, 0x99);
    } else if (op == X64_OP_SYSCALL || op == X64_OP_UD2) {
        byte(&e, 0x0f);
        byte(&e, op == X64_OP_SYSCALL ? 5 : 11);
    } else if (op >= X64_OP_CALL && op <= X64_OP_JGE) {
        if (a->kind == X64_OPERAND_SYMBOL) {
            if (op == X64_OP_CALL || op == X64_OP_JMP)byte(&e, op == X64_OP_CALL ? 0xe8 : 0xe9);
            else {
                byte(&e, 0x0f);
                byte(&e, 0x80u + (unsigned) condition(op));
            }
            e.fixup = e.size;
            e.symbol = a;
            integer(&e, 0, 4);
        } else if (op == X64_OP_CALL || op == X64_OP_JMP)operation(&e, 0, 0, 0xff, 0, op == X64_OP_CALL ? 2 : 4, a);
        else e.bad = 1;
    } else if (op >= X64_OP_SETE && op <= X64_OP_SETNP)
        operation(&e, 0, 0, 0x0f, 0x90u + (unsigned) condition(op), 0, a);
    else if (op == X64_OP_PUSH || op == X64_OP_POP) {
        if (a->kind == X64_OPERAND_REGISTER) {
            int n = register_number(&e, a);
            if (ar.bits != 64 || ar.xmm)e.bad = 1;
            if (n >= 8)byte(&e, 0x41);
            byte(&e, (op == X64_OP_PUSH ? 0x50u : 0x58u) + ((unsigned) n & 7u));
        } else if (op == X64_OP_PUSH && a->kind == X64_OPERAND_IMMEDIATE) {
            if (!signed32(a->immediate))e.bad = 1;
            byte(&e, 0x68);
            integer(&e, (uint64_t) a->immediate, 4);
        } else operation(&e, 0, 0, op == X64_OP_PUSH ? 0xff : 0x8f, 0, op == X64_OP_PUSH ? 6 : 0, a);
    } else if (op == X64_OP_MOV || op == X64_OP_MOVABS) {
        if (b->kind == X64_OPERAND_IMMEDIATE && a->kind == X64_OPERAND_REGISTER) {
            int n = register_number(&e, a);
            if (ar.xmm)e.bad = 1;
            if (prefix)byte(&e, prefix);
            if (wide || n >= 8 || e.force_rex)byte(&e, 0x40u | (wide ? 8u : 0u) | (n >= 8 ? 1u : 0u));
            byte(&e, (w == 8 ? 0xb0u : 0xb8u) + ((unsigned) n & 7u));
            integer(&e, (uint64_t) b->immediate, (size_t) w / 8);
        } else if (b->kind == X64_OPERAND_IMMEDIATE) {
            operation(&e, prefix, wide, w == 8 ? 0xc6 : 0xc7, 0, 0, a);
            if (wide && !signed32(b->immediate))e.bad = 1;
            integer(&e, (uint64_t) b->immediate, w == 8 ? 1 : w == 16 ? 2 : 4);
        } else if (a->kind == X64_OPERAND_REGISTER) {
            if (ar.xmm || br.xmm)e.bad = 1;
            operation(&e, prefix, wide, w == 8 ? 0x8a : 0x8b, 0, register_number(&e, a), b);
        } else {
            if (br.xmm)e.bad = 1;
            operation(&e, prefix, wide, w == 8 ? 0x88 : 0x89, 0, register_number(&e, b), a);
        }
    } else if (op == X64_OP_MOVSX || op == X64_OP_MOVZX) {
        int sw = b->kind == X64_OPERAND_REGISTER ? br.bits : bits(b->width);
        if (sw != 8 && sw != 16 && !(sw == 32 && wide && op == X64_OP_MOVSX))e.bad = 1;
        operation(&e, prefix, wide, sw == 32 ? 0x63 : 0x0f,
                  sw == 32 ? 0 : (op == X64_OP_MOVSX ? 0xbeu : 0xb6u) + (sw == 16 ? 1u : 0u), register_number(&e, a),
                  b);
    } else if (op == X64_OP_LEA) {
        if (b->kind != X64_OPERAND_MEMORY)e.bad = 1;
        operation(&e, prefix, wide, 0x8d, 0, register_number(&e, a), b);
    } else if (op == X64_OP_MOVD || op == X64_OP_MOVQ) {
        int q = op == X64_OP_MOVQ;
        if (ar.xmm && br.xmm && q)operation(&e, 0xf3, 0, 0x0f, 0x7e, register_number(&e, a), b);
        else if (ar.xmm)operation(&e, 0x66, q, 0x0f, 0x6e, register_number(&e, a), b);
        else if (br.xmm)operation(&e, 0x66, q, 0x0f, 0x7e, register_number(&e, b), a);
        else e.bad = 1;
    } else if (op >= X64_OP_CVTSI2SS && op <= X64_OP_XORPD) {
        unsigned p = 0, code = 0;
        int qw = 0, field = register_number(&e, a);
        switch (op) {
            case X64_OP_CVTSI2SS:
            case X64_OP_CVTSI2SSQ: p = 0xf3;
                code = 0x2a;
                qw = op == X64_OP_CVTSI2SSQ;
                break;
            case X64_OP_CVTSI2SD:
            case X64_OP_CVTSI2SDQ: p = 0xf2;
                code = 0x2a;
                qw = op == X64_OP_CVTSI2SDQ;
                break;
            case X64_OP_CVTSS2SD: p = 0xf3;
                code = 0x5a;
                break;
            case X64_OP_CVTSD2SS: p = 0xf2;
                code = 0x5a;
                break;
            case X64_OP_CVTTSS2SI:
            case X64_OP_CVTTSS2SIQ: p = 0xf3;
                code = 0x2c;
                qw = op == X64_OP_CVTTSS2SIQ;
                break;
            case X64_OP_CVTTSD2SI:
            case X64_OP_CVTTSD2SIQ: p = 0xf2;
                code = 0x2c;
                qw = op == X64_OP_CVTTSD2SIQ;
                break;
            case X64_OP_ADDSS:
            case X64_OP_ADDSD: code = 0x58;
                p = op == X64_OP_ADDSS ? 0xf3 : 0xf2;
                break;
            case X64_OP_SUBSS:
            case X64_OP_SUBSD: code = 0x5c;
                p = op == X64_OP_SUBSS ? 0xf3 : 0xf2;
                break;
            case X64_OP_MULSS:
            case X64_OP_MULSD: code = 0x59;
                p = op == X64_OP_MULSS ? 0xf3 : 0xf2;
                break;
            case X64_OP_DIVSS:
            case X64_OP_DIVSD: code = 0x5e;
                p = op == X64_OP_DIVSS ? 0xf3 : 0xf2;
                break;
            case X64_OP_UCOMISS: code = 0x2e;
                break;
            case X64_OP_UCOMISD: code = 0x2e;
                p = 0x66;
                break;
            case X64_OP_XORPS: code = 0x57;
                break;
            case X64_OP_XORPD: code = 0x57;
                p = 0x66;
                break;
            default: e.bad = 1;
                break;
        }
        operation(&e, p, qw, 0x0f, code, field, b);
    } else if (op == X64_OP_IMUL) {
        if (in->operand_count == 2 && b->kind == X64_OPERAND_IMMEDIATE) {
            operation(&e, prefix, wide, 0x69, 0, register_number(&e, a), a);
            if (!signed32(b->immediate)) e.bad = 1;
            integer(&e, (uint64_t)b->immediate, w == 16 ? 2 : 4);
        } else operation(&e, prefix, wide, in->operand_count == 3 ? 0x69 : 0x0f, in->operand_count == 3 ? 0 : 0xaf,
                  register_number(&e, a), b);
        if (in->operand_count == 3) {
            const X64Operand *c = &in->operands[2];
            if (c->kind != X64_OPERAND_IMMEDIATE || !signed32(c->immediate))e.bad = 1;
            integer(&e, (uint64_t) c->immediate, w == 16 ? 2 : 4);
        }
    } else if (op == X64_OP_IDIV || op == X64_OP_NEG || op == X64_OP_INC || op == X64_OP_DEC) {
        int group = op == X64_OP_IDIV ? 7 : op == X64_OP_NEG ? 3 : op == X64_OP_INC ? 0 : 1;
        operation(&e, prefix, wide,
                  (op == X64_OP_INC || op == X64_OP_DEC) ? (w == 8 ? 0xfe : 0xff) : (w == 8 ? 0xf6 : 0xf7), 0, group,
                  a);
    } else if (op >= X64_OP_ADD && op <= X64_OP_TEST) {
        int group = op == X64_OP_ADD
                        ? 0
                        : op == X64_OP_OR
                              ? 1
                              : op == X64_OP_AND
                                    ? 4
                                    : op == X64_OP_SUB
                                          ? 5
                                          : op == X64_OP_XOR
                                                ? 6
                                                : 7;
        if (b->kind == X64_OPERAND_IMMEDIATE) {
            operation(&e, prefix, wide, op == X64_OP_TEST ? (w == 8 ? 0xf6 : 0xf7) : (w == 8 ? 0x80 : 0x81), 0,
                      op == X64_OP_TEST ? 0 : group, a);
            if (wide && !signed32(b->immediate))e.bad = 1;
            integer(&e, (uint64_t) b->immediate, w == 8 ? 1 : w == 16 ? 2 : 4);
        } else if (op == X64_OP_TEST)operation(&e, prefix, wide, w == 8 ? 0x84 : 0x85, 0, register_number(&e, b), a);
        else if (a->kind == X64_OPERAND_REGISTER)operation(&e, prefix, wide, (unsigned) group * 8u + (w == 8 ? 2u : 3u),
                                                           0, register_number(&e, a), b);
        else operation(&e, prefix, wide, (unsigned) group * 8u + (w == 8 ? 0u : 1u), 0, register_number(&e, b), a);
    } else if(op==X64_OP_SHL || op==X64_OP_SHR) {
        if(b->kind!=X64_OPERAND_IMMEDIATE || b->immediate<0 || b->immediate>63)e.bad=1;
        operation(&e,prefix,wide,w==8?0xc0:0xc1,0,op==X64_OP_SHL?4:5,a);
        byte(&e,(unsigned)b->immediate);
    } else if(op==X64_OP_DIV)operation(&e,prefix,wide,w==8?0xf6:0xf7,0,6,a);
    else e.bad = 1;
    if (e.bad || e.size == 0 || e.size > 15) {
        char error[128];
        snprintf(error, sizeof(error), "Invalid native x86-64 instruction (opcode %d, IR instruction %zu)", (int) op,
                 in->ir_instruction);
        native_error(object, error);
        return 0;
    }
    size_t start = object->sections[object->section].size;
    if (!native_bytes(object, e.bytes, e.size))return 0;
    if (e.symbol) {
        char name[512];
        const X64Operand *s = e.symbol;
        int count = s->has_symbol_suffix
                        ? snprintf(name, sizeof(name), "%s%zu", s->symbol, s->symbol_suffix)
                        : snprintf(name, sizeof(name), "%s", s->symbol ? s->symbol : "");
        if (count < 0 || (size_t) count >= sizeof(name) || !s->symbol) {
            native_error(object, "Invalid native relocation symbol");
            return 0;
        }
        return native_reference(object, name, op == X64_OP_CALL ? NATIVE_CALL32 : NATIVE_REL32, start + e.fixup,
                                (int64_t) s->displacement - (int64_t) (e.size - e.fixup));
    }
    return 1;
}
