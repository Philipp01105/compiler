#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ============================================================================
// PARSER CREATION & DESTRUCTION
// ============================================================================

Parser *create_parser(TokenStream *tokens) {
    Parser *parser = malloc(sizeof(Parser));
    memset(parser, 0, sizeof(Parser));

    parser->tokens = tokens;
    parser->debug_mode = 0;

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

    if (parser->current_scope > 0) {
        parser->function_code_pos += vsnprintf(
            parser->function_code_buffer + parser->function_code_pos,
            sizeof(parser->function_code_buffer) - parser->function_code_pos,
            format, args
        );
    } else {
        parser->code_pos += vsnprintf(
            parser->code_buffer + parser->code_pos,
            sizeof(parser->code_buffer) - parser->code_pos,
            format, args
        );
    }

    va_end(args);
}

int add_string_literal(Parser *parser, const char *text) {
    strcpy(parser->string_literals[parser->string_literal_count].text, text);
    parser->string_literals[parser->string_literal_count].id = parser->string_count;
    parser->string_literal_count++;
    return parser->string_count++;
}

// ============================================================================
// VARIABLE MANAGEMENT
// ============================================================================

int create_variable(Parser *parser, const char *name, DataType type) {
    if (parser->debug_mode) {
        printf("      [DEBUG] Variable erstellt: '%s' (Typ: %s, Scope: %d)\n",
               name, type == TYPE_INT ? "int" : "unknown", parser->current_scope);
    }

    // Check for duplicate
    for (int i = 0; i < parser->var_count; i++) {
        if (strcmp(parser->vars[i].name, name) == 0 &&
            parser->vars[i].scope == parser->current_scope) {
            parser_error(parser, "Variable '%s' bereits deklariert", name);
            return -1;
        }
    }

    parser->stack_offset += 4;
    strcpy(parser->vars[parser->var_count].name, name);
    parser->vars[parser->var_count].offset = parser->stack_offset;
    parser->vars[parser->var_count].scope = parser->current_scope;
    parser->vars[parser->var_count].type = type;

    parser->var_count++;
    return parser->stack_offset;
}

int get_var_offset(Parser *parser, const char *name) {
    for (int i = parser->var_count - 1; i >= 0; i--) {
        if (strcmp(parser->vars[i].name, name) == 0) {
            if (parser->vars[i].scope <= parser->current_scope) {
                return parser->vars[i].offset;
            }
        }
    }

    parser_error(parser, "Variable '%s' nicht deklariert", name);
    return -1;
}

DataType get_var_type(Parser *parser, const char *name) {
    for (int i = parser->var_count - 1; i >= 0; i--) {
        if (strcmp(parser->vars[i].name, name) == 0) {
            if (parser->vars[i].scope <= parser->current_scope) {
                return parser->vars[i].type;
            }
        }
    }
    return TYPE_UNKNOWN;
}

void enter_scope(Parser *parser) {
    parser->current_scope++;
}

void exit_scope(Parser *parser) {
    // Remove variables from this scope
    while (parser->var_count > 0 &&
           parser->vars[parser->var_count - 1].scope >= parser->current_scope) {
        parser->var_count--;
    }
    parser->current_scope--;
}

// ============================================================================
// ERROR HANDLING
// ============================================================================

void parser_error(Parser *parser, const char *format, ...) {
    va_list args;
    va_start(args, format);

    fprintf(stderr, "Fehler (Zeile %d): ", parser->current_line);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");

    va_end(args);
    parser->has_error = 1;
}

void parser_error_at_token(Parser *parser, Token token, const char *format, ...) {
    va_list args;
    va_start(args, format);

    fprintf(stderr, "Fehler (Zeile %d, Spalte %d): ", token.line, token.column);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    fprintf(stderr, "  Bei Token: %s '%s'\n",
            token_type_to_string(token.type), token.value);

    va_end(args);
    parser->has_error = 1;
}

// ============================================================================
// TOKEN UTILITIES
// ============================================================================

Token expect(Parser *parser, TokenType type, const char *message) {
    Token token = peek(parser->tokens);
    if (token.type != type) {
        parser_error_at_token(parser, token, "%s (erwartet: %s)",
                            message, token_type_to_string(type));
        return token;
    }
    return consume(parser->tokens);
}

int match_any(Parser *parser, int count, ...) {
    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        TokenType type = va_arg(args, TokenType);
        if (check(parser->tokens, type)) {
            consume(parser->tokens);
            va_end(args);
            return 1;
        }
    }

    va_end(args);
    return 0;
}

DataType token_to_datatype(Token token) {
    if (token.type == TOKEN_TYPE_INT) return TYPE_INT;
    if (token.type == TOKEN_TYPE_STRING) return TYPE_STRING;
    if (token.type == TOKEN_TYPE_VOID) return TYPE_VOID;
    return TYPE_UNKNOWN;
}

// ============================================================================
// EXPRESSION PARSING (Precedence Climbing)
// ============================================================================

DataType parse_primary(Parser *parser) {
    Token token = peek(parser->tokens);

    // Number literal
    if (token.type == TOKEN_NUMBER) {
        consume(parser->tokens);
        int value = atoi(token.value);
        code_printf(parser, "    pushq $%d\n", value);
        return TYPE_INT;
    }

    // String literal (for print)
    if (token.type == TOKEN_STRING_LITERAL) {
        consume(parser->tokens);
        return TYPE_STRING;
    }

    // Parenthesized expression
    if (token.type == TOKEN_LPAREN) {
        consume(parser->tokens);
        DataType type = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Expression");
        return type;
    }

    // Function call
    if (token.type == TOKEN_IDENTIFIER) {
        Token next = peek_ahead(parser->tokens, 1);
        if (next.type == TOKEN_LPAREN) {
            Token func_name = consume(parser->tokens);
            parse_function_call(parser, func_name);
            return TYPE_INT;
        }

        // Variable
        consume(parser->tokens);
        int offset = get_var_offset(parser, token.value);
        if (offset >= 0) {
            code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
            code_printf(parser, "    pushq %%rax\n");
        }
        return get_var_type(parser, token.value);
    }

    parser_error_at_token(parser, token, "Unerwartetes Token in Expression");
    return TYPE_UNKNOWN;
}

DataType parse_factor(Parser *parser) {
    DataType left_type = parse_primary(parser);

    while (check(parser->tokens, TOKEN_STAR) || check(parser->tokens, TOKEN_SLASH)) {
        Token op = consume(parser->tokens);
        DataType right_type = parse_primary(parser);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            parser_error(parser, "Multiplikation/Division nur mit int moeglich");
        }

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op.type == TOKEN_STAR) {
            code_printf(parser, "    imull %%ebx, %%eax\n");
        } else {
            code_printf(parser, "    cltd\n");
            code_printf(parser, "    idivl %%ebx\n");
        }

        code_printf(parser, "    pushq %%rax\n");
        left_type = TYPE_INT;
    }

    return left_type;
}

DataType parse_term(Parser *parser) {
    DataType left_type = parse_factor(parser);

    while (check(parser->tokens, TOKEN_PLUS) || check(parser->tokens, TOKEN_MINUS)) {
        Token op = consume(parser->tokens);
        DataType right_type = parse_factor(parser);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            parser_error(parser, "Addition/Subtraktion nur mit int moeglich");
        }

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        if (op.type == TOKEN_PLUS) {
            code_printf(parser, "    addl %%ebx, %%eax\n");
        } else {
            code_printf(parser, "    subl %%ebx, %%eax\n");
        }

        code_printf(parser, "    pushq %%rax\n");
        left_type = TYPE_INT;
    }

    return left_type;
}

DataType parse_comparison(Parser *parser) {
    DataType left_type = parse_term(parser);

    if (match_any(parser, 6, TOKEN_EQUAL_EQUAL, TOKEN_BANG_EQUAL,
                  TOKEN_LESS, TOKEN_LESS_EQUAL, TOKEN_GREATER, TOKEN_GREATER_EQUAL)) {

        Token op = parser->tokens->tokens[parser->tokens->current - 1];
        DataType right_type = parse_term(parser);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            parser_error(parser, "Vergleiche nur mit int moeglich");
        }

        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    cmpl %%ebx, %%eax\n");

        switch (op.type) {
            case TOKEN_EQUAL_EQUAL:
                code_printf(parser, "    sete %%al\n");
                break;
            case TOKEN_BANG_EQUAL:
                code_printf(parser, "    setne %%al\n");
                break;
            case TOKEN_LESS:
                code_printf(parser, "    setl %%al\n");
                break;
            case TOKEN_LESS_EQUAL:
                code_printf(parser, "    setle %%al\n");
                break;
            case TOKEN_GREATER:
                code_printf(parser, "    setg %%al\n");
                break;
            case TOKEN_GREATER_EQUAL:
                code_printf(parser, "    setge %%al\n");
                break;
            default:
                break;
        }

        code_printf(parser, "    movzbl %%al, %%eax\n");
        code_printf(parser, "    pushq %%rax\n");
        left_type = TYPE_INT;
    }

    return left_type;
}

DataType parse_expression(Parser *parser) {
    return parse_comparison(parser);
}

// ============================================================================
// STATEMENT PARSING
// ============================================================================

void parse_print(Parser *parser) {
    if (parser->debug_mode) {
        printf("      [PARSE] Print statement\n");
    }

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach 'print'");

    // Parse print arguments - special handling for string concatenation
    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token token = peek(parser->tokens);

        // String Literal
        if (token.type == TOKEN_STRING_LITERAL) {
            consume(parser->tokens);
            int id = add_string_literal(parser, token.value);
            code_printf(parser, "    leaq .LC%d(%%rip), %%rcx\n", id);
            code_printf(parser, "    subq $32, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $32, %%rsp\n");
        }
        // Parenthesized Expression
        else if (token.type == TOKEN_LPAREN) {
            consume(parser->tokens);
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Expression");

            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movl %%eax, %%edx\n");
            int id = add_string_literal(parser, "%d");
            code_printf(parser, "    leaq .LC%d(%%rip), %%rcx\n", id);
            code_printf(parser, "    subq $32, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $32, %%rsp\n");
        }
        // Identifier (Variable or Function Call)
        else if (token.type == TOKEN_IDENTIFIER) {
            Token next = peek_ahead(parser->tokens, 1);

            if (next.type == TOKEN_LPAREN) {
                // Function call
                Token func_name = consume(parser->tokens);
                parse_function_call(parser, func_name);
                code_printf(parser, "    popq %%rax\n");
                code_printf(parser, "    movl %%eax, %%edx\n");
            } else {
                // Variable
                consume(parser->tokens);
                int offset = get_var_offset(parser, token.value);
                if (offset >= 0) {
                    code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
                    code_printf(parser, "    movl %%eax, %%edx\n");
                }
            }

            int id = add_string_literal(parser, "%d");
            code_printf(parser, "    leaq .LC%d(%%rip), %%rcx\n", id);
            code_printf(parser, "    subq $32, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $32, %%rsp\n");
        }
        // Number Literal
        else if (token.type == TOKEN_NUMBER) {
            consume(parser->tokens);
            int value = atoi(token.value);
            code_printf(parser, "    movl $%d, %%edx\n", value);
            int id = add_string_literal(parser, "%d");
            code_printf(parser, "    leaq .LC%d(%%rip), %%rcx\n", id);
            code_printf(parser, "    subq $32, %%rsp\n");
            code_printf(parser, "    call printf\n");
            code_printf(parser, "    addq $32, %%rsp\n");
        }
        else {
            parser_error_at_token(parser, token, "Ungueltiger Ausdruck in print()");
            break;
        }

        // Check for continuation with '+'
        if (check(parser->tokens, TOKEN_PLUS)) {
            consume(parser->tokens);
        } else if (!check(parser->tokens, TOKEN_RPAREN)) {
            Token unexpected = peek(parser->tokens);
            parser_error_at_token(parser, unexpected,
                "Erwarte '+' oder ')' in print-Statement");
            break;
        }
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' am Ende von print");

    // Newline ausgeben
    code_printf(parser, "    movl $10, %%ecx\n");
    code_printf(parser, "    subq $32, %%rsp\n");
    code_printf(parser, "    call putchar\n");
    code_printf(parser, "    addq $32, %%rsp\n");
}

void parse_function_call(Parser *parser, Token func_name) {
    if (parser->debug_mode) {
        printf("      [PARSE] Function call: %s\n", func_name.value);
    }

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach Funktionsname");

    // Parse arguments and collect them
    int arg_count = 0;
    if (!check(parser->tokens, TOKEN_RPAREN)) {
        do {
            parse_expression(parser);
            arg_count++;
        } while (match(parser->tokens, TOKEN_COMMA));
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Argumenten");

    // WICHTIG: Pop in REVERSE order for Windows x64
    // Arguments are pushed left-to-right, but need to be popped right-to-left
    // So we need to pop them in reverse order into registers

    switch (arg_count) {
        case 1:
            code_printf(parser, "    popq %%rcx\n");
            break;
        case 2:
            code_printf(parser, "    popq %%rdx\n");   // Second arg (was pushed second)
            code_printf(parser, "    popq %%rcx\n");   // First arg (was pushed first)
            break;
        case 3:
            code_printf(parser, "    popq %%r8\n");
            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rcx\n");
            break;
        case 4:
            code_printf(parser, "    popq %%r9\n");
            code_printf(parser, "    popq %%r8\n");
            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rcx\n");
            break;
    }

    // Call function
    code_printf(parser, "    subq $32, %%rsp\n");
    code_printf(parser, "    call %s\n", func_name.value);
    code_printf(parser, "    addq $32, %%rsp\n");
    code_printf(parser, "    pushq %%rax\n");
}

void parse_return(Parser *parser) {
    if (parser->debug_mode) {
        printf("      [PARSE] Return statement\n");
    }

    if (!check(parser->tokens, TOKEN_SEMICOLON)) {
        parse_expression(parser);
        code_printf(parser, "    popq %%rax\n");
    }

    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n");
}

void parse_var_declaration(Parser *parser) {
    Token name = expect(parser, TOKEN_IDENTIFIER, "Erwarte Variablenname");

    if (parser->debug_mode) {
        printf("      [PARSE] Variable declaration: %s\n", name.value);
    }

    expect(parser, TOKEN_EQUAL, "Erwarte '=' bei Variablendeklaration");

    DataType type = parse_expression(parser);

    int offset = create_variable(parser, name.value, type);
    if (offset >= 0) {
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
    }
}

void parse_assignment(Parser *parser, Token var_name) {
    if (parser->debug_mode) {
        printf("      [PARSE] Assignment: %s\n", var_name.value);
    }

    Token op = consume(parser->tokens);

    int offset = get_var_offset(parser, var_name.value);
    if (offset < 0) return;

    // Compound assignment (+=, -=, etc.)
    if (op.type == TOKEN_PLUS_EQUAL || op.type == TOKEN_MINUS_EQUAL ||
        op.type == TOKEN_STAR_EQUAL || op.type == TOKEN_SLASH_EQUAL) {

        // Load current value
        code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
        code_printf(parser, "    pushq %%rax\n");

        // Parse right side
        parse_expression(parser);

        // Perform operation
        code_printf(parser, "    popq %%rbx\n");
        code_printf(parser, "    popq %%rax\n");

        switch (op.type) {
            case TOKEN_PLUS_EQUAL:
                code_printf(parser, "    addl %%ebx, %%eax\n");
                break;
            case TOKEN_MINUS_EQUAL:
                code_printf(parser, "    subl %%ebx, %%eax\n");
                break;
            case TOKEN_STAR_EQUAL:
                code_printf(parser, "    imull %%ebx, %%eax\n");
                break;
            case TOKEN_SLASH_EQUAL:
                code_printf(parser, "    cltd\n");
                code_printf(parser, "    idivl %%ebx\n");
                break;
            default:
                break;
        }
    } else {
        // Simple assignment
        parse_expression(parser);
        code_printf(parser, "    popq %%rax\n");
    }

    code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
}

void parse_for_loop(Parser *parser) {
    if (parser->debug_mode) {
        printf("      [PARSE] For loop\n");
    }

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach 'for'");

    // Enter loop scope
    enter_scope(parser);
    int saved_var_count = parser->var_count;

    int loop_id = parser->label_counter++;

    // Init
    if (check(parser->tokens, TOKEN_KEYWORD_VAR)) {
        consume(parser->tokens);
        parse_var_declaration(parser);
    } else if (!check(parser->tokens, TOKEN_SEMICOLON)) {
        Token var = peek(parser->tokens);
        if (var.type == TOKEN_IDENTIFIER) {
            consume(parser->tokens);
            Token op = peek(parser->tokens);
            if (op.type == TOKEN_EQUAL || op.type == TOKEN_PLUS_EQUAL ||
                op.type == TOKEN_MINUS_EQUAL || op.type == TOKEN_STAR_EQUAL ||
                op.type == TOKEN_SLASH_EQUAL) {
                parse_assignment(parser, var);
            }
        }
    }
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach for-init");

    // Condition
    code_printf(parser, ".L_for_start_%d:\n", loop_id);

    if (!check(parser->tokens, TOKEN_SEMICOLON)) {
        parse_expression(parser);
        code_printf(parser, "    popq %%rax\n");
        code_printf(parser, "    testl %%eax, %%eax\n");
        code_printf(parser, "    je .L_for_end_%d\n", loop_id);
    }
    expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach for-condition");

    // Save increment tokens
    typedef struct {
        TokenType type;
        char value[MAX_TOKEN];
    } SavedToken;

    SavedToken increment_tokens[50];
    int increment_count = 0;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens) && increment_count < 50) {
        Token t = consume(parser->tokens);
        increment_tokens[increment_count].type = t.type;
        strcpy(increment_tokens[increment_count].value, t.value);
        increment_count++;
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach for-header");

    // Body
    expect(parser, TOKEN_LBRACE, "Erwarte '{' nach for-header");

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Erwarte '}' nach for-body");

    // Increment
    if (increment_count > 0) {
        if (increment_count >= 2) {
            Token var_token;
            var_token.type = increment_tokens[0].type;
            strcpy(var_token.value, increment_tokens[0].value);

            Token op_token;
            op_token.type = increment_tokens[1].type;
            strcpy(op_token.value, increment_tokens[1].value);

            // Pattern: var++ or var--
            if (op_token.type == TOKEN_PLUS_PLUS || op_token.type == TOKEN_MINUS_MINUS) {
                int offset = get_var_offset(parser, var_token.value);
                if (offset >= 0) {
                    code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
                    if (op_token.type == TOKEN_PLUS_PLUS) {
                        code_printf(parser, "    addl $1, %%eax\n");
                    } else {
                        code_printf(parser, "    subl $1, %%eax\n");
                    }
                    code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
                }
            }
            // Pattern: var += expr, var -= expr, etc.
            else if (op_token.type == TOKEN_PLUS_EQUAL || op_token.type == TOKEN_MINUS_EQUAL ||
                     op_token.type == TOKEN_STAR_EQUAL || op_token.type == TOKEN_SLASH_EQUAL) {

                int offset = get_var_offset(parser, var_token.value);
                if (offset >= 0) {
                    code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
                    code_printf(parser, "    pushq %%rax\n");

                    if (increment_count > 2) {
                        if (increment_tokens[2].type == TOKEN_NUMBER) {
                            int value = atoi(increment_tokens[2].value);
                            code_printf(parser, "    pushq $%d\n", value);
                        }
                    }

                    code_printf(parser, "    popq %%rbx\n");
                    code_printf(parser, "    popq %%rax\n");

                    if (op_token.type == TOKEN_PLUS_EQUAL) {
                        code_printf(parser, "    addl %%ebx, %%eax\n");
                    } else if (op_token.type == TOKEN_MINUS_EQUAL) {
                        code_printf(parser, "    subl %%ebx, %%eax\n");
                    } else if (op_token.type == TOKEN_STAR_EQUAL) {
                        code_printf(parser, "    imull %%ebx, %%eax\n");
                    } else if (op_token.type == TOKEN_SLASH_EQUAL) {
                        code_printf(parser, "    cltd\n");
                        code_printf(parser, "    idivl %%ebx\n");
                    }

                    code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
                }
            }
            // Pattern: var = expr
            else if (op_token.type == TOKEN_EQUAL) {
                if (increment_count > 2 && increment_tokens[2].type == TOKEN_NUMBER) {
                    int offset = get_var_offset(parser, var_token.value);
                    int value = atoi(increment_tokens[2].value);
                    if (offset >= 0) {
                        code_printf(parser, "    movl $%d, %%eax\n", value);
                        code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
                    }
                }
            }
        }
    }

    code_printf(parser, "    jmp .L_for_start_%d\n", loop_id);
    code_printf(parser, ".L_for_end_%d:\n", loop_id);

    // Exit scope
    parser->var_count = saved_var_count;
    exit_scope(parser);
}

void parse_statement(Parser *parser) {
    Token token = peek(parser->tokens);
    parser->current_line = token.line;

    // Skip semicolons
    if (token.type == TOKEN_SEMICOLON) {
        consume(parser->tokens);
        return;
    }

    // Check for closing brace - function/loop end
    if (token.type == TOKEN_RBRACE) {
        return;
    }

    // Variable declaration
    if (token.type == TOKEN_KEYWORD_VAR) {
        consume(parser->tokens);
        parse_var_declaration(parser);
        expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach Variablendeklaration");
        return;
    }

    // Return statement
    if (token.type == TOKEN_KEYWORD_RETURN) {
        consume(parser->tokens);
        parse_return(parser);
        expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach return");
        return;
    }

    // Print statement
    if (token.type == TOKEN_KEYWORD_PRINT) {
        consume(parser->tokens);
        parse_print(parser);
        expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach print");
        return;
    }

    // For loop
    if (token.type == TOKEN_KEYWORD_FOR) {
        consume(parser->tokens);
        parse_for_loop(parser);
        return;
    }

    // Assignment or function call
    if (token.type == TOKEN_IDENTIFIER) {
        Token name = consume(parser->tokens);
        Token next = peek(parser->tokens);

        // Function call
        if (next.type == TOKEN_LPAREN) {
            parse_function_call(parser, name);
            code_printf(parser, "    popq %%rax\n");
            expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach Funktionsaufruf");
            return;
        }

        // Assignment
        if (next.type == TOKEN_EQUAL || next.type == TOKEN_PLUS_EQUAL ||
            next.type == TOKEN_MINUS_EQUAL || next.type == TOKEN_STAR_EQUAL ||
            next.type == TOKEN_SLASH_EQUAL) {
            parse_assignment(parser, name);
            expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach Assignment");
            return;
        }

        // Increment/Decrement
        if (next.type == TOKEN_PLUS_PLUS || next.type == TOKEN_MINUS_MINUS) {
            consume(parser->tokens);
            int offset = get_var_offset(parser, name.value);
            if (offset >= 0) {
                code_printf(parser, "    movl -%d(%%rbp), %%eax\n", offset);
                if (next.type == TOKEN_PLUS_PLUS) {
                    code_printf(parser, "    addl $1, %%eax\n");
                } else {
                    code_printf(parser, "    subl $1, %%eax\n");
                }
                code_printf(parser, "    movl %%eax, -%d(%%rbp)\n", offset);
            }
            expect(parser, TOKEN_SEMICOLON, "Erwarte ';' nach Inkrement/Dekrement");
            return;
        }

        parser_error_at_token(parser, next, "Unerwartetes Token nach Identifier");
        return;
    }

    // Unknown statement
    if (token.type != TOKEN_EOF) {
        parser_error_at_token(parser, token, "Unerwartetes Token im Statement");
        consume(parser->tokens);
    }
}

// ============================================================================
// FUNCTION PARSING
// ============================================================================

void parse_function(Parser *parser) {
    Token name = expect(parser, TOKEN_IDENTIFIER, "Erwarte Funktionsname");

    if (parser->debug_mode) {
        printf("\n   [PARSE] Function: %s\n", name.value);
    }

    // Check if main
    int is_main = strcmp(name.value, "main") == 0;
    if (is_main) {
        parser->main_function_found = 1;
    }

    expect(parser, TOKEN_LPAREN, "Erwarte '(' nach Funktionsname");

    // Parse parameters
    Function *func = &parser->functions[parser->function_count];
    strcpy(func->name, name.value);
    func->param_count = 0;

    if (!check(parser->tokens, TOKEN_RPAREN)) {
        do {
            Token param_name = expect(parser, TOKEN_IDENTIFIER, "Erwarte Parametername");
            expect(parser, TOKEN_COLON, "Erwarte ':' nach Parametername");
            Token param_type = consume(parser->tokens);

            if (func->param_count < 10) {
                strcpy(func->params[func->param_count], param_name.value);
                func->param_types[func->param_count] = token_to_datatype(param_type);
                func->param_count++;
            }

        } while (match(parser->tokens, TOKEN_COMMA));
    }

    expect(parser, TOKEN_RPAREN, "Erwarte ')' nach Parametern");
    expect(parser, TOKEN_ARROW, "Erwarte '->' vor Rueckgabetyp");

    Token return_type = consume(parser->tokens);
    func->return_type = token_to_datatype(return_type);

    // Validate main
    if (is_main && func->return_type != TYPE_VOID) {
        parser_error(parser, "main() muss Rueckgabetyp 'void' haben");
    }

    parser->function_count++;

    // Function prologue
    enter_scope(parser);
    int saved_var_count = parser->var_count;
    int saved_stack_offset = parser->stack_offset;
    parser->stack_offset = 0;

    code_printf(parser, "\n.globl %s\n", name.value);
    code_printf(parser, "%s:\n", name.value);
    code_printf(parser, "    pushq %%rbp\n");
    code_printf(parser, "    movq %%rsp, %%rbp\n");
    code_printf(parser, "    subq $2048, %%rsp\n");

    // Store parameters
    for (int i = 0; i < func->param_count; i++) {
        int offset = create_variable(parser, func->params[i], func->param_types[i]);
        if (offset >= 0) {
            switch (i) {
                case 0: code_printf(parser, "    movl %%ecx, -%d(%%rbp)\n", offset); break;
                case 1: code_printf(parser, "    movl %%edx, -%d(%%rbp)\n", offset); break;
                case 2: code_printf(parser, "    movl %%r8d, -%d(%%rbp)\n", offset); break;
                case 3: code_printf(parser, "    movl %%r9d, -%d(%%rbp)\n", offset); break;
            }
        }
    }

    // Parse body
    expect(parser, TOKEN_LBRACE, "Erwarte '{' vor Funktionskoerper");

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        parse_statement(parser);
    }

    expect(parser, TOKEN_RBRACE, "Erwarte '}' nach Funktionskoerper");

    // Function epilogue
    if (func->return_type == TYPE_VOID) {
        code_printf(parser, "    movl $0, %%eax\n");
    }
    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n\n");

    // Restore state
    parser->var_count = saved_var_count;
    parser->stack_offset = saved_stack_offset;
    exit_scope(parser);
}

// ============================================================================
// MAIN PARSING
// ============================================================================

int parse_program(Parser *parser) {
    printf("\n");
    printf("================================================================\n");
    printf("           PHASE 2: SYNTAKTISCHE ANALYSE (PARSER)\n");
    printf("================================================================\n\n");

    while (!is_at_end(parser->tokens)) {
        Token token = peek(parser->tokens);

        if (token.type == TOKEN_KEYWORD_FUNC) {
            consume(parser->tokens);
            parse_function(parser);
        } else {
            parser_error_at_token(parser, token,
                "Nur Funktionen sind auf Top-Level erlaubt");
            consume(parser->tokens);
        }

        if (parser->has_error) {
            return 0;
        }
    }

    // Check for main
    if (!parser->main_function_found) {
        parser_error(parser, "Keine main() Funktion gefunden");
        return 0;
    }

    printf("OK %d Funktionen geparst\n", parser->function_count);
    printf("OK Syntaktische Analyse erfolgreich\n");

    return 1;
}