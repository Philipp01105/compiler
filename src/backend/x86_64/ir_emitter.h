#ifndef DMM_X86_64_IR_EMITTER_H
#define DMM_X86_64_IR_EMITTER_H

#include "ir.h"

int x86_64_emit_ir_file(const IrModule *module, TargetFormat target,
                        SyntaxMode syntax, int deterministic,
                        const char *output_path, const char *source_map_path);

#include "native/object.h"

int x86_64_lower_native(const IrModule *module, TargetFormat target, NativeObject *object, FILE *source_map);
#endif
