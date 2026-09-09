#include "ir_emitter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

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
} Emitter;

#define IR_NATIVE_MAX_LOCALS 400U

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

static int is_runtime_scalar(const IrInstruction *instruction) {
    return instruction != NULL && (is_numeric(instruction->type) ||
           instruction->type == TYPE_STRING || is_pointer_value(instruction));
}

static int is_named_value(const IrModule *module, const IrInstruction *instruction) {
    return instruction != NULL && instruction->type_id < module->type_count &&
           module->types[instruction->type_id].kind == IR_TYPE_NAMED;
}

static int can_initialize(DataType from, DataType to, IrOpcode producer_opcode) {
    if (from == TYPE_STRING || to == TYPE_STRING) return from == to;
    if (from == to || (is_integral(from) && is_integral(to))) return 1;
    if (is_integral(from) && is_floating(to)) return 1;
    if (from == TYPE_FLOAT && to == TYPE_DOUBLE) return 1;
    return from == TYPE_DOUBLE && to == TYPE_FLOAT && producer_opcode == IR_OP_CONSTANT;
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

static int label_exists(const IrFunction *function, size_t label) {
    size_t matches = 0;
    for (size_t i = 0; i < function->instruction_count; i++)
        if (function->instructions[i].opcode == IR_OP_LABEL &&
            function->instructions[i].target_a == label) matches++;
    return matches == 1;
}

static unsigned maximum_loop_depth(const AstStatement *statement, unsigned depth) {
    unsigned maximum = depth;
    for (; statement != NULL; statement = statement->next) {
        unsigned nested = depth;
        if (statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR)
            nested++;
        unsigned body = maximum_loop_depth(statement->body, nested);
        unsigned alternative = maximum_loop_depth(statement->else_body, nested);
        unsigned initializer = maximum_loop_depth(statement->initializer, nested);
        if (body > maximum) maximum = body;
        if (alternative > maximum) maximum = alternative;
        if (initializer > maximum) maximum = initializer;
    }
    return maximum;
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

static size_t native_builtin_arity(const char *name) {
    if (strcmp(name, "io_strlen") == 0 || strcmp(name, "strlen") == 0 ||
        strcmp(name, "sys_close") == 0) return 1;
    if (strcmp(name, "sys_write") == 0 || strcmp(name, "sys_read") == 0 ||
        strcmp(name, "sys_open") == 0) return 3;
    return IR_VALUE_NONE;
}

static int supported_module(const IrModule *module) {
    if (module == NULL || !module->verified || module->program == NULL ||
        module->semantics == NULL || module->function_count == 0) return 0;
    const SemanticSymbol *main_symbol = semantic_find_global(module->semantics, "main",
                                                              SEMANTIC_SYMBOL_FUNCTION);
    if (main_symbol == NULL || main_symbol->declaration == NULL ||
        main_symbol->declaration->as.function.parameters != NULL) return 0;
    for (const AstDeclarationNode *declaration = module->program->root;
         declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_INVALID)
            return 0;
    int module_has_call = 0;
    int single_function_reason = 0;
    for (size_t f = 0; f < module->function_count; f++) {
      const IrFunction *function = &module->functions[f];
      if (function->owner_token != AST_TOKEN_NONE ||
          function->source_program == NULL ||
          function->return_type.name_token >= function->source_program->token_count ||
          maximum_loop_depth(module->semantics->symbols[function->symbol_id].declaration
                                 ->as.function.body, 0) > 100U) return 0;
      DataType return_type = ir_ast_type(function->source_program, &function->return_type);
      if (!is_numeric(return_type) && return_type != TYPE_STRING &&
          return_type != TYPE_VOID && function->return_type.pointer_depth == 0) return 0;
      for (size_t p = 0; p < function->parameter_count; p++)
          if ((!is_numeric(function->parameters[p].type) &&
               function->parameters[p].type != TYPE_STRING &&
               function->parameters[p].pointer_depth == 0 &&
               !function->parameters[p].is_array))
              return 0;
      size_t declarations = 0;
      for (size_t i = 0; i < function->instruction_count; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        if (instruction->result != IR_VALUE_NONE &&
            producer(function, instruction->result) != instruction) return 0;
        switch (instruction->opcode) {
            case IR_OP_CONSTANT:
                if (!is_runtime_scalar(instruction) ||
                    instruction->auxiliary_token >= function->source_program->token_count) return 0;
                if (is_floating(instruction->type)) single_function_reason = 1;
                break;
            case IR_OP_LOAD: {
                if ((!is_runtime_scalar(instruction) &&
                     !is_named_value(module, instruction) &&
                     !(instruction->type == TYPE_VOID &&
                       called_function(module, instruction->symbol_id) != NULL)) ||
                    instruction->auxiliary_token >= function->source_program->token_count) return 0;
                const char *name = ast_program_lexeme(function->source_program,
                                                      instruction->auxiliary_token);
                int type_symbol = instruction->symbol_id < module->semantics->symbol_count &&
                    (module->semantics->symbols[instruction->symbol_id].kind ==
                         SEMANTIC_SYMBOL_STRUCT ||
                     module->semantics->symbols[instruction->symbol_id].kind ==
                         SEMANTIC_SYMBOL_ENUM);
                int builtin = native_builtin_arity(name) != IR_VALUE_NONE;
                if (strcmp(name, "true") != 0 && strcmp(name, "false") != 0 &&
                    local_declaration(function, instruction->symbol_id, i) == NULL &&
                    function_parameter(function, instruction->symbol_id) == NULL &&
                    called_function(module, instruction->symbol_id) == NULL && !type_symbol &&
                    !builtin)
                    return 0;
                break;
            }
            case IR_OP_DECLARE:
            {
                const IrInstruction *initializer = instruction->operand_a == IR_VALUE_NONE
                    ? NULL : producer(function, instruction->operand_a);
                if ((!is_runtime_scalar(instruction) && !instruction->is_array &&
                     !is_named_value(module, instruction)) ||
                    instruction->auxiliary_token >= function->source_program->token_count ||
                    (initializer != NULL &&
                     ((is_pointer_value(instruction) && !is_pointer_value(initializer)) ||
                      (instruction->type == TYPE_STRING && instruction->pointer_depth == 0 &&
                       initializer->type != TYPE_STRING) ||
                      (instruction->type != TYPE_STRING &&
                       !is_pointer_value(instruction) &&
                       (!is_numeric(initializer->type) ||
                        !can_initialize(initializer->type, instruction->type,
                                        initializer->opcode))))) ||
                    (instruction->operand_a != IR_VALUE_NONE && initializer == NULL)) return 0;
                if (instruction->symbol_id == AST_SYMBOL_NONE) return 0;
                for (size_t j = 0; j < i; j++)
                    if (function->instructions[j].opcode == IR_OP_DECLARE &&
                        function->instructions[j].symbol_id == instruction->symbol_id) return 0;
                declarations++;
                if (declarations > IR_NATIVE_MAX_LOCALS) return 0;
                if (is_floating(instruction->type)) single_function_reason = 1;
                break;
            }
            case IR_OP_STORE: {
                const IrInstruction *target = producer(function, instruction->operand_a);
                const IrInstruction *stored = instruction->operand_b == IR_VALUE_NONE
                    ? NULL : producer(function, instruction->operand_b);
                if (target == NULL ||
                    (target->opcode != IR_OP_LOAD && target->opcode != IR_OP_INDEX &&
                     target->opcode != IR_OP_MEMBER &&
                     !(target->opcode == IR_OP_UNARY &&
                       target->operator_type == TOKEN_STAR))) return 0;
                if (target->opcode == IR_OP_LOAD &&
                    local_declaration(function, target->symbol_id, i) == NULL &&
                    function_parameter(function, target->symbol_id) == NULL) return 0;
                if (target->type == TYPE_BIT && instruction->operator_type != TOKEN_EQUAL)
                    return 0;
                if (instruction->operator_type != TOKEN_PLUS_PLUS &&
                    instruction->operator_type != TOKEN_MINUS_MINUS &&
                    (stored == NULL || !is_numeric(stored->type))) return 0;
                if (!is_numeric(target->type) && target->type != TYPE_STRING &&
                    !is_pointer_value(target)) return 0;
                if (instruction->operator_type == TOKEN_EQUAL && stored != NULL &&
                    !is_pointer_value(target) &&
                    !can_initialize(stored->type, target->type, stored->opcode)) return 0;
                single_function_reason = 1;
                break;
            }
            case IR_OP_UNARY:
            {
                const IrInstruction *operand = producer(function, instruction->operand_b);
                int address_operation = instruction->operator_type == TOKEN_AMPERSAND ||
                                        instruction->operator_type == TOKEN_STAR;
                if ((!is_numeric(instruction->type) && !address_operation) ||
                    (instruction->operator_type != TOKEN_MINUS &&
                     instruction->operator_type != TOKEN_BANG && !address_operation) ||
                    operand == NULL ||
                    (instruction->operator_type == TOKEN_MINUS &&
                     operand->type == TYPE_BIT) ||
                    (instruction->operator_type == TOKEN_STAR && !is_pointer_value(operand)))
                    return 0;
                single_function_reason = 1;
                break;
            }
            case IR_OP_BINARY:
            {
                const IrInstruction *left_value = producer(function, instruction->operand_a);
                const IrInstruction *right_value = producer(function, instruction->operand_b);
                int string_compare = instruction->type == TYPE_BIT &&
                    (instruction->operator_type == TOKEN_EQUAL_EQUAL ||
                     instruction->operator_type == TOKEN_BANG_EQUAL) &&
                    left_value != NULL && right_value != NULL &&
                    left_value->type == TYPE_STRING && right_value->type == TYPE_STRING;
                int numeric_binary = left_value != NULL && right_value != NULL &&
                    is_numeric(left_value->type) && is_numeric(right_value->type) &&
                    is_numeric(instruction->type);
                if ((!numeric_binary && !string_compare) ||
                    instruction->operator_type == TOKEN_AMP_AMP ||
                    instruction->operator_type == TOKEN_PIPE_PIPE ||
                    left_value == NULL || right_value == NULL) return 0;
                if (instruction->operator_type >= TOKEN_PLUS &&
                    instruction->operator_type <= TOKEN_PERCENT &&
                    (left_value->type == TYPE_BIT || right_value->type == TYPE_BIT)) return 0;
                if (instruction->operator_type == TOKEN_PERCENT &&
                    (is_floating(left_value->type) || is_floating(right_value->type))) return 0;
                break;
            }
            case IR_OP_PRINT: {
                const IrInstruction *value = producer(function, instruction->operand_a);
                if (value == NULL || (!is_numeric(value->type) &&
                    value->type != TYPE_STRING)) return 0;
                if (value->type == TYPE_STRING || is_floating(value->type))
                    single_function_reason = 1;
                break;
            }
            case IR_OP_PHI:
                if (!is_integral(instruction->type) ||
                    producer(function, instruction->operand_a) == NULL ||
                    producer(function, instruction->operand_b) == NULL ||
                    !is_integral(producer(function, instruction->operand_a)->type) ||
                    !is_integral(producer(function, instruction->operand_b)->type) ||
                    !label_exists(function, instruction->target_a) ||
                    !label_exists(function, instruction->target_b)) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_RETURN:
                if ((return_type == TYPE_VOID && instruction->operand_a != IR_VALUE_NONE) ||
                    (return_type != TYPE_VOID &&
                     (instruction->operand_a == IR_VALUE_NONE ||
                      !is_runtime_scalar(producer(function, instruction->operand_a))))) return 0;
                break;
            case IR_OP_BRANCH:
                if (producer(function, instruction->operand_a) == NULL ||
                    !label_exists(function, instruction->target_a) ||
                    !label_exists(function, instruction->target_b)) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_JUMP:
            case IR_OP_LABEL:
                if (!label_exists(function, instruction->target_a)) return 0;
                break;
            case IR_OP_CALL: {
                const IrFunction *callee = called_function(module, instruction->symbol_id);
                const IrInstruction *callee_value = producer(function, instruction->operand_a);
                const char *builtin_name = callee_value == NULL ? "" :
                    ast_program_lexeme(function->source_program,
                                       callee_value->auxiliary_token);
                size_t builtin_arity = native_builtin_arity(builtin_name);
                if ((callee == NULL && builtin_arity == IR_VALUE_NONE) ||
                    (callee != NULL && instruction->argument_count != callee->parameter_count) ||
                    (callee == NULL && instruction->argument_count != builtin_arity) ||
                    (!is_numeric(instruction->type) && instruction->type != TYPE_STRING &&
                     instruction->type != TYPE_VOID && instruction->pointer_depth == 0)) return 0;
                for (size_t a = 0; a < instruction->argument_count; a++) {
                    const IrInstruction *argument = producer(function,
                        function->arguments[instruction->first_argument + a]);
                    if (callee == NULL) {
                        if (!is_runtime_scalar(argument)) return 0;
                        continue;
                    }
                    int pointer_parameter = callee->parameters[a].pointer_depth != 0 ||
                                            callee->parameters[a].is_array;
                    if (!is_runtime_scalar(argument) ||
                        (pointer_parameter && !is_pointer_value(argument)) ||
                        (!pointer_parameter &&
                         !can_initialize(argument->type, callee->parameters[a].type,
                                         argument->opcode))) return 0;
                }
                module_has_call = 1;
                single_function_reason = 1;
                break;
            }
            case IR_OP_CAST:
                if (!is_numeric(instruction->type) ||
                    producer(function, instruction->operand_a) == NULL ||
                    !is_numeric(producer(function, instruction->operand_a)->type)) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_INDEX:
                if (producer(function, instruction->operand_a) == NULL ||
                    !is_pointer_value(producer(function, instruction->operand_a)) ||
                    producer(function, instruction->operand_b) == NULL ||
                    !is_integral(producer(function, instruction->operand_b)->type)) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_MEMBER:
                if (producer(function, instruction->operand_a) == NULL ||
                    instruction->symbol_id == AST_SYMBOL_NONE ||
                    instruction->symbol_id >= module->semantics->symbol_count) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_FREE:
                if (producer(function, instruction->operand_a) == NULL ||
                    !is_pointer_value(producer(function, instruction->operand_a))) return 0;
                single_function_reason = 1;
                break;
            case IR_OP_ALLOC:
                if (!is_pointer_value(instruction)) return 0;
                single_function_reason = 1;
                break;
        }
    }
      if (function->next_value > (SIZE_MAX / 8U) - declarations - function->parameter_count)
          return 0;
    }
    return module->function_count == 1 ? single_function_reason : module_has_call;
}

int x86_64_ir_supports_module(const IrModule *module) {
    return supported_module(module);
}

static size_t value_offset(size_t value) {
    return (value + 2U) * 8U;
}

static size_t type_slots(const IrModule *module, IrTypeId type_id) {
    if (type_id >= module->type_count) return 0;
    const IrType *type = &module->types[type_id];
    if (type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_PRIMITIVE) return 1;
    if (type->kind == IR_TYPE_ARRAY) {
        size_t element = type_slots(module, type->element_type);
        if (element == 0 || type->array_length > SIZE_MAX / element) return 0;
        return type->array_length * element;
    }
    for (size_t s = 0; s < module->structure_count; s++) {
        if (module->structures[s].symbol_id != type->symbol_id) continue;
        size_t slots = 0;
        for (size_t f = 0; f < module->structures[s].field_count; f++) {
            size_t field = type_slots(module, module->structures[s].fields[f].type_id);
            if (field == 0 || slots > SIZE_MAX - field) return 0;
            slots += field;
        }
        return slots == 0 ? 1 : slots;
    }
    for (size_t e = 0; e < module->enum_count; e++) {
        if (module->enums[e].symbol_id != type->symbol_id) continue;
        size_t slots = 0;
        for (size_t f = 0; f < module->enums[e].field_count; f++) {
            size_t field = type_slots(module, module->enums[e].fields[f].type_id);
            if (field == 0 || slots > SIZE_MAX - field) return 0;
            slots += field;
        }
        return slots == 0 ? 1 : slots;
    }
    return 1;
}

static size_t declaration_slots(const Emitter *emitter,
                                const IrInstruction *declaration) {
    size_t slots = type_slots(emitter->module, declaration->type_id);
    return slots == 0 ? 1 : slots;
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
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov %s, QWORD PTR [rbp - %zu]\n", reg,
                value_offset(value));
    else
        fprintf(emitter->output, "    movq -%zu(%%rbp), %%%s\n", value_offset(value), reg);
}

static void write_value_store(const Emitter *emitter, const char *reg, size_t value) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov QWORD PTR [rbp - %zu], %s\n",
                value_offset(value), reg);
    else
        fprintf(emitter->output, "    movq %%%s, -%zu(%%rbp)\n", reg, value_offset(value));
}

static void write_local_load(const Emitter *emitter, const char *reg, size_t offset) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov %s, QWORD PTR [rbp - %zu]\n", reg, offset);
    else
        fprintf(emitter->output, "    movq -%zu(%%rbp), %%%s\n", offset, reg);
}

static void write_local_store(const Emitter *emitter, const char *reg, size_t offset) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov QWORD PTR [rbp - %zu], %s\n", offset, reg);
    else
        fprintf(emitter->output, "    movq %%%s, -%zu(%%rbp)\n", reg, offset);
}

static void write_immediate(const Emitter *emitter, const char *reg, long long value) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov %s, %lld\n", reg, value);
    else
        fprintf(emitter->output, "    movq $%lld, %%%s\n", value, reg);
}

static void write_address(const Emitter *emitter, const char *reg, const char *label,
                          size_t suffix) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    lea %s, [rip + %s%zu]\n", reg, label, suffix);
    else
        fprintf(emitter->output, "    leaq %s%zu(%%rip), %%%s\n", label, suffix, reg);
}

static void write_call(const Emitter *emitter, const char *name) {
    if (emitter->target == TARGET_COFF) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    sub rsp, 32\n" :
                                               "    subq $32, %rsp\n", emitter->output);
    }
    fprintf(emitter->output, "    call %s\n", name);
    if (emitter->target == TARGET_COFF) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    add rsp, 32\n" :
                                               "    addq $32, %rsp\n", emitter->output);
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
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov %s, QWORD PTR [rbp + %zu]\n", reg, offset);
    else
        fprintf(emitter->output, "    movq %zu(%%rbp), %%%s\n", offset, reg);
}

static void normalize_integral_parameter(const Emitter *emitter, DataType type) {
    if (type == TYPE_INT) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    movsxd rax, eax\n" :
                                               "    movslq %eax, %rax\n", emitter->output);
    } else if (type == TYPE_CHAR) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    movsx rax, al\n" :
                                               "    movsbq %al, %rax\n", emitter->output);
    } else if (type == TYPE_BYTE) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    movzx eax, al\n" :
                                               "    movzbl %al, %eax\n", emitter->output);
    } else if (type == TYPE_BIT) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    and eax, 1\n" :
                                               "    andl $1, %eax\n", emitter->output);
    }
}

static long long constant_value(const AstProgram *program,
                                const IrInstruction *instruction) {
    const AstToken *token = ast_program_token(program, instruction->auxiliary_token);
    if (token == NULL) return 0;
    if (token->type == TOKEN_CHAR_LITERAL) return (unsigned char) token->lexeme[0];
    return strtoll(token->lexeme, NULL, 10);
}

static int emit_string_compare(Emitter *emitter, const IrInstruction *instruction) {
    const char *left_argument = emitter->target == TARGET_COFF ? "rcx" : "rdi";
    const char *right_argument = emitter->target == TARGET_COFF ? "rdx" : "rsi";
    int equal_result = instruction->operator_type == TOKEN_EQUAL_EQUAL;
    write_value_load(emitter, "rax", instruction->operand_a);
    write_value_load(emitter, "rcx", instruction->operand_b);
    if (emitter->syntax == SYNTAX_INTEL) {
        fprintf(emitter->output,
                "    cmp rax, rcx\n    je .LIR_string_equal_%zu_%zu\n"
                "    test rax, rax\n    jz .LIR_string_unequal_%zu_%zu\n"
                "    test rcx, rcx\n    jz .LIR_string_unequal_%zu_%zu\n",
                emitter->function_index, instruction->result,
                emitter->function_index, instruction->result,
                emitter->function_index, instruction->result);
    } else {
        fprintf(emitter->output,
                "    cmpq %%rcx, %%rax\n    je .LIR_string_equal_%zu_%zu\n"
                "    testq %%rax, %%rax\n    jz .LIR_string_unequal_%zu_%zu\n"
                "    testq %%rcx, %%rcx\n    jz .LIR_string_unequal_%zu_%zu\n",
                emitter->function_index, instruction->result,
                emitter->function_index, instruction->result,
                emitter->function_index, instruction->result);
    }
    write_value_load(emitter, left_argument, instruction->operand_a);
    write_value_load(emitter, right_argument, instruction->operand_b);
    write_call(emitter, "strcmp");
    fputs(emitter->syntax == SYNTAX_INTEL ? "    cmp eax, 0\n" :
                                           "    cmpl $0, %eax\n", emitter->output);
    fprintf(emitter->output, "    set%s %s\n",
            equal_result ? "e" : "ne", emitter->syntax == SYNTAX_INTEL ? "al" : "%al");
    fputs(emitter->syntax == SYNTAX_INTEL ? "    movzx rax, al\n" :
                                           "    movzbq %al, %rax\n", emitter->output);
    fprintf(emitter->output, "    jmp .LIR_string_done_%zu_%zu\n"
            ".LIR_string_equal_%zu_%zu:\n",
            emitter->function_index, instruction->result,
            emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 1 : 0);
    fprintf(emitter->output, "    jmp .LIR_string_done_%zu_%zu\n"
            ".LIR_string_unequal_%zu_%zu:\n",
            emitter->function_index, instruction->result,
            emitter->function_index, instruction->result);
    write_immediate(emitter, "rax", equal_result ? 0 : 1);
    fprintf(emitter->output, ".LIR_string_done_%zu_%zu:\n",
            emitter->function_index, instruction->result);
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static void convert_rax(Emitter *emitter, DataType from, DataType to);

static void load_floating_value(Emitter *emitter, size_t value, DataType target,
                                unsigned xmm) {
    const IrInstruction *source = producer(emitter->function, value);
    write_value_load(emitter, "rax", value);
    convert_rax(emitter, source->type, target);
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov%c xmm%u, %s\n",
                target == TYPE_FLOAT ? 'd' : 'q', xmm,
                target == TYPE_FLOAT ? "eax" : "rax");
    else
        fprintf(emitter->output, "    mov%c %%%s, %%xmm%u\n",
                target == TYPE_FLOAT ? 'd' : 'q',
                target == TYPE_FLOAT ? "eax" : "rax", xmm);
}

static void store_floating_result(Emitter *emitter, DataType type, unsigned xmm,
                                  size_t result) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov%c %s, xmm%u\n",
                type == TYPE_FLOAT ? 'd' : 'q',
                type == TYPE_FLOAT ? "eax" : "rax", xmm);
    else
        fprintf(emitter->output, "    mov%c %%xmm%u, %%%s\n",
                type == TYPE_FLOAT ? 'd' : 'q', xmm,
                type == TYPE_FLOAT ? "eax" : "rax");
    write_value_store(emitter, "rax", result);
}

static int emit_floating_binary(Emitter *emitter, const IrInstruction *instruction,
                                DataType operation_type) {
    load_floating_value(emitter, instruction->operand_a, operation_type, 2);
    load_floating_value(emitter, instruction->operand_b, operation_type, 1);
    const char *suffix = operation_type == TYPE_FLOAT ? "ss" : "sd";
    if (instruction->operator_type >= TOKEN_PLUS &&
        instruction->operator_type <= TOKEN_SLASH) {
        const char *operation = instruction->operator_type == TOKEN_PLUS ? "add" :
                                instruction->operator_type == TOKEN_MINUS ? "sub" :
                                instruction->operator_type == TOKEN_STAR ? "mul" : "div";
        if (emitter->syntax == SYNTAX_INTEL)
            fprintf(emitter->output, "    %s%s xmm2, xmm1\n", operation, suffix);
        else
            fprintf(emitter->output, "    %s%s %%xmm1, %%xmm2\n", operation, suffix);
        store_floating_result(emitter, instruction->type, 2, instruction->result);
        return 1;
    }
    if (instruction->operator_type < TOKEN_EQUAL_EQUAL ||
        instruction->operator_type > TOKEN_GREATER_EQUAL) return 0;
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    ucomi%s xmm2, xmm1\n", suffix);
    else
        fprintf(emitter->output, "    ucomi%s %%xmm1, %%xmm2\n", suffix);
    const char *condition = "e";
    int require_ordered = 0;
    int include_unordered = 0;
    if (instruction->operator_type == TOKEN_BANG_EQUAL) {
        condition = "ne";
        include_unordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS) {
        condition = "b";
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_LESS_EQUAL) {
        condition = "be";
        require_ordered = 1;
    } else if (instruction->operator_type == TOKEN_GREATER) condition = "a";
    else if (instruction->operator_type == TOKEN_GREATER_EQUAL) condition = "ae";
    else require_ordered = 1;
    fprintf(emitter->output, "    set%s %s\n", condition,
            emitter->syntax == SYNTAX_INTEL ? "al" : "%al");
    if (require_ordered || include_unordered) {
        fprintf(emitter->output, "    set%s %s\n",
                include_unordered ? "p" : "np",
                emitter->syntax == SYNTAX_INTEL ? "dl" : "%dl");
        fputs(emitter->syntax == SYNTAX_INTEL ?
              (include_unordered ? "    or al, dl\n" : "    and al, dl\n") :
              (include_unordered ? "    orb %dl, %al\n" : "    andb %dl, %al\n"),
              emitter->output);
    }
    fputs(emitter->syntax == SYNTAX_INTEL ? "    movzx rax, al\n" :
                                           "    movzbq %al, %rax\n", emitter->output);
    write_value_store(emitter, "rax", instruction->result);
    return 1;
}

static int emit_typed_call(Emitter *emitter, const IrInstruction *instruction,
                           const IrFunction *callee) {
    const IrFunction *caller = emitter->function;
    size_t stack_count = stack_parameter_count(callee, emitter->target);
    if ((stack_count & 1U) != 0) {
        fputs(emitter->syntax == SYNTAX_INTEL ? "    sub rsp, 8\n" :
                                               "    subq $8, %rsp\n", emitter->output);
    }
    for (size_t a = instruction->argument_count; a-- > 0;) {
        if (parameter_register_index(callee, emitter->target, a) != IR_VALUE_NONE) continue;
        size_t value_id = caller->arguments[instruction->first_argument + a];
        const IrInstruction *value = producer(caller, value_id);
        write_value_load(emitter, "rax", value_id);
        convert_rax(emitter, value->type, callee->parameters[a].type);
        fputs(emitter->syntax == SYNTAX_INTEL ? "    push rax\n" :
                                               "    pushq %rax\n", emitter->output);
    }
    if (emitter->target == TARGET_COFF)
        fputs(emitter->syntax == SYNTAX_INTEL ? "    sub rsp, 32\n" :
                                               "    subq $32, %rsp\n", emitter->output);

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
        if (emitter->syntax == SYNTAX_INTEL)
            fprintf(emitter->output, "    mov %s, rax\n",
                    argument_register(emitter->target, register_index));
        else
            fprintf(emitter->output, "    movq %%rax, %%%s\n",
                    argument_register(emitter->target, register_index));
    }
    fprintf(emitter->output, "    call %s\n",
            ast_program_lexeme(callee->source_program, callee->name_token));
    size_t cleanup = stack_count * 8U + (emitter->target == TARGET_COFF ? 32U : 0U) +
                     (((stack_count & 1U) != 0) ? 8U : 0U);
    if (cleanup != 0) {
        if (emitter->syntax == SYNTAX_INTEL)
            fprintf(emitter->output, "    add rsp, %zu\n", cleanup);
        else
            fprintf(emitter->output, "    addq $%zu, %%rsp\n", cleanup);
    }
    if (instruction->type != TYPE_VOID) {
        if (instruction->type == TYPE_FLOAT)
            fputs(emitter->syntax == SYNTAX_INTEL ? "    movd eax, xmm0\n" :
                                                   "    movd %xmm0, %eax\n", emitter->output);
        else if (instruction->type == TYPE_DOUBLE)
            fputs(emitter->syntax == SYNTAX_INTEL ? "    movq rax, xmm0\n" :
                                                   "    movq %xmm0, %rax\n", emitter->output);
        write_value_store(emitter, "rax", instruction->result);
    }
    return 1;
}

static int emit_builtin_call(Emitter *emitter, const IrInstruction *instruction,
                             const char *name) {
    const IrFunction *function = emitter->function;
    if (strcmp(name, "io_strlen") == 0 || strcmp(name, "strlen") == 0) {
        size_t value = function->arguments[instruction->first_argument];
        write_value_load(emitter, emitter->target == TARGET_COFF ? "rcx" : "rdi", value);
        write_call(emitter, "strlen");
    } else if (emitter->target == TARGET_ELF) {
        static const char *registers[] = {"rdi", "rsi", "rdx"};
        for (size_t a = 0; a < instruction->argument_count; a++)
            write_value_load(emitter, registers[a],
                function->arguments[instruction->first_argument + a]);
        long long syscall_number = strcmp(name, "sys_write") == 0 ? 1 :
                                   strcmp(name, "sys_open") == 0 ? 2 :
                                   strcmp(name, "sys_close") == 0 ? 3 : 0;
        write_immediate(emitter, "rax", syscall_number);
        fputs("    syscall\n", emitter->output);
    } else {
        static const char *registers[] = {"rcx", "rdx", "r8"};
        for (size_t a = 0; a < instruction->argument_count; a++)
            write_value_load(emitter, registers[a],
                function->arguments[instruction->first_argument + a]);
        if (strcmp(name, "sys_open") == 0) {
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output,
                        "    mov r10d, edx\n    and edx, 3\n"
                        "    test r10d, 64\n    jz .LIR_open_no_create_%zu_%zu\n"
                        "    or edx, 256\n.LIR_open_no_create_%zu_%zu:\n"
                        "    test r10d, 512\n    jz .LIR_open_no_trunc_%zu_%zu\n"
                        "    or edx, 512\n.LIR_open_no_trunc_%zu_%zu:\n"
                        "    test r10d, 1024\n    jz .LIR_open_no_append_%zu_%zu\n"
                        "    or edx, 8\n.LIR_open_no_append_%zu_%zu:\n"
                        "    or edx, 32768\n    mov r8d, 384\n",
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result);
            else
                fprintf(emitter->output,
                        "    movl %%edx, %%r10d\n    andl $3, %%edx\n"
                        "    testl $64, %%r10d\n    jz .LIR_open_no_create_%zu_%zu\n"
                        "    orl $256, %%edx\n.LIR_open_no_create_%zu_%zu:\n"
                        "    testl $512, %%r10d\n    jz .LIR_open_no_trunc_%zu_%zu\n"
                        "    orl $512, %%edx\n.LIR_open_no_trunc_%zu_%zu:\n"
                        "    testl $1024, %%r10d\n    jz .LIR_open_no_append_%zu_%zu\n"
                        "    orl $8, %%edx\n.LIR_open_no_append_%zu_%zu:\n"
                        "    orl $32768, %%edx\n    movl $384, %%r8d\n",
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result,
                        emitter->function_index, instruction->result);
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
    if (base == NULL || base->opcode != IR_OP_LOAD) return 0;
    const IrInstruction *declaration = local_declaration(emitter->function,
        base->symbol_id, emitter->function->instruction_count);
    if (declaration == NULL || declaration->type_id >= emitter->module->type_count) return 0;
    const IrType *type = &emitter->module->types[declaration->type_id];
    return type->kind == IR_TYPE_ARRAY ? type->array_length : 0;
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
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output, "    lea rbx, [rbp - %zu]\n", offset);
            else
                fprintf(emitter->output, "    leaq -%zu(%%rbp), %%rbx\n", offset);
            return 1;
        }
        if (parameter != NULL && !parameter->is_array) {
            size_t offset = parameter_offset(emitter, parameter);
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output, "    lea rbx, [rbp - %zu]\n", offset);
            else
                fprintf(emitter->output, "    leaq -%zu(%%rbp), %%rbx\n", offset);
            return 1;
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
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output,
                        "    cmp rcx, 0\n    jl .LIR_bounds_fail_%zu_%zu\n"
                        "    cmp rcx, %zu\n    jge .LIR_bounds_fail_%zu_%zu\n",
                        emitter->function_index, bounds_id, length,
                        emitter->function_index, bounds_id);
            else
                fprintf(emitter->output,
                        "    cmpq $0, %%rcx\n    jl .LIR_bounds_fail_%zu_%zu\n"
                        "    cmpq $%zu, %%rcx\n    jge .LIR_bounds_fail_%zu_%zu\n",
                        emitter->function_index, bounds_id, length,
                        emitter->function_index, bounds_id);
        }
        size_t element_size = target->pointer_depth != 0 ? 8U :
                              (target->type == TYPE_CHAR || target->type == TYPE_BYTE ||
                               target->type == TYPE_BIT) ? 1U :
                              (target->type == TYPE_INT || target->type == TYPE_FLOAT) ? 4U : 8U;
        if (emitter->syntax == SYNTAX_INTEL)
            fprintf(emitter->output, "    imul rcx, %zu\n    lea rbx, [rax + rcx]\n",
                    element_size);
        else
            fprintf(emitter->output, "    imulq $%zu, %%rcx\n    leaq (%%rax,%%rcx), %%rbx\n",
                    element_size);
        if (length != 0) {
            fprintf(emitter->output, "    jmp .LIR_bounds_ok_%zu_%zu\n"
                    ".LIR_bounds_fail_%zu_%zu:\n    ud2\n"
                    ".LIR_bounds_ok_%zu_%zu:\n",
                    emitter->function_index, bounds_id,
                    emitter->function_index, bounds_id,
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
        if (is_pointer_value(base)) {
            write_value_load(emitter, "rbx", target->operand_a);
        } else if (!emit_lvalue_address(emitter, base, instruction_index)) {
            return 0;
        }
        size_t offset = aggregate_field_offset(emitter, aggregate, target->symbol_id);
        if (offset == SIZE_MAX) return 0;
        if (offset != 0) {
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output, "    add rbx, %zu\n", offset);
            else
                fprintf(emitter->output, "    addq $%zu, %%rbx\n", offset);
        }
        return 1;
    }
    return 0;
}

static void write_indirect_load(const Emitter *emitter, const char *destination,
                                const char *address) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov %s, QWORD PTR [%s]\n", destination, address);
    else
        fprintf(emitter->output, "    movq (%%%s), %%%s\n", address, destination);
}

static void write_indirect_store(const Emitter *emitter, const char *address,
                                 const char *source) {
    if (emitter->syntax == SYNTAX_INTEL)
        fprintf(emitter->output, "    mov QWORD PTR [%s], %s\n", address, source);
    else
        fprintf(emitter->output, "    movq %%%s, (%%%s)\n", source, address);
}

static void write_typed_indirect_load(const Emitter *emitter, DataType type,
                                      unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_load(emitter, "rax", address);
    } else if (emitter->syntax == SYNTAX_INTEL) {
        if (type == TYPE_INT) fprintf(emitter->output, "    movsxd rax, DWORD PTR [%s]\n", address);
        else if (type == TYPE_CHAR) fprintf(emitter->output, "    movsx rax, BYTE PTR [%s]\n", address);
        else if (type == TYPE_BYTE || type == TYPE_BIT)
            fprintf(emitter->output, "    movzx eax, BYTE PTR [%s]\n", address);
        else fprintf(emitter->output, "    mov eax, DWORD PTR [%s]\n", address);
    } else {
        if (type == TYPE_INT) fprintf(emitter->output, "    movslq (%%%s), %%rax\n", address);
        else if (type == TYPE_CHAR) fprintf(emitter->output, "    movsbq (%%%s), %%rax\n", address);
        else if (type == TYPE_BYTE || type == TYPE_BIT)
            fprintf(emitter->output, "    movzbl (%%%s), %%eax\n", address);
        else fprintf(emitter->output, "    movl (%%%s), %%eax\n", address);
    }
}

static void write_typed_indirect_store(const Emitter *emitter, DataType type,
                                       unsigned pointer_depth, const char *address) {
    if (pointer_depth != 0 || type == TYPE_DOUBLE || type == TYPE_STRING ||
        type == TYPE_UNKNOWN) {
        write_indirect_store(emitter, address, "rax");
    } else if (emitter->syntax == SYNTAX_INTEL) {
        if (type == TYPE_INT || type == TYPE_FLOAT)
            fprintf(emitter->output, "    mov DWORD PTR [%s], eax\n", address);
        else fprintf(emitter->output, "    mov BYTE PTR [%s], al\n", address);
    } else {
        if (type == TYPE_INT || type == TYPE_FLOAT)
            fprintf(emitter->output, "    movl %%eax, (%%%s)\n", address);
        else fprintf(emitter->output, "    movb %%al, (%%%s)\n", address);
    }
}

static void write_frame_allocation(const Emitter *emitter) {
    size_t remaining = emitter->frame_size;
    while (remaining > 4096U) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    sub rsp, 4096\n    mov BYTE PTR [rsp], 0\n" :
              "    subq $4096, %rsp\n    movb $0, (%rsp)\n", emitter->output);
        remaining -= 4096U;
    }
    if (remaining != 0) {
        if (emitter->syntax == SYNTAX_INTEL)
            fprintf(emitter->output, "    sub rsp, %zu\n", remaining);
        else
            fprintf(emitter->output, "    subq $%zu, %%rsp\n", remaining);
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
    if (base == NULL || !enum_variant_location(emitter->module, base->symbol_id,
                                               &enumeration, &enum_index,
                                               &variant_index)) return 0;
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
    if (left != NULL && right != NULL && left->type == TYPE_STRING &&
        right->type == TYPE_STRING)
        return emit_string_compare(emitter, instruction);
    if (left != NULL && right != NULL &&
        (is_floating(left->type) || is_floating(right->type))) {
        DataType operation_type = left->type == TYPE_DOUBLE || right->type == TYPE_DOUBLE
            ? TYPE_DOUBLE : TYPE_FLOAT;
        return emit_floating_binary(emitter, instruction, operation_type);
    }
    write_value_load(emitter, "rax", instruction->operand_a);
    write_value_load(emitter, "rcx", instruction->operand_b);
    switch (instruction->operator_type) {
        case TOKEN_PLUS:
            fputs(emitter->syntax == SYNTAX_INTEL ? "    add rax, rcx\n" :
                                                   "    addq %rcx, %rax\n", emitter->output);
            break;
        case TOKEN_MINUS:
            fputs(emitter->syntax == SYNTAX_INTEL ? "    sub rax, rcx\n" :
                                                   "    subq %rcx, %rax\n", emitter->output);
            break;
        case TOKEN_STAR:
            fputs(emitter->syntax == SYNTAX_INTEL ? "    imul rax, rcx\n" :
                                                   "    imulq %rcx, %rax\n", emitter->output);
            break;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            fputs("    cqo\n", emitter->output);
            fputs(emitter->syntax == SYNTAX_INTEL ? "    idiv rcx\n" :
                                                   "    idivq %rcx\n", emitter->output);
            if (instruction->operator_type == TOKEN_PERCENT)
                fputs(emitter->syntax == SYNTAX_INTEL ? "    mov rax, rdx\n" :
                                                       "    movq %rdx, %rax\n", emitter->output);
            break;
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
        case TOKEN_LESS:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER:
        case TOKEN_GREATER_EQUAL: {
            fputs(emitter->syntax == SYNTAX_INTEL ? "    cmp rax, rcx\n" :
                                                   "    cmpq %rcx, %rax\n", emitter->output);
            const char *condition = "e";
            if (instruction->operator_type == TOKEN_BANG_EQUAL) condition = "ne";
            else if (instruction->operator_type == TOKEN_LESS) condition = "l";
            else if (instruction->operator_type == TOKEN_LESS_EQUAL) condition = "le";
            else if (instruction->operator_type == TOKEN_GREATER) condition = "g";
            else if (instruction->operator_type == TOKEN_GREATER_EQUAL) condition = "ge";
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(emitter->output, "    set%s al\n", condition);
            else
                fprintf(emitter->output, "    set%s %%al\n", condition);
            fputs(emitter->syntax == SYNTAX_INTEL ? "    movzx rax, al\n" :
                                                   "    movzbq %al, %rax\n", emitter->output);
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
                fputs(emitter->syntax == SYNTAX_INTEL ?
                      "    cmp rax, 0\n    setne al\n    movzx rax, al\n" :
                      "    cmpq $0, %rax\n    setne %al\n    movzbq %al, %rax\n",
                      emitter->output);
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
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    cvtsi2sd xmm0, rax\n    movq rax, xmm0\n" :
              "    cvtsi2sdq %rax, %xmm0\n    movq %xmm0, %rax\n", emitter->output);
    } else if (is_integral(from) && to == TYPE_FLOAT) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    cvtsi2ss xmm0, rax\n    movd eax, xmm0\n" :
              "    cvtsi2ssq %rax, %xmm0\n    movd %xmm0, %eax\n", emitter->output);
    } else if (from == TYPE_FLOAT && to == TYPE_DOUBLE) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    movd xmm0, eax\n    cvtss2sd xmm0, xmm0\n    movq rax, xmm0\n" :
              "    movd %eax, %xmm0\n    cvtss2sd %xmm0, %xmm0\n    movq %xmm0, %rax\n", emitter->output);
    } else if (from == TYPE_DOUBLE && to == TYPE_FLOAT) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    movq xmm0, rax\n    cvtsd2ss xmm0, xmm0\n    movd eax, xmm0\n" :
              "    movq %rax, %xmm0\n    cvtsd2ss %xmm0, %xmm0\n    movd %xmm0, %eax\n", emitter->output);
    } else if (from == TYPE_FLOAT && is_integral(to)) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    movd xmm0, eax\n    cvttss2si rax, xmm0\n" :
              "    movd %eax, %xmm0\n    cvttss2siq %xmm0, %rax\n", emitter->output);
    } else if (from == TYPE_DOUBLE && is_integral(to)) {
        fputs(emitter->syntax == SYNTAX_INTEL ?
              "    movq xmm0, rax\n    cvttsd2si rax, xmm0\n" :
              "    movq %rax, %xmm0\n    cvttsd2siq %xmm0, %rax\n", emitter->output);
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
                    (declaration->is_array || is_named_value(emitter->module, declaration))) {
                    size_t offset = declaration_offset(emitter, declaration);
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    lea rax, [rbp - %zu]\n", offset);
                    else
                        fprintf(emitter->output, "    leaq -%zu(%%rbp), %%rax\n", offset);
                } else if (declaration != NULL)
                    write_local_load(emitter, "rax", declaration_offset(emitter, declaration));
                else if (parameter != NULL)
                    write_local_load(emitter, "rax", parameter_offset(emitter, parameter));
                else
                    write_immediate(emitter, "rax", 0);
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_DECLARE: {
            size_t offset = declaration_offset(emitter, instruction);
            if (instruction->is_array || is_named_value(emitter->module, instruction)) {
                write_immediate(emitter, "rax", 0);
                size_t slots = declaration_slots(emitter, instruction);
                for (size_t slot = 0; slot < slots; slot++)
                    write_local_store(emitter, "rax", offset - slot * 8U);
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
            write_typed_indirect_load(emitter, target->type, target->pointer_depth, "rbx");
            if (is_floating(target->type)) {
                if (instruction->operator_type == TOKEN_EQUAL) {
                    const IrInstruction *stored = producer(function, instruction->operand_b);
                    write_value_load(emitter, "rax", instruction->operand_b);
                    convert_rax(emitter, stored->type, target->type);
                } else {
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    mov%c xmm2, %s\n",
                                target->type == TYPE_FLOAT ? 'd' : 'q',
                                target->type == TYPE_FLOAT ? "eax" : "rax");
                    else
                        fprintf(emitter->output, "    mov%c %%%s, %%xmm2\n",
                                target->type == TYPE_FLOAT ? 'd' : 'q',
                                target->type == TYPE_FLOAT ? "eax" : "rax");
                    if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                        instruction->operator_type == TOKEN_MINUS_MINUS) {
                        write_immediate(emitter, "rax", 1);
                        convert_rax(emitter, TYPE_INT, target->type);
                        if (emitter->syntax == SYNTAX_INTEL)
                            fprintf(emitter->output, "    mov%c xmm1, %s\n",
                                    target->type == TYPE_FLOAT ? 'd' : 'q',
                                    target->type == TYPE_FLOAT ? "eax" : "rax");
                        else
                            fprintf(emitter->output, "    mov%c %%%s, %%xmm1\n",
                                    target->type == TYPE_FLOAT ? 'd' : 'q',
                                    target->type == TYPE_FLOAT ? "eax" : "rax");
                    } else {
                        load_floating_value(emitter, instruction->operand_b,
                                            target->type, 1);
                    }
                    const char *operation =
                        (instruction->operator_type == TOKEN_PLUS_EQUAL ||
                         instruction->operator_type == TOKEN_PLUS_PLUS) ? "add" :
                        (instruction->operator_type == TOKEN_MINUS_EQUAL ||
                         instruction->operator_type == TOKEN_MINUS_MINUS) ? "sub" :
                        instruction->operator_type == TOKEN_STAR_EQUAL ? "mul" : "div";
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    %s%s xmm2, xmm1\n", operation,
                                target->type == TYPE_FLOAT ? "ss" : "sd");
                    else
                        fprintf(emitter->output, "    %s%s %%xmm1, %%xmm2\n", operation,
                                target->type == TYPE_FLOAT ? "ss" : "sd");
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    mov%c %s, xmm2\n",
                                target->type == TYPE_FLOAT ? 'd' : 'q',
                                target->type == TYPE_FLOAT ? "eax" : "rax");
                    else
                        fprintf(emitter->output, "    mov%c %%xmm2, %%%s\n",
                                target->type == TYPE_FLOAT ? 'd' : 'q',
                                target->type == TYPE_FLOAT ? "eax" : "rax");
                }
            } else if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                instruction->operator_type == TOKEN_MINUS_MINUS) {
                if (emitter->syntax == SYNTAX_INTEL)
                    fprintf(emitter->output, "    %s rax\n",
                            instruction->operator_type == TOKEN_PLUS_PLUS ? "inc" : "dec");
                else
                    fprintf(emitter->output, "    %sq %%rax\n",
                            instruction->operator_type == TOKEN_PLUS_PLUS ? "inc" : "dec");
            } else {
                write_value_load(emitter, "rcx", instruction->operand_b);
                if (instruction->operator_type == TOKEN_EQUAL)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    mov rax, rcx\n" :
                                                           "    movq %rcx, %rax\n", emitter->output);
                else if (instruction->operator_type == TOKEN_PLUS_EQUAL)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    add rax, rcx\n" :
                                                           "    addq %rcx, %rax\n", emitter->output);
                else if (instruction->operator_type == TOKEN_MINUS_EQUAL)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    sub rax, rcx\n" :
                                                           "    subq %rcx, %rax\n", emitter->output);
                else if (instruction->operator_type == TOKEN_STAR_EQUAL)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    imul rax, rcx\n" :
                                                           "    imulq %rcx, %rax\n", emitter->output);
                else if (instruction->operator_type == TOKEN_SLASH_EQUAL) {
                    fputs("    cqo\n", emitter->output);
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    idiv rcx\n" :
                                                           "    idivq %rcx\n", emitter->output);
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
                if (emitter->syntax == SYNTAX_INTEL) fputs("    mov rax, rbx\n", emitter->output);
                else fputs("    movq %rbx, %rax\n", emitter->output);
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
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    xor eax, 0x80000000\n" :
                                                           "    xorl $0x80000000, %eax\n",
                          emitter->output);
                else if (instruction->type == TYPE_DOUBLE)
                    fputs(emitter->syntax == SYNTAX_INTEL ?
                          "    movabs rcx, 0x8000000000000000\n    xor rax, rcx\n" :
                          "    movabsq $0x8000000000000000, %rcx\n    xorq %rcx, %rax\n",
                          emitter->output);
                else
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    neg rax\n" :
                                                           "    negq %rax\n", emitter->output);
            }
            else {
                fputs(emitter->syntax == SYNTAX_INTEL ?
                      "    cmp rax, 0\n    sete al\n    movzx rax, al\n" :
                      "    cmpq $0, %rax\n    sete %al\n    movzbq %al, %rax\n", emitter->output);
            }
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_BINARY:
            return emit_binary(emitter, instruction);
        case IR_OP_PRINT: {
            const IrInstruction *value = producer(function, instruction->operand_a);
            const char *first = emitter->target == TARGET_COFF ? "rcx" : "rdi";
            const char *second = emitter->target == TARGET_COFF ? "rdx" : "rsi";
            if (value->type == TYPE_STRING) {
                write_value_load(emitter, "rax", instruction->operand_a);
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    mov %s, rax\n", first);
                    else
                        fprintf(emitter->output, "    movq %%rax, %%%s\n", first);
                    write_call(emitter, "puts");
                } else {
                    write_address(emitter, first, ".LIR_string_format_", 0);
                    if (emitter->syntax == SYNTAX_INTEL)
                        fprintf(emitter->output, "    mov %s, rax\n", second);
                    else
                        fprintf(emitter->output, "    movq %%rax, %%%s\n", second);
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    xor eax, eax\n" :
                                                           "    xorl %eax, %eax\n", emitter->output);
                    write_call(emitter, "printf");
                }
            } else if (is_floating(value->type)) {
                write_address(emitter, first, ".LIR_float_format_", 0);
                write_value_load(emitter, "rax", instruction->operand_a);
                convert_rax(emitter, value->type, TYPE_DOUBLE);
                if (emitter->syntax == SYNTAX_INTEL)
                    fprintf(emitter->output, "    movq xmm%d, rax\n",
                            emitter->target == TARGET_COFF ? 1 : 0);
                else
                    fprintf(emitter->output, "    movq %%rax, %%xmm%d\n",
                            emitter->target == TARGET_COFF ? 1 : 0);
                if (emitter->target == TARGET_COFF)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    mov rdx, rax\n" :
                                                           "    movq %rax, %rdx\n", emitter->output);
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
                fputs(emitter->syntax == SYNTAX_INTEL ? "    xor eax, eax\n" :
                                                       "    xorl %eax, %eax\n", emitter->output);
                write_call(emitter, "printf");
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_immediate(emitter, first, 10);
                    write_call(emitter, "putchar");
                }
            }
            return 1;
        }
        case IR_OP_RETURN:
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else {
                const IrInstruction *value = producer(function, instruction->operand_a);
                DataType return_type = ir_ast_type(function->source_program,
                                                   &function->return_type);
                write_value_load(emitter, "rax", instruction->operand_a);
                convert_rax(emitter, value->type, return_type);
                if (return_type == TYPE_FLOAT)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    movd xmm0, eax\n" :
                                                           "    movd %eax, %xmm0\n", emitter->output);
                else if (return_type == TYPE_DOUBLE)
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    movq xmm0, rax\n" :
                                                           "    movq %rax, %xmm0\n", emitter->output);
            }
            fprintf(emitter->output, "    jmp .LIR_epilogue_%zu\n", emitter->function_index);
            return 1;
        case IR_OP_BRANCH:
            write_value_load(emitter, "rax", instruction->operand_a);
            fputs(emitter->syntax == SYNTAX_INTEL ? "    cmp rax, 0\n" :
                                                   "    cmpq $0, %rax\n", emitter->output);
            fprintf(emitter->output, "    jne .LIR_%zu_%zu\n    jmp .LIR_%zu_%zu\n",
                    emitter->function_index, instruction->target_a,
                    emitter->function_index, instruction->target_b);
            return 1;
        case IR_OP_JUMP:
            emit_phi_moves(emitter, instruction->target_a);
            fprintf(emitter->output, "    jmp .LIR_%zu_%zu\n", emitter->function_index,
                    instruction->target_a);
            return 1;
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
                    fputs(emitter->syntax == SYNTAX_INTEL ?
                          "    movd xmm0, eax\n    xorps xmm1, xmm1\n    ucomiss xmm0, xmm1\n" :
                          "    movd %eax, %xmm0\n    xorps %xmm1, %xmm1\n    ucomiss %xmm1, %xmm0\n",
                          emitter->output);
                else
                    fputs(emitter->syntax == SYNTAX_INTEL ?
                          "    movq xmm0, rax\n    xorpd xmm1, xmm1\n    ucomisd xmm0, xmm1\n" :
                          "    movq %rax, %xmm0\n    xorpd %xmm1, %xmm1\n    ucomisd %xmm1, %xmm0\n",
                          emitter->output);
                fputs(emitter->syntax == SYNTAX_INTEL ?
                      "    setne al\n    setp dl\n    or al, dl\n    movzx rax, al\n" :
                      "    setne %al\n    setp %dl\n    orb %dl, %al\n    movzbq %al, %rax\n",
                      emitter->output);
            } else {
                convert_rax(emitter, source->type, instruction->type);
                if (instruction->type == TYPE_BIT)
                    fputs(emitter->syntax == SYNTAX_INTEL ?
                          "    test rax, rax\n    setne al\n    movzx rax, al\n" :
                          "    testq %rax, %rax\n    setne %al\n    movzbq %al, %rax\n",
                          emitter->output);
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
            write_typed_indirect_load(emitter, instruction->type,
                                      instruction->pointer_depth, "rbx");
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        case IR_OP_MEMBER:
            if (emit_enum_member(emitter, instruction)) return 1;
            if (!emit_lvalue_address(emitter, instruction, index)) return 0;
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
            return 0;
    }
    return 0;
}

static int emit_function(Emitter *emitter) {
    FILE *output = emitter->output;
    const char *name = ast_program_lexeme(emitter->function->source_program,
                                          emitter->function->name_token);
    emitter->current_label = IR_VALUE_NONE;
    emitter->bounds_sequence = 0;
    fprintf(output, "    .globl %s\n", name);
    if (emitter->target == TARGET_COFF)
        fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", name);
    else
        fprintf(output, "    .type %s, @function\n", name);
    fprintf(output, "%s:\n", name);
    if (emitter->syntax == SYNTAX_INTEL) {
        fputs("    push rbp\n    mov rbp, rsp\n    push rbx\n", output);
    } else {
        fputs("    pushq %rbp\n    movq %rsp, %rbp\n    pushq %rbx\n", output);
    }
    write_frame_allocation(emitter);
    for (size_t p = 0; p < emitter->function->parameter_count; p++) {
        const IrParameter *parameter = &emitter->function->parameters[p];
        size_t register_index = parameter_register_index(emitter->function,
                                                         emitter->target, p);
        if (register_index == IR_VALUE_NONE) {
            size_t first_stack_offset = emitter->target == TARGET_COFF ? 48U : 16U;
            write_positive_frame_load(emitter, "rax", first_stack_offset +
                stack_parameter_index(emitter->function, emitter->target, p) * 8U);
        } else if (is_floating(parameter->type)) {
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(output, "    mov%c %s, xmm%zu\n",
                        parameter->type == TYPE_FLOAT ? 'd' : 'q',
                        parameter->type == TYPE_FLOAT ? "eax" : "rax", register_index);
            else
                fprintf(output, "    mov%c %%xmm%zu, %%%s\n",
                        parameter->type == TYPE_FLOAT ? 'd' : 'q', register_index,
                        parameter->type == TYPE_FLOAT ? "eax" : "rax");
        } else {
            if (emitter->syntax == SYNTAX_INTEL)
                fprintf(output, "    mov rax, %s\n",
                        argument_register(emitter->target, register_index));
            else
                fprintf(output, "    movq %%%s, %%rax\n",
                        argument_register(emitter->target, register_index));
        }
        if (is_integral(parameter->type))
            normalize_integral_parameter(emitter, parameter->type);
        write_local_store(emitter, "rax", parameter_offset(emitter, parameter));
    }
    for (size_t i = 0; i < emitter->function->instruction_count; i++)
        if (!emit_instruction(emitter, &emitter->function->instructions[i], i)) return 0;
    write_immediate(emitter, "rax", 0);
    fprintf(output, ".LIR_epilogue_%zu:\n", emitter->function_index);
    fputs(emitter->syntax == SYNTAX_INTEL ?
          "    lea rsp, [rbp - 8]\n    pop rbx\n    pop rbp\n    ret\n" :
          "    leaq -8(%rbp), %rsp\n    popq %rbx\n    popq %rbp\n    ret\n", output);
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
              "    .def calloc; .scl 2; .type 32; .endef\n"
              "    .def free; .scl 2; .type 32; .endef\n"
              "    .def strlen; .scl 2; .type 32; .endef\n"
              "    .def _write; .scl 2; .type 32; .endef\n"
              "    .def _read; .scl 2; .type 32; .endef\n"
              "    .def _open; .scl 2; .type 32; .endef\n"
              "    .def _close; .scl 2; .type 32; .endef\n"
              "    .section .rdata,\"dr\"\n", output);
    } else {
        fputs("    .section .rodata\n", output);
    }
    fputs(".LIR_int_format_0:\n    .ascii \"%lld\\0\"\n"
          ".LIR_char_format_0:\n    .ascii \"%c\\0\"\n"
          ".LIR_float_format_0:\n    .ascii \"%f\\0\"\n"
          ".LIR_string_format_0:\n    .ascii \"%s\\0\"\n", output);
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
    }
    fputs("    .text\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        size_t declarations = 0;
        for (size_t i = 0; i < function->instruction_count; i++)
            if (function->instructions[i].opcode == IR_OP_DECLARE) {
                size_t slots = type_slots(emitter->module, function->instructions[i].type_id);
                if (slots == 0 || declarations > SIZE_MAX - slots) return 0;
                declarations += slots;
            }
        size_t slots = function->next_value + function->parameter_count + declarations;
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
                        const char *output_path) {
    if (!supported_module(module) || output_path == NULL) return 0;
    FILE *output = fopen(output_path, "w");
    if (output == NULL) return 0;
    Emitter emitter = {
        .module = module,
        .target = target,
        .syntax = syntax,
        .output = output
    };
    int success = emit_file(&emitter, deterministic);
    if (fclose(output) != 0) success = 0;
    if (!success) (void) remove(output_path);
    return success;
}
