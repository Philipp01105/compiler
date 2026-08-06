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

static int output_error(const AstProgram *program, const char *message,
                        const char *path) {
    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                 ERR_CODEGEN_OUTPUT_FAILED, program->source_path, message, path);
    return 0;
}

static int component_dependencies(const IrModule *module, NativeObject *object) {
    if (!module->program->runtime_component) return 1;
    for (size_t s = 0; s < object->symbol_count; ++s) {
        const NativeSymbol *symbol = &object->symbols[s];
        if (symbol->defined || !strcmp(symbol->name, "___chkstk_ms")) continue;
        int declared = 0;
        for (size_t n = 0; n < module->native_import_count; ++n)
            if (!strcmp(symbol->name, module->native_imports[n].native_name) &&
                ir_native_import_used(module, module->native_imports[n].symbol_id)) declared = 1;
        if (!declared) {
            char message[256];
            snprintf(message, sizeof(message), "Runtime component dependency requires an explicit native declaration: %s", symbol->name);
            native_error(object, message);
            return 0;
        }
    }
    return 1;
}

static int component_contract(const IrModule *module, const char *path) {
    if (!module->program->runtime_component) return 1;
    if (!module->program->package_name || !strcmp(module->program->package_name, "main"))
        return output_error(module->program, "Runtime component requires a library package: '%s'", path);
    for (size_t g = 0; g < module->global_count; ++g)
        if (module->globals[g].runtime_initializer ||
            (ir_type_properties(module, module->globals[g].type_id) & SEMANTIC_TYPE_NEEDS_DROP))
            return output_error(module->program, "Runtime component cannot require global initialization or cleanup: '%s'", path);
    for (size_t f = 0; f < module->function_count; ++f) {
        const IrFunction *fn = &module->functions[f];
        if (module->emission_selected && !fn->emission_reachable) continue;
        if (fn->is_async || fn->is_package_init || fn->is_package_cleanup || fn->is_drop_glue)
            return output_error(module->program, "Runtime component cannot require async, package lifecycle or drop glue: '%s'", path);
    }
    return 1;
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
    if (success) success = component_dependencies(module, &object);
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
    if (success && !module->program->runtime_component && options->emission == BACKEND_OBJECT &&
        (!module->program->package_name || !strcmp(module->program->package_name, "main")))
        success = native_runtime_emit_requirements(&object, options->target_format,
                                               options->runtime_profile, ir_main_returns_void(module), ir_runtime_requirements(module));
    if (success && !module->program->runtime_component && options->emission == BACKEND_OBJECT)
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
    if (!component_contract(module, output_path)) return 0;

    if (options->runtime_profile == RUNTIME_PLATFORM && options->emission == BACKEND_EXECUTABLE)
        return output_error(module->program, "Platform executable requires driver link handoff: '%s'", output_path);
    IrModule emission_module = *module;
    emission_module.runtime_profile = options->runtime_profile;
    module = &emission_module;
    if (options->emission != BACKEND_ASSEMBLY) return emit_native(module, options, output_path);
    if (module->program->runtime_component) {
        NativeObject validation = {0};
        int valid = x86_64_lower_native(module, options->target_format, &validation, NULL) &&
                    component_dependencies(module, &validation);
        if (!valid) output_error(module->program, "Invalid runtime component dependency: '%s'", validation.error);
        native_object_free(&validation);
        if (!valid) return 0;
    }
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
