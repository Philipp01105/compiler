/* Private native ABI: [poll, cleanup, state, result..., pinned storage...].
   r14 addresses the top of pinned storage, exactly like the ordinary rbp
   storage layout. The real stack is used only for ABI calls and saved regs. */
static long long async_header_offset(const Emitter *emitter, size_t field) {
    return -(long long) emitter->async_frame_bytes + (long long) field;
}

static int copy_async_storage(Emitter *emitter, IrTypeId type, const char *source, const char *destination) {
    IrTypeLayout layout;
    if (!ir_type_layout(emitter->module, type, &layout)) return 0;
    size_t bytes = layout.size;
    if (emitter->module->types[type].kind == IR_TYPE_ARRAY) {
        const IrType *array = &emitter->module->types[type];
        if (!ir_type_layout(emitter->module, array->element_type, &layout) ||
            array->array_length > SIZE_MAX / layout.size) return 0;
        bytes = array->array_length * layout.size;
    }
    size_t offset = 0;
    for (; bytes - offset >= 8; offset += 8) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rcx"),
                    x64_memory(X64_WIDTH_QWORD, source, (long long) offset));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, destination, (long long) offset), x64_register("rcx"));
    }
    for (; offset < bytes; offset++) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE, x64_register("cl"),
                    x64_memory(X64_WIDTH_BYTE, source, (long long) offset));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE,
                    x64_memory(X64_WIDTH_BYTE, destination, (long long) offset), x64_register("cl"));
    }
    return 1;
}

static void async_copy_parameter_word(Emitter *emitter, size_t offset) {
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rbx"),
                x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", -(long long) offset), x64_register("rbx"));
}

static void async_callback(Emitter *emitter, size_t value, size_t field) {
    write_value_load(emitter, "rax", value);
    write_register_move(emitter, argument_register(emitter->target, 0), "rax");
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r11"),
                x64_memory(X64_WIDTH_QWORD, "rax", (long long) field));
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(32));
    write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE, x64_register("r11"));
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(32));
}

static int emit_async_await(Emitter *emitter, const IrInstruction *instruction) {
    char ready[96];
    (void) snprintf(ready, sizeof(ready), ".LIR_async_ready_%zu_%zu",
                    emitter->function_index, instruction->target_a);
    write_labelf(emitter, ".LIR_async_resume_%zu_%zu:\n",
                 emitter->function_index, instruction->target_a);
    async_callback(emitter, instruction->operand_a, 0);
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
    write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(ready));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)),
                x64_immediate((long long) instruction->target_a));
    char pending[80];
    (void) snprintf(pending, sizeof(pending), ".LIR_async_pending_%zu", emitter->function_index);
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(pending));
    write_labelf(emitter, "%s:\n", ready);
    write_value_load(emitter, "rax", instruction->operand_a);
    if (instruction->result != IR_VALUE_NONE) {
        if (is_inline_structure(emitter->module, instruction) || instruction->is_array) {
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(24));
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) aggregate_result_offset(emitter, instruction)));
            if (!copy_async_storage(emitter, instruction->type_id, "rax", "rbx")) return 0;
            write_register_move(emitter, "rax", "rbx");
        } else
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                        x64_memory(X64_WIDTH_QWORD, "rax", 24));
        write_value_store(emitter, "rax", instruction->result);
    }
    async_callback(emitter, instruction->operand_a, 8);
    return 1;
}

static void async_complete(Emitter *emitter) {
    IrTypeId output = emitter->function->return_type_id;
    if (type_is_structure(emitter->module, output) ||
        emitter->module->types[output].kind == IR_TYPE_ARRAY) {
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_memory(X64_WIDTH_NONE, "r14", async_header_offset(emitter, 24)));
        (void) copy_async_storage(emitter, output, "rax", "rbx");
    } else
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 24)), x64_register("rax"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)), x64_immediate(-1));
    write_immediate(emitter, "rax", 1);
}

/* Called after ordinary parameter spills. No user body runs in a constructor. */
static int async_construct_and_poll(Emitter *emitter) {
    size_t slots = type_slots(emitter->module, emitter->function->return_type_id);
    if (slots == 0) slots = 1;
    emitter->async_frame_bytes = emitter->frame_size + 8 + 24 + slots * 8;
    write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate(16));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", 0), x64_register("r14"));
    write_immediate(emitter, argument_register(emitter->target, 0), 1);
    write_immediate(emitter, argument_register(emitter->target, 1), (long long) emitter->async_frame_bytes);
    write_call(emitter, "calloc");
    char allocated[80];
    (void) snprintf(allocated, sizeof(allocated), ".LIR_async_allocated_%zu", emitter->function_index);
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rax"));
    write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(allocated));
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", allocated);
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("r14"),
                x64_memory(X64_WIDTH_NONE, "rax", (long long) emitter->async_frame_bytes));
    for (size_t p = 0; p < emitter->function->parameter_count; p++) {
        const IrParameter *parameter = &emitter->function->parameters[p];
        async_copy_parameter_word(emitter, parameter_offset(emitter, parameter));
        if (parameter->is_slice) async_copy_parameter_word(emitter, slice_length_offset(emitter, p));
        if (!parameter->is_receiver &&
            (ir_type_properties(emitter->module, parameter->type_id) & SEMANTIC_TYPE_NEEDS_DROP))
            async_copy_parameter_word(emitter, parameter_flag_offset(emitter, parameter));
        if (!type_is_structure(emitter->module, parameter->type_id) &&
            emitter->module->types[parameter->type_id].kind != IR_TYPE_ARRAY) continue;
        size_t copy_offset = parameter_copy_offset(emitter, p);
        for (size_t s = 0; s < type_slots(emitter->module, parameter->type_id); s++)
            async_copy_parameter_word(emitter, copy_offset - s * 8);
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_memory(X64_WIDTH_NONE, "r14", -(long long) parameter_copy_offset(emitter, p)));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "r14", -(long long) parameter_offset(emitter, parameter)),
                    x64_register("rbx"));
    }
    X64Operand poll = x64_rip_memory(X64_WIDTH_NONE, ".LIR_async_poll_", emitter->function_index);
    X64Operand cleanup = x64_rip_memory(X64_WIDTH_NONE, ".LIR_async_cleanup_", emitter->function_index);
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"), poll);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rax", 0), x64_register("rbx"));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"), cleanup);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rax", 8), x64_register("rbx"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r14"), x64_memory(X64_WIDTH_QWORD, "rsp", 0));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rsp"), x64_memory(X64_WIDTH_NONE, "rbp", -8));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);

    write_labelf(emitter, ".LIR_async_cleanup_%zu:\n", emitter->function_index);
    const char *arg = argument_register(emitter->target, 0);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, arg, 16), x64_immediate(-1));
    char valid[80];
    (void) snprintf(valid, sizeof(valid), ".LIR_async_cleanup_valid_%zu", emitter->function_index);
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(valid));
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", valid);
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_call(emitter, "free");
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);

    write_labelf(emitter, ".LIR_async_poll_%zu:\n", emitter->function_index);
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_register_move(emitter, "rbp", "rsp");
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("r14"));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("r14"),
                x64_memory(X64_WIDTH_NONE, arg, (long long) emitter->async_frame_bytes));
    emitter->async_storage = 1;
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                x64_memory(X64_WIDTH_QWORD, "r14", async_header_offset(emitter, 16)));
    char start[80];
    (void) snprintf(start, sizeof(start), ".LIR_async_start_%zu", emitter->function_index);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(0));
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(start));
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *instruction = &emitter->function->instructions[i];
        if (instruction->opcode != IR_OP_AWAIT) continue;
        char resume[96];
        (void) snprintf(resume, sizeof(resume), ".LIR_async_resume_%zu_%zu",
                        emitter->function_index, instruction->target_a);
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate((long long) instruction->target_a));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(resume));
    }
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", start);
    return 1;
}
