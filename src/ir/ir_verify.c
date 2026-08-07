#include "ir.h"
#include "ir_cfg.h"
#include "ir_verify.h"
#include "core_intrinsics.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ir_async_verify.inc"

static int ir_verify_module_internal(const IrModule *module, int report);

static int native_ir_layout(const IrModule *module, IrTypeId id,
                            NativeTypeLayout *layout, size_t depth) {
    if (id >= module->type_count || depth > module->type_count) return 0;
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_POINTER) { *layout = (NativeTypeLayout){8, 8}; return 1; }
    if (type->kind == IR_TYPE_FUNCTION && type->signature_id < module->signature_count &&
        module->signatures[type->signature_id].is_native) {
        *layout = (NativeTypeLayout){8, 8}; return 1;
    }
    if (type->kind == IR_TYPE_PRIMITIVE) {
        if (!data_type_fixed_integer(type->primitive) && type->primitive != TYPE_BIT &&
            type->primitive != TYPE_FLOAT && type->primitive != TYPE_DOUBLE) return 0;
        size_t size = data_type_bytes(type->primitive);
        *layout = (NativeTypeLayout){size, size};
        return 1;
    }
    if (type->kind == IR_TYPE_ARRAY) {
        if (!type->array_length || !native_ir_layout(module, type->element_type, layout, depth + 1) ||
            type->array_length > SIZE_MAX / layout->size) return 0;
        layout->size *= type->array_length;
        return 1;
    }
    if (type->kind != IR_TYPE_NAMED) return 0;
    for (size_t s = 0; s < module->structure_count; s++) {
        const IrAggregate *structure = &module->structures[s];
        if (structure->symbol_id != type->symbol_id) continue;
        if (!structure->is_native || structure->is_opaque || !structure->field_count) return 0;
        size_t size = 0, alignment = 1;
        for (size_t f = 0; f < structure->field_count; f++) {
            const IrFieldDefinition *field = &structure->fields[f];
            NativeTypeLayout child;
            if (!native_ir_layout(module, field->type_id, &child, depth + 1) ||
                size > SIZE_MAX - (child.alignment - 1)) return 0;
            if (structure->native_pack && child.alignment > structure->native_pack)
                child.alignment = structure->native_pack;
            if (!structure->is_native_union) size = (size + child.alignment - 1) & ~(child.alignment - 1);
            if (field->native_offset != (structure->is_native_union ? 0 : size) || child.size > SIZE_MAX - size) return 0;
            if (structure->is_native_union) { if (child.size > size) size = child.size; }
            else size += child.size;
            if (child.alignment > alignment) alignment = child.alignment;
            const IrType *field_type = &module->types[field->type_id];
            size_t stride = field_type->kind == IR_TYPE_ARRAY ? child.size / field_type->array_length : 0;
            if (field->native_array_stride != stride) return 0;
        }
        if (structure->native_alignment > alignment) alignment = structure->native_alignment;
        if (size > SIZE_MAX - (alignment - 1)) return 0;
        *layout = (NativeTypeLayout){(size + alignment - 1) & ~(alignment - 1), alignment};
        return layout->size == structure->native_layout.size &&
               layout->alignment == structure->native_layout.alignment;
    }
    return 0;
}

static int native_ir_signature_type(const IrModule *module, IrTypeId id, int result) {
    if (id >= module->type_count) return 0;
    const IrType *type = &module->types[id];
    if (type->kind == IR_TYPE_ARRAY) return 0;
    if (result && type->kind == IR_TYPE_PRIMITIVE && type->primitive == TYPE_VOID) return 1;
    NativeTypeLayout layout;
    return native_ir_layout(module, id, &layout, 0);
}

static int native_type_matches_source(const IrModule *module, IrTypeId id,
                                      const AstProgram *program, const AstType *source, size_t depth) {
    if (!source || id >= module->type_count || depth > module->type_count + 64) return 0;
    AstType expected = *source;
    unsigned outer = expected.outer_pointer_depth;
    if (expected.element_type) { outer += expected.pointer_depth; expected.pointer_depth = 0; }
    if (expected.borrow_kind != AST_BORROW_NONE) outer++;
    expected.outer_pointer_depth = 0;
    expected.borrow_kind = AST_BORROW_NONE;
    for (unsigned p = 0; p < outer; p++) {
        if (id >= module->type_count || module->types[id].kind != IR_TYPE_POINTER) return 0;
        id = module->types[id].element_type;
    }
    if (id >= module->type_count) return 0;
    const IrType *type = &module->types[id];
    if (expected.is_array || expected.is_slice) {
        IrTypeKind kind = expected.is_array ? IR_TYPE_ARRAY : IR_TYPE_SLICE;
        if (type->kind != kind) return 0;
        size_t length = expected.resolved_array_length;
        if (!length && expected.array_length_token < program->token_count)
            length = (size_t)strtoull(ast_program_lexeme(program, expected.array_length_token), NULL, 10);
        if (expected.is_array && type->array_length != length) return 0;
        AstType element = ast_type_element(&expected);
        return native_type_matches_source(module, type->element_type, program, &element, depth + 1);
    }
    for (unsigned p = 0; p < expected.pointer_depth; p++) {
        if (id >= module->type_count || module->types[id].kind != IR_TYPE_POINTER) return 0;
        id = module->types[id].element_type;
    }
    if (id >= module->type_count) return 0;
    type = &module->types[id];
    if (expected.kind == AST_TYPE_FUNCTION) {
        if (type->kind != IR_TYPE_FUNCTION || type->signature_id >= module->signature_count) return 0;
        const IrFunctionSignature *signature = &module->signatures[type->signature_id];
        if (signature->is_native != expected.is_native_function) return 0;
        if (!native_type_matches_source(module, signature->return_type, program,
                                        expected.function_return_type, depth + 1)) return 0;
        size_t count = 0;
        for (const AstTypeArgument *p = expected.function_parameters; p; p = p->next, count++)
            if (count >= signature->parameter_count || !native_type_matches_source(module,
                signature->parameter_types[count], program, &p->type, depth + 1)) return 0;
        return count == signature->parameter_count;
    }
    if (expected.kind == AST_TYPE_FUTURE || expected.kind == AST_TYPE_JOIN) {
        IrTypeKind kind = expected.kind == AST_TYPE_FUTURE ? IR_TYPE_FUTURE : IR_TYPE_JOIN;
        return type->kind == kind && expected.arguments &&
               native_type_matches_source(module, type->element_type, program, &expected.arguments->type, depth + 1);
    }
    if (expected.kind == AST_TYPE_EXECUTOR) return type->kind == IR_TYPE_EXECUTOR;
    const AstToken *token = ast_program_token(program, expected.name_token);
    if (!token || expected.kind != AST_TYPE_NAMED) return 0;
    DataType primitive = token_data_type(token->type);
    if (primitive != TYPE_UNKNOWN)
        return type->kind == IR_TYPE_PRIMITIVE && type->primitive == primitive;
    const SemanticSymbol *symbol = semantic_find_in_package(module->semantics, program,
        token->lexeme, SEMANTIC_SYMBOL_STRUCT);
    if (!symbol) symbol = semantic_find_in_package(module->semantics, program, token->lexeme, SEMANTIC_SYMBOL_ENUM);
    if (!symbol) symbol = semantic_find_in_package(module->semantics, program, token->lexeme, SEMANTIC_SYMBOL_INTERFACE);
    return symbol && type->kind == IR_TYPE_NAMED && type->symbol_id == symbol->id;
}

static int instruction_produces_value(const IrInstruction *instruction) {
    IrOpcode opcode = instruction->opcode;
    return opcode == IR_OP_CONSTANT || opcode == IR_OP_FUNCTION_ADDRESS || opcode == IR_OP_LOAD ||
           opcode == IR_OP_UNARY || opcode == IR_OP_BINARY ||
           ((opcode == IR_OP_CALL || opcode == IR_OP_AWAIT || opcode == IR_OP_EXECUTOR) &&
            (data_type_has_value(instruction->type) || instruction->pointer_depth)) ||
           opcode == IR_OP_INDEX || opcode == IR_OP_SUBSLICE ||
           opcode == IR_OP_MEMBER || opcode == IR_OP_SLICE_LENGTH ||
           opcode == IR_OP_SLICE || opcode == IR_OP_SLICE_DATA ||
           opcode == IR_OP_ARRAY_LITERAL || opcode == IR_OP_STRUCT_LITERAL || opcode == IR_OP_INTERFACE_PACK || opcode == IR_OP_NATIVE_COPY ||
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
    if (from->kind == IR_TYPE_PRIMITIVE && from->primitive == TYPE_NEVER)
        return 1;
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
    if (instruction->opcode == IR_OP_AWAIT) {
        if (!function->is_async || a == NULL ||
            (module->types[a->type_id].kind != IR_TYPE_FUTURE && module->types[a->type_id].kind != IR_TYPE_JOIN) ||
            (module->types[a->type_id].kind == IR_TYPE_FUTURE && module->types[a->type_id].element_type != instruction->type_id) ||
            instruction->target_a == 0 ||
            instruction->target_a > function->async_state_count) return 0;
        for (size_t previous = 0; previous < index; previous++)
            if (function->instructions[previous].opcode == IR_OP_AWAIT &&
                function->instructions[previous].target_a == instruction->target_a) return 0;
        if(module->types[a->type_id].kind==IR_TYPE_JOIN) {
            const IrType *result=&module->types[instruction->type_id];
            const IrEnum *enumeration=NULL;
            for(size_t e=0;e<module->enum_count;e++)
                if(result->kind==IR_TYPE_NAMED && module->enums[e].symbol_id==result->symbol_id) enumeration=&module->enums[e];
            if(!enumeration || !enumeration->is_sum || enumeration->variant_count!=2) return 0;
            IrTypeId output=module->types[a->type_id].element_type;
            const IrEnumVariant *ok=&enumeration->variants[0],*error=&enumeration->variants[1];
            if(ir_void_type(module,output) ? ok->payload_count!=0 :
               (ok->payload_count!=1 || ok->payload_types[0]!=output)) return 0;
            if(error->payload_count!=1 || module->types[error->payload_types[0]].kind!=IR_TYPE_NAMED) return 0;
            size_t error_symbol=module->types[error->payload_types[0]].symbol_id;
            const SemanticSymbol *s=&module->semantics->symbols[error_symbol];
            if(!s->declaration || !s->declaration->is_async_builtin ||
               strcmp(ast_program_lexeme(s->source_program,s->name_token),"TaskError")) return 0;
        }
        return 1;
    }
    switch (instruction->opcode) {
        case IR_OP_CANCEL_CHECK: return function->is_async && !instruction->async_cleanup && ir_void_type(module,instruction->type_id) &&
                                            instruction->target_a!=instruction->target_b;
        case IR_OP_CANCEL_RETURN: return function->is_async && instruction->async_cleanup && ir_void_type(module,instruction->type_id);
        case IR_OP_CANCEL_AWAIT: return function->is_async && instruction->async_cleanup && a &&
            (module->types[a->type_id].kind==IR_TYPE_FUTURE || module->types[a->type_id].kind==IR_TYPE_JOIN) &&
            ir_void_type(module,instruction->type_id) && instruction->target_a>0 && instruction->target_a<=function->async_state_count;
        case IR_OP_CANCEL_DROP: return function->is_async && instruction->async_cleanup &&
            instruction->symbol_id<module->semantics->symbol_count &&
            (ir_type_properties(module,instruction->type_id)&SEMANTIC_TYPE_NEEDS_DROP) &&
            instruction->target_a>0 && instruction->target_a<=function->async_state_count;
        case IR_OP_EXECUTOR:
            if (!a || instruction->async_operation<ASYNC_CREATE || instruction->async_operation>ASYNC_NET_WAIT) return 0;
            if (b && module->types[b->type_id].kind!=IR_TYPE_EXECUTOR) return 0;
            switch (instruction->async_operation) {
                case ASYNC_NET_WAIT: return data_type_integral(a->type) &&
                    module->types[instruction->type_id].kind==IR_TYPE_FUTURE &&
                    module->types[module->types[instruction->type_id].element_type].kind==IR_TYPE_PRIMITIVE &&
                    module->types[module->types[instruction->type_id].element_type].primitive==TYPE_VOID &&
                    (instruction->runtime_requirements&RUNTIME_REQUIRE_NETWORK)!=0;
                case ASYNC_CREATE: return !b && ir_integral_type(module,a->type_id) && module->types[instruction->type_id].kind==IR_TYPE_EXECUTOR;
                case ASYNC_SPAWN: return module->types[a->type_id].kind==IR_TYPE_FUTURE && module->types[instruction->type_id].kind==IR_TYPE_JOIN &&
                                        module->types[a->type_id].element_type==module->types[instruction->type_id].element_type;
                case ASYNC_BLOCK_ON: return !function->is_async && module->types[a->type_id].kind==IR_TYPE_FUTURE &&
                                           module->types[a->type_id].element_type==instruction->type_id;
                case ASYNC_SHUTDOWN: return b && module->types[instruction->type_id].kind==IR_TYPE_FUTURE &&
                                           ir_void_type(module,module->types[instruction->type_id].element_type);
                case ASYNC_CANCEL: return (module->types[a->type_id].kind==IR_TYPE_FUTURE || module->types[a->type_id].kind==IR_TYPE_JOIN) &&
                                         module->types[instruction->type_id].kind==IR_TYPE_FUTURE && ir_void_type(module,module->types[instruction->type_id].element_type);
                case ASYNC_NONE: return 0;
            }
            return 0;
        case IR_OP_AWAIT: return 0; /* checked above */
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
        case IR_OP_NATIVE_COPY: {
            if (!a || a->type_id != instruction->type_id ||
                module->types[instruction->type_id].kind != IR_TYPE_NAMED) return 0;
            for (size_t s = 0; s < module->structure_count; ++s)
                if (module->structures[s].symbol_id == module->types[instruction->type_id].symbol_id)
                    return module->structures[s].is_native && !module->structures[s].is_opaque;
            return 0;
        }
        case IR_OP_INTERFACE_PACK:
            return a != NULL && a->type_id < module->type_count &&
                   instruction->type_id < module->type_count &&
                   module->types[a->type_id].kind == IR_TYPE_NAMED &&
                   module->types[instruction->type_id].kind == IR_TYPE_NAMED &&
                   module->types[a->type_id].symbol_id < module->semantics->symbol_count &&
                   module->semantics->symbols[module->types[a->type_id].symbol_id].kind ==
                       SEMANTIC_SYMBOL_STRUCT &&
                   semantic_implements_interface(module->semantics,
                       module->types[instruction->type_id].symbol_id,
                       module->types[a->type_id].symbol_id);
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
            const IrNativeImport *native = ir_native_import(module, instruction->symbol_id);
            if (native) {
                if (instruction->argument_count != native->parameter_count ||
                    instruction->type_id != native->return_type_id) return 0;
                for (size_t argument = 0; argument < native->parameter_count; argument++) {
                    const IrInstruction *value = verified_producer(function, producers,
                        function->arguments[instruction->first_argument + argument], index);
                    if (!value || !ir_types_assignable(module, value->type_id,
                            native->parameter_types[argument], value->opcode)) return 0;
                }
                return 1;
            }
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
                if (!strncmp(core->source_name,"__dmm_net_",10) &&
                    !(instruction->runtime_requirements & RUNTIME_REQUIRE_NETWORK)) return 0;
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
                instruction->type_id != (callee->is_async ? callee->future_type_id : callee->return_type_id))
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
                   (module->semantics->symbols[instruction->symbol_id].kind == SEMANTIC_SYMBOL_LOCAL ||
                    module->semantics->symbols[instruction->symbol_id].kind == SEMANTIC_SYMBOL_PARAMETER ||
                    module->semantics->symbols[instruction->symbol_id].kind == SEMANTIC_SYMBOL_VARIABLE) &&
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
                   (module->semantics->symbols[instruction->symbol_id].kind ==
                        SEMANTIC_SYMBOL_LOCAL ||
                    module->semantics->symbols[instruction->symbol_id].kind ==
                        SEMANTIC_SYMBOL_VARIABLE);
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
        case IR_OP_STRUCT_LITERAL: {
            if (instruction->type_id >= module->type_count ||
                module->types[instruction->type_id].kind != IR_TYPE_NAMED ||
                instruction->operand_a != IR_VALUE_NONE || instruction->operand_b != IR_VALUE_NONE ||
                instruction->argument_count != 0) return 0;
            size_t symbol = module->types[instruction->type_id].symbol_id;
            IrTypeLayout layout;
            return symbol < module->semantics->symbol_count &&
                module->semantics->symbols[symbol].kind == SEMANTIC_SYMBOL_STRUCT &&
                module->semantics->symbols[symbol].declaration != NULL &&
                !module->semantics->symbols[symbol].declaration->is_opaque &&
                ir_type_layout(module, instruction->type_id, &layout) && layout.size != 0;
        }
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
            return a != NULL && b != NULL &&
                   ir_types_assignable(module, a->type_id,
                                       instruction->type_id, a->opcode) &&
                   ir_types_assignable(module, b->type_id,
                                       instruction->type_id, b->opcode);
    }
    return 0;
}

int ir_verify_module(const IrModule *module) { return ir_verify_module_internal(module, 0); }

int ir_verify_module_report(const IrModule *module) { return ir_verify_module_internal(module, 1); }

static int ir_verify_module_internal(const IrModule *module, int report) {
    if (module == NULL || module->program == NULL || module->semantics == NULL ||
        module->target_format != module->semantics->target_format ||
        (module->native_import_count && !module->native_imports)) return 0;
    for (size_t i = 0; i < module->structure_count; i++) {
        uint64_t tag = ir_interface_type_tag(module,
                                             module->structures[i].symbol_id);
        if (tag == 0) return 0;
        for (size_t j = 0; j < i; j++)
            if (ir_interface_type_tag(module,
                                      module->structures[j].symbol_id) == tag)
                return 0;
    }
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
        if (type->kind < IR_TYPE_PRIMITIVE || type->kind > IR_TYPE_EXECUTOR) return 0;
        if (type->kind == IR_TYPE_PRIMITIVE &&
            type->primitive != TYPE_UNKNOWN &&
            (type->primitive < TYPE_INT || type->primitive > TYPE_NEVER))
            return 0;
        if (type->kind == IR_TYPE_ARRAY && type->array_length == 0) return 0;
        if (type->kind != IR_TYPE_ARRAY && type->array_length != 0) return 0;
        if ((type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_ARRAY ||
             type->kind == IR_TYPE_SLICE || type->kind == IR_TYPE_FUTURE || type->kind == IR_TYPE_JOIN) &&
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
            if (signature->is_native) {
                if (!native_ir_signature_type(module, signature->return_type, 1)) return 0;
                for (size_t p = 0; p < signature->parameter_count; ++p)
                    if (!native_ir_signature_type(module, signature->parameter_types[p], 0)) return 0;
            }
        }
    }
    for (size_t n = 0; n < module->native_import_count; n++) {
        const IrNativeImport *import = &module->native_imports[n];
        if (!import->abi || strcmp(import->abi, "system") || !import->library || !*import->library ||
            !import->native_name || !*import->native_name ||
            import->symbol_id >= module->semantics->symbol_count ||
            import->return_type_id >= module->type_count ||
            (import->parameter_count && !import->parameter_types)) return 0;
        const SemanticSymbol *symbol = &module->semantics->symbols[import->symbol_id];
        const AstDeclarationNode *decl = symbol->declaration;
        if (symbol->kind != SEMANTIC_SYMBOL_FUNCTION || !decl || !decl->is_native ||
            decl->as.function.body || decl->as.function.is_async || decl->generic_parameters ||
            import->source_program != symbol->source_program) return 0;
        if (import->span.begin.line != decl->span.begin.line || import->span.begin.column != decl->span.begin.column ||
            import->span.end.line != decl->span.end.line || import->span.end.column != decl->span.end.column) return 0;
        if (strcmp(import->library, ast_program_lexeme(symbol->source_program, decl->native_library_token)) ||
            strcmp(import->native_name, ast_program_lexeme(symbol->source_program, decl->native_name_token)) ||
            !native_ir_signature_type(module, import->return_type_id, 1) ||
            !native_type_matches_source(module, import->return_type_id, symbol->source_program,
                                         &decl->as.function.return_type, 0)) return 0;
        for (size_t f = 0; f < module->function_count; f++)
            if (module->functions[f].symbol_id == import->symbol_id) return 0;
        for (size_t p = 0; p < import->parameter_count; p++)
            if (!native_ir_signature_type(module, import->parameter_types[p], 0)) return 0;
        size_t parameter_count = 0;
        for (const AstParameter *p = decl->as.function.parameters; p; p = p->next, parameter_count++)
            if (parameter_count >= import->parameter_count || !native_type_matches_source(module,
                import->parameter_types[parameter_count], symbol->source_program, &p->type, 0)) return 0;
        if (import->parameter_count != parameter_count) return 0;
        for (size_t previous = 0; previous < n; previous++) {
            const IrNativeImport *other = &module->native_imports[previous];
            if (other->symbol_id == import->symbol_id) return 0;
            if (strcmp(other->native_name, import->native_name)) continue;
            if (strcmp(other->library, import->library) || other->return_type_id != import->return_type_id ||
                other->parameter_count != import->parameter_count) return 0;
            for (size_t p = 0; p < import->parameter_count; p++)
                if (other->parameter_types[p] != import->parameter_types[p]) return 0;
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
        const AstDeclarationNode *source_declaration = module->semantics->symbols[structure->symbol_id].declaration;
        if (!source_declaration || source_declaration->is_native != structure->is_native ||
            source_declaration->is_native_union != structure->is_native_union ||
            source_declaration->native_pack != structure->native_pack ||
            source_declaration->native_alignment != structure->native_alignment ||
            source_declaration->is_opaque != structure->is_opaque) return 0;
        if (structure->is_native) {
            if (structure->has_explicit_destructor ||
                (structure->type_properties & SEMANTIC_TYPE_NEEDS_DROP) ||
                !(structure->type_properties & SEMANTIC_TYPE_COPYABLE)) return 0;
            const SemanticSymbol *symbol = &module->semantics->symbols[structure->symbol_id];
            const AstDeclarationNode *decl = symbol->declaration;
            if (!decl || !decl->is_native || decl->is_opaque != structure->is_opaque) return 0;
            if (structure->is_opaque) {
                if (structure->field_count || structure->native_layout.size || structure->native_layout.alignment)
                    return 0;
            } else {
                AstType type = {.kind = AST_TYPE_NAMED, .name_token = structure->name_token};
                NativeTypeLayout expected;
                if (!semantic_native_layout(module->semantics, structure->source_program, &type, &expected) ||
                    expected.size != structure->native_layout.size ||
                    expected.alignment != structure->native_layout.alignment) return 0;
                IrTypeId id = IR_TYPE_NONE;
                for (size_t t = 0; t < module->type_count; t++)
                    if (module->types[t].kind == IR_TYPE_NAMED && module->types[t].symbol_id == structure->symbol_id) { id = t; break; }
                if (id != IR_TYPE_NONE && !native_ir_layout(module, id, &expected, 0)) return 0;
                const AstField *field = decl->as.struct_decl.fields;
                for (size_t f = 0; f < structure->field_count; f++, field = field->next) {
                    size_t offset;
                    if (!field || field->resolved_symbol_id != structure->fields[f].symbol_id ||
                        field->name_token != structure->fields[f].name_token ||
                        structure->source_program != structure->fields[f].source_program ||
                        !semantic_native_field_offset(module->semantics, structure->symbol_id, f, &offset) ||
                        offset != structure->fields[f].native_offset ||
                        !native_type_matches_source(module, structure->fields[f].type_id,
                                                     structure->source_program, &field->type, 0)) return 0;
                    size_t stride = 0;
                    if (field->type.is_array && !field->type.outer_pointer_depth) {
                        AstType element = ast_type_element(&field->type);
                        NativeTypeLayout layout;
                        if (!semantic_native_layout(module->semantics, structure->source_program, &element, &layout)) return 0;
                        stride = layout.size;
                    }
                    if (stride != structure->fields[f].native_array_stride) return 0;
                }
                if (field) return 0;
            }
        }
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
        if ((function->is_drop_glue &&
             (function->is_package_init || function->is_package_cleanup)) ||
            (function->is_package_init && function->is_package_cleanup) ||
            (function->interface_thunk_symbol_id != AST_SYMBOL_NONE &&
             (function->is_drop_glue || function->is_package_init ||
              function->is_package_cleanup))) return 0;
        if (function->is_package_init || function->is_package_cleanup) {
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
        if (!function->is_drop_glue && !function->is_package_init && !function->is_package_cleanup &&
            function->interface_thunk_symbol_id == AST_SYMBOL_NONE) {
            const AstDeclarationNode *decl = module->semantics->symbols[function->symbol_id].declaration;
            if (!decl || function->is_native_export != decl->is_native_export) return 0;
        }
        if (function->is_native_export) {
            if (function->is_async || !native_ir_signature_type(module, function->return_type_id, 1)) return 0;
            for (size_t p = 0; p < function->parameter_count; ++p)
                if (!native_ir_signature_type(module, function->parameters[p].type_id, 0)) return 0;
        }
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
                case IR_OP_STRUCT_LITERAL:
                    break;
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
                case IR_OP_NATIVE_COPY:
                case IR_OP_INTERFACE_PACK:
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
                case IR_OP_AWAIT:
                case IR_OP_CANCEL_AWAIT:
                case IR_OP_SLICE_LENGTH:
                case IR_OP_SLICE_DATA:
                    REQUIRE_VALUE(instruction->operand_a);
                    break;
                case IR_OP_EXECUTOR:
                    REQUIRE_VALUE(instruction->operand_a);
                    if(instruction->operand_b!=IR_VALUE_NONE) REQUIRE_VALUE(instruction->operand_b);
                    break;
                case IR_OP_ENUM_IS:
                case IR_OP_ENUM_PAYLOAD:
                    REQUIRE_VALUE(instruction->operand_a);
                    if (instruction->opcode == IR_OP_ENUM_PAYLOAD)
                        REQUIRE_LABEL(instruction->target_a);
                    break;
                case IR_OP_TRAP: break;
                case IR_OP_CANCEL_DROP:
                case IR_OP_CANCEL_RETURN: break;
                case IR_OP_CANCEL_CHECK:
                    REQUIRE_LABEL(instruction->target_a); REQUIRE_LABEL(instruction->target_b);
                    break;
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
        if (!valid || !ir_verify_control_flow(module, function, ir_void_type(module, function->return_type_id)) ||
            !verify_async_effects(module, function)) {
            if (report)
                ir_report_failure(function, valid ? IR_VALUE_NONE : failing_instruction,
                                  "IR verification",
                                  valid ? "malformed control flow" : "invalid instruction types or operands");
            return 0;
        }
    }
    return 1;
}
