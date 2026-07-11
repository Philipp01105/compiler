#include "ir_emitter_internal.h"
#include "ir_names.h"
#include "runtime_calls.h"
#include "core_intrinsics.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int signature_parameter(const IrModule *module, IrTypeId id,
                               IrParameter *parameter) {
    if (id >= module->type_count) return 0;
    parameter->type_id = id;
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_PRIMITIVE) parameter->type = type->primitive;
    else if (type->kind == IR_TYPE_FUNCTION) parameter->type = TYPE_UNKNOWN;
    else if (type->kind == IR_TYPE_NAMED) parameter->type = TYPE_UNKNOWN;
    else if (type->kind == IR_TYPE_ARRAY || type->kind == IR_TYPE_SLICE) {
        if (!signature_parameter(module, type->element_type, parameter)) return 0;
        parameter->type_id = id;
        parameter->is_array = type->kind == IR_TYPE_ARRAY;
        parameter->is_slice = type->kind == IR_TYPE_SLICE;
    } else if (type->kind == IR_TYPE_POINTER) {
        if (!signature_parameter(module, type->element_type, parameter)) return 0;
        parameter->type_id = id;
        parameter->pointer_depth++;
    } else return 0;
    return 1;
}

int emit_indirect_typed_call(Emitter *emitter, const IrInstruction *instruction,
                             size_t callable_value, IrTypeId callable_type) {
    if (callable_type >= emitter->module->type_count) return 0;
    const IrType *type = &emitter->module->types[callable_type];
    if (type->kind != IR_TYPE_FUNCTION ||
        type->signature_id >= emitter->module->signature_count) return 0;
    const IrFunctionSignature *signature =
        &emitter->module->signatures[type->signature_id];
    IrParameter *parameters = signature->parameter_count
                                  ? calloc(signature->parameter_count,
                                           sizeof(*parameters))
                                  : NULL;
    if (signature->parameter_count && parameters == NULL) return 0;
    IrFunction callee = {
        .parameters = parameters,
        .parameter_count = signature->parameter_count,
        .return_type_id = signature->return_type
    };
    int valid = 1;
    for (size_t i = 0; i < signature->parameter_count; i++)
        if (!signature_parameter(emitter->module,
                                 signature->parameter_types[i],
                                 &parameters[i])) {
            valid = 0;
            break;
        }
    int emitted = valid && emit_typed_call(emitter, instruction, &callee, 0,
                                           callable_value);
    free(parameters);
    return emitted;
}

int emit_typed_call(Emitter *emitter, const IrInstruction *instruction,
                           const IrFunction *callee, int interface_receiver,
                           size_t indirect_value) {
    const IrFunction *caller = emitter->function;
    size_t stack_count = stack_parameter_count(callee, emitter->target);
    size_t physical_count = physical_parameter_count(callee);
    if ((stack_count & 1U) != 0)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(8));
    for (size_t physical = physical_count; physical-- > 0;) {
        if (parameter_register_index(callee, emitter->target, physical) != IR_VALUE_NONE)
            continue;
        size_t source = 0;
        int is_length = 0;
        if (!physical_parameter(callee, physical, &source, &is_length)) return 0;
        size_t value_id = caller->arguments[instruction->first_argument + source];
        const IrInstruction *value = producer(caller, value_id);
        if (is_length) {
            if (value == NULL || value->type_id >= emitter->module->type_count) return 0;
            const IrType *value_type = &emitter->module->types[value->type_id];
            if (value_type->kind == IR_TYPE_ARRAY)
                write_immediate(emitter, "rax", (long long) value_type->array_length);
            else if (value_type->kind == IR_TYPE_SLICE) {
                write_value_load(emitter, "rax", value_id);
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 8));
            } else return 0;
        } else {
            if (value == NULL) return 0;
            write_value_load(emitter, "rax", value_id);
            if (interface_receiver && source == 0)
                write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(8));
            if (callee->parameters[source].is_slice && emitter->module->types[value->type_id].kind == IR_TYPE_SLICE)
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 0));
            if (callee->parameters[source].pointer_depth == 0 && !callee->parameters[source].is_array && !callee->
                parameters[source].is_slice)
                convert_rax(emitter, value->type, callee->parameters[source].type);
        }
        write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rax"));
    }
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(32));

    for (size_t physical = physical_count; physical-- > 0;) {
        size_t register_index = parameter_register_index(callee, emitter->target, physical);
        if (register_index == IR_VALUE_NONE || !physical_is_floating(callee, physical)) continue;
        size_t source = 0;
        int is_length = 0;
        if (!physical_parameter(callee, physical, &source, &is_length) || is_length) return 0;
        size_t value_id = caller->arguments[instruction->first_argument + source];
        load_floating_value(emitter, value_id, callee->parameters[source].type,
                            (unsigned) register_index);
    }
    for (size_t physical = 0; physical < physical_count; physical++) {
        size_t register_index = parameter_register_index(callee, emitter->target, physical);
        if (register_index == IR_VALUE_NONE || physical_is_floating(callee, physical)) continue;
        size_t source = 0;
        int is_length = 0;
        if (!physical_parameter(callee, physical, &source, &is_length)) return 0;
        size_t value_id = caller->arguments[instruction->first_argument + source];
        const IrInstruction *value = producer(caller, value_id);
        if (is_length) {
            if (value == NULL || value->type_id >= emitter->module->type_count) return 0;
            const IrType *value_type = &emitter->module->types[value->type_id];
            if (value_type->kind == IR_TYPE_ARRAY)
                write_immediate(emitter, "rax", (long long) value_type->array_length);
            else if (value_type->kind == IR_TYPE_SLICE) {
                write_value_load(emitter, "rax", value_id);
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 8));
            } else return 0;
        } else {
            if (value == NULL) return 0;
            write_value_load(emitter, "rax", value_id);
            if (interface_receiver && source == 0)
                write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(8));
            if (callee->parameters[source].is_slice && emitter->module->types[value->type_id].kind == IR_TYPE_SLICE)
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 0));
            if (callee->parameters[source].pointer_depth == 0 && !callee->parameters[source].is_array && !callee->
                parameters[source].is_slice)
                convert_rax(emitter, value->type, callee->parameters[source].type);
        }
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register(argument_register(emitter->target, register_index)),
                    x64_register("rax"));
    }
    if (indirect_value != IR_VALUE_NONE) {
        write_value_load(emitter, "r11", indirect_value);
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                    x64_register("r11"), x64_register("r11"));
        size_t sequence = emitter->bounds_sequence++;
        char valid[80];
        snprintf(valid, sizeof(valid), ".LIR_callable_valid_%zu_%zu",
                 emitter->function_index, sequence);
        write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(valid));
        write_x64_0(emitter, X64_OP_UD2);
        write_labelf(emitter, "%s:\n", valid);
        write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE, x64_register("r11"));
    } else {
        char callee_buffer[4096];
        const char *callee_name = function_link_name(emitter->module, callee, callee_buffer,
                                                     sizeof(callee_buffer));
        if (callee_name == NULL) return 0;
        write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE, x64_label(callee_name));
    }
    size_t cleanup = stack_count * 8U + (emitter->target == TARGET_COFF ? 32U : 0U) +
                     (((stack_count & 1U) != 0) ? 8U : 0U);
    if (cleanup != 0)
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate((long long) cleanup));
    if (instruction->type != TYPE_VOID) {
        if (is_inline_structure(emitter->module, instruction)) {
            size_t offset = aggregate_result_offset(emitter, instruction);
            size_t slots = type_slots(emitter->module, instruction->type_id);
            if (offset == 0 || slots == 0) return 0;
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            copy_aggregate(emitter, slots, "rax", "rbx");
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rbx"));
        } else if (instruction->type == TYPE_FLOAT && !instruction->pointer_depth)
            write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                        x64_register("eax"), x64_register("xmm0"));
        else if (instruction->type == TYPE_DOUBLE && !instruction->pointer_depth)
            write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                        x64_register("rax"), x64_register("xmm0"));
        write_value_store(emitter, "rax", instruction->result);
    }
    return 1;
}

int emit_interface_call(Emitter *emitter, const IrInstruction *instruction) {
    if (!instruction->argument_count) return 0;
    size_t receiver = emitter->function->arguments[instruction->first_argument];
    size_t sequence = emitter->bounds_sequence++;
    char end[80], next[80];
    snprintf(end, sizeof(end), ".LIR_interface_end_%zu_%zu",
             emitter->function_index, sequence);
    for (size_t s = 0; s < emitter->module->structure_count; s++) {
        size_t struct_id = emitter->module->structures[s].symbol_id;
        size_t method_id = semantic_interface_method(emitter->module->semantics,
                                                     instruction->symbol_id, struct_id);
        if (method_id == AST_SYMBOL_NONE) continue;
        const IrFunction *callee = called_function(emitter->module, method_id);
        if (!callee) return 0;
        snprintf(next, sizeof(next), ".LIR_interface_next_%zu_%zu_%zu",
                 emitter->function_index, sequence, s);
        write_value_load(emitter, "rax", receiver);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rax", 0));
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_immediate((long long) (struct_id + 1)));
        write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(next));
        if (!emit_typed_call(emitter, instruction, callee, 1, IR_VALUE_NONE)) return 0;
        write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(end));
        write_labelf(emitter, "%s:\n", next);
    }
    write_x64_0(emitter, X64_OP_UD2);
    write_labelf(emitter, "%s:\n", end);
    return 1;
}

void load_nullable_string(Emitter *emitter, const char *reg, size_t value) {
    size_t label = emitter->bounds_sequence++;
    write_value_load(emitter, reg, value);
    char nonnull[64];
    (void) snprintf(nonnull, sizeof(nonnull), ".LIR_nonnull_%zu_%zu",
                    emitter->function_index, label);
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                x64_register(reg), x64_register(reg));
    write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(nonnull));
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register(reg),
                x64_rip_memory(X64_WIDTH_NONE, ".LIR_empty_string_", 0));
    write_labelf(emitter, ".LIR_nonnull_%zu_%zu:\n",
                 emitter->function_index, label);
}

int emit_builtin_call(Emitter *emitter, const IrInstruction *instruction,
                             const char *name) {
    const RuntimeCall *runtime = runtime_call_find(name);
    if (runtime == NULL || instruction->argument_count != runtime->argument_count ||
        runtime->argument_count > 3)
        return 0;
    const CoreIntrinsic *core = core_intrinsic_find(name);
    for (size_t a = 0; a < runtime->argument_count; a++) {
        size_t value = emitter->function->arguments[instruction->first_argument + a];
        /* RAX conversions cannot clobber previously assigned argument registers. */
        write_value_load(emitter, "rax", value);
        const IrInstruction *argument = producer(emitter->function, value);
        /* Compatibility byte-buffer calls consume data, not the slice descriptor. */
        if (argument->type_id < emitter->module->type_count &&
            emitter->module->types[argument->type_id].kind == IR_TYPE_SLICE)
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rax", 0));
        if (core != NULL && core->arguments[a] != CORE_BYTES)
            convert_rax(emitter, producer(emitter->function, value)->type,
                        core_value_type(core->arguments[a]));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register(argument_register(emitter->target, a)), x64_register("rax"));
    }
    write_call(emitter, runtime->link_name);
    if (instruction->result != IR_VALUE_NONE) {
        if (instruction->pointer_depth == 0 && is_integral(instruction->type))
            normalize_integral_parameter(emitter, instruction->type);
        write_value_store(emitter, "rax", instruction->result);
    }
    return 1;
}

