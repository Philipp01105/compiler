#ifndef DMM_NATIVE_ENCODER_H
#define DMM_NATIVE_ENCODER_H
#include "object.h"
#include "instruction.h"
int native_encode(NativeObject *object, const X64Instruction *instruction);
#endif
