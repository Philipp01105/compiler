#ifndef DMM_BACKEND_H
#define DMM_BACKEND_H

#include "ast.h"

typedef struct {
    TargetFormat target_format;
    SyntaxMode syntax_mode;
    int debug;
    int deterministic;
} BackendOptions;

/* Emit one assembly file. This is the only public AST-to-machine boundary. */
int backend_emit_file(const AstProgram *program, const BackendOptions *options,
                      const char *output_path);

#endif
