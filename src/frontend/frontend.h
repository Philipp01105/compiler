#ifndef DMM_FRONTEND_H
#define DMM_FRONTEND_H

#include "ast.h"
#include <stddef.h>

typedef struct {
    int debug;
    int show_tokens;
    int recover_syntax; /* IDE analysis only: discard malformed statements/declarations. */
} FrontendOptions;

/* Parse a source file into an owned recursive AST with no backend side effects. */
AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options);

/* Reconcile dependencies across all packages in a module; never emits code. */
int frontend_sync_manifest(const char *package_directory);

/* Parse an in-memory source buffer without resolving filesystem imports. */
AstProgram *frontend_parse_source(const char *source, size_t length,
                                  const char *source_name,
                                  const FrontendOptions *options);

#endif
