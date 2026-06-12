#ifndef DMM_NATIVE_LINKER_H
#define DMM_NATIVE_LINKER_H
#include "object.h"

int native_link_executable(NativeObject *object, TargetFormat target, NativeBuffer *output);
#endif
