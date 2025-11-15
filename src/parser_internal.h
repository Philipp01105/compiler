#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include <stddef.h>
#include "compiler_types.h"
#include "lexer.h"

const char *datatype_to_string(DataType type);
const char *get_register_for_type(DataType type, int reg_num);
const char **get_arg_registers_64();
const char **get_arg_registers_32();
const char **get_arg_registers_8();
int get_max_reg_args();
int get_stack_alignment();
int get_shadow_space();
int get_call_stack_space();
const char *escape_char_for_comment(const char *ch_value);
void escape_string_for_comment(const char *str, char *output, size_t output_size);
void generate_write_syscall(Parser *parser, const char *buffer_reg, const char *length_reg);
void generate_read_syscall(Parser *parser, const char *buffer_reg, const char *length_reg);
void generate_strlen_code(Parser *parser, const char *str_ptr_reg, const char *result_reg);
void generate_int_to_str_code(Parser *parser, const char *value_reg, const char *buffer_reg);
Parser *create_parser(TokenStream *tokens);
const char *parser_get_source_line(Parser *parser, int line);
void synchronize(Parser *parser);
void data_printf(Parser *parser, const char *format, ...);
void semantic_error(Parser *parser, int error_code, const char *format, ...);
void parse_printline_statement(Parser *parser);
int is_syscall_io_function(const char *name);
int is_builtin_string_function(const char *name);
int is_builtin_memory_function(const char *name);
int is_builtin_input_function(const char *name);

#endif
