/* C POD x86-64 ABI. Deliberately independent of the DMM slot ABI.
   Only INTEGER/SSE/MEMORY can occur with the supported V1 type set.
   References: AMD64 psABI, low-level-sys-info.tex (x86-psABIs/x86-64-ABI);
   https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention */
typedef enum { NATIVE_NONE, NATIVE_INTEGER, NATIVE_SSE, NATIVE_MEMORY } NativeClass;
typedef struct {
    size_t size, alignment;
    int aggregate, indirect;
    NativeClass classes[2];
} NativeAbiType;
typedef struct {
    NativeAbiType type;
    size_t registers[2], stack, temporary, stage;
} NativeArgument;

static const IrAggregate *native_aggregate(const IrModule *module, IrTypeId id) {
    if (id >= module->type_count || module->types[id].kind != IR_TYPE_NAMED) return NULL;
    for (size_t i = 0; i < module->structure_count; ++i)
        if (module->structures[i].symbol_id == module->types[id].symbol_id &&
            module->structures[i].is_native) return &module->structures[i];
    return NULL;
}

static int native_memory_layout(const IrModule *module, IrTypeId id, size_t *size, size_t *alignment) {
    if (id >= module->type_count) return 0;
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_ARRAY) {
        if (!native_memory_layout(module, type->element_type, size, alignment) ||
            !type->array_length || *size > SIZE_MAX / type->array_length) return 0;
        *size *= type->array_length;
        return 1;
    }
    IrTypeLayout layout;
    if (!ir_type_layout(module, id, &layout)) return 0;
    *size = layout.size;
    *alignment = layout.alignment;
    return 1;
}

static int native_eightbytes(const IrModule *module, IrTypeId id, size_t offset, NativeClass classes[2]) {
    const IrType *type = &module->types[id];
    size_t size, alignment;
    if (!native_memory_layout(module, id, &size, &alignment) || offset % alignment) return 0;
    const IrAggregate *aggregate = native_aggregate(module, id);
    if (aggregate) {
        for (size_t f = 0; f < aggregate->field_count; ++f)
            if (!native_eightbytes(module, aggregate->fields[f].type_id,
                                  offset + aggregate->fields[f].native_offset, classes)) return 0;
    } else if (type->kind == IR_TYPE_ARRAY) {
        size_t stride, ignored;
        if (!native_memory_layout(module, type->element_type, &stride, &ignored)) return 0;
        for (size_t i = 0; i < type->array_length; ++i)
            if (!native_eightbytes(module, type->element_type, offset + i * stride, classes)) return 0;
    } else {
        NativeClass value = type->kind == IR_TYPE_PRIMITIVE &&
                            is_floating(type->primitive) ? NATIVE_SSE : NATIVE_INTEGER;
        for (size_t i = offset / 8; i <= (offset + size - 1) / 8; ++i) {
            if (i >= 2) return 0;
            if (classes[i] == NATIVE_NONE || value == NATIVE_INTEGER) classes[i] = value;
        }
    }
    return 1;
}

static int native_classify(const IrModule *module, TargetFormat target, IrTypeId id, NativeAbiType *out) {
    memset(out, 0, sizeof(*out));
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_PRIMITIVE && type->primitive == TYPE_VOID) return 1;
    if (!native_memory_layout(module, id, &out->size, &out->alignment)) return 0;
    out->aggregate = native_aggregate(module, id) != NULL;
    if (target == TARGET_COFF) {
        out->indirect = out->aggregate && out->size != 1 && out->size != 2 && out->size != 4 && out->size != 8;
        out->classes[0] = !out->aggregate && type->kind == IR_TYPE_PRIMITIVE && is_floating(type->primitive)
                             ? NATIVE_SSE : NATIVE_INTEGER;
    } else if (out->size > 16 || !native_eightbytes(module, id, 0, out->classes)) {
        out->classes[0] = NATIVE_MEMORY;
        out->classes[1] = NATIVE_NONE;
        out->indirect = 1;
    }
    return 1;
}

/* Byte exact operations never read the rounded DMM slot after a C object. */
void copy_bytes(Emitter *emitter, size_t bytes, const char *source, const char *destination) {
    for (size_t offset = 0; offset < bytes;) {
        size_t count = bytes - offset >= 8 ? 8 : bytes - offset >= 4 ? 4 : bytes - offset >= 2 ? 2 : 1;
        X64Width width = count == 8 ? X64_WIDTH_QWORD : count == 4 ? X64_WIDTH_DWORD :
                         count == 2 ? X64_WIDTH_WORD : X64_WIDTH_BYTE;
        const char *reg = count == 8 ? "r11" : count == 4 ? "r11d" : count == 2 ? "r11w" : "r11b";
        write_x64_2(emitter, X64_OP_MOV, width, x64_register(reg),
                    x64_memory(width, source, (long long)offset));
        write_x64_2(emitter, X64_OP_MOV, width, x64_memory(width, destination, (long long)offset),
                    x64_register(reg));
        offset += count;
    }
}

void copy_typed_value(Emitter *emitter, IrTypeId type, const char *source, const char *destination) {
    const IrAggregate *native = native_aggregate(emitter->module, type);
    if (native) copy_bytes(emitter, native->native_layout.size, source, destination);
    else copy_aggregate(emitter, type_slots(emitter->module, type), source, destination);
}

static void native_load_chunk(Emitter *emitter, const char *base, size_t offset, size_t bytes) {
    if (bytes == 8) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                    x64_memory(X64_WIDTH_QWORD, base, (long long)offset));
        return;
    }
    write_immediate(emitter, "rax", 0);
    for (size_t i = bytes; i-- > 0;) {
        if (i != bytes - 1)
            write_x64_2(emitter, X64_OP_SHL, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(8));
        write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD, x64_register("r11"),
                    x64_memory(X64_WIDTH_BYTE, base, (long long)(offset + i)));
        write_x64_2(emitter, X64_OP_OR, X64_WIDTH_QWORD, x64_register("rax"), x64_register("r11"));
    }
}

static void native_store_chunk(Emitter *emitter, const char *base, size_t offset, size_t bytes) {
    if (bytes == 8) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, base, (long long)offset), x64_register("rax"));
        return;
    }
    for (size_t i = 0; i < bytes; ++i) {
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE,
                    x64_memory(X64_WIDTH_BYTE, base, (long long)(offset + i)), x64_register("al"));
        if (i + 1 < bytes)
            write_x64_2(emitter, X64_OP_SHR, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(8));
    }
}

static void native_stack_address(Emitter *emitter, const char *reg, size_t offset) {
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register(reg),
                x64_memory(X64_WIDTH_NONE, "rsp", (long long)offset));
}

int emit_native_call(Emitter *emitter, const IrInstruction *instruction, const IrNativeImport *callee) {
    NativeAbiType result;
    if (!native_classify(emitter->module, emitter->target, callee->return_type_id, &result)) return 0;
    size_t count = callee->parameter_count;
    NativeArgument *args = calloc(count ? count : 1, sizeof(*args));
    if (!args) return 0;
    size_t integer = result.indirect ? 1 : 0, sse = 0;
    size_t stack = emitter->target == TARGET_COFF ? 32 : 0;
    int valid = 1;
    for (size_t i = 0; i < count; ++i) {
        NativeArgument *a = &args[i];
        a->registers[0] = a->registers[1] = IR_VALUE_NONE;
        a->stack = IR_VALUE_NONE;
        if (!native_classify(emitter->module, emitter->target, callee->parameter_types[i], &a->type)) {valid = 0; break;}
        if (emitter->target == TARGET_COFF) {
            size_t position = i + (result.indirect ? 1 : 0);
            if (position < 4) a->registers[0] = position;
            else { a->stack = stack; stack += 8; }
        } else {
            size_t need_integer = 0, need_sse = 0;
            for (size_t c = 0; c < 2; ++c) {
                need_integer += a->type.classes[c] == NATIVE_INTEGER;
                need_sse += a->type.classes[c] == NATIVE_SSE;
            }
            if (a->type.indirect || integer + need_integer > 6 || sse + need_sse > 8) {
                /* No registers are consumed when a whole aggregate spills. */
                a->stack = stack;
                stack += (a->type.size + 7) & ~(size_t)7;
            } else for (size_t c = 0; c < 2; ++c) {
                if (a->type.classes[c] == NATIVE_INTEGER) a->registers[c] = integer++;
                if (a->type.classes[c] == NATIVE_SSE) a->registers[c] = sse++;
            }
        }
    }
    size_t total = (stack + 15) & ~(size_t)15;
    for (size_t i = 0; valid && i < count; ++i) {
        NativeArgument *a = &args[i];
        a->temporary = total;
        if (emitter->target == TARGET_COFF && a->type.indirect)
            total += (a->type.size + 15) & ~(size_t)15;
        a->stage = total;
        total += 16;
    }
    if (!valid || total > INT32_MAX) {free(args); return 0;}
    if (total) write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate((long long)total));
    /* IR has already evaluated each argument exactly once. Stage bits before
       loading ABI registers so conversion/copy scratch registers cannot collide. */
    for (size_t i = 0; i < count; ++i) {
        NativeArgument *a = &args[i];
        size_t value_id = emitter->function->arguments[instruction->first_argument + i];
        const IrInstruction *value = producer(emitter->function, value_id);
        write_value_load(emitter, "rax", value_id);
        if (a->type.aggregate) {
            if (a->type.indirect && emitter->target == TARGET_COFF) {
                native_stack_address(emitter, "rbx", a->temporary);
                copy_bytes(emitter, a->type.size, "rax", "rbx");
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rbx"));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_memory(X64_WIDTH_QWORD, "rsp", (long long)a->stage), x64_register("rax"));
            } else if (a->stack != IR_VALUE_NONE && emitter->target == TARGET_ELF) {
                native_stack_address(emitter, "rbx", a->stack);
                copy_bytes(emitter, a->type.size, "rax", "rbx");
                continue;
            } else {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r10"), x64_register("rax"));
                for (size_t c = 0; c < (a->type.size + 7) / 8; ++c) {
                    size_t bytes = a->type.size - c * 8;
                    native_load_chunk(emitter, "r10", c * 8, bytes < 8 ? bytes : 8);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_memory(X64_WIDTH_QWORD, "rsp", (long long)(a->stage + c * 8)), x64_register("rax"));
                }
            }
        } else {
            const IrType *type = &emitter->module->types[callee->parameter_types[i]];
            if (type->kind == IR_TYPE_PRIMITIVE) convert_rax(emitter, value->type, type->primitive);
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rsp", (long long)a->stage), x64_register("rax"));
        }
        if (a->stack != IR_VALUE_NONE) {
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                        x64_memory(X64_WIDTH_QWORD, "rsp", (long long)a->stage));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rsp", (long long)a->stack), x64_register("rax"));
        }
    }
    for (size_t i = 0; i < count; ++i) for (size_t c = 0; c < 2; ++c) {
        NativeArgument *a = &args[i];
        if (a->registers[c] == IR_VALUE_NONE) continue;
        if (a->type.classes[c] == NATIVE_SSE) {
            char xmm[16]; snprintf(xmm, sizeof(xmm), "xmm%zu", a->registers[c]);
            write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE, x64_register(xmm),
                        x64_memory(X64_WIDTH_QWORD, "rsp", (long long)(a->stage + c * 8)));
        } else write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                          x64_register(argument_register(emitter->target, a->registers[c])),
                          x64_memory(X64_WIDTH_QWORD, "rsp", (long long)(a->stage + c * 8)));
    }
    size_t result_offset = result.aggregate ? aggregate_result_offset(emitter, instruction) : 0;
    if (result.indirect)
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                    x64_register(argument_register(emitter->target, 0)),
                    x64_memory(X64_WIDTH_NONE, "rbp", -(long long)result_offset));
    write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE, x64_label(callee->native_name));
    if (total) write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rsp"), x64_immediate((long long)total));
    if (result.aggregate) {
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_memory(X64_WIDTH_NONE, "rbp", -(long long)result_offset));
        if (!result.indirect) {
            size_t ri = 0, rf = 0;
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r10"), x64_register("rax"));
            for (size_t c = 0; c < (result.size + 7) / 8; ++c) {
                if (result.classes[c] == NATIVE_SSE) {
                    write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE, x64_register("rax"), x64_register(rf++ ? "xmm1" : "xmm0"));
                } else if (result.classes[c] == NATIVE_INTEGER)
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_register(ri++ ? "rdx" : "r10"));
                else continue;
                size_t bytes = result.size - c * 8;
                native_store_chunk(emitter, "rbx", c * 8, bytes < 8 ? bytes : 8);
            }
        }
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rbx"));
    } else if (result.size) {
        const IrType *type = &emitter->module->types[callee->return_type_id];
        if (result.classes[0] == NATIVE_SSE)
            write_x64_2(emitter, type->primitive == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE,
                        x64_register(type->primitive == TYPE_FLOAT ? "eax" : "rax"), x64_register("xmm0"));
        else if (type->kind == IR_TYPE_PRIMITIVE) normalize_integral_parameter(emitter, type->primitive);
    }
    if (instruction->result != IR_VALUE_NONE) write_value_store(emitter, "rax", instruction->result);
    free(args);
    return 1;
}
