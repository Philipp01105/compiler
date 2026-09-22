/* Standalone thread and address-wait primitives. Linux workers use raw clone,
   never call libc and have no libc TLS. The parent owns the stack until the
   kernel confirms thread exit by clearing tid. All stacks are ABI aligned. */
static void own_async_wait(Runtime *r) {
    begin(r, "__dmm_async_wait_create", 0);
    if (r->target == TARGET_COFF) {
        imm(r, "rcx", 0);
        imm(r, "rdx", 1); /* manual reset: wakes all parked workers */
        imm(r, "r8", 0);
        imm(r, "r9", 0);
        call(r, "__dmm_os_CreateEventA");
    } else {
        imm(r, "rdi", 1);
        imm(r, "rsi", 8);
        call(r, "calloc");
    }
    test(r, "rax");
    branch(r, X64_OP_JNE, "done");
    op0(r, X64_OP_UD2);
    label(r, "done");
    end(r);

    begin(r, "__dmm_async_wait", 16);
    if (r->target == TARGET_COFF) {
        imm(r, "rdx", -1);
        call(r, "__dmm_os_WaitForSingleObject");
        test(r, "rax");
        branch(r, X64_OP_JE, "done");
        op0(r, X64_OP_UD2);
    } else {
        store(r, "rdi", 8);
        label(r, "wait");
        load(r, "rdi", 8);
        op2(r, X64_OP_CMP, X64_WIDTH_DWORD, x64_memory(X64_WIDTH_DWORD, "rdi", 0), x64_immediate(0));
        branch(r, X64_OP_JNE, "done");
        imm(r, "rdx", 0);
        imm(r, "rsi", 128); /* FUTEX_WAIT_PRIVATE */
        imm(r, "r10", 0);
        imm(r, "r8", 0);
        imm(r, "r9", 0);
        imm(r, "rax", 202);
        op0(r, X64_OP_SYSCALL);
        test(r, "rax");
        branch(r, X64_OP_JE, "wait");
        op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(-11));
        branch(r, X64_OP_JE, "wait"); /* value changed before parking */
        op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(-4));
        branch(r, X64_OP_JE, "wait"); /* EINTR: recheck event predicate */
        op0(r, X64_OP_UD2);
    }
    label(r, "done");
    end(r);

    begin(r, "__dmm_async_wake", 0);
    if (r->target == TARGET_COFF) {
        call(r, "__dmm_os_SetEvent");
        test(r, "rax");
        branch(r, X64_OP_JNE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
    }
    else {
        mov(r, "r10", "rdi");
        imm(r, "r11", 1);
        { const unsigned char exchange[] = {0x4d, 0x87, 0x1a};
          (void) native_bytes(r->object, exchange, sizeof(exchange)); }
        imm(r, "rsi", 129); /* FUTEX_WAKE_PRIVATE */
        imm(r, "rdx", 0x7fffffff);
        imm(r, "r10", 0);
        imm(r, "r8", 0);
        imm(r, "r9", 0);
        imm(r, "rax", 202);
        op0(r, X64_OP_SYSCALL);
        test(r, "rax");
        branch(r, X64_OP_JGE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
    }
    end(r);

    begin(r, "__dmm_async_wait_reset", 0);
    if (r->target == TARGET_COFF) {
        call(r, "__dmm_os_ResetEvent");
        test(r, "rax");
        branch(r, X64_OP_JNE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
    } else {
        mov(r, "r10", "rdi");
        imm(r, "r11", 0);
        { const unsigned char exchange[] = {0x4d, 0x87, 0x1a};
          (void) native_bytes(r->object, exchange, sizeof(exchange)); }
    }
    end(r);

    begin(r, "__dmm_async_wait_destroy", 0);
    if (r->target == TARGET_COFF) {
        call(r, "__dmm_os_CloseHandle");
        test(r, "rax");
        branch(r, X64_OP_JNE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
    } else call(r, "free");
    end(r);
}

static void own_async_threads(Runtime *r) {
    begin(r, "__dmm_async_thread_create", 32);
    if (r->target == TARGET_COFF) {
        mov(r, "r8", "rcx"); /* callback */
        mov(r, "r9", "rdx"); /* argument */
        imm(r, "rcx", 0);
        imm(r, "rdx", 0);
        /* CreateThread's fifth and sixth arguments follow shadow space. */
        stack(r, X64_OP_SUB, 48);
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 32), x64_immediate(0));
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsp", 40), x64_immediate(0));
        op1(r, X64_OP_CALL, x64_label("__dmm_os_CreateThread"));
        stack(r, X64_OP_ADD, 48);
        test(r, "rax");
        branch(r, X64_OP_JNE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
        end(r);
    } else {
        store(r, "rdi", 8);
        store(r, "rsi", 16);
        imm(r, "rdi", 32);
        call(r, "malloc");
        test(r, "rax");
        branch(r, X64_OP_JNE, "control");
        op0(r, X64_OP_UD2);
        label(r, "control");
        store(r, "rax", 24);
        imm(r, "rdi", 1024 * 1024);
        call(r, "malloc");
        test(r, "rax");
        branch(r, X64_OP_JNE, "stack");
        op0(r, X64_OP_UD2);
        label(r, "stack");
        load(r, "r10", 24);
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "r10", 8), x64_register("rax"));
        op2(r, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(1024 * 1024 - 32));
        mov(r, "rsi", "rax");
        load(r, "r11", 8);
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsi", 0), x64_register("r11"));
        load(r, "r11", 16);
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rsi", 8), x64_register("r11"));
        /* Parent and child tid addresses coincide; cleared only at OS exit. */
        mov(r, "rdx", "r10");
        imm(r, "rdi", 0x350f00);
        imm(r, "r8", 0);
        imm(r, "rax", 56);
        op0(r, X64_OP_SYSCALL);
        test(r, "rax");
        branch(r, X64_OP_JE, "child");
        branch(r, X64_OP_JG, "parent");
        op0(r, X64_OP_UD2);
        label(r, "child");
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r11"), x64_memory(X64_WIDTH_QWORD, "rsp", 0));
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rdi"), x64_memory(X64_WIDTH_QWORD, "rsp", 8));
        op1(r, X64_OP_CALL, x64_register("r11"));
        imm(r, "rdi", 0);
        imm(r, "rax", 60); /* exit only this thread */
        op0(r, X64_OP_SYSCALL);
        op0(r, X64_OP_UD2);
        label(r, "parent");
        load(r, "rax", 24);
        end(r);
    }

    begin(r, "__dmm_async_thread_join", 16);
    store(r, arg(r, 0), 8);
    if (r->target == TARGET_COFF) {
        imm(r, "rdx", -1);
        call(r, "__dmm_os_WaitForSingleObject");
        test(r, "rax");
        branch(r, X64_OP_JE, "close");
        op0(r, X64_OP_UD2);
        label(r, "close");
        load(r, "rcx", 8);
        call(r, "__dmm_os_CloseHandle");
        test(r, "rax");
        branch(r, X64_OP_JNE, "done");
        op0(r, X64_OP_UD2);
        label(r, "done");
    } else {
        label(r, "wait");
        load(r, "rdi", 8);
        op2(r, X64_OP_MOV, X64_WIDTH_DWORD, x64_register("edx"), x64_memory(X64_WIDTH_DWORD, "rdi", 0));
        test(r, "rdx");
        branch(r, X64_OP_JE, "release");
        imm(r, "rsi", 0); /* kernel CHILD_CLEARTID uses shared FUTEX_WAKE */
        imm(r, "r10", 0);
        imm(r, "r8", 0);
        imm(r, "r9", 0);
        imm(r, "rax", 202);
        op0(r, X64_OP_SYSCALL);
        test(r, "rax");
        branch(r, X64_OP_JE, "wait");
        op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(-11));
        branch(r, X64_OP_JE, "wait");
        op2(r, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(-4));
        branch(r, X64_OP_JE, "wait");
        op0(r, X64_OP_UD2);
        label(r, "release");
        op2(r, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rdi"), x64_memory(X64_WIDTH_QWORD, "rdi", 8));
        call(r, "free");
        load(r, "rdi", 8);
        call(r, "free");
    }
    end(r);
    own_async_wait(r);
}
