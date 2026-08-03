#include "backend.h"

#include "asm_optimizer.h"
#include "errorHandler.h"
#include "ir_emitter.h"
#include "native/linker.h"
#include "native_runtime.h"
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <stdio.h>

static int native_type_marked(const IrModule *module, const unsigned char *marked, IrTypeId id) {
    return id < module->type_count && marked[id];
}

/* Propagate through the finite type graph, including recursive pointer cycles.
   Each type changes from zero to one at most once. */
static unsigned char *native_storage_types(const IrModule *module) {
    unsigned char *marked = calloc(module->type_count ? module->type_count : 1, 1);
    if (!marked) return NULL;
    int changed;
    do {
        changed = 0;
        for (size_t t = 0; t < module->type_count; t++) {
            if (marked[t]) continue;
            const IrType *type = &module->types[t];
            int native = 0;
            if (type->kind == IR_TYPE_POINTER || type->kind == IR_TYPE_ARRAY || type->kind == IR_TYPE_SLICE ||
                type->kind == IR_TYPE_FUTURE || type->kind == IR_TYPE_JOIN)
                native = native_type_marked(module, marked, type->element_type);
            if (type->kind == IR_TYPE_FUNCTION && type->signature_id < module->signature_count) {
                const IrFunctionSignature *signature = &module->signatures[type->signature_id];
                native |= native_type_marked(module, marked, signature->return_type);
                for (size_t p = 0; p < signature->parameter_count; p++)
                    native |= native_type_marked(module, marked, signature->parameter_types[p]);
            }
            if (type->kind == IR_TYPE_NAMED) {
                for (size_t s = 0; s < module->structure_count; s++) {
                    const IrAggregate *structure = &module->structures[s];
                    if (structure->symbol_id != type->symbol_id) continue;
                    native |= structure->is_native;
                    for (size_t f = 0; f < structure->field_count; f++)
                        native |= native_type_marked(module, marked, structure->fields[f].type_id);
                }
                for (size_t e = 0; e < module->enum_count; e++) {
                    const IrEnum *enumeration = &module->enums[e];
                    if (enumeration->symbol_id != type->symbol_id) continue;
                    for (size_t f = 0; f < enumeration->field_count; f++)
                        native |= native_type_marked(module, marked, enumeration->fields[f].type_id);
                    for (size_t v = 0; v < enumeration->variant_count; v++)
                        for (size_t p = 0; p < enumeration->variants[v].payload_count; p++)
                            native |= native_type_marked(module, marked, enumeration->variants[v].payload_types[p]);
                }
            }
            if (native) { marked[t] = 1; changed = 1; }
        }
    } while (changed);
    return marked;
}
/* Stage 1 makes native declarations analyzable. Do not route native storage or
   calls through the existing DMM aggregate ABI while stage 2 is pending. */
static int validate_native_emission(const IrModule *module) {
    int has_native = module->native_import_count != 0;
    for (size_t s = 0; s < module->structure_count; s++) has_native |= module->structures[s].is_native;
    if (!has_native) return 1;
    unsigned char *marked = native_storage_types(module);
    if (!marked) return 0;
    for (size_t g = 0; g < module->global_count; g++)
        if (native_type_marked(module, marked, module->globals[g].type_id)) {
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                         ERR_CODEGEN_OUTPUT_FAILED, module->globals[g].source_program->source_path,
                         "Native values in executable global storage require FFI stage 2");
            free(marked);
            return 0;
        }
    for (size_t f = 0; f < module->function_count; f++) {
        const IrFunction *function = &module->functions[f];
        if (module->emission_selected && !function->emission_reachable) continue;
        int native_signature = native_type_marked(module, marked, function->return_type_id);
        for (size_t p = 0; p < function->parameter_count; p++)
            native_signature |= native_type_marked(module, marked, function->parameters[p].type_id);
        if (native_signature) {
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                         ERR_CODEGEN_OUTPUT_FAILED, function->source_program->source_path,
                         "Native values in executable function signatures require FFI stage 2");
            free(marked);
            return 0;
        }
        for (size_t i = 0; i < function->instruction_count; i++) {
            const IrInstruction *in = &function->instructions[i];
            int native_import = 0;
            for (size_t n = 0; n < module->native_import_count; n++)
                if (module->native_imports[n].symbol_id == in->symbol_id) native_import = 1;
            if (native_import || native_type_marked(module, marked, in->type_id)) {
                error_report(global_error_handler, SEVERITY_ERROR, in->span.begin.line, in->span.begin.column,
                             ERROR_CATEGORY_CODEGEN, ERR_CODEGEN_OUTPUT_FAILED,
                             in->source_program ? in->source_program->source_path : function->source_program->source_path,
                             "Native declarations are supported; native calls and byte-exact storage require FFI stage 2");
                free(marked);
                return 0;
            }
        }
    }
    free(marked);
    return 1;
}

static int output_error(const AstProgram *program, const char *message,
                        const char *path) {
    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                 ERR_CODEGEN_OUTPUT_FAILED, program->source_path, message, path);
    return 0;
}

static int emit_native(const IrModule *module, const BackendOptions *options, const char *path) {
    NativeObject object = {0};
    NativeBuffer output = {0};
    FILE *map = NULL;
    int map_opened = 0, opened = 0, io_errno = 0;
    const char *io_path = path, *operation = NULL;
    int success = 1;
    if (options->source_map_path != NULL) {
        map = fopen(options->source_map_path, "w");
        map_opened = map != NULL;
        if (map == NULL) {
            io_errno = errno;
            io_path = options->source_map_path;
            operation = "open source-map";
            success = 0;
        }
    }
    if (success) success = x86_64_lower_native(module, options->target_format, &object, map);
    if (map != NULL) {
        if (ferror(map)) {
            io_errno = errno ? errno : EIO;
            io_path = options->source_map_path;
            operation = "write source-map";
            success = 0;
        }
        if (fclose(map) != 0 && operation == NULL) {
            io_errno = errno;
            io_path = options->source_map_path;
            operation = "close source-map";
            success = 0;
        }
    }
    if (success && options->emission == BACKEND_OBJECT &&
        (!module->program->package_name || !strcmp(module->program->package_name, "main")))
        success = native_runtime_emit_requirements(&object, options->target_format,
                                               options->runtime_profile, ir_main_returns_void(module), ir_runtime_requirements(module));
    if (success && options->emission == BACKEND_OBJECT)
        success = native_runtime_object_imports_profile(&object, options->target_format, options->runtime_profile);
    if (success)
        success = options->emission == BACKEND_OBJECT
                      ? native_write_object(&object, options->target_format, &output)
                      : native_link_executable(&object, options->target_format, &output);
    if (success) {
        FILE *file = fopen(path, "wb");
        if (file == NULL) {
            io_errno = errno;
            operation = "open native";
            success = 0;
        } else {
            opened = 1;
            if (fwrite(output.data, 1, output.size, file) != output.size) {
                io_errno = errno ? errno : EIO;
                operation = "write native";
                success = 0;
            }
            if (fclose(file) != 0 && operation == NULL) {
                io_errno = errno;
                operation = "close native";
                success = 0;
            }
        }
    }
#ifndef _WIN32
    if (success && options->emission == BACKEND_EXECUTABLE && options->target_format == TARGET_ELF) {
        struct stat status;
        if (stat(path, &status) != 0 || chmod(path, status.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) != 0) {
            io_errno = errno;
            operation = "set executable permissions on";
            success = 0;
        }
    }
#endif
    if (!success) {
        if (opened) (void) remove(path);
        if (map_opened) (void) remove(options->source_map_path);
        if (operation != NULL)
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                         ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                         "Could not %s output '%s': %s", operation, io_path, strerror(io_errno ? io_errno : EIO));
        else
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                         ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                         "Could not emit native output '%s': %s", path,
                         object.failed ? object.error : "internal backend failure");
    } else if (options->debug)
        printf("  [+] Native %s generated: %s\n",
               options->emission == BACKEND_OBJECT ? "object" : "executable", path);
    native_object_free(&object);
    free(output.data);
    return success;
}

int backend_emit_file(const IrModule *module, const BackendOptions *options,
                      const char *output_path) {
    if (module == NULL || module->program == NULL || options == NULL || output_path == NULL)
        return 0;
    if (!validate_native_emission(module)) return 0;
    if (options->runtime_profile == RUNTIME_PLATFORM && options->emission == BACKEND_EXECUTABLE)
        return output_error(module->program, "Platform executable requires driver link handoff: '%s'", output_path);
    IrModule emission_module = *module;
    emission_module.runtime_profile = options->runtime_profile;
    module = &emission_module;
    if (options->emission != BACKEND_ASSEMBLY) return emit_native(module, options, output_path);
    int previous_errors = error_handler_get_error_count(global_error_handler);
    if (!x86_64_emit_ir_file(module, options->target_format, options->syntax_mode,
                             options->deterministic, output_path, options->source_map_path))
        return error_handler_get_error_count(global_error_handler) > previous_errors
                   ? 0
                   : output_error(module->program,
                                  "Could not emit typed IR output '%s' (internal backend failure after semantic analysis)",
                                  output_path);
    AssemblyCleanupError cleanup_error;
    if (cleanup_assembly_file_detailed(output_path, &cleanup_error) != 0) {
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                     ERR_CODEGEN_OUTPUT_FAILED, module->program->source_path,
                     "Assembly cleanup failed to %s '%s': %s", cleanup_error.operation,
                     cleanup_error.path, cleanup_error.reason);
        return 0;
    }
    if (options->debug)
        printf("  [+] Assembly code generated from typed IR: %s\n", output_path);
    return 1;
}
