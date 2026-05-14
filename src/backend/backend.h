#ifndef DMM_BACKEND_H
#define DMM_BACKEND_H

#include "ir.h"
typedef enum { BACKEND_ASSEMBLY, BACKEND_OBJECT, BACKEND_EXECUTABLE } BackendEmission;

typedef struct {
    TargetFormat target_format;
    SyntaxMode syntax_mode;
    int debug;
    int deterministic;
    const char *source_map_path;
    BackendEmission emission;
} BackendOptions;

/* Emit assembly, a relocatable object, or an internally linked native image. */
int backend_emit_file(const IrModule *module, const BackendOptions *options,
                      const char *output_path);

#endif
