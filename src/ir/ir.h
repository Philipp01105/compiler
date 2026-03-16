#ifndef DMM_IR_H
#define DMM_IR_H

#include <stddef.h>

#include "ast.h"
#include "semantic.h"

#define IR_VALUE_NONE ((size_t)-1)

typedef enum {
    IR_OP_CONSTANT,
    IR_OP_LOAD,
    IR_OP_DECLARE,
    IR_OP_STORE,
    IR_OP_UNARY,
    IR_OP_BINARY,
    IR_OP_CALL,
    IR_OP_INDEX,
    IR_OP_MEMBER,
    IR_OP_RESERVE,
    IR_OP_PRINT,
    IR_OP_RETURN,
    IR_OP_BRANCH,
    IR_OP_JUMP,
    IR_OP_LABEL
} IrOpcode;

typedef struct {
    IrOpcode opcode;
    AstSourceSpan span;
    DataType type;
    unsigned pointer_depth;
    size_t type_name_token;
    int is_array;
    size_t result;
    size_t operand_a;
    size_t operand_b;
    size_t auxiliary_token;
    size_t symbol_id;
    size_t first_argument;
    size_t argument_count;
    size_t target_a;
    size_t target_b;
    TokenType operator_type;
} IrInstruction;

typedef struct {
    size_t name_token;
    size_t symbol_id;
    DataType type;
    unsigned pointer_depth;
    size_t type_name_token;
    int is_array;
} IrParameter;

typedef struct {
    size_t name_token;
    size_t owner_token;
    size_t symbol_id;
    AstType return_type;
    IrParameter *parameters;
    size_t parameter_count;
    IrInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;
    size_t *arguments;
    size_t argument_count;
    size_t argument_capacity;
    size_t next_value;
    size_t next_label;
} IrFunction;

typedef struct {
    const AstProgram *program;
    const SemanticModel *semantics;
    IrFunction *functions;
    size_t function_count;
    size_t function_capacity;
    int verified;
} IrModule;

IrModule *ir_lower_program(const AstProgram *program, const SemanticModel *semantics);
IrModule *ir_create_compatibility_module(const AstProgram *program);
void ir_module_free(IrModule *module);
int ir_verify_module(const IrModule *module);

#endif
