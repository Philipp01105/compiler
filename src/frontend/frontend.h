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
    const char *stdlib_root; /* Optional configured source distribution; borrowed for parsing. */
    int (*workspace_directory)(void *context, const char *directory, int boundary_only);
    void *workspace_directory_context;
} FrontendOptions;

/* Parse a source file into an owned recursive AST with no backend side effects. */
AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options);
/* IDE workspace indexing: discover unopened packages through compiler module/target rules. */
int frontend_discover_workspace(AstProgram *program, const FrontendOptions *options);

/* Reconcile dependencies across all packages in a module; never emits code. */
int frontend_sync_manifest(const char *package_directory);
/* Read-only synchronization plan; caller owns the returned manifest path. */
int frontend_preview_manifest(const char *package_directory, FILE *output, char **manifest_path);

/* Parse an in-memory source buffer without resolving filesystem imports. */
AstProgram *frontend_parse_source(const char *source, size_t length,
                                  const char *source_name,
                                  const FrontendOptions *options);

/* Deterministic token inventory for the root and every resolved import. */
int frontend_dump_tokens(FILE *output, const AstProgram *program);

#endif
