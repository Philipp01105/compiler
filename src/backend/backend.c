#include "backend.h"

#include "asm_optimizer.h"
#include "errorHandler.h"
#include "ir_emitter.h"

#include <stdio.h>

static int output_error(const AstProgram *program, const char *message,
                        const char *path) {
    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_CODEGEN,
                 ERR_CODEGEN_OUTPUT_FAILED, program->source_path, message, path);
    return 0;
}

int backend_emit_file(const IrModule *module, const BackendOptions *options,
                      const char *output_path) {
    if (module == NULL || module->program == NULL || options == NULL || output_path == NULL)
        return 0;
    if (!x86_64_ir_supports_module(module))
        return output_error(module->program,
                            "Typed IR module is not supported by the x86-64 backend: '%s'",
                            output_path);
    if (!x86_64_emit_ir_file(module, options->target_format, options->syntax_mode,
                             options->deterministic, output_path))
        return output_error(module->program, "Could not emit typed IR output '%s'", output_path);
    if (cleanup_assembly_file(output_path) != 0)
        return output_error(module->program, "Assembly cleanup pass failed for '%s'", output_path);
    if (options->debug)
        printf("  [+] Assembly code generated from typed IR: %s\n", output_path);
    return 1;
}
