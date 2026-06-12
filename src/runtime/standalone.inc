/* Instruction generators for the standalone runtime. */
static void compare(Runtime *r, const char *reg, long long value) {
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register(reg), x64_immediate(value));
}

static void binary(Runtime *r, X64Opcode op, const char *dst, const char *src) {
    op2(r, op, X64_WIDTH_QWORD, x64_register(dst), x64_register(src));
}

static void constant(Runtime *r, X64Opcode op, const char *dst, long long value) {
    op2(r, op, X64_WIDTH_QWORD, x64_register(dst), x64_immediate(value));
}

static void zero(Runtime *r, const char *reg) { imm(r, reg, 0); }

static void byte_load(Runtime *r, const char *dst, const char *base, const char *index, long long displacement) {
    op2(r, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register(dst),
        x64_indexed_memory(X64_WIDTH_BYTE, base, index, 1, displacement));
}

static void byte_store(Runtime *r, const char *base, const char *index, long long displacement, const char *src) {
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_indexed_memory(X64_WIDTH_BYTE, base, index, 1, displacement),
        x64_register(src));
}

static void memory_constant(Runtime *r, const char *base, const char *index, long long displacement, long long value) {
    op2(r, X64_OP_MOV, X64_WIDTH_BYTE, x64_indexed_memory(X64_WIDTH_BYTE, base, index, 1, displacement),
        x64_immediate(value));
}

static void own_parse_integer(Runtime *r) {
    begin(r, "__dmm_core_strtoll", 0);
    nullsafe(r, 0);
    mov(r, "r10", arg(r, 0));
    zero(r, "r11");
    zero(r, "r9");
    label(r, "space");
    byte_load(r, "rax", "r10", NULL, 0);
    compare(r, "rax", 32);
    branch(r, X64_OP_JE, "skip");
    compare(r, "rax", 9);
    branch(r, X64_OP_JL, "sign");
    compare(r, "rax", 13);
    branch(r, X64_OP_JG, "sign");
    label(r, "skip");
    op1(r, X64_OP_INC, x64_register("r10"));
    branch(r, X64_OP_JMP, "space");
    label(r, "sign");
    compare(r, "rax", 45);
    branch(r, X64_OP_JNE, "plus");
    imm(r, "r9", 1);
    branch(r, X64_OP_JMP, "advance");
    label(r, "plus");
    compare(r, "rax", 43);
    branch(r, X64_OP_JNE, "digits");
    label(r, "advance");
    op1(r, X64_OP_INC, x64_register("r10"));
    label(r, "digits");
    byte_load(r, "rax", "r10", NULL, 0);
    constant(r, X64_OP_SUB, "rax", 48);
    compare(r, "rax", 0);
    branch(r, X64_OP_JL, "done");
    compare(r, "rax", 9);
    branch(r, X64_OP_JG, "done");
    imm(r, "rdx", INT64_C(-922337203685477580));
    binary(r, X64_OP_CMP, "r11", "rdx");
    branch(r, X64_OP_JL, "overflow");
    branch(r, X64_OP_JNE, "accumulate");
    imm(r, "rdx", 7);
    binary(r, X64_OP_ADD, "rdx", "r9");
    binary(r, X64_OP_CMP, "rax", "rdx");
    branch(r, X64_OP_JG, "overflow");
    label(r, "accumulate");
    constant(r, X64_OP_IMUL, "r11", 10);
    binary(r, X64_OP_SUB, "r11", "rax");
    op1(r, X64_OP_INC, x64_register("r10"));
    branch(r, X64_OP_JMP, "digits");
    label(r, "overflow");
    imm(r, "r11", INT64_MIN);
    test(r, "r9");
    branch(r, X64_OP_JNE, "done");
    imm(r, "r11", -INT64_MAX);
    label(r, "done");
    mov(r, "rax", "r11");
    test(r, "r9");
    branch(r, X64_OP_JNE, "return");
    op1(r, X64_OP_NEG, x64_register("rax"));
    label(r, "return");
    end(r);
}

static void own_format_integer(Runtime *r, int unsign) {
    begin(r, unsign ? "__dmm_format_unsigned" : "__dmm_format_integer", 64);
    mov(r, "r10", arg(r, 0));
    mov(r, "rax", arg(r, 1));
    zero(r, "r11");
    zero(r, "r9");
    if (!unsign) {
        test(r, "rax");
        branch(r, X64_OP_JL, "negative");
        op1(r, X64_OP_NEG, x64_register("rax"));
        branch(r, X64_OP_JMP, "digits");
        label(r, "negative");
        imm(r, "r9", 1);
    }
    label(r, "digits");
    address(r, "r8", 64);
    imm(r, "rcx", 10);
    label(r, "divide");
    if (unsign) {
        zero(r, "rdx");
        op1(r, X64_OP_DIV, x64_register("rcx"));
    } else {
        op0(r, X64_OP_CQO);
        op1(r, X64_OP_IDIV, x64_register("rcx"));
        op1(r, X64_OP_NEG, x64_register("rdx"));
    }
    constant(r, X64_OP_ADD, "rdx", 48);
    byte_store(r, "r8", "r11", 0, "dl");
    op1(r, X64_OP_INC, x64_register("r11"));
    test(r, "rax");
    branch(r, X64_OP_JNE, "divide");
    zero(r, "rax");
    test(r, "r9");
    branch(r, X64_OP_JE, "copy");
    memory_constant(r, "r10", NULL, 0, 45);
    op1(r, X64_OP_INC, x64_register("rax"));
    label(r, "copy");
    op1(r, X64_OP_DEC, x64_register("r11"));
    byte_load(r, "rdx", "r8", "r11", 0);
    byte_store(r, "r10", "rax", 0, "dl");
    op1(r, X64_OP_INC, x64_register("rax"));
    test(r, "r11");
    branch(r, X64_OP_JNE, "copy");
    memory_constant(r, "r10", "rax", 0, 0);
    end(r);
}

/* Multiply a decimal, little-endian digit vector by an integer. */
static void decimal_multiply(Runtime *r, const char *suffix, long long factor) {
    char loop[64], carry[64], done[64];
    snprintf(loop, sizeof(loop), "%s_mul", suffix);
    snprintf(carry, sizeof(carry), "%s_carry", suffix);
    snprintf(done, sizeof(done), "%s_end", suffix);
    zero(r, "r9");
    zero(r, "rcx");
    imm(r, "r8", 10);
    label(r, loop);
    byte_load(r, "rax", "r10", "r9", 0);
    constant(r, X64_OP_IMUL, "rax", factor);
    binary(r, X64_OP_ADD, "rax", "rcx");
    zero(r, "rdx");
    op1(r, X64_OP_DIV, x64_register("r8"));
    byte_store(r, "r10", "r9", 0, "dl");
    mov(r, "rcx", "rax");
    op1(r, X64_OP_INC, x64_register("r9"));
    binary(r, X64_OP_CMP, "r9", "r11");
    branch(r, X64_OP_JL, loop);
    label(r, carry);
    test(r, "rcx");
    branch(r, X64_OP_JE, done);
    mov(r, "rax", "rcx");
    zero(r, "rdx");
    op1(r, X64_OP_DIV, x64_register("r8"));
    byte_store(r, "r10", "r11", 0, "dl");
    mov(r, "rcx", "rax");
    op1(r, X64_OP_INC, x64_register("r11"));
    branch(r, X64_OP_JMP, carry);
    label(r, done);
}

static void own_format_float(Runtime *r) {
    /* Exact IEEE binary64 -> fixed six decimals, rounding ties to even. */
    begin(r, "__dmm_format_float", 512);
    store(r, arg(r, 0), 8);
    store(r, arg(r, 1), 16);
    mov(r, "rax", arg(r, 1));
    mov(r, "r8", "rax");
    constant(r, X64_OP_SHR, "r8", 52);
    constant(r, X64_OP_AND, "r8", 2047);
    imm(r, "rdx", INT64_C(0x000fffffffffffff));
    binary(r, X64_OP_AND, "rax", "rdx");
    compare(r, "r8", 2047);
    branch(r, X64_OP_JE, "special");
    test(r, "r8");
    branch(r, X64_OP_JE, "subnormal");
    imm(r, "rdx", INT64_C(0x0010000000000000));
    binary(r, X64_OP_OR, "rax", "rdx");
    constant(r, X64_OP_SUB, "r8", 1069);
    branch(r, X64_OP_JMP, "initial");
    label(r, "subnormal");
    imm(r, "r8", -1068);
    label(r, "initial");
    store(r, "r8", 24);
    address(r, "r10", 512);
    zero(r, "r11");
    imm(r, "r8", 10);
    label(r, "mantissa");
    zero(r, "rdx");
    op1(r, X64_OP_DIV, x64_register("r8"));
    byte_store(r, "r10", "r11", 0, "dl");
    op1(r, X64_OP_INC, x64_register("r11"));
    test(r, "rax");
    branch(r, X64_OP_JNE, "mantissa");
    decimal_multiply(r, "decimal", 15625);
    load(r, "rax", 24);
    test(r, "rax");
    branch(r, X64_OP_JL, "halve_start");
    label(r, "double");
    load(r, "rax", 24);
    test(r, "rax");
    branch(r, X64_OP_JE, "pad");
    decimal_multiply(r, "binary", 2);
    load(r, "rax", 24);
    op1(r, X64_OP_DEC, x64_register("rax"));
    store(r, "rax", 24);
    branch(r, X64_OP_JMP, "double");
    label(r, "halve_start");
    op1(r, X64_OP_NEG, x64_register("rax"));
    store(r, "rax", 24);
    zero(r, "rax");
    store(r, "rax", 32);
    label(r, "halve");
    mov(r, "r9", "r11");
    op1(r, X64_OP_DEC, x64_register("r9"));
    zero(r, "rcx");
    label(r, "half_digit");
    byte_load(r, "rax", "r10", "r9", 0);
    constant(r, X64_OP_IMUL, "rcx", 10);
    binary(r, X64_OP_ADD, "rax", "rcx");
    mov(r, "rcx", "rax");
    constant(r, X64_OP_AND, "rcx", 1);
    constant(r, X64_OP_SHR, "rax", 1);
    byte_store(r, "r10", "r9", 0, "al");
    op1(r, X64_OP_DEC, x64_register("r9"));
    branch(r, X64_OP_JGE, "half_digit");
    label(r, "trim");
    compare(r, "r11", 1);
    branch(r, X64_OP_JLE, "half_count");
    mov(r, "r9", "r11");
    op1(r, X64_OP_DEC, x64_register("r9"));
    byte_load(r, "rax", "r10", "r9", 0);
    test(r, "rax");
    branch(r, X64_OP_JNE, "half_count");
    op1(r, X64_OP_DEC, x64_register("r11"));
    branch(r, X64_OP_JMP, "trim");
    label(r, "half_count");
    load(r, "rax", 24);
    op1(r, X64_OP_DEC, x64_register("rax"));
    store(r, "rax", 24);
    test(r, "rax");
    branch(r, X64_OP_JE, "round");
    load(r, "rax", 32);
    binary(r, X64_OP_OR, "rax", "rcx");
    store(r, "rax", 32);
    branch(r, X64_OP_JMP, "halve");
    label(r, "round");
    test(r, "rcx");
    branch(r, X64_OP_JE, "pad");
    load(r, "rax", 32);
    test(r, "rax");
    branch(r, X64_OP_JNE, "increment_start");
    byte_load(r, "rax", "r10", NULL, 0);
    constant(r, X64_OP_AND, "rax", 1);
    test(r, "rax");
    branch(r, X64_OP_JE, "pad");
    label(r, "increment_start");
    zero(r, "r9");
    label(r, "increment");
    byte_load(r, "rax", "r10", "r9", 0);
    op1(r, X64_OP_INC, x64_register("rax"));
    compare(r, "rax", 10);
    branch(r, X64_OP_JL, "increment_done");
    memory_constant(r, "r10", "r9", 0, 0);
    op1(r, X64_OP_INC, x64_register("r9"));
    binary(r, X64_OP_CMP, "r9", "r11");
    branch(r, X64_OP_JL, "increment");
    op1(r, X64_OP_INC, x64_register("r11"));
    imm(r, "rax", 1);
    label(r, "increment_done");
    byte_store(r, "r10", "r9", 0, "al");
    label(r, "pad");
    compare(r, "r11", 7);
    branch(r, X64_OP_JGE, "output");
    memory_constant(r, "r10", "r11", 0, 0);
    op1(r, X64_OP_INC, x64_register("r11"));
    branch(r, X64_OP_JMP, "pad");
    label(r, "output");
    load(r, "r8", 8);
    zero(r, "r9");
    load(r, "rax", 16);
    constant(r, X64_OP_SHR, "rax", 63);
    test(r, "rax");
    branch(r, X64_OP_JE, "integer_start");
    memory_constant(r, "r8", NULL, 0, 45);
    op1(r, X64_OP_INC, x64_register("r9"));
    label(r, "integer_start");
    mov(r, "rcx", "r11");
    op1(r, X64_OP_DEC, x64_register("rcx"));
    label(r, "integer_digit");
    byte_load(r, "rax", "r10", "rcx", 0);
    constant(r, X64_OP_ADD, "rax", 48);
    byte_store(r, "r8", "r9", 0, "al");
    op1(r, X64_OP_INC, x64_register("r9"));
    op1(r, X64_OP_DEC, x64_register("rcx"));
    compare(r, "rcx", 6);
    branch(r, X64_OP_JGE, "integer_digit");
    memory_constant(r, "r8", "r9", 0, 46);
    op1(r, X64_OP_INC, x64_register("r9"));
    imm(r, "rcx", 5);
    label(r, "fraction_digit");
    byte_load(r, "rax", "r10", "rcx", 0);
    constant(r, X64_OP_ADD, "rax", 48);
    byte_store(r, "r8", "r9", 0, "al");
    op1(r, X64_OP_INC, x64_register("r9"));
    op1(r, X64_OP_DEC, x64_register("rcx"));
    branch(r, X64_OP_JGE, "fraction_digit");
    memory_constant(r, "r8", "r9", 0, 0);
    mov(r, "rax", "r9");
    branch(r, X64_OP_JMP, "done");
    label(r, "special");
    mov(r, "r11", "rax");
    load(r, "r8", 8);
    load(r, "rax", 16);
    constant(r, X64_OP_SHR, "rax", 63);
    mov(r, "r9", "rax");
    test(r, "rax");
    branch(r, X64_OP_JE, "special_text");
    memory_constant(r, "r8", NULL, 0, 45);
    label(r, "special_text");
    test(r, "r11");
    branch(r, X64_OP_JNE, "nan");
    memory_constant(r, "r8", "r9", 0, 105);
    memory_constant(r, "r8", "r9", 1, 110);
    memory_constant(r, "r8", "r9", 2, 102);
    branch(r, X64_OP_JMP, "special_end");
    label(r, "nan");
    memory_constant(r, "r8", "r9", 0, 110);
    memory_constant(r, "r8", "r9", 1, 97);
    memory_constant(r, "r8", "r9", 2, 110);
    label(r, "special_end");
    memory_constant(r, "r8", "r9", 3, 0);
    mov(r, "rax", "r9");
    constant(r, X64_OP_ADD, "rax", 3);
    label(r, "done");
    end(r);
}

static void own_snprintf(Runtime *r) {
    begin(r, "__dmm_core_snprintf", 576);
    store(r, arg(r, 0), 8);
    store(r, arg(r, 1), 16);
    store(r, arg(r, 2), 24);
    store(r, arg(r, 3), 32);
    if (r->target == TARGET_ELF)op2(r, X64_OP_MOVQ, X64_WIDTH_NONE, x64_register("rax"), x64_register("xmm0"));
    else mov(r, "rax", "r9");
    store(r, "rax", 40);
    mov(r, "r10", arg(r, 2));
    byte_load(r, "rax", "r10", NULL, 1);
    compare(r, "rax", 115);
    branch(r, X64_OP_JE, "string");
    compare(r, "rax", 99);
    branch(r, X64_OP_JE, "character");
    compare(r, "rax", 102);
    branch(r, X64_OP_JE, "float");
    compare(r, "rax", 117);
    branch(r, X64_OP_JE, "unsigned");
    address(r, arg(r, 0), 576);
    load(r, arg(r, 1), 32);
    call(r, "__dmm_format_integer");
    branch(r, X64_OP_JMP, "temporary");
    label(r, "unsigned");
    address(r, arg(r, 0), 576);
    load(r, arg(r, 1), 32);
    call(r, "__dmm_format_unsigned");
    branch(r, X64_OP_JMP, "temporary");
    label(r, "float");
    address(r, arg(r, 0), 576);
    load(r, arg(r, 1), 40);
    call(r, "__dmm_format_float");
    branch(r, X64_OP_JMP, "temporary");
    label(r, "character");
    load(r, "rax", 32);
    address(r, "r10", 576);
    byte_store(r, "r10", NULL, 0, "al");
    memory_constant(r, "r10", NULL, 1, 0);
    imm(r, "rax", 1);
    label(r, "temporary");
    store(r, "rax", 48);
    address(r, "r10", 576);
    branch(r, X64_OP_JMP, "copy");
    label(r, "string");
    load(r, arg(r, 0), 32);
    nullsafe(r, 0);
    store(r, arg(r, 0), 56);
    call(r, "__dmm_core_strlen");
    store(r, "rax", 48);
    load(r, "r10", 56);
    label(r, "copy");
    load(r, "r8", 8);
    load(r, "r11", 16);
    test(r, "r8");
    branch(r, X64_OP_JE, "done");
    compare(r, "r11", 0);
    branch(r, X64_OP_JLE, "done");
    op1(r, X64_OP_DEC, x64_register("r11"));
    zero(r, "r9");
    load(r, "rcx", 48);
    label(r, "copy_byte");
    binary(r, X64_OP_CMP, "r9", "r11");
    branch(r, X64_OP_JGE, "terminate");
    binary(r, X64_OP_CMP, "r9", "rcx");
    branch(r, X64_OP_JGE, "terminate");
    byte_load(r, "rax", "r10", "r9", 0);
    byte_store(r, "r8", "r9", 0, "al");
    op1(r, X64_OP_INC, x64_register("r9"));
    branch(r, X64_OP_JMP, "copy_byte");
    label(r, "terminate");
    memory_constant(r, "r8", "r9", 0, 0);
    label(r, "done");
    load(r, "rax", 48);
    end(r);
}

static void own_handle(Runtime *r) {
    begin(r, "__dmm_handle", 0);
    mov(r, "rax", arg(r, 0));
    compare(r, "rax", 0);
    branch(r, X64_OP_JL, "invalid");
    compare(r, "rax", 256);
    branch(r, X64_OP_JGE, "invalid");
    compare(r, "rax", 3);
    branch(r, X64_OP_JGE, "file");
    imm(r, "rcx", -10);
    binary(r, X64_OP_SUB, "rcx", "rax");
    call(r, "__dmm_os_GetStdHandle");
    test(r, "rax");
    branch(r, X64_OP_JE, "invalid");
    branch(r, X64_OP_JMP, "done");
    label(r, "file");
    data_address(r, "r10", ".Lnative_handles");
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_indexed_memory(X64_WIDTH_QWORD, "r10", "rax", 8, 0));
    test(r, "rax");
    branch(r, X64_OP_JNE, "done");
    label(r, "invalid");
    imm(r, "rax", -1);
    label(r, "done");
    end(r);
}

static void own_windows_io(Runtime *r, int writing) {
    begin(r, writing ? "__dmm_rt_sys_write" : "__dmm_rt_sys_read", 32);
    store(r, "rcx", 8);
    store(r, "rdx", 16);
    store(r, "r8", 24);
    compare(r, "r8", 0);
    branch(r, X64_OP_JL, "invalid");
    imm(r, "rax", UINT32_MAX);
    binary(r, X64_OP_CMP, "r8", "rax");
    branch(r, X64_OP_JG, "invalid");
    call(r, "__dmm_handle");
    compare(r, "rax", -1);
    branch(r, X64_OP_JE, "handle_error");
    mov(r, "rcx", "rax");
    load(r, "rdx", 16);
    load(r, "r8", 24);
    address(r, "r9", 32);
    stack(r, X64_OP_SUB, 48);
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 32), x64_immediate(0));
    op1(r, X64_OP_CALL, x64_label(writing ? "__dmm_os_WriteFile" : "__dmm_os_ReadFile"));
    stack(r, X64_OP_ADD, 48);
    test(r, "rax");
    branch(r, X64_OP_JE, "failure");
    op2(r, X64_OP_MOV, X64_WIDTH_DWORD, x64_register("eax"), x64_memory(X64_WIDTH_DWORD, "rbp", -32));
    branch(r, X64_OP_JMP, "done");
    label(r, "failure");
    if (!writing) {
        call(r, "__dmm_os_GetLastError");
        compare(r, "rax", 109);
        branch(r, X64_OP_JNE, "error");
        zero(r, "rax");
        branch(r, X64_OP_JMP, "done");
        label(r, "error");
    }
    label(r, "handle_error");
    imm(r, "rax", -1);
    branch(r, X64_OP_JMP, "done");
    label(r, "invalid");
    imm(r, "rax", -22);
    label(r, "done");
    end(r);
}

static void own_windows_open(Runtime *r) {
    begin(r, "__dmm_rt_sys_open", 32);
    store(r, "rcx", 8);
    store(r, "rdx", 16);
    test(r, "rcx");
    branch(r, X64_OP_JE, "invalid");
    mov(r, "rax", "rdx");
    constant(r, X64_OP_AND, "rax", 3);
    compare(r, "rax", 3);
    branch(r, X64_OP_JE, "invalid");
    imm(r, "r10", INT64_C(0x80000000));
    test(r, "rax");
    branch(r, X64_OP_JE, "access_done");
    imm(r, "r10", INT64_C(0x40000000));
    compare(r, "rax", 1);
    branch(r, X64_OP_JE, "access_done");
    imm(r, "r10", INT64_C(0xc0000000));
    label(r, "access_done");
    op2(r, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rdx"), x64_immediate(1024));
    branch(r, X64_OP_JE, "disposition");
    imm(r, "rax", INT64_C(0x80000000));
    binary(r, X64_OP_AND, "r10", "rax");
    constant(r, X64_OP_OR, "r10", 4);
    label(r, "disposition");
    imm(r, "r11", 3);
    op2(r, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rdx"), x64_immediate(64));
    branch(r, X64_OP_JE, "without_create");
    imm(r, "r11", 4);
    op2(r, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rdx"), x64_immediate(512));
    branch(r, X64_OP_JE, "create_call");
    imm(r, "r11", 2);
    branch(r, X64_OP_JMP, "create_call");
    label(r, "without_create");
    op2(r, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rdx"), x64_immediate(512));
    branch(r, X64_OP_JE, "create_call");
    imm(r, "r11", 5);
    label(r, "create_call");
    mov(r, "rdx", "r10");
    imm(r, "r8", 7);
    zero(r, "r9");
    stack(r, X64_OP_SUB, 64);
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 32), x64_register("r11"));
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 40), x64_immediate(128));
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 48), x64_immediate(0));
    op1(r, X64_OP_CALL, x64_label("__dmm_os_CreateFileA"));
    stack(r, X64_OP_ADD, 64);
    compare(r, "rax", -1);
    branch(r, X64_OP_JE, "done");
    store(r, "rax", 24);
    data_address(r, "r10", ".Lnative_handles");
    imm(r, "r11", 3);
    label(r, "slot");
    op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_indexed_memory(X64_WIDTH_QWORD, "r10", "r11", 8, 0), x64_immediate(0));
    branch(r, X64_OP_JE, "store");
    op1(r, X64_OP_INC, x64_register("r11"));
    compare(r, "r11", 256);
    branch(r, X64_OP_JL, "slot");
    load(r, "rcx", 24);
    call(r, "__dmm_os_CloseHandle");
    imm(r, "rax", -24);
    branch(r, X64_OP_JMP, "done");
    label(r, "store");
    load(r, "rax", 24);
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_indexed_memory(X64_WIDTH_QWORD, "r10", "r11", 8, 0), x64_register("rax"));
    mov(r, "rax", "r11");
    branch(r, X64_OP_JMP, "done");
    label(r, "invalid");
    imm(r, "rax", -22);
    label(r, "done");
    end(r);
}

static void own_windows_close(Runtime *r) {
    begin(r, "__dmm_rt_sys_close", 16);
    store(r, "rcx", 8);
    call(r, "__dmm_handle");
    compare(r, "rax", -1);
    branch(r, X64_OP_JE, "done");
    mov(r, "rcx", "rax");
    call(r, "__dmm_os_CloseHandle");
    test(r, "rax");
    branch(r, X64_OP_JE, "failure");
    load(r, "r11", 8);
    compare(r, "r11", 3);
    branch(r, X64_OP_JL, "success");
    data_address(r, "r10", ".Lnative_handles");
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_indexed_memory(X64_WIDTH_QWORD, "r10", "r11", 8, 0), x64_immediate(0));
    label(r, "success");
    zero(r, "rax");
    branch(r, X64_OP_JMP, "done");
    label(r, "failure");
    imm(r, "rax", -1);
    label(r, "done");
    end(r);
}

static void own_linux_io(Runtime *r, const char *name, long long number, int count) {
    begin(r, name, 0);
    if (count == 3 && number != 2) {
        test(r, "rdx");
        branch(r, X64_OP_JL, "invalid");
    }
    if (number == 2) {
        test(r, "rdi");
        branch(r, X64_OP_JE, "invalid");
    }
    imm(r, "rax", number);
    op0(r, X64_OP_SYSCALL);
    branch(r, X64_OP_JMP, "done");
    label(r, "invalid");
    imm(r, "rax", -22);
    label(r, "done");
    end(r);
}

static void own_getbyte(Runtime *r) {
    begin(r, "__dmm_getbyte", 16);
    data_address(r, "r10", ".Lnative_pending");
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "r10", 0));
    compare(r, "rax", -1);
    branch(r, X64_OP_JE, "read");
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "r10", 0), x64_immediate(-1));
    branch(r, X64_OP_JMP, "done");
    label(r, "read");
    zero(r, arg(r, 0));
    address(r, arg(r, 1), 16);
    imm(r, arg(r, 2), 1);
    call(r, "__dmm_rt_sys_read");
    compare(r, "rax", 1);
    branch(r, X64_OP_JNE, "eof");
    byte_load(r, "rax", "rbp", NULL, -16);
    branch(r, X64_OP_JMP, "done");
    label(r, "eof");
    imm(r, "rax", -1);
    label(r, "done");
    end(r);
}

static void ungetbyte(Runtime *r) {
    data_address(r, "r10", ".Lnative_pending");
    op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "r10", 0), x64_register("rax"));
}

static void whitespace(Runtime *r, const char *space, const char *other) {
    compare(r, "rax", 32);
    branch(r, X64_OP_JE, space);
    compare(r, "rax", 9);
    branch(r, X64_OP_JL, other);
    compare(r, "rax", 13);
    branch(r, X64_OP_JLE, space);
    branch(r, X64_OP_JMP, other);
}

static void own_scan_char(Runtime *r) {
    begin(r, "__dmm_rt_scan_char", 0);
    call(r, "__dmm_getbyte");
    compare(r, "rax", 0);
    branch(r, X64_OP_JGE, "done");
    zero(r, "rax");
    label(r, "done");
    end(r);
}

static void own_scan_string(Runtime *r) {
    begin(r, "__dmm_rt_scan_string", 16);
    zero(r, "rax");
    store(r, "rax", 8);
    label(r, "skip");
    call(r, "__dmm_getbyte");
    test(r, "rax");
    branch(r, X64_OP_JL, "terminate");
    whitespace(r, "skip", "character");
    label(r, "character");
    data_address(r, "r10", ".Lnative_buffer");
    load(r, "r11", 8);
    byte_store(r, "r10", "r11", 0, "al");
    op1(r, X64_OP_INC, x64_register("r11"));
    store(r, "r11", 8);
    compare(r, "r11", 255);
    branch(r, X64_OP_JGE, "terminate");
    call(r, "__dmm_getbyte");
    test(r, "rax");
    branch(r, X64_OP_JL, "terminate");
    whitespace(r, "pushback", "character");
    label(r, "pushback");
    ungetbyte(r);
    label(r, "terminate");
    data_address(r, "r10", ".Lnative_buffer");
    load(r, "r11", 8);
    memory_constant(r, "r10", "r11", 0, 0);
    mov(r, "rax", "r10");
    end(r);
}

static void own_scan_int(Runtime *r) {
    begin(r, "__dmm_rt_scan_int", 32);
    zero(r, "rax");
    store(r, "rax", 8);
    store(r, "rax", 16);
    store(r, "rax", 24);
    label(r, "skip");
    call(r, "__dmm_getbyte");
    test(r, "rax");
    branch(r, X64_OP_JL, "zero");
    whitespace(r, "skip", "sign");
    label(r, "sign");
    compare(r, "rax", 43);
    branch(r, X64_OP_JE, "signed");
    compare(r, "rax", 45);
    branch(r, X64_OP_JNE, "digit");
    imm(r, "r11", 1);
    store(r, "r11", 16);
    label(r, "signed");
    call(r, "__dmm_getbyte");
    label(r, "digit");
    compare(r, "rax", 48);
    branch(r, X64_OP_JL, "finish");
    compare(r, "rax", 57);
    branch(r, X64_OP_JG, "finish");
    load(r, "r11", 24);
    test(r, "r11");
    branch(r, X64_OP_JNE, "next");
    constant(r, X64_OP_SUB, "rax", 48);
    load(r, "r11", 8);
    imm(r, "rdx", INT64_C(-922337203685477580));
    binary(r, X64_OP_CMP, "r11", "rdx");
    branch(r, X64_OP_JL, "overflow");
    branch(r, X64_OP_JNE, "accumulate");
    load(r, "r10", 16);
    imm(r, "rdx", 7);
    binary(r, X64_OP_ADD, "rdx", "r10");
    binary(r, X64_OP_CMP, "rax", "rdx");
    branch(r, X64_OP_JG, "overflow");
    label(r, "accumulate");
    constant(r, X64_OP_IMUL, "r11", 10);
    binary(r, X64_OP_SUB, "r11", "rax");
    store(r, "r11", 8);
    branch(r, X64_OP_JMP, "next");
    label(r, "overflow");
    imm(r, "r11", 1);
    store(r, "r11", 24);
    imm(r, "r11", INT64_MIN);
    load(r, "r10", 16);
    test(r, "r10");
    branch(r, X64_OP_JNE, "saturated");
    imm(r, "r11", -INT64_MAX);
    label(r, "saturated");
    store(r, "r11", 8);
    label(r, "next");
    call(r, "__dmm_getbyte");
    branch(r, X64_OP_JMP, "digit");
    label(r, "finish");
    ungetbyte(r);
    load(r, "rax", 8);
    load(r, "r10", 16);
    test(r, "r10");
    branch(r, X64_OP_JNE, "done");
    op1(r, X64_OP_NEG, x64_register("rax"));
    branch(r, X64_OP_JMP, "done");
    label(r, "zero");
    zero(r, "rax");
    label(r, "done");
    end(r);
}
