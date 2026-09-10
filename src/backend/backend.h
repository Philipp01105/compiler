#ifndef DMM_BACKEND_H
#define DMM_BACKEND_H

#include "ir.h"

typedef struct {
    TargetFormat target_format;
    SyntaxMode syntax_mode;
    int debug;
    int deterministic;
    const char *source_map_path;
} BackendOptions;

/* Emit one assembly file from the verified target-neutral module. */
int backend_emit_file(const IrModule *module, const BackendOptions *options,
                      const char *output_path);

#endif
