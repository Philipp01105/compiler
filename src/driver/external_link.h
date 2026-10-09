#ifndef DMM_EXTERNAL_LINK_H
#define DMM_EXTERNAL_LINK_H
#include "backend.h"

typedef struct {
    const char **libraries; /* NAME=PATH */
    size_t library_count;
    const char **directories;
    size_t directory_count;
} NativeLinkOptions;

int driver_native_options_valid(const NativeLinkOptions *options);

int driver_native_input_conflicts(const NativeLinkOptions *options, const char *artifact);

int driver_dump_native_link(const IrModule *module, const BackendOptions *options,
                            const NativeLinkOptions *native, const char *path);

int driver_external_link(const IrModule *module, const BackendOptions *options,
                         const char *output,
                         const char *linker_driver, const char *runtime_shim,
                         const NativeLinkOptions *native);

int driver_link_input_conflicts(TargetFormat target, const char *artifact,
                                const char *linker_driver, const char *runtime_shim);
#endif
