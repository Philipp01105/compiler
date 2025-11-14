#ifndef PARSER_H
#define PARSER_H

#include "compiler_types.h"
#include "lexer.h"

// Parser creation/destruction
Parser *create_parser(TokenStream *tokens);
void free_parser(Parser *parser);

// Code generation
void code_printf(Parser *parser, const char *format, ...);
void code_comment(Parser *parser, const char *format, ...);

// Error handling
void parser_error(Parser *parser, const char *format, ...);
void parser_error_code(Parser *parser, int error_code, const char *format, ...);
void expect(Parser *parser, TokenType type, const char *message);
void semantic_error_at_token(Parser *parser, Token token, int error_code, const char *format, ...);

// Variable & function management
Variable *find_variable(Parser *parser, const char *name);
Function *find_function(Parser *parser, const char *name);
StructDefinition *find_struct(Parser *parser, const char *name);
EnumDefinition *find_enum(Parser *parser, const char *name);
int add_string_literal(Parser *parser, const char *text);
int add_float_literal(Parser *parser, const char *value);
void cleanup_scope(Parser *parser, int scope);

// Type helpers
const char *datatype_to_string(DataType type);
int datatype_size(DataType type);
DataType token_to_datatype(TokenType token);

// Parsing functions
int parse_program(Parser *parser, const char *source_file);
void parse_import(Parser *parser, const char *base_path);
void parse_struct(Parser *parser);
void parse_enum(Parser *parser);
void parse_function(Parser *parser);
void parse_function_body(Parser *parser);
void parse_statement(Parser *parser);
void parse_variable_declaration(Parser *parser);
void parse_assignment(Parser *parser);
void parse_for_loop(Parser *parser);
void parse_if_statement(Parser *parser);
void parse_return_statement(Parser *parser);
void parse_print_statement(Parser *parser);
void parse_function_call_statement(Parser *parser);
void parse_break_statement(Parser *parser);
void parse_continue_statement(Parser *parser);

// Expression parsing
void parse_expression(Parser *parser);
void parse_logical_or(Parser *parser);
void parse_logical_and(Parser *parser);
void parse_comparison(Parser *parser);
void parse_term(Parser *parser);
void parse_factor(Parser *parser);
void parse_unary(Parser *parser);
void parse_primary(Parser *parser);

#endif