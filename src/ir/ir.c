#include "ir.h"
#include "core_intrinsics.h"
#include "ir_cfg.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>

void ir_report_failure(const IrFunction *function, size_t index,
                       const char *stage, const char *reason) {
    if (function == NULL || function->source_program == NULL) return;
    const IrInstruction *instruction = index < function->instruction_count
        ? &function->instructions[index] : NULL;
    AstSourceSpan span = instruction == NULL ? function->return_type.span : instruction->span;
    char message[2048];
    if (instruction != NULL)
        snprintf(message, sizeof(message),
            "%s failed in function '%s', IR instruction %zu "
            "(opcode=%d result=%zu operands=%zu,%zu type=%zu): %s",
            stage, ast_program_lexeme(function->source_program, function->name_token),
            index, (int)instruction->opcode, instruction->result,
            instruction->operand_a, instruction->operand_b, instruction->type_id, reason);
    else snprintf(message, sizeof(message), "%s failed in function '%s': %s", stage,
        ast_program_lexeme(function->source_program, function->name_token), reason);
    ErrorContext *context = error_context_create(SEVERITY_ERROR, span.begin.line,
        span.begin.column, ERROR_CATEGORY_COMPILER, ERR_COMP_INTERNAL_FAILURE,
        function->source_program->source_path, message);
    error_context_set_span(context, span.end.line, span.end.column);
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
}

static int ir_verify_module_internal(const IrModule *module,int report);

typedef struct {
    IrModule *module;
    IrFunction *function;
    const AstProgram *program;
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

static int is_terminator(IrOpcode opcode) {
    return opcode == IR_OP_RETURN || opcode == IR_OP_BRANCH || opcode == IR_OP_JUMP || opcode == IR_OP_TRAP;
}

static int block_terminated(const IrFunction *function) {
    return function->instruction_count != 0 &&
        is_terminator(function->instructions[function->instruction_count - 1].opcode);
}

static IrInstruction *emit(IrBuilder *builder, IrOpcode opcode, AstSourceSpan span) {
    IrFunction *function = builder->function;
    /* Structured lowering may request a join jump after a return/break. */
    if (opcode == IR_OP_JUMP && block_terminated(function)) return NULL;
    if (function->instruction_count == function->instruction_capacity &&
        !grow_array((void **) &function->instructions, &function->instruction_capacity,
                    sizeof(*function->instructions))) {
        builder->failed = 1;
        return NULL;
    }
    IrInstruction *instruction = &function->instructions[function->instruction_count++];
    *instruction = (IrInstruction){
        .opcode = opcode,
        .span = span,
        .type = TYPE_VOID,
        .type_id = IR_TYPE_NONE,
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
        case TOKEN_TYPE_I8: case TOKEN_TYPE_U8: case TOKEN_TYPE_I16: case TOKEN_TYPE_U16:
        case TOKEN_TYPE_I32: case TOKEN_TYPE_U32: case TOKEN_TYPE_I64: case TOKEN_TYPE_U64:
        case TOKEN_TYPE_ISIZE: case TOKEN_TYPE_USIZE:
            return token_data_type(program->tokens[type->name_token].type);
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

static int same_ir_type(const IrType *left, const IrType *right) {
    return left->kind == right->kind && left->primitive == right->primitive &&
           left->symbol_id == right->symbol_id && left->element_type == right->element_type &&
           left->array_length == right->array_length;
}

static IrTypeId intern_type(IrModule *module, IrType type) {
    for (size_t i = 0; i < module->type_count; i++)
        if (same_ir_type(&module->types[i], &type)) return i;
    if (module->type_count == module->type_capacity &&
        !grow_array((void **) &module->types, &module->type_capacity,
                    sizeof(*module->types)))
        return IR_TYPE_NONE;
    module->types[module->type_count] = type;
    return module->type_count++;
}

static size_t named_symbol_id(const IrModule *module, const AstProgram *program,
                              size_t name_token) {
    if (name_token >= program->token_count) return AST_SYMBOL_NONE;
    const char *name = ast_program_lexeme(program, name_token);
    const SemanticSymbol *symbol = semantic_find_in_package(module->semantics, program, name,
                                                        SEMANTIC_SYMBOL_STRUCT);
    if (symbol == NULL)
        symbol = semantic_find_in_package(module->semantics, program, name, SEMANTIC_SYMBOL_ENUM);
    return symbol == NULL ? AST_SYMBOL_NONE : symbol->id;
}

static IrTypeId type_from_parts(IrModule *module, DataType primitive,
                                unsigned pointer_depth, size_t named_type_token,
                                int is_array, int is_slice, size_t array_length,
                                unsigned outer_pointer_depth,
                                const AstProgram *source_program,
                                size_t resolved_named_symbol_id) {
    IrType base = {
        .kind = named_type_token != AST_TOKEN_NONE ? IR_TYPE_NAMED : IR_TYPE_PRIMITIVE,
        .primitive = primitive,
        .symbol_id = resolved_named_symbol_id != AST_SYMBOL_NONE
                         ? resolved_named_symbol_id
                         : (named_type_token == AST_TOKEN_NONE
                                ? AST_SYMBOL_NONE
                                : named_symbol_id(module, source_program, named_type_token)),
        .element_type = IR_TYPE_NONE
    };
    IrTypeId result = intern_type(module, base);
    if (result == IR_TYPE_NONE) return result;
    for (unsigned depth = 0; depth < pointer_depth; depth++) {
        IrType pointer = {
            .kind = IR_TYPE_POINTER,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = result
        };
        result = intern_type(module, pointer);
        if (result == IR_TYPE_NONE) return result;
    }
    if (is_array || is_slice) {
        IrType array = {
            .kind = is_slice ? IR_TYPE_SLICE : IR_TYPE_ARRAY,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = result,
            .array_length = array_length
        };
        result = intern_type(module, array);
    }
    for (unsigned depth = 0; depth < outer_pointer_depth; depth++) {
        IrType pointer = {
            .kind = IR_TYPE_POINTER,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = result
        };
        result = intern_type(module, pointer);
        if (result == IR_TYPE_NONE) return result;
    }
    return result;
}

static size_t ast_array_length(const AstProgram *program, const AstType *type) {
    if (type != NULL && type->resolved_array_length != 0) return type->resolved_array_length;
    if (type == NULL || !type->is_array || type->array_length_token >= program->token_count)
        return 0;
    return (size_t) strtoull(ast_program_lexeme(program, type->array_length_token), NULL, 10);
}

static IrTypeId type_from_ast(IrModule *module, const AstProgram *program,
                              const AstType *type) {
    size_t named = type != NULL && type->name_token < program->token_count &&
                   program->tokens[type->name_token].type == TOKEN_IDENTIFIER
                       ? type->name_token
                       : AST_TOKEN_NONE;
    return type_from_parts(module, ast_type_data_type(program, type),
                           type == NULL ? 0 : type->pointer_depth, named,
                           type != NULL && type->is_array,
                           type != NULL && type->is_slice,
                           ast_array_length(program, type),
                           type == NULL ? 0 : type->outer_pointer_depth,
                           program, AST_SYMBOL_NONE);
}

static IrTypeId type_from_expression(IrModule *module, const AstProgram *program,
                                     const AstExpression *expression) {
    return type_from_parts(module, expression->resolved_type,
                           expression->resolved_pointer_depth,
                           expression->resolved_named_type_token,
                           expression->resolved_is_array, expression->resolved_is_slice,
                           expression->resolved_array_length,
                           expression->resolved_outer_pointer_depth, program,
                           expression->resolved_named_symbol_id);
}

static void set_expression_type(IrBuilder *builder, IrInstruction *instruction,
                                const AstExpression *expression) {
    if (instruction == NULL || expression == NULL) return;
    instruction->type = expression->resolved_type;
    instruction->pointer_depth = expression->resolved_pointer_depth + expression->resolved_outer_pointer_depth;
    instruction->type_name_token = expression->resolved_named_type_token;
    instruction->is_array = expression->resolved_is_array && expression->resolved_outer_pointer_depth == 0;
    instruction->is_slice = expression->resolved_is_slice;
    instruction->type_id = IR_TYPE_NONE;
    if ((expression->resolved_is_array || expression->resolved_is_slice) &&
        expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
        const SemanticSymbol *symbol =
            &builder->module->semantics->symbols[expression->resolved_symbol_id];
        instruction->type_id = type_from_ast(builder->module, symbol->source_program,
                                              &symbol->declared_type);
    }
    if (instruction->type_id == IR_TYPE_NONE)
        instruction->type_id = type_from_expression(builder->module, builder->program, expression);
    if (instruction->type_id == IR_TYPE_NONE) builder->failed = 1;
}

static void set_void_type(IrBuilder *builder, IrInstruction *instruction) {
    if (instruction == NULL) return;
    instruction->type = TYPE_VOID;
    instruction->type_id = type_from_parts(builder->module, TYPE_VOID, 0,
                                           AST_TOKEN_NONE, 0, 0, 0, 0,
                                           builder->program, AST_SYMBOL_NONE);
    if (instruction->type_id == IR_TYPE_NONE) builder->failed = 1;
}

static void emit_label(IrBuilder *builder, size_t label, AstSourceSpan span);

static size_t lower_expression(IrBuilder *builder, const AstExpression *expression) {
    if (expression == NULL) return IR_VALUE_NONE;
    if (expression->kind == AST_EXPR_ENUM_ACCESS) {
        size_t value=lower_expression(builder,expression->left->left);
        size_t success=new_label(builder), failure=new_label(builder), join=new_label(builder);
        IrInstruction *test=emit(builder,IR_OP_ENUM_IS,expression->span);
        if (!test) return IR_VALUE_NONE;
        test->operand_a=value; test->symbol_id=expression->resolved_symbol_id;
        test->result=new_value(builder); test->type=TYPE_BIT;
        test->type_id=type_from_parts(builder->module,TYPE_BIT,0,AST_TOKEN_NONE,0,0,0,0,builder->program,AST_SYMBOL_NONE);
        size_t condition=test->result;
        IrInstruction *branch=emit(builder,IR_OP_BRANCH,expression->span);
        if (!branch) return IR_VALUE_NONE;
        set_void_type(builder,branch); branch->operand_a=condition; branch->target_a=success; branch->target_b=failure;
        emit_label(builder,success,expression->span);
        IrInstruction *payload=emit(builder,IR_OP_ENUM_PAYLOAD,expression->span);
        if (!payload) return IR_VALUE_NONE;
        set_expression_type(builder,payload,expression);
        const SemanticSymbol *variant=&builder->module->semantics->symbols[expression->resolved_symbol_id];
        const AstEnumValue *variant_value=variant->node;
        payload->type_id=type_from_ast(builder->module,variant->source_program,&variant_value->payload_types->type);
        payload->operand_a=value; payload->symbol_id=expression->resolved_symbol_id;
        payload->enum_payload_index=0; payload->target_a=success; payload->result=new_value(builder);
        size_t result=payload->result;
        IrInstruction *jump=emit(builder,IR_OP_JUMP,expression->span);
        if (!jump) return IR_VALUE_NONE;
        set_void_type(builder,jump); jump->target_a=join;
        emit_label(builder,failure,expression->span);
        IrInstruction *trap=emit(builder,IR_OP_TRAP,expression->span); set_void_type(builder,trap);
        emit_label(builder,join,expression->span);
        return result;
    }
    if (expression->folded_constant.lexeme != NULL) {
        AstProgram *program = (AstProgram *) builder->function->source_program;
        AstToken *tokens = realloc(program->tokens, (program->token_count+1)*sizeof(*tokens));
        if (tokens == NULL) { builder->failed=1; return IR_VALUE_NONE; }
        program->tokens = tokens;
        size_t token = program->token_count++;
        program->tokens[token] = expression->folded_constant;
        IrInstruction *instruction = emit(builder, IR_OP_CONSTANT, expression->span);
        if (instruction == NULL) return IR_VALUE_NONE;
        instruction->result = new_value(builder);
        instruction->auxiliary_token = token;
        instruction->type = expression->resolved_type;
        instruction->type_id = type_from_parts(builder->module, expression->resolved_type,
            0, AST_TOKEN_NONE, 0, 0, 0, 0, program, AST_SYMBOL_NONE);
        return instruction->result;
    }
    if (expression->kind == AST_EXPR_NAME &&
        expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
        const SemanticSymbol *symbol =
            &builder->module->semantics->symbols[expression->resolved_symbol_id];
        if (symbol->kind == SEMANTIC_SYMBOL_CONSTANT) {
            const AstExpression *value = NULL;
            if (symbol->declaration != NULL &&
                symbol->declaration->kind == AST_DECL_CONSTANT)
                value = symbol->declaration->as.constant.value;
            else value = (const AstExpression *) symbol->node;
            const AstProgram *saved = builder->program;
            builder->program = symbol->source_program;
            size_t result = lower_expression(builder, value);
            builder->program = saved;
            return result;
        }
    }
    int method_call = expression->kind == AST_EXPR_CALL && expression->left != NULL &&
                      expression->left->kind == AST_EXPR_MEMBER &&
                      expression->resolved_symbol_id < builder->module->semantics->symbol_count;
    const SemanticSymbol *method = method_call
                                       ? &builder->module->semantics->symbols[expression->resolved_symbol_id]
                                       : NULL;
    int has_receiver = method != NULL && method->kind == SEMANTIC_SYMBOL_FUNCTION &&
                       method->declaration != NULL &&
                       !method->declaration->as.function.is_static;
    size_t receiver = has_receiver
                          ? lower_expression(builder, expression->left->left)
                          : IR_VALUE_NONE;
    size_t left = (method_call || expression->kind == AST_EXPR_ENUM_CONSTRUCT) ? IR_VALUE_NONE : lower_expression(builder, expression->left);
    if (expression->kind == AST_EXPR_BINARY &&
        (expression->operator_type == TOKEN_AMP_AMP ||
         expression->operator_type == TOKEN_PIPE_PIPE)) {
        size_t short_label = new_label(builder);
        size_t right_label = new_label(builder);
        size_t end_label = new_label(builder);
        IrInstruction *branch = emit(builder, IR_OP_BRANCH, expression->span);
        if (branch != NULL) {
            branch->operand_a = left;
            branch->target_a = expression->operator_type == TOKEN_PIPE_PIPE
                                   ? short_label
                                   : right_label;
            branch->target_b = expression->operator_type == TOKEN_PIPE_PIPE
                                   ? right_label
                                   : short_label;
        }
        set_void_type(builder, branch);
        emit_label(builder, short_label, expression->span);
        IrInstruction *short_jump = emit(builder, IR_OP_JUMP, expression->span);
        if (short_jump != NULL) short_jump->target_a = end_label;
        set_void_type(builder, short_jump);
        emit_label(builder, right_label, expression->span);
        size_t right_value = lower_expression(builder, expression->right);
        size_t right_predecessor = right_label;
        for (size_t i = builder->function->instruction_count; i > 0; i--)
            if (builder->function->instructions[i - 1].opcode == IR_OP_LABEL) {
                right_predecessor = builder->function->instructions[i - 1].target_a;
                break;
            }
        IrInstruction *right_jump = emit(builder, IR_OP_JUMP, expression->span);
        if (right_jump != NULL) right_jump->target_a = end_label;
        set_void_type(builder, right_jump);
        emit_label(builder, end_label, expression->span);
        IrInstruction *phi = emit(builder, IR_OP_PHI, expression->span);
        if (phi == NULL) return IR_VALUE_NONE;
        phi->result = new_value(builder);
        phi->operand_a = left;
        phi->operand_b = right_value;
        phi->target_a = short_label;
        phi->target_b = right_predecessor;
        set_expression_type(builder, phi, expression);
        return phi->result;
    }
    size_t right = lower_expression(builder, expression->right);
    if ((expression->kind == AST_EXPR_CAST || expression->kind == AST_EXPR_FREE) &&
        expression->arguments != NULL)
        left = lower_expression(builder, expression->arguments);
    IrOpcode opcode = IR_OP_CONSTANT;
    switch (expression->kind) {
        case AST_EXPR_LITERAL: opcode = IR_OP_CONSTANT;
            break;
        case AST_EXPR_NAME: opcode = IR_OP_LOAD;
            break;
        case AST_EXPR_UNARY: opcode = IR_OP_UNARY;
            break;
        case AST_EXPR_BINARY: opcode = IR_OP_BINARY;
            break;
        case AST_EXPR_ENUM_CONSTRUCT: opcode = IR_OP_ENUM_CONSTRUCT; break;
        case AST_EXPR_ENUM_ACCESS: return IR_VALUE_NONE;
        case AST_EXPR_CALL: opcode = IR_OP_CALL;
            break;
        case AST_EXPR_INDEX: opcode = IR_OP_INDEX;
            break;
        case AST_EXPR_MEMBER: opcode = IR_OP_MEMBER;
            break;
        case AST_EXPR_SLICE_LENGTH: opcode = IR_OP_SLICE_LENGTH;
            break;
        case AST_EXPR_RESERVE: opcode = IR_OP_ALLOC;
            break;
        case AST_EXPR_CAST: opcode = IR_OP_CAST;
            break;
        case AST_EXPR_FREE: opcode = IR_OP_FREE;
            break;
        case AST_EXPR_ERROR: builder->failed = 1;
            return IR_VALUE_NONE;
    }
    size_t first_argument = IR_VALUE_NONE;
    size_t argument_count = 0;
    if (expression->kind == AST_EXPR_CALL || expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        if (has_receiver) argument_count++;
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next)
            argument_count++;
        size_t *values = argument_count == 0 ? NULL : calloc(argument_count, sizeof(*values));
        if (values == NULL && argument_count != 0) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
        size_t argument_index = 0;
        if (has_receiver) values[argument_index++] = receiver;
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next) {
            values[argument_index++] = lower_expression(builder, argument);
        }
        first_argument = builder->function->argument_count;
        for (size_t i = 0; i < argument_count; i++) {
            if (!append_argument(builder, values[i])) {
                free(values);
                return IR_VALUE_NONE;
            }
        }
        free(values);
    }
    IrInstruction *instruction = emit(builder, opcode, expression->span);
    if (instruction == NULL) return IR_VALUE_NONE;
    if (!((opcode == IR_OP_CALL || opcode == IR_OP_FREE) &&
          expression->resolved_type == TYPE_VOID))
        instruction->result = new_value(builder);
    instruction->operand_a = left;
    instruction->operand_b = right;
    instruction->auxiliary_token = expression->value_token;
    instruction->symbol_id = expression->resolved_symbol_id;
    instruction->operator_type = expression->operator_type;
    set_expression_type(builder, instruction, expression);
    if (expression->kind == AST_EXPR_RESERVE) {
        IrTypeId allocated = type_from_ast(builder->module, builder->program,
                                           &expression->allocated_type);
        if (allocated == IR_TYPE_NONE) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
        IrType pointer = {
            .kind = IR_TYPE_POINTER,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = allocated
        };
        instruction->type_id = intern_type(builder->module, pointer);
        if (instruction->type_id == IR_TYPE_NONE) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
    }
    if (expression->kind == AST_EXPR_CALL || expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        instruction->first_argument = first_argument;
        instruction->argument_count = argument_count;
    }
    return instruction->result;
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement);

static void emit_label(IrBuilder *builder, size_t label, AstSourceSpan span) {
    IrInstruction *instruction = emit(builder, IR_OP_LABEL, span);
    if (instruction != NULL) instruction->target_a = label;
    set_void_type(builder, instruction);
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement) {
    for (; statement != NULL && !builder->failed; statement = statement->next) {
        if (statement->kind == AST_STMT_MATCH) {
            size_t value=lower_expression(builder,statement->value);
            size_t join=new_label(builder); int wildcard=0;
            for (const AstMatchArm *arm=statement->match_arms; arm; arm=arm->next) {
                size_t body_label=IR_VALUE_NONE,next_label=IR_VALUE_NONE;
                if (!arm->wildcard) {
                    body_label=new_label(builder); next_label=new_label(builder);
                    IrInstruction *test=emit(builder,IR_OP_ENUM_IS,arm->span);
                    if (!test) return;
                    test->operand_a=value; test->symbol_id=arm->resolved_variant_symbol; test->result=new_value(builder);
                    test->type=TYPE_BIT; test->type_id=type_from_parts(builder->module,TYPE_BIT,0,AST_TOKEN_NONE,0,0,0,0,builder->program,AST_SYMBOL_NONE);
                    size_t condition=test->result;
                    IrInstruction *branch=emit(builder,IR_OP_BRANCH,arm->span); if (!branch) return;
                    set_void_type(builder,branch); branch->operand_a=condition; branch->target_a=body_label; branch->target_b=next_label;
                    emit_label(builder,body_label,arm->span);
                } else wildcard=1;
                size_t payload_index=0;
                for (const AstParameter *binding=arm->bindings; binding; binding=binding->next,payload_index++) {
                    IrInstruction *payload=emit(builder,IR_OP_ENUM_PAYLOAD,arm->span); if (!payload) return;
                    payload->operand_a=value; payload->symbol_id=arm->resolved_variant_symbol;
                    payload->enum_payload_index=payload_index; payload->target_a=body_label;
                    payload->result=new_value(builder); payload->type_id=type_from_ast(builder->module,builder->program,&binding->type);
                    payload->type=ast_type_data_type(builder->program,&binding->type);
                    payload->pointer_depth=binding->type.pointer_depth+binding->type.outer_pointer_depth;
                    payload->type_name_token=binding->type.name_token; payload->is_array=binding->type.is_array;
                    size_t initial=payload->result;
                    IrInstruction *local=emit(builder,IR_OP_DECLARE,arm->span); if (!local) return;
                    local->operand_a=initial; local->symbol_id=binding->resolved_symbol_id;
                    local->type_id=type_from_ast(builder->module,builder->program,&binding->type);
                    local->type=ast_type_data_type(builder->program,&binding->type);
                    local->pointer_depth=binding->type.pointer_depth+binding->type.outer_pointer_depth;
                    local->type_name_token=binding->type.name_token; local->is_array=binding->type.is_array;
                }
                lower_statement(builder,arm->body);
                IrInstruction *jump=emit(builder,IR_OP_JUMP,arm->span);
                if (jump) { set_void_type(builder,jump); jump->target_a=join; }
                if (!arm->wildcard) emit_label(builder,next_label,arm->span);
            }
            if (!wildcard) { IrInstruction *trap=emit(builder,IR_OP_TRAP,statement->span); set_void_type(builder,trap); }
            emit_label(builder,join,statement->span);
            continue;
        }

        if (block_terminated(builder->function)) break;
        if (statement->kind == AST_STMT_BLOCK) {
            lower_statement(builder, statement->body);
        } else if (statement->kind == AST_STMT_VARIABLE) {
            if (statement->is_const) continue;
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_DECLARE, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = value;
                instruction->auxiliary_token = statement->name_token;
                instruction->symbol_id = statement->resolved_symbol_id;
                instruction->type = ast_type_data_type(builder->program, &statement->type);
                instruction->type_id = statement->type.kind == AST_TYPE_INFERRED &&
                                       statement->value != NULL
                                           ? type_from_expression(builder->module, builder->program, statement->value)
                                           : type_from_ast(builder->module, builder->program, &statement->type);
                if (instruction->type_id == IR_TYPE_NONE) builder->failed = 1;
                instruction->pointer_depth = statement->type.pointer_depth + statement->type.outer_pointer_depth;
                instruction->type_name_token = statement->type.name_token;
                instruction->is_array = statement->type.is_array && statement->type.outer_pointer_depth == 0;
                instruction->is_slice = statement->type.is_slice;
                if (statement->type.kind == AST_TYPE_INFERRED && statement->value != NULL) {
                    instruction->type = statement->value->resolved_type;
                    instruction->pointer_depth = statement->value->resolved_pointer_depth + statement->value->resolved_outer_pointer_depth;
                    instruction->type_name_token = statement->value->resolved_named_type_token;
                    instruction->is_array = statement->value->resolved_is_array && statement->value->resolved_outer_pointer_depth == 0;
                    instruction->is_slice = statement->value->resolved_is_slice;
                }
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
            set_expression_type(builder, instruction, statement->expression);
        } else if (statement->kind == AST_STMT_EXPRESSION) {
            (void) lower_expression(builder, statement->expression);
        } else if (statement->kind == AST_STMT_RETURN) {
            size_t value = lower_expression(builder, statement->value);
            IrInstruction *instruction = emit(builder, IR_OP_RETURN, statement->span);
            if (instruction != NULL) instruction->operand_a = value;
            if (statement->value != NULL) set_expression_type(builder, instruction, statement->value);
            else set_void_type(builder, instruction);
        } else if (statement->kind == AST_STMT_BREAK ||
                   statement->kind == AST_STMT_CONTINUE) {
            size_t target = statement->kind == AST_STMT_BREAK
                                ? builder->break_label
                                : builder->continue_label;
            /* An unresolved target is retained for sema/diagnostic recovery;
               valid modules always resolve it to an enclosing loop label. */
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = target;
            set_void_type(builder, jump);
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
            set_void_type(builder, branch);
            emit_label(builder, then_label, statement->span);
            lower_statement(builder, statement->body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = end_label;
            set_void_type(builder, jump);
            if (statement->else_body != NULL) {
                emit_label(builder, else_label, statement->span);
                lower_statement(builder, statement->else_body);
            }
            emit_label(builder, end_label, statement->span);
        } else if (statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR) {
            size_t condition_label = new_label(builder);
            size_t body_label = new_label(builder);
            size_t update_label = statement->kind == AST_STMT_FOR
                                      ? new_label(builder)
                                      : condition_label;
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
                set_void_type(builder, jump);
            } else {
                IrInstruction *branch = emit(builder, IR_OP_BRANCH, statement->span);
                if (branch != NULL) {
                    branch->operand_a = condition;
                    branch->target_a = body_label;
                    branch->target_b = end_label;
                }
                set_void_type(builder, branch);
            }
            emit_label(builder, body_label, statement->span);
            lower_statement(builder, statement->body);
            if (statement->kind == AST_STMT_FOR) emit_label(builder, update_label, statement->span);
            lower_statement(builder, statement->else_body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = condition_label;
            set_void_type(builder, jump);
            emit_label(builder, end_label, statement->span);
            builder->break_label = saved_break;
            builder->continue_label = saved_continue;
        }
    }
}

static int append_function(IrModule *module, const AstProgram *program,
                           const AstDeclarationNode *declaration) {
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions)))
        return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction){
        .source_program = program,
        .name_token = declaration->name_token,
        .owner_token = declaration->as.function.owner_token,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .symbol_id = AST_SYMBOL_NONE,
        .return_type = declaration->as.function.return_type,
        .return_type_id = type_from_ast(module, program, &declaration->as.function.return_type)
    };
    if (function->return_type_id == IR_TYPE_NONE) return 0;
    for (size_t i = 0; i < module->semantics->symbol_count; i++) {
        const SemanticSymbol *symbol = &module->semantics->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration == declaration) {
            function->symbol_id = symbol->id;
            function->owner_symbol_id = symbol->owner_symbol_id;
            break;
        }
    }
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        function->parameter_count++;
    int has_receiver = function->owner_symbol_id != AST_SYMBOL_NONE &&
                       !declaration->as.function.is_static;
    if (has_receiver) function->parameter_count++;
    if (function->parameter_count != 0) {
        function->parameters = calloc(function->parameter_count, sizeof(*function->parameters));
        if (function->parameters == NULL) return 0;
    }
    size_t parameter_index = 0;
    if (has_receiver) {
        IrParameter *receiver = &function->parameters[parameter_index++];
        receiver->source_program = program;
        receiver->name_token = function->owner_token;
        receiver->symbol_id = function->owner_symbol_id;
        receiver->type = TYPE_UNKNOWN;
        receiver->pointer_depth = 1;
        receiver->type_name_token = function->owner_token;
        receiver->is_receiver = 1;
        receiver->type_id = type_from_parts(module, TYPE_UNKNOWN, 1,
                                            function->owner_token, 0, 0, 0, 0,
                                            program, function->owner_symbol_id);
        if (receiver->type_id == IR_TYPE_NONE) return 0;
    }
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next, parameter_index++) {
        IrParameter *ir_parameter = &function->parameters[parameter_index];
        *ir_parameter = (IrParameter){
            .source_program = program,
            .name_token = parameter->name_token,
            .symbol_id = parameter->resolved_symbol_id,
            .type = ast_type_data_type(program, &parameter->type),
            .type_id = type_from_ast(module, program, &parameter->type),
            .pointer_depth = parameter->type.pointer_depth + parameter->type.outer_pointer_depth,
            .type_name_token = parameter->type.name_token,
            .is_array = parameter->type.is_array && parameter->type.outer_pointer_depth == 0,
            .is_slice = parameter->type.is_slice
        };
        if (ir_parameter->type_id == IR_TYPE_NONE) return 0;
    }
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    lower_statement(&builder, declaration->as.function.body);
    if (builder.failed) ir_report_failure(function,
        function->instruction_count == 0 ? IR_VALUE_NONE : function->instruction_count - 1,
        "IR lowering", "could not lower the typed source tree");
    return !builder.failed;
}

static int copy_fields(IrModule *module, const AstProgram *program, const AstField *fields,
                       IrFieldDefinition **output, size_t *count) {
    for (const AstField *field = fields; field != NULL; field = field->next) (*count)++;
    if (*count == 0) return 1;
    *output = calloc(*count, sizeof(**output));
    if (*output == NULL) return 0;
    size_t index = 0;
    for (const AstField *field = fields; field != NULL; field = field->next, index++) {
        (*output)[index] = (IrFieldDefinition){
            .source_program = program,
            .name_token = field->name_token,
            .symbol_id = field->resolved_symbol_id,
            .type_id = type_from_ast(module, program, &field->type)
        };
        if ((*output)[index].type_id == IR_TYPE_NONE) return 0;
    }
    return 1;
}

static int append_structure(IrModule *module, const AstProgram *program,
                            const AstDeclarationNode *declaration) {
    if (module->structure_count == module->structure_capacity &&
        !grow_array((void **) &module->structures, &module->structure_capacity,
                    sizeof(*module->structures)))
        return 0;
    IrAggregate *structure = &module->structures[module->structure_count++];
    *structure = (IrAggregate){
        .source_program = program,
        .name_token = declaration->name_token,
        .symbol_id = declaration->resolved_symbol_id
    };
    return copy_fields(module, program, declaration->as.struct_decl.fields,
                       &structure->fields, &structure->field_count);
}

static int append_enum(IrModule *module, const AstProgram *program,
                       const AstDeclarationNode *declaration) {
    if (module->enum_count == module->enum_capacity &&
        !grow_array((void **) &module->enums, &module->enum_capacity,
                    sizeof(*module->enums)))
        return 0;
    IrEnum *enumeration = &module->enums[module->enum_count++];
    *enumeration = (IrEnum){
        .source_program = program,
        .name_token = declaration->name_token,
        .symbol_id = declaration->resolved_symbol_id,
        .is_sum = declaration->as.enum_decl.is_sum
    };
    if (!copy_fields(module, program, declaration->as.enum_decl.fields,
                     &enumeration->fields, &enumeration->field_count))
        return 0;
    for (const AstEnumValue *value = declaration->as.enum_decl.values;
         value != NULL; value = value->next) {
        enumeration->variant_count++;
        for (const AstExpression *argument = value->arguments;
             argument != NULL; argument = argument->next)
            enumeration->variant_argument_count++;
    }
    if (enumeration->variant_count != 0) {
        enumeration->variants = calloc(enumeration->variant_count,
                                       sizeof(*enumeration->variants));
        if (enumeration->variants == NULL) return 0;
    }
    if (enumeration->variant_argument_count != 0) {
        enumeration->variant_arguments = calloc(enumeration->variant_argument_count,
                                                sizeof(*enumeration->variant_arguments));
        if (enumeration->variant_arguments == NULL) return 0;
    }
    size_t variant_index = 0;
    size_t argument_index = 0;
    for (const AstEnumValue *value = declaration->as.enum_decl.values;
         value != NULL; value = value->next, variant_index++) {
        IrEnumVariant *variant = &enumeration->variants[variant_index];
        *variant = (IrEnumVariant){
            .source_program = program,
            .name_token = value->name_token,
            .symbol_id = value->resolved_symbol_id,
            .first_argument = argument_index
        };
        for (const AstTypeArgument *p=value->payload_types; p; p=p->next) variant->payload_count++;
        if (variant->payload_count) {
            variant->payload_types=calloc(variant->payload_count,sizeof(*variant->payload_types));
            if (!variant->payload_types) return 0;
            size_t index=0;
            for (const AstTypeArgument *p=value->payload_types; p; p=p->next)
                variant->payload_types[index++]=type_from_ast(module,program,&p->type);
        }
        for (const AstExpression *argument = value->arguments;
             argument != NULL; argument = argument->next) {
            const AstExpression *constant = argument->kind == AST_EXPR_UNARY &&
                                            argument->operator_type == TOKEN_MINUS
                ? argument->right : argument;
            enumeration->variant_arguments[argument_index++] = (IrEnumArgument){
                .token = constant->value_token,
                .type_id = type_from_expression(module, program, argument),
                .negative = constant != argument
            };
            if (enumeration->variant_arguments[argument_index - 1].type_id == IR_TYPE_NONE)
                return 0;
            variant->argument_count++;
        }
    }
    return 1;
}

static int append_import(IrModule *module, const AstProgram *program,
                         const AstDeclarationNode *declaration) {
    for (const AstImportPath *path = declaration->as.import_decl.paths; path != NULL; path = path->next) {
        if (module->import_count == module->import_capacity &&
            !grow_array((void **) &module->imports, &module->import_capacity,
                        sizeof(*module->imports)))
            return 0;
        module->imports[module->import_count++] = (IrImport){
            .source_program = program,
            .symbol_id = path->resolved_symbol_id,
            .path_token = path->path_token,
            .path_first_token = path->path_first_token,
            .path_token_count = path->path_token_count,
            .resolved_program = path->resolved_program
        };
    }
    return 1;
}

static int lower_unit(IrModule *module, const AstProgram *program) {
    if (!program->structured_ast_complete) return 0;
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->generic_parameters != NULL) continue;
        if (declaration->kind == AST_DECL_VARIABLE) {
            IrGlobal *grown=realloc(module->globals,(module->global_count+1)*sizeof(*grown));
            if (!grown) return 0;
            module->globals=grown;
            const SemanticSymbol *symbol=&module->semantics->symbols[declaration->resolved_symbol_id];
            IrGlobal global={.source_program=program,.symbol_id=symbol->id};
            global.type_id=declaration->as.constant.type.kind == AST_TYPE_INFERRED
                ? type_from_parts(module,symbol->resolved_type,0,AST_TOKEN_NONE,0,0,0,0,program,AST_SYMBOL_NONE)
                : type_from_ast(module,program,&declaration->as.constant.type);
            const AstExpression *value=declaration->as.constant.value;
            if (value) {
                if (!value->folded_constant.lexeme) return 0;
                const char *text=value->folded_constant.lexeme;
                if (symbol->resolved_type == TYPE_STRING) global.string=text;
                else if (symbol->resolved_type == TYPE_DOUBLE) { union { double f; uint64_t u; } bits={.f=strtod(text,NULL)}; global.bits=bits.u; }
                else if (symbol->resolved_type == TYPE_FLOAT) { union { float f; uint32_t u; } bits={.f=(float)strtod(text,NULL)}; global.bits=bits.u; }
                else global.bits=(uint64_t)strtoull(text,NULL,10);
            }
            module->globals[module->global_count++]=global;
        }
        if (declaration->kind == AST_DECL_IMPORT && !append_import(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_STRUCT &&
            !append_structure(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_ENUM && !append_enum(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_FUNCTION && declaration->generic_parameters == NULL &&
            !append_function(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_IMPL && declaration->as.impl_decl.attached) {
            for (const AstDeclarationNode *m=declaration->as.impl_decl.methods; m; m=m->next)
                if (!append_function(module,program,m)) return 0;
        }
        if (declaration->kind == AST_DECL_STRUCT) {
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                if (!append_function(module, program, method)) return 0;
        }
    }
    return 1;
}

IrModule *ir_lower_program(const AstProgram *program, const SemanticModel *semantics) {
    if (program == NULL || semantics == NULL || semantics->program != program ||
        !program->structured_ast_complete)
        return NULL;
    IrModule *module = calloc(1, sizeof(*module));
    if (module == NULL) return NULL;
    module->program = program;
    module->semantics = semantics;
    if (!lower_unit(module, program)) goto failure;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (!lower_unit(module, program->owned_imports[i])) goto failure;
    if (!ir_verify_module_internal(module,1)) goto failure;
    module->verified = 1;
    return module;

failure:
    ir_module_free(module);
    return NULL;
}

static int instruction_produces_value(const IrInstruction *instruction) {
    IrOpcode opcode = instruction->opcode;
    return opcode == IR_OP_CONSTANT || opcode == IR_OP_LOAD ||
           opcode == IR_OP_UNARY || opcode == IR_OP_BINARY ||
           (opcode == IR_OP_CALL && instruction->type != TYPE_VOID) || opcode == IR_OP_INDEX ||
           opcode == IR_OP_MEMBER || opcode == IR_OP_SLICE_LENGTH ||
           opcode == IR_OP_CAST || opcode == IR_OP_ALLOC ||
           opcode == IR_OP_PHI || opcode == IR_OP_ENUM_CONSTRUCT || opcode == IR_OP_ENUM_IS || opcode == IR_OP_ENUM_PAYLOAD;
}

static const IrInstruction *verified_producer(const IrFunction *function,
                                              size_t value, size_t before) {
    if (value == IR_VALUE_NONE) return NULL;
    for (size_t i = 0; i < before; i++)
        if (function->instructions[i].result == value) return &function->instructions[i];
    return NULL;
}

static int ir_integral_type(const IrModule *module, IrTypeId type_id) {
    if (type_id >= module->type_count || module->types[type_id].kind != IR_TYPE_PRIMITIVE)
        return 0;
    DataType type = module->types[type_id].primitive;
    return data_type_integral(type);
}

static int ir_floating_type(const IrModule *module, IrTypeId type_id) {
    if (type_id >= module->type_count || module->types[type_id].kind != IR_TYPE_PRIMITIVE)
        return 0;
    DataType type = module->types[type_id].primitive;
    return type == TYPE_FLOAT || type == TYPE_DOUBLE;
}

static int ir_numeric_type(const IrModule *module, IrTypeId type_id) {
    return ir_integral_type(module, type_id) || ir_floating_type(module, type_id);
}

static int ir_pointer_type(const IrModule *module, IrTypeId type_id) {
    return type_id < module->type_count &&
           (module->types[type_id].kind == IR_TYPE_POINTER ||
            module->types[type_id].kind == IR_TYPE_ARRAY ||
            module->types[type_id].kind == IR_TYPE_SLICE);
}

static int ir_string_type(const IrModule *module, IrTypeId type_id) {
    return type_id < module->type_count &&
           module->types[type_id].kind == IR_TYPE_PRIMITIVE &&
           module->types[type_id].primitive == TYPE_STRING;
}

static int ir_types_assignable(const IrModule *module, IrTypeId source,
                               IrTypeId target, IrOpcode source_opcode) {
    if (source >= module->type_count || target >= module->type_count) return 0;
    if (source == target) return 1;
    const IrType *from = &module->types[source];
    const IrType *to = &module->types[target];
    if (from->kind == IR_TYPE_ARRAY && to->kind == IR_TYPE_SLICE)
        return from->element_type == to->element_type;
    if (from->kind == IR_TYPE_POINTER && to->kind == IR_TYPE_POINTER) {
        const IrType *element = &module->types[from->element_type];
        return from->element_type == to->element_type ||
               (element->kind == IR_TYPE_PRIMITIVE &&
                element->primitive == TYPE_UNKNOWN);
    }
    if (from->kind != IR_TYPE_PRIMITIVE || to->kind != IR_TYPE_PRIMITIVE) return 0;
    if (ir_integral_type(module, source) && ir_integral_type(module, target)) return 1;
    if (ir_integral_type(module, source) && ir_floating_type(module, target)) return 1;
    if (from->primitive == TYPE_FLOAT && to->primitive == TYPE_DOUBLE) return 1;
    return from->primitive == TYPE_DOUBLE && to->primitive == TYPE_FLOAT &&
           source_opcode == IR_OP_CONSTANT;
}

static const IrFunction *ir_called_function(const IrModule *module, size_t symbol_id) {
    for (size_t i = 0; i < module->function_count; i++)
        if (module->functions[i].symbol_id == symbol_id) return &module->functions[i];
    return NULL;
}

static int ir_void_type(const IrModule *module, IrTypeId type_id) {
    return type_id < module->type_count &&
           module->types[type_id].kind == IR_TYPE_PRIMITIVE &&
           module->types[type_id].primitive == TYPE_VOID;
}

static const IrEnumVariant *ir_variant(const IrModule *module,size_t symbol,const IrEnum **owner) {
    for (size_t e=0; e<module->enum_count; e++)
        for (size_t v=0; v<module->enums[e].variant_count; v++)
            if (module->enums[e].variants[v].symbol_id == symbol) {
                if (owner) *owner=&module->enums[e];
                return &module->enums[e].variants[v];
            }
    return NULL;
}
static int verified_payload_guard(const IrFunction *function,const IrInstruction *payload,size_t index) {
    size_t label=index;
    while (label > 0 && function->instructions[label].opcode != IR_OP_LABEL) label--;
    if (label == 0 || function->instructions[label].target_a != payload->target_a) return 0;
    const IrInstruction *branch=&function->instructions[label-1];
    if (branch->opcode != IR_OP_BRANCH || branch->target_a != payload->target_a) return 0;
    const IrInstruction *test=verified_producer(function,branch->operand_a,label-1);
    if (!test || test->opcode != IR_OP_ENUM_IS || test->operand_a != payload->operand_a ||
        test->symbol_id != payload->symbol_id) return 0;
    /* A single checked entry into the arm makes the proof dominate every extraction. */
    for (size_t i=0; i<function->instruction_count; i++) {
        const IrInstruction *in=&function->instructions[i];
        if (i == label-1) continue;
        if ((in->opcode == IR_OP_BRANCH || in->opcode == IR_OP_JUMP) &&
            (in->target_a == payload->target_a || in->target_b == payload->target_a)) return 0;
    }
    return 1;
}

static int core_ir_type_matches(const IrModule *module, IrTypeId id,
                                CoreValueKind kind, int parameter) {
    if (id >= module->type_count) return 0;
    const IrType *type = &module->types[id];
    if (kind == CORE_BYTES) {
        if (type->kind != IR_TYPE_POINTER || type->element_type >= module->type_count) return 0;
        type = &module->types[type->element_type];
        return type->kind == IR_TYPE_PRIMITIVE && type->primitive == TYPE_U8;
    }
    if (type->kind != IR_TYPE_PRIMITIVE) return 0;
    if (parameter && (kind == CORE_INT || kind == CORE_SIZE || kind == CORE_OFFSET)) return ir_integral_type(module, id);
    return type->primitive == core_value_type(kind);
}

static int verify_instruction_types(const IrModule *module,
                                    const IrFunction *function,
                                    const IrInstruction *instruction,
                                    size_t index) {
    const IrInstruction *a = verified_producer(function, instruction->operand_a, index);
    const IrInstruction *b = verified_producer(function, instruction->operand_b, index);
    switch (instruction->opcode) {
        case IR_OP_ENUM_CONSTRUCT: {
            const IrEnum *owner=NULL; const IrEnumVariant *variant=ir_variant(module,instruction->symbol_id,&owner);
            if (!variant || instruction->type_id >= module->type_count ||
                module->types[instruction->type_id].kind != IR_TYPE_NAMED ||
                module->types[instruction->type_id].symbol_id != owner->symbol_id ||
                instruction->argument_count != variant->payload_count ||
                instruction->first_argument > function->argument_count ||
                instruction->argument_count > function->argument_count-instruction->first_argument) return 0;
            for (size_t n=0; n<variant->payload_count; n++) {
                const IrInstruction *value=verified_producer(function,function->arguments[instruction->first_argument+n],index);
                if (!value || !ir_types_assignable(module,value->type_id,variant->payload_types[n],value->opcode)) return 0;
            }
            return 1;
        }
        case IR_OP_ENUM_IS:
        case IR_OP_ENUM_PAYLOAD: {
            const IrEnum *owner=NULL; const IrEnumVariant *variant=ir_variant(module,instruction->symbol_id,&owner);
            if (!variant || !a || module->types[a->type_id].kind != IR_TYPE_NAMED ||
                module->types[a->type_id].symbol_id != owner->symbol_id) return 0;
            if (instruction->opcode == IR_OP_ENUM_IS) return instruction->type == TYPE_BIT && ir_integral_type(module,instruction->type_id);
            return instruction->enum_payload_index < variant->payload_count &&
                instruction->type_id == variant->payload_types[instruction->enum_payload_index] &&
                verified_payload_guard(function,instruction,index);
        }
        case IR_OP_TRAP: return ir_void_type(module,instruction->type_id);

        case IR_OP_CONSTANT:
            return (instruction->has_immediate
                        ? ir_numeric_type(module, instruction->type_id)
                        : instruction->auxiliary_token < function->source_program->token_count) &&
                   (ir_numeric_type(module, instruction->type_id) ||
                    ir_string_type(module, instruction->type_id));
        case IR_OP_LOAD:
            return instruction->auxiliary_token < function->source_program->token_count;
        case IR_OP_DECLARE:
            return a == NULL || ir_types_assignable(module, a->type_id,
                                                    instruction->type_id, a->opcode);
        case IR_OP_STORE:
            if (a == NULL || a->type_id != instruction->type_id) return 0;
            if (a->opcode != IR_OP_LOAD && a->opcode != IR_OP_INDEX &&
                a->opcode != IR_OP_MEMBER &&
                !(a->opcode == IR_OP_UNARY && a->operator_type == TOKEN_STAR))
                return 0;
            if (instruction->operator_type == TOKEN_PLUS_PLUS ||
                instruction->operator_type == TOKEN_MINUS_MINUS)
                return ir_numeric_type(module, a->type_id);
            if (b == NULL || !ir_types_assignable(module, b->type_id, a->type_id,
                                                  b->opcode))
                return 0;
            return instruction->operator_type == TOKEN_EQUAL ||
                   ((instruction->operator_type == TOKEN_PLUS_EQUAL ||
                     instruction->operator_type == TOKEN_MINUS_EQUAL ||
                     instruction->operator_type == TOKEN_STAR_EQUAL ||
                     instruction->operator_type == TOKEN_SLASH_EQUAL) &&
                    ir_numeric_type(module, a->type_id));
        case IR_OP_UNARY:
            if (b == NULL) return 0;
            if (instruction->operator_type == TOKEN_MINUS)
                return ir_numeric_type(module, b->type_id) &&
                       instruction->type_id == b->type_id;
            if (instruction->operator_type == TOKEN_BANG)
                return ir_numeric_type(module, b->type_id) &&
                       instruction->type == TYPE_BIT;
            if (instruction->operator_type == TOKEN_STAR)
                return ir_pointer_type(module, b->type_id) &&
                       module->types[b->type_id].element_type == instruction->type_id;
            if (instruction->operator_type == TOKEN_AMPERSAND)
                return module->types[instruction->type_id].kind == IR_TYPE_POINTER &&
                       (module->types[instruction->type_id].element_type == b->type_id ||
                        (module->types[b->type_id].kind == IR_TYPE_ARRAY &&
                         module->types[instruction->type_id].element_type ==
                         module->types[b->type_id].element_type));
            return 0;
        case IR_OP_BINARY: {
            if (a == NULL || b == NULL) return 0;
            TokenType operation = instruction->operator_type;
            if (operation == TOKEN_AMP_AMP || operation == TOKEN_PIPE_PIPE)
                return ir_numeric_type(module, a->type_id) &&
                       ir_numeric_type(module, b->type_id) && instruction->type == TYPE_BIT;
            if (operation >= TOKEN_EQUAL_EQUAL && operation <= TOKEN_GREATER_EQUAL)
                return instruction->type == TYPE_BIT;
            if (operation < TOKEN_PLUS || operation > TOKEN_PERCENT) return 0;
            if (operation == TOKEN_PLUS && ir_string_type(module, instruction->type_id))
                return (ir_string_type(module, a->type_id) ||
                        ir_numeric_type(module, a->type_id) ||
                        ir_pointer_type(module, a->type_id)) &&
                       (ir_string_type(module, b->type_id) ||
                        ir_numeric_type(module, b->type_id) ||
                        ir_pointer_type(module, b->type_id));
            return ir_numeric_type(module, a->type_id) &&
                   ir_numeric_type(module, b->type_id) &&
                   ir_numeric_type(module, instruction->type_id) &&
                   !(operation == TOKEN_PERCENT &&
                     (ir_floating_type(module, a->type_id) ||
                      ir_floating_type(module, b->type_id)));
        }
        case IR_OP_CALL: {
            if (instruction->symbol_id == AST_SYMBOL_NONE) {
                if (instruction->auxiliary_token >= function->source_program->token_count) return 0;
                const CoreIntrinsic *core = core_intrinsic_find(ast_program_lexeme(
                    function->source_program, instruction->auxiliary_token));
                if (core == NULL) return a != NULL;
                if (instruction->argument_count != core->argument_count ||
                    !core_ir_type_matches(module, instruction->type_id, core->result, 0) ||
                    instruction->type != core_value_type(core->result) ||
                    instruction->pointer_depth != (core->result == CORE_BYTES ? 1U : 0U) ||
                    instruction->is_array || instruction->is_slice || a == NULL ||
                    a->opcode != IR_OP_LOAD || a->auxiliary_token != instruction->auxiliary_token ||
                    a->symbol_id != AST_SYMBOL_NONE) return 0;
                for (size_t argument = 0; argument < core->argument_count; ++argument) {
                    const IrInstruction *value = verified_producer(function,
                        function->arguments[instruction->first_argument + argument], index);
                    if (value == NULL || !core_ir_type_matches(module, value->type_id,
                                                               core->arguments[argument], 1)) return 0;
                }
                return a != NULL;
            }
            const IrFunction *callee = ir_called_function(module, instruction->symbol_id);
            if (callee == NULL || instruction->argument_count != callee->parameter_count ||
                instruction->type_id != callee->return_type_id)
                return 0;
            for (size_t argument = 0; argument < instruction->argument_count; argument++) {
                const IrInstruction *value = verified_producer(function,
                                                               function->arguments[
                                                                   instruction->first_argument + argument], index);
                if (value == NULL) return 0;
                const IrParameter *parameter = &callee->parameters[argument];
                if (parameter->is_receiver) {
                    const IrType *pointer = &module->types[parameter->type_id];
                    if (pointer->kind != IR_TYPE_POINTER ||
                        pointer->element_type != value->type_id)
                        return 0;
                } else if (!ir_types_assignable(module, value->type_id,
                                                parameter->type_id, value->opcode))
                    return 0;
            }
            return 1;
        }
        case IR_OP_INDEX:
            return a != NULL && b != NULL && ir_pointer_type(module, a->type_id) &&
                   ir_integral_type(module, b->type_id) &&
                   module->types[a->type_id].element_type == instruction->type_id;
        case IR_OP_MEMBER:
            return a != NULL && instruction->symbol_id < module->semantics->symbol_count &&
                   (module->semantics->symbols[instruction->symbol_id].kind ==
                    SEMANTIC_SYMBOL_FIELD ||
                    module->semantics->symbols[instruction->symbol_id].kind ==
                    SEMANTIC_SYMBOL_ENUM_VALUE ||
                    module->semantics->symbols[instruction->symbol_id].kind ==
                    SEMANTIC_SYMBOL_FUNCTION);
        case IR_OP_SLICE_LENGTH:
            return a != NULL && a->type_id < module->type_count &&
                   module->types[a->type_id].kind == IR_TYPE_SLICE &&
                   ir_integral_type(module, instruction->type_id);
        case IR_OP_CAST:
            return a != NULL && ir_numeric_type(module, a->type_id) &&
                   ir_numeric_type(module, instruction->type_id);
        case IR_OP_ALLOC:
            return ir_pointer_type(module, instruction->type_id);
        case IR_OP_FREE:
            return a != NULL && (ir_pointer_type(module, a->type_id) ||
                                 ir_string_type(module, a->type_id)) &&
                   ir_void_type(module, instruction->type_id);
        case IR_OP_RETURN:
            return a == NULL
                       ? ir_void_type(module, function->return_type_id)
                       : ir_types_assignable(module, a->type_id, function->return_type_id, a->opcode);
        case IR_OP_BRANCH:
            return a != NULL && ir_numeric_type(module, a->type_id) &&
                   ir_void_type(module, instruction->type_id);
        case IR_OP_JUMP:
        case IR_OP_LABEL:
            return ir_void_type(module, instruction->type_id);
        case IR_OP_PHI:
            return a != NULL && b != NULL && instruction->type == TYPE_BIT &&
                   a->type == TYPE_BIT && b->type == TYPE_BIT;
    }
    return 0;
}

int ir_verify_module(const IrModule *module) { return ir_verify_module_internal(module,0); }

static int ir_verify_module_internal(const IrModule *module,int report) {
    if (module == NULL || module->program == NULL || module->semantics == NULL) return 0;
    if (module->global_count && !module->globals) return 0;
    for (size_t g=0;g<module->global_count;g++) {
        const IrGlobal *global=&module->globals[g];
        if (global->type_id >= module->type_count || global->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[global->symbol_id].kind != SEMANTIC_SYMBOL_VARIABLE ||
            module->semantics->symbols[global->symbol_id].source_program != global->source_program) return 0;
        if (global->string && (module->types[global->type_id].kind != IR_TYPE_PRIMITIVE ||
            module->types[global->type_id].primitive != TYPE_STRING)) return 0;
        for (size_t previous=0;previous<g;previous++) if (module->globals[previous].symbol_id == global->symbol_id) return 0;
    }
    for (size_t t = 0; t < module->type_count; t++) {
        const IrType *type = &module->types[t];
        if (type->kind < IR_TYPE_PRIMITIVE || type->kind > IR_TYPE_SLICE) return 0;
        if (type->kind == IR_TYPE_PRIMITIVE &&
            type->primitive != TYPE_UNKNOWN &&
            (type->primitive < TYPE_INT || type->primitive > TYPE_VOID)) return 0;
        if (type->kind == IR_TYPE_ARRAY && type->array_length == 0) return 0;
        if (type->kind != IR_TYPE_ARRAY && type->array_length != 0) return 0;
        if ((type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_ARRAY ||
             type->kind == IR_TYPE_SLICE) &&
            (type->element_type == IR_TYPE_NONE || type->element_type >= t))
            return 0;
        if ((type->kind == IR_TYPE_PRIMITIVE || type->kind == IR_TYPE_NAMED) &&
            type->element_type != IR_TYPE_NONE)
            return 0;
        if (type->kind == IR_TYPE_NAMED &&
            (type->symbol_id == AST_SYMBOL_NONE ||
             type->symbol_id >= module->semantics->symbol_count ||
             (module->semantics->symbols[type->symbol_id].kind != SEMANTIC_SYMBOL_STRUCT &&
              module->semantics->symbols[type->symbol_id].kind != SEMANTIC_SYMBOL_ENUM)))
            return 0;
    }
    for (size_t s = 0; s < module->structure_count; s++) {
        const IrAggregate *structure = &module->structures[s];
        if (structure->symbol_id == AST_SYMBOL_NONE ||
            structure->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[structure->symbol_id].kind != SEMANTIC_SYMBOL_STRUCT)
            return 0;
        for (size_t field = 0; field < structure->field_count; field++)
            if (structure->fields[field].type_id >= module->type_count ||
                structure->fields[field].symbol_id == AST_SYMBOL_NONE)
                return 0;
    }
    for (size_t e = 0; e < module->enum_count; e++) {
        const IrEnum *enumeration = &module->enums[e];
        if (enumeration->is_sum != 0 && enumeration->is_sum != 1) return 0;
        if (enumeration->variant_count && !enumeration->variants) return 0;
        if (enumeration->symbol_id == AST_SYMBOL_NONE ||
            enumeration->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[enumeration->symbol_id].kind != SEMANTIC_SYMBOL_ENUM)
            return 0;
        for (size_t variant = 0; variant < enumeration->variant_count; variant++) {
            const IrEnumVariant *item = &enumeration->variants[variant];
            if (item->payload_count && (!enumeration->is_sum || !item->payload_types)) return 0;
            if (item->symbol_id == AST_SYMBOL_NONE ||
                item->argument_count != enumeration->field_count ||
                item->first_argument > enumeration->variant_argument_count ||
                item->argument_count > enumeration->variant_argument_count -
                item->first_argument)
                return 0;
            for (size_t p=0; p<item->payload_count; p++)
                if (item->payload_types[p] >= module->type_count) return 0;
            for (size_t argument = 0; argument < item->argument_count; argument++)
                if (enumeration->variant_arguments[item->first_argument + argument].type_id >=
                    module->type_count)
                    return 0;
        }
    }
    for (size_t f = 0; f < module->function_count; f++) {
        const IrFunction *function = &module->functions[f];
        if (function->symbol_id == AST_SYMBOL_NONE ||
            function->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[function->symbol_id].kind !=
            SEMANTIC_SYMBOL_FUNCTION ||
            module->semantics->symbols[function->symbol_id].source_program !=
            function->source_program ||
            module->semantics->symbols[function->symbol_id].name_token !=
            function->name_token || function->return_type_id >= module->type_count)
            return 0;
        if ((function->owner_token == AST_TOKEN_NONE) !=
            (function->owner_symbol_id == AST_SYMBOL_NONE))
            return 0;
        if (function->owner_symbol_id != AST_SYMBOL_NONE &&
            (function->owner_symbol_id >= module->semantics->symbol_count ||
             module->semantics->symbols[function->owner_symbol_id].kind !=
             SEMANTIC_SYMBOL_STRUCT))
            return 0;
        for (size_t p = 0; p < function->parameter_count; p++) {
            const IrParameter *parameter = &function->parameters[p];
            if (parameter->is_receiver) {
                if (p != 0 || function->owner_symbol_id == AST_SYMBOL_NONE ||
                    parameter->symbol_id != function->owner_symbol_id ||
                    parameter->pointer_depth != 1 ||
                    parameter->type_id >= module->type_count)
                    return 0;
                continue;
            }
            const SemanticSymbol *symbol = parameter->symbol_id <
                                           module->semantics->symbol_count
                                               ? &module->semantics->symbols[parameter->symbol_id]
                                               : NULL;
            if (parameter->source_program == NULL ||
                parameter->name_token >= parameter->source_program->token_count ||
                parameter->symbol_id == AST_SYMBOL_NONE ||
                parameter->symbol_id >= module->semantics->symbol_count ||
                symbol->kind != SEMANTIC_SYMBOL_PARAMETER ||
                symbol->source_program != parameter->source_program ||
                symbol->name_token != parameter->name_token ||
                symbol->owner_token != function->name_token ||
                ast_type_data_type(symbol->source_program, &symbol->declared_type) !=
                parameter->type ||
                symbol->declared_type.pointer_depth + symbol->declared_type.outer_pointer_depth != parameter->pointer_depth)
                return 0;
            if (parameter->type_id >= module->type_count) return 0;
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
        size_t failing_instruction=IR_VALUE_NONE;
        int valid = 1;
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            failing_instruction=i;
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
            failing_instruction=i;
            if ((instruction->has_immediate != 0 && instruction->has_immediate != 1) ||
                (instruction->has_immediate && instruction->opcode != IR_OP_CONSTANT)) {
                valid = 0;
                break;
            }
            int produces_value = instruction_produces_value(instruction);
            if ((produces_value && instruction->result == IR_VALUE_NONE) ||
                (!produces_value && instruction->result != IR_VALUE_NONE) ||
                (instruction->result != IR_VALUE_NONE &&
                 (instruction->result >= function->next_value ||
                  defined[instruction->result] != 0))) {
                valid = 0;
                break;
            }
            if (instruction->type_id >= module->type_count) {
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
                case IR_OP_ALLOC:
                case IR_OP_LABEL:
                    break;
                case IR_OP_CAST:
                case IR_OP_FREE:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_DECLARE:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
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
                case IR_OP_PHI:
                    REQUIRE_VALUE(instruction->operand_a);
                    REQUIRE_VALUE(instruction->operand_b);
                    REQUIRE_LABEL(instruction->target_a);
                    REQUIRE_LABEL(instruction->target_b);
                    if (instruction->target_a == instruction->target_b) valid = 0;
                    break;
                case IR_OP_MEMBER:
                case IR_OP_SLICE_LENGTH:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_ENUM_IS:
                case IR_OP_ENUM_PAYLOAD:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->opcode == IR_OP_ENUM_PAYLOAD) REQUIRE_LABEL(instruction->target_a);
                    break;
                case IR_OP_TRAP: break;
                case IR_OP_ENUM_CONSTRUCT:
                    if (instruction->first_argument > function->argument_count ||
                        instruction->argument_count > function->argument_count-instruction->first_argument) valid=0;
                    else for (size_t n=0; n<instruction->argument_count; n++)
                        REQUIRE_VALUE(function->arguments[instruction->first_argument+n]);
                    break;
                case IR_OP_CALL:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
                    else if (instruction->symbol_id == AST_SYMBOL_NONE)
                        valid = 0;
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
                case IR_OP_RETURN:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
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
            if (valid && !verify_instruction_types(module, function, instruction, i))
                valid = 0;
            if (valid && instruction->result != IR_VALUE_NONE)
                defined[instruction->result] = 1;
        }
        free(defined);
        free(labels);
        if (!valid || !ir_verify_control_flow(function, ir_void_type(module, function->return_type_id))) {
            if (report) ir_report_failure(function,valid ? IR_VALUE_NONE : failing_instruction,
                "IR verification",valid ? "malformed control flow" : "invalid instruction types or operands");
            return 0;
        }
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
    for (size_t i = 0; i < module->structure_count; i++)
        free(module->structures[i].fields);
    for (size_t i = 0; i < module->enum_count; i++) {
        free(module->enums[i].fields);
        if (module->enums[i].variants != NULL)
            for (size_t v=0; v<module->enums[i].variant_count; v++) free(module->enums[i].variants[v].payload_types);
        free(module->enums[i].variants);
        free(module->enums[i].variant_arguments);
    }
    free(module->functions);
    free(module->types);
    free(module->structures);
    free(module->enums);
    free(module->imports);
    free(module->globals);
    free(module);
}
