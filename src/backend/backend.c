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

static int emit_native(const IrModule *module,const BackendOptions *options,const char *path) {
    NativeObject object={0}; NativeBuffer output={0};
    FILE *map=options->source_map_path?fopen(options->source_map_path,"w"):NULL;
    int map_opened=map!=NULL;
    int success=!options->source_map_path || map;
    if(!success) native_error(&object,"Could not open native source map");
    if(success) success=x86_64_lower_native(module,options->target_format,&object,map);
    if(map) {if(ferror(map))success=0;if(fclose(map)!=0)success=0;}
    if(success && options->emission==BACKEND_OBJECT)success=native_runtime_emit(&object,options->target_format);
    if(success && options->emission==BACKEND_OBJECT)success=native_runtime_object_imports(&object,options->target_format);
    if(success)success=options->emission==BACKEND_OBJECT?native_write_object(&object,options->target_format,&output):
        native_link_executable(&object,options->target_format,&output);
    int opened=0;
    if(success) {
        FILE *file=fopen(path,"wb");
        if(!file)success=0;
        else {opened=1;if(fwrite(output.data,1,output.size,file)!=output.size)success=0;if(fclose(file)!=0)success=0;}
    }
#ifndef _WIN32
    if(success && options->emission==BACKEND_EXECUTABLE && options->target_format==TARGET_ELF) {
        struct stat status;
        if(stat(path,&status)!=0 || chmod(path,status.st_mode|S_IXUSR|S_IXGRP|S_IXOTH)!=0)success=0;
    }
#endif
    if(!success) {
        if(opened)(void)remove(path);
        if(map_opened)(void)remove(options->source_map_path);
        error_report(global_error_handler,SEVERITY_ERROR,0,0,ERROR_CATEGORY_CODEGEN,ERR_CODEGEN_OUTPUT_FAILED,
            module->program->source_path,"Could not emit native output '%s': %s",path,
            object.failed?object.error:strerror(errno?errno:EIO));
    } else if(options->debug)printf("  [+] Native %s generated: %s\n",options->emission==BACKEND_OBJECT?"object":"executable",path);
    native_object_free(&object);free(output.data);return success;
}

int backend_emit_file(const IrModule *module, const BackendOptions *options,
                      const char *output_path) {
    if (module == NULL || module->program == NULL || options == NULL || output_path == NULL)
        return 0;
    if (options->emission != BACKEND_ASSEMBLY) return emit_native(module,options,output_path);
    int previous_errors = error_handler_get_error_count(global_error_handler);
    if (!x86_64_emit_ir_file(module, options->target_format, options->syntax_mode,
                             options->deterministic, output_path, options->source_map_path))
        return error_handler_get_error_count(global_error_handler) > previous_errors ? 0 :
            output_error(module->program, "Could not emit typed IR output '%s' (internal backend failure after semantic analysis)", output_path);
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
