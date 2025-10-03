#ifndef PARSER_H
#define PARSER_H

#include "compiler_types.h"
#include "lexer.h"
#include <stdarg.h>

// Parser State
typedef struct {
    TokenStream *tokens;

    // Symbol Tables
    Variable vars[MAX_VARS];
    int var_count;

    Function functions[MAX_FUNCTIONS];
    int function_count;

    StringLiteral string_literals[1000];
    int string_literal_count;
    int string_count;

    // Scope Management
    int current_scope;
    int stack_offset;

    // Code Generation Buffers
    char code_buffer[500000];
    int code_pos;

    char function_code_buffer[500000];
    int function_code_pos;

    // State
    int label_counter;
    int current_line;
    int main_function_found;
    int has_error;

    // Debug
    int debug_mode;
} Parser;

// Parser Creation/Destruction
Parser *create_parser(TokenStream *tokens);
void free_parser(Parser *parser);

// Main Parsing Functions
int parse_program(Parser *parser);
void parse_function(Parser *parser);
void parse_statement(Parser *parser);
void parse_for_loop(Parser *parser);
void parse_var_declaration(Parser *parser);
void parse_assignment(Parser *parser, Token var_name);
void parse_return(Parser *parser);
void parse_print(Parser *parser);
void parse_function_call(Parser *parser, Token func_name);

// Expression Parsing
DataType parse_expression(Parser *parser);
DataType parse_comparison(Parser *parser);
DataType parse_term(Parser *parser);
DataType parse_factor(Parser *parser);
DataType parse_primary(Parser *parser);

// Helper Functions
void code_printf(Parser *parser, const char *format, ...);
int add_string_literal(Parser *parser, const char *text);
int create_variable(Parser *parser, const char *name, DataType type);
int get_var_offset(Parser *parser, const char *name);
DataType get_var_type(Parser *parser, const char *name);
void enter_scope(Parser *parser);
void exit_scope(Parser *parser);

// Error Handling
void parser_error(Parser *parser, const char *format, ...);
void parser_error_at_token(Parser *parser, Token token, const char *format, ...);

// Utility
Token expect(Parser *parser, TokenType type, const char *message);
int match_any(Parser *parser, int count, ...);
DataType token_to_datatype(Token token);

#endif