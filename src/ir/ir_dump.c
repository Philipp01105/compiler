#include "ir.h"

#include <stdio.h>

static int quoted(FILE *output, const char *text) {
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

static const char *primitive_name(DataType type) {
    static const char *names[] = {DMM_TYPE_NAMES};
    return type >= TYPE_INT && type <= TYPE_UNKNOWN ? names[type] : "invalid";
}

static const char *opcode_name(IrOpcode opcode) {
    static const char *names[] = {
        "constant", "load", "declare", "store", "unary",
        "binary", "call", "index", "subslice", "member", "slice-length", "cast", "alloc", "free",
        "return", "branch", "jump", "label", "phi", "enum-construct", "enum-is", "enum-payload", "trap", "slice",
        "slice-data", "array-literal", "drop", "move", "reinit", "free-slice-backing"
    };
    return opcode >= IR_OP_CONSTANT && opcode <= IR_OP_FREE_SLICE_BACKING ? names[opcode] : "invalid";
}

static const char *operator_name(TokenType type) {
    switch (type) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_EQUAL: return "=";
        case TOKEN_EQUAL_EQUAL: return "==";
        case TOKEN_BANG_EQUAL: return "!=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_AMP_AMP: return "&&";
        case TOKEN_PIPE_PIPE: return "||";
        case TOKEN_BANG: return "!";
        case TOKEN_AMPERSAND: return "&";
        case TOKEN_PLUS_EQUAL: return "+=";
        case TOKEN_MINUS_EQUAL: return "-=";
        case TOKEN_STAR_EQUAL: return "*=";
        case TOKEN_SLASH_EQUAL: return "/=";
        case TOKEN_PLUS_PLUS: return "++";
        case TOKEN_MINUS_MINUS: return "--";
        default: return "-";
    }
}

static int print_id(FILE *output, const char *prefix, size_t value, size_t none) {
    return value == none ? fputc('-', output) != EOF : fprintf(output, "%s%zu", prefix, value) >= 0;
}

static int print_span(FILE *output, AstSourceSpan span) {
    return fprintf(output, "%d:%d-%d:%d", span.begin.line, span.begin.column,
                   span.end.line, span.end.column) >= 0;
}

static int print_token(FILE *output, const AstProgram *program, size_t token) {
    return token == AST_TOKEN_NONE
               ? fputc('-', output) != EOF
               : quoted(output, ast_program_lexeme(program, token));
}

static int dump_types(FILE *output, const IrModule *module) {
    for (size_t i = 0; i < module->type_count; i++) {
        const IrType *type = &module->types[i];
        if (fprintf(output, "type @%zu ", i) < 0) return 0;
        switch (type->kind) {
            case IR_TYPE_PRIMITIVE:
                if (fprintf(output, "primitive %s", primitive_name(type->primitive)) < 0) return 0;
                break;
            case IR_TYPE_NAMED:
                if (fprintf(output, "named symbol=%zu", type->symbol_id) < 0) return 0;
                break;
            case IR_TYPE_POINTER:
                if (fprintf(output, "pointer element=@%zu", type->element_type) < 0) return 0;
                break;
            case IR_TYPE_ARRAY:
                if (fprintf(output, "array element=@%zu length=%zu", type->element_type,
                            type->array_length) < 0)
                    return 0;
                break;
            case IR_TYPE_SLICE:
                if (fprintf(output, "slice element=@%zu", type->element_type) < 0) return 0;
                break;
            default: return 0;
        }
        if (fputc('\n', output) == EOF) return 0;
    }
    return 1;
}

static int dump_instruction(FILE *output, const IrFunction *function,
                            const IrInstruction *instruction, size_t index) {
    if (fprintf(output, "  instruction #%zu opcode=%s result=", index,
                opcode_name(instruction->opcode)) < 0 ||
        !print_id(output, "%", instruction->result, IR_VALUE_NONE) ||
        fputs(" type=", output) == EOF ||
        !print_id(output, "@", instruction->type_id, IR_TYPE_NONE) ||
        fprintf(output, " primitive=%s pointers=%u array=%d slice=%d a=",
                primitive_name(instruction->type), instruction->pointer_depth,
                instruction->is_array, instruction->is_slice) < 0 ||
        !print_id(output, "%", instruction->operand_a, IR_VALUE_NONE) ||
        fputs(" b=", output) == EOF ||
        !print_id(output, "%", instruction->operand_b, IR_VALUE_NONE) ||
        fputs(" symbol=", output) == EOF ||
        !print_id(output, "", instruction->symbol_id, AST_SYMBOL_NONE) ||
        fputs(" token=", output) == EOF ||
        !print_token(output, function->source_program, instruction->auxiliary_token) ||
        fputs(" targets=", output) == EOF ||
        !print_id(output, "L", instruction->target_a, IR_VALUE_NONE) ||
        fputc(',', output) == EOF || !print_id(output, "L", instruction->target_b, IR_VALUE_NONE) ||
        fputs(" arguments=", output) == EOF)
        return 0;
    if (instruction->argument_count == 0) {
        if (fputc('-', output) == EOF) return 0;
    } else if (fprintf(output, "%zu+%zu", instruction->first_argument,
                       instruction->argument_count) < 0)
        return 0;
    if (instruction->opcode == IR_OP_ENUM_PAYLOAD &&
        fprintf(output, " payload-index=%zu", instruction->enum_payload_index) < 0)
        return 0;
    if (fputs(" span=", output) == EOF || !print_span(output, instruction->span)) return 0;
    if (instruction->argument_count != 0) {
        if (fputs(" values=[", output) == EOF) return 0;
        for (size_t a = 0; a < instruction->argument_count; a++) {
            size_t position = instruction->first_argument + a;
            if (a != 0 && fputc(',', output) == EOF) return 0;
            if (position >= function->argument_count ||
                !print_id(output, "%", function->arguments[position], IR_VALUE_NONE))
                return 0;
        }
        if (fputc(']', output) == EOF) return 0;
    }
    if ((instruction->opcode == IR_OP_UNARY || instruction->opcode == IR_OP_BINARY ||
         instruction->opcode == IR_OP_STORE) &&
        (fputs(" operator=", output) == EOF ||
         !quoted(output, operator_name(instruction->operator_type))))
        return 0;
    if (instruction->has_immediate &&
        fprintf(output, " immediate=0x%016llx", (unsigned long long) instruction->immediate) < 0)
        return 0;
    if (instruction->bounds_check_elided && fputs(" bounds-check=elided", output) == EOF) return 0;
    return fputc('\n', output) != EOF;
}

static int dump_aggregates(FILE *output, const IrModule *module) {
    for (size_t i = 0; i < module->structure_count; i++) {
        const IrAggregate *aggregate = &module->structures[i];
        if (fprintf(output, "struct #%zu name=", i) < 0 ||
            !print_token(output, aggregate->source_program, aggregate->name_token) ||
            fprintf(output, " symbol=%zu fields=%zu\n", aggregate->symbol_id,
                    aggregate->field_count) < 0)
            return 0;
        for (size_t f = 0; f < aggregate->field_count; f++)
            if (fputs("  field name=", output) == EOF ||
                !print_token(output, aggregate->fields[f].source_program,
                             aggregate->fields[f].name_token) ||
                fprintf(output, " symbol=%zu type=@%zu\n", aggregate->fields[f].symbol_id,
                        aggregate->fields[f].type_id) < 0)
                return 0;
    }
    for (size_t i = 0; i < module->enum_count; i++) {
        const IrEnum *enumeration = &module->enums[i];
        if (fprintf(output, "enum #%zu name=", i) < 0 ||
            !print_token(output, enumeration->source_program, enumeration->name_token) ||
            fprintf(output, " symbol=%zu fields=%zu variants=%zu sum=%d\n", enumeration->symbol_id,
                    enumeration->field_count, enumeration->variant_count, enumeration->is_sum) < 0)
            return 0;
        for (size_t field = 0; field < enumeration->field_count; field++) {
            const IrFieldDefinition *definition = &enumeration->fields[field];
            if (fputs("  field name=", output) == EOF ||
                !print_token(output, definition->source_program, definition->name_token) ||
                fprintf(output, " symbol=%zu type=@%zu\n", definition->symbol_id,
                        definition->type_id) < 0)
                return 0;
        }
        for (size_t v = 0; v < enumeration->variant_count; v++) {
            const IrEnumVariant *variant = &enumeration->variants[v];
            if (fputs("  variant name=", output) == EOF ||
                !print_token(output, variant->source_program, variant->name_token) ||
                fprintf(output, " symbol=%zu arguments=%zu+%zu\n", variant->symbol_id,
                        variant->first_argument, variant->argument_count) < 0)
                return 0;
            for (size_t p = 0; p < variant->payload_count; p++)
                if (fprintf(output, "    payload #%zu type=@%zu\n", p, variant->payload_types[p]) < 0) return 0;
            for (size_t p = 0; p < variant->payload_count; p++)
                if (fprintf(output, "    payload #%zu type=@%zu\n", p, variant->payload_types[p]) < 0) return 0;
        }
        for (size_t a = 0; a < enumeration->variant_argument_count; a++) {
            const IrEnumArgument *argument = &enumeration->variant_arguments[a];
            if (fprintf(output, "  argument #%zu value=", a) < 0 ||
                !print_token(output, enumeration->source_program, argument->token) ||
                fprintf(output, " type=@%zu negative=%d\n", argument->type_id,
                        argument->negative) < 0)
                return 0;
        }
    }
    for (size_t i = 0; i < module->import_count; i++) {
        const IrImport *import = &module->imports[i];
        if (fprintf(output, "import #%zu symbol=%zu path=", i, import->symbol_id) < 0) return 0;
        if (import->path_token != AST_TOKEN_NONE) {
            if (!print_token(output, import->source_program, import->path_token)) return 0;
        } else if (fprintf(output, "tokens:%zu+%zu", import->path_first_token,
                           import->path_token_count) < 0)
            return 0;
        if (fputc('\n', output) == EOF) return 0;
    }
    return 1;
}

int ir_dump(FILE *output, const IrModule *module) {
    if (output == NULL || module == NULL || !ir_verify_module(module)) return 0;
    if (fputs("dmm-ir-v3\nmodule path=", output) == EOF ||
        !quoted(output, module->program->source_path) ||
        fprintf(output, " verified=%d types=%zu functions=%zu structs=%zu enums=%zu imports=%zu\n",
                module->verified, module->type_count, module->function_count,
                module->structure_count, module->enum_count, module->import_count) < 0 ||
        !dump_types(output, module) || !dump_aggregates(output, module))
        return 0;
    if (fputs("package identity=", output) == EOF || !quoted(
            output, module->program->module_identity ? module->program->module_identity : "") ||
        fputc('\n', output) == EOF)
        return 0;
    for (size_t g = 0; g < module->global_count; g++) {
        if (fprintf(output, "global #%zu symbol=%zu type=@%zu bits=%llu\n", g, module->globals[g].symbol_id,
                    module->globals[g].type_id,
                    (unsigned long long) module->globals[g].bits) < 0)
            return 0;
    }
    for (size_t f = 0; f < module->function_count; f++)
        if (!ir_dump_function(output, module, f)) return 0;
    return !ferror(output);
}

int ir_dump_function(FILE *output, const IrModule *module, size_t function_index) {
    if (output == NULL || module == NULL || function_index >= module->function_count) return 0;

    const IrFunction *function = &module->functions[function_index];
    if (fprintf(output, "function #%zu name=", function_index) < 0 ||
        !print_token(output, function->source_program, function->name_token) ||
        fprintf(output, " symbol=%zu owner=", function->symbol_id) < 0 ||
        !print_token(output, function->source_program, function->owner_token) ||
        fprintf(output, " return=@%zu parameters=%zu instructions=%zu\n",
                function->return_type_id, function->parameter_count,
                function->instruction_count) < 0)
        return 0;

    for (size_t p = 0; p < function->parameter_count; p++) {
        const IrParameter *parameter = &function->parameters[p];
        if (fprintf(output, "  parameter #%zu name=", p) < 0 ||
            !print_token(output, parameter->source_program, parameter->name_token) ||
            fprintf(output, " symbol=%zu type=@%zu receiver=%d\n", parameter->symbol_id,
                    parameter->type_id, parameter->is_receiver) < 0)
            return 0;
    }

    for (size_t i = 0; i < function->instruction_count; i++)
        if (!dump_instruction(output, function, &function->instructions[i], i)) return 0;

    return !ferror(output);
}
