#include "parser.h"
#include "parser_internal.h"
#include "errorHandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

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

    if (match(parser->tokens, TOKEN_STAR)) {
        code_comment(parser, "Dereference operator (*)");

        // Check if we're dereferencing a pointer-to-struct with field access: *p.field
        if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token var_token = peek(parser->tokens);
            Variable *var = find_variable(parser, var_token.value);

            if (var && var->is_pointer && var->struct_type[0] != '\0') {
                // Check if next token is DOT for field access
                Token lookahead = peek_ahead(parser->tokens, 1);
                if (lookahead.type == TOKEN_DOT) {
                    // This is *p.field syntax - pointer to struct field access
                    consume(parser->tokens); // consume identifier
                    consume(parser->tokens); // consume dot

                    StructDefinition *struct_def = find_struct(parser, var->struct_type);
                    if (!struct_def) {
                        semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found",
                                       var->struct_type);
                        return;
                    }

                    Token field_name = consume(parser->tokens);
                    StructField *field = NULL;
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (strcmp(struct_def->fields[i].name, field_name.value) == 0) {
                            field = &struct_def->fields[i];
                            break;
                        }
                    }

                    if (!field) {
                        parser_error(parser, "Field '%s' not found in struct '%s'", field_name.value, var->struct_type);
                        return;
                    }

                    code_comment(parser, "Pointer-to-struct field access: *%s.%s", var->name, field_name.value);

                    // Load pointer value and add field offset
                    code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
                    code_printf(parser, "    addq $%d, %%rax\n", field->offset);

                    // Load the field value based on type
                    if (field->type == TYPE_CHAR) {
                        code_printf(parser, "    movsbl (%%rax), %%eax\n");
                    } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                        code_printf(parser, "    movzbl (%%rax), %%eax\n");
                    } else if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss (%%rax), %%xmm0\n");
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                    } else if (field->type == TYPE_DOUBLE) {
                        code_printf(parser, "    movsd (%%rax), %%xmm0\n");
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                    } else if (field->type == TYPE_STRING) {
                        code_printf(parser, "    movq (%%rax), %%rax\n");
                    } else {
                        code_printf(parser, "    movl (%%rax), %%eax\n");
                    }

                    code_printf(parser, "    pushq %%rax\n");
                    return;
                }
            }
        }

        // Regular pointer dereference
        DataType pointed_type = TYPE_INT; // Default to int
        if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token var_token = peek(parser->tokens);
            Variable *var = find_variable(parser, var_token.value);
            if (var && var->is_pointer) {
                pointed_type = var->type;
            }
        }

        parse_unary(parser);
        code_printf(parser, "    popq %%rax\n");

        // Load based on the pointed-to type
        if (pointed_type == TYPE_CHAR) {
            code_printf(parser, "    movsbl (%%rax), %%eax\n");
        } else if (pointed_type == TYPE_BYTE || pointed_type == TYPE_BIT) {
            code_printf(parser, "    movzbl (%%rax), %%eax\n");
        } else if (pointed_type == TYPE_FLOAT) {
            code_printf(parser, "    movss (%%rax), %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %%rax\n");
        } else if (pointed_type == TYPE_DOUBLE) {
            code_printf(parser, "    movsd (%%rax), %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %%rax\n");
        } else if (pointed_type == TYPE_STRING) {
            code_printf(parser, "    movq (%%rax), %%rax\n");
        } else {
            code_printf(parser, "    movl (%%rax), %%eax\n");
        }

        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (match(parser->tokens, TOKEN_AMPERSAND)) {
        code_comment(parser, "Address-of operator (&)");

        if (!check(parser->tokens, TOKEN_IDENTIFIER)) {
            parser_error(parser, "Expected variable name after '&'");
            return;
        }
        Token var_token = consume(parser->tokens);
        Variable *var = find_variable(parser, var_token.value);
        if (var == NULL) {
            semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", var_token.value);
            return;
        }

        code_printf(parser, "    leaq %d(%%rbp), %%rax\n", var->offset);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    parse_primary(parser);
}

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

        const char *escaped_char = escape_char_for_comment(ch.value);
        code_comment(parser, "Character literal: '%s' (ASCII %d)", escaped_char, char_value);
        code_printf(parser, "    movl $%d, %%eax\n", char_value);
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
        Token str = consume(parser->tokens);
        int str_id = add_string_literal(parser, str.value);

        char escaped_str[512];
        escape_string_for_comment(str.value, escaped_str, sizeof(escaped_str));

        if (check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);

            code_comment(parser, "String literal indexing: \"%s\"[...]", escaped_str);

            parse_expression(parser);

            expect(parser, TOKEN_RBRACKET, "Expected ']' after string index");

            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    leaq .LC%d(%%rip), %%rbx\n", str_id);
            code_printf(parser, "    movzbl (%%rbx, %%rax, 1), %%eax\n");
            code_printf(parser, "    pushq %%rax\n");
        } else {
            code_comment(parser, "String literal: \"%s\"", escaped_str);
            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    pushq %%rax\n");
        }
        return;
    }

    if (check(parser->tokens, TOKEN_KEYWORD_RESERVE)) {
        consume(parser->tokens);
        expect(parser, TOKEN_LPAREN, "Expected '(' after reserve");

        Token type_token = consume(parser->tokens);
        DataType type = token_to_datatype(type_token.type);

        if (type == TYPE_UNKNOWN) {
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                int struct_size = struct_def->total_size;
                code_comment(parser, "Built-in: reserve(%s) - allocating %d bytes for struct",
                             type_token.value, struct_size);
                code_printf(parser, "    movl $%d, %%eax\n", struct_size);
                code_printf(parser, "    pushq %%rax\n");
            } else {
                parser_error(parser, "Unknown type '%s' in reserve()", type_token.value);
                return;
            }
        } else {
            int size = datatype_size(type);
            code_comment(parser, "Built-in: reserve(%s) - allocating %d bytes",
                         datatype_to_string(type), size);
            code_printf(parser, "    movl $%d, %%eax\n", size);
            code_printf(parser, "    pushq %%rax\n");
        }

        expect(parser, TOKEN_RPAREN, "Expected ')' after type in reserve()");

        const char **arg_regs = get_arg_registers_64();
        code_printf(parser, "    popq %s\n", arg_regs[0]);
        {
            generate_stack_align(parser);
            code_printf(parser, "    call malloc\n");
            generate_stack_restore(parser);
        }
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = peek(parser->tokens);
        Token lookahead = peek_ahead(parser->tokens, 1);

        if (lookahead.type == TOKEN_LPAREN) {
            consume(parser->tokens);

            if (is_builtin_string_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");

                if (strcmp(name.value, "strlen") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: strlen()");
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call strlen\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "strcmp") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcmp");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: strcmp()");
                    code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call strcmp\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "strcpy") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcpy");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: strcpy()");
                    code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call strcpy\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "strcat") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcat");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: strcat()");
                    code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call strcat\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "strdup") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: strdup()");
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call strdup\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                }
            }

            if (is_builtin_memory_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");

                if (strcmp(name.value, "malloc") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: malloc()");
                    code_printf(parser, "    popq %%rcx\n");
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call malloc\n");
                        generate_stack_restore(parser);
                    }
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "free") == 0) {
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_comment(parser, "Built-in: free()");
                    const char **arg_regs = get_arg_registers_64();
                    code_printf(parser, "    popq %s\n", arg_regs[0]);
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call free\n");
                        generate_stack_restore(parser);
                    }
                    return;
                }
            }

            if (is_builtin_input_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");
                expect(parser, TOKEN_RPAREN, "Expected ')' - input functions take no arguments");

                if (strcmp(name.value, "scanfInt") == 0) {
                    code_comment(parser, "Built-in: scanfInt()");

                    code_printf(parser, "    subq $16, %%rsp\n");
                    code_printf(parser, "    movq %%rsp, %%r15\n");

                    const char **scanf_regs = get_arg_registers_64();
                    code_printf(parser, "    movq %%r15, %s\n", scanf_regs[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", scanf_regs[0]);
                    code_printf(parser, "    xor %%eax, %%eax\n");
                    {
                        int stack_adj_scanf = get_call_stack_space();
                        if (stack_adj_scanf > 0) {
                            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_scanf);
                        }
                        code_printf(parser, "    call scanf\n");
                        if (stack_adj_scanf > 0) {
                            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_scanf);
                        }
                    }

                    code_printf(parser, "    movl (%%r15), %%eax\n");
                    code_printf(parser, "    addq $16, %%rsp\n");
                    code_printf(parser, "    cltq\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "scanfChar") == 0) {
                    code_comment(parser, "Built-in: scanfChar()");

                    code_printf(parser, "    subq $16, %%rsp\n");
                    code_printf(parser, "    movq %%rsp, %%r15\n");

                    const char **scanf_regs = get_arg_registers_64();
                    code_printf(parser, "    movq %%r15, %s\n", scanf_regs[1]);
                    code_printf(parser, "    leaq .LC_char_input_format(%%rip), %s\n", scanf_regs[0]);
                    code_printf(parser, "    xor %%eax, %%eax\n");
                    {
                        int stack_adj_scanf = get_call_stack_space();
                        if (stack_adj_scanf > 0) {
                            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_scanf);
                        }
                        code_printf(parser, "    call scanf\n");
                        if (stack_adj_scanf > 0) {
                            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_scanf);
                        }
                    }

                    code_printf(parser, "    movzbl (%%r15), %%eax\n");
                    code_printf(parser, "    addq $16, %%rsp\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "scanfString") == 0) {
                    code_comment(parser, "Built-in: scanfString()");

                    code_printf(parser, "    pushq %%r15\n");
                    code_printf(parser, "    pushq %%r14\n");
                    code_printf(parser, "    pushq %%r13\n");

                    code_printf(parser, "    leaq _scanfString_buffer(%%rip), %%r15\n");
                    code_printf(parser, "    movq %%r15, %%r14\n");
                    code_printf(parser, "    movl $255, %%r13d\n");

                    code_printf(parser, ".Lread_loop_%d:\n", parser->label_counter);
                    {
                        int stack_adj_getchar = get_call_stack_space();
                        if (stack_adj_getchar > 0) {
                            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_getchar);
                        }
                        code_printf(parser, "    call getchar\n");
                        if (stack_adj_getchar > 0) {
                            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_getchar);
                        }
                    }
                    code_printf(parser, "    cmp $10, %%eax\n");
                    code_printf(parser, "    je .Lread_done_%d\n", parser->label_counter);
                    code_printf(parser, "    cmp $-1, %%eax\n");
                    code_printf(parser, "    je .Lread_done_%d\n", parser->label_counter);
                    code_printf(parser, "    movb %%al, (%%r15)\n");
                    code_printf(parser, "    incq %%r15\n");
                    code_printf(parser, "    decl %%r13d\n");
                    code_printf(parser, "    jnz .Lread_loop_%d\n", parser->label_counter);

                    code_printf(parser, ".Lread_done_%d:\n", parser->label_counter);
                    code_printf(parser, "    movb $0, (%%r15)\n");
                    code_printf(parser, "    movq %%r14, %%rax\n");

                    code_printf(parser, "    popq %%r13\n");
                    code_printf(parser, "    popq %%r14\n");
                    code_printf(parser, "    popq %%r15\n");

                    code_printf(parser, "    pushq %%rax\n");
                    parser->label_counter++;
                    return;
                }
            }

            if (is_syscall_io_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");

                if (strcmp(name.value, "sys_write") == 0) {
                    code_comment(parser, "Built-in syscall: sys_write(fd, buffer, count)");

                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdx\n");
                    code_printf(parser, "    popq %%rsi\n");
                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    movq $1, %%rax\n");
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "sys_read") == 0) {
                    code_comment(parser, "Built-in syscall: sys_read(fd, buffer, count)");

                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdx\n");
                    code_printf(parser, "    popq %%rsi\n");
                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    movq $0, %%rax\n");
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "sys_open") == 0) {
                    code_comment(parser, "Built-in syscall: sys_open(pathname, flags, mode)");

                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdx\n");
                    code_printf(parser, "    popq %%rsi\n");
                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    movq $2, %%rax\n");
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "sys_close") == 0) {
                    code_comment(parser, "Built-in syscall: sys_close(fd)");

                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    movq $3, %%rax\n");
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "io_strlen") == 0) {
                    code_comment(parser, "Built-in: io_strlen(s)");

                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdi\n");
                    generate_strlen_code(parser, "%rdi", "%rax");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "io_int_to_str") == 0) {
                    code_comment(parser, "Built-in: io_int_to_str(value, buffer, buffer_size)");

                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    code_printf(parser, "    popq %%rdx\n");
                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    popq %%rsi\n");
                    generate_int_to_str_code(parser, "%rsi", "%rdi");

                    generate_strlen_code(parser, "%rdi", "%rax");
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "io_str_to_int") == 0) {
                    code_comment(parser, "Built-in: io_str_to_int(s)");

                    parse_expression(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    int label_num = parser->label_counter++;
                    code_printf(parser, "    popq %%rdi\n");
                    code_printf(parser, "    xorq %%rax, %%rax\n");
                    code_printf(parser, "    xorq %%rcx, %%rcx\n");
                    code_printf(parser, "    movzbl (%%rdi), %%edx\n");
                    code_printf(parser, "    cmpb $45, %%dl\n");
                    code_printf(parser, "    jne .Lstr2int_loop_%d\n", label_num);
                    code_printf(parser, "    movq $1, %%rcx\n");
                    code_printf(parser, "    incq %%rdi\n");
                    code_printf(parser, ".Lstr2int_loop_%d:\n", label_num);
                    code_printf(parser, "    movzbl (%%rdi), %%edx\n");
                    code_printf(parser, "    testb %%dl, %%dl\n");
                    code_printf(parser, "    je .Lstr2int_done_%d\n", label_num);
                    code_printf(parser, "    subb $48, %%dl\n");
                    code_printf(parser, "    cmpb $9, %%dl\n");
                    code_printf(parser, "    ja .Lstr2int_done_%d\n", label_num);
                    code_printf(parser, "    imulq $10, %%rax\n");
                    code_printf(parser, "    movzbl %%dl, %%edx\n");
                    code_printf(parser, "    addq %%rdx, %%rax\n");
                    code_printf(parser, "    incq %%rdi\n");
                    code_printf(parser, "    jmp .Lstr2int_loop_%d\n", label_num);
                    code_printf(parser, ".Lstr2int_done_%d:\n", label_num);
                    code_printf(parser, "    testq %%rcx, %%rcx\n");
                    code_printf(parser, "    je .Lstr2int_positive_%d\n", label_num);
                    code_printf(parser, "    negq %%rax\n");
                    code_printf(parser, ".Lstr2int_positive_%d:\n", label_num);
                    code_printf(parser, "    pushq %%rax\n");
                    return;
                } else if (strcmp(name.value, "read") == 0) {
                    code_comment(parser, "Built-in: read(stream, format)");

                    parse_expression(parser);
                    expect(parser, TOKEN_COMMA, "Expected ','");

                    if (!check(parser->tokens, TOKEN_STRING_LITERAL)) {
                        parser_error(parser, "Format specifier must be a string literal");
                        return;
                    }
                    Token format_token = consume(parser->tokens);
                    expect(parser, TOKEN_RPAREN, "Expected ')'");

                    const char *format = format_token.value;

                    if (strcmp(format, "%i") == 0 || strcmp(format, "%d") == 0) {
                        code_comment(parser, "Read integer from stream");

                        code_printf(parser, "    subq $32, %%rsp\n");
                        code_printf(parser, "    movq %%rsp, %%r15\n");

                        code_printf(parser, "    popq %%rdi\n");

                        code_printf(parser, "    movq %%r15, %%rsi\n");
                        code_printf(parser, "    movq $31, %%rdx\n");
                        code_printf(parser, "    movq $0, %%rax\n");
                        code_printf(parser, "    syscall\n");

                        code_printf(parser, "    movb $0, (%%r15,%%rax,1)\n");

                        int label_num = parser->label_counter++;
                        code_printf(parser, "    movq %%r15, %%rdi\n");
                        code_printf(parser, "    xorq %%rax, %%rax\n");
                        code_printf(parser, "    xorq %%rcx, %%rcx\n");
                        code_printf(parser, "    movzbl (%%rdi), %%edx\n");
                        code_printf(parser, "    cmpb $45, %%dl\n");
                        code_printf(parser, "    jne .Lread_int_loop_%d\n", label_num);
                        code_printf(parser, "    movq $1, %%rcx\n");
                        code_printf(parser, "    incq %%rdi\n");
                        code_printf(parser, ".Lread_int_loop_%d:\n", label_num);
                        code_printf(parser, "    movzbl (%%rdi), %%edx\n");
                        code_printf(parser, "    testb %%dl, %%dl\n");
                        code_printf(parser, "    je .Lread_int_done_%d\n", label_num);
                        code_printf(parser, "    cmpb $10, %%dl\n");
                        code_printf(parser, "    je .Lread_int_done_%d\n", label_num);
                        code_printf(parser, "    subb $48, %%dl\n");
                        code_printf(parser, "    cmpb $9, %%dl\n");
                        code_printf(parser, "    ja .Lread_int_done_%d\n", label_num);
                        code_printf(parser, "    imulq $10, %%rax\n");
                        code_printf(parser, "    movzbl %%dl, %%edx\n");
                        code_printf(parser, "    addq %%rdx, %%rax\n");
                        code_printf(parser, "    incq %%rdi\n");
                        code_printf(parser, "    jmp .Lread_int_loop_%d\n", label_num);
                        code_printf(parser, ".Lread_int_done_%d:\n", label_num);
                        code_printf(parser, "    testq %%rcx, %%rcx\n");
                        code_printf(parser, "    je .Lread_int_positive_%d\n", label_num);
                        code_printf(parser, "    negq %%rax\n");
                        code_printf(parser, ".Lread_int_positive_%d:\n", label_num);

                        code_printf(parser, "    addq $32, %%rsp\n");
                        code_printf(parser, "    pushq %%rax\n");
                        return;
                    } else if (strcmp(format, "%c") == 0) {
                        code_comment(parser, "Read character from stream");

                        code_printf(parser, "    popq %%rdi\n");

                        code_printf(parser, "    subq $16, %%rsp\n");
                        code_printf(parser, "    movq %%rsp, %%rsi\n");

                        code_printf(parser, "    movq $1, %%rdx\n");
                        code_printf(parser, "    movq $0, %%rax\n");
                        code_printf(parser, "    syscall\n");

                        code_printf(parser, "    movzbl (%%rsp), %%eax\n");
                        code_printf(parser, "    addq $16, %%rsp\n");
                        code_printf(parser, "    pushq %%rax\n");
                        return;
                    } else if (strcmp(format, "%s") == 0) {
                        parser_error(parser, "Use read_string() function for string input instead of read()");
                        return;
                    } else {
                        parser_error(parser, "Unsupported format specifier '%s'. Use %%i, %%d, or %%c", format);
                        return;
                    }
                }
            }

            int is_method_call = 0;
            char method_mangled_name[MAX_TOKEN * 2];
            Function *func = find_function(parser, name.value);

            if (!func && parser->current_struct_context[0] != '\0') {
                snprintf(method_mangled_name, sizeof(method_mangled_name), "%s_%s",
                         parser->current_struct_context, name.value);
                func = find_function(parser, method_mangled_name);
                if (func) {
                    is_method_call = 1;
                }
            }

            if (!func) {
                semantic_error(parser, ERR_SEM_UNDEFINED_FUNCTION, "Function '%s' not found", name.value);
                return;
            }

            expect(parser, TOKEN_LPAREN, "Expected '('");

            int arg_count = 0;
            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
                    Token str_token = consume(parser->tokens);
                    int str_id = add_string_literal(parser, str_token.value);

                    char escaped_str[512];
                    escape_string_for_comment(str_token.value, escaped_str, sizeof(escaped_str));
                    code_comment(parser, "String arg: \"%s\"", escaped_str);
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

            const char **arg_regs_64 = get_arg_registers_64();
            const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

            if (is_method_call) {
                int max_reg_args = get_max_reg_args();
                for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
                    DataType param_type = func->param_types[i];
                    int reg_idx = i + 1;

                    if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[reg_idx]);
                    } else if (param_type == TYPE_STRING) {
                        code_printf(parser, "    popq %s\n", arg_regs_64[reg_idx]);
                    } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                        const char **method_self_call_arg_regs_8 = get_arg_registers_8();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %s\n", method_self_call_arg_regs_8[reg_idx]);
                    } else {
                        const char **method_self_call_arg_regs_32 = get_arg_registers_32();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movl %%eax, %s\n", method_self_call_arg_regs_32[reg_idx]);
                    }
                }

                Variable *this_var = find_variable(parser, "this");
                if (this_var) {
                    code_comment(parser, "Method-to-method call: %s()", name.value);
                    code_printf(parser, "    movq %d(%%rbp), %s\n", this_var->offset, arg_regs_64[0]);
                }

                int method_stack_adjust = get_call_stack_space();
                if (method_stack_adjust > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", method_stack_adjust);
                }
                code_printf(parser, "    call %s\n", method_mangled_name);
                if (method_stack_adjust > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", method_stack_adjust);
                }
            } else {
                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    DataType param_type = func->param_types[i];

                    if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[i]);
                    } else if (param_type == TYPE_STRING) {
                        code_printf(parser, "    popq %s\n", arg_regs_64[i]);
                    } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                        const char **expr_arg_regs_8 = get_arg_registers_8();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %s\n", expr_arg_regs_8[i]);
                    } else {
                        const char **expr_arg_regs_32 = get_arg_registers_32();
                        int expr_max_reg_args_32 = get_max_reg_args();
                        code_printf(parser, "    popq %%rax\n");
                        if (i < expr_max_reg_args_32) {
                            code_printf(parser, "    movl %%eax, %s\n", expr_arg_regs_32[i]);
                        }
                    }
                }

                int expr_stack_adjust = get_call_stack_space();
                if (expr_stack_adjust > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", expr_stack_adjust);
                }
                code_printf(parser, "    call %s\n", name.value);
                if (expr_stack_adjust > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", expr_stack_adjust);
                }
            }

            if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %%xmm0, %%rax\n");
                code_printf(parser, "    pushq %%rax\n");
            } else {
                code_printf(parser, "    pushq %%rax\n");
            }
        } else {
            consume(parser->tokens);

            Variable *var = find_variable(parser, name.value);

            if (!var && parser->current_struct_context[0] != '\0') {
                Variable *this_var = find_variable(parser, "this");
                if (this_var && this_var->struct_type[0] != '\0') {
                    StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
                    if (struct_def) {
                        StructField *field = NULL;
                        for (int i = 0; i < struct_def->field_count; i++) {
                            if (strcmp(struct_def->fields[i].name, name.value) == 0) {
                                field = &struct_def->fields[i];
                                break;
                            }
                        }

                        if (field) {
                            if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                                consume(parser->tokens);

                                code_comment(parser, "Array field access through implicit 'this': %s[...]", name.value);

                                parse_expression(parser);

                                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");

                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                                code_printf(parser, "    popq %%rax\n");
                                int element_size = datatype_size(field->type);
                                code_printf(parser, "    imulq $%d, %%rax\n", element_size);
                                code_printf(parser, "    addq $%d, %%rax\n", field->offset);
                                code_printf(parser, "    addq %%rbx, %%rax\n");

                                if (field->type == TYPE_FLOAT) {
                                    code_printf(parser, "    movss (%%rax), %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else if (field->type == TYPE_DOUBLE) {
                                    code_printf(parser, "    movsd (%%rax), %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else if (field->type == TYPE_CHAR) {
                                    code_printf(parser, "    movsbl (%%rax), %%eax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else if (field->type == TYPE_BYTE) {
                                    code_printf(parser, "    movzbl (%%rax), %%eax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else if (field->type == TYPE_BIT) {
                                    code_printf(parser, "    movzbl (%%rax), %%eax\n");
                                    code_printf(parser, "    andl $1, %%eax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else if (field->type == TYPE_STRING) {
                                    code_printf(parser, "    movq (%%rax), %%rax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                } else {
                                    code_printf(parser, "    movl (%%rax), %%eax\n");
                                    code_printf(parser, "    pushq %%rax\n");
                                }
                            } else {
                                code_comment(parser, "Field access through implicit 'this': %s", name.value);

                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                                if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                    code_printf(parser, "    addq $%d, %%rbx\n", field->offset);

                                    StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                                    if (!nested_struct_def) {
                                        parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                                        return;
                                    }

                                    while (check(parser->tokens, TOKEN_DOT)) {
                                        consume(parser->tokens);
                                        Token next_field_name = consume(parser->tokens);

                                        StructField *nested_field = NULL;
                                        for (int i = 0; i < nested_struct_def->field_count; i++) {
                                            if (strcmp(nested_struct_def->fields[i].name, next_field_name.value) == 0) {
                                                nested_field = &nested_struct_def->fields[i];
                                                break;
                                            }
                                        }

                                        if (!nested_field) {
                                            parser_error(parser, "Field '%s' not found in struct '%s'",
                                                         next_field_name.value, nested_struct_def->name);
                                            return;
                                        }

                                        if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                            code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                                            nested_struct_def = find_struct(parser, nested_field->struct_type);
                                            if (!nested_struct_def) {
                                                parser_error(parser, "Nested struct type '%s' not found",
                                                             nested_field->struct_type);
                                                return;
                                            }
                                        } else {
                                            if (nested_field->type == TYPE_FLOAT) {
                                                code_printf(parser, "    movss %d(%%rbx), %%xmm0\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    movq %%xmm0, %%rax\n");
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else if (nested_field->type == TYPE_DOUBLE) {
                                                code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    movq %%xmm0, %%rax\n");
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else if (nested_field->type == TYPE_CHAR) {
                                                code_printf(parser, "    movsbl %d(%%rbx), %%eax\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else if (nested_field->type == TYPE_BYTE) {
                                                code_printf(parser, "    movzbl %d(%%rbx), %%eax\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else if (nested_field->type == TYPE_BIT) {
                                                code_printf(parser, "    movzbl %d(%%rbx), %%eax\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    andl $1, %%eax\n");
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else if (nested_field->type == TYPE_STRING) {
                                                code_printf(parser, "    movq %d(%%rbx), %%rax\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    pushq %%rax\n");
                                            } else {
                                                code_printf(parser, "    movl %d(%%rbx), %%eax\n",
                                                            nested_field->offset);
                                                code_printf(parser, "    pushq %%rax\n");
                                            }
                                            break;
                                        }
                                    }
                                } else {
                                    if (field->type == TYPE_FLOAT) {
                                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (field->type == TYPE_DOUBLE) {
                                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (field->type == TYPE_CHAR) {
                                        code_printf(parser, "    movsbl %d(%%rbx), %%eax\n", field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (field->type == TYPE_BYTE) {
                                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (field->type == TYPE_BIT) {
                                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                                        code_printf(parser, "    andl $1, %%eax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (field->type == TYPE_STRING) {
                                        code_printf(parser, "    movq %d(%%rbx), %%rax\n", field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else {
                                        code_printf(parser, "    movl %d(%%rbx), %%eax\n", field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    }
                                }
                            }

                            return;
                        }
                    }
                }
            }

            if (!var) {
                EnumDefinition *enum_def = find_enum(parser, name.value);
                if (enum_def && check(parser->tokens, TOKEN_DOT)) {
                    consume(parser->tokens);
                    Token value_name_token = consume(parser->tokens);

                    EnumValue *enum_value = NULL;
                    for (int i = 0; i < enum_def->value_count; i++) {
                        if (strcmp(enum_def->values[i].name, value_name_token.value) == 0) {
                            enum_value = &enum_def->values[i];
                            break;
                        }
                    }

                    if (!enum_value) {
                        parser_error(parser, "Enum value '%s' not found in enum '%s'",
                                     value_name_token.value, name.value);
                        return;
                    }

                    if (!check(parser->tokens, TOKEN_DOT)) {
                        parser_error(parser, "Expected '.' after enum value '%s'", value_name_token.value);
                        return;
                    }

                    consume(parser->tokens);
                    Token field_name_token = consume(parser->tokens);

                    StructField *field = NULL;
                    for (int i = 0; i < enum_def->field_count; i++) {
                        if (strcmp(enum_def->fields[i].name, field_name_token.value) == 0) {
                            field = &enum_def->fields[i];
                            break;
                        }
                    }

                    if (!field) {
                        parser_error(parser, "Field '%s' not found in enum '%s'",
                                     field_name_token.value, name.value);
                        return;
                    }

                    char global_name[MAX_TOKEN * 2];
                    snprintf(global_name, sizeof(global_name), "%s_%s", name.value, value_name_token.value);

                    code_comment(parser, "Access enum field: %s.%s.%s",
                                 name.value, value_name_token.value, field_name_token.value);
                    code_printf(parser, "    leaq %s(%%rip), %%rbx\n", global_name);

                    if (field->type == TYPE_INT) {
                        code_printf(parser, "    movq %d(%%rbx), %%rax\n", field->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_DOUBLE) {
                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_CHAR) {
                        code_printf(parser, "    movsbl %d(%%rbx), %%eax\n", field->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_BYTE) {
                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_BIT) {
                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                        code_printf(parser, "    andl $1, %%eax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else if (field->type == TYPE_STRING) {
                        code_printf(parser, "    movq %d(%%rbx), %%rax\n", field->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        code_printf(parser, "    movl %d(%%rbx), %%eax\n", field->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    }

                    return;
                }

                StructDefinition *struct_def = find_struct(parser, name.value);
                if (struct_def && check(parser->tokens, TOKEN_DOT)) {
                    consume(parser->tokens);
                    Token method_name = consume(parser->tokens);

                    if (!check(parser->tokens, TOKEN_LPAREN)) {
                        parser_error(parser, "Expected '(' after static method name");
                        return;
                    }

                    consume(parser->tokens);

                    char mangled_name[MAX_TOKEN * 2];
                    snprintf(mangled_name, sizeof(mangled_name), "%s_%s", name.value, method_name.value);

                    Function *method = find_function(parser, mangled_name);
                    if (!method) {
                        parser_error(parser, "Static method '%s' not found in struct '%s'", method_name.value,
                                     name.value);
                        return;
                    }

                    if (!method->is_static) {
                        parser_error(parser, "Method '%s' is not static. Use an instance to call it.",
                                     method_name.value);
                        return;
                    }

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

                    expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                    if (arg_count != method->param_count) {
                        parser_error(parser, "Static method '%s' expects %d arguments",
                                     method_name.value, method->param_count);
                        return;
                    }

                    const char **arg_regs_64 = get_arg_registers_64();
                    const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

                    int max_reg_args = get_max_reg_args();
                    for (int i = arg_count - 1; i >= 0 && i < max_reg_args; i--) {
                        DataType param_type = method->param_types[i];

                        if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[i]);
                        } else if (param_type == TYPE_STRING) {
                            code_printf(parser, "    popq %s\n", arg_regs_64[i]);
                        } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                            const char **static_expr_arg_regs_8 = get_arg_registers_8();
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movb %%al, %s\n", static_expr_arg_regs_8[i]);
                        } else {
                            const char **static_expr_arg_regs_32 = get_arg_registers_32();
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movl %%eax, %s\n", static_expr_arg_regs_32[i]);
                        }
                    }

                    {
                        int stack_adj_static_expr = get_call_stack_space();
                        if (stack_adj_static_expr > 0) {
                            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_static_expr);
                        }
                        code_printf(parser, "    call %s\n", mangled_name);
                        if (stack_adj_static_expr > 0) {
                            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_static_expr);
                        }
                    }

                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        code_printf(parser, "    pushq %%rax\n");
                    }

                    return;
                }

                semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                return;
            }

            if (check(parser->tokens, TOKEN_DOT)) {
                consume(parser->tokens);

                if (var->struct_type[0] == '\0') {
                    parser_error(parser, "Variable '%s' is not a struct", name.value);
                    return;
                }

                StructDefinition *struct_def = find_struct(parser, var->struct_type);
                if (!struct_def) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found", var->struct_type);
                    return;
                }

                Token member_name = consume(parser->tokens);

                if (check(parser->tokens, TOKEN_LPAREN)) {
                    consume(parser->tokens);

                    char mangled_name[MAX_TOKEN * 2];
                    snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, member_name.value);

                    Function *method = find_function(parser, mangled_name);
                    if (!method) {
                        parser_error(parser, "Method '%s' not found in struct '%s'", member_name.value,
                                     var->struct_type);
                        return;
                    }

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

                    expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                    if (arg_count != method->param_count) {
                        parser_error(parser, "Method '%s' expects %d arguments (excluding implicit 'this')",
                                     member_name.value, method->param_count);
                        return;
                    }

                    const char **arg_regs_64 = get_arg_registers_64();
                    const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

                    int max_reg_args = get_max_reg_args();
                    for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
                        DataType param_type = method->param_types[i];
                        int reg_idx = i + 1;

                        if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movq %%rax, %s\n", arg_regs_float[reg_idx]);
                        } else if (param_type == TYPE_STRING) {
                            code_printf(parser, "    popq %s\n", arg_regs_64[reg_idx]);
                        } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                            const char **method_call_arg_regs_8 = get_arg_registers_8();
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movb %%al, %s\n", method_call_arg_regs_8[reg_idx]);
                        } else {
                            const char **method_call2_arg_regs_32 = get_arg_registers_32();
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movl %%eax, %s\n", method_call2_arg_regs_32[reg_idx]);
                        }
                    }

                    code_comment(parser, "Method call: %s.%s()", name.value, member_name.value);
                    const char **this_regs2_64 = get_arg_registers_64();
                    code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this_regs2_64[0]);

                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call %s\n", mangled_name);
                        generate_stack_restore(parser);
                    }

                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        code_printf(parser, "    pushq %%rax\n");
                    }

                    return;
                } else {
                    StructField *field = NULL;
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (strcmp(struct_def->fields[i].name, member_name.value) == 0) {
                            field = &struct_def->fields[i];
                            break;
                        }
                    }

                    if (!field) {
                        parser_error(parser, "Field '%s' not found in struct '%s'", member_name.value,
                                     var->struct_type);
                        return;
                    }

                    if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                        consume(parser->tokens);

                        code_comment(parser, "Array field access: %s.%s[...]", name.value, member_name.value);

                        parse_expression(parser);

                        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");

                        code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

                        code_printf(parser, "    popq %%rax\n");
                        int element_size = datatype_size(field->type);
                        code_printf(parser, "    imulq $%d, %%rax\n", element_size);
                        code_printf(parser, "    addq $%d, %%rax\n", field->offset);
                        code_printf(parser, "    addq %%rbx, %%rax\n");

                        if (field->type == TYPE_FLOAT) {
                            code_printf(parser, "    movss (%%rax), %%xmm0\n");
                            code_printf(parser, "    movq %%xmm0, %%rax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else if (field->type == TYPE_DOUBLE) {
                            code_printf(parser, "    movsd (%%rax), %%xmm0\n");
                            code_printf(parser, "    movq %%xmm0, %%rax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else if (field->type == TYPE_CHAR) {
                            code_printf(parser, "    movsbl (%%rax), %%eax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else if (field->type == TYPE_BYTE) {
                            code_printf(parser, "    movzbl (%%rax), %%eax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else if (field->type == TYPE_BIT) {
                            code_printf(parser, "    movzbl (%%rax), %%eax\n");
                            code_printf(parser, "    andl $1, %%eax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else if (field->type == TYPE_STRING) {
                            code_printf(parser, "    movq (%%rax), %%rax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        } else {
                            code_printf(parser, "    movl (%%rax), %%eax\n");
                            code_printf(parser, "    pushq %%rax\n");
                        }
                    } else {
                        code_comment(parser, "Field access: %s.%s", name.value, member_name.value);

                        code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

                        if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                            code_printf(parser, "    addq $%d, %%rbx\n", field->offset);

                            StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                            if (!nested_struct_def) {
                                parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                                return;
                            }

                            while (check(parser->tokens, TOKEN_DOT)) {
                                consume(parser->tokens);
                                Token next_field_name = consume(parser->tokens);

                                StructField *nested_field = NULL;
                                for (int i = 0; i < nested_struct_def->field_count; i++) {
                                    if (strcmp(nested_struct_def->fields[i].name, next_field_name.value) == 0) {
                                        nested_field = &nested_struct_def->fields[i];
                                        break;
                                    }
                                }

                                if (!nested_field) {
                                    parser_error(parser, "Field '%s' not found in struct '%s'",
                                                 next_field_name.value, nested_struct_def->name);
                                    return;
                                }

                                if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                    code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                                    nested_struct_def = find_struct(parser, nested_field->struct_type);
                                    if (!nested_struct_def) {
                                        parser_error(parser, "Nested struct type '%s' not found",
                                                     nested_field->struct_type);
                                        return;
                                    }
                                } else {
                                    if (nested_field->type == TYPE_FLOAT) {
                                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", nested_field->offset);
                                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (nested_field->type == TYPE_DOUBLE) {
                                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", nested_field->offset);
                                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (nested_field->type == TYPE_CHAR) {
                                        code_printf(parser, "    movsbl %d(%%rbx), %%eax\n", nested_field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (nested_field->type == TYPE_BYTE) {
                                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", nested_field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (nested_field->type == TYPE_BIT) {
                                        code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", nested_field->offset);
                                        code_printf(parser, "    andl $1, %%eax\n");
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else if (nested_field->type == TYPE_STRING) {
                                        code_printf(parser, "    movq %d(%%rbx), %%rax\n", nested_field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    } else {
                                        code_printf(parser, "    movl %d(%%rbx), %%eax\n", nested_field->offset);
                                        code_printf(parser, "    pushq %%rax\n");
                                    }
                                    break;
                                }
                            }
                        } else {
                            if (field->type == TYPE_FLOAT) {
                                code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                code_printf(parser, "    movq %%xmm0, %%rax\n");
                                code_printf(parser, "    pushq %%rax\n");
                            } else if (field->type == TYPE_DOUBLE) {
                                code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                code_printf(parser, "    movq %%xmm0, %%rax\n");
                                code_printf(parser, "    pushq %%rax\n");
                            } else if (field->type == TYPE_CHAR) {
                                code_printf(parser, "    movsbl %d(%%rbx), %%eax\n", field->offset);
                                code_printf(parser, "    pushq %%rax\n");
                            } else if (field->type == TYPE_BYTE) {
                                code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                                code_printf(parser, "    pushq %%rax\n");
                            } else if (field->type == TYPE_BIT) {
                                code_printf(parser, "    movzbl %d(%%rbx), %%eax\n", field->offset);
                                code_printf(parser, "    andl $1, %%eax\n");
                                code_printf(parser, "    pushq %%rax\n");
                            } else if (field->type == TYPE_STRING) {
                                code_printf(parser, "    movq %d(%%rbx), %%rax\n", field->offset);
                                code_printf(parser, "    pushq %%rax\n");
                            } else {
                                code_printf(parser, "    movl %d(%%rbx), %%eax\n", field->offset);
                                code_printf(parser, "    pushq %%rax\n");
                            }
                        }
                    }

                    return;
                }
            }

            if (check(parser->tokens, TOKEN_LBRACKET)) {
                consume(parser->tokens);

                if (!var->is_array && var->type != TYPE_STRING) {
                    parser_error(parser, "Variable '%s' is not an array or string", name.value);
                    return;
                }

                parse_expression(parser);

                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");

                int element_size = datatype_size(var->type);

                code_printf(parser, "    popq %%rax\n");

                if (var->is_array && var->array_size == 0) {
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
                } else {
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                }

                if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss (%%rbx, %%rax, %d), %%xmm0\n", element_size);
                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd (%%rbx, %%rax, %d), %%xmm0\n", element_size);
                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl (%%rbx, %%rax, %d), %%eax\n", element_size);
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl (%%rbx, %%rax, %d), %%eax\n", element_size);
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
                    code_printf(parser, "    movzbl (%%rbx, %%rax, 1), %%eax\n");
                    code_printf(parser, "    pushq %%rax\n");
                } else {
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %%eax\n", element_size);
                    code_printf(parser, "    pushq %%rax\n");
                }
            } else {
                if (var->is_array) {
                    if (var->array_size == 0) {
                        code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        code_printf(parser, "    leaq %d(%%rbp), %%rax\n", var->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    }
                } else if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %%rax\n");
                    code_printf(parser, "    pushq %%rax\n");
                } else if (var->is_pointer) {
                    // Check is_pointer BEFORE checking type, since pointers are always 8 bytes
                    code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
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
