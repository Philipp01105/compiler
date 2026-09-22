#ifndef DMM_FRONTEND_H
#define DMM_FRONTEND_H

#include "ast.h"
#include <stddef.h>
#include <stdio.h>

typedef struct {
    int debug;
    int show_tokens;
    int recover_syntax; /* IDE analysis only: discard malformed statements/declarations. */
    int has_target;
    TargetFormat target_format;
    int include_all_targets; /* Manifest dependency discovery only. */
    int system_packages; /* Load the default stdlib system package as source. */
} FrontendOptions;

/* Parse a source file into an owned recursive AST with no backend side effects. */
AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options);

/* Reconcile dependencies across all packages in a module; never emits code. */
int frontend_sync_manifest(const char *package_directory);

/* Parse an in-memory source buffer without resolving filesystem imports. */
AstProgram *frontend_parse_source(const char *source, size_t length,
                                  const char *source_name,
                                  const FrontendOptions *options);

/* Deterministic token inventory for the root and every resolved import. */
int frontend_dump_tokens(FILE *output, const AstProgram *program);

#endif
