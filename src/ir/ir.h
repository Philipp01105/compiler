#ifndef DMM_IR_H
#define DMM_IR_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "ast.h"
#include "semantic.h"

#define IR_VALUE_NONE ((size_t)-1)
#define IR_TYPE_NONE ((size_t)-1)

typedef size_t IrTypeId;

typedef enum {
    IR_TYPE_PRIMITIVE,
    IR_TYPE_NAMED,
    IR_TYPE_POINTER,
    IR_TYPE_ARRAY,
    IR_TYPE_SLICE
} IrTypeKind;

typedef struct {
    IrTypeKind kind;
    DataType primitive;
    size_t symbol_id;
    IrTypeId element_type;
    size_t array_length;
} IrType;

typedef struct {
    size_t size;
    size_t alignment;
    size_t storage_slots;
} IrTypeLayout;

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
    IR_OP_SLICE_LENGTH,
    IR_OP_CAST,
    IR_OP_ALLOC,
    IR_OP_FREE,
    IR_OP_RETURN,
    IR_OP_BRANCH,
    IR_OP_JUMP,
    IR_OP_LABEL,
    IR_OP_PHI, IR_OP_ENUM_CONSTRUCT, IR_OP_ENUM_IS, IR_OP_ENUM_PAYLOAD, IR_OP_TRAP,
    IR_OP_SLICE, IR_OP_SLICE_DATA, IR_OP_ARRAY_LITERAL,
    /* Ownership effects are explicit so optimization and code generation
       preserve exactly-once destruction. */
    IR_OP_DROP, IR_OP_MOVE, IR_OP_REINIT, IR_OP_FREE_SLICE_BACKING
} IrOpcode;

typedef struct {
    IrOpcode opcode;
    AstSourceSpan span;
    DataType type;
    IrTypeId type_id;
    unsigned pointer_depth;
    size_t type_name_token;
    int is_array;
    int is_slice;
    int owns_slice_backing;
    size_t result;
    size_t operand_a;
    size_t operand_b;
    size_t auxiliary_token;
    size_t symbol_id;
    size_t first_argument;
    size_t argument_count;
    size_t target_a;
    size_t target_b;
    size_t enum_payload_index;
    /* Materialized element count for context-typed array/slice literals. */
    size_t element_count;
    TokenType operator_type;
    /* Compiler-owned numeric literal; source tokens remain immutable. */
    int has_immediate;
    uint64_t immediate;
    /* A dominating fixed-array access has already checked this SSA index. */
    int bounds_check_elided;
} IrInstruction;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    DataType type;
    IrTypeId type_id;
    unsigned pointer_depth;
    size_t type_name_token;
    int is_array;
    int is_slice;
    int is_receiver;
} IrParameter;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t owner_token;
    size_t owner_symbol_id;
    size_t symbol_id;
    AstType return_type;
    IrTypeId return_type_id;
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
    int is_drop_glue;
    int is_package_cleanup;
} IrFunction;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    IrTypeId type_id;
} IrFieldDefinition;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    IrFieldDefinition *fields;
    size_t field_count;
    unsigned type_properties;
    int has_explicit_destructor;
} IrAggregate;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    size_t first_argument;
    size_t argument_count;
    IrTypeId *payload_types;
    size_t payload_count;
} IrEnumVariant;

typedef struct {
    size_t token;
    IrTypeId type_id;
    int negative;
} IrEnumArgument;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    IrFieldDefinition *fields;
    size_t field_count;
    IrEnumVariant *variants;
    size_t variant_count;
    IrEnumArgument *variant_arguments;
    size_t variant_argument_count;
    int is_sum;
} IrEnum;

typedef struct {
    const AstProgram *source_program;
    size_t symbol_id;
    size_t path_token;
    size_t path_first_token;
    size_t path_token_count;
    const AstProgram *resolved_program;
} IrImport;

typedef struct {
    const AstProgram *program;
    const SemanticModel *semantics;
    IrFunction *functions;
    size_t function_count;
    size_t function_capacity;
    IrType *types;
    size_t type_count;
    size_t type_capacity;
    IrAggregate *structures;
    size_t structure_count;
    size_t structure_capacity;
    IrEnum *enums;
    size_t enum_count;
    size_t enum_capacity;
    IrImport *imports;
    size_t import_count;
    size_t import_capacity;
    struct IrGlobal *globals;
    size_t global_count;
    int verified;
    int optimized;
} IrModule;

typedef struct IrGlobal {
    const AstProgram *source_program;
    size_t symbol_id;
    IrTypeId type_id;
    uint64_t bits;
    const char *string;
    const AstExpression *array_literal;
    size_t literal_element_count;
} IrGlobal;

IrModule *ir_lower_program(const AstProgram *program, const SemanticModel *semantics);

void ir_module_free(IrModule *module);

int ir_verify_module(const IrModule *module);

int ir_dump(FILE *output, const IrModule *module);

int ir_dump_function(FILE *output, const IrModule *module, size_t function_index);

int ir_type_layout(const IrModule *module, IrTypeId type, IrTypeLayout *layout);

unsigned ir_type_properties(const IrModule *module, IrTypeId type);

/* Internal failures retain the concrete instruction and its original source unit. */
void ir_report_failure(const IrFunction *function, size_t instruction_index,
                       const char *stage, const char *reason);

#endif
