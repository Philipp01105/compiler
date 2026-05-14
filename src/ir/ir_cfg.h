#ifndef DMM_IR_CFG_H
#define DMM_IR_CFG_H
#include "ir.h"
int ir_verify_control_flow(const IrFunction *function, int implicit_void_return);
#endif
