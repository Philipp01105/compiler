#include "ast.h"
#include "errorHandler.h"
#include "frontend.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "expected source fixture path\n");
        return 1;
    }
    ErrorHandler *errors = error_handler_init();
    if (errors == NULL) return 1;
    error_handler_set_global(errors);

    const FrontendOptions options = {0};
    AstProgram *program = frontend_parse_file(argv[1], &options);
    if (program == NULL) {
        error_handler_free(errors);
        return 1;
    }
    if (program->declaration_count != 4 ||
        program->declarations[0].kind != AST_DECL_STRUCT ||
        program->declarations[1].kind != AST_DECL_ENUM ||
        program->declarations[2].kind != AST_DECL_FUNCTION ||
        program->declarations[3].kind != AST_DECL_FUNCTION) {
        fprintf(stderr, "unexpected AST declaration structure\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }
    if (program->tokens == NULL || program->token_count == 0 ||
        program->tokens[0].span.begin.line != 1) {
        fprintf(stderr, "AST did not preserve token leaves/source spans\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }
    if (!program->structured_ast_complete || program->structured_declaration_count != 4 ||
        program->root == NULL || program->root->kind != AST_DECL_STRUCT ||
        !ast_validate_program(program)) {
        fprintf(stderr, "frontend did not build a complete structured AST\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }
    const AstDeclarationNode *function = program->root->next->next;
    if (function == NULL || function->kind != AST_DECL_FUNCTION ||
        function->as.function.body == NULL || function->as.function.body->body == NULL ||
        function->as.function.body->body->kind != AST_STMT_RETURN ||
        function->as.function.body->body->value == NULL ||
        function->as.function.body->body->value->kind != AST_EXPR_BINARY) {
        fprintf(stderr, "function body/expression nodes are incomplete\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }

    ast_program_free(program);
    error_handler_free(errors);
    return 0;
}
