#include "ast.h"

#include <stdlib.h>

void ast_program_free(AstProgram *program) {
    if (program == NULL) return;
    free(program->source_path);
    free(program->tokens);
    free(program->declarations);
    free(program);
}

const char *ast_declaration_kind_name(AstDeclarationKind kind) {
    switch (kind) {
        case AST_DECL_IMPORT: return "import";
        case AST_DECL_STRUCT: return "struct";
        case AST_DECL_ENUM: return "enum";
        case AST_DECL_FUNCTION: return "function";
        case AST_DECL_INVALID: return "invalid";
    }
    return "invalid";
}
