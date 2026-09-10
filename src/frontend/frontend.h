#ifndef DMM_FRONTEND_H
#define DMM_FRONTEND_H

#include "ast.h"
#include <stddef.h>

typedef struct {
    int debug;
    int show_tokens;
} FrontendOptions;

/* Parse a source file into an owned recursive AST with no backend side effects. */
AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options);

/* Parse an in-memory source buffer without resolving filesystem imports. */
AstProgram *frontend_parse_source(const char *source, size_t length,
                                  const char *source_name,
                                  const FrontendOptions *options);

#endif
