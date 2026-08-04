#include "ir_emitter_internal.h"
#include "ir_names.h"
#include "runtime_calls.h"
#include "native_runtime.h"
#include "errorHandler.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int map_quoted(FILE *output, const char *text) {
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

static int write_sized_bits(Emitter *emitter, uint64_t bits, size_t size) {
    if (size != 1 && size != 2 && size != 4 && size != 8) return 0;
    if (emitter->native) return native_uint(emitter->native, bits, size);
    const char *directive = size == 1 ? ".byte" : size == 2 ? ".short" :
                            size == 4 ? ".long" : ".quad";
    return fprintf(emitter->output, "    %s 0x%llx\n", directive,
                   (unsigned long long) bits) >= 0;
}

static int write_zero_bytes(Emitter *emitter, size_t count) {
    while (count != 0) {
        size_t width = count >= 8 ? 8 : count >= 4 ? 4 : count >= 2 ? 2 : 1;
        if (!write_sized_bits(emitter, 0, width)) return 0;
        count -= width;
    }
    return 1;
}

static int emit_global_literal_value(Emitter *emitter,
                                     const AstProgram *program,
                                     const AstExpression *value,
                                     IrTypeId type_id,
                                     size_t *written) {
    if (value == NULL || type_id >= emitter->module->type_count) return 0;
    const IrType *type = &emitter->module->types[type_id];
    if (type->kind == IR_TYPE_ARRAY || type->kind == IR_TYPE_SLICE) {
        if (value->kind != AST_EXPR_ARRAY_LITERAL ||
            type->element_type >= emitter->module->type_count)
            return 0;
        const IrType *element_type =
            &emitter->module->types[type->element_type];
        if (element_type->kind == IR_TYPE_SLICE) return 0;
        IrTypeLayout element_layout;
        if (!ir_type_layout(emitter->module, type->element_type,
                            &element_layout))
            return 0;
        size_t pattern_count = 0;
        for (const AstExpression *element = value->arguments;
             element != NULL; element = element->next)
            pattern_count++;
        if (pattern_count == 0) return 0;
        size_t count = value->literal_element_count;
        for (size_t index = 0; index < count; index++) {
            const AstExpression *element = value->arguments;
            for (size_t pattern = index % pattern_count; pattern > 0;
                 pattern--)
                element = element->next;
            size_t element_written = 0;
            if (!emit_global_literal_value(emitter, program, element,
                                           type->element_type,
                                           &element_written) ||
                element_written > element_layout.size ||
                !write_zero_bytes(emitter,
                                  element_layout.size - element_written))
                return 0;
        }
        *written = count * element_layout.size;
        return 1;
    }
    if (type->kind != IR_TYPE_PRIMITIVE) return 0;
    IrTypeLayout layout;
    if (!ir_type_layout(emitter->module, type_id, &layout)) return 0;
    const char *text = value->folded_constant.lexeme != NULL
                           ? value->folded_constant.lexeme
                           : ast_program_lexeme(program, value->value_token);
    uint64_t bits;
    if (type->primitive == TYPE_DOUBLE) {
        union { double value; uint64_t bits; } converted = {strtod(text, NULL)};
        bits = converted.bits;
    } else if (type->primitive == TYPE_FLOAT) {
        union { float value; uint32_t bits; } converted = {(float) strtod(text, NULL)};
        bits = converted.bits;
    } else if (type->primitive == TYPE_STRING) {
        return 0;
    } else {
        IrInstruction literal = {.auxiliary_token = value->value_token};
        bits = (uint64_t) constant_value(program, &literal);
        if (value->folded_constant.lexeme != NULL)
            bits = (uint64_t) strtoll(text, NULL, 0);
    }
    if (!write_sized_bits(emitter, bits, layout.size)) return 0;
    *written = layout.size;
    return 1;
}

static int emit_global_literal_elements(Emitter *emitter,
                                        const IrGlobal *global) {
    if (global->array_literal == NULL ||
        global->type_id >= emitter->module->type_count) return 0;
    const IrType *container = &emitter->module->types[global->type_id];
    if ((container->kind != IR_TYPE_ARRAY && container->kind != IR_TYPE_SLICE) ||
        container->element_type >= emitter->module->type_count) return 0;
    size_t written = 0;
    return emit_global_literal_value(emitter, global->source_program,
                                     global->array_literal, global->type_id,
                                     &written);
}


static int emit_file(Emitter *emitter, int deterministic) {
    FILE *output = emitter->output;
    if (!emitter->native) {
        fputs("# Generated by Philipp01105's Compiler\n", output);
        fprintf(output, "# Source: %s\n", emitter->module->program->source_path);
        if (!deterministic) {
            time_t now = time(NULL);
            struct tm *local = localtime(&now);
            char datetime[64] = "unknown";
            if (local != NULL)
                (void) strftime(datetime, sizeof(datetime),
                                "%Y-%m-%d %H:%M:%S", local);
            fprintf(output, "# Date: %s\n", datetime);
        }
        fputs("# Lowering: typed IR\n", output);
        fprintf(output, "# Target Format: %s\n# Syntax: %s\n",
                emitter->target == TARGET_COFF ? "COFF" : "ELF",
                emitter->syntax == SYNTAX_INTEL ? "Intel" : "AT&T");
        if (emitter->syntax == SYNTAX_INTEL) fputs("    .intel_syntax noprefix\n", output);
        if (emitter->target == TARGET_COFF) {
            fputs("    .section .rdata,\"dr\"\n", output);
        } else {
            fputs("    .section .rodata\n", output);
        }
        if (emitter->target == TARGET_COFF)
            for (size_t r = 0; r < runtime_call_count(); r++) {
                const RuntimeCall *runtime = runtime_call_at(r);
                int duplicate = 0;
                for (size_t previous = 0; previous < r; previous++)
                    if (strcmp(runtime_call_at(previous)->link_name, runtime->link_name) == 0) duplicate = 1;
                if (!duplicate)
                    fprintf(output, "    .def %s; .scl 2; .type 32; .endef\n", runtime->link_name);
            }
    } else emitter->native->section = NATIVE_RODATA;
    write_labelf(emitter, ".LIR_empty_string_0:\n");
    write_cstring(emitter, "");
    write_labelf(emitter, ".LIR_int_format_0:\n");
    write_cstring(emitter, "%lld");
    write_labelf(emitter, ".LIR_uint_format_0:\n");
    write_cstring(emitter, "%u");
    write_labelf(emitter, ".LIR_char_format_0:\n");
    write_cstring(emitter, "%c");
    write_labelf(emitter, ".LIR_float_format_0:\n");
    write_cstring(emitter, "%f");
    write_labelf(emitter, ".LIR_string_format_0:\n");
    write_cstring(emitter, "%s");
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *instruction = &function->instructions[i];
            if (instruction->opcode == IR_OP_CONSTANT && instruction->type == TYPE_STRING) {
                write_labelf(emitter, ".LIR_string_%zu_%zu:\n", f,
                             instruction->result);
                const AstProgram *program = instruction->source_program != NULL
                                                ? instruction->source_program
                                                : function->source_program;
                write_cstring(emitter, ast_program_lexeme(program,
                                                          instruction->auxiliary_token));
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
            write_labelf(emitter, ".LIR_enum_%zu_%zu:\n", e, a);
            write_cstring(emitter, ast_program_lexeme(enumeration->source_program,
                                                      argument->token));
        }
        for (size_t field = 0; field < enumeration->field_count; field++) {
            write_labelf(emitter, ".LIR_enum_field_%zu_%zu:\n", e, field);
            if (enumeration->fields[field].type_id >= emitter->module->type_count) return 0;
            const IrType *field_type =
                    &emitter->module->types[enumeration->fields[field].type_id];
            if (field_type->kind != IR_TYPE_PRIMITIVE) return 0;
            for (size_t variant_index = 0;
                 variant_index < enumeration->variant_count; variant_index++) {
                const IrEnumVariant *variant = &enumeration->variants[variant_index];
                size_t argument_index = variant->first_argument + field;
                if (field >= variant->argument_count ||
                    argument_index >= enumeration->variant_argument_count)
                    return 0;
                const IrEnumArgument *argument =
                        &enumeration->variant_arguments[argument_index];
                if (field_type->primitive == TYPE_STRING) {
                    if (emitter->native) {
                        char symbol[128];
                        snprintf(symbol, sizeof(symbol), ".LIR_enum_%zu_%zu", e, argument_index);
                        (void) native_reference(emitter->native, symbol, NATIVE_ADDR64,
                                                emitter->native->sections[NATIVE_RODATA].size, 0);
                        write_quad(emitter, 0);
                    } else fprintf(output, "    .quad .LIR_enum_%zu_%zu\n", e, argument_index);
                } else if (field_type->primitive == TYPE_DOUBLE) {
                    union {
                        double value;
                        uint64_t bits;
                    } converted = {
                        strtod(ast_program_lexeme(enumeration->source_program,
                                                  argument->token), NULL)
                    };
                    if (argument->negative) converted.value = -converted.value;
                    write_quad(emitter, converted.bits);
                } else if (field_type->primitive == TYPE_FLOAT) {
                    union {
                        float value;
                        uint32_t bits;
                    } converted = {
                        (float) strtod(ast_program_lexeme(enumeration->source_program,
                                                          argument->token), NULL)
                    };
                    if (argument->negative) converted.value = -converted.value;
                    write_quad(emitter, converted.bits);
                } else {
                    IrInstruction literal = {.auxiliary_token = argument->token};
                    long long value = constant_value(enumeration->source_program, &literal);
                    if (argument->negative) value = -value;
                    write_quad(emitter, (uint64_t) value);
                }
            }
        }
    }
    for (size_t g = 0; g < emitter->module->global_count; g++)
        if (emitter->module->globals[g].string) {
            write_labelf(emitter, ".LIR_global_string_%zu:\n", g);
            write_cstring(emitter, emitter->module->globals[g].string);
        }
    if (emitter->native) emitter->native->section = NATIVE_DATA;
    else fputs("    .data\n    .balign 8\n", output);
    for (size_t g = 0; g < emitter->module->global_count; g++) {
        const IrGlobal *global = &emitter->module->globals[g];
        const SemanticSymbol *symbol = &emitter->module->semantics->symbols[global->symbol_id];
        char label[4096];
        if (!global_label(symbol, label, sizeof(label))) return 0;
        const IrType *global_type = global->type_id < emitter->module->type_count
                                        ? &emitter->module->types[global->type_id]
                                        : NULL;
        if (global->array_literal != NULL && global_type != NULL &&
            global_type->kind == IR_TYPE_SLICE) {
            char backing[64];
            snprintf(backing, sizeof(backing), ".LIR_global_backing_%zu", g);
            if (emitter->native) {
                if (!native_define(emitter->native, backing, 0, 0)) return 0;
            } else fprintf(output, "%s:\n", backing);
            if (!emit_global_literal_elements(emitter, global)) return 0;
        }
        if (emitter->native) {
            if (!native_define(emitter->native, label, symbol->declaration->is_public, 0)) return 0;
        } else {
            if (symbol->declaration->is_public) fprintf(output, "    .globl %s\n", label);
            fprintf(output, "%s:\n", label);
        }
        if (global->array_literal != NULL && global_type != NULL &&
            global_type->kind == IR_TYPE_SLICE) {
            char backing[64];
            snprintf(backing, sizeof(backing), ".LIR_global_backing_%zu", g);
            if (emitter->native) {
                native_reference(emitter->native, backing, NATIVE_ADDR64,
                                 emitter->native->sections[NATIVE_DATA].size, 0);
                write_quad(emitter, 0);
            } else fprintf(output, "    .quad %s\n", backing);
            write_quad(emitter, global->literal_element_count);
        } else if (global->array_literal != NULL) {
            if (!emit_global_literal_elements(emitter, global)) return 0;
        } else if (global->function_symbol_id != AST_SYMBOL_NONE) {
            const IrFunction *addressed = addressed_function(emitter->module,
                                                           global->function_symbol_id);
            if (addressed == NULL) return 0;
            char function_buffer[4096];
            const char *function_name = function_link_name(emitter->module, addressed,
                                                           function_buffer,
                                                           sizeof(function_buffer));
            if (function_name == NULL) return 0;
            if (emitter->native) {
                native_reference(emitter->native, function_name, NATIVE_ADDR64,
                                 emitter->native->sections[NATIVE_DATA].size, 0);
                write_quad(emitter, 0);
            } else fprintf(output, "    .quad %s\n", function_name);
        } else if (global->string) {
            if (emitter->native) {
                char string[64];
                snprintf(string, sizeof(string), ".LIR_global_string_%zu", g);
                native_reference(emitter->native, string, NATIVE_ADDR64, emitter->native->sections[NATIVE_DATA].size,
                                 0);
                write_quad(emitter, 0);
            } else fprintf(output, "    .quad .LIR_global_string_%zu\n", g);
        } else {
            size_t slots = type_slots(emitter->module, global->type_id);
            if (!slots || slots > 1048576) return 0;
            write_quad(emitter, global->bits);
            for (size_t slot = 1; slot < slots; slot++) write_quad(emitter, 0);
        }
        if (ir_type_properties(emitter->module, global->type_id) &
            SEMANTIC_TYPE_NEEDS_DROP) {
            char flag[4096];
            if (!global_drop_flag_label(symbol, flag, sizeof(flag))) return 0;
            if (emitter->native) {
                if (!native_define(emitter->native, flag, 0, 0)) return 0;
            } else fprintf(output, "%s:\n", flag);
            write_quad(emitter, global->runtime_initializer == NULL ? 1 : 0);
        }
        if (global_type != NULL && global_type->kind == IR_TYPE_SLICE) {
            char owner[4096];
            if (!global_slice_owner_label(symbol, owner, sizeof(owner))) return 0;
            if (emitter->native) {
                if (!native_define(emitter->native, owner, 0, 0)) return 0;
            } else fprintf(output, "%s:\n", owner);
            write_quad(emitter, 0);
        }
    }
    if (emitter->native) emitter->native->section = NATIVE_TEXT;
    else fputs("    .text\n", output);
    for (size_t f = 0; f < emitter->module->function_count; f++) {
        const IrFunction *function = &emitter->module->functions[f];
        if(emitter->module->emission_selected && !function->emission_reachable) continue;
        size_t declarations = 0;
        size_t aggregate_results = 0;
        size_t aggregate_parameters = 0;
        for (size_t i = 0; i < function->instruction_count; i++)
            if (function->instructions[i].opcode == IR_OP_DECLARE) {
                size_t slots = type_slots(emitter->module, function->instructions[i].type_id);
                if (ir_type_properties(emitter->module,
                                       function->instructions[i].type_id) &
                    SEMANTIC_TYPE_NEEDS_DROP)
                    slots++;
                if (function->instructions[i].is_slice) slots++;
                if (slots == 0 || declarations > SIZE_MAX - slots) return 0;
                declarations += slots;
            } else if ((function->instructions[i].opcode == IR_OP_EXECUTOR || function->instructions[i].opcode == IR_OP_AWAIT || function->instructions[i].opcode == IR_OP_CALL || function->instructions[i].opcode ==
                        IR_OP_ENUM_CONSTRUCT || function->instructions[i].opcode == IR_OP_SLICE ||
                        function->instructions[i].opcode == IR_OP_SUBSLICE ||
                        function->instructions[i].opcode == IR_OP_ARRAY_LITERAL ||
                        function->instructions[i].opcode == IR_OP_INTERFACE_PACK ||
                        function->instructions[i].opcode == IR_OP_NATIVE_COPY) &&
                       (is_inline_structure(emitter->module,
                                            &function->instructions[i]) ||
                        function->instructions[i].is_array)) {
                size_t slots = type_slots(emitter->module,
                                          function->instructions[i].type_id);
                if (slots == 0 || aggregate_results > SIZE_MAX - slots) return 0;
                aggregate_results += slots;
            }
        for (size_t p = 0; p < function->parameter_count; p++) {
            if (!type_is_structure(emitter->module, function->parameters[p].type_id) &&
                !(function->is_async && emitter->module->types[function->parameters[p].type_id].kind == IR_TYPE_ARRAY)) continue;
            size_t parameter_slots = type_slots(emitter->module,
                                                function->parameters[p].type_id);
            if (parameter_slots == 0 || aggregate_parameters > SIZE_MAX - parameter_slots)
                return 0;
            aggregate_parameters += parameter_slots;
        }
        size_t parameter_slots = parameter_storage_slots(function);
        for (size_t p = 0; p < function->parameter_count; p++)
            if (!function->parameters[p].is_receiver &&
                (ir_type_properties(emitter->module,
                                    function->parameters[p].type_id) &
                 SEMANTIC_TYPE_NEEDS_DROP))
                parameter_slots++;
        if (function->next_value > SIZE_MAX - parameter_slots ||
            function->next_value + parameter_slots > SIZE_MAX - declarations)
            return 0;
        size_t slots = function->next_value + parameter_slots + declarations;
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
    if (output == NULL) {
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                     ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                     "Could not open assembly output '%s': %s", output_path, strerror(errno));
        return 0;
    }
    SourceMapWriter map = {0};
    if (source_map_path != NULL) {
        map.output = fopen(source_map_path, "w");
        if (map.output == NULL) {
            int saved_errno = errno;
            fclose(output);
            (void) remove(output_path);
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                         ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                         "Could not open source-map output '%s': %s", source_map_path, strerror(saved_errno));
            return 0;
        }
        if (fputs("dmm-source-map-v1\nsource path=", map.output) == EOF ||
            !map_quoted(map.output, module->program->source_path) ||
            fputs("\n", map.output) == EOF)
            map.failed = 1;
    }
    Emitter emitter = {
        .module = module,
        .target = target,
        .syntax = syntax,
        .output = output,
        .source_map = source_map_path == NULL ? NULL : &map
    };
    int success = emit_file(&emitter, deterministic);
    if (success && (!module->program->package_name || !strcmp(module->program->package_name, "main")))
        success = native_runtime_assembly_requirements(output, target, module->runtime_profile,
                                                  ir_main_returns_void(module),ir_runtime_requirements(module));
    int assembly_io_error = ferror(output);
    if (fclose(output) != 0) assembly_io_error = 1;
    int map_io_error = map.output != NULL && ferror(map.output);
    if (map.output != NULL && fclose(map.output) != 0) map_io_error = 1;
    int saved_errno = errno;
    if (assembly_io_error || map_io_error || map.failed) success = 0;
    if (!success) (void) remove(output_path);
    if (!success && source_map_path != NULL) (void) remove(source_map_path);
    if (assembly_io_error || map_io_error)
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                     ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                     "Could not write %s output '%s': %s", assembly_io_error ? "assembly" : "source-map",
                     assembly_io_error ? output_path : source_map_path, strerror(saved_errno == 0 ? EIO : saved_errno));
    return success;
}

int x86_64_lower_native(const IrModule *module, TargetFormat target, NativeObject *object, FILE *source_map) {
    if (!valid_module(module)) {
        native_error(object, "Invalid IR module");
        return 0;
    }
    SourceMapWriter map = {.output = source_map};
    if (source_map) fputs("dmm-native-map-v1\n", source_map);
    Emitter emitter = {
        .module = module, .target = target, .native = object,
        .source_map = source_map ? &map : NULL
    };
    int success = emit_file(&emitter, 1);
    return success && !object->failed && !map.failed;
}
