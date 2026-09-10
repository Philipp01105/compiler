#include "ir_emitter.h"
#include "instruction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

typedef struct {
    FILE *output;
    size_t instruction_index;
    size_t current_ir_instruction;
    AstSourceSpan current_span;
    int has_source;
    int failed;
} SourceMapWriter;

typedef struct {
    const IrModule *module;
    const IrFunction *function;
    TargetFormat target;
    SyntaxMode syntax;
    FILE *output;
    size_t function_index;
    size_t declaration_count;
    size_t frame_size;
    size_t current_label;
    size_t bounds_sequence;
    SourceMapWriter *source_map;
} Emitter;

static int map_quoted(FILE *output, const char *text) {
    if (fputc('"', output) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *) (text == NULL ? "" : text); *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', output) == EOF || fputc(*p, output) == EOF) return 0;
        } else if (*p == '\n') {
            if (fputs("\\n", output) == EOF) return 0;
        } else if (*p == '\r') {
            if (fputs("\\r", output) == EOF) return 0;
        } else if (*p == '\t') {
            if (fputs("\\t", output) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(output, "\\x%02x", *p) < 0) return 0;
        } else if (fputc(*p, output) == EOF) return 0;
    }
    return fputc('"', output) != EOF;
}

static void write_x64(const Emitter *emitter, X64Instruction instruction) {
    SourceMapWriter *map = emitter->source_map;
    if (map != NULL) {
        if (map->has_source)
            instruction = x64_instruction_with_source(instruction, map->current_ir_instruction,
                map->current_span.begin.line, map->current_span.begin.column,
                map->current_span.end.line, map->current_span.end.column);
        if (fprintf(map->output, "instruction %zu function=", map->instruction_index) < 0 ||
            !map_quoted(map->output, emitter->function == NULL ? "" :
                ast_program_lexeme(emitter->function->source_program, emitter->function->name_token)) ||
            fputs(" source=", map->output) == EOF ||
            !map_quoted(map->output, emitter->function == NULL ? "" :
                emitter->function->source_program->source_path)) {
            map->failed = 1;
        } else if (instruction.has_source) {
            if (fprintf(map->output, " ir=%zu span=%d:%d-%d:%d\n", instruction.ir_instruction,
                    instruction.source_begin_line, instruction.source_begin_column,
                    instruction.source_end_line, instruction.source_end_column) < 0) map->failed = 1;
        } else if (fputs(" ir=- span=-\n", map->output) == EOF) map->failed = 1;
        map->instruction_index++;
    }
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_x64_0(const Emitter *emitter, X64Opcode opcode) {
    write_x64(emitter, x64_instruction0(opcode, X64_WIDTH_NONE));
}

static void write_x64_1(const Emitter *emitter, X64Opcode opcode, X64Width width,
                        X64Operand operand) {
    write_x64(emitter, x64_instruction1(opcode, width, operand));
}

static void write_x64_2(const Emitter *emitter, X64Opcode opcode, X64Width width,
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

static int is_integral(DataType type) {
    return type == TYPE_INT || type == TYPE_CHAR || type == TYPE_BYTE ||
           type == TYPE_BIT;
}

static int is_floating(DataType type) {
    return type == TYPE_FLOAT || type == TYPE_DOUBLE;
}

static int is_numeric(DataType type) {
    return is_integral(type) || is_floating(type);
}

static int is_pointer_value(const IrInstruction *instruction) {
    return instruction != NULL &&
           (instruction->pointer_depth != 0 || instruction->is_array);
}

static int type_is_structure(const IrModule *module, IrTypeId type_id) {
    if (type_id >= module->type_count || module->types[type_id].kind != IR_TYPE_NAMED)
        return 0;
    size_t symbol_id = module->types[type_id].symbol_id;
    return symbol_id < module->semantics->symbol_count &&
           module->semantics->symbols[symbol_id].kind == SEMANTIC_SYMBOL_STRUCT;
}

static int is_inline_structure(const IrModule *module,
                               const IrInstruction *instruction) {
    return instruction != NULL && type_is_structure(module, instruction->type_id);
}

static DataType ir_ast_type(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->name_token >= program->token_count) return TYPE_UNKNOWN;
    switch (program->tokens[type->name_token].type) {
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
        case TOKEN_TYPE_FLOAT: return TYPE_FLOAT;
        case TOKEN_TYPE_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_TYPE_STRING: return TYPE_STRING;
        case TOKEN_TYPE_VOID: return TYPE_VOID;
        default: return TYPE_UNKNOWN;
    }
}

static const IrInstruction *producer(const IrFunction *function, size_t value) {
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

static const IrFunction *called_function(const IrModule *module, size_t symbol_id) {
    for (size_t i = 0; i < module->function_count; i++)
        if (module->functions[i].symbol_id == symbol_id) return &module->functions[i];
    return NULL;
}

static const IrParameter *function_receiver(const IrFunction *function) {
    return function != NULL && function->parameter_count != 0 &&
           function->parameters[0].is_receiver ? &function->parameters[0] : NULL;
}

static int runtime_link_name(const char *name) {
    static const char *runtime_names[] = {
        "printf", "putchar", "puts", "strcmp", "strcpy", "strcat", "strdup",
        "_strdup", "malloc", "calloc", "free", "strlen", "strtoll", "scanf",
        "snprintf", "fflush", "read", "_read", "_write", "_open", "_close"
    };
    for (size_t i = 0; i < sizeof(runtime_names) / sizeof(runtime_names[0]); i++)
        if (strcmp(name, runtime_names[i]) == 0) return 1;
    return 0;
}

static const char *function_link_name(const IrFunction *function, char *buffer,
                                      size_t buffer_size) {
    const char *name = ast_program_lexeme(function->source_program, function->name_token);
    if (function->owner_token == AST_TOKEN_NONE) {
        if (!runtime_link_name(name)) return name;
        (void) snprintf(buffer, buffer_size, "__dmm_function_%zu_%s", strlen(name), name);
        return buffer;
    }
    const char *owner = ast_program_lexeme(function->source_program, function->owner_token);
    (void) snprintf(buffer, buffer_size, "__dmm_method_%zu_%s_%s",
                    strlen(owner), owner, name);
    return buffer;
}

static int valid_module(const IrModule *module) {
    if (module == NULL || !module->verified || module->program == NULL ||
        module->semantics == NULL || module->function_count == 0) return 0;
    const SemanticSymbol *main_symbol = semantic_find_global(module->semantics, "main",
                                                              SEMANTIC_SYMBOL_FUNCTION);
    return main_symbol != NULL && main_symbol->declaration != NULL &&
           main_symbol->declaration->as.function.parameters == NULL;
}

static size_t value_offset(size_t value) {
    return (value + 2U) * 8U;
}

static size_t type_slots_depth(const IrModule *module, IrTypeId type_id, size_t depth) {
    if (type_id >= module->type_count || depth > module->type_count) return 0;
    const IrType *type = &module->types[type_id];
    if (type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_PRIMITIVE) return 1;
    if (type->kind == IR_TYPE_ARRAY) {
        size_t element = type_slots_depth(module, type->element_type, depth + 1U);
        if (element == 0 || type->array_length > SIZE_MAX / element) return 0;
        return type->array_length * element;
    }
    for (size_t s = 0; s < module->structure_count; s++) {
        if (module->structures[s].symbol_id != type->symbol_id) continue;
        size_t slots = 0;
        for (size_t f = 0; f < module->structures[s].field_count; f++) {
            size_t field = type_slots_depth(module, module->structures[s].fields[f].type_id,
                                            depth + 1U);
            if (field == 0 || slots > SIZE_MAX - field) return 0;
            slots += field;
        }
        return slots == 0 ? 1 : slots;
    }
    for (size_t e = 0; e < module->enum_count; e++) {
        if (module->enums[e].symbol_id == type->symbol_id) return 1;
    }
    return 1;
}

static size_t type_slots(const IrModule *module, IrTypeId type_id) {
    return type_slots_depth(module, type_id, 0);
}

static size_t declaration_slots(const Emitter *emitter,
                                const IrInstruction *declaration) {
    size_t slots = type_slots(emitter->module, declaration->type_id);
    return slots;
}

static size_t declaration_offset(const Emitter *emitter,
                                 const IrInstruction *declaration) {
    size_t slots_before = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *candidate = &emitter->function->instructions[i];
        if (candidate->opcode != IR_OP_DECLARE) continue;
        if (candidate == declaration)
            return (emitter->function->next_value + emitter->function->parameter_count +
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
            case '\n': fputs("\\n", output); break;
            case '\r': fputs("\\r", output); break;
            case '\t': fputs("\\t", output); break;
            case '\\': fputs("\\\\", output); break;
            case '"': fputs("\\\"", output); break;
            default:
                if (c >= 32U && c < 127U) fputc((int) c, output);
                else fprintf(output, "\\%03o", (unsigned) c);
                break;
        }
    }
}

static void write_value_load(const Emitter *emitter, const char *reg, size_t value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_register(reg), x64_memory(X64_WIDTH_QWORD, "rbp",
                                      -(long long) value_offset(value)));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_value_store(const Emitter *emitter, const char *reg, size_t value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) value_offset(value)),
        x64_register(reg));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_local_load(const Emitter *emitter, const char *reg, size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_register(reg), x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_local_store(const Emitter *emitter, const char *reg, size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_memory(X64_WIDTH_QWORD, "rbp", -(long long) offset), x64_register(reg));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_immediate(const Emitter *emitter, const char *reg, long long value) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_register(reg), x64_immediate(value));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_address(const Emitter *emitter, const char *reg, const char *label,
                          size_t suffix) {
    X64Instruction instruction = x64_instruction2(X64_OP_LEA, X64_WIDTH_QWORD,
        x64_register(reg), x64_rip_memory(X64_WIDTH_NONE, label, suffix));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_call(const Emitter *emitter, const char *name) {
    if (emitter->target == TARGET_COFF) {
        X64Instruction stack = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
            x64_register("rsp"), x64_immediate(32));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &stack);
    }
    X64Instruction call = x64_instruction1(X64_OP_CALL, X64_WIDTH_NONE,
                                            x64_label(name));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &call);
    if (emitter->target == TARGET_COFF) {
        X64Instruction stack = x64_instruction2(X64_OP_ADD, X64_WIDTH_QWORD,
            x64_register("rsp"), x64_immediate(32));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &stack);
    }
}

static const char *argument_register(TargetFormat target, size_t index) {
    static const char *sysv[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    static const char *coff[] = {"rcx", "rdx", "r8", "r9"};
    return target == TARGET_COFF ? coff[index] : sysv[index];
}

static size_t parameter_register_index(const IrFunction *function, TargetFormat target,
                                       size_t parameter_index) {
    if (target == TARGET_COFF) return parameter_index < 4 ? parameter_index : IR_VALUE_NONE;
    size_t class_index = 0;
    int floating = is_floating(function->parameters[parameter_index].type);
    for (size_t i = 0; i < parameter_index; i++)
        if (is_floating(function->parameters[i].type) == floating) class_index++;
    size_t limit = floating ? 8U : 6U;
    return class_index < limit ? class_index : IR_VALUE_NONE;
}

static size_t stack_parameter_index(const IrFunction *function, TargetFormat target,
                                    size_t parameter_index) {
    size_t stack_index = 0;
    for (size_t i = 0; i < parameter_index; i++)
        if (parameter_register_index(function, target, i) == IR_VALUE_NONE) stack_index++;
    return stack_index;
}

static size_t stack_parameter_count(const IrFunction *function, TargetFormat target) {
    size_t count = 0;
    for (size_t i = 0; i < function->parameter_count; i++)
        if (parameter_register_index(function, target, i) == IR_VALUE_NONE) count++;
    return count;
}

static void write_positive_frame_load(const Emitter *emitter, const char *reg,
                                      size_t offset) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_register(reg), x64_memory(X64_WIDTH_QWORD, "rbp", (long long) offset));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void normalize_integral_parameter(const Emitter *emitter, DataType type) {
    if (type == TYPE_INT) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVSX, X64_WIDTH_QWORD,
            x64_sized_register(X64_WIDTH_QWORD, "rax"),
            x64_sized_register(X64_WIDTH_DWORD, "eax"));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    } else if (type == TYPE_CHAR) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVSX, X64_WIDTH_QWORD,
            x64_sized_register(X64_WIDTH_QWORD, "rax"),
            x64_sized_register(X64_WIDTH_BYTE, "al"));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    } else if (type == TYPE_BYTE) {
        X64Instruction instruction = x64_instruction2(X64_OP_MOVZX, X64_WIDTH_DWORD,
            x64_sized_register(X64_WIDTH_DWORD, "eax"),
            x64_sized_register(X64_WIDTH_BYTE, "al"));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    } else if (type == TYPE_BIT) {
        X64Instruction instruction = x64_instruction2(X64_OP_AND, X64_WIDTH_DWORD,
            x64_sized_register(X64_WIDTH_DWORD, "eax"), x64_immediate(1));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    }
}

static long long constant_value(const AstProgram *program,
                                const IrInstruction *instruction) {
    const AstToken *token = ast_program_token(program, instruction->auxiliary_token);
    if (token == NULL) return 0;
    if (token->type == TOKEN_CHAR_LITERAL) return (unsigned char) token->lexeme[0];
    if (strcmp(token->lexeme, "true") == 0) return 1;
    if (strcmp(token->lexeme, "false") == 0) return 0;
    return strtoll(token->lexeme, NULL, 10);
}

static void load_nullable_string(Emitter *emitter, const char *reg, size_t value);

static int emit_string_compare(Emitter *emitter, const IrInstruction *instruction) {
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
    fprintf(emitter->output, ".LIR_string_equal_%zu_%zu:\n",
            emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 1 : 0);
    write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(done_label));
    fprintf(emitter->output, ".LIR_string_unequal_%zu_%zu:\n",
            emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 0 : 1);
    fprintf(emitter->output, ".LIR_string_done_%zu_%zu:\n",
            emitter->function_index, instruction->result);
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static void convert_rax(Emitter *emitter, DataType from, DataType to);
static size_t aggregate_result_offset(const Emitter *emitter,
                                      const IrInstruction *result);
static void copy_aggregate(Emitter *emitter, size_t slots,
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
        fprintf(emitter->output, ".LIR_concat_value_%zu_%zu_%u:\n",
                emitter->function_index, result_id, operand_index);
        return;
    }

    const char *destination = emitter->target == TARGET_COFF ? "rcx" : "rdi";
    const char *capacity = emitter->target == TARGET_COFF ? "rdx" : "rsi";
    const char *format = emitter->target == TARGET_COFF ? "r8" : "rdx";
    write_stack_address(emitter, destination, buffer_offset);
    write_immediate(emitter, capacity, 64);
    write_address(emitter, format,
                  value->type == TYPE_CHAR ? ".LIR_char_format_" :
                  is_floating(value->type) ? ".LIR_float_format_" :
                                             ".LIR_int_format_", 0);
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

static int emit_string_concat(Emitter *emitter, const IrInstruction *instruction) {
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

static void load_floating_value(Emitter *emitter, size_t value, DataType target,
                                unsigned xmm) {
    const IrInstruction *source = producer(emitter->function, value);
    write_value_load(emitter, "rax", value);
    convert_rax(emitter, source->type, target);
    char destination[16];
    (void) snprintf(destination, sizeof(destination), "xmm%u", xmm);
    write_x64_2(emitter, target == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ,
                X64_WIDTH_NONE, x64_register(destination),
                x64_register(target == TYPE_FLOAT ? "eax" : "rax"));
}

static void store_floating_result(Emitter *emitter, DataType type, unsigned xmm,
                                  size_t result) {
    char source[16];
    (void) snprintf(source, sizeof(source), "xmm%u", xmm);
    write_x64_2(emitter, type == TYPE_FLOAT ? X64_OP_MOVD : X64_OP_MOVQ,
                X64_WIDTH_NONE, x64_register(type == TYPE_FLOAT ? "eax" : "rax"),
                x64_register(source));
    write_value_store(emitter, "rax", result);
}

static int emit_floating_binary(Emitter *emitter, const IrInstruction *instruction,
                                DataType operation_type) {
    load_floating_value(emitter, instruction->operand_a, operation_type, 2);
    load_floating_value(emitter, instruction->operand_b, operation_type, 1);
    if (instruction->operator_type >= TOKEN_PLUS &&
        instruction->operator_type <= TOKEN_SLASH) {
        static const X64Opcode float_operations[] = {
            X64_OP_ADDSS, X64_OP_SUBSS, X64_OP_MULSS, X64_OP_DIVSS
        };
        static const X64Opcode double_operations[] = {
            X64_OP_ADDSD, X64_OP_SUBSD, X64_OP_MULSD, X64_OP_DIVSD
        };
        size_t operation = (size_t) (instruction->operator_type - TOKEN_PLUS);
        write_x64_2(emitter, operation_type == TYPE_FLOAT
                    ? float_operations[operation] : double_operations[operation],
                    X64_WIDTH_NONE, x64_register("xmm2"), x64_register("xmm1"));
        store_floating_result(emitter, instruction->type, 2, instruction->result);
        return 1;
    }
    if (instruction->operator_type < TOKEN_EQUAL_EQUAL ||
        instruction->operator_type > TOKEN_GREATER_EQUAL) return 0;
    write_x64_2(emitter, operation_type == TYPE_FLOAT ? X64_OP_UCOMISS : X64_OP_UCOMISD,
                X64_WIDTH_NONE, x64_register("xmm2"), x64_register("xmm1"));
    X64Opcode condition = X64_OP_SETE;
    int require_ordered = 0;
    int include_unordered = 0;
    if (instruction->operator_type == TOKEN_BANG_EQUAL) {
        condition = X64_OP_SETNE;
        include_unordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS) {
        condition = X64_OP_SETB;
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS_EQUAL) {
        condition = X64_OP_SETBE;
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_GREATER) condition = X64_OP_SETA;
    else if (instruction->operator_type == TOKEN_GREATER_EQUAL) condition = X64_OP_SETAE;
    else require_ordered = 1;
    write_x64_1(emitter, condition, X64_WIDTH_NONE, x64_register("al"));
    if (require_ordered || include_unordered) {
        write_x64_1(emitter, include_unordered ? X64_OP_SETP : X64_OP_SETNP,
                    X64_WIDTH_NONE, x64_register("dl"));
        write_x64_2(emitter, include_unordered ? X64_OP_OR : X64_OP_AND,
                    X64_WIDTH_BYTE, x64_register("al"), x64_register("dl"));
    }
    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                x64_sized_register(X64_WIDTH_BYTE, "al"));
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static int emit_typed_call(Emitter *emitter, const IrInstruction *instruction,
                           const IrFunction *callee) {
    const IrFunction *caller = emitter->function;
    size_t stack_count = stack_parameter_count(callee, emitter->target);
    if ((stack_count & 1U) != 0)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(8));
    for (size_t a = instruction->argument_count; a-- > 0;) {
        if (parameter_register_index(callee, emitter->target, a) != IR_VALUE_NONE) continue;
        size_t value_id = caller->arguments[instruction->first_argument + a];
        const IrInstruction *value = producer(caller, value_id);
        write_value_load(emitter, "rax", value_id);
        convert_rax(emitter, value->type, callee->parameters[a].type);
        write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rax"));
    }
    if (emitter->target == TARGET_COFF)
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(32));

    for (size_t a = instruction->argument_count; a-- > 0;) {
        size_t register_index = parameter_register_index(callee, emitter->target, a);
        if (register_index == IR_VALUE_NONE || !is_floating(callee->parameters[a].type)) continue;
        size_t value_id = caller->arguments[instruction->first_argument + a];
        load_floating_value(emitter, value_id, callee->parameters[a].type,
                            (unsigned) register_index);
    }
    for (size_t a = 0; a < instruction->argument_count; a++) {
        size_t register_index = parameter_register_index(callee, emitter->target, a);
        if (register_index == IR_VALUE_NONE || is_floating(callee->parameters[a].type)) continue;
        size_t value_id = caller->arguments[instruction->first_argument + a];
        const IrInstruction *value = producer(caller, value_id);
        write_value_load(emitter, "rax", value_id);
        convert_rax(emitter, value->type, callee->parameters[a].type);
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                    x64_register(argument_register(emitter->target, register_index)),
                    x64_register("rax"));
    }
    char callee_buffer[MAX_TOKEN * 2U + 64U];
    write_x64_1(emitter, X64_OP_CALL, X64_WIDTH_NONE,
                x64_label(function_link_name(callee, callee_buffer, sizeof(callee_buffer))));
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
        } else if (instruction->type == TYPE_FLOAT)
            write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                        x64_register("eax"), x64_register("xmm0"));
        else if (instruction->type == TYPE_DOUBLE)
            write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                        x64_register("rax"), x64_register("xmm0"));
        write_value_store(emitter, "rax", instruction->result);
    }
    return 1;
}

static void load_nullable_string(Emitter *emitter, const char *reg, size_t value) {
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
    fprintf(emitter->output, ".LIR_nonnull_%zu_%zu:\n",
            emitter->function_index, label);
}

static int emit_builtin_call(Emitter *emitter, const IrInstruction *instruction,
                             const char *name) {
    const IrFunction *function = emitter->function;
    if (strcmp(name, "io_strlen") == 0 || strcmp(name, "strlen") == 0) {
        size_t value = function->arguments[instruction->first_argument];
        load_nullable_string(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                             value);
        write_call(emitter, "strlen");
    } else if (strcmp(name, "strcmp") == 0 || strcmp(name, "strcpy") == 0 ||
               strcmp(name, "strcat") == 0) {
        static const char *coff_registers[] = {"rcx", "rdx"};
        static const char *elf_registers[] = {"rdi", "rsi"};
        const char **registers = emitter->target == TARGET_COFF
            ? coff_registers : elf_registers;
        for (size_t a = 0; a < 2; a++) {
            size_t value = function->arguments[instruction->first_argument + a];
            if (strcmp(name, "strcmp") == 0 || a == 1)
                load_nullable_string(emitter, registers[a], value);
            else
                write_value_load(emitter, registers[a], value);
        }
        write_call(emitter, name);
    } else if (strcmp(name, "strdup") == 0) {
        load_nullable_string(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
            function->arguments[instruction->first_argument]);
        write_call(emitter, emitter->target == TARGET_COFF ? "_strdup" : "strdup");
    } else if (strcmp(name, "malloc") == 0) {
        write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
            function->arguments[instruction->first_argument]);
        write_call(emitter, "malloc");
    } else if (strcmp(name, "scanfInt") == 0 || strcmp(name, "scanfChar") == 0) {
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(16));
        write_address(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                      strcmp(name, "scanfInt") == 0 ? ".LIR_scan_int_format_" :
                                                       ".LIR_scan_char_format_", 0);
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                    x64_register(emitter->target == TARGET_COFF ? "rdx" : "rsi"),
                    x64_memory(X64_WIDTH_NONE, "rsp", 0));
        write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                    x64_register("eax"), x64_register("eax"));
        write_call(emitter, "scanf");
        X64Width source_width = strcmp(name, "scanfInt") == 0
            ? X64_WIDTH_DWORD : X64_WIDTH_BYTE;
        X64Width destination_width = strcmp(name, "scanfInt") == 0
            ? X64_WIDTH_QWORD : X64_WIDTH_DWORD;
        write_x64_2(emitter, strcmp(name, "scanfInt") == 0 ? X64_OP_MOVSX : X64_OP_MOVZX,
                    destination_width,
                    x64_sized_register(destination_width,
                        destination_width == X64_WIDTH_QWORD ? "rax" : "eax"),
                    x64_memory(source_width, "rsp", 0));
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(16));
    } else if (strcmp(name, "scanfString") == 0) {
        write_address(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                      ".LIR_scan_string_format_", 0);
        write_address(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi",
                      ".LIR_scan_string_buffer_", 0);
        write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                    x64_register("eax"), x64_register("eax"));
        write_call(emitter, "scanf");
        write_address(emitter, "rax", ".LIR_scan_string_buffer_", 0);
    } else if (strcmp(name, "io_int_to_str") == 0) {
        size_t value = function->arguments[instruction->first_argument];
        size_t buffer = function->arguments[instruction->first_argument + 1U];
        size_t capacity = function->arguments[instruction->first_argument + 2U];
        if (emitter->target == TARGET_COFF) {
            write_value_load(emitter, "rcx", buffer);
            write_value_load(emitter, "rdx", capacity);
            write_address(emitter, "r8", ".LIR_int_format_", 0);
            write_value_load(emitter, "r9", value);
        } else {
            write_value_load(emitter, "rdi", buffer);
            write_value_load(emitter, "rsi", capacity);
            write_address(emitter, "rdx", ".LIR_int_format_", 0);
            write_value_load(emitter, "rcx", value);
        }
        write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                    x64_register("eax"), x64_register("eax"));
        write_call(emitter, "snprintf");
        write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", buffer);
        write_call(emitter, "strlen");
    } else if (strcmp(name, "io_str_to_int") == 0) {
        size_t value = function->arguments[instruction->first_argument];
        load_nullable_string(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", value);
        write_immediate(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi", 0);
        write_immediate(emitter, emitter->target == TARGET_COFF ? "r8" : "rdx", 10);
        write_call(emitter, "strtoll");
    } else if (strcmp(name, "read") == 0) {
        size_t descriptor = function->arguments[instruction->first_argument];
        const IrInstruction *format = producer(function,
            function->arguments[instruction->first_argument + 1U]);
        const char *format_text = format != NULL && format->opcode == IR_OP_CONSTANT
            ? ast_program_lexeme(function->source_program, format->auxiliary_token) : "%i";
        write_immediate(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", 0);
        write_call(emitter, "fflush");
        write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(64));
        write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", descriptor);
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                    x64_register(emitter->target == TARGET_COFF ? "rdx" : "rsi"),
                    x64_memory(X64_WIDTH_NONE, "rsp", 0));
        write_immediate(emitter, emitter->target == TARGET_COFF ? "r8" : "rdx", 63);
        write_call(emitter, emitter->target == TARGET_COFF ? "_read" : "read");
        char empty_label[64];
        char done_label[64];
        (void) snprintf(empty_label, sizeof(empty_label), ".LIR_read_empty_%zu_%zu",
                        emitter->function_index, instruction->result);
        (void) snprintf(done_label, sizeof(done_label), ".LIR_read_done_%zu_%zu",
                        emitter->function_index, instruction->result);
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_DWORD,
                    x64_register("eax"), x64_register("eax"));
        write_x64_1(emitter, X64_OP_JLE, X64_WIDTH_NONE, x64_label(empty_label));
        write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_BYTE,
                    x64_indexed_memory(X64_WIDTH_BYTE, "rsp", "rax", 1, 0),
                    x64_immediate(0));
        if (strcmp(format_text, "%c") == 0) {
            write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_DWORD,
                        x64_sized_register(X64_WIDTH_DWORD, "eax"),
                        x64_memory(X64_WIDTH_BYTE, "rsp", 0));
        } else {
            write_stack_address(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", 0);
            write_immediate(emitter, emitter->target == TARGET_COFF ? "rdx" : "rsi", 0);
            write_immediate(emitter, emitter->target == TARGET_COFF ? "r8" : "rdx", 10);
            write_call(emitter, "strtoll");
        }
        write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(done_label));
        fprintf(emitter->output, ".LIR_read_empty_%zu_%zu:\n",
                emitter->function_index, instruction->result);
        write_immediate(emitter, "rax", 0);
        fprintf(emitter->output, ".LIR_read_done_%zu_%zu:\n",
                emitter->function_index, instruction->result);
        write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                    x64_register("rsp"), x64_immediate(64));
    } else if (emitter->target == TARGET_ELF) {
        static const char *registers[] = {"rdi", "rsi", "rdx"};
        if (strcmp(name, "sys_write") == 0 || strcmp(name, "sys_read") == 0) {
            write_immediate(emitter, "rdi", 0);
            write_call(emitter, "fflush");
        }
        for (size_t a = 0; a < instruction->argument_count; a++)
            write_value_load(emitter, registers[a],
                function->arguments[instruction->first_argument + a]);
        long long syscall_number = strcmp(name, "sys_write") == 0 ? 1 :
                                   strcmp(name, "sys_open") == 0 ? 2 :
                                   strcmp(name, "sys_close") == 0 ? 3 : 0;
        write_immediate(emitter, "rax", syscall_number);
        write_x64_0(emitter, X64_OP_SYSCALL);
    } else {
        static const char *registers[] = {"rcx", "rdx", "r8"};
        if (strcmp(name, "sys_write") == 0 || strcmp(name, "sys_read") == 0) {
            write_immediate(emitter, "rcx", 0);
            write_call(emitter, "fflush");
        }
        for (size_t a = 0; a < instruction->argument_count; a++)
            write_value_load(emitter, registers[a],
                function->arguments[instruction->first_argument + a]);
        if (strcmp(name, "sys_open") == 0) {
            const char *kinds[] = {"create", "trunc", "append"};
            const long long source_flags[] = {64, 512, 1024};
            const long long target_flags[] = {256, 512, 8};
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_DWORD,
                        x64_register("r10d"), x64_register("edx"));
            write_x64_2(emitter, X64_OP_AND, X64_WIDTH_DWORD,
                        x64_register("edx"), x64_immediate(3));
            for (size_t flag = 0; flag < 3; flag++) {
                char skip_label[80];
                (void) snprintf(skip_label, sizeof(skip_label), ".LIR_open_no_%s_%zu_%zu",
                                kinds[flag], emitter->function_index, instruction->result);
                write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_DWORD,
                            x64_register("r10d"), x64_immediate(source_flags[flag]));
                write_x64_1(emitter, X64_OP_JE, X64_WIDTH_NONE, x64_label(skip_label));
                write_x64_2(emitter, X64_OP_OR, X64_WIDTH_DWORD,
                            x64_register("edx"), x64_immediate(target_flags[flag]));
                fprintf(emitter->output, ".LIR_open_no_%s_%zu_%zu:\n",
                        kinds[flag], emitter->function_index, instruction->result);
            }
            write_x64_2(emitter, X64_OP_OR, X64_WIDTH_DWORD,
                        x64_register("edx"), x64_immediate(32768));
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_DWORD,
                        x64_register("r8d"), x64_immediate(384));
        }
        const char *runtime_name = strcmp(name, "sys_write") == 0 ? "_write" :
                                   strcmp(name, "sys_read") == 0 ? "_read" :
                                   strcmp(name, "sys_open") == 0 ? "_open" : "_close";
        write_call(emitter, runtime_name);
    }
    if (instruction->result != IR_VALUE_NONE) {
        if (is_integral(instruction->type))
            normalize_integral_parameter(emitter, instruction->type);
        write_value_store(emitter, "rax", instruction->result);
    }
    return 1;
}

static size_t fixed_array_length(const Emitter *emitter,
                                 const IrInstruction *base) {
    if (base == NULL) return 0;
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
            ? (const AstField *) symbol->node : NULL;
        if (field != NULL && field->type.is_array &&
            field->type.array_length_token < symbol->source_program->token_count)
            return (size_t) strtoull(ast_program_lexeme(symbol->source_program,
                field->type.array_length_token), NULL, 10);
    }
    return 0;
}

static size_t aggregate_result_offset(const Emitter *emitter,
                                      const IrInstruction *result) {
    size_t slots_before = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *candidate = &emitter->function->instructions[i];
        if (candidate->opcode != IR_OP_CALL || !is_inline_structure(emitter->module,
                                                                    candidate))
            continue;
        size_t slots = type_slots(emitter->module, candidate->type_id);
        if (candidate == result)
            return (emitter->function->next_value + emitter->function->parameter_count +
                    emitter->declaration_count + slots_before + slots + 1U) * 8U;
        slots_before += slots;
    }
    return 0;
}

static size_t aggregate_result_slots(const Emitter *emitter) {
    size_t result = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *instruction = &emitter->function->instructions[i];
        if (instruction->opcode == IR_OP_CALL &&
            is_inline_structure(emitter->module, instruction))
            result += type_slots(emitter->module, instruction->type_id);
    }
    return result;
}

static size_t parameter_copy_offset(const Emitter *emitter, size_t parameter_index) {
    size_t slots_before = 0;
    for (size_t i = 0; i < parameter_index; i++)
        if (type_is_structure(emitter->module,
                              emitter->function->parameters[i].type_id))
            slots_before += type_slots(emitter->module,
                emitter->function->parameters[i].type_id);
    size_t slots = type_slots(emitter->module,
                              emitter->function->parameters[parameter_index].type_id);
    return (emitter->function->next_value + emitter->function->parameter_count +
            emitter->declaration_count + aggregate_result_slots(emitter) +
            slots_before + slots + 1U) * 8U;
}

static void copy_aggregate(Emitter *emitter, size_t slots,
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

static int emit_lvalue_address(Emitter *emitter, const IrInstruction *target,
                               size_t instruction_index) {
    if (target == NULL) return 0;
    if (target->opcode == IR_OP_LOAD) {
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
            size_t field_offset = aggregate == NULL ? SIZE_MAX :
                aggregate_field_offset(emitter, aggregate, target->symbol_id);
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
        size_t bounds_id = emitter->bounds_sequence++;
        if (length != 0) {
            char fail_label[64];
            (void) snprintf(fail_label, sizeof(fail_label), ".LIR_bounds_fail_%zu_%zu",
                            emitter->function_index, bounds_id);
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rcx"), x64_immediate(0));
            write_x64_1(emitter, X64_OP_JL, X64_WIDTH_NONE, x64_label(fail_label));
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rcx"), x64_immediate((long long) length));
            write_x64_1(emitter, X64_OP_JGE, X64_WIDTH_NONE, x64_label(fail_label));
        }
        size_t element_size = is_inline_structure(emitter->module, target)
                                  ? type_slots(emitter->module, target->type_id) * 8U :
                              target->pointer_depth != 0 ? 8U :
                              (target->type == TYPE_CHAR || target->type == TYPE_BYTE ||
                               target->type == TYPE_BIT) ? 1U :
                              (target->type == TYPE_INT || target->type == TYPE_FLOAT) ? 4U : 8U;
        write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                    x64_register("rcx"), x64_immediate((long long) element_size));
        write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                    x64_indexed_memory(X64_WIDTH_NONE, "rax", "rcx", 1, 0));
        if (length != 0) {
            char ok_label[64];
            (void) snprintf(ok_label, sizeof(ok_label), ".LIR_bounds_ok_%zu_%zu",
                            emitter->function_index, bounds_id);
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(ok_label));
            fprintf(emitter->output, ".LIR_bounds_fail_%zu_%zu:\n",
                    emitter->function_index, bounds_id);
            write_x64_0(emitter, X64_OP_UD2);
            fprintf(emitter->output, ".LIR_bounds_ok_%zu_%zu:\n",
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
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_indirect_store(const Emitter *emitter, const char *address,
                                 const char *source) {
    X64Instruction instruction = x64_instruction2(X64_OP_MOV, X64_WIDTH_QWORD,
        x64_memory(X64_WIDTH_QWORD, address, 0), x64_register(source));
    (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
}

static void write_typed_indirect_load(const Emitter *emitter, DataType type,
                                      unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_load(emitter, "rax", address);
    } else {
        X64Opcode opcode = type == TYPE_INT || type == TYPE_CHAR ? X64_OP_MOVSX :
                           type == TYPE_BYTE || type == TYPE_BIT ? X64_OP_MOVZX : X64_OP_MOV;
        X64Width source_width = type == TYPE_INT || type == TYPE_FLOAT
            ? X64_WIDTH_DWORD : X64_WIDTH_BYTE;
        X64Width destination_width = type == TYPE_INT || type == TYPE_CHAR
            ? X64_WIDTH_QWORD : X64_WIDTH_DWORD;
        X64Instruction instruction = x64_instruction2(opcode, destination_width,
            x64_sized_register(destination_width,
                destination_width == X64_WIDTH_QWORD ? "rax" : "eax"),
            x64_memory(source_width, address, 0));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    }
}

static void write_typed_indirect_store(const Emitter *emitter, DataType type,
                                       unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_store(emitter, address, "rax");
    } else {
        X64Width width = type == TYPE_INT || type == TYPE_FLOAT
            ? X64_WIDTH_DWORD : X64_WIDTH_BYTE;
        X64Instruction instruction = x64_instruction2(X64_OP_MOV, width,
            x64_memory(width, address, 0),
            x64_sized_register(width, width == X64_WIDTH_DWORD ? "eax" : "al"));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &instruction);
    }
}

static void write_frame_allocation(const Emitter *emitter) {
    size_t remaining = emitter->frame_size;
    while (remaining > 4096U) {
        X64Instruction subtract = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
            x64_register("rsp"), x64_immediate(4096));
        X64Instruction probe = x64_instruction2(X64_OP_MOV, X64_WIDTH_BYTE,
            x64_memory(X64_WIDTH_BYTE, "rsp", 0), x64_immediate(0));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &subtract);
        (void) x64_print_instruction(emitter->output, emitter->syntax, &probe);
        remaining -= 4096U;
    }
    if (remaining != 0) {
        X64Instruction subtract = x64_instruction2(X64_OP_SUB, X64_WIDTH_QWORD,
            x64_register("rsp"), x64_immediate((long long) remaining));
        (void) x64_print_instruction(emitter->output, emitter->syntax, &subtract);
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
        size_t field_index = enumeration == NULL ? IR_VALUE_NONE :
            enum_field_index(enumeration, instruction->symbol_id);
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
        union { double floating; uint64_t bits; } value = {strtod(text, NULL)};
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

static int emit_binary(Emitter *emitter, const IrInstruction *instruction) {
    const IrInstruction *left = producer(emitter->function, instruction->operand_a);
    const IrInstruction *right = producer(emitter->function, instruction->operand_b);
    if (left != NULL && right != NULL && instruction->type == TYPE_STRING &&
        instruction->operator_type == TOKEN_PLUS)
        return emit_string_concat(emitter, instruction);
    if (left != NULL && right != NULL && left->type == TYPE_STRING &&
        right->type == TYPE_STRING) {
        return emit_string_compare(emitter, instruction);
    }
    if (left != NULL && right != NULL &&
        (is_floating(left->type) || is_floating(right->type))) {
        DataType operation_type = is_floating(instruction->type)
            ? instruction->type
            : (left->type == TYPE_DOUBLE || right->type == TYPE_DOUBLE
                ? TYPE_DOUBLE : TYPE_FLOAT);
        return emit_floating_binary(emitter, instruction, operation_type);
    }
    write_value_load(emitter, "rax", instruction->operand_a);
    write_value_load(emitter, "rcx", instruction->operand_b);
    switch (instruction->operator_type) {
        case TOKEN_PLUS:
            write_x64_2(emitter, X64_OP_ADD, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_MINUS:
            write_x64_2(emitter, X64_OP_SUB, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_STAR:
            write_x64_2(emitter, X64_OP_IMUL, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            write_x64_0(emitter, X64_OP_CQO);
            write_x64_1(emitter, X64_OP_IDIV, X64_WIDTH_QWORD, x64_register("rcx"));
            if (instruction->operator_type == TOKEN_PERCENT)
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rdx"));
            break;
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL: {
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rcx"));
            X64Opcode condition = X64_OP_SETE;
            if (instruction->operator_type == TOKEN_BANG_EQUAL) condition = X64_OP_SETNE;
            else if (instruction->operator_type == TOKEN_LESS) condition = X64_OP_SETL;
            else if (instruction->operator_type == TOKEN_LESS_EQUAL) condition = X64_OP_SETLE;
            else if (instruction->operator_type == TOKEN_GREATER) condition = X64_OP_SETG;
            else if (instruction->operator_type == TOKEN_GREATER_EQUAL) condition = X64_OP_SETGE;
            write_x64_1(emitter, condition, X64_WIDTH_NONE, x64_register("al"));
            write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                        x64_sized_register(X64_WIDTH_QWORD, "rax"),
                        x64_sized_register(X64_WIDTH_BYTE, "al"));
            break;
        }
        default: return 0;
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

/* Values use an eight-byte virtual slot. Floating-point values are kept as
   their IEEE bit pattern so conversions happen only at typed IR boundaries. */
static void convert_rax(Emitter *emitter, DataType from, DataType to) {
    if (from == to || !is_numeric(from) || !is_numeric(to)) return;
    if (is_integral(from) && to == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_CVTSI2SDQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (is_integral(from) && to == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_CVTSI2SSQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("eax"), x64_register("xmm0"));
    } else if (from == TYPE_FLOAT && to == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_CVTSS2SD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm0"));
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (from == TYPE_DOUBLE && to == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_CVTSD2SS, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm0"));
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("eax"), x64_register("xmm0"));
    } else if (from == TYPE_FLOAT && is_integral(to)) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_CVTTSS2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    } else if (from == TYPE_DOUBLE && is_integral(to)) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_CVTTSD2SIQ, X64_WIDTH_NONE,
                    x64_register("rax"), x64_register("xmm0"));
    }
}

static void normalize_truth_rax(Emitter *emitter, DataType type) {
    if (type == TYPE_FLOAT) {
        write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("eax"));
        write_x64_2(emitter, X64_OP_XORPS, X64_WIDTH_NONE,
                    x64_register("xmm1"), x64_register("xmm1"));
        write_x64_2(emitter, X64_OP_UCOMISS, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm1"));
    } else if (type == TYPE_DOUBLE) {
        write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("rax"));
        write_x64_2(emitter, X64_OP_XORPD, X64_WIDTH_NONE,
                    x64_register("xmm1"), x64_register("xmm1"));
        write_x64_2(emitter, X64_OP_UCOMISD, X64_WIDTH_NONE,
                    x64_register("xmm0"), x64_register("xmm1"));
    } else {
        write_x64_2(emitter, X64_OP_TEST, X64_WIDTH_QWORD,
                    x64_register("rax"), x64_register("rax"));
        write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
        write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                    x64_sized_register(X64_WIDTH_QWORD, "rax"),
                    x64_sized_register(X64_WIDTH_BYTE, "al"));
        return;
    }
    write_x64_1(emitter, X64_OP_SETNE, X64_WIDTH_NONE, x64_register("al"));
    write_x64_1(emitter, X64_OP_SETP, X64_WIDTH_NONE, x64_register("dl"));
    write_x64_2(emitter, X64_OP_OR, X64_WIDTH_BYTE,
                x64_register("al"), x64_register("dl"));
    write_x64_2(emitter, X64_OP_MOVZX, X64_WIDTH_QWORD,
                x64_sized_register(X64_WIDTH_QWORD, "rax"),
                x64_sized_register(X64_WIDTH_BYTE, "al"));
}

static void emit_gc_cleanup(Emitter *emitter) {
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *declaration = &emitter->function->instructions[i];
        if (declaration->opcode != IR_OP_DECLARE || !declaration->is_gc) continue;
        write_local_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                         declaration_offset(emitter, declaration));
        write_call(emitter, "free");
    }
}

static int emit_instruction(Emitter *emitter, const IrInstruction *instruction,
                            size_t index) {
    const IrFunction *function = emitter->function;
    switch (instruction->opcode) {
        case IR_OP_CONSTANT:
            if (instruction->type == TYPE_STRING) {
                char string_label[64];
                (void) snprintf(string_label, sizeof(string_label), ".LIR_string_%zu_",
                                emitter->function_index);
                write_address(emitter, "rax", string_label, instruction->result);
                write_value_store(emitter, "rax", instruction->result);
            } else {
                if (is_floating(instruction->type)) {
                    const char *text = ast_program_lexeme(function->source_program,
                                                          instruction->auxiliary_token);
                    union { double floating; uint64_t bits; } value = {strtod(text, NULL)};
                    write_immediate(emitter, "rax", (long long) value.bits);
                } else {
                    write_immediate(emitter, "rax", constant_value(function->source_program,
                                                                    instruction));
                }
                write_value_store(emitter, "rax", instruction->result);
            }
            return 1;
        case IR_OP_LOAD: {
            const char *name = ast_program_lexeme(function->source_program,
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
                else if (emit_lvalue_address(emitter, instruction, index))
                    write_typed_indirect_load(emitter, instruction->type,
                                              instruction->pointer_depth, "rbx");
                else write_immediate(emitter, "rax", 0);
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_DECLARE: {
            size_t offset = declaration_offset(emitter, instruction);
            if (instruction->is_array || is_inline_structure(emitter->module, instruction)) {
                size_t slots = declaration_slots(emitter, instruction);
                if (instruction->operand_a != IR_VALUE_NONE &&
                    is_inline_structure(emitter->module, instruction)) {
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
                return 1;
            }
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else {
                const IrInstruction *value = producer(function, instruction->operand_a);
                write_value_load(emitter, "rax", instruction->operand_a);
                convert_rax(emitter, value->type, instruction->type);
            }
            write_local_store(emitter, "rax", offset);
            return 1;
        }
        case IR_OP_STORE: {
            const IrInstruction *target = producer(function, instruction->operand_a);
            if (!emit_lvalue_address(emitter, target, index)) return 0;
            if (instruction->operator_type == TOKEN_EQUAL &&
                is_inline_structure(emitter->module, target)) {
                write_value_load(emitter, "rax", instruction->operand_b);
                copy_aggregate(emitter, type_slots(emitter->module, target->type_id),
                               "rax", "rbx");
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
                                ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register("xmm2"),
                                x64_register(target->type == TYPE_FLOAT ? "eax" : "rax"));
                    if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                        instruction->operator_type == TOKEN_MINUS_MINUS) {
                        write_immediate(emitter, "rax", 1);
                        convert_rax(emitter, TYPE_INT, target->type);
                        write_x64_2(emitter, target->type == TYPE_FLOAT
                                    ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE,
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
                        ? (add ? X64_OP_ADDSS : subtract ? X64_OP_SUBSS :
                           instruction->operator_type == TOKEN_STAR_EQUAL
                               ? X64_OP_MULSS : X64_OP_DIVSS)
                        : (add ? X64_OP_ADDSD : subtract ? X64_OP_SUBSD :
                           instruction->operator_type == TOKEN_STAR_EQUAL
                               ? X64_OP_MULSD : X64_OP_DIVSD);
                    write_x64_2(emitter, operation, X64_WIDTH_NONE,
                                x64_register("xmm2"), x64_register("xmm1"));
                    write_x64_2(emitter, target->type == TYPE_FLOAT
                                ? X64_OP_MOVD : X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register(target->type == TYPE_FLOAT ? "eax" : "rax"),
                                x64_register("xmm2"));
                }
            } else if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                instruction->operator_type == TOKEN_MINUS_MINUS) {
                write_x64_1(emitter, instruction->operator_type == TOKEN_PLUS_PLUS
                            ? X64_OP_INC : X64_OP_DEC, X64_WIDTH_QWORD,
                            x64_register("rax"));
            } else {
                write_value_load(emitter, "rcx", instruction->operand_b);
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
                    write_x64_0(emitter, X64_OP_CQO);
                    write_x64_1(emitter, X64_OP_IDIV, X64_WIDTH_QWORD,
                                x64_register("rcx"));
                } else return 0;
            }
            write_typed_indirect_store(emitter, target->type, target->pointer_depth, "rbx");
            return 1;
        }
        case IR_OP_UNARY:
        {
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
                write_typed_indirect_load(emitter, instruction->type,
                                          instruction->pointer_depth, "rax");
                write_value_store(emitter, "rax", instruction->result);
                return 1;
            }
            if (instruction->operator_type == TOKEN_MINUS) {
                if (instruction->type == TYPE_FLOAT)
                    write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                                x64_register("eax"), x64_immediate(0x80000000LL));
                else if (instruction->type == TYPE_DOUBLE)
                {
                    write_x64_2(emitter, X64_OP_MOVABS, X64_WIDTH_QWORD,
                                x64_register("rcx"), x64_immediate((long long) UINT64_C(0x8000000000000000)));
                    write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_QWORD,
                                x64_register("rax"), x64_register("rcx"));
                }
                else
                    write_x64_1(emitter, X64_OP_NEG, X64_WIDTH_QWORD,
                                x64_register("rax"));
            }
            else {
                normalize_truth_rax(emitter, operand->type);
                write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_immediate(1));
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_BINARY:
            return emit_binary(emitter, instruction);
        case IR_OP_PRINT: {
            const IrInstruction *value = producer(function, instruction->operand_a);
            const char *first = emitter->target == TARGET_COFF ? "rcx" : "rdi";
            const char *second = emitter->target == TARGET_COFF ? "rdx" : "rsi";
            if (value == NULL) {
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_immediate(emitter, first, 10);
                    write_call(emitter, "putchar");
                }
                return 1;
            }
            if (value->type == TYPE_STRING ||
                (value->type == TYPE_CHAR && is_pointer_value(value))) {
                load_nullable_string(emitter, "rax", instruction->operand_a);
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register(first), x64_register("rax"));
                    write_call(emitter, "puts");
                } else {
                    write_address(emitter, first, ".LIR_string_format_", 0);
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register(second), x64_register("rax"));
                    write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                                x64_register("eax"), x64_register("eax"));
                    write_call(emitter, "printf");
                }
            } else if (is_floating(value->type)) {
                write_address(emitter, first, ".LIR_float_format_", 0);
                write_value_load(emitter, "rax", instruction->operand_a);
                convert_rax(emitter, value->type, TYPE_DOUBLE);
                char float_argument[16];
                (void) snprintf(float_argument, sizeof(float_argument), "xmm%d",
                                emitter->target == TARGET_COFF ? 1 : 0);
                write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                            x64_register(float_argument), x64_register("rax"));
                if (emitter->target == TARGET_COFF)
                    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                                x64_register("rdx"), x64_register("rax"));
                else
                    write_immediate(emitter, "rax", 1);
                write_call(emitter, "printf");
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_immediate(emitter, first, 10);
                    write_call(emitter, "putchar");
                }
            } else {
                write_address(emitter, first, value->type == TYPE_CHAR ?
                              ".LIR_char_format_" : ".LIR_int_format_", 0);
                write_value_load(emitter, second, instruction->operand_a);
                write_x64_2(emitter, X64_OP_XOR, X64_WIDTH_DWORD,
                            x64_register("eax"), x64_register("eax"));
                write_call(emitter, "printf");
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_immediate(emitter, first, 10);
                    write_call(emitter, "putchar");
                }
            }
            return 1;
        }
        case IR_OP_RETURN:
            emit_gc_cleanup(emitter);
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else {
                const IrInstruction *value = producer(function, instruction->operand_a);
                DataType return_type = ir_ast_type(function->source_program,
                                                   &function->return_type);
                write_value_load(emitter, "rax", instruction->operand_a);
                convert_rax(emitter, value->type, return_type);
                if (return_type == TYPE_FLOAT)
                    write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("eax"));
                else if (return_type == TYPE_DOUBLE)
                    write_x64_2(emitter, X64_OP_MOVQ, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("rax"));
            }
            char epilogue[64];
            (void) snprintf(epilogue, sizeof(epilogue), ".LIR_epilogue_%zu",
                            emitter->function_index);
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(epilogue));
            return 1;
        case IR_OP_BRANCH:
            write_value_load(emitter, "rax", instruction->operand_a);
            normalize_truth_rax(emitter,
                producer(function, instruction->operand_a)->type);
            write_x64_2(emitter, X64_OP_CMP, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_immediate(0));
            char true_label[64];
            char false_label[64];
            (void) snprintf(true_label, sizeof(true_label), ".LIR_%zu_%zu",
                            emitter->function_index, instruction->target_a);
            (void) snprintf(false_label, sizeof(false_label), ".LIR_%zu_%zu",
                            emitter->function_index, instruction->target_b);
            write_x64_1(emitter, X64_OP_JNE, X64_WIDTH_NONE, x64_label(true_label));
            write_x64_1(emitter, X64_OP_JMP, X64_WIDTH_NONE, x64_label(false_label));
            return 1;
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
            fprintf(emitter->output, ".LIR_%zu_%zu:\n", emitter->function_index,
                    instruction->target_a);
            return 1;
        case IR_OP_PHI:
            return 1;
        case IR_OP_CAST: {
            const IrInstruction *source = producer(function, instruction->operand_a);
            write_value_load(emitter, "rax", instruction->operand_a);
            if (instruction->type == TYPE_BIT && is_floating(source->type)) {
                if (source->type == TYPE_FLOAT)
                {
                    write_x64_2(emitter, X64_OP_MOVD, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("eax"));
                    write_x64_2(emitter, X64_OP_XORPS, X64_WIDTH_NONE,
                                x64_register("xmm1"), x64_register("xmm1"));
                    write_x64_2(emitter, X64_OP_UCOMISS, X64_WIDTH_NONE,
                                x64_register("xmm0"), x64_register("xmm1"));
                }
                else
                {
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
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        }
        case IR_OP_CALL: {
            const IrFunction *callee = called_function(emitter->module, instruction->symbol_id);
            if (callee == NULL) {
                const IrInstruction *callee_value = producer(function, instruction->operand_a);
                const char *name = ast_program_lexeme(function->source_program,
                                                      callee_value->auxiliary_token);
                return emit_builtin_call(emitter, instruction, name);
            }
            return emit_typed_call(emitter, instruction, callee);
        }
        case IR_OP_INDEX:
            if (!emit_lvalue_address(emitter, instruction, index)) return 0;
            if (is_inline_structure(emitter->module, instruction)) {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rbx"));
            } else
                write_typed_indirect_load(emitter, instruction->type,
                                          instruction->pointer_depth, "rbx");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_MEMBER:
            if (emit_enum_member(emitter, instruction)) return 1;
            if (!emit_lvalue_address(emitter, instruction, index)) return 0;
            if (is_inline_structure(emitter->module, instruction)) {
                write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                            x64_register("rax"), x64_register("rbx"));
            } else
                write_typed_indirect_load(emitter, instruction->type,
                                          instruction->pointer_depth, "rbx");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
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
        case IR_OP_FREE:
            write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi",
                             instruction->operand_a);
            write_call(emitter, "free");
            return 1;
    }
    return 0;
}

static int emit_function(Emitter *emitter) {
    FILE *output = emitter->output;
    char name_buffer[MAX_TOKEN * 2U + 64U];
    const char *name = function_link_name(emitter->function, name_buffer,
                                          sizeof(name_buffer));
    emitter->current_label = IR_VALUE_NONE;
    emitter->bounds_sequence = 0;
    fprintf(output, "    .globl %s\n", name);
    if (emitter->target == TARGET_COFF)
        fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", name);
    else
        fprintf(output, "    .type %s, @function\n", name);
    fprintf(output, "%s:\n", name);
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                x64_register("rbp"), x64_register("rsp"));
    write_x64_1(emitter, X64_OP_PUSH, X64_WIDTH_QWORD, x64_register("rbx"));
    write_frame_allocation(emitter);
    write_immediate(emitter, "rax", 0);
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *declaration = &emitter->function->instructions[i];
        if (declaration->opcode == IR_OP_DECLARE && declaration->is_gc)
            write_local_store(emitter, "rax", declaration_offset(emitter, declaration));
    }
    for (size_t p = 0; p < emitter->function->parameter_count; p++) {
        const IrParameter *parameter = &emitter->function->parameters[p];
        size_t register_index = parameter_register_index(emitter->function,
                                                         emitter->target, p);
        if (register_index == IR_VALUE_NONE) {
            size_t first_stack_offset = emitter->target == TARGET_COFF ? 48U : 16U;
            write_positive_frame_load(emitter, "rax", first_stack_offset +
                stack_parameter_index(emitter->function, emitter->target, p) * 8U);
        } else if (is_floating(parameter->type)) {
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
        if (is_integral(parameter->type) && parameter->pointer_depth == 0 &&
            !parameter->is_array)
            normalize_integral_parameter(emitter, parameter->type);
        if (type_is_structure(emitter->module, parameter->type_id)) {
            size_t copy_offset = parameter_copy_offset(emitter, p);
            size_t copy_slots = type_slots(emitter->module, parameter->type_id);
            write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD, x64_register("rbx"),
                        x64_memory(X64_WIDTH_NONE, "rbp", -(long long) copy_offset));
            copy_aggregate(emitter, copy_slots, "rax", "rbx");
            write_x64_2(emitter, X64_OP_MOV, X64_WIDTH_QWORD,
                        x64_register("rax"), x64_register("rbx"));
        }
        write_local_store(emitter, "rax", parameter_offset(emitter, parameter));
    }
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        if (emitter->source_map != NULL) {
            emitter->source_map->current_ir_instruction = i;
            emitter->source_map->current_span = emitter->function->instructions[i].span;
            emitter->source_map->has_source = emitter->function->instructions[i].span.begin.line > 0;
        }
        if (!emit_instruction(emitter, &emitter->function->instructions[i], i)) return 0;
    }
    if (emitter->source_map != NULL) emitter->source_map->has_source = 0;
    emit_gc_cleanup(emitter);
    write_immediate(emitter, "rax", 0);
    fprintf(output, ".LIR_epilogue_%zu:\n", emitter->function_index);
    write_x64_2(emitter, X64_OP_LEA, X64_WIDTH_QWORD,
                x64_register("rsp"), x64_memory(X64_WIDTH_NONE, "rbp", -8));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbx"));
    write_x64_1(emitter, X64_OP_POP, X64_WIDTH_QWORD, x64_register("rbp"));
    write_x64_0(emitter, X64_OP_RET);
    if (emitter->target == TARGET_ELF) fprintf(output, "    .size %s, .-%s\n", name, name);
    return 1;
}

static int emit_file(Emitter *emitter, int deterministic) {
    FILE *output = emitter->output;
    fputs("# Generated by Philipp01105's Compiler\n", output);
    fprintf(output, "# Source: %s\n", emitter->module->program->source_path);
    if (!deterministic) {
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        char datetime[64] = "unknown";
        if (local != NULL) (void) strftime(datetime, sizeof(datetime),
                                           "%Y-%m-%d %H:%M:%S", local);
        fprintf(output, "# Date: %s\n", datetime);
    }
    fputs("# Lowering: typed IR\n", output);
    fprintf(output, "# Target Format: %s\n# Syntax: %s\n",
            emitter->target == TARGET_COFF ? "COFF" : "ELF",
            emitter->syntax == SYNTAX_INTEL ? "Intel" : "AT&T");
    if (emitter->syntax == SYNTAX_INTEL) fputs("    .intel_syntax noprefix\n", output);
    if (emitter->target == TARGET_COFF) {
        fputs("    .def printf; .scl 2; .type 32; .endef\n"
              "    .def putchar; .scl 2; .type 32; .endef\n"
              "    .def puts; .scl 2; .type 32; .endef\n"
              "    .def strcmp; .scl 2; .type 32; .endef\n"
              "    .def strcpy; .scl 2; .type 32; .endef\n"
              "    .def strcat; .scl 2; .type 32; .endef\n"
              "    .def _strdup; .scl 2; .type 32; .endef\n"
              "    .def malloc; .scl 2; .type 32; .endef\n"
              "    .def calloc; .scl 2; .type 32; .endef\n"
              "    .def free; .scl 2; .type 32; .endef\n"
              "    .def strlen; .scl 2; .type 32; .endef\n"
              "    .def strtoll; .scl 2; .type 32; .endef\n"
              "    .def scanf; .scl 2; .type 32; .endef\n"
              "    .def snprintf; .scl 2; .type 32; .endef\n"
              "    .def fflush; .scl 2; .type 32; .endef\n"
              "    .def _write; .scl 2; .type 32; .endef\n"
              "    .def _read; .scl 2; .type 32; .endef\n"
              "    .def _open; .scl 2; .type 32; .endef\n"
              "    .def _close; .scl 2; .type 32; .endef\n"
              "    .section .rdata,\"dr\"\n", output);
    } else {
        fputs("    .section .rodata\n", output);
    }
    fputs(".LIR_empty_string_0:\n    .ascii \"\\0\"\n"
          ".LIR_int_format_0:\n    .ascii \"%lld\\0\"\n"
          ".LIR_char_format_0:\n    .ascii \"%c\\0\"\n"
          ".LIR_float_format_0:\n    .ascii \"%f\\0\"\n"
          ".LIR_string_format_0:\n    .ascii \"%s\\0\"\n"
          ".LIR_scan_int_format_0:\n    .ascii \"%d\\0\"\n"
          ".LIR_scan_char_format_0:\n    .ascii \" %c\\0\"\n"
          ".LIR_scan_string_format_0:\n    .ascii \"%255s\\0\"\n"
          "    .comm .LIR_scan_string_buffer_0,256\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            if (instruction->opcode == IR_OP_CONSTANT && instruction->type == TYPE_STRING) {
                fprintf(output, ".LIR_string_%zu_%zu:\n    .ascii \"", f,
                        instruction->result);
                write_escaped(output, ast_program_lexeme(function->source_program,
                                                         instruction->auxiliary_token));
                fputs("\\0\"\n", output);
            }
        }
    }
    for (size_t e = 0; e < emitter->module->enum_count; e++) {
        const IrEnum *enumeration = &emitter->module->enums[e];
        for (size_t a = 0; a < enumeration->variant_argument_count; a++) {
            const IrEnumArgument *argument = &enumeration->variant_arguments[a];
            if (argument->type_id >= emitter->module->type_count) continue;
            const IrType *type = &emitter->module->types[argument->type_id];
            if (type->kind != IR_TYPE_PRIMITIVE || type->primitive != TYPE_STRING) continue;
            fprintf(output, ".LIR_enum_%zu_%zu:\n    .ascii \"", e, a);
            write_escaped(output, ast_program_lexeme(enumeration->source_program,
                                                     argument->token));
            fputs("\\0\"\n", output);
        }
        for (size_t field = 0; field < enumeration->field_count; field++) {
            fprintf(output, ".LIR_enum_field_%zu_%zu:\n", e, field);
            if (enumeration->fields[field].type_id >= emitter->module->type_count) return 0;
            const IrType *field_type =
                &emitter->module->types[enumeration->fields[field].type_id];
            if (field_type->kind != IR_TYPE_PRIMITIVE) return 0;
            for (size_t variant_index = 0;
                 variant_index < enumeration->variant_count; variant_index++) {
                const IrEnumVariant *variant = &enumeration->variants[variant_index];
                size_t argument_index = variant->first_argument + field;
                if (field >= variant->argument_count ||
                    argument_index >= enumeration->variant_argument_count) return 0;
                const IrEnumArgument *argument =
                    &enumeration->variant_arguments[argument_index];
                if (field_type->primitive == TYPE_STRING) {
                    fprintf(output, "    .quad .LIR_enum_%zu_%zu\n", e,
                            argument_index);
                } else if (field_type->primitive == TYPE_DOUBLE) {
                    union { double value; uint64_t bits; } converted = {
                        strtod(ast_program_lexeme(enumeration->source_program,
                                                 argument->token), NULL)
                    };
                    if (argument->negative) converted.value = -converted.value;
                    fprintf(output, "    .quad 0x%016llx\n",
                            (unsigned long long) converted.bits);
                } else if (field_type->primitive == TYPE_FLOAT) {
                    union { float value; uint32_t bits; } converted = {
                        (float) strtod(ast_program_lexeme(enumeration->source_program,
                                                        argument->token), NULL)
                    };
                    if (argument->negative) converted.value = -converted.value;
                    fprintf(output, "    .quad 0x%08lx\n",
                            (unsigned long) converted.bits);
                } else {
                    IrInstruction literal = {.auxiliary_token = argument->token};
                    long long value = constant_value(enumeration->source_program, &literal);
                    if (argument->negative) value = -value;
                    fprintf(output, "    .quad %lld\n",
                            value);
                }
            }
        }
    }
    fputs("    .text\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        size_t declarations = 0;
        size_t aggregate_results = 0;
        size_t aggregate_parameters = 0;
        for (size_t i = 0; i < function->instruction_count; i++)
            if (function->instructions[i].opcode == IR_OP_DECLARE) {
                size_t slots = type_slots(emitter->module, function->instructions[i].type_id);
                if (slots == 0 || declarations > SIZE_MAX - slots) return 0;
                declarations += slots;
            } else if (function->instructions[i].opcode == IR_OP_CALL &&
                       is_inline_structure(emitter->module,
                                           &function->instructions[i])) {
                size_t slots = type_slots(emitter->module,
                                          function->instructions[i].type_id);
                if (slots == 0 || aggregate_results > SIZE_MAX - slots) return 0;
                aggregate_results += slots;
            }
        for (size_t p = 0; p < function->parameter_count; p++) {
            if (!type_is_structure(emitter->module, function->parameters[p].type_id)) continue;
            size_t parameter_slots = type_slots(emitter->module,
                                                function->parameters[p].type_id);
            if (parameter_slots == 0 || aggregate_parameters > SIZE_MAX - parameter_slots)
                return 0;
            aggregate_parameters += parameter_slots;
        }
        if (function->next_value > SIZE_MAX - function->parameter_count ||
            function->next_value + function->parameter_count > SIZE_MAX - declarations)
            return 0;
        size_t slots = function->next_value + function->parameter_count + declarations;
        if (slots > SIZE_MAX - aggregate_results) return 0;
        slots += aggregate_results;
        if (slots > SIZE_MAX - aggregate_parameters) return 0;
        slots += aggregate_parameters;
        if (slots > (8U * 1024U * 1024U - 8U) / 8U) return 0;
        size_t bytes = slots * 8U;
        emitter->function = function;
        emitter->function_index = f;
        emitter->declaration_count = declarations;
        emitter->frame_size = ((bytes + 15U) & ~(size_t) 15U) + 8U;
        if (!emit_function(emitter)) return 0;
    }
    return 1;
}

int x86_64_emit_ir_file(const IrModule *module, TargetFormat target,
                        SyntaxMode syntax, int deterministic,
                        const char *output_path, const char *source_map_path) {
    if (!valid_module(module) || output_path == NULL) return 0;
    FILE *output = fopen(output_path, "w");
    if (output == NULL) return 0;
    SourceMapWriter map = {0};
    if (source_map_path != NULL) {
        map.output = fopen(source_map_path, "w");
        if (map.output == NULL) {
            fclose(output);
            (void) remove(output_path);
            return 0;
        }
        if (fputs("dmm-source-map-v1\nsource path=", map.output) == EOF ||
            !map_quoted(map.output, module->program->source_path) ||
            fputs("\n", map.output) == EOF) map.failed = 1;
    }
    Emitter emitter = {
        .module = module,
        .target = target,
        .syntax = syntax,
        .output = output,
        .source_map = source_map_path == NULL ? NULL : &map
    };
    int success = emit_file(&emitter, deterministic);
    if (fclose(output) != 0) success = 0;
    if (map.output != NULL && (fclose(map.output) != 0 || map.failed)) success = 0;
    if (!success) (void) remove(output_path);
    if (!success && source_map_path != NULL) (void) remove(source_map_path);
    return success;
}
