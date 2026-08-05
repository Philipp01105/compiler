#ifndef DMM_IR_H
#define DMM_IR_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "ast.h"
#include "semantic.h"
#include "runtime_profile.h"

#define IR_VALUE_NONE ((size_t)-1)
#define IR_TYPE_NONE ((size_t)-1)

typedef size_t IrTypeId;

typedef enum {
    IR_TYPE_PRIMITIVE,
    IR_TYPE_NAMED,
    IR_TYPE_POINTER,
    IR_TYPE_ARRAY,
    IR_TYPE_SLICE,
    IR_TYPE_FUNCTION,
    IR_TYPE_FUTURE, IR_TYPE_JOIN, IR_TYPE_EXECUTOR
} IrTypeKind;

typedef struct {
    IrTypeKind kind;
    DataType primitive;
    size_t symbol_id;
    IrTypeId element_type;
    size_t array_length;
    size_t signature_id;
} IrType;

typedef struct {
    IrTypeId *parameter_types;
    size_t parameter_count;
    IrTypeId return_type;
    int is_native;
} IrFunctionSignature;

typedef struct {
    size_t size;
    size_t alignment;
    size_t storage_slots;
} IrTypeLayout;

typedef enum {
    IR_OP_CONSTANT,
    IR_OP_FUNCTION_ADDRESS,
    IR_OP_LOAD,
    IR_OP_DECLARE,
    IR_OP_STORE,
    IR_OP_INTERFACE_PACK,
    IR_OP_UNARY,
    IR_OP_BINARY,
    IR_OP_CALL,
    IR_OP_INDEX,
    IR_OP_SUBSLICE,
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
    IR_OP_DROP, IR_OP_MOVE, IR_OP_REINIT, IR_OP_FREE_SLICE_BACKING,
    /* A suspend point polls operand_a at a stable frame address. target_a is
       its unique resume state; completion consumes the child exactly once. */
    IR_OP_AWAIT, IR_OP_EXECUTOR, IR_OP_CANCEL_CHECK, IR_OP_CANCEL_AWAIT,
    IR_OP_CANCEL_DROP, IR_OP_CANCEL_RETURN,
    /* Snapshot C aggregates at argument evaluation, before later arguments
       can mutate their source through native pointers. */
    IR_OP_NATIVE_COPY
} IrOpcode;

typedef struct {
    const AstProgram *source_program;
    IrOpcode opcode;
    /* Lowering attaches requirements to future platform-specific operations. */
    RuntimeRequirements runtime_requirements;
    AstAsyncOperation async_operation;
    int async_cleanup;
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
    /* Semantic interface method implemented by this addressable dispatch thunk. */
    size_t interface_thunk_symbol_id;
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
    int emission_reachable;
    int is_package_init;
    int is_package_cleanup;
    int is_async;
    int is_native_export;
    IrTypeId future_type_id;
    size_t async_state_count;
    int async_frame_pinned;
    unsigned async_frame_properties;
    size_t async_cancel_entry;
} IrFunction;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    IrTypeId type_id;
    size_t native_offset;
    size_t native_array_stride;
} IrFieldDefinition;

typedef struct {
    const AstProgram *source_program;
    size_t name_token;
    size_t symbol_id;
    IrFieldDefinition *fields;
    size_t field_count;
    unsigned type_properties;
    int has_explicit_destructor;
    int is_native;
    int is_opaque;
    int is_native_union;
    size_t native_pack;
    size_t native_alignment;
    NativeTypeLayout native_layout;
} IrAggregate;

typedef struct {
    const AstProgram *source_program;
    AstSourceSpan span;
    size_t symbol_id;
    const char *abi;
    const char *library;
    const char *native_name;
    IrTypeId return_type_id;
    IrTypeId *parameter_types;
    size_t parameter_count;
} IrNativeImport;

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
    TargetFormat target_format;
    IrNativeImport *native_imports;
    size_t native_import_count;
    IrFunction *functions;
    size_t function_count;
    size_t function_capacity;
    IrType *types;
    size_t type_count;
    size_t type_capacity;
    IrFunctionSignature *signatures;
    size_t signature_count;
    size_t signature_capacity;
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
    /* Operation requirements are populated by lowering as platform intrinsics
       are introduced. Profile is an emission view, not a linker decision. */
    RuntimeRequirements runtime_requirements;
    RuntimeProfile runtime_profile;
    int verified;
    int optimized;
    int emission_selected;
} IrModule;

const IrNativeImport *ir_native_import(const IrModule *module, size_t symbol_id);
/* Inspect the selected IR after optimization: discarded operations do not
   require runtime facilities. Current operations (including async) are zero. */
static inline RuntimeRequirements ir_runtime_requirements(const IrModule *module) {
    RuntimeRequirements result = module->runtime_requirements;
    for (size_t f = 0; f < module->function_count; ++f)
        if(!module->emission_selected || module->functions[f].emission_reachable)
        for (size_t i = 0; i < module->functions[f].instruction_count; ++i)
            result |= module->functions[f].instructions[i].runtime_requirements;
    return runtime_requirements_normalize(result);
}
void ir_select_runtime_functions(IrModule *module);

typedef struct IrGlobal {
    const AstProgram *source_program;
    size_t symbol_id;
    IrTypeId type_id;
    uint64_t bits;
    const char *string;
    const AstExpression *array_literal;
    const AstExpression *runtime_initializer;
    size_t literal_element_count;
    size_t function_symbol_id;
    int owns_slice_backing;
} IrGlobal;

IrModule *ir_lower_program(const AstProgram *program, const SemanticModel *semantics);

void ir_module_free(IrModule *module);

int ir_verify_module(const IrModule *module);
int ir_main_returns_void(const IrModule *module);

int ir_dump(FILE *output, const IrModule *module);

int ir_dump_function(FILE *output, const IrModule *module, size_t function_index);

int ir_type_layout(const IrModule *module, IrTypeId type, IrTypeLayout *layout);

unsigned ir_type_properties(const IrModule *module, IrTypeId type);

/* Stable, nonzero wire identity for a concrete aggregate in an interface value. */
uint64_t ir_interface_type_tag(const IrModule *module, size_t symbol_id);

/* Internal failures retain the concrete instruction and its original source unit. */
void ir_report_failure(const IrFunction *function, size_t instruction_index,
                       const char *stage, const char *reason);

static inline int ir_native_import_used(const IrModule *module, size_t symbol_id) {
    for (size_t g = 0; g < module->global_count; ++g)
        if (module->globals[g].function_symbol_id == symbol_id) return 1;
    for (size_t f = 0; f < module->function_count; ++f) {
        const IrFunction *function = &module->functions[f];
        if (module->emission_selected && !function->emission_reachable) continue;
        for (size_t i = 0; i < function->instruction_count; ++i) {
            const IrInstruction *in = &function->instructions[i];
            if ((in->opcode == IR_OP_CALL || in->opcode == IR_OP_FUNCTION_ADDRESS) &&
                in->symbol_id == symbol_id) return 1;
        }
    }
    return 0;
}

#endif
