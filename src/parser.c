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
    fprintf(stderr, "Fehler (Zeile %d, Spalte %d): ", current.line, current.column);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fprintf(stderr, "  Bei Token: %s '%s'\n",
            token_type_to_string(current.type), current.value);
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
        if (parser->vars[i].scope == -1) continue; // Skip deleted variables
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
        parser_error(parser, "Zu viele String-Literale (max %d)", MAX_STRING_LITERALS);
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
        parser_error(parser, "Zu viele Float-Literale (max %d)", MAX_FLOAT_LITERALS);
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
            // Mark as deleted instead of actually removing
            parser->vars[i].scope = -1;
            removed++;
        } else {
            break;
        }
    }

    if (parser->debug_mode && removed > 0) {
        fprintf(stderr, "      [SCOPE] Bereinige %d Variable(n) aus Scope %d\n", removed, scope);
        code_comment(parser, "Cleanup: %d Variable(n) aus Scope %d entfernt", removed, scope);
    }
}

// ============================================================================
// PROGRAM PARSING
// ============================================================================

int parse_program(Parser *parser) {
    printf("\n");
    printf("================================================================\n");
    printf("           PHASE 2: SYNTAKTISCHE ANALYSE (PARSER)\n");
    printf("================================================================\n");
    printf("\n");

    if (parser->debug_mode) {
        fprintf(stderr, "[PARSER] Starte Parsing...\n");
    }

    while (!is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            parse_function(parser);
        } else {
            parser_error(parser, "Nur Funktionen sind auf Top-Level erlaubt");
            consume(parser->tokens);
        }
    }

    if (parser->has_error) {
        return 0;
    }

    Function *main_func = find_function(parser, "main");
    if (!main_func) {
        fprintf(stderr, "Fehler: Keine main() Funktion gefunden\n");
        return 0;
    }

    if (parser->debug_mode) {
        fprintf(stderr, "[PARSER] Parsing erfolgreich abgeschlossen\n");
        fprintf(stderr, "[PARSER] Funktionen: %d, Variablen: %d, Strings: %d, Floats: %d\n",
                parser->function_count, parser->var_count,
                parser->string_literal_count, parser->float_literal_count);
    }

    printf("✓ Parsing erfolgreich\n");
    printf("   Funktionen: %d\n", parser->function_count);
    printf("   String-Literale: %d\n", parser->string_literal_count);
    printf("   Float-Literale: %d\n", parser->float_literal_count);

    return 1;
}

// ============================================================================
// FUNCTION PARSING
// ============================================================================

void parse_function(Parser *parser) {
    if (parser->debug_mode) {
        fprintf(stderr, "  [PARSE] Function\n");
    }

    Token func_token = peek(parser->tokens);
    consume(parser->tokens);

    Token name_token = consume(parser->tokens);
    char func_name[MAX_TOKEN];
    strcpy(func_name, name_token.value);

    if (parser->debug_mode) {
        fprintf(stderr, "    [PARSE] Function name: %s\n", func_name);
    }

    if (parser->function_count >= MAX_FUNCTIONS) {
        parser_error(parser, "Zu viele Funktionen (max %d)", MAX_FUNCTIONS);
        return;
    }

    Function *func = &parser->functions[parser->function_count];
    strcpy(func->name, func_name);
    func->param_count = 0;
    parser->function_count++;

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach Funktionsname");

    int saved_var_count = parser->var_count;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token param_token = consume(parser->tokens);

        if (func->param_count >= 10) {
            parser_error(parser, "Zu viele Parameter (max 10)");
            return;
        }

        strcpy(func->params[func->param_count], param_token.value);

        expect(parser, TOKEN_COLON, "Erwarte ':' nach Parameter");

        Token type_token = consume(parser->tokens);
        DataType param_type = token_to_datatype(type_token.type);

        if (param_type == TYPE_UNKNOWN) {
            parser_error(parser, "Unbekannter Parameter-Typ");
            return;
        }

        func->param_types[func->param_count] = param_type;

        if (parser->var_count >= MAX_VARS) {
            parser_error(parser, "Zu viele Variablen");
            return;
        }

        Variable *var = &parser->vars[parser->var_count];
        strcpy(var->name, param_token.value);
        var->type = param_type;
        var->size = datatype_size(param_type);
        var->offset = -4 * (func->param_count + 1);
        var->scope = 1;
        parser->var_count++;

        func->param_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Parametern");
    expect(parser, TOKEN_ARROW, "Erwarte '->' vor Rückgabetyp");

    Token return_type_token = consume(parser->tokens);
    DataType return_type = token_to_datatype(return_type_token.type);

    if (return_type == TYPE_UNKNOWN) {
        parser_error(parser, "Unbekannter Rückgabetyp");
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
    code_printf(parser, "    subq $2048, %%rsp\n");

    if (func->param_count > 0) {
        code_comment(parser, "Save parameters to stack");
    }

    const char *param_regs_int[] = {"%ecx", "%edx", "%r8d", "%r9d"};
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
            code_printf(parser, "    movb %%cl, %d(%%rbp)\n", var->offset);
        } else {
            code_printf(parser, "    movl %s, %d(%%rbp)\n", param_regs_int[i], var->offset);
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
    expect(parser, TOKEN_LBRACE, "Erwarte '{' am Anfang des Funktionskörpers");

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Erwarte '}' am Ende des Funktionskörpers");
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
            parser_error(parser, "Unerwartetes Token nach Identifier");
            consume(parser->tokens);
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
        parser_error(parser, "Unerwartetes Statement");
        consume(parser->tokens);
    }
}

void parse_variable_declaration(Parser *parser) {
    Token var_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Variable declaration\n");
    }

    consume(parser->tokens);
    Token name_token = consume(parser->tokens);

    DataType var_type = TYPE_INT;
    int has_explicit_type = 0;
    int has_initializer = 0;

    if (check(parser->tokens, TOKEN_COLON)) {
        consume(parser->tokens);

        Token type_token = consume(parser->tokens);
        var_type = token_to_datatype(type_token.type);

        if (var_type == TYPE_UNKNOWN) {
            parser_error(parser, "Unbekannter Variablen-Typ '%s'", type_token.value);
            return;
        }

        has_explicit_type = 1;

        if (parser->debug_mode) {
            fprintf(stderr, "      [INFO] Expliziter Typ: %s\n", datatype_to_string(var_type));
        }
    }

    if (check(parser->tokens, TOKEN_EQUAL)) {
        consume(parser->tokens);
        has_initializer = 1;

        code_comment(parser, "Line %d: var %s:%s = ...",
                     var_token.line, name_token.value, datatype_to_string(var_type));

        parse_expression(parser);
    } else {
        if (!has_explicit_type) {
            parser_error(parser, "Variable '%s' ohne Typ und ohne Initialisierung", name_token.value);
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

    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' am Ende der Variablendeklaration");

    if (parser->var_count >= MAX_VARS) {
        parser_error(parser, "Zu viele Variablen (max %d)", MAX_VARS);
        return;
    }

    Variable *var = &parser->vars[parser->var_count];
    strcpy(var->name, name_token.value);
    var->type = var_type;
    var->size = datatype_size(var_type);

    int active_var_count = 0;
    for (int i = 0; i < parser->var_count; i++) {
        if (parser->vars[i].scope >= 1) {
            active_var_count++;
        }
    }

    var->offset = -4 * (active_var_count + 1);
    var->scope = parser->current_scope;
    parser->var_count++;

    if (parser->debug_mode) {
        fprintf(stderr, "      [DEBUG] Variable '%s' @ offset %d (Typ: %s, Size: %d, Scope: %d, Active: %d)\n",
                var->name, var->offset, datatype_to_string(var->type), var->size,
                var->scope, active_var_count + 1);
    }

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
    } else {
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
    }
}

void parse_assignment(Parser *parser) {
    Token assign_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Assignment\n");
    }

    Token name_token = consume(parser->tokens);
    Variable *var = find_variable(parser, name_token.value);

    if (!var) {
        parser_error(parser, "Variable '%s' nicht gefunden", name_token.value);
        return;
    }

    TokenType op = peek(parser->tokens).type;

    if (op == TOKEN_PLUS_PLUS || op == TOKEN_MINUS_MINUS) {
        code_comment(parser, "Line %d: %s%s", assign_token.line, name_token.value,
                     op == TOKEN_PLUS_PLUS ? "++" : "--");
        consume(parser->tokens);
        expect(parser, TOKEN_SEMICOLON, "Erwarte ';'");

        if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
            parser_error(parser, "++ und -- nicht für Floating Point Typen erlaubt");
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

    const char *op_str = "";
    if (op == TOKEN_EQUAL) op_str = "=";
    else if (op == TOKEN_PLUS_EQUAL) op_str = "+=";
    else if (op == TOKEN_MINUS_EQUAL) op_str = "-=";
    else if (op == TOKEN_STAR_EQUAL) op_str = "*=";
    else if (op == TOKEN_SLASH_EQUAL) op_str = "/=";

    code_comment(parser, "Line %d: %s %s ...", assign_token.line, name_token.value, op_str);

    consume(parser->tokens);
    parse_expression(parser);
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';'");

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

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] For loop\n");
    }

    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach 'for'");

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
        expect(parser, TOKEN_EQUAL, "Erwarte '=' nach Variablenname");
        parse_expression(parser);
        expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach for-Init");

        Variable *var = find_variable(parser, name.value);
        if (var) {
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
        }
    }

    code_comment(parser, "For-Condition");
    code_printf(parser, ".L_for_condition_%d:\n", loop_id);

    parse_expression(parser);
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach for-Bedingung");

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
            parser_error(parser, "Variable '%s' nicht gefunden", name.value);
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
                parser_error(parser, "Unerwarteter Operator im for-Inkrement");
            }
        }
    }

    code_printf(parser, "    jmp .L_for_condition_%d\n", loop_id);

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach for-Inkrement");
    expect(parser, TOKEN_LBRACE, "Erwarte '{' nach for-Header");

    code_comment(parser, "For-Body");
    code_printf(parser, ".L_for_body_%d:\n", loop_id);

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Erwarte '}' am Ende der for-Schleife");

    code_printf(parser, "    jmp .L_for_increment_%d\n", loop_id);
    code_comment(parser, "For-End (ID: %d)", loop_id);
    code_printf(parser, ".L_for_end_%d:\n", loop_id);

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = saved_scope;
}

// ============================================================================
// IF/ELSE STATEMENT
// ============================================================================

void parse_if_statement(Parser *parser) {
    Token if_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] If statement\n");
    }

    consume(parser->tokens);

    int if_id = parser->label_counter++;

    code_comment(parser, "========================================");
    code_comment(parser, "If-Statement (Line %d, ID: %d)", if_token.line, if_id);
    code_comment(parser, "========================================");

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach 'if'");
    code_comment(parser, "If-Condition");
    parse_expression(parser);
    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach if-Bedingung");

    code_printf(parser, "    popq %%rax\n");
    code_printf(parser, "    testl %%eax, %%eax\n");
    code_printf(parser, "    je .L_else_%d\n", if_id);

    expect(parser, TOKEN_LBRACE, "Erwarte '{' nach if-Bedingung");

    code_comment(parser, "If-Then-Block");
    int saved_scope = parser->current_scope;
    parser->current_scope++;
    parser->scope_depth++;

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Erwarte '}' am Ende des if-Blocks");

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = saved_scope;
    parser->scope_depth--;

    if (check(parser->tokens, TOKEN_KEYWORD_ELSE)) {
        Token else_token = peek(parser->tokens);
        consume(parser->tokens);

        code_printf(parser, "    jmp .L_endif_%d\n", if_id);
        code_comment(parser, "If-Else-Block (Line %d)", else_token.line);
        code_printf(parser, ".L_else_%d:\n", if_id);

        expect(parser, TOKEN_LBRACE, "Erwarte '{' nach 'else'");

        parser->current_scope++;
        parser->scope_depth++;

        while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
            parse_statement(parser);
        }

        expect(parser, TOKEN_RBRACE, "Erwarte '}' am Ende des else-Blocks");

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

void parse_return_statement(Parser *parser) {
    Token return_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Return statement\n");
    }

    consume(parser->tokens);

    if (!check(parser->tokens, TOKEN_SEMICOLON)) {
        code_comment(parser, "Line %d: return <expression>", return_token.line);
        parse_expression(parser);

        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movl %%eax, %%eax\n");
    } else {
        code_comment(parser, "Line %d: return (void)", return_token.line);
    }

    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach return");

    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n");
}

// ============================================================================
// PRINT STATEMENT
// ============================================================================

void parse_print_statement(Parser *parser) {
    Token print_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Print statement\n");
    }

    code_comment(parser, "Line %d: print(...)", print_token.line);

    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach 'print'");

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
            expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Expression");

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
                    parser_error(parser, "Funktion '%s' nicht gefunden", name.value);
                    return;
                }

                expect(parser, TOKEN_LPAREN, "Erwarte '('");

                int arg_count = 0;
                while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                    parse_expression(parser);
                    arg_count++;

                    if (check(parser->tokens, TOKEN_COMMA)) {
                        consume(parser->tokens);
                    }
                }

                expect(parser, TOKEN_RPAREN, "Erwarte ')'");

                if (arg_count != func->param_count) {
                    parser_error(parser, "Funktion '%s' erwartet %d Argumente",
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
                    parser_error(parser, "Variable '%s' nicht gefunden", name.value);
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
                    // ✅ FIX: Load byte/bit properly
                    code_printf(parser, "    movzbl %d(%%rbp), %%edx\n", var->offset);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %%rcx\n");
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
            parser_error(parser, "Unerwartetes Token in print-Statement");
            consume(parser->tokens);
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

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach print-Argumenten");
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach print");
}

void parse_function_call_statement(Parser *parser) {
    Token call_token = peek(parser->tokens);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Function call\n");
    }

    Token name = consume(parser->tokens);

    Function *func = find_function(parser, name.value);
    if (!func) {
        parser_error(parser, "Funktion '%s' nicht gefunden", name.value);
        return;
    }

    code_comment(parser, "Line %d: %s(...)", call_token.line, name.value);

    if (parser->debug_mode) {
        fprintf(stderr, "      [PARSE] Function call: %s\n", name.value);
    }

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach Funktionsname");

    int arg_count = 0;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        parse_expression(parser);
        arg_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Funktionsargumenten");
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach Funktionsaufruf");

    if (arg_count != func->param_count) {
        parser_error(parser, "Funktion '%s' erwartet %d Argumente, aber %d wurden übergeben",
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
// EXPRESSION PARSING
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
        code_printf(parser, "    cmpl %%ebx, %%eax\n");

        const char *set_instruction = "sete";
        if (op == TOKEN_EQUAL_EQUAL) set_instruction = "sete";
        else if (op == TOKEN_BANG_EQUAL) set_instruction = "setne";
        else if (op == TOKEN_LESS) set_instruction = "setl";
        else if (op == TOKEN_LESS_EQUAL) set_instruction = "setle";
        else if (op == TOKEN_GREATER) set_instruction = "setg";
        else if (op == TOKEN_GREATER_EQUAL) set_instruction = "setge";

        code_printf(parser, "    %s %%al\n", set_instruction);
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

    while (check(parser->tokens, TOKEN_STAR) || check(parser->tokens, TOKEN_SLASH)) {
        TokenType op = peek(parser->tokens).type;
        consume(parser->tokens);

        parse_unary(parser);

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op == TOKEN_STAR) {
            code_printf(parser, "    imull %%ebx, %%eax\n");
        } else {
            code_printf(parser, "    cltd\n");
            code_printf(parser, "    idivl %%ebx\n");
        }

        code_printf(parser, "    pushq %%rax\n");
    }
}

void parse_unary(Parser *parser) {
    // Logical NOT (!)
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

    // Unary minus (negation)
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

void parse_primary(Parser *parser) {
    // Number (Integer)
    if (check(parser->tokens, TOKEN_NUMBER)) {
        Token num = consume(parser->tokens);
        code_printf(parser, "    movl $%s, %%eax\n", num.value);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    // Float Literal
    if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
        Token num = consume(parser->tokens);

        int float_id = add_float_literal(parser, num.value);

        code_comment(parser, "Float literal: %s", num.value);
        code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
        code_printf(parser, "    movq %%xmm0, %%rax\n");
        code_printf(parser, "    pushq %%rax\n");

        return;
    }

    // Character Literal
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

    // Variable or Function Call
    if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = peek(parser->tokens);
        Token lookahead = peek_ahead(parser->tokens, 1);

        if (lookahead.type == TOKEN_LPAREN) {
            // Function call
            consume(parser->tokens);

            Function *func = find_function(parser, name.value);
            if (!func) {
                parser_error(parser, "Funktion '%s' nicht gefunden", name.value);
                return;
            }

            if (parser->debug_mode) {
                fprintf(stderr, "      [PARSE] Function call: %s\n", name.value);
            }

            expect(parser, TOKEN_LPAREN, "Erwarte '('");

            int arg_count = 0;
            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                parse_expression(parser);
                arg_count++;

                if (check(parser->tokens, TOKEN_COMMA)) {
                    consume(parser->tokens);
                }
            }

            expect(parser, TOKEN_RPAREN, "Erwarte ')'");

            if (arg_count != func->param_count) {
                parser_error(parser, "Funktion '%s' erwartet %d Argumente",
                             name.value, func->param_count);
                return;
            }

            const char *arg_regs_int[] = {"%rcx", "%rdx", "%r8", "%r9"};
            const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

            for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                DataType param_type = func->param_types[i];

                if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[i]);
                } else {
                    code_printf(parser, "    popq %s\n", arg_regs_int[i]);
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
            // Variable reference
            consume(parser->tokens);

            Variable *var = find_variable(parser, name.value);
            if (!var) {
                parser_error(parser, "Variable '%s' nicht gefunden", name.value);
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
            } else {
                code_printf(parser, "    movl %d(%%rbp), %%eax\n", var->offset);
                code_printf(parser, "    pushq %%rax\n");
            }
        }
        return;
    }

    // Parenthesized expression
    if (match(parser->tokens, TOKEN_LPAREN)) {
        parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Expression");
        return;
    }

    parser_error(parser, "Unerwartetes Token in Expression");
}
