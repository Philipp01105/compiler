#include "ir_emitter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const IrModule *module;
    const IrFunction *function;
    TargetFormat target;
    SyntaxMode syntax;
    FILE *output;
    size_t function_index;
    size_t declaration_count;
    size_t frame_size;
} Emitter;

#define IR_NATIVE_MAX_LOCALS 400U

static int is_integral(DataType type) {
    return type == TYPE_INT || type == TYPE_CHAR || type == TYPE_BYTE ||
           type == TYPE_BIT;
}

static DataType ir_ast_type(const AstProgram *program, const AstType *type) {
    if (type == NULL || type->name_token >= program->token_count) return TYPE_UNKNOWN;
    switch (program->tokens[type->name_token].type) {
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
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

static int supported_module(const IrModule *module) {
    if (module == NULL || !module->verified || module->program == NULL ||
        module->semantics == NULL || module->function_count == 0) return 0;
    const SemanticSymbol *main_symbol = semantic_find_global(module->semantics, "main",
                                                              SEMANTIC_SYMBOL_FUNCTION);
    if (main_symbol == NULL || main_symbol->declaration == NULL ||
        main_symbol->declaration->as.function.parameters != NULL) return 0;
    for (const AstDeclarationNode *declaration = module->program->root;
         declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_IMPORT || declaration->kind == AST_DECL_INVALID)
            return 0;
    if (module->function_count == 1 &&
        (module->program->root == NULL || module->program->root->next != NULL ||
         module->program->root->kind != AST_DECL_FUNCTION)) return 0;
    int module_has_call = 0;
    int single_function_reason = 0;
    for (size_t f = 0; f < module->function_count; f++) {
      const IrFunction *function = &module->functions[f];
      if (function->owner_token != AST_TOKEN_NONE || function->parameter_count > 4 ||
          function->return_type.name_token >= module->program->token_count ||
          maximum_loop_depth(module->semantics->symbols[function->symbol_id].declaration
                                 ->as.function.body, 0) > 100U) return 0;
      int is_main = strcmp(ast_program_lexeme(module->program, function->name_token), "main") == 0;
      DataType return_type = ir_ast_type(module->program, &function->return_type);
      if ((is_main && return_type != TYPE_VOID) ||
          (!is_main && !is_integral(return_type))) return 0;
      for (size_t p = 0; p < function->parameter_count; p++)
          if (!is_integral(function->parameters[p].type) ||
              function->parameters[p].pointer_depth != 0 || function->parameters[p].is_array)
              return 0;
      size_t declarations = 0;
      for (size_t i = 0; i < function->instruction_count; i++) {
        const IrInstruction *instruction = &function->instructions[i];
        if (instruction->result != IR_VALUE_NONE &&
            producer(function, instruction->result) != instruction) return 0;
        switch (instruction->opcode) {
            case IR_OP_CONSTANT:
                if ((!is_integral(instruction->type) && instruction->type != TYPE_STRING) ||
                    instruction->auxiliary_token >= module->program->token_count) return 0;
                break;
            case IR_OP_LOAD: {
                if (!is_integral(instruction->type) ||
                    instruction->auxiliary_token >= module->program->token_count) return 0;
                const char *name = ast_program_lexeme(module->program,
                                                      instruction->auxiliary_token);
                if (strcmp(name, "true") != 0 && strcmp(name, "false") != 0 &&
                    local_declaration(function, instruction->symbol_id, i) == NULL &&
                    function_parameter(function, instruction->symbol_id) == NULL &&
                    called_function(module, instruction->symbol_id) == NULL)
                    return 0;
                break;
            }
            case IR_OP_DECLARE:
                if (!is_integral(instruction->type) || instruction->pointer_depth != 0 ||
                    instruction->is_array ||
                    instruction->auxiliary_token >= module->program->token_count ||
                    (instruction->operand_a != IR_VALUE_NONE &&
                     (producer(function, instruction->operand_a) == NULL ||
                      !is_integral(producer(function, instruction->operand_a)->type)))) return 0;
                if (instruction->symbol_id == AST_SYMBOL_NONE) return 0;
                for (size_t j = 0; j < i; j++)
                    if (function->instructions[j].opcode == IR_OP_DECLARE &&
                        function->instructions[j].symbol_id == instruction->symbol_id) return 0;
                declarations++;
                if (declarations > IR_NATIVE_MAX_LOCALS) return 0;
                break;
            case IR_OP_STORE: {
                const IrInstruction *target = producer(function, instruction->operand_a);
                if (target == NULL || target->opcode != IR_OP_LOAD ||
                    (local_declaration(function, target->symbol_id, i) == NULL &&
                     function_parameter(function, target->symbol_id) == NULL))
                    return 0;
                if (instruction->operator_type != TOKEN_PLUS_PLUS &&
                    instruction->operator_type != TOKEN_MINUS_MINUS &&
                    (producer(function, instruction->operand_b) == NULL ||
                     !is_integral(producer(function, instruction->operand_b)->type))) return 0;
                break;
            }
            case IR_OP_UNARY:
                if (!is_integral(instruction->type) ||
                    (instruction->operator_type != TOKEN_MINUS &&
                     instruction->operator_type != TOKEN_BANG) ||
                    producer(function, instruction->operand_b) == NULL ||
                    (instruction->operator_type == TOKEN_MINUS &&
                     producer(function, instruction->operand_b)->type == TYPE_BIT)) return 0;
                break;
            case IR_OP_BINARY:
                if (!is_integral(instruction->type) ||
                    instruction->operator_type == TOKEN_AMP_AMP ||
                    instruction->operator_type == TOKEN_PIPE_PIPE ||
                    producer(function, instruction->operand_a) == NULL ||
                    producer(function, instruction->operand_b) == NULL) return 0;
                if (!is_integral(producer(function, instruction->operand_a)->type) ||
                    !is_integral(producer(function, instruction->operand_b)->type)) return 0;
                if (instruction->operator_type >= TOKEN_PLUS &&
                    instruction->operator_type <= TOKEN_PERCENT &&
                    (producer(function, instruction->operand_a)->type == TYPE_BIT ||
                     producer(function, instruction->operand_b)->type == TYPE_BIT)) return 0;
                break;
            case IR_OP_PRINT: {
                const IrInstruction *value = producer(function, instruction->operand_a);
                if (value == NULL || (!is_integral(value->type) &&
                    !(value->opcode == IR_OP_CONSTANT && value->type == TYPE_STRING))) return 0;
                if (value->type == TYPE_STRING) single_function_reason = 1;
                break;
            }
            case IR_OP_RETURN:
                if ((is_main && instruction->operand_a != IR_VALUE_NONE) ||
                    (!is_main && (instruction->operand_a == IR_VALUE_NONE ||
                     producer(function, instruction->operand_a) == NULL ||
                     !is_integral(producer(function, instruction->operand_a)->type)))) return 0;
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
                if (callee == NULL || instruction->argument_count != callee->parameter_count ||
                    instruction->argument_count > 4 || !is_integral(instruction->type)) return 0;
                for (size_t a = 0; a < instruction->argument_count; a++) {
                    const IrInstruction *argument = producer(function,
                        function->arguments[instruction->first_argument + a]);
                    if (argument == NULL || !is_integral(argument->type)) return 0;
                }
                module_has_call = 1;
                break;
            }
            case IR_OP_INDEX:
            case IR_OP_MEMBER:
            case IR_OP_CAST:
            case IR_OP_ALLOC:
            case IR_OP_FREE:
                return 0;
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

static size_t declaration_offset(const Emitter *emitter,
                                 const IrInstruction *declaration) {
    size_t ordinal = 0;
    for (size_t i = 0; i < emitter->function->instruction_count; i++) {
        const IrInstruction *candidate = &emitter->function->instructions[i];
        if (candidate->opcode != IR_OP_DECLARE) continue;
        if (candidate == declaration)
            return (emitter->function->next_value + emitter->function->parameter_count +
                    ordinal + 2U) * 8U;
        ordinal++;
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
    static const char *sysv[] = {"rdi", "rsi", "rdx", "rcx"};
    static const char *coff[] = {"rcx", "rdx", "r8", "r9"};
    return target == TARGET_COFF ? coff[index] : sysv[index];
}

static long long constant_value(const AstProgram *program,
                                const IrInstruction *instruction) {
    const AstToken *token = ast_program_token(program, instruction->auxiliary_token);
    if (token == NULL) return 0;
    if (token->type == TOKEN_CHAR_LITERAL) return (unsigned char) token->lexeme[0];
    return strtoll(token->lexeme, NULL, 10);
}

static int emit_binary(Emitter *emitter, const IrInstruction *instruction) {
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

static int emit_instruction(Emitter *emitter, const IrInstruction *instruction,
                            size_t index) {
    const IrFunction *function = emitter->function;
    switch (instruction->opcode) {
        case IR_OP_CONSTANT:
            if (instruction->type != TYPE_STRING) {
                write_immediate(emitter, "rax", constant_value(emitter->module->program,
                                                                instruction));
                write_value_store(emitter, "rax", instruction->result);
            }
            return 1;
        case IR_OP_LOAD: {
            const char *name = ast_program_lexeme(emitter->module->program,
                                                  instruction->auxiliary_token);
            if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0)
                write_immediate(emitter, "rax", strcmp(name, "true") == 0 ? 1 : 0);
            else {
                const IrInstruction *declaration = local_declaration(function,
                    instruction->symbol_id, index);
                const IrParameter *parameter = function_parameter(function,
                    instruction->symbol_id);
                if (declaration != NULL)
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
            if (instruction->operand_a == IR_VALUE_NONE) write_immediate(emitter, "rax", 0);
            else write_value_load(emitter, "rax", instruction->operand_a);
            write_local_store(emitter, "rax", offset);
            return 1;
        }
        case IR_OP_STORE: {
            const IrInstruction *target = producer(function, instruction->operand_a);
            const IrInstruction *declaration = local_declaration(function,
                target->symbol_id, index);
            const IrParameter *parameter = function_parameter(function, target->symbol_id);
            size_t offset = declaration != NULL ? declaration_offset(emitter, declaration) :
                            parameter_offset(emitter, parameter);
            write_local_load(emitter, "rax", offset);
            if (instruction->operator_type == TOKEN_PLUS_PLUS ||
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
            write_local_store(emitter, "rax", offset);
            return 1;
        }
        case IR_OP_UNARY:
            write_value_load(emitter, "rax", instruction->operand_b);
            if (instruction->operator_type == TOKEN_MINUS)
                fputs(emitter->syntax == SYNTAX_INTEL ? "    neg rax\n" :
                                                       "    negq %rax\n", emitter->output);
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
                char string_label[64];
                (void) snprintf(string_label, sizeof(string_label), ".LIR_string_%zu_",
                                emitter->function_index);
                if (instruction->operator_type == TOKEN_KEYWORD_PRINTLINE) {
                    write_address(emitter, first, string_label, value->result);
                    write_call(emitter, "puts");
                } else {
                    write_address(emitter, first, ".LIR_string_format_", 0);
                    write_address(emitter, second, string_label, value->result);
                    fputs(emitter->syntax == SYNTAX_INTEL ? "    xor eax, eax\n" :
                                                           "    xorl %eax, %eax\n", emitter->output);
                    write_call(emitter, "printf");
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
            else write_value_load(emitter, "rax", instruction->operand_a);
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
            fprintf(emitter->output, "    jmp .LIR_%zu_%zu\n", emitter->function_index,
                    instruction->target_a);
            return 1;
        case IR_OP_LABEL:
            fprintf(emitter->output, ".LIR_%zu_%zu:\n", emitter->function_index,
                    instruction->target_a);
            return 1;
        case IR_OP_CALL: {
            const IrFunction *callee = called_function(emitter->module, instruction->symbol_id);
            for (size_t a = 0; a < instruction->argument_count; a++)
                write_value_load(emitter, argument_register(emitter->target, a),
                    function->arguments[instruction->first_argument + a]);
            write_call(emitter, ast_program_lexeme(emitter->module->program,
                                                   callee->name_token));
            write_value_store(emitter, "rax", instruction->result);
            return 1;
        }
        case IR_OP_INDEX:
        case IR_OP_MEMBER:
        case IR_OP_CAST:
        case IR_OP_ALLOC:
        case IR_OP_FREE:
            return 0;
    }
    return 0;
}

static int emit_function(Emitter *emitter) {
    FILE *output = emitter->output;
    const char *name = ast_program_lexeme(emitter->module->program,
                                          emitter->function->name_token);
    fprintf(output, "    .globl %s\n", name);
    if (emitter->target == TARGET_COFF)
        fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", name);
    else
        fprintf(output, "    .type %s, @function\n", name);
    fprintf(output, "%s:\n", name);
    if (emitter->syntax == SYNTAX_INTEL) {
        fputs("    push rbp\n    mov rbp, rsp\n    push rbx\n", output);
        if (emitter->frame_size != 0) fprintf(output, "    sub rsp, %zu\n", emitter->frame_size);
    } else {
        fputs("    pushq %rbp\n    movq %rsp, %rbp\n    pushq %rbx\n", output);
        if (emitter->frame_size != 0) fprintf(output, "    subq $%zu, %%rsp\n", emitter->frame_size);
    }
    for (size_t p = 0; p < emitter->function->parameter_count; p++)
        write_local_store(emitter, argument_register(emitter->target, p),
                          parameter_offset(emitter, &emitter->function->parameters[p]));
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
              "    .section .rdata,\"dr\"\n", output);
    } else {
        fputs("    .section .rodata\n", output);
    }
    fputs(".LIR_int_format_0:\n    .ascii \"%lld\\0\"\n"
          ".LIR_char_format_0:\n    .ascii \"%c\\0\"\n"
          ".LIR_string_format_0:\n    .ascii \"%s\\0\"\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            if (instruction->opcode == IR_OP_CONSTANT && instruction->type == TYPE_STRING) {
                fprintf(output, ".LIR_string_%zu_%zu:\n    .ascii \"", f,
                        instruction->result);
                write_escaped(output, ast_program_lexeme(emitter->module->program,
                                                         instruction->auxiliary_token));
                fputs("\\0\"\n", output);
            }
        }
    }
    fputs("    .text\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        size_t declarations = 0;
        for (size_t i = 0; i < function->instruction_count; i++)
            if (function->instructions[i].opcode == IR_OP_DECLARE) declarations++;
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
