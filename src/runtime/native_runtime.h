#ifndef DMM_NATIVE_RUNTIME_H
#define DMM_NATIVE_RUNTIME_H
#include "native/object.h"
#include <stdio.h>

int native_runtime_emit(NativeObject *object, TargetFormat target);

const char *native_runtime_import(const char *name, TargetFormat target);

int native_runtime_assembly(FILE *output, TargetFormat target);

int native_runtime_object_imports(NativeObject *object, TargetFormat target);
#endif
