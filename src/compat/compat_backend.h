#ifndef DMM_COMPAT_BACKEND_H
#define DMM_COMPAT_BACKEND_H

#include "backend.h"

int compat_backend_emit_file(const IrModule *module, const BackendOptions *options,
                             const char *output_path);

#endif
