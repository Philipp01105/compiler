#include "asm_optimizer.h"
#include "backend.h"
#include "errorHandler.h"
#include "ir_emitter.h"
#include "native/linker.h"
int x86_64_lower_native(const IrModule *m, TargetFormat t, NativeObject *o, FILE *f) {
    (void)m;(void)t;(void)o;(void)f;return 0;
}
int native_write_object(NativeObject *o,TargetFormat t,NativeBuffer *b){(void)o;(void)t;(void)b;return 0;}
int native_link_executable(NativeObject *o,TargetFormat t,NativeBuffer *b){(void)o;(void)t;(void)b;return 0;}
void native_object_free(NativeObject *o){(void)o;}
void native_error(NativeObject *o,const char *s){(void)o;(void)s;}
int native_runtime_emit(NativeObject *o,TargetFormat t){(void)o;(void)t;return 0;}
int native_runtime_object_imports(NativeObject *o,TargetFormat t){(void)o;(void)t;return 0;}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Keep the real backend diagnostic path; use the prepared file instead of emission. */
int x86_64_emit_ir_file(const IrModule *module, TargetFormat target,
                        SyntaxMode syntax, int deterministic,
                        const char *output_path, const char *source_map_path) {
    (void) module; (void) target; (void) syntax; (void) deterministic;
    (void) output_path; (void) source_map_path;
    return 1;
}

static int diagnostic_case(const char *path, int json, int flush, int close, int replace,
                           const char *operation, int number) {
    FILE *capture = tmpfile();
    ErrorHandler *handler = error_handler_init();
    if (capture == NULL || handler == NULL) {
        if (capture != NULL) fclose(capture);
        error_handler_free(handler);
        return 0;
    }
    handler->output_stream = capture;
    error_handler_set_colors(handler, 0);
    error_handler_set_json_output(handler, json);
    error_handler_set_global(handler);
    AstProgram program = {.source_path = "optimizer-test.dmm"};
    IrModule module = {.program = &program};
    BackendOptions options = {0};
    assembly_cleanup_test_fail(flush, close, replace);
    int result = backend_emit_file(&module, &options, path);
    error_handler_flush(handler);
    rewind(capture);
    char text[16384] = {0};
    size_t count = fread(text, 1, sizeof(text) - 1, capture);
    int valid = result == 0 && count != 0 && strstr(text, "G103") != NULL &&
        strstr(text, operation) != NULL && strstr(text, strerror(number)) != NULL &&
        strstr(text, replace ? "optimizer_test_input.s" : ".tmp.") != NULL;
    if (json && strstr(text, "\"errorCode\": \"G103\"") == NULL) valid = 0;
    error_handler_free(handler);
    error_handler_set_global(NULL);
    fclose(capture);
    return valid;
}

static int failure_case(const char *path, int flush, int close, int replace,
                        const char *operation, int number) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    fputs("    push rax\n    pop rax\n    ret\n", file);
    if (fclose(file) != 0) return 0;
    assembly_cleanup_test_fail(flush, close, replace);
    AssemblyCleanupError error;
    if (cleanup_assembly_file_detailed(path, &error) != -1 ||
        error.error_number != number || errno != number ||
        strcmp(error.operation, operation) != 0 ||
        assembly_cleanup_test_close_count() != 1) return 0;
    if (replace ? strcmp(error.path, path) != 0 : strstr(error.path, ".tmp.") == NULL) return 0;
    if (!replace) {
        file = fopen(error.path, "rb");
        if (file != NULL) { fclose(file); return 0; }
    }
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    char original[128] = {0};
    size_t count = fread(original, 1, sizeof(original) - 1, file);
    fclose(file);
    return count != 0 && strcmp(original, "    push rax\n    pop rax\n    ret\n") == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!failure_case(argv[1], 1, 0, 0, "flush temporary output", ENOSPC) ||
        !failure_case(argv[1], 0, 1, 0, "close temporary output", EIO) ||
        !failure_case(argv[1], 0, 0, 1, "replace assembly output", EACCES) ||
        !failure_case(argv[1], 1, 1, 0, "flush temporary output", ENOSPC)) return 7;
    for (int json = 0; json <= 1; json++) {
        if (!diagnostic_case(argv[1], json, 1, 0, 0, "flush temporary output", ENOSPC) ||
            !diagnostic_case(argv[1], json, 0, 1, 0, "close temporary output", EIO) ||
            !diagnostic_case(argv[1], json, 0, 0, 1, "replace assembly output", EACCES)) return 8;
    }
    assembly_cleanup_test_fail(0, 0, 0);
    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) return 3;
    for (int i = 0; i < 12050; ++i) fprintf(file, "label_%d:\n", i);
    fputs("    pushq %rax\n    popq %rax\n    push rbx\n    pop rbx\n    ret\n", file);
    if (fclose(file) != 0 || cleanup_assembly_file(argv[1]) != 0) return 4;

    file = fopen(argv[1], "rb");
    if (file == NULL) return 5;
    char line[128];
    int count = 0;
    int saw_push = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        ++count;
        if (strstr(line, "pushq") != NULL || strstr(line, "popq") != NULL) saw_push = 1;
    }
    fclose(file);
    remove(argv[1]);
    return count == 12051 && !saw_push ? 0 : 6;
}
/* Category: unit/backend optimizer. */
