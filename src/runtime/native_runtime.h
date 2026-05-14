#ifndef DMM_NATIVE_RUNTIME_H
#define DMM_NATIVE_RUNTIME_H
#include "native/object.h"
int native_runtime_emit(NativeObject *object, TargetFormat target);
const char *native_runtime_import(const char *name, TargetFormat target);
#endif
