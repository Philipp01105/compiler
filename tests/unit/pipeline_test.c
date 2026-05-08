#include "errorHandler.h"
#include "frontend.h"
#include "ir.h"
#include "semantic.h"

#include <stdio.h>
#include <string.h>

static void report_expression(const AstProgram *program, const AstExpression *expression) {
    if (expression == NULL) return;
    report_expression(program, expression->left);
    report_expression(program, expression->right);
    for (const AstExpression *argument = expression->arguments; argument != NULL;
         argument = argument->next) report_expression(program, argument);
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

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    ErrorHandler *errors = error_handler_init();
    if (errors == NULL) return 1;
    error_handler_set_global(errors);
    int failed = 0;
    int saw_cast = 0;
    int saw_alloc = 0;
    int saw_free = 0;
    const FrontendOptions options = {0};
    for (int i = 1; i < argc; i++) {
        AstProgram *program = frontend_parse_file(argv[i], &options);
        SemanticModel *semantics = semantic_analyze(program);
        IrModule *module = semantics != NULL && semantics->error_count == 0
            ? ir_lower_program(program, semantics) : NULL;
        int has_import = module != NULL && module->import_count != 0;
        if (semantics != NULL && semantics->unresolved_expression_count != 0)
            fprintf(stderr, "unresolved expressions for %s: %zu\n", argv[i],
                    semantics->unresolved_expression_count);
        if (semantics != NULL && semantics->unresolved_expression_count != 0)
            report_unresolved(program);
        if (semantics != NULL && semantics->unresolved_expression_count != 0 && !has_import)
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
        if (program == NULL || !program->structured_ast_complete ||
            !ast_validate_program(program) || semantics == NULL ||
            module == NULL || !ir_verify_module(module) || module->function_count == 0) {
            fprintf(stderr, "typed frontend pipeline failed for %s\n", argv[i]);
            failed = 1;
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
                !ir_verify_module(module)) failed = 1;
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
