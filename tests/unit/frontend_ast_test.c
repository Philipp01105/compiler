#include "ast.h"
#include "errorHandler.h"
#include "frontend.h"

#include <stdio.h>
#include <string.h>

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

    static const char resource_source[] =
        "package main;\n"
        "struct File {\n"
        "  var handle:int;\n"
        "  destructor { handle=0; }\n"
        "}\n"
        "func inspect(value:&File)->void {}\n"
        "func modify(value:&mut File)->void {}\n"
        "func main()->void {\n"
        "  var value:File;\n"
        "  var reference:&mut File=&mut value;\n"
        "  defer inspect(&value);\n"
        "  defer func() { inspect(&value); }\n"
        "}\n";
    program = frontend_parse_source(resource_source, strlen(resource_source),
                                    "<resource-syntax>", &options);
    const AstDeclarationNode *resource = program == NULL ? NULL : program->root;
    const AstDeclarationNode *inspect = resource == NULL ? NULL : resource->next;
    const AstDeclarationNode *modify = inspect == NULL ? NULL : inspect->next;
    const AstDeclarationNode *main_function = modify == NULL ? NULL : modify->next;
    const AstStatement *body = main_function == NULL ? NULL : main_function->as.function.body;
    const AstStatement *variable = body == NULL ? NULL : body->body;
    const AstStatement *reference = variable == NULL ? NULL : variable->next;
    const AstStatement *deferred_call = reference == NULL ? NULL : reference->next;
    const AstStatement *deferred_closure = deferred_call == NULL ? NULL : deferred_call->next;
    if (program == NULL || !ast_validate_program(program) || resource == NULL ||
        resource->kind != AST_DECL_STRUCT ||
        resource->as.struct_decl.destructor == NULL || inspect == NULL ||
        inspect->as.function.parameters == NULL ||
        inspect->as.function.parameters->type.borrow_kind != AST_BORROW_IMMUTABLE ||
        modify == NULL || modify->as.function.parameters == NULL ||
        modify->as.function.parameters->type.borrow_kind != AST_BORROW_MUTABLE ||
        reference == NULL || reference->type.borrow_kind != AST_BORROW_MUTABLE ||
        reference->value == NULL || !reference->value->mutable_borrow ||
        deferred_call == NULL || deferred_call->kind != AST_STMT_DEFER ||
        deferred_call->expression == NULL || deferred_call->body != NULL ||
        deferred_closure == NULL || deferred_closure->kind != AST_STMT_DEFER ||
        deferred_closure->expression != NULL || deferred_closure->body == NULL) {
        fprintf(stderr, "destructor/borrow/defer syntax AST is incomplete\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }
    ast_program_free(program);

    static const char async_source[] =
        "package main;\n"
        "async func pending() -> int { return 4; }\n"
        "async func main() -> int { return pending().await(); }\n";
    program = frontend_parse_source(async_source, strlen(async_source),
                                    "<async-syntax>", &options);
    const AstDeclarationNode *pending = program == NULL ? NULL : program->root;
    const AstDeclarationNode *async_main = pending == NULL ? NULL : pending->next;
    const AstStatement *returned = async_main == NULL ||
                                   async_main->as.function.body == NULL
                                       ? NULL : async_main->as.function.body->body;
    if (program == NULL || !ast_validate_program(program) ||
        pending == NULL || !pending->as.function.is_async ||
        async_main == NULL || !async_main->as.function.is_async ||
        returned == NULL || returned->kind != AST_STMT_RETURN ||
        returned->value == NULL || returned->value->kind != AST_EXPR_AWAIT ||
        returned->value->right == NULL ||
        returned->value->right->kind != AST_EXPR_CALL) {
        fprintf(stderr, "async/await syntax AST is incomplete\n");
        ast_program_free(program);
        error_handler_free(errors);
        return 1;
    }
    ast_program_free(program);
    error_handler_free(errors);
    return 0;
}
