/* Native runtime lowering lives here, independently of language IR emission. */
#include "native_runtime.h"
#include "native/encoder.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    NativeObject *object;
    TargetFormat target;
    const char *function;
} Runtime;

static const char *arg(Runtime *r, int n) {
    static const char *const win[] = {"rcx", "rdx", "r8", "r9"};
    static const char *const unix_args[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    return r->target == TARGET_COFF ? win[n] : unix_args[n];
}

static void op0(Runtime *r, X64Opcode op) {
    (void) native_encode(r->object, &(X64Instruction)
    {
        .opcode = op
    });
}

static void op1(Runtime *r, X64Opcode op, X64Operand a) {
    X64Instruction in = x64_instruction1(op, X64_WIDTH_NONE, a);
    (void) native_encode(r->object, &in);
}

static void op2(Runtime *r, X64Opcode op, X64Width width, X64Operand a, X64Operand b) {
    X64Instruction in = x64_instruction2(op, width, a, b);
    (void) native_encode(r->object, &in);
}

static void mov(Runtime *r, const char *dst, const char *src) {
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register(dst), x64_register(src));
}

static void imm(Runtime *r, const char *dst, long long value) {
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register(dst), x64_immediate(value));
}

static void stack(Runtime *r, X64Opcode op, long long bytes) {
    op2(r, op, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(bytes));
}

static void store(Runtime *r, const char *src, int offset) {
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbp", -offset), x64_register(src));
}

static void load(Runtime *r, const char *dst, int offset) {
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register(dst), x64_memory(X64_WIDTH_QWORD, "rbp", -offset));
}

static void address(Runtime *r, const char *dst, int offset) {
    op2(r, X64_OP_LEA, X64_WIDTH_QWORD, x64_register(dst), x64_memory(X64_WIDTH_NONE, "rbp", -offset));
}

static void data_address(Runtime *r, const char *dst, const char *symbol) {
    X64Operand mem = x64_rip_memory(X64_WIDTH_NONE, symbol, 0);
    mem.has_symbol_suffix = 0;
    op2(r, X64_OP_LEA, X64_WIDTH_QWORD, x64_register(dst), mem);
}

static void call(Runtime *r, const char *symbol) {
    char import[128];
    if (strncmp(symbol, "__dmm_", 6)) {
        snprintf(import, sizeof(import), "__dmm_core_%s", symbol);
        symbol = import;
    }
    if (r->target == TARGET_COFF)stack(r, X64_OP_SUB, 32);
    else op2(r, X64_OP_XOR, X64_WIDTH_DWORD, x64_register("eax"), x64_register("eax"));
    op1(r, X64_OP_CALL, x64_label(symbol));
    if (r->target == TARGET_COFF)stack(r, X64_OP_ADD, 32);
}

static void label_name(Runtime *r, char *out, size_t size, const char *suffix) {
    snprintf(out, size, ".Lnative_%s_%s", r->function, suffix);
}

static void label(Runtime *r, const char *suffix) {
    char name[256];
    label_name(r, name, sizeof(name), suffix);
    (void) native_define(r->object, name, 0, 0);
}

static void branch(Runtime *r, X64Opcode op, const char *suffix) {
    char name[256];
    label_name(r, name, sizeof(name), suffix);
    op1(r, op, x64_label(name));
}

static void test(Runtime *r, const char *value) {
    op2(r, X64_OP_TEST, X64_WIDTH_QWORD, x64_register(value), x64_register(value));
}

static void begin(Runtime *r, const char *name, int frame) {
    r->function = name;
    (void) native_define(r->object, name, 1, 1);
    op1(r, X64_OP_PUSH, x64_register("rbp"));
    mov(r, "rbp", "rsp");
    if (frame)stack(r, X64_OP_SUB, frame);
}

static void end(Runtime *r) {
    mov(r, "rsp", "rbp");
    op1(r, X64_OP_POP, x64_register("rbp"));
    op0(r, X64_OP_RET);
}

static void nullsafe(Runtime *r, int n) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), "nonnull%d", n);
    test(r, arg(r, n));
    branch(r, X64_OP_JNE, suffix);
    data_address(r, arg(r, n), ".Lnative_empty");
    label(r, suffix);
}

static void emit_alias(Runtime *r, const char *name, const char *import, int null_mask) {
    r->function = name;
    (void) native_define(r->object, name, 1, 1);
    for (int n = 0; n < 2; ++n)if (null_mask & (1 << n))nullsafe(r, n);
    char symbol[128];
    snprintf(symbol, sizeof(symbol), "__dmm_core_%s", import);
    op1(r, X64_OP_JMP, x64_label(symbol));
}

/* These routines emit their own machine instructions, with no library imports. */
static void own_strlen(Runtime *r) {
    r->function = "__dmm_core_strlen";
    (void) native_define(r->object, r->function, 0, 1);
    mov(r, "r10", arg(r, 0));
    imm(r, "rax", 0);
    test(r, "r10");
    branch(r, X64_OP_JE, "done");
    label(r, "loop");
    op2(r, X64_OP_CMP, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_immediate(0));
    branch(r, X64_OP_JE, "done");
    op1(r, X64_OP_INC, x64_register("r10"));
    op1(r, X64_OP_INC, x64_register("rax"));
    branch(r, X64_OP_JMP, "loop");
    label(r, "done");
    op0(r, X64_OP_RET);
}

static void own_strcmp(Runtime *r) {
    r->function = "__dmm_core_strcmp";
    (void) native_define(r->object, r->function, 0, 1);
    nullsafe(r, 0);
    nullsafe(r, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "r11", arg(r, 1));
    label(r, "loop");
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_BYTE, "r10", 0));
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rdx"), x64_memory(X64_WIDTH_BYTE, "r11", 0));
    op2(r, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rdx"));
    branch(r, X64_OP_JNE, "done");
    test(r, "rdx");
    branch(r, X64_OP_JE, "done");
    op1(r, X64_OP_INC, x64_register("r10"));
    op1(r, X64_OP_INC, x64_register("r11"));
    branch(r, X64_OP_JMP, "loop");
    label(r, "done");
    op0(r, X64_OP_RET);
}

/* The public AtomicBit/AtomicUsize storage is an aligned machine word.  On
   x86-64, a plain load participates in the SeqCst total order established by
   locked RMW stores; XCHG with memory is implicitly locked. */
static void own_atomics(Runtime *r) {
    r->function = "__dmm_core_atomic_load";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
        x64_memory(X64_WIDTH_QWORD, "r10", 0));
    op0(r, X64_OP_RET);

    r->function = "__dmm_core_atomic_store";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "rax", arg(r, 1));
    { const unsigned char xchg[] = {0x49, 0x87, 0x02};
      (void) native_bytes(r->object, xchg, sizeof(xchg)); }
    op0(r, X64_OP_RET);

    r->function = "__dmm_core_atomic_swap";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "rax", arg(r, 1));
    { const unsigned char xchg[] = {0x49, 0x87, 0x02};
      (void) native_bytes(r->object, xchg, sizeof(xchg)); }
    op0(r, X64_OP_RET);

    r->function = "__dmm_core_atomic_compare_exchange";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "rax", arg(r, 1));
    mov(r, "r11", arg(r, 2));
    { const unsigned char cmpxchg[] = {0xf0, 0x4d, 0x0f, 0xb1, 0x1a};
      (void) native_bytes(r->object, cmpxchg, sizeof(cmpxchg)); }
    op0(r, X64_OP_RET);
}

static void own_copy(Runtime *r, int append) {
    r->function = append ? "__dmm_core_strcat" : "__dmm_core_strcpy";
    (void) native_define(r->object, r->function, 0, 1);
    nullsafe(r, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "r11", arg(r, 1));
    mov(r, "r9", "r10");
    if (append) {
        label(r, "seek");
        op2(r, X64_OP_CMP, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_immediate(0));
        branch(r, X64_OP_JE, "copy");
        op1(r, X64_OP_INC, x64_register("r10"));
        branch(r, X64_OP_JMP, "seek");
    }
    label(r, "copy");
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_BYTE, "r11", 0));
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_register("al"));
    test(r, "rax");
    branch(r, X64_OP_JE, "done");
    op1(r, X64_OP_INC, x64_register("r10"));
    op1(r, X64_OP_INC, x64_register("r11"));
    branch(r, X64_OP_JMP, "copy");
    label(r, "done");
    mov(r, "rax", "r9");
    op0(r, X64_OP_RET);
}

static void own_malloc(Runtime *r) {
    begin(r, "__dmm_core_malloc", 16);
    mov(r, "rax", arg(r, 0));
    op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(16));
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(16));
    branch(r, X64_OP_JL, "zero");
    store(r, "rax", 8);
    if (r->target == TARGET_COFF) {
        mov(r, "rdx", "rax");
        imm(r, "rcx", 0);
        imm(r, "r8", 0x3000);
        imm(r, "r9", 4);
        call(r, "__dmm_os_VirtualAlloc");
        test(r, "rax");
        branch(r, X64_OP_JE, "zero");
    } else {
        mov(r, "rsi", "rax");
        imm(r, "rdi", 0);
        imm(r, "rdx", 3);
        imm(r, "r10", 0x22);
        imm(r, "r8", -1);
        imm(r, "r9", 0);
        imm(r, "rax", 9);
        op0(r, X64_OP_SYSCALL);
        test(r, "rax");
        branch(r, X64_OP_JL, "zero");
    }
    load(r, "r10", 8);
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rax", 0), x64_register("r10"));
    op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(16));
    branch(r, X64_OP_JMP, "done");
    label(r, "zero");
    imm(r, "rax", 0);
    label(r, "done");
    end(r);
}

static void own_free(Runtime *r) {
    begin(r, "__dmm_core_free", 0);
    test(r, arg(r, 0));
    branch(r, X64_OP_JE, "done");
    op2(r, X64_OP_SUB, X64_WIDTH_QWORD, x64_register(arg(r, 0)), x64_immediate(16));
    if (r->target == TARGET_COFF) {
        imm(r, "rdx", 0);
        imm(r, "r8", 0x8000);
        call(r, "__dmm_os_VirtualFree");
    } else {
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rsi"), x64_memory(X64_WIDTH_QWORD, "rdi", 0));
        imm(r, "rax", 11);
        op0(r, X64_OP_SYSCALL);
    }
    label(r, "done");
    end(r);
}

/* Raw byte-region operations. No strings, formatting or allocation policy. */
static void own_core_memory(Runtime *r) {
    r->function = "__dmm_core_null";
    (void) native_define(r->object, r->function, 1, 1);
    imm(r, "rax", 0);
    op0(r, X64_OP_RET);
    r->function = "__dmm_core_offset";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "rax", arg(r, 0));
    op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_register(arg(r, 1)));
    op0(r, X64_OP_RET);
    r->function = "__dmm_core_string_data";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "rax", arg(r, 0));
    op0(r, X64_OP_RET);

    r->function = "__dmm_core_fill";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "rax", arg(r, 1));
    mov(r, "r11", arg(r, 2));
    test(r, "r11");
    branch(r, X64_OP_JL, "trap");
    branch(r, X64_OP_JE, "done");
    test(r, "r10");
    branch(r, X64_OP_JE, "trap");
    label(r, "loop");
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_register("al"));
    op1(r, X64_OP_INC, x64_register("r10"));
    op1(r, X64_OP_DEC, x64_register("r11"));
    branch(r, X64_OP_JNE, "loop");
    label(r, "done");
    op0(r, X64_OP_RET);
    label(r, "trap");
    op0(r, X64_OP_UD2);

    r->function = "__dmm_core_copy";
    (void) native_define(r->object, r->function, 1, 1);
    mov(r, "r10", arg(r, 0));
    mov(r, "r11", arg(r, 1));
    mov(r, "r9", arg(r, 2));
    test(r, "r9");
    branch(r, X64_OP_JL, "trap");
    branch(r, X64_OP_JE, "done");
    test(r, "r10");
    branch(r, X64_OP_JE, "trap");
    test(r, "r11");
    branch(r, X64_OP_JE, "trap");
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("r10"), x64_register("r11"));
    op1(r, X64_OP_SETBE, x64_register("al"));
    op2(r, X64_OP_TEST, X64_WIDTH_BYTE, x64_register("al"), x64_register("al"));
    branch(r, X64_OP_JNE, "forward");
    op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("r10"), x64_register("r9"));
    op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("r11"), x64_register("r9"));
    label(r, "backward");
    op1(r, X64_OP_DEC, x64_register("r10"));
    op1(r, X64_OP_DEC, x64_register("r11"));
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_BYTE, "r11", 0));
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_register("al"));
    op1(r, X64_OP_DEC, x64_register("r9"));
    branch(r, X64_OP_JNE, "backward");
    branch(r, X64_OP_JMP, "done");
    label(r, "forward");
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_BYTE, "r11", 0));
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_memory(X64_WIDTH_BYTE, "r10", 0), x64_register("al"));
    op1(r, X64_OP_INC, x64_register("r10"));
    op1(r, X64_OP_INC, x64_register("r11"));
    op1(r, X64_OP_DEC, x64_register("r9"));
    branch(r, X64_OP_JNE, "forward");
    label(r, "done");
    op0(r, X64_OP_RET);
    label(r, "trap");
    op0(r, X64_OP_UD2);
}

static void own_core_process(Runtime *r) {
    begin(r, "__dmm_core_exit", 0);
    if (r->target == TARGET_COFF) call(r, "__dmm_os_ExitProcess");
    else {
        imm(r, "rax", 60);
        op0(r, X64_OP_SYSCALL);
    }
    op0(r, X64_OP_UD2);
    r->function = "__dmm_core_trap";
    (void) native_define(r->object, r->function, 1, 1);
    op0(r, X64_OP_UD2);
}

static void own_calloc(Runtime *r) {
    begin(r, "__dmm_core_calloc", 0);
    mov(r, "rax", arg(r, 0));
    mov(r, "r10", arg(r, 1));
    test(r, "r10");
    branch(r, X64_OP_JE, "allocate");
    test(r, arg(r, 0));
    branch(r, X64_OP_JL, "zero");
    test(r, "r10");
    branch(r, X64_OP_JL, "zero");
    /* Check n*s against the supported signed allocation range before multiplying. */
    imm(r, "rax", INT64_MAX);
    imm(r, "rdx", 0);
    op1(r, X64_OP_IDIV, x64_register("r10"));
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register(arg(r, 0)), x64_register("rax"));
    branch(r, X64_OP_JG, "zero");
    label(r, "allocate");
    op2(r, X64_OP_IMUL, X64_WIDTH_QWORD, x64_register(arg(r, 0)), x64_register("r10"));
    call(r, "__dmm_core_malloc");
    branch(r, X64_OP_JMP, "done");
    label(r, "zero");
    imm(r, "rax", 0);
    label(r, "done");
    end(r);
}

static void own_strdup(Runtime *r) {
    begin(r, "__dmm_core_strdup", 16);
    nullsafe(r, 0);
    store(r, arg(r, 0), 8);
    call(r, "__dmm_core_strlen");
    op1(r, X64_OP_INC, x64_register("rax"));
    mov(r, arg(r, 0), "rax");
    call(r, "__dmm_core_malloc");
    test(r, "rax");
    branch(r, X64_OP_JE, "done");
    mov(r, arg(r, 0), "rax");
    load(r, arg(r, 1), 8);
    call(r, "__dmm_core_strcpy");
    label(r, "done");
    end(r);
}

#include "standalone.inc"

static void integer_string(Runtime *r) {
    begin(r, "__dmm_rt_int_to_string", 32);
    store(r, arg(r, 0), 8);
    store(r, arg(r, 1), 16);
    store(r, arg(r, 2), 24);
    test(r, arg(r, 1));
    branch(r, X64_OP_JE, "zero");
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register(arg(r, 2)), x64_immediate(0));
    branch(r, X64_OP_JLE, "zero");
    load(r, arg(r, 0), 16);
    load(r, arg(r, 1), 24);
    data_address(r, arg(r, 2), ".Lnative_integer");
    load(r, arg(r, 3), 8);
    call(r, "snprintf");
    load(r, arg(r, 0), 16);
    call(r, "strlen");
    branch(r, X64_OP_JMP, "done");
    label(r, "zero");
    imm(r, "rax", 0);
    label(r, "done");
    end(r);
}

static void string_integer(Runtime *r) {
    begin(r, "__dmm_rt_string_to_int", 0);
    nullsafe(r, 0);
    imm(r, arg(r, 1), 0);
    imm(r, arg(r, 2), 10);
    call(r, "strtoll");
    end(r);
}

static void read_value(Runtime *r) {
    begin(r, "__dmm_rt_read_value", 96);
    store(r, arg(r, 0), 8);
    store(r, arg(r, 1), 16);
    address(r, arg(r, 1), 96);
    imm(r, arg(r, 2), 63);
    call(r, "__dmm_rt_sys_read");
    test(r, "rax");
    branch(r, X64_OP_JLE, "zero");
    address(r, "r10", 96);
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_indexed_memory(X64_WIDTH_BYTE, "r10", "rax", 1, 0), x64_immediate(0));
    load(r, arg(r, 0), 16);
    nullsafe(r, 0);
    data_address(r, arg(r, 1), ".Lnative_char");
    call(r, "strcmp");
    test(r, "rax");
    branch(r, X64_OP_JNE, "integer");
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_BYTE, "rbp", -96));
    branch(r, X64_OP_JMP, "done");
    label(r, "integer");
    address(r, arg(r, 0), 96);
    call(r, "__dmm_rt_string_to_int");
    branch(r, X64_OP_JMP, "done");
    label(r, "zero");
    imm(r, "rax", 0);
    label(r, "done");
    end(r);
}

const char *native_runtime_import(const char *name, TargetFormat target) {
    static const char *const imports[] = {
        "VirtualAlloc", "VirtualFree", "GetStdHandle", "ReadFile",
        "WriteFile", "CreateFileA", "CloseHandle", "GetLastError", "ExitProcess"
    };
    if (target != TARGET_COFF || strncmp(name, "__dmm_os_", 9)) return NULL;
    for (size_t n = 0; n < sizeof(imports) / sizeof(imports[0]); ++n)
        if (!strcmp(name + 9, imports[n])) return imports[n];
    return NULL;
}

int native_runtime_emit(NativeObject *object, TargetFormat target) {
    Runtime r = {object, target, NULL};
    object->section = NATIVE_RODATA;
    const char *const names[] = {".Lnative_empty", ".Lnative_integer", ".Lnative_char"};
    const char *const values[] = {"", "%lld", "%c"};
    for (size_t n = 0; n < 3; ++n) {
        (void) native_define(object, names[n], 0, 0);
        (void) native_bytes(object, values[n], strlen(values[n]) + 1);
    }
    object->section = NATIVE_DATA;
    (void) native_define(object, ".Lnative_buffer", 0, 0);
    (void) native_bytes(object, NULL, 256);
    (void) native_buffer_align(&object->sections[NATIVE_DATA], 8);
    (void) native_define(object, ".Lnative_pending", 0, 0);
    (void) native_uint(object, UINT64_MAX, 8);
    if (target == TARGET_COFF) {
        (void) native_define(object, ".Lnative_handles", 0, 0);
        (void) native_bytes(object, NULL, 256 * 8);
    }
    object->section = NATIVE_TEXT;
    own_strlen(&r);
    own_strcmp(&r);
    own_copy(&r, 0);
    own_copy(&r, 1);
    own_malloc(&r);
    own_free(&r);
    own_calloc(&r);
    own_strdup(&r);
    own_core_memory(&r);
    own_atomics(&r);
    own_core_process(&r);
    emit_alias(&r, "__dmm_rt_strlen", "strlen", 1);
    emit_alias(&r, "__dmm_rt_strcmp", "strcmp", 3);
    emit_alias(&r, "__dmm_rt_strcpy", "strcpy", 2);
    emit_alias(&r, "__dmm_rt_strcat", "strcat", 2);
    emit_alias(&r, "__dmm_rt_strdup", "strdup", 1);
    emit_alias(&r, "__dmm_rt_malloc", "malloc", 0);
    own_parse_integer(&r);
    own_format_integer(&r, 0);
    own_format_integer(&r, 1);
    own_format_float(&r);
    own_snprintf(&r);
    integer_string(&r);
    string_integer(&r);
    if (target == TARGET_COFF) {
        own_handle(&r);
        own_windows_io(&r, 0);
        own_windows_io(&r, 1);
        own_windows_open(&r);
        own_windows_close(&r);
    } else {
        own_linux_io(&r, "__dmm_rt_sys_read", 0, 3);
        own_linux_io(&r, "__dmm_rt_sys_write", 1, 3);
        own_linux_io(&r, "__dmm_rt_sys_open", 2, 3);
        own_linux_io(&r, "__dmm_rt_sys_close", 3, 1);
    }
    own_getbyte(&r);
    own_scan_char(&r);
    own_scan_int(&r);
    own_scan_string(&r);
    read_value(&r);
    (void) native_define(object, "__dmm_entry", 1, 1);
    if (target == TARGET_COFF) {
        stack(&r, X64_OP_SUB, 40);
        op1(&r, X64_OP_CALL, x64_label("__dmm_package_init"));
        op1(&r, X64_OP_CALL, x64_label("main"));
        op2(&r, X64_OP_MOV, X64_WIDTH_QWORD,
            x64_memory(X64_WIDTH_QWORD, "rsp", 32), x64_register("rax"));
        op1(&r, X64_OP_CALL, x64_label("__dmm_package_cleanup"));
        op2(&r, X64_OP_MOV, X64_WIDTH_DWORD, x64_register("ecx"),
            x64_memory(X64_WIDTH_DWORD, "rsp", 32));
        op1(&r, X64_OP_CALL, x64_label("__dmm_os_ExitProcess"));
        op0(&r, X64_OP_UD2);
    } else {
        constant(&r, X64_OP_AND, "rsp", -16);
        op1(&r, X64_OP_CALL, x64_label("__dmm_package_init"));
        op1(&r, X64_OP_CALL, x64_label("main"));
        stack(&r, X64_OP_SUB, 16);
        op2(&r, X64_OP_MOV, X64_WIDTH_QWORD,
            x64_memory(X64_WIDTH_QWORD, "rsp", 0), x64_register("rax"));
        op1(&r, X64_OP_CALL, x64_label("__dmm_package_cleanup"));
        op2(&r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rdi"),
            x64_memory(X64_WIDTH_QWORD, "rsp", 0));
        stack(&r, X64_OP_ADD, 16);
        imm(&r, "rax", 60);
        op0(&r, X64_OP_SYSCALL);
        op0(&r, X64_OP_UD2);
    }
    return !object->failed;
}

int native_runtime_object_imports(NativeObject *object, TargetFormat target) {
    for (size_t n = 0; n < object->symbol_count; ++n) {
        NativeSymbol *symbol = &object->symbols[n];
        if (symbol->defined)continue;
        const char *import = native_runtime_import(symbol->name, target);
        if (!import) {
            native_error(object, "Standalone object contains an unresolved non-OS symbol");
            return 0;
        }
        size_t length = strlen(import) + 1;
        char *copy = malloc(length);
        if (!copy) {
            native_error(object, "Out of memory for OS import name");
            return 0;
        }
        memcpy(copy, import, length);
        free(symbol->name);
        symbol->name = copy;
    }
    return !object->failed;
}

int native_runtime_assembly(FILE *output, TargetFormat target) {
    NativeObject object = {0};
    if (!native_runtime_emit(&object, target) || !native_validate(&object)) {
        native_object_free(&object);
        return 0;
    }
    for (size_t section = 0; section < NATIVE_SECTION_COUNT; ++section) {
        fputs(section == NATIVE_TEXT
                  ? "\n    .text\n"
                  : section == NATIVE_DATA
                        ? "\n    .data\n"
                        : target == TARGET_COFF
                              ? "\n    .section .rdata,\"dr\"\n"
                              : "\n    .section .rodata\n", output);
        fputs("    .balign 16\n", output);
        NativeBuffer *buffer = &object.sections[section];
        for (size_t offset = 0; offset <= buffer->size;) {
            for (size_t n = 0; n < object.symbol_count; ++n) {
                NativeSymbol *symbol = &object.symbols[n];
                if (symbol->defined && symbol->section == (int) section && symbol->offset == offset) {
                    if (symbol->global)fprintf(output, "    .globl %s\n", symbol->name);
                    fprintf(output, "%s:\n", symbol->name);
                }
            }
            if (offset == buffer->size)break;
            const NativeRelocation *relocation = NULL;
            for (size_t n = 0; n < object.relocation_count; ++n)
                if ((size_t) object.relocations[n].section == section && object.relocations[n].offset == offset) {
                    relocation = &object.relocations[n];
                    break;
                }
            if (relocation) {
                NativeSymbol *symbol = &object.symbols[relocation->symbol];
                const char *name = symbol->name;
                if (!symbol->defined && strcmp(name, "main") &&
                    strcmp(name, "__dmm_package_init") &&
                    strcmp(name, "__dmm_package_cleanup")) {
                    name = native_runtime_import(name, target);
                    if (!name) {
                        native_object_free(&object);
                        return 0;
                    }
                    if (target == TARGET_COFF)fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", name);
                }
                fprintf(output, "    %s %s%+lld%s\n", relocation->kind == NATIVE_ADDR64 ? ".quad" : ".long",
                        name, (long long) relocation->addend, relocation->kind == NATIVE_ADDR64 ? "" : " - .");
                offset += relocation->kind == NATIVE_ADDR64 ? 8 : 4;
            } else {
                fprintf(output, "    .byte 0x%02x\n", buffer->data[offset]);
                offset++;
            }
        }
    }
    if (target == TARGET_ELF)fputs("    .section .note.GNU-stack,\"\",@progbits\n", output);
    int success = !ferror(output);
    native_object_free(&object);
    return success;
}
