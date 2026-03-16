#include "errorHandler.h"
#include "frontend.h"
#include "ir.h"
#include "semantic.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    ErrorHandler *errors = error_handler_init();
    if (errors == NULL) return 1;
    error_handler_set_global(errors);
    int failed = 0;
    const FrontendOptions options = {0};
    for (int i = 1; i < argc; i++) {
        AstProgram *program = frontend_parse_file(argv[i], &options);
        SemanticModel *semantics = semantic_analyze(program);
        IrModule *module = ir_lower_program(program, semantics);
        if (program == NULL || !program->structured_ast_complete || semantics == NULL ||
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
            int saw_resolved_load = 0;
            int tested_verifier = 0;
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
                    if (!tested_verifier && instruction->opcode == IR_OP_BINARY) {
                        size_t saved = instruction->operand_a;
                        instruction->operand_a = function->next_value;
                        if (ir_verify_module(module)) failed = 1;
                        instruction->operand_a = saved;
                        tested_verifier = 1;
                    }
                }
            }
            if (!saw_resolved_load || !saw_typed_parameters || !tested_verifier ||
                !ir_verify_module(module)) failed = 1;
        }
        ir_module_free(module);
        semantic_model_free(semantics);
        ast_program_free(program);
    }
    error_handler_free(errors);
    return failed;
}
