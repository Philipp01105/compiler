#ifndef DMM_IR_VERIFY_H
#define DMM_IR_VERIFY_H

#include "ir.h"

/* Report verification failures while lowering a source program. */
int ir_verify_module_report(const IrModule *module);
DataType ir_ast_type_data_type(const AstProgram *program, const AstType *type);

#endif
