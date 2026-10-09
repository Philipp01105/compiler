#include "errorHandler.h"
#include "test_source.h"
#include "ir.h"
#include "semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int control_flow_regressions(void) {
    const char *source =
            "func main() -> int {"
            "var condition = true && (false || true);"
            "if (condition) { return 1; } return 0; }";
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source), "cfg-test.dmm", &options);
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    IrModule *module = semantics == NULL || semantics->error_count != 0 ? NULL : ir_lower_program(program, semantics);
    int failed = module == NULL || !ir_verify_module(module);
    if (module != NULL) {
        IrFunction *function = &module->functions[0];
        size_t inner = IR_VALUE_NONE, outer = IR_VALUE_NONE, branch = IR_VALUE_NONE;
        for (size_t i = 0; i < function->instruction_count; i++) {
            if (function->instructions[i].opcode == IR_OP_PHI) {
                if (inner == IR_VALUE_NONE) inner = i;
                else outer = i;
            }
            if (outer != IR_VALUE_NONE && function->instructions[i].opcode == IR_OP_BRANCH) branch = i;
        }
        if (inner == IR_VALUE_NONE || outer == IR_VALUE_NONE || branch == IR_VALUE_NONE) failed = 1;
        else {
            IrInstruction saved = function->instructions[inner];
            /* A value produced on one incoming branch is unavailable on the other. */
            function->instructions[inner].operand_a = saved.operand_b;
            if (ir_verify_module(module)) failed = 1;
            function->instructions[inner] = saved;
            function->instructions[inner].target_a = function->instructions[outer].target_a;
            if (ir_verify_module(module)) failed = 1;
            function->instructions[inner] = saved;
            /* The inner PHI precedes this use in the array, but can be bypassed. */
            size_t condition = function->instructions[branch].operand_a;
            function->instructions[branch].operand_a = saved.result;
            if (ir_verify_module(module)) failed = 1;
            function->instructions[branch].operand_a = condition;
            /* Remove an incoming edge while retaining both valid target labels. */
            for (size_t i = 0; i < inner; i++)
                if (function->instructions[i].opcode == IR_OP_LABEL &&
                    function->instructions[i].target_a == saved.target_a) {
                    size_t jump = i + 1;
                    size_t target = function->instructions[jump].target_a;
                    function->instructions[jump].target_a = function->instructions[outer].target_a;
                    if (ir_verify_module(module)) failed = 1;
                    function->instructions[jump].target_a = target;
                    break;
                }
            /* A non-void reachable exit must retain its return terminator. */
            function->instruction_count--;
            if (ir_verify_module(module)) failed = 1;
            function->instruction_count++;
            /* Insert a valid fresh constant after a jump, before its next label. */
            IrInstruction *original = function->instructions;
            size_t n = function->instruction_count;
            IrInstruction *mutated = malloc((n + 1) * sizeof(*mutated));
            if (mutated == NULL) failed = 1;
            else {
                size_t at = 0, constant = 0;
                for (size_t i = 0; i < n; i++) {
                    if (function->instructions[i].opcode == IR_OP_JUMP && at == 0) at = i + 1;
                    if (function->instructions[i].opcode == IR_OP_CONSTANT) constant = i;
                }
                memcpy(mutated, original, at * sizeof(*mutated));
                mutated[at] = original[constant];
                mutated[at].result = function->next_value++;
                memcpy(mutated + at + 1, original + at, (n - at) * sizeof(*mutated));
                function->instructions = mutated;
                function->instruction_count++;
                if (ir_verify_module(module)) failed = 1;
                /* PHIs must precede ordinary instructions in a join block. */
                memcpy(mutated, original, inner * sizeof(*mutated));
                mutated[inner] = original[constant];
                mutated[inner].result = function->next_value - 1;
                memcpy(mutated + inner + 1, original + inner, (n - inner) * sizeof(*mutated));
                if (ir_verify_module(module)) failed = 1;
                function->instructions = original;
                function->instruction_count--;
                function->next_value--;
                free(mutated);
            }
            if (!ir_verify_module(module)) failed = 1;
        }
    }
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    if (failed) fprintf(stderr, "CFG dominance/PHI/terminator regression failed\n");
    return failed;
}

static int ownership_property_regressions(void) {
    const char *source =
        "struct File { var handle:int; destructor {} }"
        "struct Wrapper { var file:File; }"
        "struct Box<T> { var value:T; }"
        "var package_file:File;"
        "var package_wrapper:Wrapper;"
        "func main()->int { var copyable:Box<int>; var owned:Box<File>; "
        "var moved=owned; owned=moved; return 0; }";
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source),
                                            "ownership-properties.dmm", &options);
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    int failed = semantics == NULL || semantics->error_count != 0;
    int saw_file = 0, saw_wrapper = 0, saw_copyable_box = 0, saw_owned_box = 0;
    for (size_t i = 0; semantics != NULL && i < semantics->symbol_count; i++) {
        const SemanticSymbol *symbol = &semantics->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL)
            continue;
        unsigned properties = semantic_symbol_type_properties(semantics, symbol->id);
        const AstDeclarationNode *origin = symbol->declaration->generic_origin;
        const char *name = ast_program_lexeme(symbol->source_program, symbol->name_token);
        if (origin == NULL && !strcmp(name, "File")) {
            saw_file = (properties & (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP)) ==
                       (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP);
        } else if (origin == NULL && !strcmp(name, "Wrapper")) {
            saw_wrapper = (properties & (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP)) ==
                          (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP);
        } else if (origin != NULL && !strcmp(ast_program_lexeme(symbol->source_program,
                                                                origin->name_token), "Box")) {
            if (properties & SEMANTIC_TYPE_MOVE_ONLY) saw_owned_box = properties & SEMANTIC_TYPE_NEEDS_DROP;
            else saw_copyable_box = (properties & SEMANTIC_TYPE_COPYABLE) != 0 &&
                                    (properties & SEMANTIC_TYPE_NEEDS_DROP) == 0;
        }
    }
    if (!saw_file || !saw_wrapper || !saw_copyable_box || !saw_owned_box)
        failed = 1;
    IrModule *module = !failed ? ir_lower_program(program, semantics) : NULL;
    int saw_ir_drop_metadata = 0;
    for (size_t i = 0; module != NULL && i < module->structure_count; i++)
        if (module->structures[i].has_explicit_destructor &&
            (module->structures[i].type_properties &
             (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP)) ==
                (SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP))
            saw_ir_drop_metadata = 1;
    int saw_drop_glue = 0, saw_package_init = 0, saw_package_cleanup = 0;
    int saw_drop = 0, saw_move = 0, saw_reinit = 0;
    for (size_t f = 0; module != NULL && f < module->function_count; f++) {
        const IrFunction *function = &module->functions[f];
        if (function->is_drop_glue) saw_drop_glue = 1;
        if (function->is_package_init) saw_package_init = 1;
        if (function->is_package_cleanup) {
            saw_package_cleanup = function->instruction_count == 2 &&
                                  function->instructions[0].opcode == IR_OP_DROP &&
                                  function->instructions[1].opcode == IR_OP_DROP &&
                                  function->instructions[0].symbol_id ==
                                      module->globals[1].symbol_id &&
                                  function->instructions[1].symbol_id ==
                                      module->globals[0].symbol_id;
        }
        for (size_t i = 0; i < function->instruction_count; i++) {
            saw_drop |= function->instructions[i].opcode == IR_OP_DROP;
            saw_move |= function->instructions[i].opcode == IR_OP_MOVE;
            saw_reinit |= function->instructions[i].opcode == IR_OP_REINIT;
        }
    }
    if (module == NULL || !saw_ir_drop_metadata || !saw_drop_glue ||
        !saw_package_init ||
        !saw_package_cleanup ||
        !saw_drop || !saw_move || !saw_reinit)
        failed = 1;
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    if (failed) fprintf(stderr, "derived ownership property regression failed\n");
    return failed;
}

static void report_expression(const AstProgram *program, const AstExpression *expression) {
    if (expression == NULL) return;
    report_expression(program, expression->left);
    report_expression(program, expression->right);
    for (const AstExpression *argument = expression->arguments; argument != NULL;
         argument = argument->next)
        report_expression(program, argument);
    if (expression->resolved_type == TYPE_UNKNOWN &&
        expression->resolved_named_type_token == AST_TOKEN_NONE &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE &&
        expression->kind != AST_EXPR_RESERVE)
        fprintf(stderr, "  unresolved kind=%d token='%s' at %d:%d\n", expression->kind,
                ast_program_lexeme(program, expression->value_token),
                expression->span.begin.line, expression->span.begin.column);
}

static void report_statement(const AstProgram *program, const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        report_expression(program, statement->expression);
        report_expression(program, statement->value);
        report_expression(program, statement->condition);
        report_expression(program, statement->update);
        report_statement(program, statement->initializer);
        report_statement(program, statement->body);
        report_statement(program, statement->else_body);
    }
}

static void report_unresolved(const AstProgram *program) {
    for (const AstDeclarationNode *declaration = program->root; declaration != NULL;
         declaration = declaration->next) {
        if (declaration->kind == AST_DECL_FUNCTION)
            report_statement(program, declaration->as.function.body);
        else if (declaration->kind == AST_DECL_STRUCT)
            for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
                 method != NULL; method = method->next)
                report_statement(program, method->as.function.body);
    }
}

static int instruction_growth_regression(void) {
    char *source = malloc(65536);
    if (source == NULL) return 1;
    size_t used = (size_t) snprintf(source, 65536,
        "enum Propagation<T,R> { Continue(T), Break(R) } "
        "enum Outcome { Ok(int), Err(int); "
        "static func branch(value:Self)->Propagation<int,int> { match(value) { "
        "Ok(output)=>return Propagation<int,int>.Continue(output); "
        "Err(error)=>return Propagation<int,int>.Break(error); } } "
        "static func fromResidual(error:int)->Self { return Outcome.Err(error); } } "
        "struct File { var handle:int; destructor {} } enum Files { ");
    /* Drop glue keeps its receiver across many emissions and growth boundaries. */
    for (int i = 0; i < 20; i++)
        used += (size_t) snprintf(source + used, 65536 - used, "V%d(File),", i);
    used += (size_t) snprintf(source + used, 65536 - used, "} ");
    /* Shift '?' and value-match lowering across every offset of the initial
       32-instruction allocation, then several larger allocations. */
    for (int i = 0; i < 64; i++) {
        used += (size_t) snprintf(source + used, 65536 - used,
            "func step%d(input:Outcome)->Outcome { var padding:int=0;", i);
        for (int j = 0; j < i; j++)
            used += (size_t) snprintf(source + used, 65536 - used, "padding+=1;");
        used += (size_t) snprintf(source + used, 65536 - used,
            "var value=input?; var chosen=match(Outcome.Ok(value)) { "
            "Ok(output)=>{output} Err(error)=>{error} }; return Outcome.Ok(chosen); }");
    }
    used += (size_t) snprintf(source + used, 65536 - used,
        "func main()->int { var file:File; var files=Files.V0(file); "
        "var moved=files; return 0; }");
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, used, "instruction-growth.dmm", &options);
    free(source);
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    IrModule *module = semantics == NULL || semantics->error_count != 0
        ? NULL : ir_lower_program(program, semantics);
    int failed = module == NULL || !ir_verify_module(module);
    size_t large_functions = 0;
    for (size_t i = 0; module != NULL && i < module->function_count; i++)
        if (module->functions[i].instruction_count > 32) large_functions++;
    if (large_functions < 64) failed = 1;
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    if (failed) fprintf(stderr, "IR instruction growth regression failed\n");
    return failed;
}

static int generic_async_callee_regression(void) {
    const char *source =
        "struct Box<T> { var value:T; } "
        "func make<T>(value:T)->Box<T> { return Box<T>{value:value}; } "
        "async func one()->int { return 1; } "
        "async func invoke(callback:func()->Future<int>)->int { return callback().await(); } "
        "async func build()->int { var box=make<int>(42); return box.value+invoke(one).await(); } "
        "func main()->int { return block_on(build()); }";
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source),
                                            "generic-async-callee.dmm", &options);
    if (program != NULL) {
        DmmModule *module = ast_program_alloc(program, sizeof(*module));
        DmmFeature *feature = ast_program_alloc(program, sizeof(*feature));
        if (module == NULL || feature == NULL) { ast_program_free(program); return 1; }
        feature->name = "async";
        module->features = feature;
        program->module = module;
    }
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    int failed = semantics == NULL || semantics->error_count != 0 ||
                 semantics->unresolved_expression_count != 0;
    IrModule *module = !failed ? ir_lower_program(program, semantics) : NULL;
    if (module == NULL || !ir_verify_module(module)) failed = 1;
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    if (failed) fprintf(stderr, "generic async callee regression failed\n");
    return failed;
}

static int unresolved_call_regression(void) {
    const char *source = "func main() -> void { missing(); }";
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source),
                                            "unresolved-call-test.dmm", &options);
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    int failed = semantics == NULL || semantics->error_count == 0 ||
                 semantics->unresolved_expression_count == 0;
    semantic_model_free(semantics);
    ast_program_free(program);
    return failed;
}

static int thread_type_property_regression(void) {
    const char *source =
        "struct Safe { var value:int; }"
        "struct Raw { var pointer:*int; }"
        "struct Nested { var values:Safe[2]; }"
        "struct NestedRaw { var value:Raw; }"
        "interface Reading { func read() -> int; }"
        "struct Dynamic { var value:Reading; }"
        "func main() -> void { var a:Safe; var b:Raw; var c:Nested;"
        "var d:NestedRaw; var e:Dynamic; }";
    const FrontendOptions options = {0};
    AstProgram *program = test_parse_source(source, strlen(source),
                                            "thread-type-test.dmm", &options);
    SemanticModel *semantics = program == NULL ? NULL : semantic_analyze(program);
    IrModule *module = semantics != NULL && semantics->error_count == 0
                           ? ir_lower_program(program, semantics) : NULL;
    const char *names[] = {"Safe", "Raw", "Nested", "NestedRaw", "Dynamic"};
    int expected[] = {1, 0, 1, 0, 0};
    int failed = semantics == NULL || semantics->error_count != 0 ||
                 module == NULL || !ir_verify_module(module);
    for (size_t i = 0; !failed && i < sizeof(names) / sizeof(names[0]); i++) {
        const SemanticSymbol *symbol = semantic_find_global(
            semantics, names[i], SEMANTIC_SYMBOL_STRUCT);
        unsigned properties = symbol == NULL ? 0 :
            semantic_symbol_type_properties(semantics, symbol->id);
        int thread_safe = (properties & (SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC)) ==
                          (SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC);
        int ir_thread_safe = 0;
        for (IrTypeId t = 0; module != NULL && t < module->type_count; t++)
            if (symbol != NULL && module->types[t].symbol_id == symbol->id) {
                unsigned ir_properties = ir_type_properties(module, t);
                ir_thread_safe = (ir_properties & (SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC)) ==
                                 (SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC);
            }
        if (symbol == NULL || thread_safe != expected[i] || ir_thread_safe != expected[i]) {
            fprintf(stderr, "thread type properties for %s: sema=%d ir=%d expected=%d\n",
                    names[i], thread_safe, ir_thread_safe, expected[i]);
            failed = 1;
        }
    }
    ir_module_free(module);
    semantic_model_free(semantics);
    ast_program_free(program);
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    ErrorHandler *errors = error_handler_init();
    if (errors == NULL) return 1;
    error_handler_set_global(errors);
    int failed = control_flow_regressions() || ownership_property_regressions() ||
                 unresolved_call_regression() || instruction_growth_regression() || generic_async_callee_regression() ||
                 thread_type_property_regression();
    int saw_cast = 0;
    int saw_alloc = 0;
    int saw_free = 0;
    const FrontendOptions options = {0};
    for (int i = 1; i < argc; i++) {
        AstProgram *program = frontend_parse_file(argv[i], &options);
        SemanticModel *semantics = semantic_analyze(program);
        IrModule *module = semantics != NULL && semantics->error_count == 0
                               ? ir_lower_program(program, semantics)
                               : NULL;
        if (strstr(argv[i], "package_import_fixture") != NULL) {
            const AstImportPath *paths = program != NULL && program->root != NULL
                                             ? program->root->as.import_decl.paths
                                             : NULL;
            if (program == NULL || program->structured_declaration_count != 2 ||
                program->owned_import_count != 2 || paths == NULL || paths->next != NULL ||
                module == NULL || module->import_count != 1 || module->function_count != 5) {
                fprintf(stderr, "grouped package import AST/IR contract failed\n");
                failed = 1;
            }
        }
        if (semantics != NULL && semantics->unresolved_expression_count != 0)
            fprintf(stderr, "unresolved expressions for %s: %zu\n", argv[i],
                    semantics->unresolved_expression_count);
        if (semantics != NULL && semantics->unresolved_expression_count != 0)
            report_unresolved(program);
        if (semantics != NULL && semantics->unresolved_expression_count != 0)
            failed = 1;
        if (semantics != NULL && semantics->error_count != 0) {
            fprintf(stderr, "semantic errors for %s: %zu\n", argv[i],
                    semantics->error_count);
            failed = 1;
        }
        if (strstr(argv[i], "import_root.dmm") != NULL &&
            (program->owned_import_count != 1 || semantics == NULL ||
             semantics->unresolved_expression_count != 0 || module == NULL ||
             module->import_count != 1 || module->function_count != 2 ||
             module->structure_count != 1)) {
            fprintf(stderr, "imported AST/sema/IR graph is incomplete\n");
            failed = 1;
        }
        if (strstr(argv[i], "import_root.dmm") != NULL &&
            program != NULL && program->owned_import_count == 1) {
            AstProgram *imported = program->owned_imports[0];
            const char *root_func = NULL;
            const char *import_func = NULL;
            for (size_t token = 0; token < program->token_count; token++)
                if (program->tokens[token].type == TOKEN_KEYWORD_FUNC) {
                    root_func = program->tokens[token].lexeme;
                    break;
                }
            for (size_t token = 0; token < imported->token_count; token++)
                if (imported->tokens[token].type == TOKEN_KEYWORD_FUNC) {
                    import_func = imported->tokens[token].lexeme;
                    break;
                }
            if (program->strings == NULL || imported->strings != program->strings ||
                !program->owns_strings || imported->owns_strings ||
                root_func == NULL || root_func != import_func) {
                fprintf(stderr, "source strings are not interned across imports\n");
                failed = 1;
            }
        }
        if (program == NULL || !program->structured_ast_complete ||
            !ast_validate_program(program) || semantics == NULL ||
            module == NULL || !ir_verify_module(module)) {
            fprintf(stderr, "typed frontend pipeline failed for %s\n", argv[i]);
            failed = 1;
        }
        for (IrTypeId t = 0; module != NULL && t < module->type_count; t++) {
            IrType saved = module->types[t];
            if (saved.kind == IR_TYPE_ARRAY) module->types[t].array_length = 0;
            else if (saved.kind == IR_TYPE_POINTER) module->types[t].element_type = t;
            else if (saved.kind == IR_TYPE_SLICE) module->types[t].array_length = 1;
            else continue;
            if (ir_verify_module(module)) {
                fprintf(stderr, "malformed P1 type graph accepted for %s\n", argv[i]);
                failed = 1;
            }
            module->types[t] = saved;
        }
        if (i == 1) {
            const SemanticSymbol *add = semantic_find_global(semantics, "add",
                                                             SEMANTIC_SYMBOL_FUNCTION);
            if (add == NULL || add->declared_type.name_token == AST_TOKEN_NONE ||
                strcmp(ast_program_lexeme(program, add->declared_type.name_token), "int") != 0)
                failed = 1;
            if (module == NULL || module->type_count == 0 || module->structure_count != 1 ||
                module->enum_count != 1 || module->structures[0].field_count != 2 ||
                module->enums[0].variant_count != 2 ||
                program->root->resolved_symbol_id == AST_SYMBOL_NONE ||
                program->root->as.struct_decl.fields->resolved_symbol_id == AST_SYMBOL_NONE)
                failed = 1;
            int saw_resolved_load = 0;
            int tested_verifier = 0;
            int tested_typed_verifier = 0;
            int tested_return_verifier = 0;
            int saw_typed_parameters = 0;
            for (size_t f = 0; module != NULL && f < module->function_count; f++) {
                IrFunction *function = &module->functions[f];
                if (strcmp(ast_program_lexeme(program, function->name_token), "add") == 0) {
                    if (function->parameter_count != 2 ||
                        function->parameters[0].type != TYPE_INT ||
                        function->parameters[1].type != TYPE_INT ||
                        function->parameters[0].symbol_id == AST_SYMBOL_NONE ||
                        function->parameters[1].symbol_id == AST_SYMBOL_NONE)
                        failed = 1;
                    else
                        saw_typed_parameters = 1;
                    if (function->parameter_count == 2) {
                        size_t saved_symbol = function->parameters[0].symbol_id;
                        function->parameters[0].symbol_id = AST_SYMBOL_NONE;
                        if (ir_verify_module(module)) failed = 1;
                        function->parameters[0].symbol_id = saved_symbol;
                    }
                }
                for (size_t n = 0; n < function->instruction_count; n++) {
                    IrInstruction *instruction = &function->instructions[n];
                    if (instruction->opcode == IR_OP_LOAD &&
                        instruction->symbol_id != AST_SYMBOL_NONE)
                        saw_resolved_load = 1;
                    if (instruction->opcode == IR_OP_CAST) saw_cast = 1;
                    if (instruction->opcode == IR_OP_ALLOC) saw_alloc = 1;
                    if (instruction->opcode == IR_OP_FREE) saw_free = 1;
                    if (!tested_verifier && instruction->opcode == IR_OP_BINARY) {
                        size_t saved = instruction->operand_a;
                        instruction->operand_a = function->next_value;
                        if (ir_verify_module(module)) failed = 1;
                        instruction->operand_a = saved;
                        tested_verifier = 1;
                    }
                    if (!tested_typed_verifier && instruction->opcode == IR_OP_BINARY) {
                        TokenType saved = instruction->operator_type;
                        instruction->operator_type = TOKEN_AMP_AMP;
                        if (ir_verify_module(module)) failed = 1;
                        instruction->operator_type = saved;
                        tested_typed_verifier = 1;
                    }
                    if (!tested_return_verifier && instruction->opcode == IR_OP_RETURN &&
                        instruction->operand_a != IR_VALUE_NONE) {
                        IrTypeId invalid_return_type = IR_TYPE_NONE;
                        for (IrTypeId t = 0; t < module->type_count; t++)
                            if (module->types[t].kind == IR_TYPE_PRIMITIVE &&
                                module->types[t].primitive == TYPE_VOID) {
                                invalid_return_type = t;
                                break;
                            }
                        if (invalid_return_type != IR_TYPE_NONE) {
                            IrTypeId saved = function->return_type_id;
                            function->return_type_id = invalid_return_type;
                            if (ir_verify_module(module)) failed = 1;
                            function->return_type_id = saved;
                            tested_return_verifier = 1;
                        }
                    }
                }
            }
            if (!saw_resolved_load || !saw_typed_parameters || !tested_verifier ||
                !tested_typed_verifier || !tested_return_verifier ||
                !ir_verify_module(module))
                failed = 1;
        }
        for (size_t f = 0; module != NULL && f < module->function_count; f++)
            for (size_t n = 0; n < module->functions[f].instruction_count; n++) {
                IrOpcode opcode = module->functions[f].instructions[n].opcode;
                if (opcode == IR_OP_CAST) saw_cast = 1;
                if (opcode == IR_OP_ALLOC) saw_alloc = 1;
                if (opcode == IR_OP_FREE) saw_free = 1;
            }
        ir_module_free(module);
        semantic_model_free(semantics);
        ast_program_free(program);
    }
    if (!saw_cast || !saw_alloc || !saw_free) {
        fprintf(stderr, "pipeline corpus did not cover cast/allocation/free IR\n");
        failed = 1;
    }
    error_handler_free(errors);
    return failed;
}
