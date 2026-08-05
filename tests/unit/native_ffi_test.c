#include "test_source.h"
#include "semantic.h"
#include "ir.h"
#include "errorHandler.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned char tag; unsigned int count; unsigned char bytes[3]; } Reference;
typedef struct { Reference values[2]; double weight; } NestedReference;

static int check_target(TargetFormat target) {
    const char *source =
        "const byteCount:usize = 3;"
        "extern \"system\" {"
        "pub struct Handle;"
        "pub struct Record { pub var tag:u8; pub var count:u32; pub var bytes:u8[byteCount]; }"
        "pub struct Nested { pub var values:Record[2]; pub var weight:double; }"
        "struct Recursive { var next:*Recursive; }"
        "}"
        "extern \"system\" from \"ffi_test\" {"
        "pub func transform(value:Nested, handle:*Handle) -> Nested = \"native_transform\";"
        "func close(handle:*Handle) -> void;"
        "}"
        "const recordSize:usize = sizeof(Record);"
        "const recordAlign:usize = alignof(Record);"
        "const nestedSize:usize = sizeof(Nested);"
        "func main() -> int { var value:Nested; var handle:*Handle;"
        "var result = transform(value, handle); close(handle); return 0; }";
    FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source), "ffi-test.dmm", &options);
    SemanticModel *semantics = semantic_analyze_target(program, target);
    IrModule *module = semantics && !semantics->error_count ? ir_lower_program(program, semantics) : NULL;
    int failed = !module || !ast_validate_program(program);
    if (module) {
        failed |= module->native_import_count != 2 || module->function_count != 3 ||
                  module->structure_count != 4 || module->target_format != target;
        for (size_t f = 0; f < module->function_count; f++)
            if (ir_native_import(module, module->functions[f].symbol_id)) failed = 1;
        IrAggregate *record = &module->structures[1];
        IrAggregate *nested = &module->structures[2];
        failed |= record->native_layout.size != sizeof(Reference) ||
                  record->native_layout.alignment != _Alignof(Reference) ||
                  record->fields[1].native_offset != offsetof(Reference, count) ||
                  record->fields[2].native_offset != offsetof(Reference, bytes) ||
                  record->fields[2].native_array_stride != 1 ||
                  nested->native_layout.size != sizeof(NestedReference) ||
                  nested->fields[0].native_array_stride != sizeof(Reference) ||
                  nested->fields[1].native_offset != offsetof(NestedReference, weight) ||
                  !(record->type_properties & SEMANTIC_TYPE_COPYABLE) ||
                  (record->type_properties & SEMANTIC_TYPE_NEEDS_DROP);
        for (const AstDeclarationNode *d = program->root; d; d = d->next) {
            if (d->kind == AST_DECL_STRUCT && d->native_library_token != AST_TOKEN_NONE) failed = 1;
            if (d->kind == AST_DECL_FUNCTION && d->is_native && d->as.function.body) failed = 1;
            if (d->kind == AST_DECL_CONSTANT) {
                const char *name = ast_program_lexeme(program, d->name_token);
                unsigned long long value = strtoull(d->as.constant.value->folded_constant.lexeme, NULL, 10);
                if (!strcmp(name, "recordSize") && value != sizeof(Reference)) failed = 1;
                if (!strcmp(name, "recordAlign") && value != _Alignof(Reference)) failed = 1;
                if (!strcmp(name, "nestedSize") && value != sizeof(NestedReference)) failed = 1;
            }
        }
        size_t offset = record->fields[1].native_offset;
        record->fields[1].native_offset++;
        if (ir_verify_module(module)) failed = 1;
        record->fields[1].native_offset = offset;
        size_t stride = nested->fields[0].native_array_stride;
        nested->fields[0].native_array_stride++;
        if (ir_verify_module(module)) failed = 1;
        nested->fields[0].native_array_stride = stride;
        size_t size = record->native_layout.size;
        record->native_layout.size++;
        if (ir_verify_module(module)) failed = 1;
        record->native_layout.size = size;
        record->is_native = 0;
        if (ir_verify_module(module)) failed = 1;
        record->is_native = 1;
        const char *abi = module->native_imports[0].abi;
        module->native_imports[0].abi = "invalid";
        if (ir_verify_module(module)) failed = 1;
        module->native_imports[0].abi = abi;
        IrTypeId result = module->native_imports[0].return_type_id;
        module->native_imports[0].return_type_id = IR_TYPE_NONE;
        if (ir_verify_module(module)) failed = 1;
        module->native_imports[0].return_type_id = result;
        IrTypeId parameter = module->native_imports[0].parameter_types[0];
        module->native_imports[0].parameter_types[0] = record->fields[1].type_id;
        if (ir_verify_module(module)) failed = 1;
        module->native_imports[0].parameter_types[0] = parameter;
        IrTypeId field_type = record->fields[1].type_id;
        record->fields[1].type_id = module->structures[2].fields[1].type_id;
        if (ir_verify_module(module)) failed = 1;
        record->fields[1].type_id = field_type;
        if (!ir_verify_module(module)) failed = 1;
        FILE *dump = tmpfile();
        if (!dump || !ir_dump(dump, module)) failed = 1;
        if (dump) fclose(dump);
    }
    if (failed) {
        fprintf(stderr, "Native frontend/layout/IR test failed for target %d (parsed=%d errors=%zu module=%d)\n",
                (int)target, program ? program->structured_ast_complete : 0,
                semantics ? semantics->error_count : 0, module != NULL);
        error_handler_flush(global_error_handler);
        if (module) (void)ir_dump(stderr, module);
    }
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    return failed;
}

static int reject(const char *source) {
    FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source), "ffi-negative.dmm", &options);
    SemanticModel *semantics = program && program->structured_ast_complete ? semantic_analyze(program) : NULL;
    int failed = program && program->structured_ast_complete && semantics && !semantics->error_count;
    if (failed) fprintf(stderr, "Invalid native declaration accepted: %s\n", source);
    semantic_model_free(semantics);
    ast_program_free(program);
    error_handler_reset(global_error_handler);
    return failed;
}

int main(void) {
    ErrorHandler *handler = error_handler_init();
    if (!handler) return 1;
    error_handler_set_global(handler);
    error_handler_set_buffered(handler, 1);
    int failed = check_target(TARGET_ELF) | check_target(TARGET_COFF);
    const char *invalid[] = {
        "extern \"C\" {}",
        "extern \"system\" { func f() -> i32; }",
        "extern \"system\" from \"c\" { func f() -> i32 { return 0; } }",
        "extern \"system\" from \"c\" { func f<T>() -> i32; }",
        "extern \"system\" { struct S { var x:i32; func f() -> i32 { return 0; } } }",
        "extern \"system\" { struct S { destructor {} } }",
        "extern \"system\" { struct S { var x:S; } }",
        "extern \"system\" { struct S {} }",
        "extern \"system\" { struct H; struct S { var x:H; } }",
        "extern \"system\" { struct H; } func main() -> int { var h:H; return 0; }",
        "extern \"system\" { struct S { var x:string; } }",
        "extern \"system\" { struct S { var x:i32[]; } }",
        "extern \"system\" { struct S { var x:void; } }",
        "extern \"system\" from \"c\" { func f(x:int) -> i32; }",
        "extern \"system\" from \"c\" { func f(x:char) -> i32; }",
        "extern \"system\" from \"c\" { func f(x:byte) -> i32; }",
        "extern \"system\" from \"c\" { func f(x:void) -> i32; }",
        "extern \"system\" from \"c\" { func f(x:i32[2]) -> i32; }",
        "struct S { var x:i32; } extern \"system\" from \"c\" { func f(x:S) -> i32; }",
        "extern \"system\" from \"c\" { func f(x:&i32) -> i32; }",
        "extern \"system\" from \"ws2_32.dll\" { func f() -> i32; }",
        "extern \"system\" from \"bad.dll\" { struct S { var x:i32; } }",
        "extern \"system\" from \"-evil\" { func f() -> i32; }",
        "extern \"system\" from \"c\" { func f() -> i32 = \"\"; }",
        "extern \"system\" from \"c\" { func f() -> i32 = \"x\"; func g() -> double = \"x\"; }",
        "extern \"system\" from \"c\" { func f() -> i32 = \"x\"; } extern \"system\" from \"m\" { func g() -> i32 = \"x\"; }",
        "extern \"system\" from \"c\" { func f() -> i32; } func main() -> int { var g:func() -> i32 = f; return 0; }",
        "func f() -> i32 { return 0; } func main() -> int { var g:extern \"system\" func() -> i32 = f; return 0; }",
        "extern \"system\" from \"c\" { func f(x:extern \"system\" func(string) -> i32) -> i32; }",
        "export \"system\" func f(x:int) -> i32 { return 0; }",
        "export \"system\" func f<T>(x:T) -> i32 { return 0; }",
        "struct S { var x:i32; } export \"system\" func f(x:S) -> i32 { return 0; }",
        "extern \"system\" { struct S { var callback:func() -> i32; } }",
        "extern \"system\" { align(16) struct S; }",
        "extern \"system\" { pack(3) struct S { var x:i32; } }"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) failed |= reject(invalid[i]);
    error_handler_free(handler);
    return failed;
}
