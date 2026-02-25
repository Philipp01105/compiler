#ifndef DMM_FRONTEND_H
#define DMM_FRONTEND_H

#include "ast.h"

typedef struct {
    int debug;
    int show_tokens;
} FrontendOptions;

/* Parse a source file into an AST with no backend side effects. */
AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options);

#endif
