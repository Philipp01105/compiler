#include "ir_emitter_internal.h"
#include "ir_names.h"
#include "instruction.h"
#include "native/encoder.h"
#include "runtime_calls.h"
#include "core_intrinsics.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_x64(const Emitter *emitter, X64Instruction instruction) {
    if (emitter->async_storage)
        for (size_t i = 0; i < instruction.operand_count; i++) {
            X64Operand *operand = &instruction.operands[i];
            if (operand->kind == X64_OPERAND_MEMORY && operand->base != NULL &&
                !strcmp(operand->base, "rbp") && operand->displacement < 0)
                operand->base = "r14";
        }
    SourceMapWriter *map = emitter->source_map;
    if (map != NULL) {
        if (map->has_source)
            instruction = x64_instruction_with_source(instruction, map->current_ir_instruction,
                                                      map->current_span.begin.line, map->current_span.begin.column,
                                                      map->current_span.end.line, map->current_span.end.column);
        if (emitter->native) fprintf(map->output, "text-offset %zu ", emitter->native->sections[NATIVE_TEXT].size);
        if (fprintf(map->output, "instruction %zu function=", map->instruction_index) < 0 ||
            !map_quoted(map->output,
                        emitter->function == NULL
                            ? ""
                            : ast_program_lexeme(emitter->function->source_program, emitter->function->name_token)) ||
            fputs(" source=", map->output) == EOF ||
            !map_quoted(map->output,
                        map->current_program != NULL
                            ? map->current_program->source_path
                            : emitter->function == NULL
                                  ? ""
                                  : emitter->function->source_program->source_path)) {
            map->failed = 1;
        } else if (instruction.has_source) {
            if (fprintf(map->output, " ir=%zu span=%d:%d-%d:%d\n", instruction.ir_instruction,
                        instruction.source_begin_line, instruction.source_begin_column,
                        instruction.source_end_line, instruction.source_end_column) < 0)
                map->failed = 1;
        } else if (fputs(" ir=- span=-\n", map->output) == EOF) map->failed = 1;
        map->instruction_index++;
    }
    if (emitter->native) (void) native_encode(emitter->native, &instruction);
    else (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

void write_labelf(const Emitter *emitter, const char *format, ...) {
    char name[512];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(name, sizeof(name), format, args);
    va_end(args);
    if (count < 0 || (size_t) count >= sizeof(name)) {
        if (emitter->native) native_error(emitter->native, "Native label too long");
        return;
    }
    if (!emitter->native) {
        fputs(name, emitter->output);
        return;
    }
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    (void) native_define(emitter->native, name, 0, 0);
}

void write_x64_0(const Emitter *emitter, X64Opcode opcode) {
    write_x64(emitter, x64_instruction0(opcode, X64_WIDTH_NONE));
}

void write_x64_1(const Emitter *emitter, X64Opcode opcode, X64Width width,
                        X64Operand operand) {
    write_x64(emitter, x64_instruction1(opcode, width, operand));
}

void write_x64_2(const Emitter *emitter, X64Opcode opcode, X64Width width,
                        X64Operand destination, X64Operand source) {
    write_x64(emitter, x64_instruction2(opcode, width, destination, source));
}

static void write_register_move(const Emitter *emitter, const char *destination,
                                const char *source) {
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register(destination), x64_register(source));
}

static void write_stack_load(const Emitter *emitter, const char *destination,
                             size_t offset) {
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register(destination),
                x64_memory(X64_WIDTH_QWORD, "rsp", (long long) offset));
}

static void write_stack_store(const Emitter *emitter, size_t offset,
                              const char *source) {
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", (long long) offset),
                x64_register(source));
}

static void write_stack_address(const Emitter *emitter, const char *destination,
                                size_t offset) {
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register(destination),
                x64_memory(X64_WIDTH_NONE, "rsp", (long long) offset));
}

int is_integral(DataType type) {
    return data_type_integral(type);
}

int is_floating(DataType type) {
    return type == TYPE_FLOAT || type == TYPE_DOUBLE;
}

int is_numeric(DataType type) {
    return is_integral(type) || is_floating(type);
}

int is_pointer_value(const IrInstruction *instruction) {
    return instruction != NULL &&
           (instruction->pointer_depth != 0 || instruction->is_array);
}

int type_is_structure(const IrModule *module, IrTypeId type_id) {
    if (type_id < module->type_count &&
        module->types[type_id].kind == IR_TYPE_SLICE) return 1;
    if (type_id >= module->type_count || module->types[type_id].kind != IR_TYPE_NAMED)
        return 0;
    size_t symbol_id = module->types[type_id].symbol_id;
    if (symbol_id < module->semantics->symbol_count &&
        (module->semantics->symbols[symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
         module->semantics->symbols[symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE))
        return 1;
    for (size_t e = 0; e < module->enum_count; e++)
        if (module->enums[e].symbol_id == symbol_id) return module->enums[e].is_sum;
    return 0;
}

int is_inline_structure(const IrModule *module,
                               const IrInstruction *instruction) {
    return instruction != NULL && type_is_structure(module, instruction->type_id);
}

static DataType ir_ast_type(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->name_token >= program->token_count) return TYPE_UNKNOWN;
    switch (program->tokens[type->name_token].type) {
        case TOKEN_TYPE_I8:
        case TOKEN_TYPE_U8:
        case TOKEN_TYPE_I16:
        case TOKEN_TYPE_U16:
        case TOKEN_TYPE_I32:
        case TOKEN_TYPE_U32:
        case TOKEN_TYPE_I64:
        case TOKEN_TYPE_U64:
        case TOKEN_TYPE_ISIZE:
        case TOKEN_TYPE_USIZE:
            return token_data_type(program->tokens[type->name_token].type);
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
        case TOKEN_TYPE_FLOAT: return TYPE_FLOAT;
        case TOKEN_TYPE_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_TYPE_STRING: return TYPE_STRING;
        case TOKEN_TYPE_VOID: return TYPE_VOID;
        case TOKEN_TYPE_NEVER: return TYPE_NEVER;
        default: return TYPE_UNKNOWN;
    }
}

const IrInstruction *producer(const IrFunction *function, size_t value) {
    if (value == IR_VALUE_NONE) return NULL;
    for (size_t i = 0; i < function->instruction_count; i++)
        if (function->instructions[i].result == value) return &function->instructions[i];
    return NULL;
}

static const IrInstruction *local_declaration(const IrFunction *function,
                                              size_t symbol_id,
                                              size_t before) {
    const IrInstruction *result = NULL;
    for (size_t i = 0; i < before; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        if (instruction->opcode == IR_OP_DECLARE &&
            instruction->symbol_id == symbol_id)
            result = instruction;
    }
    return result;
}

static const IrParameter *function_parameter(const IrFunction *function,
                                             size_t symbol_id) {
    for (size_t i = 0; i < function->parameter_count; i++)
        if (function->parameters[i].symbol_id == symbol_id) return &function->parameters[i];
    return NULL;
}

const IrFunction *called_function(const IrModule *module, size_t symbol_id) {
    if (symbol_id == AST_SYMBOL_NONE) return NULL;
    for (size_t i = 0; i < module->function_count; i++)
        if (module->functions[i].symbol_id == symbol_id &&
            module->functions[i].interface_thunk_symbol_id == AST_SYMBOL_NONE)
            return &module->functions[i];
    return NULL;
}

const IrFunction *addressed_function(const IrModule *module, size_t symbol_id) {
    const IrFunction *function = called_function(module, symbol_id);
    if (function != NULL) return function;
    for (size_t i = 0; i < module->function_count; i++)
        if (module->functions[i].interface_thunk_symbol_id == symbol_id)
            return &module->functions[i];
    return NULL;
}

static size_t slice_parameter_count(const IrFunction *function) {
    size_t count = 0;
    for (size_t i = 0; i < function->parameter_count; i++)
        if (function->parameters[i].is_slice) count++;
    return count;
}

size_t parameter_storage_slots(const IrFunction *function) {
    return function->parameter_count + slice_parameter_count(function);
}

static size_t parameter_drop_slots(const IrModule *module,
                                   const IrFunction *function) {
    size_t count = 0;
    for (size_t i = 0; i < function->parameter_count; i++)
        if (!function->parameters[i].is_receiver &&
            (ir_type_properties(module, function->parameters[i].type_id) &
             SEMANTIC_TYPE_NEEDS_DROP))
            count++;
    return count;
}

static size_t parameter_flag_offset(const Emitter *emitter,
                                    const IrParameter *parameter) {
    size_t ordinal = 0;
    for (size_t i = 0; i < emitter->function->parameter_count; i++) {
        const IrParameter *candidate = &emitter->function->parameters[i];
        if (candidate == parameter)
            return (emitter->function->next_value +
                    parameter_storage_slots(emitter->function) + ordinal + 2U) * 8U;
        if (!candidate->is_receiver &&
            (ir_type_properties(emitter->module, candidate->type_id) &
             SEMANTIC_TYPE_NEEDS_DROP))
            ordinal++;
    }
    return 0;
}

static size_t slice_length_ordinal(const IrFunction *function, size_t parameter_index) {
    size_t ordinal = 0;
    for (size_t i = 0; i < parameter_index; i++)
        if (function->parameters[i].is_slice) ordinal++;
    return ordinal;
}

static size_t value_offset(size_t value) {
    return (value + 2U) * 8U;
}

size_t type_slots(const IrModule *module, IrTypeId type_id) {
    IrTypeLayout layout;
    return ir_type_layout(module, type_id, &layout) ? layout.storage_slots : 0;
}

static size_t declaration_offset(const Emitter *emitter,
                                 const IrInstruction *declaration);

static size_t declaration_slots(const Emitter *emitter,
                                const IrInstruction *declaration) {
    size_t slots = type_slots(emitter->module, declaration->type_id);
    if (ir_type_properties(emitter->module, declaration->type_id) &
        SEMANTIC_TYPE_NEEDS_DROP)
        slots++;
    if (declaration->is_slice) slots++;
    return slots;
}

static size_t declaration_slice_owner_offset(
    const Emitter *emitter, const IrInstruction *declaration) {
    size_t offset = declaration_offset(emitter, declaration) -
                    type_slots(emitter->module, declaration->type_id) * 8U;
    if (ir_type_properties(emitter->module, declaration->type_id) &
        SEMANTIC_TYPE_NEEDS_DROP)
        offset -= 8U;
    return offset;
}

static size_t declaration_flag_offset(const Emitter *emitter,
                                      const IrInstruction *declaration) {
    size_t offset = declaration_offset(emitter, declaration);
    size_t data_slots = type_slots(emitter->module, declaration->type_id);
    return offset - data_slots * 8U;
}

static size_t declaration_offset(const Emitter *emitter,
                                 const IrInstruction *declaration) {
    size_t slots_before = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *candidate = &emitter->function->instructions[i];
        if (candidate->opcode != IR_OP_DECLARE) continue;
        if (candidate == declaration)
            return (emitter->function->next_value + parameter_storage_slots(emitter->function) +
                    parameter_drop_slots(emitter->module, emitter->function) +
                    slots_before + declaration_slots(emitter, candidate) + 1U) * 8U;
        slots_before += declaration_slots(emitter, candidate);
    }
    return 0;
}

static size_t parameter_offset(const Emitter *emitter,
                               const IrParameter *parameter) {
    for (size_t i = 0; i < emitter->function->parameter_count; i++)
        if (&emitter->function->parameters[i] == parameter)
            return (emitter->function->next_value + i + 2U) * 8U;
    return 0;
}

static void write_escaped(FILE *output, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char) text[i];
        switch (c) {
            case '\n': fputs("\\n", output);
                break;
            case '\r': fputs("\\r", output);
                break;
            case '\t': fputs("\\t", output);
                break;
            case '\\': fputs("\\\\", output);
                break;
            case '"': fputs("\\\"", output);
                break;
            default:
                if (c >= 32U && c < 127U) fputc((int) c, output);
                else fprintf(output, "\\%03o", (unsigned) c);
                break;
        }
    }
}

void write_cstring(const Emitter *emitter, const char *text) {
    if (emitter->native) (void) native_bytes(emitter->native, text, strlen(text) + 1);
    else {
        fputs("    .ascii \"", emitter->output);
        write_escaped(emitter->output, text);
        fputs("\\0\"\n", emitter->output);
    }
}

void write_quad(const Emitter *emitter, uint64_t value) {
    if (emitter->native) (void) native_uint(emitter->native, value, 8);
    else fprintf(emitter->output, "    .quad 0x%016llx\n", (unsigned long long) value);
}

void write_value_load(const Emitter *emitter, const char *reg, size_t value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_register(reg), x64_memory(X64_WIDTH_QWORD, "rbp",
                                                      -(long long) value_offset(value)));
    write_x64(emitter, instruction);
}

static size_t slice_length_offset(const Emitter *emitter, size_t parameter_index) {
    return (emitter->function->next_value + emitter->function->parameter_count +
            slice_length_ordinal(emitter->function, parameter_index) + 2U) * 8U;
}

void write_value_store(const Emitter *emitter, const char *reg, size_t value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) value_offset(value)),
                                                  x64_register(reg));
    write_x64(emitter, instruction);
}

static void write_local_load(const Emitter *emitter, const char *reg, size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_register(reg),
                                                  x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset));
    write_x64(emitter, instruction);
}

static void write_local_store(const Emitter *emitter, const char *reg, size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset),
                                                  x64_register(reg));
    write_x64(emitter, instruction);
}

void write_immediate(const Emitter *emitter, const char *reg, long long value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_register(reg), x64_immediate(value));
    write_x64(emitter, instruction);
}

static void write_address(const Emitter *emitter, const char *reg, const char *label,
                          size_t suffix) {
    X64Instruction instruction = x64_instruction2(X64_OP_LEA, X64_WIDTH_QWORD,
                                                  x64_register(reg), x64_rip_memory(X64_WIDTH_NONE, label, suffix));
    write_x64(emitter, instruction);
}

void write_call(const Emitter *emitter, const char *name) {
    char core_symbol[128];
    if (strcmp(name, "strlen") == 0 || strcmp(name, "strcmp") == 0 ||
        strcmp(name, "strcpy") == 0 || strcmp(name, "strcat") == 0 ||
        strcmp(name, "calloc") == 0 || strcmp(name, "free") == 0 || strcmp(name, "snprintf") == 0) {
        snprintf(core_symbol, sizeof(core_symbol), "__dmm_core_%s", name);
        name = core_symbol;
    }

    if (emitter->target == TARGET_COFF) {
        X64Instruction stack = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
                                                x64_register("rsp"), x64_immediate(32));
        write_x64(emitter, stack);
    }
    X64Instruction call = x64_instruction1(X64_OP_CALL, X64_WIDTH_NONE,
                                           x64_label(name));
    write_x64(emitter, call);
    if (emitter->target == TARGET_COFF) {
        X64Instruction stack = x64_instruction2(X64_OP_ADD, X64_WIDTH_QWORD,
                                                x64_register("rsp"), x64_immediate(32));
        write_x64(emitter, stack);
    }
}

const char *argument_register(TargetFormat target, size_t index) {
    static const char *sysv[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const char *coff[] = {"rcx", "rdx", "r8", "r9"};
    return target == TARGET_COFF ? coff[index] : sysv[index];
}

size_t physical_parameter_count(const IrFunction *function) {
    return function->parameter_count + slice_parameter_count(function);
}

int physical_parameter(const IrFunction *function, size_t physical_index,
                              size_t *source_index, int *is_length) {
    size_t physical = 0;
    for (size_t source = 0; source < function->parameter_count; source++) {
        if (physical == physical_index) {
            *source_index = source;
            *is_length = 0;
            return 1;
        }
        physical++;
        if (function->parameters[source].is_slice) {
            if (physical == physical_index) {
                *source_index = source;
                *is_length = 1;
                return 1;
            }
            physical++;
        }
    }
    return 0;
}

int physical_is_floating(const IrFunction *function, size_t physical_index) {
    size_t source = 0;
    int is_length = 0;
    return physical_parameter(function, physical_index, &source, &is_length) &&
           !is_length && function->parameters[source].pointer_depth == 0 &&
           !function->parameters[source].is_array && !function->parameters[source].is_slice &&
           is_floating(function->parameters[source].type);
}

size_t parameter_register_index(const IrFunction *function, TargetFormat target,
                                       size_t physical_index) {
    if (target == TARGET_COFF)
        return physical_index < 4 ? physical_index : IR_VALUE_NONE;
    size_t class_index = 0;
    int floating = physical_is_floating(function, physical_index);
    for (size_t i = 0; i < physical_index; i++)
        if (physical_is_floating(function, i) == floating) class_index++;
    size_t limit = floating ? 8U : 6U;
    return class_index < limit ? class_index : IR_VALUE_NONE;
}

static size_t stack_parameter_index(const IrFunction *function, TargetFormat target,
                                    size_t physical_index) {
    size_t stack_index = 0;
    for (size_t i = 0; i < physical_index; i++)
        if (parameter_register_index(function, target, i) == IR_VALUE_NONE) stack_index++;
    return stack_index;
}

size_t stack_parameter_count(const IrFunction *function, TargetFormat target) {
    size_t count = 0;
    for (size_t i = 0; i < physical_parameter_count(function); i++)
        if (parameter_register_index(function, target, i) == IR_VALUE_NONE) count++;
    return count;
}

static void write_positive_frame_load(const Emitter *emitter, const char *reg,
                                      size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_register(reg),
                                                  x64_memory(X64_WIDTH_QWORD, "rbp", (long long) offset));
    write_x64(emitter, instruction);
}

void normalize_integral_parameter(const Emitter *emitter, DataType type) {
    if (data_type_fixed_integer(type)) {
        unsigned bytes = data_type_bytes(type);
        if (bytes == 8) return;
        X64Width source = bytes == 1 ? X64_WIDTH_BYTE : bytes == 2 ? X64_WIDTH_WORD : X64_WIDTH_DWORD;
        if (bytes == 4 && data_type_unsigned(type))
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_DWORD, x64_register("eax"), x64_register("eax"));
        else
            write_x64_2(emitter, data_type_unsigned(type) ? X64_OP_MOVZX : X64_OP_MOVSX,
                        X64_WIDTH_QWORD, x64_sized_register(X64_WIDTH_QWORD, "rax"),
                        x64_sized_register(source, bytes == 1 ? "al" : bytes == 2 ? "ax" : "eax"));
    } else if (type == TYPE_INT) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVSX, X64_WIDTH_QWORD,
                                                      x64_sized_register(X64_WIDTH_QWORD, "rax"),
                                                      x64_sized_register(X64_WIDTH_DWORD, "eax"));
        write_x64(emitter, instruction);
    } else if (type == TYPE_CHAR) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVSX, X64_WIDTH_QWORD,
                                                      x64_sized_register(X64_WIDTH_QWORD, "rax"),
                                                      x64_sized_register(X64_WIDTH_BYTE, "al"));
        write_x64(emitter, instruction);
    } else if (type == TYPE_BYTE) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVZX, X64_WIDTH_DWORD,
                                                      x64_sized_register(X64_WIDTH_DWORD, "eax"),
                                                      x64_sized_register(X64_WIDTH_BYTE, "al"));
        write_x64(emitter, instruction);
    } else if (type == TYPE_BIT) {
        X64Instruction instruction = x64_instruction2(X64_OP_AND, X64_WIDTH_DWORD,
                                                      x64_sized_register(X64_WIDTH_DWORD, "eax"), x64_immediate(1));
        write_x64(emitter, instruction);
    }
}

long long constant_value(const AstProgram *program,
                                const IrInstruction *instruction) {
    const AstToken *token = ast_program_token(program, instruction->auxiliary_token);
    if (token == NULL) return 0;
    if (token->type == TOKEN_CHAR_LITERAL) return (unsigned char) token->lexeme[0];
    if (strcmp(token->lexeme, "true") == 0) return 1;
    if (strcmp(token->lexeme, "false") == 0) return 0;
    return (long long) strtoull(token->lexeme, NULL, 10);
}

void load_nullable_string(Emitter *emitter, const char *reg, size_t value);

int emit_string_compare(Emitter *emitter, const IrInstruction *instruction) {
    const char *left_argument = emitter->target == TARGET_COFF ? "rcx" : "rdi";
    const char *right_argument = emitter->target == TARGET_COFF ? "rdx" : "rsi";
    int equal_result = instruction->operator_type == TOKEN_EQUAL_EQUAL;
    write_value_load(emitter, "rax", instruction->operand_a);
    write_value_load(emitter, "rcx", instruction->operand_b);
    char equal_label[64];
    char done_label[64];
    (void) snprintf(equal_label, sizeof(equal_label), ".LIR_string_equal_%zu_%zu",
                    emitter->function_index, instruction->result);
    (void) snprintf(done_label, sizeof(done_label), ".LIR_string_done_%zu_%zu",
                    emitter->function_index, instruction->result);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                x64_register("rax"), x64_register("rcx"));
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(equal_label));
    load_nullable_string(emitter, left_argument, instruction->operand_a);
    load_nullable_string(emitter, right_argument, instruction->operand_b);
    write_call(emitter, "strcmp");
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_DWORD,
                x64_register("eax"), x64_immediate(0));
    write_x64_1(emitter, equal_result ? X64_OP_SETE : X64_OP_SETNE,
                X64_WIDTH_NONE, x64_register("al"));
    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                x64_sized_register(X64_WIDTH_BYTE, "al"));
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(done_label));
    write_labelf(emitter, ".LIR_string_equal_%zu_%zu:\n",
                 emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 1 : 0);
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(done_label));
    write_labelf(emitter, ".LIR_string_unequal_%zu_%zu:\n",
                 emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 0 : 1);
    write_labelf(emitter, ".LIR_string_done_%zu_%zu:\n",
                 emitter->function_index, instruction->result);
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

void convert_rax(Emitter *emitter, DataType from, DataType to);

size_t aggregate_result_offset(const Emitter *emitter,
                                      const IrInstruction *result);

void copy_aggregate(Emitter *emitter, size_t slots,
                           const char *source, const char *destination);

static void emit_concat_operand(Emitter *emitter, const IrInstruction *value,
                                size_t value_id, size_t buffer_offset,
                                unsigned operand_index, size_t result_id) {
    if (value->type == TYPE_STRING ||
        (value->type == TYPE_CHAR && is_pointer_value(value))) {
        write_value_load(emitter, "rax", value_id);
        char nonnull[80];
        (void) snprintf(nonnull, sizeof(nonnull), ".LIR_concat_value_%zu_%zu_%u",
                        emitter->function_index, result_id, operand_index);
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(nonnull));
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rax"),
                    x64_rip_memory(X64_WIDTH_NONE, ".LIR_empty_string_", 0));
        write_labelf(emitter, ".LIR_concat_value_%zu_%zu_%u:\n",
                     emitter->function_index, result_id, operand_index);
        return;
    }

    const char *destination = emitter->target == TARGET_COFF ? "rcx" : "rdi";
    const char *capacity = emitter->target == TARGET_COFF ? "rdx" : "rsi";
    const char *format = emitter->target == TARGET_COFF ? "r8" : "rdx";
    write_stack_address(emitter, destination, buffer_offset);
    write_immediate(emitter, capacity, 64);
    write_address(emitter, format,
                  value->type == TYPE_CHAR
                      ? ".LIR_char_format_"
                      : is_floating(value->type)
                            ? ".LIR_float_format_"
                            : data_type_fixed_integer(value->type) && data_type_unsigned(value->type)
                                  ? ".LIR_uint_format_"
                                  : ".LIR_int_format_", 0);
    write_value_load(emitter, "rax", value_id);
    if (is_floating(value->type)) {
        convert_rax(emitter, value->type, TYPE_DOUBLE);
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register(emitter->target == TARGET_COFF ? "xmm3" : "xmm0"),
                    x64_register("rax"));
        if (emitter->target == TARGET_COFF)
            write_register_move(emitter, "r9", "rax");
        else write_immediate(emitter, "rax", 1);
    } else {
        const char *argument = emitter->target == TARGET_COFF ? "r9" : "rcx";
        write_register_move(emitter, argument, "rax");
        if (emitter->target == TARGET_ELF) write_immediate(emitter, "rax", 0);
    }
    write_call(emitter, "snprintf");
    write_stack_address(emitter, "rax", buffer_offset);
}

int emit_string_concat(Emitter *emitter, const IrInstruction *instruction) {
    const IrInstruction *left = producer(emitter->function, instruction->operand_a);
    const IrInstruction *right = producer(emitter->function, instruction->operand_b);
    if (left == NULL || right == NULL) return 0;
    const char *first = emitter->target == TARGET_COFF ? "rcx" : "rdi";
    const char *second = emitter->target == TARGET_COFF ? "rdx" : "rsi";
    write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_immediate(160));
    emit_concat_operand(emitter, left, instruction->operand_a, 0, 0,
                        instruction->result);
    write_stack_store(emitter, 128, "rax");
    emit_concat_operand(emitter, right, instruction->operand_b, 64, 1,
                        instruction->result);
    write_stack_store(emitter, 136, "rax");
    write_stack_load(emitter, first, 128);
    write_call(emitter, "strlen");
    write_stack_store(emitter, 144, "rax");
    write_stack_load(emitter, first, 136);
    write_call(emitter, "strlen");
    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD, x64_register("rax"),
                x64_memory(X64_WIDTH_QWORD, "rsp", 144));
    write_x64_1(emitter, X64_OP_INC, X64_WIDTH_QWORD, x64_register("rax"));
    write_immediate(emitter, first, 1);
    write_register_move(emitter, second, "rax");
    write_call(emitter, "calloc");
    write_stack_store(emitter, 152, "rax");
    write_register_move(emitter, first, "rax");
    write_stack_load(emitter, second, 128);
    write_call(emitter, "strcpy");
    write_stack_load(emitter, first, 152);
    write_stack_load(emitter, second, 136);
    write_call(emitter, "strcat");
    /* A concatenation result is an owned temporary.  When it is consumed by a
       larger concatenation, the new buffer already contains its bytes and the
       intermediate buffer can be released immediately. */
    if (left->opcode == IR_OP_BINARY && left->operator_type == TOKEN_PLUS &&
        left->type == TYPE_STRING) {
        write_stack_load(emitter, first, 128);
        write_call(emitter, "free");
    }
    if (right->opcode == IR_OP_BINARY && right->operator_type == TOKEN_PLUS &&
        right->type == TYPE_STRING) {
        write_stack_load(emitter, first, 136);
        write_call(emitter, "free");
    }
    write_stack_load(emitter, "rax", 152);
    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_immediate(160));
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static size_t fixed_array_length(const Emitter *emitter,
                                 const IrInstruction *base) {
    if (base == NULL) return 0;
    if (base->type_id < emitter->module->type_count &&
        emitter->module->types[base->type_id].kind == IR_TYPE_ARRAY)
        return emitter->module->types[base->type_id].array_length;
    if (base->opcode == IR_OP_LOAD) {
        const IrInstruction *declaration = local_declaration(emitter->function,
                                                             base->symbol_id, emitter->function->instruction_count);
        if (declaration != NULL && declaration->type_id < emitter->module->type_count) {
            const IrType *type = &emitter->module->types[declaration->type_id];
            if (type->kind == IR_TYPE_ARRAY) return type->array_length;
        }
    }
    if (base->opcode == IR_OP_MEMBER &&
        base->symbol_id < emitter->module->semantics->symbol_count) {
        const SemanticSymbol *symbol = &emitter->module->semantics->symbols[base->symbol_id];
        const AstField *field = symbol->kind == SEMANTIC_SYMBOL_FIELD
                                    ? (const AstField *) symbol->node
                                    : NULL;
        if (field != NULL && field->type.is_array &&
            field->type.array_length_token < symbol->source_program->token_count) {
            if (field->type.resolved_array_length != 0) return field->type.resolved_array_length;
            return (size_t) strtoull(ast_program_lexeme(symbol->source_program,
                                                        field->type.array_length_token), NULL, 10);
        }
    }
    return 0;
}

size_t aggregate_result_offset(const Emitter *emitter,
                                      const IrInstruction *result) {
    size_t slots_before = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *candidate = &emitter->function->instructions[i];
        if ((candidate->opcode != IR_OP_AWAIT && candidate->opcode != IR_OP_CALL && candidate->opcode != IR_OP_ENUM_CONSTRUCT && candidate->opcode !=
             IR_OP_SLICE && candidate->opcode != IR_OP_SUBSLICE &&
             candidate->opcode != IR_OP_ARRAY_LITERAL &&
             candidate->opcode != IR_OP_INTERFACE_PACK) ||
            (!is_inline_structure(emitter->module, candidate) &&
             !candidate->is_array))
            continue;
        size_t slots = type_slots(emitter->module, candidate->type_id);
        if (candidate == result)
            return (emitter->function->next_value +
                    parameter_storage_slots(emitter->function) +
                    parameter_drop_slots(emitter->module, emitter->function) +
                    emitter->declaration_count + slots_before + slots + 1U) * 8U;
        slots_before += slots;
    }
    return 0;
}

static size_t aggregate_result_slots(const Emitter *emitter) {
    size_t result = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *instruction = &emitter->function->instructions[i];
        if ((instruction->opcode == IR_OP_AWAIT || instruction->opcode == IR_OP_CALL || instruction->opcode == IR_OP_ENUM_CONSTRUCT || instruction->opcode ==
             IR_OP_SLICE || instruction->opcode == IR_OP_SUBSLICE ||
             instruction->opcode == IR_OP_ARRAY_LITERAL ||
             instruction->opcode == IR_OP_INTERFACE_PACK) &&
            (is_inline_structure(emitter->module, instruction) ||
             instruction->is_array))
            result += type_slots(emitter->module, instruction->type_id);
    }
    return result;
}

static size_t parameter_copy_offset(const Emitter *emitter, size_t parameter_index) {
    size_t slots_before = 0;
    for (size_t i = 0; i < parameter_index; i++)
        if (type_is_structure(emitter->module,
                              emitter->function->parameters[i].type_id) ||
            (emitter->function->is_async && emitter->module->types[
                emitter->function->parameters[i].type_id].kind == IR_TYPE_ARRAY))
            slots_before += type_slots(emitter->module,
                                       emitter->function->parameters[i].type_id);
    size_t slots = type_slots(emitter->module,
                              emitter->function->parameters[parameter_index].type_id);
    return (emitter->function->next_value + parameter_storage_slots(emitter->function) +
            parameter_drop_slots(emitter->module, emitter->function) +
            emitter->declaration_count + aggregate_result_slots(emitter) +
            slots_before + slots + 1U) * 8U;
}

void copy_aggregate(Emitter *emitter, size_t slots,
                           const char *source, const char *destination) {
    for (size_t slot = 0; slot < slots; slot++) {
        size_t offset = slot * 8U;
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rcx"),
                    x64_memory(X64_WIDTH_QWORD, source, (long long) offset));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, destination, (long long) offset),
                    x64_register("rcx"));
    }
}

static const IrAggregate *aggregate_for_symbol(const IrModule *module,
                                               size_t symbol_id) {
    for (size_t i = 0; i < module->structure_count; i++)
        if (module->structures[i].symbol_id == symbol_id) return &module->structures[i];
    return NULL;
}

static const IrEnum *enum_for_symbol(const IrModule *module, size_t symbol_id,
                                     size_t *enum_index) {
    for (size_t i = 0; i < module->enum_count; i++) {
        if (module->enums[i].symbol_id == symbol_id) {
            if (enum_index != NULL) *enum_index = i;
            return &module->enums[i];
        }
    }
    return NULL;
}

static int enum_variant_location(const IrModule *module, size_t variant_symbol_id,
                                 const IrEnum **enumeration, size_t *enum_index,
                                 size_t *variant_index) {
    for (size_t e = 0; e < module->enum_count; e++) {
        for (size_t v = 0; v < module->enums[e].variant_count; v++) {
            if (module->enums[e].variants[v].symbol_id == variant_symbol_id) {
                *enumeration = &module->enums[e];
                *enum_index = e;
                *variant_index = v;
                return 1;
            }
        }
    }
    return 0;
}

static size_t enum_field_index(const IrEnum *enumeration, size_t field_symbol_id) {
    for (size_t i = 0; i < enumeration->field_count; i++)
        if (enumeration->fields[i].symbol_id == field_symbol_id) return i;
    return IR_VALUE_NONE;
}

static size_t aggregate_field_offset(const Emitter *emitter,
                                     const IrAggregate *aggregate,
                                     size_t field_symbol_id) {
    size_t slots = 0;
    for (size_t i = 0; i < aggregate->field_count; i++) {
        if (aggregate->fields[i].symbol_id == field_symbol_id) return slots * 8U;
        slots += type_slots(emitter->module, aggregate->fields[i].type_id);
    }
    return SIZE_MAX;
}

static int emit_drop_type(Emitter *emitter, IrTypeId type_id) {
    if (type_id >= emitter->module->type_count) return 0;
    const IrType *type = &emitter->module->types[type_id];
    if (type->kind == IR_TYPE_FUTURE) {
        write_x64_0(emitter, X64_OP_UD2);
        return 1;
    }
    if (type->kind == IR_TYPE_ARRAY) {
        IrTypeLayout element;
        if (!ir_type_layout(emitter->module, type->element_type, &element))
            return 0;
        /* Keep the array base in an aligned, call-safe stack slot while
           recursively destroying elements in reverse order. */
        write_register_move(emitter, "rbx", "rax");
        write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD,
                    x64_register("rbx"));
        write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD,
                    x64_register("rbx"));
        for (size_t i = type->array_length; i > 0; i--) {
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rbx"),
                        x64_memory(X64_WIDTH_QWORD, "rsp", 0));
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                        x64_register("rax"),
                        x64_memory(X64_WIDTH_NONE, "rbx",
                                   (long long) ((i - 1) * element.size)));
            if (!emit_drop_type(emitter, type->element_type)) return 0;
        }
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(16));
        return 1;
    }
    if (type->kind != IR_TYPE_NAMED) return 1;
    if (type->symbol_id < emitter->module->semantics->symbol_count &&
        emitter->module->semantics->symbols[type->symbol_id].kind ==
            SEMANTIC_SYMBOL_INTERFACE) {
        size_t sequence = emitter->bounds_sequence++;
        char done[96], release[96], next[96];
        snprintf(done, sizeof(done), ".LIR_interface_drop_done_%zu_%zu",
                 emitter->function_index, sequence);
        snprintf(release, sizeof(release), ".LIR_interface_drop_release_%zu_%zu",
                 emitter->function_index, sequence);
        write_register_move(emitter, "rbx", "rax");
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rbx", 8));
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(done));
        for (size_t s = 0; s < emitter->module->structure_count; s++) {
            const IrAggregate *structure = &emitter->module->structures[s];
            if (!semantic_implements_interface(emitter->module->semantics,
                                               type->symbol_id,
                                               structure->symbol_id)) continue;
            snprintf(next, sizeof(next), ".LIR_interface_drop_next_%zu_%zu_%zu",
                     emitter->function_index, sequence, s);
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rbx", 0));
            write_immediate(emitter, "rdx",
                            (long long) ir_interface_type_tag(emitter->module,
                                                                structure->symbol_id));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rdx"));
            write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(next));
            if (structure->type_properties & SEMANTIC_TYPE_NEEDS_DROP) {
                const IrFunction *glue = called_function(emitter->module,
                                                         structure->symbol_id);
                char buffer[4096];
                if (glue == NULL || !glue->is_drop_glue) return 0;
                const char *name = function_link_name(emitter->module, glue,
                                                       buffer, sizeof(buffer));
                if (name == NULL) return 0;
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register(emitter->target == TARGET_COFF ? "rcx" : "rdi"),
                            x64_memory(X64_WIDTH_QWORD, "rbx", 8));
                write_call(emitter, name);
            }
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(release));
            write_labelf(emitter, "%s:\n", next);
        }
        write_x64_0(emitter, X64_OP_UD2);
        write_labelf(emitter, "%s:\n", release);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register(emitter->target == TARGET_COFF ? "rcx" : "rdi"),
                    x64_memory(X64_WIDTH_QWORD, "rbx", 8));
        write_call(emitter, "free");
        write_immediate(emitter, "rax", 0);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "rbx", 0), x64_register("rax"));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_memory(X64_WIDTH_QWORD, "rbx", 8), x64_register("rax"));
        write_labelf(emitter, "%s:\n", done);
        return 1;
    }
    const IrFunction *glue = called_function(emitter->module,
                                             type->symbol_id);
    if (glue == NULL || !glue->is_drop_glue) return 0;
    char buffer[4096];
    const char *name = function_link_name(emitter->module, glue, buffer,
                                          sizeof(buffer));
    if (name == NULL) return 0;
    write_register_move(emitter,
                        emitter->target == TARGET_COFF ? "rcx" : "rdi",
                        "rax");
    write_call(emitter, name);
    return 1;
}

static int emit_drop_owned_slice_elements(Emitter *emitter, IrTypeId slice_type) {
    if (slice_type >= emitter->module->type_count ||
        emitter->module->types[slice_type].kind != IR_TYPE_SLICE) return 0;
    IrTypeId element_type = emitter->module->types[slice_type].element_type;
    if ((ir_type_properties(emitter->module, element_type) &
         SEMANTIC_TYPE_NEEDS_DROP) == 0) return 1;
    IrTypeLayout element_layout;
    if (!ir_type_layout(emitter->module, element_type, &element_layout)) return 0;
    size_t sequence = emitter->bounds_sequence++;
    char loop[96], done[96];
    snprintf(loop, sizeof(loop), ".LIR_slice_drop_loop_%zu_%zu",
             emitter->function_index, sequence);
    snprintf(done, sizeof(done), ".LIR_slice_drop_done_%zu_%zu",
             emitter->function_index, sequence);
    write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_immediate(32));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", 0), x64_register("rax"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", 8), x64_register("rdx"));
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                x64_register("rax"), x64_register("rax"));
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(done));
    write_labelf(emitter, "%s:\n", loop);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register("rcx"), x64_memory(X64_WIDTH_QWORD, "rsp", 8));
    write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                x64_register("rcx"), x64_register("rcx"));
    write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(done));
    write_x64_1(emitter, X64_OP_DEC, X64_WIDTH_QWORD, x64_register("rcx"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_memory(X64_WIDTH_QWORD, "rsp", 8), x64_register("rcx"));
    write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                x64_register("rcx"), x64_immediate((long long) element_layout.size));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rsp", 0));
    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                x64_register("rax"), x64_register("rcx"));
    if (!emit_drop_type(emitter, element_type)) return 0;
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(loop));
    write_labelf(emitter, "%s:\n", done);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register("rax"), x64_memory(X64_WIDTH_QWORD, "rsp", 0));
    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_immediate(32));
    return 1;
}

static int emit_ownership_effect(Emitter *emitter,
                                 const IrInstruction *instruction,
                                 size_t index) {
    const SemanticSymbol *symbol =
        instruction->symbol_id < emitter->module->semantics->symbol_count
            ? &emitter->module->semantics->symbols[instruction->symbol_id]
            : NULL;
    const IrInstruction *declaration = local_declaration(
        emitter->function, instruction->symbol_id, index);
    const IrParameter *parameter = function_parameter(
        emitter->function, instruction->symbol_id);
    const IrGlobal *global = NULL;
    for (size_t g = 0; g < emitter->module->global_count; g++)
        if (emitter->module->globals[g].symbol_id == instruction->symbol_id) {
            global = &emitter->module->globals[g];
            break;
        }
    if (instruction->opcode == IR_OP_MOVE ||
        instruction->opcode == IR_OP_REINIT) {
        if (declaration == NULL && parameter == NULL && global == NULL) return 0;
        write_immediate(emitter, "rax",
                        instruction->opcode == IR_OP_REINIT ? 1 : 0);
        if (global != NULL) {
            char flag[4096];
            if (!global_drop_flag_label(symbol, flag, sizeof(flag))) return 0;
            X64Operand flag_memory =
                x64_rip_memory(X64_WIDTH_QWORD, flag, 0);
            flag_memory.has_symbol_suffix = 0;
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        flag_memory,
                        x64_register("rax"));
        } else
            write_local_store(emitter, "rax",
                              declaration != NULL
                                  ? declaration_flag_offset(emitter, declaration)
                                  : parameter_flag_offset(emitter, parameter));
        return 1;
    }
    if (instruction->opcode != IR_OP_DROP) return 0;
    if (instruction->operand_a != IR_VALUE_NONE) {
        write_value_load(emitter, "rax", instruction->operand_a);
        return emit_drop_type(emitter, instruction->type_id);
    }
    if (declaration != NULL) {
        char skip[96];
        snprintf(skip, sizeof(skip), ".LIR_drop_skip_%zu_%zu",
                 emitter->function_index, index);
        size_t flag = declaration_flag_offset(emitter, declaration);
        write_local_load(emitter, "rax", flag);
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_immediate(0));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(skip));
        write_immediate(emitter, "rax", 0);
        write_local_store(emitter, "rax", flag);
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                    x64_register("rax"),
                    x64_memory(X64_WIDTH_NONE, "rbp",
                               -(long long) declaration_offset(emitter,
                                                               declaration)));
        if (!emit_drop_type(emitter, instruction->type_id)) return 0;
        write_labelf(emitter, "%s:\n", skip);
        return 1;
    }
    if (parameter != NULL && !parameter->is_receiver) {
        char skip[96];
        snprintf(skip, sizeof(skip), ".LIR_drop_skip_%zu_%zu",
                 emitter->function_index, index);
        size_t flag = parameter_flag_offset(emitter, parameter);
        write_local_load(emitter, "rax", flag);
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_immediate(0));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(skip));
        write_immediate(emitter, "rax", 0);
        write_local_store(emitter, "rax", flag);
        write_local_load(emitter, "rax", parameter_offset(emitter, parameter));
        if (!emit_drop_type(emitter, instruction->type_id)) return 0;
        write_labelf(emitter, "%s:\n", skip);
        return 1;
    }
    if (global != NULL) {
        char flag[4096], label[4096], skip[96];
        if (!global_drop_flag_label(symbol, flag, sizeof(flag)) ||
            !global_label(symbol, label, sizeof(label)))
            return 0;
        snprintf(skip, sizeof(skip), ".LIR_drop_skip_%zu_%zu",
                 emitter->function_index, index);
        X64Operand flag_memory =
            x64_rip_memory(X64_WIDTH_QWORD, flag, 0);
        flag_memory.has_symbol_suffix = 0;
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register("rax"),
                    flag_memory);
        write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_immediate(0));
        write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(skip));
        write_immediate(emitter, "rax", 0);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    flag_memory,
                    x64_register("rax"));
        X64Operand global_memory =
            x64_rip_memory(X64_WIDTH_NONE, label, 0);
        global_memory.has_symbol_suffix = 0;
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                    x64_register("rax"), global_memory);
        if (!emit_drop_type(emitter, instruction->type_id)) return 0;
        write_labelf(emitter, "%s:\n", skip);
        return 1;
    }
    if (symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_FIELD &&
        emitter->function->is_drop_glue) {
        const IrParameter *receiver = function_receiver(emitter->function);
        const IrAggregate *aggregate = aggregate_for_symbol(
            emitter->module, emitter->function->owner_symbol_id);
        size_t offset = aggregate == NULL
                            ? SIZE_MAX
                            : aggregate_field_offset(emitter, aggregate,
                                                     instruction->symbol_id);
        if (receiver == NULL || offset == SIZE_MAX) return 0;
        write_local_load(emitter, "rax", parameter_offset(emitter, receiver));
        if (offset != 0)
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                        x64_register("rax"),
                        x64_immediate((long long) offset));
        return emit_drop_type(emitter, instruction->type_id);
    }
    return 0;
}

int global_label(const SemanticSymbol *symbol, char *label, size_t size) {
    size_t used = 0;
    label[0] = '\0';
    if (!mangle_append(label, size, &used, "__dmm_g")) return 0;
    const char *identity = symbol->source_program->module_identity;
    for (const unsigned char *p = (const unsigned char *) identity; *p; p++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", *p);
        if (!mangle_append(label, size, &used, hex)) return 0;
    }
    return mangle_append(label, size, &used, "_") && mangle_append(label, size, &used,
                                                                   ast_program_lexeme(
                                                                       symbol->source_program, symbol->name_token));
}

int global_drop_flag_label(const SemanticSymbol *symbol, char *label,
                           size_t size) {
    size_t used = 0;
    char global[4096];
    if (!global_label(symbol, global, sizeof(global)) ||
        !mangle_append(label, size, &used, global))
        return 0;
    return mangle_append(label, size, &used, "__drop_flag");
}

int global_slice_owner_label(const SemanticSymbol *symbol, char *label,
                             size_t size) {
    size_t used = 0;
    char global[4096];
    if (!global_label(symbol, global, sizeof(global)) ||
        !mangle_append(label, size, &used, global))
        return 0;
    return mangle_append(label, size, &used, "__slice_owner");
}

static int emit_lvalue_address(Emitter *emitter, const IrInstruction *target,
                               size_t instruction_index) {
    if (target == NULL) return 0;
    if (target->opcode == IR_OP_LOAD) {
        for (size_t g = 0; g < emitter->module->global_count; g++)
            if (emitter->module->globals[g].symbol_id == target->symbol_id) {
                char label[4096];
                if (!global_label(&emitter->module->semantics->symbols[target->symbol_id], label, sizeof(label)))
                    return
                            0;
                X64Operand address = x64_rip_memory(X64_WIDTH_NONE, label, 0);
                address.has_symbol_suffix = 0;
                write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"), address);
                return 1;
            }
        const IrInstruction *declaration = local_declaration(emitter->function,
                                                             target->symbol_id, instruction_index);
        const IrParameter *parameter = function_parameter(emitter->function,
                                                          target->symbol_id);
        if (declaration != NULL) {
            size_t offset = declaration_offset(emitter, declaration);
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            return 1;
        }
        if (parameter != NULL && !parameter->is_array) {
            size_t offset = parameter_offset(emitter, parameter);
            if (is_inline_structure(emitter->module, target)) {
                write_local_load(emitter, "rbx", offset);
                return 1;
            }
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            return 1;
        }
        const IrParameter *receiver = function_receiver(emitter->function);
        if (receiver != NULL && target->symbol_id < emitter->module->semantics->symbol_count) {
            const SemanticSymbol *field =
                    &emitter->module->semantics->symbols[target->symbol_id];
            const IrAggregate *aggregate = aggregate_for_symbol(emitter->module,
                                                                field->owner_symbol_id);
            size_t field_offset = aggregate == NULL
                                      ? SIZE_MAX
                                      : aggregate_field_offset(emitter, aggregate, target->symbol_id);
            if (field->kind == SEMANTIC_SYMBOL_FIELD && field_offset != SIZE_MAX) {
                write_local_load(emitter, "rbx", parameter_offset(emitter, receiver));
                if (field_offset != 0)
                    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                                x64_register("rbx"),
                                x64_immediate((long long) field_offset));
                return 1;
            }
        }
        return 0;
    }
    if (target->opcode == IR_OP_UNARY && target->operator_type == TOKEN_STAR) {
        write_value_load(emitter, "rbx", target->operand_b);
        return 1;
    }
    if (target->opcode == IR_OP_INDEX) {
        const IrInstruction *base = producer(emitter->function, target->operand_a);
        write_value_load(emitter, "rax", target->operand_a);
        write_value_load(emitter, "rcx", target->operand_b);
        size_t length = fixed_array_length(emitter, base);
        int dynamic_length = 0;
        if (base != NULL && base->type_id < emitter->module->type_count &&
            emitter->module->types[base->type_id].kind == IR_TYPE_SLICE) {
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rdx"),
                        x64_memory(X64_WIDTH_QWORD, "rax", 8));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                        x64_memory(X64_WIDTH_QWORD, "rax", 0));
            dynamic_length = 1;
        }
        size_t bounds_id = emitter->bounds_sequence++;
        if (!target->bounds_check_elided && (length != 0 || dynamic_length)) {
            char fail_label[64];
            (void) snprintf(fail_label, sizeof(fail_label), ".LIR_bounds_fail_%zu_%zu",
                            emitter->function_index, bounds_id);
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rcx"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JL, X64_WIDTH_NONE, x64_label(fail_label));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rcx"), dynamic_length
                                                 ? x64_register("rdx")
                                                 : x64_immediate((long long) length));
            write_x64_1(emitter, X64_OP_JGE, X64_WIDTH_NONE, x64_label(fail_label));
        }
        IrTypeLayout element;
        if (!ir_type_layout(emitter->module, target->type_id, &element)) return 0;
        size_t element_size = element.size;
        write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                    x64_register("rcx"), x64_immediate((long long) element_size));
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_indexed_memory(X64_WIDTH_NONE, "rax", "rcx", 1, 0));
        if (!target->bounds_check_elided && (length != 0 || dynamic_length)) {
            char ok_label[64];
            (void) snprintf(ok_label, sizeof(ok_label), ".LIR_bounds_ok_%zu_%zu",
                            emitter->function_index, bounds_id);
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(ok_label));
            write_labelf(emitter, ".LIR_bounds_fail_%zu_%zu:\n",
                         emitter->function_index, bounds_id);
            write_x64_0(emitter, X64_OP_UD2);
            write_labelf(emitter, ".LIR_bounds_ok_%zu_%zu:\n",
                         emitter->function_index, bounds_id);
        }
        return 1;
    }
    if (target->opcode == IR_OP_MEMBER) {
        const IrInstruction *base = producer(emitter->function, target->operand_a);
        if (base == NULL || target->symbol_id >= emitter->module->semantics->symbol_count)
            return 0;
        const SemanticSymbol *field = &emitter->module->semantics->symbols[target->symbol_id];
        const IrAggregate *aggregate = aggregate_for_symbol(emitter->module,
                                                            field->owner_symbol_id);
        if (aggregate == NULL) return 0;
        if (is_pointer_value(base) || is_inline_structure(emitter->module, base)) {
            write_value_load(emitter, "rbx", target->operand_a);
        } else if (!emit_lvalue_address(emitter, base, instruction_index)) {
            return 0;
        }
        size_t offset = aggregate_field_offset(emitter, aggregate, target->symbol_id);
        if (offset == SIZE_MAX) return 0;
        if (offset != 0)
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                        x64_register("rbx"), x64_immediate((long long) offset));
        return 1;
    }
    return 0;
}

static void write_indirect_load(const Emitter *emitter, const char *destination,
                                const char *address) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_register(destination), x64_memory(X64_WIDTH_QWORD, address, 0));
    write_x64(emitter, instruction);
}

static void write_indirect_store(const Emitter *emitter, const char *address,
                                 const char *source) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
                                                  x64_memory(X64_WIDTH_QWORD, address, 0), x64_register(source));
    write_x64(emitter, instruction);
}

static void write_typed_indirect_load(const Emitter *emitter, DataType type,
                                      unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || data_type_bytes(type) == 8 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_load(emitter, "rax", address);
    } else {
        unsigned bytes = data_type_bytes(type);
        int signed_integer = is_integral(type) && !data_type_unsigned(type);
        X64Opcode opcode = signed_integer ? X64_OP_MOVSX : bytes < 4 ? X64_OP_MOVZX : X64_OP_MOV;
        X64Width source_width = bytes == 4 ? X64_WIDTH_DWORD : bytes == 2 ? X64_WIDTH_WORD : X64_WIDTH_BYTE;
        X64Width destination_width = signed_integer ? X64_WIDTH_QWORD : X64_WIDTH_DWORD;
        X64Instruction instruction = x64_instruction2(opcode, destination_width,
                                                      x64_sized_register(destination_width,
                                                                         destination_width == X64_WIDTH_QWORD
                                                                             ? "rax"
                                                                             : "eax"),
                                                      x64_memory(source_width, address, 0));
        write_x64(emitter, instruction);
    }
}

static void write_typed_indirect_store(const Emitter *emitter, DataType type,
                                       unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || data_type_bytes(type) == 8 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_store(emitter, address, "rax");
    } else {
        X64Width width = data_type_bytes(type) == 4
                             ? X64_WIDTH_DWORD
                             : data_type_bytes(type) == 2
                                   ? X64_WIDTH_WORD
                                   : X64_WIDTH_BYTE;
        X64Instruction instruction = x64_instruction2(X64_OP_MOV, width,
                                                      x64_memory(width, address, 0),
                                                      x64_sized_register(
                                                          width, width == X64_WIDTH_DWORD
                                                                     ? "eax"
                                                                     : width == X64_WIDTH_WORD
                                                                           ? "ax"
                                                                           : "al"));
        write_x64(emitter, instruction);
    }
}

static void write_frame_allocation(const Emitter *emitter) {
    size_t remaining = emitter->frame_size;
    while (remaining > 4096U) {
        X64Instruction subtract = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
                                                   x64_register("rsp"), x64_immediate(4096));
        X64Instruction probe = x64_instruction2(X64_OP_MOV, X64_WIDTH_BYTE,
                                                x64_memory(X64_WIDTH_BYTE, "rsp", 0), x64_immediate(0));
        write_x64(emitter, subtract);
        write_x64(emitter, probe);
        remaining -= 4096U;
    }
    if (remaining != 0) {
        X64Instruction subtract = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
                                                   x64_register("rsp"), x64_immediate((long long) remaining));
        write_x64(emitter, subtract);
    }
}

static int emit_enum_member(Emitter *emitter, const IrInstruction *instruction) {
    if (instruction->symbol_id >= emitter->module->semantics->symbol_count) return 0;
    const SemanticSymbol *symbol = &emitter->module->semantics->symbols[instruction->symbol_id];
    if (symbol->kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
        size_t enum_index = 0;
        const IrEnum *enumeration = enum_for_symbol(emitter->module,
                                                    symbol->owner_symbol_id, &enum_index);
        if (enumeration == NULL) return 0;
        for (size_t v = 0; v < enumeration->variant_count; v++) {
            if (enumeration->variants[v].symbol_id == instruction->symbol_id) {
                write_immediate(emitter, "rax", (long long) v);
                write_value_store(emitter, "rax", instruction->result);
                return 1;
            }
        }
        return 0;
    }
    if (symbol->kind != SEMANTIC_SYMBOL_FIELD) return 0;
    const IrInstruction *base = producer(emitter->function, instruction->operand_a);
    const IrEnum *enumeration = NULL;
    size_t enum_index = 0;
    size_t variant_index = 0;
    if (base == NULL) return 0;
    if (!enum_variant_location(emitter->module, base->symbol_id,
                               &enumeration, &enum_index, &variant_index)) {
        enumeration = enum_for_symbol(emitter->module, symbol->owner_symbol_id,
                                      &enum_index);
        size_t field_index = enumeration == NULL
                                 ? IR_VALUE_NONE
                                 : enum_field_index(enumeration, instruction->symbol_id);
        if (field_index == IR_VALUE_NONE) return 0;
        write_value_load(emitter, "rax", instruction->operand_a);
        char table[64];
        (void) snprintf(table, sizeof(table), ".LIR_enum_field_%zu_", enum_index);
        write_address(emitter, "rcx", table, field_index);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                    x64_indexed_memory(X64_WIDTH_QWORD, "rcx", "rax", 8, 0));
        write_value_store(emitter, "rax", instruction->result);
        return 1;
    }
    size_t field_index = enum_field_index(enumeration, instruction->symbol_id);
    const IrEnumVariant *variant = &enumeration->variants[variant_index];
    if (field_index == IR_VALUE_NONE || field_index >= variant->argument_count) return 0;
    size_t argument_index = variant->first_argument + field_index;
    const IrEnumArgument *argument = &enumeration->variant_arguments[argument_index];
    if (instruction->type == TYPE_STRING) {
        char label[64];
        (void) snprintf(label, sizeof(label), ".LIR_enum_%zu_", enum_index);
        write_address(emitter, "rax", label, argument_index);
    } else if (is_floating(instruction->type)) {
        const char *text = ast_program_lexeme(enumeration->source_program, argument->token);
        union {
            double floating;
            uint64_t bits;
        } value = {strtod(text, NULL)};
        write_immediate(emitter, "rax", (long long) value.bits);
        if (instruction->type == TYPE_FLOAT) convert_rax(emitter, TYPE_DOUBLE, TYPE_FLOAT);
    } else {
        IrInstruction literal = {.auxiliary_token = argument->token};
        write_immediate(emitter, "rax",
                        constant_value(enumeration->source_program, &literal));
    }
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static void emit_phi_moves(Emitter *emitter, size_t destination_label) {
    const IrFunction *function = emitter->function;
    size_t label_index = function->instruction_count;
    for (size_t i = 0; i < function->instruction_count; i++) {
        if (function->instructions[i].opcode == IR_OP_LABEL &&
            function->instructions[i].target_a == destination_label) {
            label_index = i;
            break;
        }
    }
    for (size_t i = label_index + 1; i < function->instruction_count; i++) {
        const IrInstruction *phi = &function->instructions[i];
        if (phi->opcode != IR_OP_PHI) break;
        size_t incoming = IR_VALUE_NONE;
        if (phi->target_a == emitter->current_label) incoming = phi->operand_a;
        else if (phi->target_b == emitter->current_label) incoming = phi->operand_b;
        if (incoming != IR_VALUE_NONE) {
            write_value_load(emitter, "rax", incoming);
            if (phi->type == TYPE_BIT) {
                write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(0));
                write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
                write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                            x64_sized_register(X64_WIDTH_QWORD, "rax"),
                            x64_sized_register(X64_WIDTH_BYTE, "al"));
            }
            write_value_store(emitter, "rax", phi->result);
        }
    }
}

#include "ir_async.inc"

static int emit_instruction(Emitter *emitter, const IrInstruction *instruction,
                            size_t index) {
    const IrFunction *function = emitter->function;
    switch (instruction->opcode) {
        case IR_OP_INTERFACE_PACK: {
            const IrInstruction *source = producer(function, instruction->operand_a);
            if (source == NULL || source->type_id >= emitter->module->type_count) return 0;
            const IrType *concrete = &emitter->module->types[source->type_id];
            size_t offset = aggregate_result_offset(emitter, instruction);
            size_t slots = type_slots(emitter->module, instruction->type_id);
            if (concrete->kind != IR_TYPE_NAMED || offset == 0 || slots == 0) return 0;
            write_immediate(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", 1);
            write_immediate(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi",
                            (long long) (type_slots(emitter->module, source->type_id) * 8U));
            write_call(emitter, "calloc");
            size_t allocation = emitter->bounds_sequence++;
            char allocated[96];
            snprintf(allocated, sizeof(allocated), ".LIR_interface_alloc_%zu_%zu",
                     emitter->function_index, allocation);
            write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rax"));
            write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(allocated));
            write_x64_0(emitter, X64_OP_UD2);
            write_labelf(emitter, "%s:\n", allocated);
            write_register_move(emitter, "rbx", "rax");
            write_value_load(emitter, "rax", instruction->operand_a);
            copy_aggregate(emitter, type_slots(emitter->module, source->type_id), "rax", "rbx");
            write_immediate(emitter, "rax",
                            (long long) ir_interface_type_tag(emitter->module,
                                                                concrete->symbol_id));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset),
                        x64_register("rax"));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset + 8),
                        x64_register("rbx"));
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_ENUM_CONSTRUCT:
        case IR_OP_ENUM_IS:
        case IR_OP_ENUM_PAYLOAD: {
            const IrEnum *enumeration = NULL;
            const IrEnumVariant *variant = NULL;
            size_t tag = 0;
            for (size_t e = 0; e < emitter->module->enum_count; e++)
                for (size_t v = 0; v < emitter->module->enums[e].variant_count; v++)
                    if (emitter->module->enums[e].variants[v].symbol_id == instruction->symbol_id) {
                        enumeration = &emitter->module->enums[e];
                        variant = &enumeration->variants[v];
                        tag = v;
                    }
            if (!enumeration || !variant) return 0;
            if (instruction->opcode == IR_OP_ENUM_IS) {
                write_value_load(emitter, "rax", instruction->operand_a);
                if (enumeration->is_sum)
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                                x64_memory(X64_WIDTH_QWORD, "rax", 0));
                write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate((long long) tag));
                write_x64_1(emitter, X64_OP_SETE, X64_WIDTH_BYTE, x64_register("al"));
                write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                            x64_sized_register(X64_WIDTH_QWORD, "rax"), x64_sized_register(X64_WIDTH_BYTE, "al"));
            } else if (instruction->opcode == IR_OP_ENUM_PAYLOAD) {
                size_t slot = 1;
                for (size_t p = 0; p < instruction->enum_payload_index; p++)
                    slot += type_slots(emitter->module,
                                       variant->payload_types[p]);
                write_value_load(emitter, "rax", instruction->operand_a);
                if (type_is_structure(emitter->module, instruction->type_id) || instruction->is_array)
                    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rax"),
                                x64_memory(X64_WIDTH_NONE, "rax", (long long) (slot * 8)));
                else
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                                x64_memory(X64_WIDTH_QWORD, "rax", (long long) (slot * 8)));
            } else if (!enumeration->is_sum) write_immediate(emitter, "rax", (long long) tag);
            else {
                size_t offset = aggregate_result_offset(emitter, instruction);
                size_t slots = type_slots(emitter->module, instruction->type_id);
                if (!offset || !slots) return 0;
                write_immediate(emitter, "rax", 0);
                for (size_t p = 0; p < slots; p++)
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset + (long long) (p * 8)),
                                x64_register("rax"));
                write_immediate(emitter, "rax", (long long) tag);
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset), x64_register("rax"));
                size_t slot = 1;
                for (size_t p = 0; p < variant->payload_count; p++) {
                    size_t argument = function->arguments[instruction->first_argument + p];
                    const IrInstruction *value = producer(function, argument);
                    IrTypeId type = variant->payload_types[p];
                    write_value_load(emitter, "rax", argument);
                    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                                x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset + (long long) (slot * 8)));
                    if (type_is_structure(emitter->module, type) || emitter->module->types[type].kind == IR_TYPE_ARRAY)
                        copy_aggregate(emitter, type_slots(emitter->module, type), "rax", "rbx");
                    else {
                        convert_rax(emitter, value->type,
                                    emitter->module->types[type].kind == IR_TYPE_PRIMITIVE
                                        ? emitter->module->types[type].primitive
                                        : TYPE_UNKNOWN);
                        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbx", 0),
                                    x64_register("rax"));
                    }
                    slot += type_slots(emitter->module, type);
                }
                write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rax"),
                            x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_TRAP: write_x64_0(emitter, X64_OP_UD2);
            return 1;

        case IR_OP_CONSTANT:
            if (instruction->type == TYPE_STRING) {
                char string_label[64];
                (void) snprintf(string_label, sizeof(string_label), ".LIR_string_%zu_",
                                emitter->function_index);
                write_address(emitter, "rax", string_label, instruction->result);
                write_value_store(emitter, "rax", instruction->result);
            } else {
                if (instruction->has_immediate) {
                    write_immediate(emitter, "rax", (long long) instruction->immediate);
                } else if (is_floating(instruction->type)) {
                    const AstProgram *program = instruction->source_program != NULL
                                                    ? instruction->source_program
                                                    : function->source_program;
                    const char *text = ast_program_lexeme(program,
                                                          instruction->auxiliary_token);
                    union {
                        double floating;
                        uint64_t bits;
                    } value = {strtod(text, NULL)};
                    if (instruction->type == TYPE_FLOAT) {
                        union {
                            float floating;
                            uint32_t bits;
                        } single = {
                            (float) value.floating
                        };
                        write_immediate(emitter, "rax", (long long) single.bits);
                    } else write_immediate(emitter, "rax", (long long) value.bits);
                } else {
                    const AstProgram *program = instruction->source_program != NULL
                                                    ? instruction->source_program
                                                    : function->source_program;
                    write_immediate(emitter, "rax", constant_value(program,
                                                                   instruction));
                }
                write_value_store(emitter, "rax", instruction->result);
            }
            return 1;
        case IR_OP_FUNCTION_ADDRESS: {
            const IrFunction *addressed = addressed_function(emitter->module,
                                                           instruction->symbol_id);
            if (addressed == NULL) return 0;
            char buffer[4096];
            const char *name = function_link_name(emitter->module, addressed,
                                                  buffer, sizeof(buffer));
            if (name == NULL) return 0;
            X64Operand address = x64_rip_memory(X64_WIDTH_NONE, name, 0);
            address.has_symbol_suffix = 0;
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                        x64_register("rax"), address);
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_LOAD: {
            const AstProgram *program = instruction->source_program != NULL
                                            ? instruction->source_program
                                            : function->source_program;
            const char *name = ast_program_lexeme(program,
                                                  instruction->auxiliary_token);
            if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0)
                write_immediate(emitter, "rax", strcmp(name, "true") == 0 ? 1 : 0);
            else {
                const IrInstruction *declaration = local_declaration(function,
                                                                     instruction->symbol_id, index);
                const IrParameter *parameter = function_parameter(function,
                                                                  instruction->symbol_id);
                if (declaration != NULL &&
                    (declaration->is_array || is_inline_structure(emitter->module,
                                                                  declaration))) {
                    size_t offset = declaration_offset(emitter, declaration);
                    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                                x64_register("rax"),
                                x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
                } else if (declaration != NULL)
                    write_local_load(emitter, "rax", declaration_offset(emitter, declaration));
                else if (parameter != NULL)
                    write_local_load(emitter, "rax", parameter_offset(emitter, parameter));
                else if (emit_lvalue_address(emitter, instruction, index)) {
                    if (instruction->is_array || is_inline_structure(emitter->module, instruction))
                        write_register_move(
                            emitter, "rax", "rbx");
                    else write_typed_indirect_load(emitter, instruction->type, instruction->pointer_depth, "rbx");
                } else write_immediate(emitter, "rax", 0);
            }
            if (instruction->pointer_depth == 0 && !instruction->is_array && !instruction->is_slice &&
                instruction->symbol_id != AST_SYMBOL_NONE && data_type_fixed_integer(instruction->type))
                normalize_integral_parameter(emitter, instruction->type);
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_DECLARE: {
            size_t offset = declaration_offset(emitter, instruction);
            if (instruction->is_array || is_inline_structure(emitter->module, instruction)) {
                size_t slots = type_slots(emitter->module, instruction->type_id);
                if (instruction->operand_a != IR_VALUE_NONE &&
                    (instruction->is_array || is_inline_structure(emitter->module, instruction))) {
                    write_value_load(emitter, "rax", instruction->operand_a);
                    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                                x64_register("rbx"),
                                x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
                    copy_aggregate(emitter, slots, "rax", "rbx");
                } else {
                    write_immediate(emitter, "rax", 0);
                    for (size_t slot = 0; slot < slots; slot++)
                        write_local_store(emitter, "rax", offset - slot * 8U);
                }
                if (ir_type_properties(emitter->module, instruction->type_id) &
                    SEMANTIC_TYPE_NEEDS_DROP) {
                    write_immediate(emitter, "rax", 1);
                    write_local_store(emitter, "rax",
                                      declaration_flag_offset(emitter,
                                                              instruction));
                }
                if (instruction->is_slice) {
                    if (instruction->owns_slice_backing)
                        write_local_load(emitter, "rax", offset);
                    else write_immediate(emitter, "rax", 0);
                    write_local_store(emitter, "rax",
                                      declaration_slice_owner_offset(
                                          emitter, instruction));
                }
                return 1;
            }
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else {
                const IrInstruction *value = producer(function, instruction->operand_a);
                write_value_load(emitter, "rax", instruction->operand_a);
                if (instruction->pointer_depth == 0)
                    convert_rax(emitter, value->type, instruction->type);
            }
            write_local_store(emitter, "rax", offset);
            if (ir_type_properties(emitter->module, instruction->type_id) &
                SEMANTIC_TYPE_NEEDS_DROP) {
                write_immediate(emitter, "rax", 1);
                write_local_store(emitter, "rax",
                                  declaration_flag_offset(emitter,
                                                          instruction));
            }
            return 1;
        }
        case IR_OP_AWAIT:
            return emit_async_await(emitter, instruction);
        case IR_OP_DROP:
        case IR_OP_MOVE:
        case IR_OP_REINIT:
            return emit_ownership_effect(emitter, instruction, index);
        case IR_OP_STORE: {
            const IrInstruction *target = producer(function, instruction->operand_a);
            if (emitter->module->optimized && target && target->opcode == IR_OP_LOAD &&
                target->type == TYPE_INT && target->pointer_depth == 0 && !target->is_array &&
                !target->is_slice && (instruction->operator_type == TOKEN_PLUS_EQUAL ||
                                      instruction->operator_type == TOKEN_MINUS_EQUAL)) {
                const IrInstruction *value = producer(function, instruction->operand_b);
                const IrInstruction *declaration = local_declaration(function, target->symbol_id, index);
                const IrParameter *parameter = function_parameter(function, target->symbol_id);
                if (value && value->type == TYPE_INT && (declaration || parameter)) {
                    size_t offset = declaration ? declaration_offset(emitter, declaration)
                                                : parameter_offset(emitter, parameter);
                    write_value_load(emitter, "rax", instruction->operand_b);
                    write_x64_2(emitter, instruction->operator_type == TOKEN_PLUS_EQUAL
                                             ? X64_OP_ADD : X64_OP_SUB, X64_WIDTH_DWORD,
                                x64_memory(X64_WIDTH_DWORD, "rbp", -(long long) offset),
                                x64_register("eax"));
                    return 1;
                }
            }
            if (!emit_lvalue_address(emitter, target, index)) return 0;
            if (instruction->operator_type == TOKEN_EQUAL &&
                (target->is_array || is_inline_structure(emitter->module, target))) {
                write_value_load(emitter, "rax", instruction->operand_b);
                copy_aggregate(emitter, type_slots(emitter->module, target->type_id),
                               "rax", "rbx");
                if (target->is_slice && target->opcode == IR_OP_LOAD) {
                    const IrInstruction *declaration = local_declaration(
                        function, target->symbol_id, index);
                    if (declaration != NULL) {
                        if (instruction->owns_slice_backing)
                            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                        x64_register("rax"),
                                        x64_memory(X64_WIDTH_QWORD, "rbx", 0));
                        else write_immediate(emitter, "rax", 0);
                        write_local_store(
                            emitter, "rax",
                            declaration_slice_owner_offset(emitter,
                                                           declaration));
                    } else if (target->symbol_id <
                               emitter->module->semantics->symbol_count &&
                               emitter->module->semantics->symbols[
                                   target->symbol_id].kind ==
                                   SEMANTIC_SYMBOL_VARIABLE) {
                        char owner[4096];
                        if (!global_slice_owner_label(
                                &emitter->module->semantics->symbols[
                                    target->symbol_id],
                                owner, sizeof(owner)))
                            return 0;
                        X64Operand owner_memory =
                            x64_rip_memory(X64_WIDTH_QWORD, owner, 0);
                        owner_memory.has_symbol_suffix = 0;
                        if (instruction->owns_slice_backing)
                            write_x64_2(emitter, X64_OP_MOV,
                                        X64_WIDTH_QWORD,
                                        x64_register("rax"),
                                        x64_memory(X64_WIDTH_QWORD,
                                                   "rbx", 0));
                        else write_immediate(emitter, "rax", 0);
                        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                    owner_memory, x64_register("rax"));
                    }
                }
                return 1;
            }
            write_typed_indirect_load(emitter, target->type, target->pointer_depth, "rbx");
            if (is_floating(target->type)) {
                if (instruction->operator_type == TOKEN_EQUAL) {
                    const IrInstruction *stored = producer(function, instruction->operand_b);
                    write_value_load(emitter, "rax", instruction->operand_b);
                    convert_rax(emitter, stored->type, target->type);
                } else {
                    write_x64_2(emitter, target->type == TYPE_FLOAT
                                             ? X64_OP_MOVD
                                             : X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register("xmm2"),
                                x64_register(target->type == TYPE_FLOAT ? "eax" : "rax"));
                    if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                        instruction->operator_type == TOKEN_MINUS_MINUS) {
                        write_immediate(emitter, "rax", 1);
                        convert_rax(emitter, TYPE_INT, target->type);
                        write_x64_2(emitter, target->type == TYPE_FLOAT
                                                 ? X64_OP_MOVD
                                                 : X64_OP_MOVQ, X64_WIDTH_NONE,
                                    x64_register("xmm1"),
                                    x64_register(target->type == TYPE_FLOAT ? "eax" : "rax"));
                    } else {
                        load_floating_value(emitter, instruction->operand_b,
                                            target->type, 1);
                    }
                    int add = instruction->operator_type == TOKEN_PLUS_EQUAL ||
                              instruction->operator_type == TOKEN_PLUS_PLUS;
                    int subtract = instruction->operator_type == TOKEN_MINUS_EQUAL ||
                                   instruction->operator_type == TOKEN_MINUS_MINUS;
                    X64Opcode operation = target->type == TYPE_FLOAT
                                              ? (add
                                                     ? X64_OP_ADDSS
                                                     : subtract
                                                           ? X64_OP_SUBSS
                                                           : instruction->operator_type == TOKEN_STAR_EQUAL
                                                                 ? X64_OP_MULSS
                                                                 : X64_OP_DIVSS)
                                              : (add
                                                     ? X64_OP_ADDSD
                                                     : subtract
                                                           ? X64_OP_SUBSD
                                                           : instruction->operator_type == TOKEN_STAR_EQUAL
                                                                 ? X64_OP_MULSD
                                                                 : X64_OP_DIVSD);
                    write_x64_2(emitter, operation, X64_WIDTH_NONE,
                                x64_register("xmm2"), x64_register("xmm1"));
                    write_x64_2(emitter, target->type == TYPE_FLOAT
                                             ? X64_OP_MOVD
                                             : X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register(target->type == TYPE_FLOAT ? "eax" : "rax"),
                                x64_register("xmm2"));
                }
            } else if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                       instruction->operator_type == TOKEN_MINUS_MINUS) {
                write_x64_1(emitter, instruction->operator_type == TOKEN_PLUS_PLUS
                                         ? X64_OP_INC
                                         : X64_OP_DEC, X64_WIDTH_QWORD,
                            x64_register("rax"));
            } else {
                if (target->pointer_depth == 0 && data_type_fixed_integer(target->type)) {
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("r10"), x64_register("rax"));
                    write_value_load(emitter, "rax", instruction->operand_b);
                    normalize_integral_parameter(emitter, target->type);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rcx"), x64_register("rax"));
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"), x64_register("r10"));
                } else write_value_load(emitter, "rcx", instruction->operand_b);
                if (instruction->operator_type == TOKEN_EQUAL)
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                else if (instruction->operator_type == TOKEN_PLUS_EQUAL)
                    write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                else if (instruction->operator_type == TOKEN_MINUS_EQUAL)
                    write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                else if (instruction->operator_type == TOKEN_STAR_EQUAL)
                    write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                else if (instruction->operator_type == TOKEN_SLASH_EQUAL) {
                    if (data_type_fixed_integer(target->type) && data_type_unsigned(target->type)) {
                        write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD, x64_register("edx"), x64_register("edx"));
                        write_x64_1(emitter, X64_OP_DIV, X64_WIDTH_QWORD, x64_register("rcx"));
                    } else {
                        write_x64_0(emitter, X64_OP_CQO);
                        write_x64_1(emitter, X64_OP_IDIV, X64_WIDTH_QWORD, x64_register("rcx"));
                    }
                } else return 0;
            }
            write_typed_indirect_store(emitter, target->type, target->pointer_depth, "rbx");
            return 1;
        }
        case IR_OP_UNARY: {
            const IrInstruction *operand = producer(function, instruction->operand_b);
            if (instruction->operator_type == TOKEN_AMPERSAND) {
                if (!emit_lvalue_address(emitter, operand, index)) return 0;
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rbx"));
                write_value_store(emitter, "rax", instruction->result);
                return 1;
            }
            write_value_load(emitter, "rax", instruction->operand_b);
            if (instruction->operator_type == TOKEN_STAR) {
                if (!instruction->is_array && !is_inline_structure(emitter->module, instruction))
                    write_typed_indirect_load(emitter, instruction->type,
                                              instruction->pointer_depth, "rax");
                write_value_store(emitter, "rax", instruction->result);
                return 1;
            }
            if (instruction->operator_type == TOKEN_MINUS) {
                if (instruction->type == TYPE_FLOAT)
                    write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                                x64_register("eax"), x64_immediate(0x80000000LL));
                else if (instruction->type == TYPE_DOUBLE) {
                    write_x64_2(emitter, X64_OP_MOVABS, X64_WIDTH_QWORD,
                                x64_register("rcx"), x64_immediate((long long) UINT64_C(0x8000000000000000)));
                    write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                } else
                    write_x64_1(emitter, X64_OP_NEG, X64_WIDTH_QWORD,
                                x64_register("rax"));
            } else {
                normalize_truth_rax(emitter, operand->type);
                write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(1));
            }
            if (data_type_fixed_integer(instruction->type) && instruction->operator_type == TOKEN_MINUS)
                normalize_integral_parameter(emitter, instruction->type);
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_BINARY:
            return emit_binary(emitter, instruction);
        case IR_OP_RETURN:
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else {
                const IrInstruction *value = producer(function, instruction->operand_a);
                DataType return_type = ir_ast_type(function->source_program,
                                                   &function->return_type);
                write_value_load(emitter, "rax", instruction->operand_a);
                int scalar = emitter->module->types[function->return_type_id].kind == IR_TYPE_PRIMITIVE;
                if (scalar)
                    convert_rax(emitter, value->type, return_type);
                if (!function->is_async && scalar && return_type == TYPE_FLOAT)
                    write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("eax"));
                else if (!function->is_async && scalar && return_type == TYPE_DOUBLE)
                    write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("rax"));
            }
            if (function->is_async) async_complete(emitter);
            char epilogue[64];
            (void) snprintf(epilogue, sizeof(epilogue), ".LIR_epilogue_%zu",
                            emitter->function_index);
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(epilogue));
            return 1;
        case IR_OP_BRANCH: {
            write_value_load(emitter, "rax", instruction->operand_a);
            DataType condition_type = producer(function, instruction->operand_a)->type;
            if (emitter->module->optimized && data_type_integral(condition_type))
                write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rax"));
            else {
                normalize_truth_rax(emitter, condition_type);
                write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(0));
            }
            char true_label[64];
            char false_label[64];
            (void) snprintf(true_label, sizeof(true_label), ".LIR_%zu_%zu",
                            emitter->function_index, instruction->target_a);
            (void) snprintf(false_label, sizeof(false_label), ".LIR_%zu_%zu",
                            emitter->function_index, instruction->target_b);
            if (emitter->module->optimized && index + 1 < function->instruction_count &&
                function->instructions[index + 1].opcode == IR_OP_LABEL &&
                function->instructions[index + 1].target_a == instruction->target_a)
                write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(false_label));
            else {
                write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(true_label));
                write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(false_label));
            }
            return 1;
        }
        case IR_OP_JUMP:
            emit_phi_moves(emitter, instruction->target_a);
            {
                char target_label[64];
                (void) snprintf(target_label, sizeof(target_label), ".LIR_%zu_%zu",
                                emitter->function_index, instruction->target_a);
                write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(target_label));
                return 1;
            }
        case IR_OP_LABEL:
            emitter->current_label = instruction->target_a;
            write_labelf(emitter, ".LIR_%zu_%zu:\n", emitter->function_index,
                         instruction->target_a);
            return 1;
        case IR_OP_PHI:
            return 1;
        case IR_OP_CAST: {
            const IrInstruction *source = producer(function, instruction->operand_a);
            write_value_load(emitter, "rax", instruction->operand_a);
            if (emitter->module->types[instruction->type_id].kind == IR_TYPE_POINTER) {
                write_value_store(emitter, "rax", instruction->result);
                return 1;
            }
            if (instruction->type == TYPE_BIT && is_floating(source->type)) {
                if (source->type == TYPE_FLOAT) {
                    write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("eax"));
                    write_x64_2(emitter, X64_OP_XORPS, X64_WIDTH_NONE,
                                x64_register("xmm1"), x64_register("xmm1"));
                    write_x64_2(emitter, X64_OP_UCOMISS, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("xmm1"));
                } else {
                    write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("rax"));
                    write_x64_2(emitter, X64_OP_XORPD, X64_WIDTH_NONE,
                                x64_register("xmm1"), x64_register("xmm1"));
                    write_x64_2(emitter, X64_OP_UCOMISD, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("xmm1"));
                }
                write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
                write_x64_1(emitter, X64_OP_SETP, X64_WIDTH_NONE, x64_register("dl"));
                write_x64_2(emitter, X64_OP_OR, X64_WIDTH_BYTE,
                            x64_register("al"), x64_register("dl"));
                write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                            x64_sized_register(X64_WIDTH_QWORD, "rax"),
                            x64_sized_register(X64_WIDTH_BYTE, "al"));
            } else {
                convert_rax(emitter, source->type, instruction->type);
                if (instruction->type == TYPE_BIT)
                    normalize_truth_rax(emitter, source->type);
                else if (instruction->type == TYPE_BYTE || instruction->type == TYPE_CHAR)
                    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                                x64_sized_register(X64_WIDTH_BYTE, "al"));
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        }
        case IR_OP_CALL: {
            const IrFunction *callee = called_function(emitter->module, instruction->symbol_id);
            if (callee == NULL) {
                const IrInstruction *callee_value = producer(function,
                                                              instruction->operand_a);
                if (callee_value && callee_value->type_id < emitter->module->type_count &&
                    emitter->module->types[callee_value->type_id].kind == IR_TYPE_FUNCTION)
                    return emit_indirect_typed_call(emitter, instruction,
                                                    instruction->operand_a,
                                                    callee_value->type_id);
                if (instruction->symbol_id < emitter->module->semantics->symbol_count) {
                    const SemanticSymbol *method =
                        &emitter->module->semantics->symbols[instruction->symbol_id];
                    if (method->kind == SEMANTIC_SYMBOL_FUNCTION &&
                        method->owner_symbol_id < emitter->module->semantics->symbol_count &&
                        emitter->module->semantics->symbols[method->owner_symbol_id].kind ==
                        SEMANTIC_SYMBOL_INTERFACE)
                        return emit_interface_call(emitter, instruction);
                }
                if (!callee_value) return 0;
                const AstProgram *program = callee_value->source_program != NULL
                                                ? callee_value->source_program
                                                : function->source_program;
                const char *name = ast_program_lexeme(program,
                                                      callee_value->auxiliary_token);
                return emit_builtin_call(emitter, instruction, name);
            }
            return emit_typed_call(emitter, instruction, callee, 0,
                                   IR_VALUE_NONE);
        }
        case IR_OP_INDEX:
            if (!emit_lvalue_address(emitter, instruction, index)) return 0;
            if (instruction->is_array || is_inline_structure(emitter->module, instruction)) {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rbx"));
            } else
                write_typed_indirect_load(emitter, instruction->type,
                                          instruction->pointer_depth, "rbx");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_SUBSLICE: {
            const IrInstruction *base =
                producer(function, instruction->operand_a);
            if (base == NULL || base->type_id >= emitter->module->type_count ||
                instruction->type_id >= emitter->module->type_count)
                return 0;
            const IrType *base_type =
                &emitter->module->types[base->type_id];
            const IrType *result_type =
                &emitter->module->types[instruction->type_id];
            if ((base_type->kind != IR_TYPE_ARRAY &&
                 base_type->kind != IR_TYPE_SLICE) ||
                result_type->kind != IR_TYPE_SLICE)
                return 0;
            IrTypeLayout element;
            if (!ir_type_layout(emitter->module, result_type->element_type,
                                &element))
                return 0;
            size_t sequence = emitter->bounds_sequence++;
            char fail[80], valid[80];
            snprintf(fail, sizeof(fail), ".LIR_subslice_fail_%zu_%zu",
                     emitter->function_index, sequence);
            snprintf(valid, sizeof(valid), ".LIR_subslice_valid_%zu_%zu",
                     emitter->function_index, sequence);
            write_value_load(emitter, "rax", instruction->operand_a);
            if (base_type->kind == IR_TYPE_SLICE) {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rdx"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 8));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 0));
            } else {
                write_immediate(emitter, "rdx",
                                (long long) base_type->array_length);
            }
            if (instruction->operand_b == IR_VALUE_NONE)
                write_immediate(emitter, "rcx", 0);
            else
                write_value_load(emitter, "rcx", instruction->operand_b);
            if (instruction->argument_count == 0)
                write_register_move(emitter, "r8", "rdx");
            else
                write_value_load(
                    emitter, "r8",
                    function->arguments[instruction->first_argument]);
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rcx"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JL, X64_WIDTH_NONE,
                        x64_label(fail));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("r8"), x64_register("rcx"));
            write_x64_1(emitter, X64_OP_JL, X64_WIDTH_NONE,
                        x64_label(fail));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("r8"), x64_register("rdx"));
            write_x64_1(emitter, X64_OP_JG, X64_WIDTH_NONE,
                        x64_label(fail));
            write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                        x64_register("rcx"),
                        x64_immediate((long long) element.size));
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            /* Recover the unscaled start to form the result length. */
            if (instruction->operand_b == IR_VALUE_NONE)
                write_immediate(emitter, "r9", 0);
            else
                write_value_load(emitter, "r9", instruction->operand_b);
            if (instruction->argument_count == 0)
                write_register_move(emitter, "r8", "rdx");
            else
                write_value_load(
                    emitter, "r8",
                    function->arguments[instruction->first_argument]);
            write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                        x64_register("r8"), x64_register("r9"));
            size_t offset = aggregate_result_offset(emitter, instruction);
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                        x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp",
                                   -(long long) offset));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rbx", 0),
                        x64_register("rax"));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_memory(X64_WIDTH_QWORD, "rbx", 8),
                        x64_register("r8"));
            write_value_store(emitter, "rbx", instruction->result);
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE,
                        x64_label(valid));
            write_labelf(emitter, "%s:\n", fail);
            write_x64_0(emitter, X64_OP_UD2);
            write_labelf(emitter, "%s:\n", valid);
            return 1;
        }
        case IR_OP_MEMBER:
            if (emit_enum_member(emitter, instruction)) return 1;
            if (!emit_lvalue_address(emitter, instruction, index)) return 0;
            if (instruction->is_array || is_inline_structure(emitter->module, instruction)) {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rbx"));
            } else
                write_typed_indirect_load(emitter, instruction->type,
                                          instruction->pointer_depth, "rbx");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_SLICE_LENGTH:
        case IR_OP_SLICE_DATA: {
            write_value_load(emitter, "rax", instruction->operand_a);
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rax"),
                        x64_memory(X64_WIDTH_QWORD, "rax", instruction->opcode == IR_OP_SLICE_LENGTH ? 8 : 0));
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_SLICE: {
            size_t sequence = emitter->bounds_sequence++;
            char fail[80], valid[80];
            snprintf(fail, sizeof(fail), ".LIR_slice_fail_%zu_%zu", emitter->function_index, sequence);
            snprintf(valid, sizeof(valid), ".LIR_slice_valid_%zu_%zu", emitter->function_index, sequence);
            write_value_load(emitter, "rax", instruction->operand_a);
            write_value_load(emitter, "rcx", instruction->operand_b);
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rcx"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JL, X64_WIDTH_NONE, x64_label(fail));
            IrTypeLayout element;
            if (!ir_type_layout(emitter->module, emitter->module->types[instruction->type_id].element_type,
                                &element))
                return 0;
            write_immediate(emitter, "rdx", (long long) ((uint64_t) INT64_MAX / element.size));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rcx"), x64_register("rdx"));
            write_x64_1(emitter, X64_OP_JG, X64_WIDTH_NONE, x64_label(fail));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rcx"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(valid));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(valid));
            write_labelf(emitter, "%s:\n", fail);
            write_x64_0(emitter, X64_OP_UD2);
            write_labelf(emitter, "%s:\n", valid);
            size_t offset = aggregate_result_offset(emitter, instruction);
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbx", 0),
                        x64_register("rax"));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbx", 8),
                        x64_register("rcx"));
            write_value_store(emitter, "rbx", instruction->result);
            return 1;
        }
        case IR_OP_ARRAY_LITERAL: {
            if (instruction->type_id >= emitter->module->type_count ||
                instruction->argument_count == 0 || instruction->element_count == 0)
                return 0;
            const IrType *container = &emitter->module->types[instruction->type_id];
            if (container->kind != IR_TYPE_ARRAY && container->kind != IR_TYPE_SLICE)
                return 0;
            IrTypeLayout element_layout;
            if (!ir_type_layout(emitter->module, container->element_type,
                                &element_layout) || element_layout.storage_slots == 0)
                return 0;
            if (container->kind == IR_TYPE_SLICE) {
                write_immediate(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                                (long long) instruction->element_count);
                write_immediate(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi",
                                (long long) element_layout.size);
                write_call(emitter, "calloc");
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("r10"), x64_register("rax"));
            } else {
                size_t offset = aggregate_result_offset(emitter, instruction);
                write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                            x64_register("r10"),
                            x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
            }
            for (size_t element = 0; element < instruction->element_count; element++) {
                size_t value_id = function->arguments[
                    instruction->first_argument + element % instruction->argument_count];
                const IrInstruction *value = producer(function, value_id);
                if (value == NULL) return 0;
                size_t byte_offset = element * element_layout.size;
                const IrType *target = &emitter->module->types[container->element_type];
                if (element_layout.storage_slots > 1U ||
                           type_is_structure(emitter->module, container->element_type)) {
                    write_value_load(emitter, "rax", value_id);
                    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                                x64_register("rdx"),
                                x64_memory(X64_WIDTH_NONE, "r10", (long long) byte_offset));
                    copy_aggregate(emitter, element_layout.storage_slots, "rax", "rdx");
                } else {
                    write_value_load(emitter, "rax", value_id);
                    if (target->kind == IR_TYPE_PRIMITIVE)
                        convert_rax(emitter, value->type, target->primitive);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_memory(X64_WIDTH_QWORD, "r10", (long long) byte_offset),
                                x64_register("rax"));
                }
            }
            if (container->kind == IR_TYPE_SLICE) {
                size_t descriptor = aggregate_result_offset(emitter, instruction);
                write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                            x64_register("rbx"),
                            x64_memory(X64_WIDTH_NONE, "rbp", -(long long) descriptor));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_memory(X64_WIDTH_QWORD, "rbx", 0), x64_register("r10"));
                write_immediate(emitter, "rax", (long long) instruction->element_count);
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_memory(X64_WIDTH_QWORD, "rbx", 8), x64_register("rax"));
                write_value_store(emitter, "rbx", instruction->result);
            } else write_value_store(emitter, "r10", instruction->result);
            return 1;
        }
        case IR_OP_ALLOC: {
            size_t slots = 1;
            if (instruction->type_id < emitter->module->type_count) {
                const IrType *pointer = &emitter->module->types[instruction->type_id];
                if (pointer->kind == IR_TYPE_POINTER)
                    slots = type_slots(emitter->module, pointer->element_type);
            }
            write_immediate(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", 1);
            write_immediate(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi",
                            (long long) (slots * 8U));
            write_call(emitter, "calloc");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_FREE_SLICE_BACKING: {
            const char *argument = emitter->target == TARGET_COFF ? "rcx" : "rdi";
            if (instruction->operand_a != IR_VALUE_NONE) {
                write_value_load(emitter, "rax", instruction->operand_a);
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rdx"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 8));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"),
                            x64_memory(X64_WIDTH_QWORD, "rax", 0));
            } else {
                const IrInstruction *declaration = local_declaration(
                    function, instruction->symbol_id, index);
                if (declaration != NULL) {
                    write_local_load(emitter, "rdx",
                                     declaration_offset(emitter, declaration) - 8U);
                    write_local_load(emitter, "rax",
                                     declaration_slice_owner_offset(
                                         emitter, declaration));
                    write_immediate(emitter, "rcx", 0);
                    write_local_store(emitter, "rcx",
                                      declaration_slice_owner_offset(
                                          emitter, declaration));
                } else if (instruction->symbol_id <
                               emitter->module->semantics->symbol_count &&
                           emitter->module->semantics->symbols[
                               instruction->symbol_id].kind ==
                               SEMANTIC_SYMBOL_VARIABLE) {
                    char owner[4096];
                    if (!global_slice_owner_label(
                            &emitter->module->semantics->symbols[
                                instruction->symbol_id],
                            owner, sizeof(owner)))
                        return 0;
                    X64Operand owner_memory =
                        x64_rip_memory(X64_WIDTH_QWORD, owner, 0);
                    owner_memory.has_symbol_suffix = 0;
                    char label[4096];
                    if (!global_label(&emitter->module->semantics->symbols[
                                          instruction->symbol_id],
                                      label, sizeof(label))) return 0;
                    X64Operand length_memory =
                        x64_rip_memory(X64_WIDTH_QWORD, label, 8);
                    length_memory.has_symbol_suffix = 0;
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register("rdx"), length_memory);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register("rax"), owner_memory);
                    write_immediate(emitter, "rcx", 0);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                owner_memory, x64_register("rcx"));
                } else return 0;
            }
            if (!emit_drop_owned_slice_elements(emitter, instruction->type_id)) return 0;
            write_register_move(emitter, argument, "rax");
            write_call(emitter, "free");
            return 1;
        }
        case IR_OP_FREE:
            write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                             instruction->operand_a);
            write_call(emitter, "free");
            return 1;
    }
    return 0;
}

/* A comparison used only by the immediately following branch needs no
   materialized boolean or virtual stack slot. Keep this to signed int
   operands until the other comparison domains have direct branch lowering. */
static int emit_fused_compare_branch(Emitter *emitter, size_t index) {
    const IrFunction *function = emitter->function;
    if (!emitter->module->optimized || index + 1 >= function->instruction_count) return 0;
    const IrInstruction *compare = &function->instructions[index];
    const IrInstruction *branch = &function->instructions[index + 1];
    if (compare->opcode != IR_OP_BINARY || compare->result == IR_VALUE_NONE ||
        branch->opcode != IR_OP_BRANCH || branch->operand_a != compare->result) return 0;
    const IrInstruction *left = producer(function, compare->operand_a);
    const IrInstruction *right = producer(function, compare->operand_b);
    if (!left || !right || left->type != TYPE_INT || right->type != TYPE_INT) return 0;
    X64Opcode false_jump;
    switch (compare->operator_type) {
        case TOKEN_EQUAL_EQUAL: false_jump = X64_OP_JNE; break;
        case TOKEN_BANG_EQUAL: false_jump = X64_OP_JE; break;
        case TOKEN_LESS: false_jump = X64_OP_JGE; break;
        case TOKEN_LESS_EQUAL: false_jump = X64_OP_JG; break;
        case TOKEN_GREATER: false_jump = X64_OP_JLE; break;
        case TOKEN_GREATER_EQUAL: false_jump = X64_OP_JL; break;
        default: return 0;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
        if (i == index + 1) continue;
        const IrInstruction *in = &function->instructions[i];
        if (in->operand_a == compare->result || in->operand_b == compare->result) return 0;
        for (size_t a = 0; a < in->argument_count; a++)
            if (function->arguments[in->first_argument + a] == compare->result) return 0;
    }
    write_value_load(emitter, "rax", compare->operand_b);
    normalize_integral_parameter(emitter, TYPE_INT);
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_register("rcx"), x64_register("rax"));
    write_value_load(emitter, "rax", compare->operand_a);
    normalize_integral_parameter(emitter, TYPE_INT);
    write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD, x64_register("rax"), x64_register("rcx"));
    char true_label[64], false_label[64];
    (void) snprintf(true_label, sizeof(true_label), ".LIR_%zu_%zu",
                    emitter->function_index, branch->target_a);
    (void) snprintf(false_label, sizeof(false_label), ".LIR_%zu_%zu",
                    emitter->function_index, branch->target_b);
    if (index + 2 < function->instruction_count &&
        function->instructions[index + 2].opcode == IR_OP_LABEL &&
        function->instructions[index + 2].target_a == branch->target_a)
        write_x64_1(emitter, false_jump, X64_WIDTH_NONE, x64_label(false_label));
    else {
        write_x64_1(emitter, false_jump, X64_WIDTH_NONE, x64_label(false_label));
        write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(true_label));
    }
    return 1;
}

static int store_target_needs_no_value(const Emitter *emitter, size_t index) {
    const IrFunction *function = emitter->function;
    if (!emitter->module->optimized || index + 1 >= function->instruction_count) return 0;
    const IrInstruction *load = &function->instructions[index];
    const IrInstruction *store = &function->instructions[index + 1];
    if (load->opcode != IR_OP_LOAD || load->result == IR_VALUE_NONE ||
        store->opcode != IR_OP_STORE || store->operand_a != load->result ||
        store->operand_b == load->result ||
        (!local_declaration(function, load->symbol_id, index) &&
         !function_parameter(function, load->symbol_id))) return 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
        if (i == index + 1) continue;
        const IrInstruction *in = &function->instructions[i];
        if (in->operand_a == load->result || in->operand_b == load->result) return 0;
        for (size_t a = 0; a < in->argument_count; a++)
            if (function->arguments[in->first_argument + a] == load->result) return 0;
    }
    return 1;
}

int emit_function(Emitter *emitter) {
    emitter->async_storage = 0;
    emitter->async_frame_bytes = 0;
    FILE *output = emitter->output;
    if (emitter->source_map != NULL)
        emitter->source_map->current_program = emitter->function->source_program;
    char name_buffer[4096];
    const char *name = function_link_name(emitter->module, emitter->function, name_buffer,
                                          sizeof(name_buffer));
    if (name == NULL) return 0;
    emitter->current_label = IR_VALUE_NONE;
    emitter->bounds_sequence = 0;
    int exported = !emitter->function->is_drop_glue &&
                   !emitter->function->is_package_init &&
                   !emitter->function->is_package_cleanup &&
                   (emitter->module->semantics->symbols[
                        emitter->function->symbol_id].declaration->is_public ||
                    !strcmp(name, "main"));
    if (emitter->native) {
        if (!native_define(emitter->native, name, exported, 1)) return 0;
    } else {
        if (exported) fprintf(output, "    .globl %s\n", name);
        if (emitter->target == TARGET_COFF)
            fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", name);
        else
            fprintf(output, "    .type %s, @function\n", name);
        fprintf(output, "%s:\n", name);
    }
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register("rbp"), x64_register("rsp"));
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbx"));
    write_frame_allocation(emitter);
    write_immediate(emitter, "rax", 0);
    for (size_t physical = 0;
         physical < physical_parameter_count(emitter->function); physical++) {
        size_t p = 0;
        int is_length = 0;
        if (!physical_parameter(emitter->function, physical, &p, &is_length)) return 0;
        const IrParameter *parameter = &emitter->function->parameters[p];
        size_t register_index = parameter_register_index(emitter->function,
                                                         emitter->target, physical);
        if (register_index == IR_VALUE_NONE) {
            size_t first_stack_offset = emitter->target == TARGET_COFF ? 48U : 16U;
            write_positive_frame_load(emitter, "rax", first_stack_offset +
                                                      stack_parameter_index(
                                                          emitter->function, emitter->target, physical) * 8U);
        } else if (physical_is_floating(emitter->function, physical)) {
            char source[32];
            (void) snprintf(source, sizeof(source), "xmm%zu", register_index);
            write_x64_2(emitter, parameter->type == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ,
                        X64_WIDTH_NONE,
                        x64_register(parameter->type == TYPE_FLOAT ? "eax" : "rax"),
                        x64_register(source));
        } else {
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"),
                        x64_register(argument_register(emitter->target, register_index)));
        }
        if (!is_length && is_integral(parameter->type) &&
            parameter->pointer_depth == 0 && !parameter->is_array && !parameter->is_slice)
            normalize_integral_parameter(emitter, parameter->type);
        if (parameter->is_slice) {
            if (!is_length) {
                size_t offset = parameter_copy_offset(emitter, p);
                write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                            x64_memory(X64_WIDTH_NONE, "rbp", -(long long) offset));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbx", 0),
                            x64_register("rax"));
                write_register_move(emitter, "rax", "rbx");
            } else {
                write_local_load(emitter, "rbx", parameter_offset(emitter, parameter));
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD, x64_memory(X64_WIDTH_QWORD, "rbx", 8),
                            x64_register("rax"));
            }
        } else if (!is_length && (type_is_structure(emitter->module, parameter->type_id) ||
                   (emitter->function->is_async && emitter->module->types[parameter->type_id].kind == IR_TYPE_ARRAY))) {
            size_t copy_offset = parameter_copy_offset(emitter, p);
            size_t copy_slots = type_slots(emitter->module, parameter->type_id);
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) copy_offset));
            if (emitter->function->is_async) {
                if (!copy_async_storage(emitter, parameter->type_id, "rax", "rbx")) return 0;
            } else copy_aggregate(emitter, copy_slots, "rax", "rbx");
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rbx"));
        }
        write_local_store(emitter, "rax", is_length
                                              ? slice_length_offset(emitter, p)
                                              : parameter_offset(emitter, parameter));
    }
    for (size_t p = 0; p < emitter->function->parameter_count; p++) {
        const IrParameter *parameter = &emitter->function->parameters[p];
        if (parameter->is_receiver ||
            !(ir_type_properties(emitter->module, parameter->type_id) &
              SEMANTIC_TYPE_NEEDS_DROP))
            continue;
        write_immediate(emitter, "rax", 1);
        write_local_store(emitter, "rax",
                          parameter_flag_offset(emitter, parameter));
    }
    if (emitter->function->is_async && !async_construct_and_poll(emitter)) return 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        if (emitter->source_map != NULL) {
            emitter->source_map->current_ir_instruction = i;
            emitter->source_map->current_span = emitter->function->instructions[i].span;
            emitter->source_map->current_program =
                emitter->function->instructions[i].source_program != NULL
                    ? emitter->function->instructions[i].source_program
                    : emitter->function->source_program;
            emitter->source_map->has_source = emitter->function->instructions[i].span.begin.line > 0;
        }
        if (emit_fused_compare_branch(emitter, i)) {
            i++;
            if (emitter->native != NULL && emitter->native->failed) return 0;
            continue;
        }
        if (store_target_needs_no_value(emitter, i)) continue;
        if (!emit_instruction(emitter, &emitter->function->instructions[i], i) ||
            (emitter->native != NULL && emitter->native->failed)) {
            ir_report_failure(emitter->function, i, "x86-64 lowering",
                              emitter->native != NULL && emitter->native->failed
                                  ? emitter->native->error
                                  : "unsupported instruction or operand");
            return 0;
        }
    }
    if (emitter->source_map != NULL) emitter->source_map->has_source = 0;
    write_immediate(emitter, "rax", 0);
    if (emitter->function->is_async) {
        async_complete(emitter);
        char epilogue[80];
        (void) snprintf(epilogue, sizeof(epilogue), ".LIR_epilogue_%zu", emitter->function_index);
        write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(epilogue));
        write_labelf(emitter, ".LIR_async_pending_%zu:\n", emitter->function_index);
        write_immediate(emitter, "rax", 0);
    }
    write_labelf(emitter, ".LIR_epilogue_%zu:\n", emitter->function_index);
    emitter->async_storage = 0;
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_memory(X64_WIDTH_NONE, "rbp", emitter->function->is_async ? -16 : -8));
    if (emitter->function->is_async)
        write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("r14"));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);
    if (emitter->native) {
        size_t symbol = native_symbol(emitter->native, name);
        emitter->native->symbols[symbol].size = emitter->native->sections[NATIVE_TEXT].size - emitter->native->symbols[
                                                    symbol].offset;
    } else if (emitter->target == TARGET_ELF) fprintf(output, "    .size %s, .-%s\n", name, name);
    return 1;
}
