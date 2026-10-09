#ifndef DMM_NATIVE_RUNTIME_H
#define DMM_NATIVE_RUNTIME_H
#include "native/object.h"
#include <stdio.h>
#include "runtime_profile.h"

int native_runtime_emit_requirements(NativeObject *, TargetFormat, RuntimeProfile, int, RuntimeRequirements);

int native_runtime_assembly_requirements(FILE *, TargetFormat, RuntimeProfile, int, RuntimeRequirements);

int native_runtime_emit_profile(NativeObject *object, TargetFormat target,
                                RuntimeProfile profile, int main_returns_void);

int native_runtime_assembly_profile(FILE *output, TargetFormat target,
                                    RuntimeProfile profile, int main_returns_void);

int native_runtime_object_imports_profile(NativeObject *object, TargetFormat target,
                                          RuntimeProfile profile);

int native_runtime_emit(NativeObject *object, TargetFormat target);

const char *native_runtime_import(const char *name, TargetFormat target);

int native_runtime_assembly(FILE *output, TargetFormat target);

int native_runtime_object_imports(NativeObject *object, TargetFormat target);
#endif
