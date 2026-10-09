#ifndef DMM_X86_64_IR_NAMES_H
#define DMM_X86_64_IR_NAMES_H

#include "ir.h"

const IrParameter *function_receiver(const IrFunction *function);

int mangle_append(char *buffer, size_t buffer_size, size_t *used, const char *text);

const char *function_link_name(const IrModule *module, const IrFunction *function,
                               char *buffer, size_t buffer_size);

int valid_module(const IrModule *module);

const char *function_address_link_name(const IrModule *module, size_t symbol,
                                       char *buffer, size_t buffer_size);

#endif
