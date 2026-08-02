#ifndef DMM_EXTERNAL_LINK_H
#define DMM_EXTERNAL_LINK_H
#include "backend.h"

int driver_external_link(const IrModule *module, const BackendOptions *options,
                         const char *output,
                         const char *linker_driver, const char *runtime_shim);
int driver_link_input_conflicts(TargetFormat target, const char *artifact,
                                const char *linker_driver, const char *runtime_shim);
#endif
