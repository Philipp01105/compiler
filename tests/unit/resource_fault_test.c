#include "test_source.h"
#include "backend.h"
#include "errorHandler.h"
#include "resource_fault.h"
#include <string.h>
#ifdef _WIN32
#define TokenType WindowsTokenType
#include <windows.h>
#undef TokenType
#endif

static int emit_case(const IrModule *module, BackendOptions *options, const char *path,
                      size_t allocation, size_t io) {
    dmm_fault_reset(allocation, io);
    int result = backend_emit_file(module, options, path);
    int triggered = dmm_fault_triggered();
    dmm_fault_reset(0, 0);
    if (triggered && result) {
        fprintf(stderr, "Emission accepted resource failure allocation=%zu io=%zu\n", allocation, io);
        return 0;
    }
    if (triggered && io != 0) {
        FILE *file = fopen(path, "rb");
        if (file != NULL) { fclose(file); fprintf(stderr, "Partial output retained\n"); return 0; }
        if (options->source_map_path) {
            file=fopen(options->source_map_path,"rb");
            if (file) { fclose(file); fprintf(stderr,"Partial source map retained\n"); return 0; }
        }
    }
    return 1;
}
static int pipeline_fault_case(const char *source,size_t nth,size_t *count) {
    dmm_fault_reset(nth,0);
    AstProgram *program=test_parse_source(source,strlen(source),"pipeline-fault.dmm",NULL);
    SemanticModel *model=program ? semantic_analyze(program) : NULL;
    IrModule *module=model && model->error_count == 0 ? ir_lower_program(program,model) : NULL;
    int success=module != NULL, triggered=dmm_fault_triggered();
    if (count) *count=dmm_fault_allocations();
    dmm_fault_reset(0,0);
    ir_module_free(module); semantic_model_free(model); ast_program_free(program);
    if (nth == 0) return success;
    if (triggered && success) { fprintf(stderr,"Pipeline accepted allocation fault %zu\n",nth); return 0; }
    return 1;
}
int main(int argc, char **argv) {
    if (argc != 2) return 1;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    ErrorHandler *handler = error_handler_init();
    FILE *capture = tmpfile();
    if (handler == NULL || capture == NULL) return 1;
    handler->output_stream = capture;
    error_handler_set_global(handler);
    const char *source = "func add(a:int, b:int) -> int { return a+b; } func main() -> int { return add(2,3); }";
    AstProgram *program = test_parse_source(source, strlen(source), "fault.dmm", NULL);
    SemanticModel *model = semantic_analyze(program);
    IrModule *module = ir_lower_program(program, model);
    int valid = module != NULL;
    char map_path[4096];
    if (snprintf(map_path,sizeof(map_path),"%s.map",argv[1]) >= (int)sizeof(map_path)) return 1;
    const char *generic_source="enum O<T> { Some(T), None, } func id<T>(x:T) -> T { return x; } "
        "func main() -> int { var o:O<int> = O<int>.Some(id(3)); match(o) { Some(v) => return v; None => return 0; } }";
    size_t pipeline_allocations=0;
    valid=valid && pipeline_fault_case(generic_source,0,&pipeline_allocations);
    for (size_t n=1; valid && n<=pipeline_allocations; n++) {
        valid=pipeline_fault_case(generic_source,n,NULL); error_handler_flush(handler);
    }

    for (int target = TARGET_ELF; valid && target <= TARGET_COFF; target++) {
        for (int mode = BACKEND_OBJECT; valid && mode <= BACKEND_EXECUTABLE; mode++) {
            BackendOptions options = {.target_format = (TargetFormat)target, .emission = (BackendEmission)mode};
            options.source_map_path=map_path;
            dmm_fault_reset(0, 0);
            valid = backend_emit_file(module, &options, argv[1]);
            size_t allocations = dmm_fault_allocations(), io_calls = dmm_fault_io_calls();
            for (size_t n = 1; valid && n <= allocations; n++) {
                valid = emit_case(module, &options, argv[1], n, 0);
                error_handler_flush(handler);
            }
            for (size_t n = 1; valid && n <= io_calls; n++) {
                remove(argv[1]);
                remove(map_path);
                valid = emit_case(module, &options, argv[1], 0, n);
                error_handler_flush(handler);
            }
        }
    }
    dmm_fault_reset(0, 0);
    remove(argv[1]);
    remove(map_path);
    ir_module_free(module); semantic_model_free(model); ast_program_free(program);
    error_handler_free(handler); error_handler_set_global(NULL); fclose(capture);
    return valid ? 0 : 1;
}
