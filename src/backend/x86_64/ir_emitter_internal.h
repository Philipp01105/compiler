#ifndef DMM_X86_64_IR_EMITTER_INTERNAL_H
#define DMM_X86_64_IR_EMITTER_INTERNAL_H

#include "ir_emitter.h"
#include "instruction.h"

typedef struct {
    FILE *output;
    size_t instruction_index;
    size_t current_ir_instruction;
    AstSourceSpan current_span;
    const AstProgram *current_program;
    int has_source;
    int failed;
} SourceMapWriter;

typedef struct {
    const IrModule *module;
    const IrFunction *function;
    TargetFormat target;
    SyntaxMode syntax;
    FILE *output;
    size_t function_index;
    size_t declaration_count;
    size_t frame_size;
    size_t current_label;
    size_t bounds_sequence;
    SourceMapWriter *source_map;
    NativeObject *native;
} Emitter;

int map_quoted(FILE *output, const char *text);
void write_labelf(const Emitter *emitter, const char *format, ...);
int type_is_structure(const IrModule *module, IrTypeId type_id);
int is_inline_structure(const IrModule *module, const IrInstruction *instruction);
size_t parameter_storage_slots(const IrFunction *function);
size_t type_slots(const IrModule *module, IrTypeId type_id);
void write_cstring(const Emitter *emitter, const char *text);
void write_quad(const Emitter *emitter, uint64_t value);
long long constant_value(const AstProgram *program, const IrInstruction *instruction);
int global_label(const SemanticSymbol *symbol, char *label, size_t size);
int global_drop_flag_label(const SemanticSymbol *symbol, char *label,
                           size_t size);
int global_slice_owner_label(const SemanticSymbol *symbol, char *label,
                             size_t size);
int emit_function(Emitter *emitter);

void write_x64_0(const Emitter *emitter, X64Opcode opcode);
void write_x64_1(const Emitter *emitter, X64Opcode opcode, X64Width width, X64Operand operand);
void write_x64_2(const Emitter *emitter, X64Opcode opcode, X64Width width, X64Operand destination, X64Operand source);
int is_integral(DataType type);
int is_floating(DataType type);
int is_numeric(DataType type);
int is_pointer_value(const IrInstruction *instruction);
const IrInstruction *producer(const IrFunction *function, size_t value);
const IrFunction *called_function(const IrModule *module, size_t symbol_id);
const IrFunction *addressed_function(const IrModule *module, size_t symbol_id);
void write_value_load(const Emitter *emitter, const char *reg, size_t value);
void write_value_store(const Emitter *emitter, const char *reg, size_t value);
void write_immediate(const Emitter *emitter, const char *reg, long long value);
void write_call(const Emitter *emitter, const char *name);
const char *argument_register(TargetFormat target, size_t index);
size_t physical_parameter_count(const IrFunction *function);
int physical_parameter(const IrFunction *function, size_t physical_index, size_t *source_index, int *is_length);
int physical_is_floating(const IrFunction *function, size_t physical_index);
size_t parameter_register_index(const IrFunction *function, TargetFormat target, size_t physical_index);
size_t stack_parameter_count(const IrFunction *function, TargetFormat target);
void normalize_integral_parameter(const Emitter *emitter, DataType type);
void load_floating_value(Emitter *emitter, size_t value, DataType target, unsigned xmm);
size_t aggregate_result_offset(const Emitter *emitter, const IrInstruction *result);
void copy_aggregate(Emitter *emitter, size_t slots, const char *source, const char *destination);
void convert_rax(Emitter *emitter, DataType from, DataType to);
int emit_string_compare(Emitter *emitter, const IrInstruction *instruction);
int emit_string_concat(Emitter *emitter, const IrInstruction *instruction);
int emit_binary(Emitter *emitter, const IrInstruction *instruction);
void normalize_truth_rax(Emitter *emitter, DataType type);
int emit_typed_call(Emitter *emitter, const IrInstruction *instruction, const IrFunction *callee,
                    int interface_receiver, size_t indirect_value);
int emit_indirect_typed_call(Emitter *emitter, const IrInstruction *instruction,
                             size_t callable_value, IrTypeId callable_type);
int emit_interface_call(Emitter *emitter, const IrInstruction *instruction);
void load_nullable_string(Emitter *emitter, const char *reg, size_t value);
int emit_builtin_call(Emitter *emitter, const IrInstruction *instruction, const char *name);

#endif
