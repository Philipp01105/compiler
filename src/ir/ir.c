#include "ir.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    IrModule *module;
    IrFunction *function;
    size_t break_label;
    size_t continue_label;
    int failed;
} IrBuilder;

static int grow_array(void **items, size_t *capacity, size_t item_size) {
    size_t next = *capacity == 0 ? 32 : *capacity * 2;
    if (next < *capacity || (item_size != 0 && next > SIZE_MAX / item_size)) return 0;
    void *grown = realloc(*items, next * item_size);
    if (grown == NULL) return 0;
    *items = grown;
    *capacity = next;
    return 1;
}

static IrInstruction *emit(IrBuilder *builder, IrOpcode opcode, AstSourceSpan span) {
    IrFunction *function = builder->function;
    if (function->instruction_count == function->instruction_capacity &&
        !grow_array((void **) &function->instructions, &function->instruction_capacity,
                    sizeof(*function->instructions))) {
        builder->failed = 1;
        return NULL;
    }
    IrInstruction *instruction = &function->instructions[function->instruction_count++];
    *instruction = (IrInstruction) {
        .opcode = opcode,
        .span = span,
        .type = TYPE_VOID,
        .type_name_token = AST_TOKEN_NONE,
        .result = IR_VALUE_NONE,
        .operand_a = IR_VALUE_NONE,
        .operand_b = IR_VALUE_NONE,
        .auxiliary_token = AST_TOKEN_NONE,
        .symbol_id = AST_SYMBOL_NONE,
        .first_argument = IR_VALUE_NONE,
        .target_a = IR_VALUE_NONE,
        .target_b = IR_VALUE_NONE
    };
    return instruction;
}

static size_t new_value(IrBuilder *builder) {
    if (builder->function->next_value == SIZE_MAX) {
        builder->failed = 1;
        return IR_VALUE_NONE;
    }
    return builder->function->next_value++;
}

static size_t new_label(IrBuilder *builder) {
    if (builder->function->next_label == SIZE_MAX) {
        builder->failed = 1;
        return IR_VALUE_NONE;
    }
    return builder->function->next_label++;
}

static int append_argument(IrBuilder *builder, size_t value) {
    IrFunction *function = builder->function;
    if (function->argument_count == function->argument_capacity &&
        !grow_array((void **) &function->arguments, &function->argument_capacity,
                    sizeof(*function->arguments))) {
        builder->failed = 1;
        return 0;
    }
    function->arguments[function->argument_count++] = value;
    return 1;
}

static DataType ast_type_data_type(const AstProgram *program, const AstType *type) {
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

static size_t lower_expression(IrBuilder *builder, const AstExpression *expression) {
    if (expression == NULL) return IR_VALUE_NONE;
    size_t left = lower_expression(builder, expression->left);
    size_t right = lower_expression(builder, expression->right);
    IrOpcode opcode = IR_OP_CONSTANT;
    switch (expression->kind) {
        case AST_EXPR_LITERAL: opcode = IR_OP_CONSTANT; break;
        case AST_EXPR_NAME: opcode = IR_OP_LOAD; break;
        case AST_EXPR_UNARY: opcode = IR_OP_UNARY; break;
        case AST_EXPR_BINARY: opcode = IR_OP_BINARY; break;
        case AST_EXPR_CALL: opcode = IR_OP_CALL; break;
        case AST_EXPR_INDEX: opcode = IR_OP_INDEX; break;
        case AST_EXPR_MEMBER: opcode = IR_OP_MEMBER; break;
        case AST_EXPR_RESERVE: opcode = IR_OP_RESERVE; break;
        case AST_EXPR_ERROR: builder->failed = 1; return IR_VALUE_NONE;
    }
    size_t first_argument = IR_VALUE_NONE;
    size_t argument_count = 0;
    if (expression->kind == AST_EXPR_CALL) {
        first_argument = builder->function->argument_count;
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next) {
            size_t value = lower_expression(builder, argument);
            if (!append_argument(builder, value)) return IR_VALUE_NONE;
            argument_count++;
        }
    }
    IrInstruction *instruction = emit(builder, opcode, expression->span);
    if (instruction == NULL) return IR_VALUE_NONE;
    instruction->result = new_value(builder);
    instruction->operand_a = left;
    instruction->operand_b = right;
    instruction->auxiliary_token = expression->value_token;
    instruction->symbol_id = expression->resolved_symbol_id;
    instruction->operator_type = expression->operator_type;
    instruction->type = expression->resolved_type;
    instruction->pointer_depth = expression->resolved_pointer_depth;
    instruction->type_name_token = expression->resolved_named_type_token;
    instruction->is_array = expression->resolved_is_array;
    if (expression->kind == AST_EXPR_CALL) {
        instruction->first_argument = first_argument;
        instruction->argument_count = argument_count;
    }
    return instruction->result;
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement);

static void emit_label(IrBuilder *builder, size_t label, AstSourceSpan span) {
    IrInstruction *instruction = emit(builder, IR_OP_LABEL, span);
    if (instruction != NULL) instruction->target_a = label;
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement) {
    for (; statement != NULL && !builder->failed; statement = statement->next) {
        if (statement->kind == AST_STMT_BLOCK) {
            lower_statement(builder, statement->body);
        } else if (statement->kind == AST_STMT_VARIABLE) {
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_DECLARE, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = value;
                instruction->auxiliary_token = statement->name_token;
                for (size_t i = 0; i < builder->module->semantics->symbol_count; i++) {
                    const SemanticSymbol *symbol = &builder->module->semantics->symbols[i];
                    if (symbol->kind == SEMANTIC_SYMBOL_LOCAL &&
                        symbol->name_token == statement->name_token) {
                        instruction->symbol_id = symbol->id;
                        break;
                    }
                }
                instruction->type = ast_type_data_type(builder->module->program, &statement->type);
                instruction->pointer_depth = statement->type.pointer_depth;
                instruction->type_name_token = statement->type.name_token;
                instruction->is_array = statement->type.is_array;
            }
        } else if (statement->kind == AST_STMT_ASSIGNMENT) {
            size_t target = lower_expression(builder, statement->expression);
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_STORE, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = target;
                instruction->operand_b = value;
                instruction->operator_type = statement->assignment_operator;
            }
        } else if (statement->kind == AST_STMT_EXPRESSION) {
            (void) lower_expression(builder, statement->expression);
        } else if (statement->kind == AST_STMT_PRINT) {
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_PRINT, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = value;
                instruction->operator_type = statement->print_newline
                    ? TOKEN_KEYWORD_PRINTLINE : TOKEN_KEYWORD_PRINT;
            }
        } else if (statement->kind == AST_STMT_RETURN) {
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_RETURN, statement->span);
            if (instruction != NULL) instruction->operand_a = value;
        } else if (statement->kind == AST_STMT_BREAK ||
                   statement->kind == AST_STMT_CONTINUE) {
            size_t target = statement->kind == AST_STMT_BREAK
                ? builder->break_label : builder->continue_label;
            /* An unresolved target is retained for sema/diagnostic recovery;
               valid modules always resolve it to an enclosing loop label. */
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = target;
        } else if (statement->kind == AST_STMT_IF) {
            size_t then_label = new_label(builder);
            size_t else_label = new_label(builder);
            size_t end_label = new_label(builder);
            size_t condition = lower_expression(builder, statement->condition);
            IrInstruction *branch = emit(builder, IR_OP_BRANCH, statement->span);
            if (branch != NULL) {
                branch->operand_a = condition;
                branch->target_a = then_label;
                branch->target_b = statement->else_body == NULL ? end_label : else_label;
            }
            emit_label(builder, then_label, statement->span);
            lower_statement(builder, statement->body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = end_label;
            if (statement->else_body != NULL) {
                emit_label(builder, else_label, statement->span);
                lower_statement(builder, statement->else_body);
            }
            emit_label(builder, end_label, statement->span);
        } else if (statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR) {
            size_t condition_label = new_label(builder);
            size_t body_label = new_label(builder);
            size_t update_label = statement->kind == AST_STMT_FOR
                ? new_label(builder) : condition_label;
            size_t end_label = new_label(builder);
            size_t saved_break = builder->break_label;
            size_t saved_continue = builder->continue_label;
            builder->break_label = end_label;
            builder->continue_label = update_label;
            lower_statement(builder, statement->initializer);
            emit_label(builder, condition_label, statement->span);
            size_t condition = lower_expression(builder, statement->condition);
            if (statement->condition == NULL) {
                IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
                if (jump != NULL) jump->target_a = body_label;
            } else {
                IrInstruction *branch = emit(builder, IR_OP_BRANCH, statement->span);
                if (branch != NULL) {
                    branch->operand_a = condition;
                    branch->target_a = body_label;
                    branch->target_b = end_label;
                }
            }
            emit_label(builder, body_label, statement->span);
            lower_statement(builder, statement->body);
            if (statement->kind == AST_STMT_FOR) emit_label(builder, update_label, statement->span);
            lower_statement(builder, statement->else_body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = condition_label;
            emit_label(builder, end_label, statement->span);
            builder->break_label = saved_break;
            builder->continue_label = saved_continue;
        }
    }
}

static int append_function(IrModule *module, const AstDeclarationNode *declaration) {
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions))) return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction) {
        .name_token = declaration->name_token,
        .owner_token = declaration->as.function.owner_token,
        .symbol_id = AST_SYMBOL_NONE,
        .return_type = declaration->as.function.return_type
    };
    for (size_t i = 0; i < module->semantics->symbol_count; i++) {
        const SemanticSymbol *symbol = &module->semantics->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration == declaration) {
            function->symbol_id = symbol->id;
            break;
        }
    }
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        function->parameter_count++;
    if (function->parameter_count != 0) {
        function->parameters = calloc(function->parameter_count, sizeof(*function->parameters));
        if (function->parameters == NULL) return 0;
    }
    size_t parameter_index = 0;
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next, parameter_index++) {
        IrParameter *ir_parameter = &function->parameters[parameter_index];
        *ir_parameter = (IrParameter) {
            .name_token = parameter->name_token,
            .symbol_id = AST_SYMBOL_NONE,
            .type = ast_type_data_type(module->program, &parameter->type),
            .pointer_depth = parameter->type.pointer_depth,
            .type_name_token = parameter->type.name_token,
            .is_array = parameter->type.is_array || parameter->is_array
        };
        for (size_t i = 0; i < module->semantics->symbol_count; i++) {
            const SemanticSymbol *symbol = &module->semantics->symbols[i];
            if (symbol->kind == SEMANTIC_SYMBOL_PARAMETER &&
                symbol->owner_token == declaration->name_token &&
                symbol->name_token == parameter->name_token) {
                ir_parameter->symbol_id = symbol->id;
                break;
            }
        }
    }
    IrBuilder builder = {
        .module = module,
        .function = function,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    lower_statement(&builder, declaration->as.function.body);
    return !builder.failed;
}

IrModule *ir_lower_program(const AstProgram *program, const SemanticModel *semantics) {
    if (program == NULL || semantics == NULL || semantics->program != program ||
        !program->structured_ast_complete) return NULL;
    IrModule *module = calloc(1, sizeof(*module));
    if (module == NULL) return NULL;
    module->program = program;
    module->semantics = semantics;
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->kind == AST_DECL_FUNCTION && !append_function(module, declaration))
            goto failure;
        if (declaration->kind == AST_DECL_STRUCT) {
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                if (!append_function(module, method)) goto failure;
        }
    }
    if (!ir_verify_module(module)) goto failure;
    module->verified = 1;
    return module;

failure:
    ir_module_free(module);
    return NULL;
}

IrModule *ir_create_compatibility_module(const AstProgram *program) {
    if (program == NULL) return NULL;
    IrModule *module = calloc(1, sizeof(*module));
    if (module != NULL) module->program = program;
    return module;
}

static int opcode_produces_value(IrOpcode opcode) {
    return opcode == IR_OP_CONSTANT || opcode == IR_OP_LOAD ||
           opcode == IR_OP_UNARY || opcode == IR_OP_BINARY ||
           opcode == IR_OP_CALL || opcode == IR_OP_INDEX ||
           opcode == IR_OP_MEMBER || opcode == IR_OP_RESERVE;
}

int ir_verify_module(const IrModule *module) {
    if (module == NULL || module->program == NULL || module->semantics == NULL) return 0;
    for (size_t f = 0; f < module->function_count; f++) {
        const IrFunction *function = &module->functions[f];
        if (function->symbol_id == AST_SYMBOL_NONE ||
            function->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[function->symbol_id].kind !=
                SEMANTIC_SYMBOL_FUNCTION ||
            module->semantics->symbols[function->symbol_id].name_token !=
                function->name_token) return 0;
        for (size_t p = 0; p < function->parameter_count; p++) {
            const IrParameter *parameter = &function->parameters[p];
            const SemanticSymbol *symbol = parameter->symbol_id <
                module->semantics->symbol_count
                    ? &module->semantics->symbols[parameter->symbol_id] : NULL;
            if (parameter->name_token >= module->program->token_count ||
                parameter->symbol_id == AST_SYMBOL_NONE ||
                parameter->symbol_id >= module->semantics->symbol_count ||
                symbol->kind != SEMANTIC_SYMBOL_PARAMETER ||
                symbol->name_token != parameter->name_token ||
                symbol->owner_token != function->name_token ||
                ast_type_data_type(module->program, &symbol->declared_type) != parameter->type ||
                symbol->declared_type.pointer_depth != parameter->pointer_depth)
                return 0;
            for (size_t previous = 0; previous < p; previous++)
                if (function->parameters[previous].symbol_id == parameter->symbol_id) return 0;
        }
        if ((function->next_value != 0 && function->next_value > SIZE_MAX / sizeof(unsigned char)) ||
            (function->next_label != 0 && function->next_label > SIZE_MAX / sizeof(unsigned char)))
            return 0;
        unsigned char *defined = calloc(function->next_value, sizeof(*defined));
        unsigned char *labels = calloc(function->next_label, sizeof(*labels));
        if ((defined == NULL && function->next_value != 0) ||
            (labels == NULL && function->next_label != 0)) {
            free(defined);
            free(labels);
            return 0;
        }
        int valid = 1;
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            if (instruction->opcode == IR_OP_LABEL) {
                if (instruction->target_a >= function->next_label ||
                    labels[instruction->target_a] != 0) {
                    valid = 0;
                    break;
                }
                labels[instruction->target_a] = 1;
            }
        }
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            if (!valid) break;
            int produces_value = opcode_produces_value(instruction->opcode);
            if ((produces_value && instruction->result == IR_VALUE_NONE) ||
                (!produces_value && instruction->result != IR_VALUE_NONE) ||
                (instruction->result != IR_VALUE_NONE &&
                 (instruction->result >= function->next_value ||
                  defined[instruction->result] != 0))) {
                valid = 0;
                break;
            }
#define REQUIRE_VALUE(value) \
            do { \
                size_t required_value = (value); \
                if (required_value == IR_VALUE_NONE || required_value >= function->next_value || \
                    defined[required_value] == 0) valid = 0; \
            } while (0)
#define REQUIRE_LABEL(label) \
            do { \
                size_t required_label = (label); \
                if (required_label == IR_VALUE_NONE || required_label >= function->next_label || \
                    labels[required_label] == 0) valid = 0; \
            } while (0)
            switch (instruction->opcode) {
                case IR_OP_CONSTANT:
                case IR_OP_LOAD:
                case IR_OP_RESERVE:
                case IR_OP_LABEL:
                    break;
                case IR_OP_DECLARE:
                    if (instruction->operand_a != IR_VALUE_NONE) REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_STORE:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->operator_type != TOKEN_PLUS_PLUS &&
                        instruction->operator_type != TOKEN_MINUS_MINUS)
                        REQUIRE_VALUE(instruction->operand_b);
                    break;
                case IR_OP_UNARY:
                    REQUIRE_VALUE(instruction->operand_b);
                    break;
                case IR_OP_BINARY:
                case IR_OP_INDEX:
                    REQUIRE_VALUE(instruction->operand_a);
                    REQUIRE_VALUE(instruction->operand_b);
                    break;
                case IR_OP_MEMBER:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_CALL:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->symbol_id != AST_SYMBOL_NONE &&
                        (instruction->symbol_id >= module->semantics->symbol_count ||
                         module->semantics->symbols[instruction->symbol_id].kind !=
                             SEMANTIC_SYMBOL_FUNCTION))
                        valid = 0;
                    if (instruction->first_argument > function->argument_count ||
                        instruction->argument_count >
                            function->argument_count - instruction->first_argument) {
                        valid = 0;
                    } else {
                        for (size_t a = 0; a < instruction->argument_count; a++)
                            REQUIRE_VALUE(function->arguments[instruction->first_argument + a]);
                    }
                    break;
                case IR_OP_PRINT:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_RETURN:
                    if (instruction->operand_a != IR_VALUE_NONE) REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_BRANCH:
                    REQUIRE_VALUE(instruction->operand_a);
                    REQUIRE_LABEL(instruction->target_a);
                    REQUIRE_LABEL(instruction->target_b);
                    break;
                case IR_OP_JUMP:
                    REQUIRE_LABEL(instruction->target_a);
                    break;
                default:
                    valid = 0;
                    break;
            }
#undef REQUIRE_LABEL
#undef REQUIRE_VALUE
            if (valid && instruction->result != IR_VALUE_NONE)
                defined[instruction->result] = 1;
        }
        free(defined);
        free(labels);
        if (!valid) return 0;
    }
    return 1;
}

void ir_module_free(IrModule *module) {
    if (module == NULL) return;
    for (size_t i = 0; i < module->function_count; i++) {
        free(module->functions[i].parameters);
        free(module->functions[i].instructions);
        free(module->functions[i].arguments);
    }
    free(module->functions);
    free(module);
}
