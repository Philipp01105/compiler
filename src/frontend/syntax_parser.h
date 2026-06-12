#ifndef DMM_SYNTAX_PARSER_H
#define DMM_SYNTAX_PARSER_H

#include "ast.h"

/* Builds structured nodes from the program's owned token leaves. */
int frontend_build_structured_ast(AstProgram * program);

int frontend_build_structured_ast_recover(AstProgram *program, int recover_syntax);

#endif
