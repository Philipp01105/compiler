#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ============================================================================
// TYPE HELPER FUNCTIONS
// ============================================================================

const char *datatype_to_string(DataType type) {
    switch (type) {
        case TYPE_INT: return "int";
        case TYPE_CHAR: return "char";
        case TYPE_BYTE: return "byte";
        case TYPE_BIT: return "bit";
        case TYPE_FLOAT: return "float";
        case TYPE_DOUBLE: return "double";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        default: return "unknown";
    }
}

int datatype_size(DataType type) {
    switch (type) {
        case TYPE_BIT: return 1;
        case TYPE_CHAR: return 1;
        case TYPE_BYTE: return 1;
        case TYPE_INT: return 4;
        case TYPE_FLOAT: return 4;
        case TYPE_DOUBLE: return 8;
        case TYPE_STRING: return 8;
        default: return 4;
    }
}

DataType token_to_datatype(TokenType token) {
    switch (token) {
        case TOKEN_TYPE_INT: return TYPE_INT;
        case TOKEN_TYPE_CHAR: return TYPE_CHAR;
        case TOKEN_TYPE_BYTE: return TYPE_BYTE;
        case TOKEN_TYPE_BIT: return TYPE_BIT;
        case TOKEN_TYPE_FLOAT: return TYPE_FLOAT;
        case TOKEN_TYPE_DOUBLE: return TYPE_DOUBLE;
        case TOKEN_TYPE_STRING: return TYPE_STRING;
        case TOKEN_TYPE_VOID: return TYPE_VOID;
        default: return TYPE_UNKNOWN;
    }
}

const char *get_register_for_type(DataType type, int reg_num) {
    switch (type) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
            if (reg_num == 0) return "%xmm0";
            else if (reg_num == 1) return "%xmm1";
            else return "%xmm2";

        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_BIT:
            if (reg_num == 0) return "%al";
            else if (reg_num == 1) return "%bl";
            else return "%cl";

        case TYPE_INT:
        default:
            if (reg_num == 0) return "%eax";
            else if (reg_num == 1) return "%ebx";
            else return "%ecx";
    }
}

// ============================================================================
// PARSER CREATION & DESTRUCTION
// ============================================================================

Parser *create_parser(TokenStream *tokens) {
    Parser *parser = malloc(sizeof(Parser));

    parser->tokens = tokens;
    parser->var_count = 0;
    parser->function_count = 0;
    parser->string_literal_count = 0;
    parser->float_literal_count = 0;
    parser->code_pos = 0;
    parser->function_code_pos = 0;
    parser->current_scope = 0;
    parser->scope_depth = 0;
    parser->has_error = 0;
    parser->debug_mode = 0;
    parser->label_counter = 0;
    parser->loop_counter = 0;

    memset(parser->code_buffer, 0, CODE_BUFFER_SIZE);
    memset(parser->function_code_buffer, 0, CODE_BUFFER_SIZE);

    return parser;
}

void free_parser(Parser *parser) {
    if (parser) {
        free(parser);
    }
}

// ============================================================================
// CODE GENERATION HELPERS
// ============================================================================

void code_printf(Parser *parser, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    int len = strlen(buffer);
    if (parser->function_code_pos + len < CODE_BUFFER_SIZE) {
        strcpy(parser->function_code_buffer + parser->function_code_pos, buffer);
        parser->function_code_pos += len;
    }

    va_end(args);
}

void code_comment(Parser *parser, const char *format, ...) {
    if (!parser->debug_mode) {
        return;
    }

    va_list args;
    va_start(args, format);

    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);

    code_printf(parser, "    # %s\n", buffer);

    va_end(args);
}

// ============================================================================
// ERROR HANDLING
// ============================================================================

void parser_error(Parser *parser, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    fprintf(stderr, "Error (Line %d, Col %d): ", current.line, current.column);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fprintf(stderr, "  At token: %s '%s'\n",
            token_type_to_string(current.type), current.value);

    if (!is_at_end(parser->tokens)) {
        consume(parser->tokens);
    }
}

void expect(Parser *parser, TokenType type, const char *message) {
    if (!check(parser->tokens, type)) {
        parser_error(parser, "%s", message);
        return;
    }
    consume(parser->tokens);
}

// ============================================================================
// VARIABLE & FUNCTION MANAGEMENT
// ============================================================================

Variable *find_variable(Parser *parser, const char *name) {
    for (int i = parser->var_count - 1; i >= 0; i--) {
        if (parser->vars[i].scope == -1) continue;
        if (strcmp(parser->vars[i].name, name) == 0) {
            return &parser->vars[i];
        }
    }
    return NULL;
}

Function *find_function(Parser *parser, const char *name) {
    for (int i = 0; i < parser->function_count; i++) {
        if (strcmp(parser->functions[i].name, name) == 0) {
            return &parser->functions[i];
        }
    }
    return NULL;
}

int add_string_literal(Parser *parser, const char *text) {
    for (int i = 0; i < parser->string_literal_count; i++) {
        if (strcmp(parser->string_literals[i].text, text) == 0) {
            return parser->string_literals[i].id;
        }
    }

    if (parser->string_literal_count >= MAX_STRING_LITERALS) {
        parser_error(parser, "Too many string literals (max %d)", MAX_STRING_LITERALS);
        return -1;
    }

    int id = parser->string_literal_count;
    parser->string_literals[id].id = id;
    strncpy(parser->string_literals[id].text, text, MAX_LINE - 1);
    parser->string_literals[id].text[MAX_LINE - 1] = '\0';
    parser->string_literal_count++;

    return id;
}

int add_float_literal(Parser *parser, const char *value) {
    for (int i = 0; i < parser->float_literal_count; i++) {
        if (strcmp(parser->float_literals[i].value, value) == 0) {
            return parser->float_literals[i].id;
        }
    }

    if (parser->float_literal_count >= MAX_FLOAT_LITERALS) {
        parser_error(parser, "Too many float literals (max %d)", MAX_FLOAT_LITERALS);
        return -1;
    }

    int id = parser->float_literal_count;
    parser->float_literals[id].id = id;
    strncpy(parser->float_literals[id].value, value, MAX_TOKEN - 1);
    parser->float_literals[id].value[MAX_TOKEN - 1] = '\0';
    parser->float_literal_count++;

    return id;
}

void cleanup_scope(Parser *parser, int scope) {
    int removed = 0;
    for (int i = parser->var_count - 1; i >= 0; i--) {
        if (parser->vars[i].scope == scope) {
            parser->vars[i].scope = -1;
            removed++;
        } else {
            break;
        }
    }

    if (parser->debug_mode && removed > 0) {
        code_comment(parser, "Cleanup: %d variable(s) from scope %d", removed, scope);
    }
}

// ============================================================================
// PROGRAM PARSING
// ============================================================================

int parse_program(Parser *parser) {
    printf("\n================================================================\n");
    printf("           PHASE 2: SYNTAX ANALYSIS (PARSER)\n");
    printf("================================================================\n\n");

    while (!is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            parse_function(parser);
        } else {
            parser_error(parser, "Only functions allowed at top level");
            consume(parser->tokens);
        }
    }

    if (parser->has_error) {
        return 0;
    }

    Function *main_func = find_function(parser, "main");
    if (!main_func) {
        fprintf(stderr, "Error: No main() function found\n");
        return 0;
    }

    printf("✓ Parsing successful\n");
    printf("  Functions: %d\n", parser->function_count);
    printf("  String literals: %d\n", parser->string_literal_count);
    printf("  Float literals: %d\n", parser->float_literal_count);

    return 1;
}

// ============================================================================
// FUNCTION PARSING
// ============================================================================

/**
 * Parse a function declaration including:
 * - Function name
 * - Parameters with types
 * - Return type
 * - Function body
 *
 * Handles parameter passing via registers (Windows x64 calling convention):
 * - Integer/Pointer parameters: RCX, RDX, R8, R9
 * - Float parameters: XMM0, XMM1, XMM2, XMM3
 */
void parse_function(Parser *parser) {
    Token func_token = peek(parser->tokens);
    consume(parser->tokens);

    Token name_token = consume(parser->tokens);
    char func_name[MAX_TOKEN];
    strcpy(func_name, name_token.value);

    if (parser->function_count >= MAX_FUNCTIONS) {
        parser_error(parser, "Too many functions (max %d)", MAX_FUNCTIONS);
        return;
    }

    Function *func = &parser->functions[parser->function_count];
    strcpy(func->name, func_name);
    func->param_count = 0;
    parser->function_count++;

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

    int saved_var_count = parser->var_count;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token param_token = consume(parser->tokens);

        if (func->param_count >= 10) {
            parser_error(parser, "Too many parameters (max 10)");
            return;
        }

        strcpy(func->params[func->param_count], param_token.value);

        expect(parser, TOKEN_COLON, "Expected ':' after parameter");

        Token type_token = consume(parser->tokens);
        DataType param_type = token_to_datatype(type_token.type);

        if (param_type == TYPE_UNKNOWN) {
            parser_error(parser, "Unknown parameter type");
            return;
        }

        func->param_types[func->param_count] = param_type;

        if (parser->var_count >= MAX_VARS) {
            parser_error(parser, "Too many variables");
            return;
        }

        Variable *var = &parser->vars[parser->var_count];
        strcpy(var->name, param_token.value);
        var->type = param_type;
        var->size = datatype_size(param_type);
        int param_offset = 0;
        for (int j = 0; j < func->param_count; j++) {
            param_offset += datatype_size(func->param_types[j]);
        }
        var->offset = -(param_offset + datatype_size(param_type));

        if (datatype_size(param_type) == 8 && var->offset % 8 != 0) {
            var->offset -= (8 - ((-var->offset) % 8));
        }
        var->scope = 1;
        parser->var_count++;

        func->param_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after parameters");
    expect(parser, TOKEN_ARROW, "Expected '->' before return type");

    Token return_type_token = consume(parser->tokens);
    DataType return_type = token_to_datatype(return_type_token.type);

    if (return_type == TYPE_UNKNOWN) {
        parser_error(parser, "Unknown return type");
        return;
    }

    func->return_type = return_type;

    code_comment(parser, "========================================");
    code_comment(parser, "Function: %s (Line %d)", func_name, func_token.line);
    code_comment(parser, "Parameters: %d, Return: %s",
                 func->param_count,
                 datatype_to_string(func->return_type));
    code_comment(parser, "========================================");

    if (strcmp(func_name, "main") == 0) {
        code_printf(parser, ".globl main\n");
        code_printf(parser, "main:\n");
    } else {
        code_printf(parser, ".globl %s\n", func_name);
        code_printf(parser, "%s:\n", func_name);
    }

    code_comment(parser, "Function prologue");
    code_printf(parser, "    pushq %%rbp\n");
    code_printf(parser, "    movq %%rsp, %%rbp\n");
    code_printf(parser, "    subq $8192, %%rsp\n");

    if (func->param_count > 0) {
        code_comment(parser, "Save parameters to stack");
    }

    /**
     * Windows x64 calling convention parameter passing:
     * - 64-bit registers for pointers (strings): RCX, RDX, R8, R9
     * - 32-bit register names for integers: ECX, EDX, R8D, R9D
     * - Float registers: XMM0-XMM3
     *
     * Critical: Must use correct register size to avoid data corruption!
     */
    const char *param_regs_64[] = {"%rcx", "%rdx", "%r8", "%r9"};
    const char *param_regs_32[] = {"%ecx", "%edx", "%r8d", "%r9d"};
    const char *param_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

    for (int i = 0; i < func->param_count && i < 4; i++) {
        Variable *var = &parser->vars[saved_var_count + i];

        if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
            if (var->type == TYPE_FLOAT) {
                code_printf(parser, "    movss %s, %d(%%rbp)\n", param_regs_float[i], var->offset);
            } else {
                code_printf(parser, "    movsd %s, %d(%%rbp)\n", param_regs_float[i], var->offset);
            }
        } else if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
            if (i == 0) {
                code_printf(parser, "    movb %%cl, %d(%%rbp)\n", var->offset);
            } else if (i == 1) {
                code_printf(parser, "    movb %%dl, %d(%%rbp)\n", var->offset);
            } else if (i == 2) {
                code_printf(parser, "    movb %%r8b, %d(%%rbp)\n", var->offset);
            } else if (i == 3) {
                code_printf(parser, "    movb %%r9b, %d(%%rbp)\n", var->offset);
            }
        } else if (var->type == TYPE_STRING) {
            code_printf(parser, "    movq %s, %d(%%rbp)\n", param_regs_64[i], var->offset);
        } else {
            code_printf(parser, "    movl %s, %d(%%rbp)\n", param_regs_32[i], var->offset);
        }
    }

    parser->current_scope = 1;
    parser->scope_depth = 1;

    parse_function_body(parser);

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = 0;
    parser->scope_depth = 0;
    parser->var_count = saved_var_count;

    if (func->return_type == TYPE_VOID) {
        code_comment(parser, "Function epilogue (void return)");
        code_printf(parser, "    leave\n");
        code_printf(parser, "    ret\n");
    }

    code_printf(parser, "\n");
}

void parse_function_body(Parser *parser) {
    expect(parser, TOKEN_LBRACE, "Expected '{' at start of function body");

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Expected '}' at end of function body");
}

// ============================================================================
// STATEMENT PARSING
// ============================================================================

void parse_statement(Parser *parser) {
    if (check(parser->tokens, TOKEN_KEYWORD_VAR)) {
        parse_variable_declaration(parser);
    } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token lookahead = peek_ahead(parser->tokens, 1);
        if (lookahead.type == TOKEN_EQUAL || lookahead.type == TOKEN_PLUS_EQUAL ||
            lookahead.type == TOKEN_MINUS_EQUAL || lookahead.type == TOKEN_STAR_EQUAL ||
            lookahead.type == TOKEN_SLASH_EQUAL || lookahead.type == TOKEN_PLUS_PLUS ||
            lookahead.type == TOKEN_MINUS_MINUS) {
            parse_assignment(parser);
        } else if (lookahead.type == TOKEN_LPAREN) {
            parse_function_call_statement(parser);
        } else {
            parser_error(parser, "Unexpected token after identifier");
        }
    } else if (check(parser->tokens, TOKEN_KEYWORD_FOR)) {
        parse_for_loop(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_IF)) {
        parse_if_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_RETURN)) {
        parse_return_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_PRINT)) {
        parse_print_statement(parser);
    } else {
        parser_error(parser, "Unexpected statement");
    }
}

/**
 * Parse variable declarations with optional type inference and initialization
 *
 * Syntax variations:
 * - var x = 42;              (type inference from initializer)
 * - var x:int;               (explicit type, uninitialized)
 * - var x:int = 42;          (explicit type with initializer)
 * - var name:string = "Hi";  (string literal assignment)
 *
 * Stack layout considerations:
 * - Variables are allocated on the stack with proper alignment
 * - Strings (8 bytes) require 8-byte alignment
 * - Offsets are calculated cumulatively based on actual sizes
 */
void parse_variable_declaration(Parser *parser) {
    Token var_token = peek(parser->tokens);
    consume(parser->tokens);
    Token name_token = consume(parser->tokens);

    DataType var_type = TYPE_INT;
    int has_explicit_type = 0;

    if (check(parser->tokens, TOKEN_COLON)) {
        consume(parser->tokens);

        Token type_token = consume(parser->tokens);
        var_type = token_to_datatype(type_token.type);

        if (var_type == TYPE_UNKNOWN) {
            parser_error(parser, "Unknown variable type '%s'", type_token.value);
            return;
        }

        has_explicit_type = 1;
    }

    if (check(parser->tokens, TOKEN_EQUAL)) {
        consume(parser->tokens);

        if (var_type == TYPE_STRING && check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_comment(parser, "Line %d: var %s:string = \"%s\"",
                         var_token.line, name_token.value, str_token.value);

            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    pushq %%rax\n");
        } else {
            code_comment(parser, "Line %d: var %s:%s = ...",
                         var_token.line, name_token.value, datatype_to_string(var_type));
            parse_expression(parser);
        }
    } else {
        if (!has_explicit_type) {
            parser_error(parser, "Variable '%s' without type and without initialization", name_token.value);
            return;
        }

        code_comment(parser, "Line %d: var %s:%s (uninitialized)",
                     var_token.line, name_token.value, datatype_to_string(var_type));

        if (var_type == TYPE_FLOAT || var_type == TYPE_DOUBLE) {
            code_printf(parser, "    xorps %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %%rax\n");
            code_printf(parser, "    pushq %%rax\n");
        } else {
            code_printf(parser, "    xorq %%rax, %%rax\n");
            code_printf(parser, "    pushq %%rax\n");
        }
    }

    expect(parser, TOKEN_SEMICOLON, "Expected ';' at end of variable declaration");

    if (parser->var_count >= MAX_VARS) {
        parser_error(parser, "Too many variables (max %d)", MAX_VARS);
        return;
    }

    Variable *var = &parser->vars[parser->var_count];
    strcpy(var->name, name_token.value);
    var->type = var_type;
    var->size = datatype_size(var_type);

    /**
     * Calculate stack offset with proper alignment:
     * - Sum actual byte sizes of all existing variables
     * - Align 8-byte types (string, double) to 8-byte boundaries
     * - This ensures proper memory access and prevents corruption
     */
    int total_bytes = 0;
    for (int i = 0; i < parser->var_count; i++) {
        if (parser->vars[i].scope >= 1) {
            total_bytes += parser->vars[i].size;
        }
    }

    if (var->size == 8) {
        if (total_bytes % 8 != 0) {
            total_bytes += (8 - (total_bytes % 8));
        }
    }

    var->offset = -(total_bytes + var->size);
    var->scope = parser->current_scope;
    parser->var_count++;

    if (var_type == TYPE_FLOAT) {
        code_printf(parser, "    movss (%%rsp), %%xmm0\n");
        code_printf(parser, "    addq $8, %%rsp\n");
        code_printf(parser, "    movss %%xmm0, %d(%%rbp)\n", var->offset);
    } else if (var_type == TYPE_DOUBLE) {
        code_printf(parser, "    movsd (%%rsp), %%xmm0\n");
        code_printf(parser, "    addq $8, %%rsp\n");
        code_printf(parser, "    movsd %%xmm0, %d(%%rbp)\n", var->offset);
    } else if (var_type == TYPE_CHAR || var_type == TYPE_BYTE || var_type == TYPE_BIT) {
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movb %%al, %d(%%rbp)\n", var->offset);
    } else if (var_type == TYPE_STRING) {
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
    } else {
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
    }
}

/**
 * Parse assignment statements including:
 * - Simple assignment: x = 5;
 * - Compound assignment: x += 5; x -= 3; x *= 2; x /= 4;
 * - Increment/Decrement: x++; x--;
 * - String assignment: name = "Hello";
 * - String function assignment: name = getGreeting();  // NEW!
 *
 * Strings are assigned by pointer copy (shallow copy)
 */
void parse_assignment(Parser *parser) {
    Token assign_token = peek(parser->tokens);
    Token name_token = consume(parser->tokens);
    Variable *var = find_variable(parser, name_token.value);

    if (!var) {
        parser_error(parser, "Variable '%s' not found", name_token.value);
        return;
    }

    TokenType op = peek(parser->tokens).type;

    // ========== STRING ASSIGNMENT ==========
    if (var->type == TYPE_STRING) {
        if (op != TOKEN_EQUAL) {
            parser_error(parser, "Compound assignment not allowed for strings");
            return;
        }

        consume(parser->tokens);  // '='

        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            // String literal assignment: name = "Hello";
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_comment(parser, "Line %d: %s = \"%s\"",
                         assign_token.line, name_token.value, str_token.value);

            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);

        } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token src_name = peek(parser->tokens);
            Token lookahead = peek_ahead(parser->tokens, 1);

            // Check if it's a function call
            if (lookahead.type == TOKEN_LPAREN) {
                // String function call: name = getGreeting();
                consume(parser->tokens);  // identifier

                Function *func = find_function(parser, src_name.value);
                if (!func) {
                    parser_error(parser, "Function '%s' not found", src_name.value);
                    return;
                }

                if (func->return_type != TYPE_STRING) {
                    parser_error(parser, "Function '%s' does not return string", src_name.value);
                    return;
                }

                code_comment(parser, "Line %d: %s = %s() (string function)",
                             assign_token.line, name_token.value, src_name.value);

                expect(parser, TOKEN_LPAREN, "Expected '('");

                // Parse function arguments
                int arg_count = 0;
                while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                    if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
                        Token str_token = consume(parser->tokens);
                        int str_id = add_string_literal(parser, str_token.value);
                        code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        parse_expression(parser);
                    }
                    arg_count++;

                    if (check(parser->tokens, TOKEN_COMMA)) {
                        consume(parser->tokens);
                    }
                }

                expect(parser, TOKEN_RPAREN, "Expected ')'");

                if (arg_count != func->param_count) {
                    parser_error(parser, "Function '%s' expects %d arguments, got %d",
                                 src_name.value, func->param_count, arg_count);
                    return;
                }

                // Load arguments into registers (Windows x64 calling convention)
                const char *arg_regs_64[] = {"%rcx", "%rdx", "%r8", "%r9"};
                const char *arg_regs_32[] = {"%ecx", "%edx", "%r8d", "%r9d"};

                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    DataType param_type = func->param_types[i];

                    if (param_type == TYPE_STRING) {
                        // String parameter: use 64-bit register
                        code_printf(parser, "    popq %s\n", arg_regs_64[i]);
                    } else if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                        // Float parameter: use XMM register
                        code_printf(parser, "    popq %%rax\n");
                        if (i == 0) code_printf(parser, "    movq %%rax, %%xmm0\n");
                        else if (i == 1) code_printf(parser, "    movq %%rax, %%xmm1\n");
                        else if (i == 2) code_printf(parser, "    movq %%rax, %%xmm2\n");
                        else if (i == 3) code_printf(parser, "    movq %%rax, %%xmm3\n");
                    } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                        // Byte parameter: use 8-bit register
                        code_printf(parser, "    popq %%rax\n");
                        if (i == 0) code_printf(parser, "    movb %%al, %%cl\n");
                        else if (i == 1) code_printf(parser, "    movb %%al, %%dl\n");
                        else if (i == 2) code_printf(parser, "    movb %%al, %%r8b\n");
                        else if (i == 3) code_printf(parser, "    movb %%al, %%r9b\n");
                    } else {
                        // Integer parameter: use 32-bit register
                        code_printf(parser, "    popq %%rax\n");
                        if (i == 0) code_printf(parser, "    movl %%eax, %%ecx\n");
                        else if (i == 1) code_printf(parser, "    movl %%eax, %%edx\n");
                        else if (i == 2) code_printf(parser, "    movl %%eax, %%r8d\n");
                        else if (i == 3) code_printf(parser, "    movl %%eax, %%r9d\n");
                    }
                }

                // Call function
                code_printf(parser, "    subq $40, %%rsp\n");
                code_printf(parser, "    call %s\n", src_name.value);
                code_printf(parser, "    addq $40, %%rsp\n");

                // Store string pointer return value (RAX contains pointer)
                code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);

            } else {
                // String variable assignment: name = otherName;
                consume(parser->tokens);  // identifier

                Variable *src_var = find_variable(parser, src_name.value);
                if (!src_var || src_var->type != TYPE_STRING) {
                    parser_error(parser, "Can only assign string to string");
                    return;
                }

                code_comment(parser, "Line %d: %s = %s (pointer copy)",
                             assign_token.line, name_token.value, src_name.value);

                code_printf(parser, "    movq %d(%%rbp), %%rax\n", src_var->offset);
                code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
            }
        } else {
            parser_error(parser, "Invalid string assignment");
            return;
        }

        expect(parser, TOKEN_SEMICOLON, "Expected ';'");
        return;
    }

    // ========== INCREMENT/DECREMENT ==========
    if (op == TOKEN_PLUS_PLUS || op == TOKEN_MINUS_MINUS) {
        code_comment(parser, "Line %d: %s%s", assign_token.line, name_token.value,
                     op == TOKEN_PLUS_PLUS ? "++" : "--");
        consume(parser->tokens);
        expect(parser, TOKEN_SEMICOLON, "Expected ';'");

        if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
            parser_error(parser, "++ and -- not allowed for floating point types");
            return;
        }

        if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
            code_printf(parser, "    movb %d(%%rbp), %%al\n", var->offset);
            if (op == TOKEN_PLUS_PLUS) {
                code_printf(parser, "    addb $1, %%al\n");
            } else {
                code_printf(parser, "    subb $1, %%al\n");
            }
            code_printf(parser, "    movb %%al, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);
            if (op == TOKEN_PLUS_PLUS) {
                code_printf(parser, "    addl $1, %%eax\n");
            } else {
                code_printf(parser, "    subl $1, %%eax\n");
            }
            code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
        }
        return;
    }

    // ========== NORMAL ASSIGNMENT ==========
    const char *op_str = "";
    if (op == TOKEN_EQUAL) op_str = "=";
    else if (op == TOKEN_PLUS_EQUAL) op_str = "+=";
    else if (op == TOKEN_MINUS_EQUAL) op_str = "-=";
    else if (op == TOKEN_STAR_EQUAL) op_str = "*=";
    else if (op == TOKEN_SLASH_EQUAL) op_str = "/=";

    code_comment(parser, "Line %d: %s %s ...", assign_token.line, name_token.value, op_str);

    consume(parser->tokens);
    parse_expression(parser);
    expect(parser, TOKEN_SEMICOLON, "Expected ';'");

    // ========== FLOAT ASSIGNMENT ==========
    if (var->type == TYPE_FLOAT) {
        if (op == TOKEN_EQUAL) {
            code_printf(parser, "    movss (%%rsp), %%xmm0\n");
            code_printf(parser, "    addq $8, %%rsp\n");
            code_printf(parser, "    movss %%xmm0, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    movss (%%rsp), %%xmm1\n");
            code_printf(parser, "    addq $8, %%rsp\n");
            code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);

            if (op == TOKEN_PLUS_EQUAL) {
                code_printf(parser, "    addss %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_MINUS_EQUAL) {
                code_printf(parser, "    subss %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_STAR_EQUAL) {
                code_printf(parser, "    mulss %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_SLASH_EQUAL) {
                code_printf(parser, "    divss %%xmm1, %%xmm0\n");
            }

            code_printf(parser, "    movss %%xmm0, %d(%%rbp)\n", var->offset);
        }

    // ========== DOUBLE ASSIGNMENT ==========
    } else if (var->type == TYPE_DOUBLE) {
        if (op == TOKEN_EQUAL) {
            code_printf(parser, "    movsd (%%rsp), %%xmm0\n");
            code_printf(parser, "    addq $8, %%rsp\n");
            code_printf(parser, "    movsd %%xmm0, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    movsd (%%rsp), %%xmm1\n");
            code_printf(parser, "    addq $8, %%rsp\n");
            code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);

            if (op == TOKEN_PLUS_EQUAL) {
                code_printf(parser, "    addsd %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_MINUS_EQUAL) {
                code_printf(parser, "    subsd %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_STAR_EQUAL) {
                code_printf(parser, "    mulsd %%xmm1, %%xmm0\n");
            } else if (op == TOKEN_SLASH_EQUAL) {
                code_printf(parser, "    divsd %%xmm1, %%xmm0\n");
            }

            code_printf(parser, "    movsd %%xmm0, %d(%%rbp)\n", var->offset);
        }

    // ========== CHAR/BYTE/BIT ASSIGNMENT ==========
    } else if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
        if (op == TOKEN_EQUAL) {
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movb %%al, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    popq %%rbx\n");
            code_printf(parser, "    movb %d(%%rbp), %%al\n", var->offset);

            if (op == TOKEN_PLUS_EQUAL) {
                code_printf(parser, "    addb %%bl, %%al\n");
            } else if (op == TOKEN_MINUS_EQUAL) {
                code_printf(parser, "    subb %%bl, %%al\n");
            } else if (op == TOKEN_STAR_EQUAL) {
                code_printf(parser, "    imulb %%bl\n");
            } else if (op == TOKEN_SLASH_EQUAL) {
                code_printf(parser, "    idivb %%bl\n");
            }

            code_printf(parser, "    movb %%al, %d(%%rbp)\n", var->offset);
        }

    // ========== INT ASSIGNMENT ==========
    } else {
        if (op == TOKEN_EQUAL) {
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    popq %%rbx\n");
            code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);

            if (op == TOKEN_PLUS_EQUAL) {
                code_printf(parser, "    addl %%ebx, %%eax\n");
            } else if (op == TOKEN_MINUS_EQUAL) {
                code_printf(parser, "    subl %%ebx, %%eax\n");
            } else if (op == TOKEN_STAR_EQUAL) {
                code_printf(parser, "    imull %%ebx, %%eax\n");
            } else if (op == TOKEN_SLASH_EQUAL) {
                code_printf(parser, "    cltd\n");
                code_printf(parser, "    idivl %%ebx\n");
            }

            code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
        }
    }
}

void parse_for_loop(Parser *parser) {
    Token for_token = peek(parser->tokens);
    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Expected '(' after 'for'");

    int loop_id = parser->loop_counter++;
    int saved_scope = parser->current_scope;
    parser->current_scope++;

    code_comment(parser, "========================================");
    code_comment(parser, "For-Loop (Line %d, ID: %d)", for_token.line, loop_id);
    code_comment(parser, "========================================");

    code_comment(parser, "For-Init");
    if (check(parser->tokens, TOKEN_KEYWORD_VAR)) {
        parse_variable_declaration(parser);
    } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = consume(parser->tokens);
        expect(parser, TOKEN_EQUAL, "Expected '=' after variable name");
        parse_expression(parser);
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after for-init");

        Variable *var = find_variable(parser, name.value);
        if (var) {
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
        }
    }

    code_comment(parser, "For-Condition");
    code_printf(parser, ".L_for_condition_%d:\n", loop_id);

    parse_expression(parser);
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after for-condition");

    code_printf(parser, "    popq %%rax\n");
    code_printf(parser, "    testl %%eax, %%eax\n");
    code_printf(parser, "    je .L_for_end_%d\n", loop_id);
    code_printf(parser, "    jmp .L_for_body_%d\n", loop_id);

    code_comment(parser, "For-Increment");
    code_printf(parser, ".L_for_increment_%d:\n", loop_id);

    if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = peek(parser->tokens);
        Token op = peek_ahead(parser->tokens, 1);

        Variable *var = find_variable(parser, name.value);

        if (!var) {
            parser_error(parser, "Variable '%s' not found", name.value);
            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                consume(parser->tokens);
            }
        } else {
            consume(parser->tokens);

            if (op.type == TOKEN_PLUS_PLUS || op.type == TOKEN_MINUS_MINUS) {
                consume(parser->tokens);

                code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);
                if (op.type == TOKEN_PLUS_PLUS) {
                    code_printf(parser, "    addl $1, %%eax\n");
                } else {
                    code_printf(parser, "    subl $1, %%eax\n");
                }
                code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
            } else if (op.type == TOKEN_PLUS_EQUAL || op.type == TOKEN_MINUS_EQUAL ||
                       op.type == TOKEN_STAR_EQUAL || op.type == TOKEN_SLASH_EQUAL) {
                consume(parser->tokens);
                parse_expression(parser);

                code_printf(parser, "    popq %%rbx\n");
                code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);

                if (op.type == TOKEN_PLUS_EQUAL) {
                    code_printf(parser, "    addl %%ebx, %%eax\n");
                } else if (op.type == TOKEN_MINUS_EQUAL) {
                    code_printf(parser, "    subl %%ebx, %%eax\n");
                } else if (op.type == TOKEN_STAR_EQUAL) {
                    code_printf(parser, "    imull %%ebx, %%eax\n");
                } else if (op.type == TOKEN_SLASH_EQUAL) {
                    code_printf(parser, "    cltd\n");
                    code_printf(parser, "    idivl %%ebx\n");
                }

                code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
            } else if (op.type == TOKEN_EQUAL) {
                consume(parser->tokens);
                parse_expression(parser);

                code_printf(parser, "    popq %%rax\n");
                code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
            } else {
                parser_error(parser, "Unexpected operator in for-increment");
            }
        }
    }

    code_printf(parser, "    jmp .L_for_condition_%d\n", loop_id);

    expect(parser, TOKEN_RPAREN, "Expected ')' after for-increment");
    expect(parser, TOKEN_LBRACE, "Expected '{' after for-header");

    code_comment(parser, "For-Body");
    code_printf(parser, ".L_for_body_%d:\n", loop_id);

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Expected '}' at end of for-loop");

    code_printf(parser, "    jmp .L_for_increment_%d\n", loop_id);
    code_comment(parser, "For-End (ID: %d)", loop_id);
    code_printf(parser, ".L_for_end_%d:\n", loop_id);

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = saved_scope;
}

void parse_if_statement(Parser *parser) {
    Token if_token = peek(parser->tokens);
    consume(parser->tokens);

    int if_id = parser->label_counter++;

    code_comment(parser, "========================================");
    code_comment(parser, "If-Statement (Line %d, ID: %d)", if_token.line, if_id);
    code_comment(parser, "========================================");

    expect(parser, TOKEN_LPAREN, "Expected '(' after 'if'");
    code_comment(parser, "If-Condition");
    parse_expression(parser);
    expect(parser, TOKEN_RPAREN, "Expected ')' after if-condition");

    code_printf(parser, "    popq %%rax\n");
    code_printf(parser, "    testl %%eax, %%eax\n");
    code_printf(parser, "    je .L_else_%d\n", if_id);

    expect(parser, TOKEN_LBRACE, "Expected '{' after if-condition");

    code_comment(parser, "If-Then-Block");
    int saved_scope = parser->current_scope;
    parser->current_scope++;
    parser->scope_depth++;

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Expected '}' at end of if-block");

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = saved_scope;
    parser->scope_depth--;

    if (check(parser->tokens, TOKEN_KEYWORD_ELSE)) {
        Token else_token = peek(parser->tokens);
        consume(parser->tokens);

        code_printf(parser, "    jmp .L_endif_%d\n", if_id);
        code_comment(parser, "If-Else-Block (Line %d)", else_token.line);
        code_printf(parser, ".L_else_%d:\n", if_id);

        expect(parser, TOKEN_LBRACE, "Expected '{' after 'else'");

        parser->current_scope++;
        parser->scope_depth++;

        while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
            parse_statement(parser);
        }

        expect(parser, TOKEN_RBRACE, "Expected '}' at end of else-block");

        cleanup_scope(parser, parser->current_scope);
        parser->current_scope = saved_scope;
        parser->scope_depth--;

        code_comment(parser, "If-End (ID: %d)", if_id);
        code_printf(parser, ".L_endif_%d:\n", if_id);
    } else {
        code_comment(parser, "If-End (No else, ID: %d)", if_id);
        code_printf(parser, ".L_else_%d:\n", if_id);
    }
}

/**
 * Parse return statement
 *
 * Critical for string returns:
 * - String literals must be loaded as 64-bit pointers
 * - Must NOT use movl %eax, %eax as it zeros upper 32 bits
 * - Pointers are returned in RAX (full 64 bits)
 */
void parse_return_statement(Parser *parser) {
    Token return_token = peek(parser->tokens);
    consume(parser->tokens);

    if (!check(parser->tokens, TOKEN_SEMICOLON)) {
        code_comment(parser, "Line %d: return <expression>", return_token.line);

        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_comment(parser, "Return string literal: \"%s\"", str_token.value);
            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
        } else {
            parse_expression(parser);
            code_printf(parser, "    popq %%rax\n");
        }
    } else {
        code_comment(parser, "Line %d: return (void)", return_token.line);
    }

    expect(parser, TOKEN_SEMICOLON, "Expected ';' after return");

    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n");
}

// ============================================================================
// PRINT STATEMENT
// ============================================================================

/**
 * Parse print statement with support for:
 * - String literals: print("Hello");
 * - Variables: print(x);
 * - Expressions: print((5 + 3));
 * - Function calls: print(getNumber());
 * - String concatenation: print("Result: " + result);
 *
 * Uses printf with appropriate format strings:
 * - %d for integers
 * - %f for floats/doubles
 * - %c for characters
 * - %s for strings
 */
void parse_print_statement(Parser *parser) {
    Token print_token = peek(parser->tokens);
    code_comment(parser, "Line %d: print(...)", print_token.line);

    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Expected '(' after 'print'");

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_printf(parser, "    leaq .LC%d(%%rip), %%rcx\n", str_id);
            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $40, %%rsp\n");
        } else if (check(parser->tokens, TOKEN_LPAREN)) {
            consume(parser->tokens);
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')' after expression");

            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $40, %%rsp\n");
        } else if (check(parser->tokens, TOKEN_NUMBER)) {
            Token num = consume(parser->tokens);
            code_printf(parser, "    movl $%s, %%edx\n", num.value);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $40, %%rsp\n");
        } else if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
            Token num = consume(parser->tokens);
            int float_id = add_float_literal(parser, num.value);

            code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %%rdx\n");
            code_printf(parser, "    leaq .LC_float_format(%%rip), %%rcx\n");
            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $40, %%rsp\n");
        } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
            Token ch = consume(parser->tokens);
            int char_value = (unsigned char) ch.value[0];
            code_printf(parser, "    movb $%d, %%dl\n", char_value);
            code_printf(parser, "    leaq .LC_char_format(%%rip), %%rcx\n");
            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $40, %%rsp\n");
        } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token name = peek(parser->tokens);
            Token lookahead = peek_ahead(parser->tokens, 1);

            if (lookahead.type == TOKEN_LPAREN) {
                consume(parser->tokens);

                Function *func = find_function(parser, name.value);
                if (!func) {
                    parser_error(parser, "Function '%s' not found", name.value);
                    return;
                }

                expect(parser, TOKEN_LPAREN, "Expected '('");

                int arg_count = 0;
                while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                    parse_expression(parser);
                    arg_count++;

                    if (check(parser->tokens, TOKEN_COMMA)) {
                        consume(parser->tokens);
                    }
                }

                expect(parser, TOKEN_RPAREN, "Expected ')'");

                if (arg_count != func->param_count) {
                    parser_error(parser, "Function '%s' expects %d arguments",
                                 name.value, func->param_count);
                    return;
                }

                const char *arg_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};
                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    code_printf(parser, "    popq %s\n", arg_regs[i]);
                }

                code_printf(parser, "    subq $40, %%rsp\n");
                code_printf(parser, "    call %s\n", name.value);
                code_printf(parser, "    addq $40, %%rsp\n");

                code_printf(parser, "    movl %%eax, %%edx\n");
                code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
                code_printf(parser, "    subq $40, %%rsp\n");
                code_printf(parser, "    call printf\n");
                code_printf(parser, "    addq $40, %%rsp\n");
            } else {
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    parser_error(parser, "Variable '%s' not found", name.value);
                    return;
                }

                if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                    code_printf(parser, "    movq %%xmm0, %%rdx\n");
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %%rdx\n");
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl %d(%%rbp), %%edx\n", var->offset);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl %d(%%rbp), %%edx\n", var->offset);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %%rdx\n", var->offset);
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                } else {
                    code_printf(parser, "    movl %d(%%rbp), %%edx\n", var->offset);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
                    code_printf(parser, "    subq $40, %%rsp\n");
                    code_printf(parser, "    call printf\n");
                    code_printf(parser, "    addq $40, %%rsp\n");
                }
            }
        } else {
            parser_error(parser, "Unexpected token in print statement");
        }

        if (check(parser->tokens, TOKEN_PLUS)) {
            consume(parser->tokens);
        } else {
            break;
        }
    }

    code_printf(parser, "    movl $10, %%ecx\n");
    code_printf(parser, "    subq $40, %%rsp\n");
    code_printf(parser, "    call putchar\n");
    code_printf(parser, "    addq $40, %%rsp\n");

    expect(parser, TOKEN_RPAREN, "Expected ')' after print arguments");
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after print");
}

/**
 * Parse function call as statement (not in expression)
 *
 * Handles argument passing via Windows x64 calling convention:
 * - First 4 args in registers: RCX, RDX, R8, R9
 * - Floats in XMM0-XMM3
 * - String literals passed as pointers
 */
void parse_function_call_statement(Parser *parser) {
    Token call_token = peek(parser->tokens);
    Token name = consume(parser->tokens);

    Function *func = find_function(parser, name.value);
    if (!func) {
        parser_error(parser, "Function '%s' not found", name.value);
        return;
    }

    code_comment(parser, "Line %d: %s(...)", call_token.line, name.value);

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

    int arg_count = 0;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_comment(parser, "String argument: \"%s\"", str_token.value);
            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    pushq %%rax\n");
        } else {
            parse_expression(parser);
        }

        arg_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after function arguments");
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after function call");

    if (arg_count != func->param_count) {
        parser_error(parser, "Function '%s' expects %d arguments, got %d",
                     name.value, func->param_count, arg_count);
        return;
    }

    const char *arg_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};

    for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
        code_printf(parser, "    popq %s\n", arg_regs[i]);
    }

    code_printf(parser, "    subq $40, %%rsp\n");
    code_printf(parser, "    call %s\n", name.value);
    code_printf(parser, "    addq $40, %%rsp\n");

    if (func->return_type != TYPE_VOID) {
        code_printf(parser, "    pushq %%rax\n");
    }
}

// ============================================================================
// EXPRESSION PARSING (Recursive Descent)
// ============================================================================

void parse_expression(Parser *parser) {
    parse_logical_or(parser);
}

void parse_logical_or(Parser *parser) {
    parse_logical_and(parser);

    while (match(parser->tokens, TOKEN_PIPE_PIPE)) {
        int label_id = parser->label_counter++;

        code_comment(parser, "Logical OR (||)");
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    jne .L_or_true_%d\n", label_id);

        parse_logical_and(parser);
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    jne .L_or_true_%d\n", label_id);

        code_printf(parser, "    movl $0, %%eax\n");
        code_printf(parser, "    jmp .L_or_end_%d\n", label_id);

        code_printf(parser, ".L_or_true_%d:\n", label_id);
        code_printf(parser, "    movl $1, %%eax\n");

        code_printf(parser, ".L_or_end_%d:\n", label_id);
        code_printf(parser, "    pushq %%rax\n");
    }
}

void parse_logical_and(Parser *parser) {
    parse_comparison(parser);

    while (match(parser->tokens, TOKEN_AMP_AMP)) {
        int label_id = parser->label_counter++;

        code_comment(parser, "Logical AND (&&)");
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    je .L_and_false_%d\n", label_id);

        parse_comparison(parser);
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    je .L_and_false_%d\n", label_id);

        code_printf(parser, "    movl $1, %%eax\n");
        code_printf(parser, "    jmp .L_and_end_%d\n", label_id);

        code_printf(parser, ".L_and_false_%d:\n", label_id);
        code_printf(parser, "    movl $0, %%eax\n");

        code_printf(parser, ".L_and_end_%d:\n", label_id);
        code_printf(parser, "    pushq %%rax\n");
    }
}

/**
 * Parse comparison operators: ==, !=, <, <=, >, >=
 *
 * String comparison uses 64-bit pointer comparison (cmpq)
 * This works for string literals as they share the same address
 * when identical
 */
void parse_comparison(Parser *parser) {
    parse_term(parser);

    while (1) {
        TokenType op = peek(parser->tokens).type;

        if (op != TOKEN_EQUAL_EQUAL && op != TOKEN_BANG_EQUAL &&
            op != TOKEN_LESS && op != TOKEN_LESS_EQUAL &&
            op != TOKEN_GREATER && op != TOKEN_GREATER_EQUAL) {
            break;
        }

        consume(parser->tokens);
        parse_term(parser);

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) {
            code_printf(parser, "    cmpq %%rbx, %%rax\n");

            if (op == TOKEN_EQUAL_EQUAL) {
                code_printf(parser, "    sete %%al\n");
            } else {
                code_printf(parser, "    setne %%al\n");
            }
        } else {
            code_printf(parser, "    cmpl %%ebx, %%eax\n");

            const char *set_instruction = "sete";
            if (op == TOKEN_LESS) set_instruction = "setl";
            else if (op == TOKEN_LESS_EQUAL) set_instruction = "setle";
            else if (op == TOKEN_GREATER) set_instruction = "setg";
            else if (op == TOKEN_GREATER_EQUAL) set_instruction = "setge";

            code_printf(parser, "    %s %%al\n", set_instruction);
        }

        code_printf(parser, "    movzbl %%al, %%eax\n");
        code_printf(parser, "    pushq %%rax\n");
    }
}

void parse_term(Parser *parser) {
    parse_factor(parser);

    while (check(parser->tokens, TOKEN_PLUS) || check(parser->tokens, TOKEN_MINUS)) {
        TokenType op = peek(parser->tokens).type;
        consume(parser->tokens);

        parse_factor(parser);

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op == TOKEN_PLUS) {
            code_printf(parser, "    addl %%ebx, %%eax\n");
        } else {
            code_printf(parser, "    subl %%ebx, %%eax\n");
        }

        code_printf(parser, "    pushq %%rax\n");
    }
}

void parse_factor(Parser *parser) {
    parse_unary(parser);

    while (check(parser->tokens, TOKEN_STAR) || check(parser->tokens, TOKEN_SLASH) || check(
               parser->tokens, TOKEN_PERCENT)) {
        TokenType op = peek(parser->tokens).type;
        consume(parser->tokens);

        parse_unary(parser);

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op == TOKEN_STAR) {
            code_printf(parser, "    imull %%ebx, %%eax\n");
        } else if (op == TOKEN_SLASH) {
            code_printf(parser, "    cltd\n");
            code_printf(parser, "    idivl %%ebx\n");
        } else if (op == TOKEN_PERCENT) {
            code_printf(parser, "    cltd\n");
            code_printf(parser, "    idivl %%ebx\n");
            code_printf(parser, "    movl %%edx, %%eax\n");
        }

        code_printf(parser, "    pushq %%rax\n");
    }
}

void parse_unary(Parser *parser) {
    if (match(parser->tokens, TOKEN_BANG)) {
        code_comment(parser, "Logical NOT (!)");
        parse_unary(parser);
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    sete %%al\n");
        code_printf(parser, "    movzbl %%al, %%eax\n");
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (match(parser->tokens, TOKEN_MINUS)) {
        code_comment(parser, "Unary minus (-)");
        parse_unary(parser);
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    negl %%eax\n");
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    parse_primary(parser);
}

/**
 * Parse primary expressions (literals, variables, function calls)
 *
 * This is where the actual values enter the evaluation stack:
 * - Literals are loaded and pushed
 * - Variables are read from stack and pushed
 * - Function calls are executed and result pushed
 * - Parenthesized expressions are recursively parsed
 */
void parse_primary(Parser *parser) {
    if (check(parser->tokens, TOKEN_NUMBER)) {
        Token num = consume(parser->tokens);
        code_printf(parser, "    movl $%s, %%eax\n", num.value);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
        Token num = consume(parser->tokens);
        int float_id = add_float_literal(parser, num.value);

        code_comment(parser, "Float literal: %s", num.value);
        code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
        code_printf(parser, "    movq %%xmm0, %%rax\n");
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
        Token ch = consume(parser->tokens);

        int char_value = 0;
        if (ch.value[0] == '\\') {
            switch (ch.value[1]) {
                case 'n': char_value = '\n';
                    break;
                case 't': char_value = '\t';
                    break;
                case 'r': char_value = '\r';
                    break;
                case '0': char_value = '\0';
                    break;
                case '\\': char_value = '\\';
                    break;
                case '\'': char_value = '\'';
                    break;
                default: char_value = ch.value[1];
                    break;
            }
        } else {
            char_value = (unsigned char) ch.value[0];
        }

        code_comment(parser, "Character literal: '%s' (ASCII %d)", ch.value, char_value);
        code_printf(parser, "    movl $%d, %%eax\n", char_value);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
        Token str = consume(parser->tokens);
        int str_id = add_string_literal(parser, str.value);

        code_comment(parser, "String literal: \"%s\"", str.value);
        code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = peek(parser->tokens);
        Token lookahead = peek_ahead(parser->tokens, 1);

        if (lookahead.type == TOKEN_LPAREN) {
            consume(parser->tokens);

            Function *func = find_function(parser, name.value);
            if (!func) {
                parser_error(parser, "Function '%s' not found", name.value);
                return;
            }

            expect(parser, TOKEN_LPAREN, "Expected '('");

            int arg_count = 0;
            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
                    Token str_token = consume(parser->tokens);
                    int str_id = add_string_literal(parser, str_token.value);

                    code_comment(parser, "String arg: \"%s\"", str_token.value);
                    code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
                    code_printf(parser, "    pushq %%rax\n");
                } else {
                    parse_expression(parser);
                }

                arg_count++;

                if (check(parser->tokens, TOKEN_COMMA)) {
                    consume(parser->tokens);
                }
            }

            expect(parser, TOKEN_RPAREN, "Expected ')'");

            if (arg_count != func->param_count) {
                parser_error(parser, "Function '%s' expects %d arguments",
                             name.value, func->param_count);
                return;
            }

            /**
             * Load arguments into registers according to type:
             * - INT: Use 32-bit register names (ECX, EDX, R8D, R9D)
             * - STRING: Use 64-bit registers (RCX, RDX, R8, R9)
             * - FLOAT: Use XMM registers (XMM0-XMM3)
             *
             * Critical: Register size must match parameter type!
             */
            const char *arg_regs_int[] = {"%rcx", "%rdx", "%r8", "%r9"};
            const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

            for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                DataType param_type = func->param_types[i];

                if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[i]);
                } else if (param_type == TYPE_STRING) {
                    code_printf(parser, "    popq %s\n", arg_regs_int[i]);
                } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                    code_printf(parser, "    popq %%rax\n");
                    if (i == 0) {
                        code_printf(parser, "    movb %%al, %%cl\n");
                    } else if (i == 1) {
                        code_printf(parser, "    movb %%al, %%dl\n");
                    } else if (i == 2) {
                        code_printf(parser, "    movb %%al, %%r8b\n");
                    } else if (i == 3) {
                        code_printf(parser, "    movb %%al, %%r9b\n");
                    }
                } else {
                    code_printf(parser, "    popq %%rax\n");
                    if (i == 0) {
                        code_printf(parser, "    movl %%eax, %%ecx\n");
                    } else if (i == 1) {
                        code_printf(parser, "    movl %%eax, %%edx\n");
                    } else if (i == 2) {
                        code_printf(parser, "    movl %%eax, %%r8d\n");
                    } else if (i == 3) {
                        code_printf(parser, "    movl %%eax, %%r9d\n");
                    }
                }
            }

            code_printf(parser, "    subq $40, %%rsp\n");
            code_printf(parser, "    call %s\n", name.value);
            code_printf(parser, "    addq $40, %%rsp\n");

            if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %%xmm0, %%rax\n");
                code_printf(parser, "    pushq %%rax\n");
            } else {
                code_printf(parser, "    pushq %%rax\n");
            }
        } else {
            consume(parser->tokens);

            Variable *var = find_variable(parser, name.value);
            if (!var) {
                parser_error(parser, "Variable '%s' not found", name.value);
                return;
            }

            if (var->type == TYPE_FLOAT) {
                code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);
                code_printf(parser, "    movq %%xmm0, %%rax\n");
                code_printf(parser, "    pushq %%rax\n");
            } else if (var->type == TYPE_DOUBLE) {
                code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                code_printf(parser, "    movq %%xmm0, %%rax\n");
                code_printf(parser, "    pushq %%rax\n");
            } else if (var->type == TYPE_CHAR) {
                code_printf(parser, "    movsbl %d(%%rbp), %%eax\n", var->offset);
                code_printf(parser, "    pushq %%rax\n");
            } else if (var->type == TYPE_BYTE) {
                code_printf(parser, "    movzbl %d(%%rbp), %%eax\n", var->offset);
                code_printf(parser, "    pushq %%rax\n");
            } else if (var->type == TYPE_BIT) {
                code_printf(parser, "    movzbl %d(%%rbp), %%eax\n", var->offset);
                code_printf(parser, "    andl $1, %%eax\n");
                code_printf(parser, "    pushq %%rax\n");
            } else if (var->type == TYPE_STRING) {
                code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
                code_printf(parser, "    pushq %%rax\n");
            } else {
                code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);
                code_printf(parser, "    pushq %%rax\n");
            }
        }
        return;
    }

    if (match(parser->tokens, TOKEN_LPAREN)) {
        parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return;
    }

    parser_error(parser, "Unexpected token in expression");
}
