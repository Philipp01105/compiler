#include "ir.h"
#include "core_intrinsics.h"
#include "ir_cfg.h"
#include "ir_verify.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void ir_report_failure(const IrFunction *function, size_t index,
                       const char *stage, const char *reason) {
    if (function == NULL || function->source_program == NULL) return;
    const IrInstruction *instruction = index < function->instruction_count
                                           ? &function->instructions[index]
                                           : NULL;
    AstSourceSpan span = instruction == NULL ? function->return_type.span : instruction->span;
    char message[2048];
    if (instruction != NULL)
        snprintf(message, sizeof(message),
                 "%s failed in function '%s', IR instruction %zu "
                 "(opcode=%d result=%zu operands=%zu,%zu type=%zu): %s",
                 stage, ast_program_lexeme(function->source_program, function->name_token),
                 index, (int) instruction->opcode, instruction->result,
                 instruction->operand_a, instruction->operand_b, instruction->type_id, reason);
    else
        snprintf(message, sizeof(message), "%s failed in function '%s': %s", stage,
                 ast_program_lexeme(function->source_program, function->name_token), reason);
    ErrorContext *context = error_context_create(SEVERITY_ERROR, span.begin.line,
                                                 span.begin.column, ERROR_CATEGORY_COMPILER, ERR_COMP_INTERNAL_FAILURE,
                                                 function->source_program->source_path, message);
    error_context_set_span(context, span.end.line, span.end.column);
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
}

#include "type_layout.inc"

typedef struct DeferredAction {
    int captured_call;
    int drop_local;
    int free_slice_backing;
    IrInstruction call;
    IrTypeId drop_type_id;
    size_t drop_symbol_id;
    AstSourceSpan drop_span;
    size_t *arguments;
    size_t argument_count;
    const AstStatement *body;
    struct DeferredAction *next;
} DeferredAction;

typedef struct CleanupScope {
    DeferredAction *actions;
    struct CleanupScope *previous;
} CleanupScope;

typedef struct {
    IrModule *module;
    IrFunction *function;
    const AstProgram *program;
    size_t break_label;
    size_t continue_label;
    CleanupScope *cleanup_scope;
    CleanupScope *break_cleanup_stop;
    CleanupScope *continue_cleanup_stop;
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
    *instruction = (IrInstruction)
    {
        .source_program = function->is_package_init ? builder->program : NULL,
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

DataType ir_ast_type_data_type(const AstProgram *program, const AstType *type) {
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

static int same_ir_type(const IrType *left, const IrType *right) {
    return left->kind == right->kind && left->primitive == right->primitive &&
           left->symbol_id == right->symbol_id && left->element_type == right->element_type &&
           left->array_length == right->array_length && left->signature_id == right->signature_id;
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
    if (symbol == NULL)
        symbol = semantic_find_in_package(module->semantics, program, name, SEMANTIC_SYMBOL_INTERFACE);
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
    if (type != NULL && type->element_type != NULL) {
        IrTypeId result = type_from_ast(module, program, type->element_type);
        if (result == IR_TYPE_NONE) return result;
        IrType container = {
            .kind = type->is_slice ? IR_TYPE_SLICE : IR_TYPE_ARRAY,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = result,
            .array_length = ast_array_length(program, type)
        };
        result = intern_type(module, container);
        for (unsigned depth = 0;
             result != IR_TYPE_NONE &&
             depth < type->pointer_depth + type->outer_pointer_depth +
                         (type->borrow_kind != AST_BORROW_NONE);
             depth++) {
            IrType pointer = {
                .kind = IR_TYPE_POINTER,
                .primitive = TYPE_UNKNOWN,
                .symbol_id = AST_SYMBOL_NONE,
                .element_type = result
            };
            result = intern_type(module, pointer);
        }
        return result;
    }
    if (type != NULL && type->kind == AST_TYPE_FUNCTION) {
        if (type->function_generic_parameters != NULL) return IR_TYPE_NONE;
        size_t count = 0;
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
            count++;
        IrTypeId *parameters = count ? calloc(count, sizeof(*parameters)) : NULL;
        if (count && parameters == NULL) return IR_TYPE_NONE;
        size_t index = 0;
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
            parameters[index++] = type_from_ast(module, program, &parameter->type);
        IrTypeId result = type_from_ast(module, program, type->function_return_type);
        if (result == IR_TYPE_NONE) { free(parameters); return IR_TYPE_NONE; }
        size_t signature = 0;
        for (; signature < module->signature_count; signature++) {
            const IrFunctionSignature *candidate = &module->signatures[signature];
            if (candidate->parameter_count != count || candidate->return_type != result) continue;
            size_t p = 0;
            for (; p < count && candidate->parameter_types[p] == parameters[p]; p++) {}
            if (p == count) break;
        }
        if (signature == module->signature_count) {
            if (module->signature_count == module->signature_capacity &&
                !grow_array((void **) &module->signatures, &module->signature_capacity,
                            sizeof(*module->signatures))) { free(parameters); return IR_TYPE_NONE; }
            module->signatures[module->signature_count++] = (IrFunctionSignature) {
                .parameter_types = parameters, .parameter_count = count, .return_type = result
            };
        } else free(parameters);
        IrTypeId value = intern_type(module, (IrType) {
            .kind = IR_TYPE_FUNCTION, .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE, .element_type = IR_TYPE_NONE,
            .signature_id = signature
        });
        if (value == IR_TYPE_NONE) return value;
        if (type->is_array || type->is_slice)
            value = intern_type(module, (IrType) {
            .kind = type->is_slice ? IR_TYPE_SLICE : IR_TYPE_ARRAY,
            .primitive = TYPE_UNKNOWN,
            .symbol_id = AST_SYMBOL_NONE,
            .element_type = value,
            .array_length = ast_array_length(program, type)
            });
        for (unsigned depth = 0;
             value != IR_TYPE_NONE &&
             depth < type->pointer_depth + type->outer_pointer_depth +
                         (type->borrow_kind != AST_BORROW_NONE);
             depth++) {
            IrType pointer = {
                .kind = IR_TYPE_POINTER,
                .primitive = TYPE_UNKNOWN,
                .symbol_id = AST_SYMBOL_NONE,
                .element_type = value
            };
            value = intern_type(module, pointer);
        }
        return value;
    }
    size_t named = type != NULL && type->name_token < program->token_count &&
                   program->tokens[type->name_token].type == TOKEN_IDENTIFIER
                       ? type->name_token
                       : AST_TOKEN_NONE;
    return type_from_parts(module, ir_ast_type_data_type(program, type),
                            type == NULL ? 0 : type->pointer_depth +
                                (type->borrow_kind != AST_BORROW_NONE), named,
                           type != NULL && type->is_array,
                           type != NULL && type->is_slice,
                           ast_array_length(program, type),
                           type == NULL ? 0 : type->outer_pointer_depth,
                           program, AST_SYMBOL_NONE);
}

static IrTypeId type_from_expression(IrModule *module, const AstProgram *program,
                                     const AstExpression *expression) {
    if (expression->has_resolved_ast_type &&
        (expression->resolved_ast_type.element_type != NULL ||
         expression->resolved_ast_type.kind == AST_TYPE_FUNCTION))
        return type_from_ast(module,
                             expression->resolved_type_program != NULL
                                 ? expression->resolved_type_program : program,
                             &expression->resolved_ast_type);
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
    instruction->is_slice = expression->resolved_is_slice && !expression->resolved_outer_pointer_depth;
    instruction->type_id = IR_TYPE_NONE;
    if ((expression->resolved_is_array || expression->resolved_is_slice) &&
        expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
        const SemanticSymbol *symbol =
                &builder->module->semantics->symbols[expression->resolved_symbol_id];
        if (symbol->declared_type.kind != AST_TYPE_INFERRED)
            instruction->type_id = type_from_ast(builder->module, symbol->source_program, &symbol->declared_type);
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
static void emit_deferred_until(IrBuilder *builder,
                                const CleanupScope *stop);

static size_t coerce_slice(IrBuilder *builder, size_t value, IrTypeId target, AstSourceSpan span) {
    if (target >= builder->module->type_count || builder->module->types[target].kind != IR_TYPE_SLICE || value ==
        IR_VALUE_NONE)
        return value;
    IrInstruction source = {0};
    int found = 0;
    for (size_t i = 0; i < builder->function->instruction_count; i++)
        if (builder->function->instructions[i].result == value) {
            source = builder->function->instructions[i];
            found = 1;
            break;
        }
    if (!found || source.type_id >= builder->module->type_count || builder->module->types[source.type_id].kind !=
        IR_TYPE_ARRAY)
        return value;
    size_t length = builder->module->types[source.type_id].array_length;
    IrInstruction *count = emit(builder, IR_OP_CONSTANT, span);
    if (!count) return IR_VALUE_NONE;
    count->type = TYPE_USIZE;
    count->type_id = type_from_parts(builder->module, TYPE_USIZE, 0, AST_TOKEN_NONE, 0, 0, 0, 0, builder->program,
                                     AST_SYMBOL_NONE);
    count->has_immediate = 1;
    count->immediate = length;
    count->result = new_value(builder);
    size_t count_value = count->result;
    IrInstruction *slice = emit(builder, IR_OP_SLICE, span);
    if (!slice) return IR_VALUE_NONE;
    slice->type_id = target;
    slice->type = source.type;
    slice->pointer_depth = source.pointer_depth;
    slice->type_name_token = source.type_name_token;
    slice->is_slice = 1;
    slice->operand_a = value;
    slice->operand_b = count_value;
    slice->result = new_value(builder);
    return slice->result;
}

static void emit_move_if_owned(IrBuilder *builder,
                               const AstExpression *expression);

static size_t lower_expression(IrBuilder *builder, const AstExpression *expression) {
    if (expression == NULL) return IR_VALUE_NONE;
    if (expression->kind == AST_EXPR_PROPAGATE) {
        if (expression->propagation_branch_symbol_id >=
                builder->module->semantics->symbol_count ||
            expression->propagation_from_residual_symbol_id >=
                builder->module->semantics->symbol_count) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
        const SemanticSymbol *branch_target =
            &builder->module->semantics->symbols[
                expression->propagation_branch_symbol_id];
        const SemanticSymbol *from_target =
            &builder->module->semantics->symbols[
                expression->propagation_from_residual_symbol_id];
        size_t operand = lower_expression(builder, expression->left);
        emit_move_if_owned(builder, expression->left);
        size_t branch_argument = builder->function->argument_count;
        if (!append_argument(builder, operand)) return IR_VALUE_NONE;
        IrInstruction *branch_call = emit(builder, IR_OP_CALL,
                                          expression->span);
        if (branch_call == NULL) return IR_VALUE_NONE;
        branch_call->symbol_id = branch_target->id;
        branch_call->first_argument = branch_argument;
        branch_call->argument_count = 1;
        branch_call->result = new_value(builder);
        branch_call->type_id = type_from_ast(
            builder->module, branch_target->source_program,
            &branch_target->declaration->as.function.return_type);
        branch_call->type = ir_ast_type_data_type(
            branch_target->source_program,
            &branch_target->declaration->as.function.return_type);
        size_t propagated = branch_call->result;

        size_t failure_label = new_label(builder);
        size_t continue_check_label = new_label(builder);
        size_t success_label = new_label(builder);
        size_t invalid_label = new_label(builder);
        IrInstruction *test = emit(builder, IR_OP_ENUM_IS,
                                   expression->span);
        if (test == NULL) return IR_VALUE_NONE;
        test->operand_a = propagated;
        test->symbol_id = expression->propagation_break_symbol_id;
        test->result = new_value(builder);
        test->type = TYPE_BIT;
        test->type_id = type_from_parts(
            builder->module, TYPE_BIT, 0, AST_TOKEN_NONE, 0, 0, 0, 0,
            builder->program, AST_SYMBOL_NONE);
        IrInstruction *select = emit(builder, IR_OP_BRANCH,
                                     expression->span);
        if (select == NULL) return IR_VALUE_NONE;
        set_void_type(builder, select);
        select->operand_a = test->result;
        select->target_a = failure_label;
        select->target_b = continue_check_label;

        emit_label(builder, failure_label, expression->span);
        IrInstruction *residual = emit(builder, IR_OP_ENUM_PAYLOAD,
                                       expression->span);
        if (residual == NULL) return IR_VALUE_NONE;
        residual->operand_a = propagated;
        residual->symbol_id = expression->propagation_break_symbol_id;
        residual->enum_payload_index = 0;
        residual->target_a = failure_label;
        residual->result = new_value(builder);
        const SemanticSymbol *break_variant =
            &builder->module->semantics->symbols[
                expression->propagation_break_symbol_id];
        const AstType *residual_type =
            &((const AstEnumValue *) break_variant->node)->payload_types->type;
        residual->type_id = type_from_ast(builder->module,
                                          break_variant->source_program,
                                          residual_type);
        residual->type = ir_ast_type_data_type(break_variant->source_program,
                                               residual_type);
        size_t conversion_argument = builder->function->argument_count;
        if (!append_argument(builder, residual->result))
            return IR_VALUE_NONE;
        IrInstruction *conversion = emit(builder, IR_OP_CALL,
                                         expression->span);
        if (conversion == NULL) return IR_VALUE_NONE;
        conversion->symbol_id = from_target->id;
        conversion->first_argument = conversion_argument;
        conversion->argument_count = 1;
        conversion->result = new_value(builder);
        conversion->type_id = type_from_ast(
            builder->module, from_target->source_program,
            &from_target->declaration->as.function.return_type);
        conversion->type = ir_ast_type_data_type(
            from_target->source_program,
            &from_target->declaration->as.function.return_type);
        size_t return_value = conversion->result;
        if (expression->propagation_return_variant_symbol_id !=
            AST_SYMBOL_NONE) {
            size_t wrapper_argument = builder->function->argument_count;
            if (!append_argument(builder, return_value))
                return IR_VALUE_NONE;
            IrInstruction *wrapper = emit(builder, IR_OP_ENUM_CONSTRUCT,
                                          expression->span);
            if (wrapper == NULL) return IR_VALUE_NONE;
            wrapper->symbol_id =
                expression->propagation_return_variant_symbol_id;
            wrapper->first_argument = wrapper_argument;
            wrapper->argument_count = 1;
            wrapper->result = new_value(builder);
            wrapper->type_id = builder->function->return_type_id;
            wrapper->type = ir_ast_type_data_type(
                builder->program, &builder->function->return_type);
            return_value = wrapper->result;
        }
        emit_deferred_until(builder, NULL);
        IrInstruction *early_return = emit(builder, IR_OP_RETURN,
                                            expression->span);
        if (early_return == NULL) return IR_VALUE_NONE;
        early_return->operand_a = return_value;
        early_return->type_id = builder->function->return_type_id;
        early_return->type = conversion->type;

        emit_label(builder, invalid_label, expression->span);
        IrInstruction *invalid = emit(builder, IR_OP_TRAP,
                                      expression->span);
        if (invalid == NULL) return IR_VALUE_NONE;
        set_void_type(builder, invalid);
        emit_label(builder, continue_check_label, expression->span);
        IrInstruction *continue_test = emit(builder, IR_OP_ENUM_IS,
                                            expression->span);
        if (continue_test == NULL) return IR_VALUE_NONE;
        continue_test->operand_a = propagated;
        continue_test->symbol_id =
            expression->propagation_continue_symbol_id;
        continue_test->result = new_value(builder);
        continue_test->type = TYPE_BIT;
        continue_test->type_id = type_from_parts(
            builder->module, TYPE_BIT, 0, AST_TOKEN_NONE, 0, 0, 0, 0,
            builder->program, AST_SYMBOL_NONE);
        IrInstruction *continue_select = emit(builder, IR_OP_BRANCH,
                                              expression->span);
        if (continue_select == NULL) return IR_VALUE_NONE;
        set_void_type(builder, continue_select);
        continue_select->operand_a = continue_test->result;
        continue_select->target_a = success_label;
        continue_select->target_b = invalid_label;
        emit_label(builder, success_label, expression->span);
        IrInstruction *output = emit(builder, IR_OP_ENUM_PAYLOAD,
                                     expression->span);
        if (output == NULL) return IR_VALUE_NONE;
        output->operand_a = propagated;
        output->symbol_id = expression->propagation_continue_symbol_id;
        output->enum_payload_index = 0;
        output->target_a = success_label;
        output->result = new_value(builder);
        set_expression_type(builder, output, expression);
        const SemanticSymbol *continue_variant =
            &builder->module->semantics->symbols[
                expression->propagation_continue_symbol_id];
        const AstType *output_type =
            &((const AstEnumValue *) continue_variant->node)->payload_types->type;
        output->type_id = type_from_ast(builder->module,
                                        continue_variant->source_program,
                                        output_type);
        return output->result;
    }
    if (expression->kind == AST_EXPR_ENUM_ACCESS) {
        size_t value = lower_expression(builder, expression->left->left);
        size_t success = new_label(builder), failure = new_label(builder), join = new_label(builder);
        IrInstruction *test = emit(builder, IR_OP_ENUM_IS, expression->span);
        if (!test) return IR_VALUE_NONE;
        test->operand_a = value;
        test->symbol_id = expression->resolved_symbol_id;
        test->result = new_value(builder);
        test->type = TYPE_BIT;
        test->type_id = type_from_parts(builder->module, TYPE_BIT, 0, AST_TOKEN_NONE, 0, 0, 0, 0, builder->program,
                                        AST_SYMBOL_NONE);
        size_t condition = test->result;
        IrInstruction *branch = emit(builder, IR_OP_BRANCH, expression->span);
        if (!branch) return IR_VALUE_NONE;
        set_void_type(builder, branch);
        branch->operand_a = condition;
        branch->target_a = success;
        branch->target_b = failure;
        emit_label(builder, success, expression->span);
        IrInstruction *payload = emit(builder, IR_OP_ENUM_PAYLOAD, expression->span);
        if (!payload) return IR_VALUE_NONE;
        set_expression_type(builder, payload, expression);
        const SemanticSymbol *variant = &builder->module->semantics->symbols[expression->resolved_symbol_id];
        const AstEnumValue *variant_value = variant->node;
        payload->type_id = type_from_ast(builder->module, variant->source_program, &variant_value->payload_types->type);
        payload->operand_a = value;
        payload->symbol_id = expression->resolved_symbol_id;
        payload->enum_payload_index = 0;
        payload->target_a = success;
        payload->result = new_value(builder);
        size_t result = payload->result;
        IrInstruction *jump = emit(builder, IR_OP_JUMP, expression->span);
        if (!jump) return IR_VALUE_NONE;
        set_void_type(builder, jump);
        jump->target_a = join;
        emit_label(builder, failure, expression->span);
        IrInstruction *trap = emit(builder, IR_OP_TRAP, expression->span);
        set_void_type(builder, trap);
        emit_label(builder, join, expression->span);
        return result;
    }
    if (expression->folded_constant.lexeme != NULL) {
        AstProgram *program = (AstProgram *) builder->function->source_program;
        AstToken *tokens = realloc(program->tokens, (program->token_count + 1) * sizeof(*tokens));
        if (tokens == NULL) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
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
    if (expression->kind == AST_EXPR_ARRAY_LITERAL) {
        size_t count = 0;
        for (const AstExpression *element = expression->arguments;
             element != NULL; element = element->next) count++;
        size_t *values = count == 0 ? NULL : calloc(count, sizeof(*values));
        if (values == NULL && count != 0) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
        size_t index = 0;
        for (const AstExpression *element = expression->arguments;
             element != NULL; element = element->next) {
            values[index++] = lower_expression(builder, element);
            emit_move_if_owned(builder, element);
        }
        IrInstruction *instruction = emit(builder, IR_OP_ARRAY_LITERAL,
                                          expression->span);
        if (instruction == NULL) {
            free(values);
            return IR_VALUE_NONE;
        }
        instruction->result = new_value(builder);
        instruction->first_argument = builder->function->argument_count;
        instruction->argument_count = count;
        for (size_t element = 0; element < count; element++)
            if (!append_argument(builder, values[element])) {
                free(values);
                return IR_VALUE_NONE;
            }
        free(values);
        instruction->element_count = expression->literal_element_count;
        set_expression_type(builder, instruction, expression);
        return instruction->result;
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
    int direct_call = expression->kind == AST_EXPR_CALL &&
                      expression->resolved_symbol_id < builder->module->semantics->symbol_count &&
                      builder->module->semantics->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION;
    int function_reference = expression->kind == AST_EXPR_MEMBER &&
                             expression->resolved_symbol_id < builder->module->semantics->symbol_count &&
                             builder->module->semantics->symbols[expression->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_FUNCTION &&
                             expression->has_resolved_ast_type &&
                             expression->resolved_ast_type.kind == AST_TYPE_FUNCTION;
    size_t left = (method_call || expression->kind == AST_EXPR_ENUM_CONSTRUCT || direct_call ||
                   function_reference)
                      ? IR_VALUE_NONE
                      : lower_expression(builder, expression->left);
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
        case AST_EXPR_NAME:
            opcode = expression->resolved_symbol_id < builder->module->semantics->symbol_count &&
                     builder->module->semantics->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION
                         ? IR_OP_FUNCTION_ADDRESS : IR_OP_LOAD;
            break;
        case AST_EXPR_UNARY: opcode = IR_OP_UNARY;
            break;
        case AST_EXPR_BINARY: opcode = IR_OP_BINARY;
            break;
        case AST_EXPR_ENUM_CONSTRUCT: opcode = IR_OP_ENUM_CONSTRUCT;
            break;
        case AST_EXPR_ENUM_ACCESS: return IR_VALUE_NONE;
        case AST_EXPR_CALL: opcode = IR_OP_CALL;
            break;
        case AST_EXPR_INDEX: opcode = IR_OP_INDEX;
            break;
        case AST_EXPR_SUBSLICE: opcode = IR_OP_SUBSLICE;
            break;
        case AST_EXPR_MEMBER: opcode = function_reference
                                           ? IR_OP_FUNCTION_ADDRESS
                                           : IR_OP_MEMBER;
            break;
        case AST_EXPR_SLICE_LENGTH: opcode = IR_OP_SLICE_LENGTH;
            break;
        case AST_EXPR_SLICE_DATA: opcode = IR_OP_SLICE_DATA;
            break;
        case AST_EXPR_SLICE: opcode = IR_OP_SLICE;
            break;
        case AST_EXPR_SIZEOF:
        case AST_EXPR_ALIGNOF:
        case AST_EXPR_TYPE_INFO:
        case AST_EXPR_TYPE_PROPERTY: return IR_VALUE_NONE;
        case AST_EXPR_ARRAY_LITERAL: return IR_VALUE_NONE;
        case AST_EXPR_PROPAGATE: return IR_VALUE_NONE;
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
        const AstParameter *physical_parameter = NULL;
        if (expression->kind == AST_EXPR_CALL &&
            expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
            const SemanticSymbol *target =
                &builder->module->semantics->symbols[expression->resolved_symbol_id];
            if (target->declaration != NULL)
                physical_parameter = target->declaration->as.function.parameters;
        }
        if (has_receiver) argument_count++;
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next) {
            int erased = physical_parameter != NULL &&
                         physical_parameter->compile_time_value != NULL;
            if (!erased) argument_count++;
            if (physical_parameter != NULL)
                physical_parameter = physical_parameter->next;
        }
        size_t *values = argument_count == 0 ? NULL : calloc(argument_count, sizeof(*values));
        if (values == NULL && argument_count != 0) {
            builder->failed = 1;
            return IR_VALUE_NONE;
        }
        size_t argument_index = 0;
        if (has_receiver) values[argument_index++] = receiver;
        const AstParameter *parameter = NULL;
        const AstTypeArgument *payload = NULL;
        const AstProgram *type_unit = NULL;
        if (expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
            const SemanticSymbol *target = &builder->module->semantics->symbols[expression->resolved_symbol_id];
            type_unit = target->source_program;
            if (expression->kind == AST_EXPR_CALL && target->declaration)
                parameter = target->declaration->as.function.parameters;
            else if (expression->kind == AST_EXPR_ENUM_CONSTRUCT && target->node)
                payload = ((const AstEnumValue *) target->node)->payload_types;
        }
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next) {
            if (parameter != NULL && parameter->compile_time_value != NULL) {
                parameter = parameter->next;
                continue;
            }
            size_t value = lower_expression(builder, argument);
            int consumes = parameter == NULL ||
                           parameter->type.borrow_kind == AST_BORROW_NONE;
            if (parameter || payload) {
                const AstType *type = parameter ? &parameter->type : &payload->type;
                value = coerce_slice(builder, value, type_from_ast(builder->module, type_unit, type), argument->span);
                if (parameter) parameter = parameter->next;
                else payload = payload->next;
            }
            values[argument_index++] = value;
            if (consumes) emit_move_if_owned(builder, argument);
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
    size_t subslice_end = IR_VALUE_NONE;
    size_t subslice_argument = IR_VALUE_NONE;
    if (expression->kind == AST_EXPR_SUBSLICE &&
        expression->arguments != NULL) {
        subslice_end = lower_expression(builder, expression->arguments);
        subslice_argument = builder->function->argument_count;
        if (!append_argument(builder, subslice_end)) return IR_VALUE_NONE;
    }
    IrInstruction *instruction = emit(builder, opcode, expression->span);
    if (instruction == NULL) return IR_VALUE_NONE;
    if (!((opcode == IR_OP_CALL || opcode == IR_OP_FREE) &&
          !data_type_has_value(expression->resolved_type)))
        instruction->result = new_value(builder);
    instruction->operand_a = left;
    instruction->operand_b = right;
    instruction->auxiliary_token = expression->value_token;
    instruction->symbol_id = expression->resolved_symbol_id;
    instruction->operator_type = expression->operator_type;
    set_expression_type(builder, instruction, expression);
    if (expression->kind == AST_EXPR_SUBSLICE &&
        expression->arguments != NULL) {
        instruction->first_argument = subslice_argument;
        instruction->argument_count = 1;
    }
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
    size_t result = instruction->result;
    if (expression->kind == AST_EXPR_CALL) {
        size_t argument_index = has_receiver ? 1 : 0;
        const AstParameter *parameter = NULL;
        if (expression->resolved_symbol_id < builder->module->semantics->symbol_count) {
            const SemanticSymbol *target =
                &builder->module->semantics->symbols[expression->resolved_symbol_id];
            if (target->declaration != NULL)
                parameter = target->declaration->as.function.parameters;
        }
        for (const AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next) {
            int erased = parameter != NULL &&
                         parameter->compile_time_value != NULL;
            if (!erased) {
                if (argument->owns_slice_backing) {
                    IrInstruction *cleanup = emit(builder,
                                                  IR_OP_FREE_SLICE_BACKING,
                                                  argument->span);
                    if (cleanup == NULL) return IR_VALUE_NONE;
                    cleanup->type = TYPE_VOID;
                    cleanup->type_id = type_from_expression(builder->module,
                                                            builder->program,
                                                            argument);
                    cleanup->operand_a = builder->function->arguments[
                        first_argument + argument_index];
                }
                argument_index++;
            }
            if (parameter != NULL) parameter = parameter->next;
        }
    }
    if (expression->kind == AST_EXPR_CALL &&
        expression->resolved_type == TYPE_NEVER) {
        IrInstruction *unreachable = emit(builder, IR_OP_TRAP,
                                          expression->span);
        if (unreachable != NULL) set_void_type(builder, unreachable);
        return IR_VALUE_NONE;
    }
    return result;
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement);
static void lower_scoped_statement(IrBuilder *builder,
                                   const AstStatement *statement);

static void emit_deferred_scope(IrBuilder *builder,
                                const CleanupScope *scope) {
    if (scope == NULL) return;
    for (const DeferredAction *action = scope->actions;
         action != NULL && !builder->failed; action = action->next) {
        if (action->free_slice_backing) {
            IrInstruction *instruction = emit(builder, IR_OP_FREE_SLICE_BACKING,
                                               action->drop_span);
            if (instruction == NULL) return;
            instruction->type_id = action->drop_type_id;
            instruction->symbol_id = action->drop_symbol_id;
            instruction->type = TYPE_VOID;
        } else if (action->drop_local) {
            IrInstruction *instruction = emit(builder, IR_OP_DROP,
                                               action->drop_span);
            if (instruction == NULL) return;
            instruction->type_id = action->drop_type_id;
            instruction->symbol_id = action->drop_symbol_id;
            instruction->type = TYPE_VOID;
        } else if (action->captured_call) {
            IrInstruction *instruction =
                emit(builder, action->call.opcode, action->call.span);
            if (instruction == NULL) return;
            *instruction = action->call;
            instruction->first_argument =
                builder->function->argument_count;
            instruction->argument_count = action->argument_count;
            for (size_t i = 0; i < action->argument_count; i++)
                if (!append_argument(builder, action->arguments[i]))
                    return;
            if (instruction->result != IR_VALUE_NONE)
                instruction->result = new_value(builder);
        } else {
            lower_scoped_statement(builder, action->body);
        }
    }
}

static void register_slice_backing_cleanup(IrBuilder *builder,
                                           size_t symbol_id,
                                           IrTypeId type_id,
                                           AstSourceSpan span) {
    if (builder->cleanup_scope == NULL) {
        builder->failed = 1;
        return;
    }
    DeferredAction *action = calloc(1, sizeof(*action));
    if (action == NULL) {
        builder->failed = 1;
        return;
    }
    action->free_slice_backing = 1;
    action->drop_type_id = type_id;
    action->drop_symbol_id = symbol_id;
    action->drop_span = span;
    action->next = builder->cleanup_scope->actions;
    builder->cleanup_scope->actions = action;
}

static int type_needs_drop(const IrBuilder *builder, IrTypeId type) {
    return (ir_type_properties(builder->module, type) &
            SEMANTIC_TYPE_NEEDS_DROP) != 0;
}

static void register_local_drop(IrBuilder *builder, size_t symbol_id,
                                IrTypeId type_id, AstSourceSpan span) {
    if (!type_needs_drop(builder, type_id)) return;
    if (builder->cleanup_scope == NULL) {
        builder->failed = 1;
        return;
    }
    DeferredAction *action = calloc(1, sizeof(*action));
    if (action == NULL) {
        builder->failed = 1;
        return;
    }
    action->drop_local = 1;
    action->drop_type_id = type_id;
    action->drop_symbol_id = symbol_id;
    action->drop_span = span;
    action->next = builder->cleanup_scope->actions;
    builder->cleanup_scope->actions = action;
}

static int expression_moves_ownership(const IrBuilder *builder,
                                      const AstExpression *expression) {
    if (expression == NULL || expression->kind != AST_EXPR_NAME ||
        expression->resolved_borrow_kind != AST_BORROW_NONE ||
        expression->resolved_pointer_depth != 0 ||
        expression->resolved_outer_pointer_depth != 0 ||
        expression->resolved_is_slice ||
        expression->resolved_named_symbol_id >= builder->module->semantics->symbol_count)
        return 0;
    return (semantic_symbol_type_properties(
                builder->module->semantics,
                expression->resolved_named_symbol_id) &
            SEMANTIC_TYPE_MOVE_ONLY) != 0;
}

static void emit_move_if_owned(IrBuilder *builder,
                               const AstExpression *expression) {
    if (!expression_moves_ownership(builder, expression)) return;
    IrInstruction *instruction = emit(builder, IR_OP_MOVE, expression->span);
    if (instruction == NULL) return;
    instruction->type = TYPE_VOID;
    instruction->type_id = type_from_expression(builder->module,
                                                builder->program,
                                                expression);
    instruction->symbol_id = expression->resolved_symbol_id;
}

static void emit_deferred_until(IrBuilder *builder,
                                const CleanupScope *stop) {
    for (const CleanupScope *scope = builder->cleanup_scope;
         scope != NULL && scope != stop && !builder->failed;
         scope = scope->previous)
        emit_deferred_scope(builder, scope);
}

static void free_deferred_actions(DeferredAction *action) {
    while (action != NULL) {
        DeferredAction *next = action->next;
        free(action->arguments);
        free(action);
        action = next;
    }
}

static void lower_scoped_statement(IrBuilder *builder,
                                   const AstStatement *statement) {
    CleanupScope scope = {.previous = builder->cleanup_scope};
    builder->cleanup_scope = &scope;
    lower_statement(builder, statement);
    if (!block_terminated(builder->function))
        emit_deferred_scope(builder, &scope);
    builder->cleanup_scope = scope.previous;
    free_deferred_actions(scope.actions);
}

static void register_deferred_action(IrBuilder *builder,
                                     const AstStatement *statement) {
    if (builder->cleanup_scope == NULL) {
        builder->failed = 1;
        return;
    }
    DeferredAction *action = calloc(1, sizeof(*action));
    if (action == NULL) {
        builder->failed = 1;
        return;
    }
    if (statement->expression != NULL) {
        size_t before = builder->function->instruction_count;
        (void) lower_expression(builder, statement->expression);
        if (builder->failed ||
            builder->function->instruction_count <= before ||
            builder->function->instructions[
                builder->function->instruction_count - 1].opcode != IR_OP_CALL) {
            free(action);
            builder->failed = 1;
            return;
        }
        action->captured_call = 1;
        action->call = builder->function->instructions[
            builder->function->instruction_count - 1];
        action->argument_count = action->call.argument_count;
        if (action->argument_count != 0) {
            action->arguments =
                calloc(action->argument_count, sizeof(*action->arguments));
            if (action->arguments == NULL) {
                free(action);
                builder->failed = 1;
                return;
            }
            for (size_t i = 0; i < action->argument_count; i++)
                action->arguments[i] = builder->function->arguments[
                    action->call.first_argument + i];
            builder->function->argument_count =
                action->call.first_argument;
        }
        builder->function->instruction_count--;
    } else {
        action->body = statement->body;
    }
    action->next = builder->cleanup_scope->actions;
    builder->cleanup_scope->actions = action;
}

static void emit_label(IrBuilder *builder, size_t label, AstSourceSpan span) {
    IrInstruction *instruction = emit(builder, IR_OP_LABEL, span);
    if (instruction != NULL) instruction->target_a = label;
    set_void_type(builder, instruction);
}

static void lower_statement(IrBuilder *builder, const AstStatement *statement) {
    for (; statement != NULL && !builder->failed; statement = statement->next) {
        if (statement->kind == AST_STMT_MATCH) {
            if (statement->is_type_match) {
                if (statement->selected_type_arm)
                    lower_scoped_statement(builder,
                                           statement->selected_type_arm->body);
                continue;
            }
            size_t value = lower_expression(builder, statement->value);
            size_t join = new_label(builder);
            int wildcard = 0;
            for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                size_t body_label = IR_VALUE_NONE, next_label = IR_VALUE_NONE;
                if (!arm->wildcard) {
                    body_label = new_label(builder);
                    next_label = new_label(builder);
                    IrInstruction *test = emit(builder, IR_OP_ENUM_IS, arm->span);
                    if (!test) return;
                    test->operand_a = value;
                    test->symbol_id = arm->resolved_variant_symbol;
                    test->result = new_value(builder);
                    test->type = TYPE_BIT;
                    test->type_id = type_from_parts(builder->module, TYPE_BIT, 0, AST_TOKEN_NONE, 0, 0, 0, 0,
                                                    builder->program, AST_SYMBOL_NONE);
                    size_t condition = test->result;
                    IrInstruction *branch = emit(builder, IR_OP_BRANCH, arm->span);
                    if (!branch) return;
                    set_void_type(builder, branch);
                    branch->operand_a = condition;
                    branch->target_a = body_label;
                    branch->target_b = next_label;
                    emit_label(builder, body_label, arm->span);
                } else wildcard = 1;
                size_t payload_index = 0;
                for (const AstParameter *binding = arm->bindings; binding; binding = binding->next, payload_index++) {
                    IrInstruction *payload = emit(builder, IR_OP_ENUM_PAYLOAD, arm->span);
                    if (!payload) return;
                    payload->operand_a = value;
                    payload->symbol_id = arm->resolved_variant_symbol;
                    payload->enum_payload_index = payload_index;
                    payload->target_a = body_label;
                    payload->result = new_value(builder);
                    payload->type_id = type_from_ast(builder->module, builder->program, &binding->type);
                    payload->type = ir_ast_type_data_type(builder->program, &binding->type);
                    payload->pointer_depth = binding->type.pointer_depth +
                                             binding->type.outer_pointer_depth +
                                             (binding->type.borrow_kind != AST_BORROW_NONE);
                    payload->type_name_token = binding->type.name_token;
                    payload->is_array = binding->type.is_array;
                    size_t initial = payload->result;
                    IrInstruction *local = emit(builder, IR_OP_DECLARE, arm->span);
                    if (!local) return;
                    local->operand_a = initial;
                    local->symbol_id = binding->resolved_symbol_id;
                    local->type_id = type_from_ast(builder->module, builder->program, &binding->type);
                    local->type = ir_ast_type_data_type(builder->program, &binding->type);
                    local->pointer_depth = binding->type.pointer_depth +
                                           binding->type.outer_pointer_depth +
                                           (binding->type.borrow_kind != AST_BORROW_NONE);
                    local->type_name_token = binding->type.name_token;
                    local->is_array = binding->type.is_array;
                }
                lower_scoped_statement(builder, arm->body);
                IrInstruction *jump = emit(builder, IR_OP_JUMP, arm->span);
                if (jump) {
                    set_void_type(builder, jump);
                    jump->target_a = join;
                }
                if (!arm->wildcard) emit_label(builder, next_label, arm->span);
            }
            if (!wildcard) {
                IrInstruction *trap = emit(builder, IR_OP_TRAP, statement->span);
                set_void_type(builder, trap);
            }
            emit_label(builder, join, statement->span);
            continue;
        }

        if (block_terminated(builder->function)) break;
        if (statement->kind == AST_STMT_BLOCK) {
            lower_scoped_statement(builder, statement->body);
        } else if (statement->kind == AST_STMT_DEFER) {
            register_deferred_action(builder, statement);
        } else if (statement->kind == AST_STMT_VARIABLE) {
            if (statement->is_const) continue;
            const AstType *runtime_type = statement->type.kind == AST_TYPE_INFERRED && statement->value &&
                                          statement->value->has_resolved_ast_type
                                              ? &statement->value->resolved_ast_type : &statement->type;
            if (runtime_type->kind == AST_TYPE_FUNCTION &&
                runtime_type->function_generic_parameters != NULL)
                continue;
            size_t value = lower_expression(builder, statement->value);
            if (statement->type.kind != AST_TYPE_INFERRED)
                value = coerce_slice(builder, value, type_from_ast(builder->module, builder->program, &statement->type),
                                     statement->span);
            IrInstruction *instruction = emit(builder, IR_OP_DECLARE, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = value;
                instruction->auxiliary_token = statement->name_token;
                instruction->symbol_id = statement->resolved_symbol_id;
                instruction->type = ir_ast_type_data_type(builder->program, &statement->type);
                instruction->type_id = statement->type.kind == AST_TYPE_INFERRED &&
                                       statement->value != NULL
                                           ? type_from_expression(builder->module, builder->program, statement->value)
                                           : type_from_ast(builder->module, builder->program, &statement->type);
                if (instruction->type_id == IR_TYPE_NONE) builder->failed = 1;
                instruction->pointer_depth = statement->type.pointer_depth +
                                             statement->type.outer_pointer_depth +
                                             (statement->type.borrow_kind != AST_BORROW_NONE);
                instruction->type_name_token = statement->type.name_token;
                instruction->is_array = statement->type.is_array && statement->type.outer_pointer_depth == 0;
                instruction->is_slice = statement->type.is_slice && !statement->type.outer_pointer_depth;
                if (statement->type.kind == AST_TYPE_INFERRED && statement->value != NULL) {
                    instruction->type = statement->value->resolved_type;
                    instruction->pointer_depth =
                            statement->value->resolved_pointer_depth + statement->value->resolved_outer_pointer_depth;
                    instruction->type_name_token = statement->value->resolved_named_type_token;
                    instruction->is_array = statement->value->resolved_is_array && statement->value->
                                            resolved_outer_pointer_depth == 0;
                    instruction->is_slice = statement->value->resolved_is_slice && !statement->value->
                                            resolved_outer_pointer_depth;
                }
                emit_move_if_owned(builder, statement->value);
                instruction->owns_slice_backing = statement->value != NULL &&
                                                   statement->value->owns_slice_backing;
                register_local_drop(builder, instruction->symbol_id,
                                    instruction->type_id, statement->span);
                if (instruction->is_slice)
                    register_slice_backing_cleanup(builder,
                                                   instruction->symbol_id,
                                                   instruction->type_id,
                                                   statement->span);
            }
        } else if (statement->kind == AST_STMT_ASSIGNMENT) {
            size_t target = lower_expression(builder, statement->expression);
            size_t value = lower_expression(builder, statement->value);
            value = coerce_slice(builder, value,
                                 type_from_expression(builder->module, builder->program, statement->expression),
                                 statement->span);
            IrTypeId target_type = type_from_expression(builder->module,
                                                        builder->program,
                                                        statement->expression);
            if (statement->assignment_operator == TOKEN_EQUAL &&
                statement->expression != NULL &&
                statement->expression->kind == AST_EXPR_NAME &&
                statement->expression->resolved_is_slice &&
                statement->expression->resolved_symbol_id <
                    builder->module->semantics->symbol_count &&
                (builder->module->semantics->symbols[
                     statement->expression->resolved_symbol_id].kind ==
                     SEMANTIC_SYMBOL_LOCAL ||
                 builder->module->semantics->symbols[
                     statement->expression->resolved_symbol_id].kind ==
                     SEMANTIC_SYMBOL_VARIABLE)) {
                IrInstruction *release = emit(builder,
                                              IR_OP_FREE_SLICE_BACKING,
                                              statement->span);
                if (release != NULL) {
                    release->type = TYPE_VOID;
                    release->type_id = target_type;
                    release->symbol_id =
                        statement->expression->resolved_symbol_id;
                }
            }
            if (statement->assignment_operator == TOKEN_EQUAL &&
                statement->expression != NULL &&
                statement->expression->kind == AST_EXPR_NAME &&
                type_needs_drop(builder, target_type)) {
                IrInstruction *drop = emit(builder, IR_OP_DROP, statement->span);
                if (drop != NULL) {
                    drop->type = TYPE_VOID;
                    drop->type_id = target_type;
                    drop->symbol_id = statement->expression->resolved_symbol_id;
                }
            }
            IrInstruction *instruction = emit(builder, IR_OP_STORE, statement->span);
            if (instruction != NULL) {
                instruction->operand_a = target;
                instruction->operand_b = value;
                instruction->operator_type = statement->assignment_operator;
                instruction->owns_slice_backing =
                    statement->value != NULL &&
                    statement->value->owns_slice_backing;
            }
            set_expression_type(builder, instruction, statement->expression);
            emit_move_if_owned(builder, statement->value);
            if (statement->assignment_operator == TOKEN_EQUAL &&
                statement->expression != NULL &&
                statement->expression->kind == AST_EXPR_NAME &&
                type_needs_drop(builder, target_type)) {
                IrInstruction *reinit = emit(builder, IR_OP_REINIT, statement->span);
                if (reinit != NULL) {
                    reinit->type = TYPE_VOID;
                    reinit->type_id = target_type;
                    reinit->symbol_id = statement->expression->resolved_symbol_id;
                }
            }
        } else if (statement->kind == AST_STMT_EXPRESSION) {
            size_t value = lower_expression(builder, statement->expression);
            if (statement->expression != NULL &&
                statement->expression->owns_slice_backing) {
                IrInstruction *cleanup = emit(builder,
                                              IR_OP_FREE_SLICE_BACKING,
                                              statement->span);
                if (cleanup != NULL) {
                    cleanup->type = TYPE_VOID;
                    cleanup->type_id = type_from_expression(
                        builder->module, builder->program,
                        statement->expression);
                    cleanup->operand_a = value;
                }
            }
        } else if (statement->kind == AST_STMT_RETURN) {
            size_t value = lower_expression(builder, statement->value);
            if (block_terminated(builder->function)) continue;
            value = coerce_slice(builder, value, builder->function->return_type_id, statement->span);
            emit_move_if_owned(builder, statement->value);
            emit_deferred_until(builder, NULL);
            IrInstruction *instruction = emit(builder, IR_OP_RETURN, statement->span);
            if (instruction != NULL) instruction->operand_a = value;
            if (statement->value != NULL) {
                set_expression_type(builder, instruction, statement->value);
                instruction->type_id = builder->function->return_type_id;
            } else set_void_type(builder, instruction);
        } else if (statement->kind == AST_STMT_BREAK ||
                   statement->kind == AST_STMT_CONTINUE) {
            size_t target = statement->kind == AST_STMT_BREAK
                                ? builder->break_label
                                : builder->continue_label;
            /* An unresolved target is retained for sema/diagnostic recovery;
               valid modules always resolve it to an enclosing loop label. */
            emit_deferred_until(builder,
                                statement->kind == AST_STMT_BREAK
                                    ? builder->break_cleanup_stop
                                    : builder->continue_cleanup_stop);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = target;
            set_void_type(builder, jump);
        } else if (statement->kind == AST_STMT_IF) {
            size_t then_label = new_label(builder);
            size_t else_label = new_label(builder);
            size_t end_label = new_label(builder);
            size_t condition = lower_expression(builder, statement->condition);
            if (block_terminated(builder->function)) continue;
            IrInstruction *branch = emit(builder, IR_OP_BRANCH, statement->span);
            if (branch != NULL) {
                branch->operand_a = condition;
                branch->target_a = then_label;
                branch->target_b = statement->else_body == NULL ? end_label : else_label;
            }
            set_void_type(builder, branch);
            emit_label(builder, then_label, statement->span);
            lower_scoped_statement(builder, statement->body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = end_label;
            set_void_type(builder, jump);
            if (statement->else_body != NULL) {
                emit_label(builder, else_label, statement->span);
                lower_scoped_statement(builder, statement->else_body);
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
            CleanupScope *saved_break_cleanup = builder->break_cleanup_stop;
            CleanupScope *saved_continue_cleanup =
                builder->continue_cleanup_stop;
            CleanupScope *loop_parent_scope = builder->cleanup_scope;
            builder->break_label = end_label;
            builder->continue_label = update_label;
            builder->break_cleanup_stop = loop_parent_scope;
            builder->continue_cleanup_stop = loop_parent_scope;
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
            lower_scoped_statement(builder, statement->body);
            if (statement->kind == AST_STMT_FOR) emit_label(builder, update_label, statement->span);
            lower_statement(builder, statement->else_body);
            IrInstruction *jump = emit(builder, IR_OP_JUMP, statement->span);
            if (jump != NULL) jump->target_a = condition_label;
            set_void_type(builder, jump);
            emit_label(builder, end_label, statement->span);
            builder->break_label = saved_break;
            builder->continue_label = saved_continue;
            builder->break_cleanup_stop = saved_break_cleanup;
            builder->continue_cleanup_stop = saved_continue_cleanup;
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
    *function = (IrFunction)
    {
        .source_program = program,
        .name_token = declaration->name_token,
        .owner_token = declaration->as.function.owner_token,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .symbol_id = AST_SYMBOL_NONE,
        .interface_thunk_symbol_id = AST_SYMBOL_NONE,
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
        if (parameter->compile_time_value == NULL)
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
         parameter != NULL; parameter = parameter->next) {
        if (parameter->compile_time_value != NULL) continue;
        IrParameter *ir_parameter = &function->parameters[parameter_index];
        *ir_parameter = (IrParameter)
        {
            .source_program = program,
            .name_token = parameter->name_token,
            .symbol_id = parameter->resolved_symbol_id,
            .type = ir_ast_type_data_type(program, &parameter->type),
            .type_id = type_from_ast(module, program, &parameter->type),
            .pointer_depth = parameter->type.pointer_depth +
                             parameter->type.outer_pointer_depth +
                             (parameter->type.borrow_kind != AST_BORROW_NONE),
            .type_name_token = parameter->type.name_token,
            .is_array = parameter->type.is_array && parameter->type.outer_pointer_depth == 0,
            .is_slice = parameter->type.is_slice && !parameter->type.outer_pointer_depth
        };
        if (ir_parameter->type_id == IR_TYPE_NONE) return 0;
        parameter_index++;
    }
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    CleanupScope parameter_scope = {0};
    builder.cleanup_scope = &parameter_scope;
    for (size_t i = 0; i < function->parameter_count; i++) {
        const IrParameter *parameter = &function->parameters[i];
        if (!parameter->is_receiver)
            register_local_drop(&builder, parameter->symbol_id,
                                parameter->type_id,
                                declaration->span);
    }
    lower_statement(&builder, declaration->as.function.body);
    if (!block_terminated(function))
        emit_deferred_scope(&builder, &parameter_scope);
    free_deferred_actions(parameter_scope.actions);
    builder.cleanup_scope = NULL;
    if (builder.failed)
        ir_report_failure(function,
                          function->instruction_count == 0 ? IR_VALUE_NONE : function->instruction_count - 1,
                          "IR lowering", "could not lower the typed source tree");
    return !builder.failed;
}

static int append_interface_thunk(IrModule *module, const AstProgram *program,
                                  const AstDeclarationNode *method) {
    size_t before = module->function_count;
    if (!append_function(module, program, method) ||
        module->function_count != before + 1)
        return 0;
    IrFunction *function = &module->functions[before];
    size_t method_symbol = function->symbol_id;
    if (method_symbol == AST_SYMBOL_NONE) return 0;
    function->interface_thunk_symbol_id = method_symbol;
    for (size_t i = 1; i < function->parameter_count; i++)
        if (function->parameters[i].symbol_id == AST_SYMBOL_NONE)
            function->parameters[i].symbol_id = module->semantics->symbol_count + i;

    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    size_t first_argument = function->argument_count;
    for (size_t i = 0; i < function->parameter_count; i++) {
        const IrParameter *parameter = &function->parameters[i];
        IrInstruction *load = emit(&builder, IR_OP_LOAD, method->span);
        if (load == NULL) return 0;
        load->result = new_value(&builder);
        load->symbol_id = parameter->symbol_id;
        load->auxiliary_token = parameter->name_token;
        load->type = parameter->type;
        load->type_id = parameter->type_id;
        load->pointer_depth = parameter->pointer_depth;
        load->type_name_token = parameter->type_name_token;
        load->is_array = parameter->is_array;
        load->is_slice = parameter->is_slice;
        if (!append_argument(&builder, load->result)) return 0;
    }
    IrInstruction *call = emit(&builder, IR_OP_CALL, method->span);
    if (call == NULL) return 0;
    call->symbol_id = method_symbol;
    call->first_argument = first_argument;
    call->argument_count = function->parameter_count;
    call->type = ir_ast_type_data_type(program, &method->as.function.return_type);
    call->type_id = function->return_type_id;
    call->pointer_depth = method->as.function.return_type.pointer_depth +
                          method->as.function.return_type.outer_pointer_depth +
                          (method->as.function.return_type.borrow_kind != AST_BORROW_NONE);
    call->type_name_token = method->as.function.return_type.name_token;
    call->is_array = method->as.function.return_type.is_array;
    call->is_slice = method->as.function.return_type.is_slice;
    if (call->type != TYPE_VOID || call->pointer_depth != 0)
        call->result = new_value(&builder);

    IrInstruction *return_instruction = emit(&builder, IR_OP_RETURN, method->span);
    if (return_instruction == NULL) return 0;
    return_instruction->operand_a = call->result;
    return_instruction->type = call->type;
    return_instruction->type_id = call->type_id;
    return_instruction->pointer_depth = call->pointer_depth;
    return_instruction->type_name_token = call->type_name_token;
    return_instruction->is_array = call->is_array;
    return_instruction->is_slice = call->is_slice;
    return !builder.failed;
}

static int append_referenced_interface_thunks(IrModule *module) {
    size_t symbol_count = module->semantics->symbol_count;
    unsigned char *referenced = calloc(symbol_count, sizeof(*referenced));
    if (referenced == NULL && symbol_count != 0) return 0;
    size_t source_function_count = module->function_count;
    for (size_t f = 0; f < source_function_count; f++)
        for (size_t i = 0; i < module->functions[f].instruction_count; i++) {
            const IrInstruction *instruction = &module->functions[f].instructions[i];
            if (instruction->opcode != IR_OP_FUNCTION_ADDRESS ||
                instruction->symbol_id >= symbol_count) continue;
            const SemanticSymbol *method = &module->semantics->symbols[instruction->symbol_id];
            if (method->kind == SEMANTIC_SYMBOL_FUNCTION &&
                method->owner_symbol_id < symbol_count &&
                module->semantics->symbols[method->owner_symbol_id].kind ==
                    SEMANTIC_SYMBOL_INTERFACE)
                referenced[instruction->symbol_id] = 1;
        }
    for (size_t g = 0; g < module->global_count; g++) {
        size_t id = module->globals[g].function_symbol_id;
        if (id >= symbol_count) continue;
        const SemanticSymbol *method = &module->semantics->symbols[id];
        if (method->kind == SEMANTIC_SYMBOL_FUNCTION &&
            method->owner_symbol_id < symbol_count &&
            module->semantics->symbols[method->owner_symbol_id].kind ==
                SEMANTIC_SYMBOL_INTERFACE)
            referenced[id] = 1;
    }
    for (size_t id = 0; id < symbol_count; id++) {
        if (!referenced[id]) continue;
        const SemanticSymbol *method = &module->semantics->symbols[id];
        if (method->declaration == NULL ||
            !append_interface_thunk(module, method->source_program,
                                    method->declaration)) {
            free(referenced);
            return 0;
        }
    }
    free(referenced);
    return 1;
}

static int copy_fields(IrModule *module, const AstProgram *program, const AstField *fields,
                       IrFieldDefinition **output, size_t *count) {
    for (const AstField *field = fields; field != NULL; field = field->next) (*count)++;
    if (*count == 0) return 1;
    *output = calloc(*count, sizeof(**output));
    if (*output == NULL) return 0;
    size_t index = 0;
    for (const AstField *field = fields; field != NULL; field = field->next, index++) {
        (*output)[index] = (IrFieldDefinition)
        {
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
    *structure = (IrAggregate)
    {
        .source_program = program,
        .name_token = declaration->name_token,
        .symbol_id = declaration->resolved_symbol_id,
        .type_properties = semantic_symbol_type_properties(
            module->semantics, declaration->resolved_symbol_id),
        .has_explicit_destructor =
            declaration->as.struct_decl.destructor != NULL
    };
    return copy_fields(module, program, declaration->as.struct_decl.fields,
                       &structure->fields, &structure->field_count);
}

static int append_drop_glue(IrModule *module, const IrAggregate *structure) {
    if ((structure->type_properties & SEMANTIC_TYPE_NEEDS_DROP) == 0)
        return 1;
    if (structure->symbol_id >= module->semantics->symbol_count) return 0;
    const SemanticSymbol *symbol =
        &module->semantics->symbols[structure->symbol_id];
    const AstDeclarationNode *declaration = symbol->declaration;
    if (declaration == NULL || declaration->kind != AST_DECL_STRUCT) return 0;
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions)))
        return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction) {
        .source_program = structure->source_program,
        .name_token = structure->name_token,
        .owner_token = structure->name_token,
        .owner_symbol_id = structure->symbol_id,
        .symbol_id = structure->symbol_id,
        .interface_thunk_symbol_id = AST_SYMBOL_NONE,
        .return_type_id = type_from_parts(module, TYPE_VOID, 0,
                                          AST_TOKEN_NONE, 0, 0, 0, 0,
                                          structure->source_program,
                                          AST_SYMBOL_NONE),
        .is_drop_glue = 1
    };
    if (function->return_type_id == IR_TYPE_NONE) return 0;
    function->parameters = calloc(1, sizeof(*function->parameters));
    if (function->parameters == NULL) return 0;
    function->parameter_count = 1;
    function->parameters[0] = (IrParameter) {
        .source_program = structure->source_program,
        .name_token = structure->name_token,
        .symbol_id = structure->symbol_id,
        .type = TYPE_UNKNOWN,
        .type_id = type_from_parts(module, TYPE_UNKNOWN, 1,
                                   structure->name_token, 0, 0, 0, 0,
                                   structure->source_program,
                                   structure->symbol_id),
        .pointer_depth = 1,
        .type_name_token = structure->name_token,
        .is_receiver = 1
    };
    if (function->parameters[0].type_id == IR_TYPE_NONE) return 0;
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = structure->source_program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    if (declaration->as.struct_decl.destructor != NULL)
        lower_scoped_statement(&builder,
                               declaration->as.struct_decl.destructor);
    for (size_t i = structure->field_count; i > 0 && !builder.failed; i--) {
        const IrFieldDefinition *field = &structure->fields[i - 1];
        if ((ir_type_properties(module, field->type_id) &
             SEMANTIC_TYPE_NEEDS_DROP) == 0)
            continue;
        IrInstruction *drop = emit(&builder, IR_OP_DROP, declaration->span);
        if (drop != NULL) {
            drop->type = TYPE_VOID;
            drop->type_id = field->type_id;
            drop->symbol_id = field->symbol_id;
        }
    }
    return !builder.failed;
}

static int append_enum_drop_glue(IrModule *module,
                                 const IrEnum *enumeration) {
    if (enumeration->symbol_id >= module->semantics->symbol_count ||
        (semantic_symbol_type_properties(module->semantics,
                                         enumeration->symbol_id) &
         SEMANTIC_TYPE_NEEDS_DROP) == 0)
        return 1;
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions)))
        return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction) {
        .source_program = enumeration->source_program,
        .name_token = enumeration->name_token,
        .owner_token = enumeration->name_token,
        .owner_symbol_id = enumeration->symbol_id,
        .symbol_id = enumeration->symbol_id,
        .interface_thunk_symbol_id = AST_SYMBOL_NONE,
        .return_type_id = type_from_parts(module, TYPE_VOID, 0,
                                          AST_TOKEN_NONE, 0, 0, 0, 0,
                                          enumeration->source_program,
                                          AST_SYMBOL_NONE),
        .is_drop_glue = 1
    };
    if (function->return_type_id == IR_TYPE_NONE) return 0;
    function->parameters = calloc(1, sizeof(*function->parameters));
    if (function->parameters == NULL) return 0;
    function->parameter_count = 1;
    function->parameters[0] = (IrParameter) {
        .source_program = enumeration->source_program,
        .name_token = enumeration->name_token,
        .symbol_id = enumeration->symbol_id,
        .type = TYPE_UNKNOWN,
        .type_id = type_from_parts(module, TYPE_UNKNOWN, 1,
                                   enumeration->name_token, 0, 0, 0, 0,
                                   enumeration->source_program,
                                   enumeration->symbol_id),
        .pointer_depth = 1,
        .type_name_token = enumeration->name_token,
        .is_receiver = 1
    };
    if (function->parameters[0].type_id == IR_TYPE_NONE) return 0;
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = enumeration->source_program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    IrInstruction *receiver = emit(&builder, IR_OP_LOAD, (AstSourceSpan) {0});
    if (receiver == NULL) return 0;
    receiver->result = new_value(&builder);
    receiver->symbol_id = enumeration->symbol_id;
    receiver->auxiliary_token = enumeration->name_token;
    receiver->type = TYPE_UNKNOWN;
    receiver->type_id = type_from_parts(module, TYPE_UNKNOWN, 0,
                                        enumeration->name_token, 0, 0, 0, 0,
                                        enumeration->source_program,
                                        enumeration->symbol_id);
    if (receiver->type_id == IR_TYPE_NONE) return 0;
    receiver->type_name_token = enumeration->name_token;
    size_t end_label = new_label(&builder);
    for (size_t v = 0; v < enumeration->variant_count; v++) {
        const IrEnumVariant *variant = &enumeration->variants[v];
        int needs_drop = 0;
        for (size_t p = 0; p < variant->payload_count; p++)
            if ((ir_type_properties(module, variant->payload_types[p]) &
                 SEMANTIC_TYPE_NEEDS_DROP) != 0)
                needs_drop = 1;
        if (!needs_drop) continue;
        size_t body_label = new_label(&builder);
        size_t next_label = new_label(&builder);
        IrInstruction *test = emit(&builder, IR_OP_ENUM_IS,
                                   (AstSourceSpan) {0});
        if (test == NULL) return 0;
        test->operand_a = receiver->result;
        test->symbol_id = variant->symbol_id;
        test->result = new_value(&builder);
        test->type = TYPE_BIT;
        test->type_id = type_from_parts(module, TYPE_BIT, 0,
                                        AST_TOKEN_NONE, 0, 0, 0, 0,
                                        enumeration->source_program,
                                        AST_SYMBOL_NONE);
        IrInstruction *branch = emit(&builder, IR_OP_BRANCH,
                                     (AstSourceSpan) {0});
        if (branch == NULL) return 0;
        set_void_type(&builder, branch);
        branch->operand_a = test->result;
        branch->target_a = body_label;
        branch->target_b = next_label;
        emit_label(&builder, body_label, (AstSourceSpan) {0});
        for (size_t p = variant->payload_count; p > 0; p--) {
            IrTypeId payload_type = variant->payload_types[p - 1];
            if ((ir_type_properties(module, payload_type) &
                 SEMANTIC_TYPE_NEEDS_DROP) == 0)
                continue;
            IrInstruction *payload = emit(&builder, IR_OP_ENUM_PAYLOAD,
                                          (AstSourceSpan) {0});
            if (payload == NULL) return 0;
            payload->operand_a = receiver->result;
            payload->symbol_id = variant->symbol_id;
            payload->enum_payload_index = p - 1;
            payload->target_a = body_label;
            payload->result = new_value(&builder);
            payload->type_id = payload_type;
            payload->type = module->types[payload_type].kind == IR_TYPE_PRIMITIVE
                                ? module->types[payload_type].primitive
                                : TYPE_UNKNOWN;
            payload->is_array = module->types[payload_type].kind == IR_TYPE_ARRAY;
            IrInstruction *drop = emit(&builder, IR_OP_DROP,
                                       (AstSourceSpan) {0});
            if (drop == NULL) return 0;
            drop->type = TYPE_VOID;
            drop->type_id = payload_type;
            drop->operand_a = payload->result;
        }
        IrInstruction *jump = emit(&builder, IR_OP_JUMP,
                                   (AstSourceSpan) {0});
        if (jump == NULL) return 0;
        set_void_type(&builder, jump);
        jump->target_a = end_label;
        emit_label(&builder, next_label, (AstSourceSpan) {0});
    }
    emit_label(&builder, end_label, (AstSourceSpan) {0});
    return !builder.failed;
}

static void set_global_instruction_type(const IrModule *module,
                                        const IrGlobal *global,
                                        IrInstruction *instruction) {
    const SemanticSymbol *symbol =
        &module->semantics->symbols[global->symbol_id];
    const IrType *type = &module->types[global->type_id];
    instruction->type_id = global->type_id;
    instruction->type = type->kind == IR_TYPE_PRIMITIVE
                            ? type->primitive
                            : TYPE_UNKNOWN;
    instruction->pointer_depth = symbol->resolved_pointer_depth +
                                 symbol->resolved_outer_pointer_depth;
    instruction->type_name_token = symbol->resolved_named_type_token;
    instruction->is_array = type->kind == IR_TYPE_ARRAY;
    instruction->is_slice = type->kind == IR_TYPE_SLICE;
}

static int append_package_init(IrModule *module) {
    if (module->program->package_name != NULL &&
        strcmp(module->program->package_name, "main") != 0)
        return 1;
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions)))
        return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction) {
        .source_program = module->program,
        .name_token = module->program->package_token != AST_TOKEN_NONE
                          ? module->program->package_token
                          : 0,
        .owner_token = AST_TOKEN_NONE,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .symbol_id = AST_SYMBOL_NONE,
        .interface_thunk_symbol_id = AST_SYMBOL_NONE,
        .return_type_id = type_from_parts(module, TYPE_VOID, 0,
                                          AST_TOKEN_NONE, 0, 0, 0, 0,
                                          module->program,
                                          AST_SYMBOL_NONE),
        .is_package_init = 1
    };
    if (function->return_type_id == IR_TYPE_NONE) return 0;
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = module->program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    for (size_t i = 0; i < module->global_count && !builder.failed; i++) {
        IrGlobal *global = &module->globals[i];
        const AstExpression *initializer = global->runtime_initializer;
        if (initializer == NULL) continue;
        builder.program = global->source_program;
        size_t value = lower_expression(&builder, initializer);
        value = coerce_slice(&builder, value, global->type_id,
                             initializer->span);

        IrInstruction *target = emit(&builder, IR_OP_LOAD,
                                     initializer->span);
        if (target == NULL) break;
        target->result = new_value(&builder);
        target->auxiliary_token = function->name_token;
        target->symbol_id = global->symbol_id;
        set_global_instruction_type(module, global, target);

        IrInstruction *store = emit(&builder, IR_OP_STORE,
                                    initializer->span);
        if (store == NULL) break;
        store->operand_a = target->result;
        store->operand_b = value;
        store->operator_type = TOKEN_EQUAL;
        store->owns_slice_backing = global->owns_slice_backing;
        set_global_instruction_type(module, global, store);
        emit_move_if_owned(&builder, initializer);

        if (type_needs_drop(&builder, global->type_id)) {
            IrInstruction *reinit = emit(&builder, IR_OP_REINIT,
                                         initializer->span);
            if (reinit == NULL) break;
            reinit->type = TYPE_VOID;
            reinit->type_id = global->type_id;
            reinit->symbol_id = global->symbol_id;
        }
    }
    return !builder.failed;
}

static int append_package_cleanup(IrModule *module) {
    if (module->program->package_name != NULL &&
        strcmp(module->program->package_name, "main") != 0)
        return 1;
    if (module->function_count == module->function_capacity &&
        !grow_array((void **) &module->functions, &module->function_capacity,
                    sizeof(*module->functions)))
        return 0;
    IrFunction *function = &module->functions[module->function_count++];
    *function = (IrFunction) {
        .source_program = module->program,
        .name_token = module->program->package_token != AST_TOKEN_NONE
                          ? module->program->package_token
                          : 0,
        .owner_token = AST_TOKEN_NONE,
        .owner_symbol_id = AST_SYMBOL_NONE,
        .symbol_id = AST_SYMBOL_NONE,
        .interface_thunk_symbol_id = AST_SYMBOL_NONE,
        .return_type_id = type_from_parts(module, TYPE_VOID, 0,
                                          AST_TOKEN_NONE, 0, 0, 0, 0,
                                          module->program,
                                          AST_SYMBOL_NONE),
        .is_package_cleanup = 1
    };
    if (function->return_type_id == IR_TYPE_NONE) return 0;
    IrBuilder builder = {
        .module = module,
        .function = function,
        .program = module->program,
        .break_label = IR_VALUE_NONE,
        .continue_label = IR_VALUE_NONE
    };
    for (size_t i = module->global_count; i > 0; i--) {
        const IrGlobal *global = &module->globals[i - 1];
        if (module->types[global->type_id].kind == IR_TYPE_SLICE) {
            IrInstruction *release = emit(&builder,
                                          IR_OP_FREE_SLICE_BACKING,
                                          module->semantics->symbols[
                                              global->symbol_id].declaration->span);
            if (release != NULL) {
                release->type = TYPE_VOID;
                release->type_id = global->type_id;
                release->symbol_id = global->symbol_id;
            }
        }
        if ((ir_type_properties(module, global->type_id) &
             SEMANTIC_TYPE_NEEDS_DROP) == 0)
            continue;
        IrInstruction *drop = emit(&builder, IR_OP_DROP,
                                   module->semantics->symbols[
                                       global->symbol_id].declaration->span);
        if (drop != NULL) {
            drop->type = TYPE_VOID;
            drop->type_id = global->type_id;
            drop->symbol_id = global->symbol_id;
        }
    }
    return !builder.failed;
}

static int append_enum(IrModule *module, const AstProgram *program,
                       const AstDeclarationNode *declaration) {
    if (module->enum_count == module->enum_capacity &&
        !grow_array((void **) &module->enums, &module->enum_capacity,
                    sizeof(*module->enums)))
        return 0;
    IrEnum *enumeration = &module->enums[module->enum_count++];
    *enumeration = (IrEnum)
    {
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
        *variant = (IrEnumVariant)
        {
            .source_program = program,
            .name_token = value->name_token,
            .symbol_id = value->resolved_symbol_id,
            .first_argument = argument_index
        };
        for (const AstTypeArgument *p = value->payload_types; p; p = p->next) variant->payload_count++;
        if (variant->payload_count) {
            variant->payload_types = calloc(variant->payload_count, sizeof(*variant->payload_types));
            if (!variant->payload_types) return 0;
            size_t index = 0;
            for (const AstTypeArgument *p = value->payload_types; p; p = p->next)
                variant->payload_types[index++] = type_from_ast(module, program, &p->type);
        }
        for (const AstExpression *argument = value->arguments;
             argument != NULL; argument = argument->next) {
            const AstExpression *constant = argument->kind == AST_EXPR_UNARY &&
                                            argument->operator_type == TOKEN_MINUS
                                                ? argument->right
                                                : argument;
            enumeration->variant_arguments[argument_index++] = (IrEnumArgument)
            {
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
        module->imports[module->import_count++] = (IrImport)
        {
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

typedef struct {
    const DmmPackage **packages;
    unsigned char *states;
    const DmmPackage **ordered;
    size_t count;
    size_t ordered_count;
} PackageOrder;

static int package_path_order(const void *left, const void *right) {
    const DmmPackage *const *a = left;
    const DmmPackage *const *b = right;
    return strcmp((*a)->path, (*b)->path);
}

static size_t package_order_index(const PackageOrder *order,
                                  const DmmPackage *package) {
    for (size_t i = 0; i < order->count; i++)
        if (order->packages[i] == package) return i;
    return SIZE_MAX;
}

static int visit_package(PackageOrder *order, const DmmPackage *package) {
    size_t index = package_order_index(order, package);
    if (index == SIZE_MAX) return 0;
    if (order->states[index] == 2) return 1;
    if (order->states[index] == 1) return 0;
    order->states[index] = 1;

    const DmmPackage **dependencies = order->count == 0
                                         ? NULL
                                         : calloc(order->count,
                                                  sizeof(*dependencies));
    if (dependencies == NULL && order->count != 0) return 0;
    size_t dependency_count = 0;
    for (size_t f = 0; f < package->file_count; f++) {
        const AstProgram *file = package->files[f];
        for (const AstDeclarationNode *declaration = file->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->kind != AST_DECL_IMPORT) continue;
            for (const AstImportPath *path = declaration->as.import_decl.paths;
                 path != NULL; path = path->next) {
                const DmmPackage *dependency = path->resolved_program == NULL
                                                   ? NULL
                                                   : path->resolved_program->package;
                if (dependency == NULL || dependency == package) continue;
                size_t existing = 0;
                while (existing < dependency_count &&
                       dependencies[existing] != dependency)
                    existing++;
                if (existing == dependency_count)
                    dependencies[dependency_count++] = dependency;
            }
        }
    }
    if (dependency_count > 1)
        qsort(dependencies, dependency_count, sizeof(*dependencies),
              package_path_order);
    int ok = 1;
    for (size_t i = 0; i < dependency_count && ok; i++)
        ok = visit_package(order, dependencies[i]);
    free(dependencies);
    if (!ok) return 0;
    order->states[index] = 2;
    order->ordered[order->ordered_count++] = package;
    return 1;
}

static int build_package_order(const AstProgram *program,
                               PackageOrder *order) {
    if (program->package == NULL || program->module == NULL) return 0;
    for (const DmmPackage *package = program->module->packages;
         package != NULL; package = package->next)
        order->count++;
    order->packages = order->count == 0
                          ? NULL
                          : calloc(order->count, sizeof(*order->packages));
    order->states = order->count == 0
                        ? NULL
                        : calloc(order->count, sizeof(*order->states));
    order->ordered = order->count == 0
                         ? NULL
                         : calloc(order->count, sizeof(*order->ordered));
    if (order->count != 0 &&
        (order->packages == NULL || order->states == NULL ||
         order->ordered == NULL))
        return 0;
    size_t index = 0;
    for (const DmmPackage *package = program->module->packages;
         package != NULL; package = package->next)
        order->packages[index++] = package;
    return visit_package(order, program->package);
}

static void free_package_order(PackageOrder *order) {
    free(order->packages);
    free(order->states);
    free(order->ordered);
    *order = (PackageOrder) {0};
}

static int static_array_initializer(const AstExpression *literal) {
    if (literal == NULL || literal->kind != AST_EXPR_ARRAY_LITERAL) return 0;
    for (const AstExpression *element = literal->arguments;
         element != NULL; element = element->next) {
        if (element->kind == AST_EXPR_ARRAY_LITERAL) {
            if (!static_array_initializer(element)) return 0;
        } else if (element->resolved_type == TYPE_STRING ||
                   (element->kind != AST_EXPR_LITERAL &&
                    element->folded_constant.lexeme == NULL))
            return 0;
    }
    return literal->right == NULL ||
           literal->right->kind == AST_EXPR_LITERAL ||
           literal->right->folded_constant.lexeme != NULL;
}

static int lower_unit(IrModule *module, const AstProgram *program) {
    if (!program->structured_ast_complete) return 0;
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next) {
        if (declaration->generic_parameters != NULL) continue;
        if (declaration->kind == AST_DECL_VARIABLE) {
            IrGlobal *grown = realloc(module->globals, (module->global_count + 1) * sizeof(*grown));
            if (!grown) return 0;
            module->globals = grown;
            const SemanticSymbol *symbol = &module->semantics->symbols[declaration->resolved_symbol_id];
            IrGlobal global = {.source_program = program, .symbol_id = symbol->id,
                               .function_symbol_id = AST_SYMBOL_NONE};
            global.type_id = declaration->as.constant.type.kind == AST_TYPE_INFERRED
                                 ? type_from_parts(module, symbol->resolved_type, 0, AST_TOKEN_NONE, 0, 0, 0, 0,
                                                   program, AST_SYMBOL_NONE)
                                 : type_from_ast(module, program, &declaration->as.constant.type);
            const AstExpression *value = declaration->as.constant.value;
            if (value) {
                if (value->has_resolved_ast_type && value->resolved_ast_type.kind == AST_TYPE_FUNCTION &&
                    value->resolved_symbol_id < module->semantics->symbol_count &&
                    module->semantics->symbols[value->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION) {
                    global.function_symbol_id = value->resolved_symbol_id;
                } else if (value->kind == AST_EXPR_ARRAY_LITERAL &&
                           static_array_initializer(value)) {
                    global.array_literal = value;
                    global.literal_element_count = value->literal_element_count;
                } else if (value->folded_constant.lexeme != NULL) {
                    const char *text = value->folded_constant.lexeme;
                    if (symbol->resolved_type == TYPE_STRING) global.string = text;
                    else if (symbol->resolved_type == TYPE_DOUBLE) {
                    union {
                        double f;
                        uint64_t u;
                    } bits = {.f = strtod(text, NULL)};
                    global.bits = bits.u;
                    } else if (symbol->resolved_type == TYPE_FLOAT) {
                    union {
                        float f;
                        uint32_t u;
                    } bits = {.f = (float) strtod(text, NULL)};
                    global.bits = bits.u;
                    } else global.bits = (uint64_t) strtoull(text, NULL, 10);
                } else {
                    global.runtime_initializer = value;
                    global.owns_slice_backing = value->owns_slice_backing;
                }
            }
            module->globals[module->global_count++] = global;
        }
        if (declaration->kind == AST_DECL_IMPORT && !append_import(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_STRUCT &&
            !append_structure(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_ENUM && !append_enum(module, program, declaration))
            return 0;
        if (declaration->kind == AST_DECL_FUNCTION && declaration->generic_parameters == NULL) {
            int requires_specialization =
                declaration->as.function.return_type.kind == AST_TYPE_FUNCTION &&
                declaration->as.function.return_type.function_generic_parameters != NULL;
            for (const AstParameter *parameter = declaration->as.function.parameters;
                 parameter != NULL; parameter = parameter->next)
                if (parameter->type.kind == AST_TYPE_FUNCTION &&
                    parameter->type.function_generic_parameters != NULL &&
                    parameter->compile_time_value == NULL)
                    requires_specialization = 1;
            if (!requires_specialization &&
                !append_function(module, program, declaration))
                return 0;
        }
        if (declaration->kind == AST_DECL_STRUCT) {
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                if (!append_function(module, program, method)) return 0;
        }
        if (declaration->kind == AST_DECL_ENUM) {
            for (const AstDeclarationNode *method = declaration->as.enum_decl.methods;
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
    PackageOrder order = {0};
    if (program->package != NULL && program->module != NULL) {
        if (!build_package_order(program, &order)) {
            free_package_order(&order);
            goto failure;
        }
        for (size_t p = 0; p < order.ordered_count; p++)
            for (size_t f = 0; f < order.ordered[p]->file_count; f++)
                if (!lower_unit(module, order.ordered[p]->files[f])) {
                    free_package_order(&order);
                    goto failure;
                }
        free_package_order(&order);
    } else {
        if (!lower_unit(module, program)) goto failure;
        for (size_t i = 0; i < program->owned_import_count; i++)
            if (!lower_unit(module, program->owned_imports[i])) goto failure;
    }
    if (!append_referenced_interface_thunks(module)) goto failure;
    for (size_t i = 0; i < module->structure_count; i++)
        if (!append_drop_glue(module, &module->structures[i])) goto failure;
    for (size_t i = 0; i < module->enum_count; i++)
        if (!append_enum_drop_glue(module, &module->enums[i])) goto failure;
    if (!append_package_init(module)) goto failure;
    if (!append_package_cleanup(module)) goto failure;
    if (!ir_verify_module_report(module)) goto failure;
    module->verified = 1;
    return module;

failure:
    ir_module_free(module);
    return NULL;
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
            for (size_t v = 0; v < module->enums[i].variant_count; v++)
                free(module->enums[i].variants[v].payload_types);
        free(module->enums[i].variants);
        free(module->enums[i].variant_arguments);
    }
    for (size_t i = 0; i < module->signature_count; i++)
        free(module->signatures[i].parameter_types);
    free(module->functions);
    free(module->types);
    free(module->signatures);
    free(module->structures);
    free(module->enums);
    free(module->imports);
    free(module->globals);
    free(module);
}
