/*
 * Internal parser functions shared across modules.
 * These are not part of the public parser API.
 */
#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include <stddef.h>
#include "compiler_types.h"

/* Type system helpers */
const char *datatype_to_string(DataType type);

const char *get_register_for_type(DataType type, int reg_num);

/* Platform-specific register accessors */
const char **get_arg_registers_64();

const char **get_arg_registers_32();

const char **get_arg_registers_8();

const char *get_arg_reg_64(int index);

const char *get_arg_reg_32(int index);

const char *get_arg_reg_8(int index);

/* Calling convention helpers */
int get_max_reg_args();

int get_stack_alignment();

int get_shadow_space();

int get_call_stack_space();

void set_target_format(TargetFormat target);

void generate_function_call(Parser *parser, Function *func, const char *name, int arg_count);

void convert_stack_value(Parser *parser, DataType from, DataType to);

void generate_system_io_call(Parser *parser, const char *operation, int keep_result);

/* Assembly code generation utilities */
const char *escape_char_for_comment(const char *ch_value);

void escape_string_for_comment(const char *str, char *output, size_t output_size);

void generate_write_syscall(Parser *parser, const char *buffer_reg, const char *length_reg);

void generate_read_syscall(Parser *parser, const char *buffer_reg, const char *length_reg);

void generate_strlen_code(Parser *parser, const char *str_ptr_reg, const char *result_reg);

void generate_int_to_str_code(Parser *parser, const char *value_reg, const char *buffer_reg);

void generate_stack_align(Parser *parser);

void generate_stack_restore(Parser *parser);

/* Parser core functions */
Parser *create_parser(TokenStream *tokens);

void parser_load_source(Parser *parser, const char *filename);

const char *parser_get_source_line(Parser *parser, int line);

void synchronize(Parser *parser);

void data_printf(Parser *parser, const char *format, ...);

void semantic_error(Parser *parser, int error_code, const char *format, ...);

/* Statement parsing */
void parse_printline_statement(Parser *parser);

/* Builtin function checks */
int is_syscall_io_function(const char *name);

int is_builtin_string_function(const char *name);

int is_builtin_memory_function(const char *name);

int is_builtin_input_function(const char *name);

#endif
