#ifndef DMM_X86_64_IR_EMITTER_H
#define DMM_X86_64_IR_EMITTER_H

#include "ir.h"

int x86_64_emit_ir_file(const IrModule *module, TargetFormat target,
                        SyntaxMode syntax, int deterministic,
                        const char *output_path);

#endif
