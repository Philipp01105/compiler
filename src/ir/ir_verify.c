#include "ir.h"
#include "ir_cfg.h"
#include "ir_verify.h"
#include "core_intrinsics.h"

#include <stdint.h>
#include <stdlib.h>

static int ir_verify_module_internal(const IrModule *module, int report);

static int instruction_produces_value(const IrInstruction *instruction) {
    IrOpcode opcode = instruction->opcode;
    return opcode == IR_OP_CONSTANT || opcode == IR_OP_FUNCTION_ADDRESS || opcode == IR_OP_LOAD ||
           opcode == IR_OP_UNARY || opcode == IR_OP_BINARY ||
           (opcode == IR_OP_CALL && instruction->type != TYPE_VOID) ||
           opcode == IR_OP_INDEX || opcode == IR_OP_SUBSLICE ||
           opcode == IR_OP_MEMBER || opcode == IR_OP_SLICE_LENGTH ||
           opcode == IR_OP_SLICE || opcode == IR_OP_SLICE_DATA ||
           opcode == IR_OP_ARRAY_LITERAL ||
           opcode == IR_OP_CAST || opcode == IR_OP_ALLOC ||
           opcode == IR_OP_PHI || opcode == IR_OP_ENUM_CONSTRUCT || opcode == IR_OP_ENUM_IS || opcode ==
           IR_OP_ENUM_PAYLOAD;
}

static const IrInstruction *verified_producer(const IrFunction *function,
                                              const IrInstruction *const *producers,
                                              size_t value, size_t before) {
    if (value == IR_VALUE_NONE || value >= function->next_value) return NULL;
    const IrInstruction *producer = producers[value];
    return producer && (size_t) (producer - function->instructions) < before ? producer : NULL;
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
    if (from->kind == IR_TYPE_NAMED && to->kind == IR_TYPE_NAMED &&
        semantic_implements_interface(module->semantics, to->symbol_id, from->symbol_id))
        return 1;
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
    if (symbol_id == AST_SYMBOL_NONE) return NULL;
    for (size_t i = 0; i < module->function_count; i++)
        if (module->functions[i].symbol_id == symbol_id) return &module->functions[i];
    return NULL;
}

static int ir_void_type(const IrModule *module, IrTypeId type_id) {
    return type_id < module->type_count &&
           module->types[type_id].kind == IR_TYPE_PRIMITIVE &&
           module->types[type_id].primitive == TYPE_VOID;
}

static const IrEnumVariant *ir_variant(const IrModule *module, size_t symbol, const IrEnum **owner) {
    for (size_t e = 0; e < module->enum_count; e++)
        for (size_t v = 0; v < module->enums[e].variant_count; v++)
            if (module->enums[e].variants[v].symbol_id == symbol) {
                if (owner) *owner = &module->enums[e];
                return &module->enums[e].variants[v];
            }
    return NULL;
}

static int verified_payload_guard(const IrFunction *function, const IrInstruction *const *producers,
                                  const IrInstruction *payload, size_t index) {
    size_t label = index;
    while (label > 0 && function->instructions[label].opcode != IR_OP_LABEL) label--;
    if (label == 0 || function->instructions[label].target_a != payload->target_a) return 0;
    const IrInstruction *branch = &function->instructions[label - 1];
    if (branch->opcode != IR_OP_BRANCH || branch->target_a != payload->target_a) return 0;
    const IrInstruction *test = verified_producer(function, producers, branch->operand_a, label - 1);
    if (!test || test->opcode != IR_OP_ENUM_IS || test->operand_a != payload->operand_a ||
        test->symbol_id != payload->symbol_id)
        return 0;
    /* A single checked entry into the arm makes the proof dominate every extraction. */
    for (size_t i = 0; i < function->instruction_count; i++) {
        const IrInstruction *in = &function->instructions[i];
        if (i == label - 1) continue;
        if ((in->opcode == IR_OP_BRANCH || in->opcode == IR_OP_JUMP) &&
            (in->target_a == payload->target_a || in->target_b == payload->target_a))
            return 0;
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
    if (parameter && (kind == CORE_INT || kind == CORE_SIZE || kind == CORE_OFFSET))
        return
                ir_integral_type(module, id);
    return type->primitive == core_value_type(kind);
}

static int verify_instruction_types(const IrModule *module,
                                    const IrFunction *function,
                                    const IrInstruction *const *producers,
                                    const IrInstruction *instruction,
                                    size_t index) {
    const IrInstruction *a = verified_producer(function, producers, instruction->operand_a, index);
    const IrInstruction *b = verified_producer(function, producers, instruction->operand_b, index);
    switch (instruction->opcode) {
        case IR_OP_ENUM_CONSTRUCT: {
            const IrEnum *owner = NULL;
            const IrEnumVariant *variant = ir_variant(module, instruction->symbol_id, &owner);
            if (!variant || instruction->type_id >= module->type_count ||
                module->types[instruction->type_id].kind != IR_TYPE_NAMED ||
                module->types[instruction->type_id].symbol_id != owner->symbol_id ||
                instruction->argument_count != variant->payload_count ||
                instruction->first_argument > function->argument_count ||
                instruction->argument_count > function->argument_count - instruction->first_argument)
                return 0;
            for (size_t n = 0; n < variant->payload_count; n++) {
                const IrInstruction *value = verified_producer(
                    function, producers, function->arguments[instruction->first_argument + n], index);
                if (!value || !ir_types_assignable(module, value->type_id, variant->payload_types[n], value->opcode))
                    return 0;
            }
            return 1;
        }
        case IR_OP_ENUM_IS:
        case IR_OP_ENUM_PAYLOAD: {
            const IrEnum *owner = NULL;
            const IrEnumVariant *variant = ir_variant(module, instruction->symbol_id, &owner);
            if (!variant || !a || module->types[a->type_id].kind != IR_TYPE_NAMED ||
                module->types[a->type_id].symbol_id != owner->symbol_id)
                return 0;
            if (instruction->opcode == IR_OP_ENUM_IS)
                return instruction->type == TYPE_BIT && ir_integral_type(
                           module, instruction->type_id);
            return instruction->enum_payload_index < variant->payload_count &&
                   instruction->type_id == variant->payload_types[instruction->enum_payload_index] &&
                   verified_payload_guard(function, producers, instruction, index);
        }
        case IR_OP_TRAP: return ir_void_type(module, instruction->type_id);

        case IR_OP_CONSTANT:
            return (instruction->has_immediate
                        ? ir_numeric_type(module, instruction->type_id)
                        : instruction->auxiliary_token < function->source_program->token_count) &&
                   (ir_numeric_type(module, instruction->type_id) ||
                   ir_string_type(module, instruction->type_id));
        case IR_OP_FUNCTION_ADDRESS:
            return instruction->symbol_id < module->semantics->symbol_count &&
                   module->semantics->symbols[instruction->symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION &&
                   instruction->type_id < module->type_count &&
                   module->types[instruction->type_id].kind == IR_TYPE_FUNCTION;
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
            if (a != NULL && a->type_id < module->type_count &&
                module->types[a->type_id].kind == IR_TYPE_FUNCTION) {
                const IrType *callable = &module->types[a->type_id];
                if (callable->signature_id >= module->signature_count) return 0;
                const IrFunctionSignature *signature = &module->signatures[callable->signature_id];
                if (instruction->argument_count != signature->parameter_count ||
                    instruction->type_id != signature->return_type) return 0;
                for (size_t argument = 0; argument < signature->parameter_count; argument++) {
                    const IrInstruction *value = verified_producer(function, producers,
                        function->arguments[instruction->first_argument + argument], index);
                    if (value == NULL || !ir_types_assignable(module, value->type_id,
                            signature->parameter_types[argument], value->opcode)) return 0;
                }
                return 1;
            }
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
                    a->symbol_id != AST_SYMBOL_NONE)
                    return 0;
                for (size_t argument = 0; argument < core->argument_count; ++argument) {
                    const IrInstruction *value = verified_producer(function, producers,
                                                                   function->arguments[
                                                                       instruction->first_argument + argument], index);
                    if (value == NULL || !core_ir_type_matches(module, value->type_id,
                                                               core->arguments[argument], 1))
                        return 0;
                }
                return a != NULL;
            }
            const IrFunction *callee = ir_called_function(module, instruction->symbol_id);
            if (callee == NULL && instruction->symbol_id < module->semantics->symbol_count) {
                const SemanticSymbol *method = &module->semantics->symbols[instruction->symbol_id];
                if (method->kind == SEMANTIC_SYMBOL_FUNCTION &&
                    method->owner_symbol_id < module->semantics->symbol_count &&
                    module->semantics->symbols[method->owner_symbol_id].kind ==
                    SEMANTIC_SYMBOL_INTERFACE) {
                    size_t count = 1;
                    for (const AstParameter *p = method->declaration->as.function.parameters;
                         p; p = p->next) count++;
                    if (instruction->argument_count != count ||
                        instruction->first_argument >= function->argument_count)
                        return 0;
                    const IrInstruction *receiver = verified_producer(function, producers,
                        function->arguments[instruction->first_argument], index);
                    if (receiver == NULL || receiver->type_id >= module->type_count)
                        return 0;
                    const IrType *receiver_type = &module->types[receiver->type_id];
                    if (receiver_type->kind == IR_TYPE_POINTER) {
                        if (receiver_type->element_type >= module->type_count)
                            return 0;
                        receiver_type = &module->types[receiver_type->element_type];
                    }
                    return receiver_type->kind == IR_TYPE_NAMED &&
                           receiver_type->symbol_id == method->owner_symbol_id;
                }
            }
            if (callee == NULL || instruction->argument_count != callee->parameter_count ||
                instruction->type_id != callee->return_type_id)
                return 0;
            for (size_t argument = 0; argument < instruction->argument_count; argument++) {
                const IrInstruction *value = verified_producer(function, producers,
                                                               function->arguments[
                                                                   instruction->first_argument + argument], index);
                if (value == NULL) return 0;
                const IrParameter *parameter = &callee->parameters[argument];
                if (parameter->is_receiver) {
                    const IrType *pointer = &module->types[parameter->type_id];
                    if (pointer->kind != IR_TYPE_POINTER ||
                        (pointer->element_type != value->type_id && parameter->type_id != value->type_id))
                        return 0;
                } else if (!ir_types_assignable(module, value->type_id,
                                                parameter->type_id, value->opcode))
                    return 0;
            }
            return 1;
        }
        case IR_OP_DROP:
            if ((ir_type_properties(module, instruction->type_id) &
                 SEMANTIC_TYPE_NEEDS_DROP) == 0)
                return 0;
            if (instruction->operand_a != IR_VALUE_NONE) {
                const IrInstruction *value = verified_producer(
                    function, producers, instruction->operand_a, index);
                return value != NULL && value->type_id == instruction->type_id;
            }
            return instruction->symbol_id < module->semantics->symbol_count;
        case IR_OP_MOVE:
            return instruction->symbol_id < module->semantics->symbol_count &&
                   (ir_type_properties(module, instruction->type_id) &
                    SEMANTIC_TYPE_MOVE_ONLY) != 0;
        case IR_OP_REINIT:
            return instruction->symbol_id < module->semantics->symbol_count &&
                   (ir_type_properties(module, instruction->type_id) &
                    SEMANTIC_TYPE_NEEDS_DROP) != 0;
        case IR_OP_FREE_SLICE_BACKING:
            if (instruction->type_id >= module->type_count ||
                module->types[instruction->type_id].kind != IR_TYPE_SLICE)
                return 0;
            if (a != NULL) return a->type_id == instruction->type_id;
            return instruction->symbol_id < module->semantics->symbol_count &&
                   module->semantics->symbols[instruction->symbol_id].kind ==
                       SEMANTIC_SYMBOL_LOCAL;
        case IR_OP_INDEX:
            return a != NULL && b != NULL && ir_pointer_type(module, a->type_id) &&
                   ir_integral_type(module, b->type_id) &&
                   module->types[a->type_id].element_type == instruction->type_id;
        case IR_OP_SUBSLICE: {
            if (a == NULL || a->type_id >= module->type_count ||
                instruction->type_id >= module->type_count ||
                (module->types[a->type_id].kind != IR_TYPE_ARRAY &&
                 module->types[a->type_id].kind != IR_TYPE_SLICE) ||
                module->types[instruction->type_id].kind != IR_TYPE_SLICE ||
                module->types[a->type_id].element_type !=
                    module->types[instruction->type_id].element_type ||
                (b != NULL && !ir_integral_type(module, b->type_id)) ||
                instruction->argument_count > 1)
                return 0;
            if (instruction->argument_count == 0) return 1;
            if (instruction->first_argument >= function->argument_count)
                return 0;
            const IrInstruction *end = verified_producer(
                function, producers,
                function->arguments[instruction->first_argument], index);
            return end != NULL && ir_integral_type(module, end->type_id);
        }
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
            return a != NULL && ((ir_numeric_type(module, a->type_id) && ir_numeric_type(module, instruction->type_id))
                                 ||
                                 (a->type_id < module->type_count && instruction->type_id < module->type_count &&
                                  module->types[a->type_id].kind == IR_TYPE_POINTER && module->types[instruction->
                                      type_id].kind == IR_TYPE_POINTER));
        case IR_OP_SLICE_DATA:
            return a && a->type_id < module->type_count && module->types[a->type_id].kind == IR_TYPE_SLICE &&
                   instruction->type_id < module->type_count && module->types[instruction->type_id].kind ==
                   IR_TYPE_POINTER &&
                   module->types[instruction->type_id].element_type == module->types[a->type_id].element_type;
        case IR_OP_SLICE:
            return a && b && a->type_id < module->type_count &&
                   (module->types[a->type_id].kind == IR_TYPE_POINTER || module->types[a->type_id].kind ==
                    IR_TYPE_ARRAY) &&
                   instruction->type_id < module->type_count && module->types[instruction->type_id].kind ==
                   IR_TYPE_SLICE &&
                   module->types[instruction->type_id].element_type == module->types[a->type_id].element_type &&
                   ir_integral_type(module, b->type_id);
        case IR_OP_ARRAY_LITERAL:
            if (instruction->type_id >= module->type_count ||
                (module->types[instruction->type_id].kind != IR_TYPE_ARRAY &&
                 module->types[instruction->type_id].kind != IR_TYPE_SLICE) ||
                instruction->argument_count == 0 || instruction->element_count == 0)
                return 0;
            for (size_t argument = 0; argument < instruction->argument_count; argument++)
                if (verified_producer(function, producers,
                                      function->arguments[instruction->first_argument + argument],
                                      index) == NULL)
                    return 0;
            return 1;
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

int ir_verify_module(const IrModule *module) { return ir_verify_module_internal(module, 0); }

int ir_verify_module_report(const IrModule *module) { return ir_verify_module_internal(module, 1); }

static int ir_verify_module_internal(const IrModule *module, int report) {
    if (module == NULL || module->program == NULL || module->semantics == NULL) return 0;
    if (module->global_count && !module->globals) return 0;
    for (size_t g = 0; g < module->global_count; g++) {
        const IrGlobal *global = &module->globals[g];
        if (global->type_id >= module->type_count || global->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[global->symbol_id].kind != SEMANTIC_SYMBOL_VARIABLE ||
            module->semantics->symbols[global->symbol_id].source_program != global->source_program)
            return 0;
        if (global->string && (module->types[global->type_id].kind != IR_TYPE_PRIMITIVE ||
                               module->types[global->type_id].primitive != TYPE_STRING))
            return 0;
        for (size_t previous = 0; previous < g; previous++)
            if (
                module->globals[previous].symbol_id == global->symbol_id)
                return 0;
    }
    for (size_t t = 0; t < module->type_count; t++) {
        const IrType *type = &module->types[t];
        if (type->kind < IR_TYPE_PRIMITIVE || type->kind > IR_TYPE_FUNCTION) return 0;
        if (type->kind == IR_TYPE_PRIMITIVE &&
            type->primitive != TYPE_UNKNOWN &&
            (type->primitive < TYPE_INT || type->primitive > TYPE_VOID))
            return 0;
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
              module->semantics->symbols[type->symbol_id].kind != SEMANTIC_SYMBOL_ENUM &&
              module->semantics->symbols[type->symbol_id].kind != SEMANTIC_SYMBOL_INTERFACE)))
            return 0;
        if (type->kind == IR_TYPE_FUNCTION) {
            if (type->signature_id >= module->signature_count) return 0;
            const IrFunctionSignature *signature = &module->signatures[type->signature_id];
            if (signature->return_type >= t ||
                (signature->parameter_count && signature->parameter_types == NULL)) return 0;
            for (size_t p = 0; p < signature->parameter_count; p++)
                if (signature->parameter_types[p] >= t) return 0;
        }
    }
    for (size_t s = 0; s < module->structure_count; s++) {
        const IrAggregate *structure = &module->structures[s];
        if (structure->symbol_id == AST_SYMBOL_NONE ||
            structure->symbol_id >= module->semantics->symbol_count ||
            module->semantics->symbols[structure->symbol_id].kind != SEMANTIC_SYMBOL_STRUCT)
            return 0;
        if (((structure->type_properties & SEMANTIC_TYPE_COPYABLE) != 0) ==
            ((structure->type_properties & SEMANTIC_TYPE_MOVE_ONLY) != 0))
            return 0;
        if (structure->has_explicit_destructor &&
            (structure->type_properties &
             (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP)) !=
                (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP))
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
            for (size_t p = 0; p < item->payload_count; p++)
                if (item->payload_types[p] >= module->type_count) return 0;
            for (size_t argument = 0; argument < item->argument_count; argument++)
                if (enumeration->variant_arguments[item->first_argument + argument].type_id >=
                    module->type_count)
                    return 0;
        }
    }
    for (size_t f = 0; f < module->function_count; f++) {
        const IrFunction *function = &module->functions[f];
        if ((function->is_drop_glue && function->is_package_cleanup) ||
            (function->interface_thunk_symbol_id != AST_SYMBOL_NONE &&
             (function->is_drop_glue || function->is_package_cleanup))) return 0;
        if (function->is_package_cleanup) {
            if (function->symbol_id != AST_SYMBOL_NONE ||
                function->owner_symbol_id != AST_SYMBOL_NONE ||
                function->parameter_count != 0 ||
                function->source_program != module->program ||
                function->return_type_id >= module->type_count ||
                !ir_void_type(module, function->return_type_id))
                return 0;
        } else if (function->interface_thunk_symbol_id != AST_SYMBOL_NONE) {
            size_t method_id = function->interface_thunk_symbol_id;
            if (function->symbol_id != method_id ||
                method_id >= module->semantics->symbol_count ||
                module->semantics->symbols[method_id].kind != SEMANTIC_SYMBOL_FUNCTION ||
                module->semantics->symbols[method_id].source_program != function->source_program ||
                module->semantics->symbols[method_id].name_token != function->name_token ||
                function->return_type_id >= module->type_count) {
                return 0;
            }
        } else if (function->symbol_id == AST_SYMBOL_NONE ||
                   function->symbol_id >= module->semantics->symbol_count ||
                   (!function->is_drop_glue &&
                    module->semantics->symbols[function->symbol_id].kind !=
                    SEMANTIC_SYMBOL_FUNCTION) ||
                    (function->is_drop_glue &&
                     module->semantics->symbols[function->symbol_id].kind !=
                         SEMANTIC_SYMBOL_STRUCT &&
                     module->semantics->symbols[function->symbol_id].kind !=
                         SEMANTIC_SYMBOL_ENUM) ||
                   module->semantics->symbols[function->symbol_id].source_program !=
                   function->source_program ||
                   module->semantics->symbols[function->symbol_id].name_token !=
                   function->name_token ||
                   function->return_type_id >= module->type_count)
            return 0;
        if ((function->owner_token == AST_TOKEN_NONE) !=
            (function->owner_symbol_id == AST_SYMBOL_NONE))
            return 0;
        if (function->owner_symbol_id != AST_SYMBOL_NONE &&
            (function->owner_symbol_id >= module->semantics->symbol_count ||
             (module->semantics->symbols[function->owner_symbol_id].kind !=
                  SEMANTIC_SYMBOL_STRUCT &&
              module->semantics->symbols[function->owner_symbol_id].kind !=
                  SEMANTIC_SYMBOL_ENUM &&
              !(function->interface_thunk_symbol_id != AST_SYMBOL_NONE &&
                module->semantics->symbols[function->owner_symbol_id].kind ==
                    SEMANTIC_SYMBOL_INTERFACE)))) {
            return 0;
        }
        for (size_t p = 0; p < function->parameter_count; p++) {
            const IrParameter *parameter = &function->parameters[p];
            if (parameter->is_receiver) {
                if (p != 0 || function->owner_symbol_id == AST_SYMBOL_NONE ||
                    parameter->symbol_id != function->owner_symbol_id ||
                    parameter->pointer_depth != 1 ||
                    parameter->type_id >= module->type_count) {
                    return 0;
                }
                continue;
            }
            const SemanticSymbol *symbol = parameter->symbol_id <
                                           module->semantics->symbol_count
                                               ? &module->semantics->symbols[parameter->symbol_id]
                                               : NULL;
            if (function->interface_thunk_symbol_id != AST_SYMBOL_NONE) {
                if (parameter->source_program == NULL ||
                    parameter->name_token >= parameter->source_program->token_count ||
                    parameter->symbol_id < module->semantics->symbol_count ||
                    parameter->type_id >= module->type_count)
                    return 0;
                for (size_t previous = 0; previous < p; previous++)
                    if (function->parameters[previous].symbol_id == parameter->symbol_id)
                        return 0;
                continue;
            }
            if (parameter->source_program == NULL ||
                parameter->name_token >= parameter->source_program->token_count ||
                parameter->symbol_id == AST_SYMBOL_NONE ||
                parameter->symbol_id >= module->semantics->symbol_count ||
                symbol->kind != SEMANTIC_SYMBOL_PARAMETER ||
                symbol->source_program != parameter->source_program ||
                symbol->name_token != parameter->name_token ||
                symbol->owner_token != function->name_token ||
                ir_ast_type_data_type(symbol->source_program, &symbol->declared_type) !=
                parameter->type ||
                symbol->declared_type.pointer_depth +
                    symbol->declared_type.outer_pointer_depth +
                    (symbol->declared_type.borrow_kind != AST_BORROW_NONE) !=
                parameter->pointer_depth) {
                return 0;
            }
            if (parameter->type_id >= module->type_count) return 0;
            for (size_t previous = 0; previous < p; previous++)
                if (function->parameters[previous].symbol_id == parameter->symbol_id) return 0;
        }
        if ((function->next_value != 0 && function->next_value > SIZE_MAX / sizeof(const IrInstruction *)) ||
            (function->next_label != 0 && function->next_label > SIZE_MAX / sizeof(unsigned char)))
            return 0;
        const IrInstruction **producers = calloc(function->next_value, sizeof(*producers));
        unsigned char *labels = calloc(function->next_label, sizeof(*labels));
        if ((producers == NULL && function->next_value != 0) ||
            (labels == NULL && function->next_label != 0)) {
            free(producers);
            free(labels);
            return 0;
        }
        size_t failing_instruction = IR_VALUE_NONE;
        int valid = 1;
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            failing_instruction = i;
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
            failing_instruction = i;
            if ((instruction->has_immediate != 0 && instruction->has_immediate != 1) ||
                (instruction->has_immediate && instruction->opcode != IR_OP_CONSTANT) ||
                (instruction->bounds_check_elided != 0 && instruction->bounds_check_elided != 1) ||
                (instruction->bounds_check_elided && instruction->opcode != IR_OP_INDEX)) {
                valid = 0;
                break;
            }
            int produces_value = instruction_produces_value(instruction);
            if ((produces_value && instruction->result == IR_VALUE_NONE) ||
                (!produces_value && instruction->result != IR_VALUE_NONE) ||
                (instruction->result != IR_VALUE_NONE &&
                 (instruction->result >= function->next_value ||
                  producers[instruction->result] != NULL))) {
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
                    producers[required_value] == NULL) valid = 0; \
            } while (0)
#define REQUIRE_LABEL(label) \
            do { \
                size_t required_label = (label); \
                if (required_label == IR_VALUE_NONE || required_label >= function->next_label || \
                    labels[required_label] == 0) valid = 0; \
            } while (0)
            switch (instruction->opcode) {
                case IR_OP_CONSTANT:
                case IR_OP_FUNCTION_ADDRESS:
                case IR_OP_LOAD:
                case IR_OP_ALLOC:
                case IR_OP_LABEL:
                case IR_OP_MOVE:
                case IR_OP_REINIT:
                    break;
                case IR_OP_DROP:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_FREE_SLICE_BACKING:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
                    else if (instruction->symbol_id == AST_SYMBOL_NONE)
                        valid = 0;
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
                case IR_OP_SLICE:
                case IR_OP_INDEX:
                    REQUIRE_VALUE(instruction->operand_a);
                    REQUIRE_VALUE(instruction->operand_b);
                    break;
                case IR_OP_SUBSLICE:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->operand_b != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_b);
                    if (instruction->argument_count > 1 ||
                        (instruction->argument_count == 1 &&
                         instruction->first_argument >=
                             function->argument_count))
                        valid = 0;
                    else if (instruction->argument_count == 1)
                        REQUIRE_VALUE(function->arguments[
                            instruction->first_argument]);
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
                case IR_OP_SLICE_DATA:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_ENUM_IS:
                case IR_OP_ENUM_PAYLOAD:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->opcode == IR_OP_ENUM_PAYLOAD)
                        REQUIRE_LABEL(instruction->target_a);
                    break;
                case IR_OP_TRAP: break;
                case IR_OP_ENUM_CONSTRUCT:
                case IR_OP_ARRAY_LITERAL:
                    if (instruction->first_argument > function->argument_count ||
                        instruction->argument_count > function->argument_count - instruction->first_argument)
                        valid = 0;
                    else
                        for (size_t n = 0; n < instruction->argument_count; n++)
                            REQUIRE_VALUE(function->arguments[instruction->first_argument+n]);
                    break;
                case IR_OP_CALL:
                    if (instruction->operand_a != IR_VALUE_NONE)
                        REQUIRE_VALUE(instruction->operand_a);
                    else if (instruction->symbol_id == AST_SYMBOL_NONE)
                        valid = 0;
                    if (instruction->operand_a == IR_VALUE_NONE &&
                        instruction->symbol_id != AST_SYMBOL_NONE &&
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
            if (valid && !verify_instruction_types(module, function, producers, instruction, i))
                valid = 0;
            if (valid && instruction->result != IR_VALUE_NONE)
                producers[instruction->result] = instruction;
        }
        free(producers);
        free(labels);
        if (!valid || !ir_verify_control_flow(module, function, ir_void_type(module, function->return_type_id))) {
            if (report)
                ir_report_failure(function, valid ? IR_VALUE_NONE : failing_instruction,
                                  "IR verification",
                                  valid ? "malformed control flow" : "invalid instruction types or operands");
            return 0;
        }
    }
    return 1;
}
