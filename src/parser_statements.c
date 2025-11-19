#include "parser.h"
#include "parser_internal.h"
#include "errorHandler.h"
#include "instruction_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

void parse_statement(Parser *parser) {
    if (check(parser->tokens, TOKEN_AT)) {
        consume(parser->tokens);
        expect(parser, TOKEN_KEYWORD_GC, "Expected 'gc' after '@'");
        parser->next_var_is_gc = 1;
        if (!check(parser->tokens, TOKEN_KEYWORD_VAR)) {
            parser_error(parser, "Expected 'var' after '@gc'");
            return;
        }
        parse_variable_declaration(parser);
        parser->next_var_is_gc = 0;
    } else if (check(parser->tokens, TOKEN_KEYWORD_VAR)) {
        parse_variable_declaration(parser);
    } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token lookahead = peek_ahead(parser->tokens, 1);
        if (lookahead.type == TOKEN_DOT) {
            Token lookahead2 = peek_ahead(parser->tokens, 2);
            Token lookahead3 = peek_ahead(parser->tokens, 3);
            if (lookahead3.type == TOKEN_LPAREN) {
                parse_function_call_statement(parser);
            } else {
                parse_assignment(parser);
            }
        } else if (lookahead.type == TOKEN_EQUAL || lookahead.type == TOKEN_PLUS_EQUAL ||
                   lookahead.type == TOKEN_MINUS_EQUAL || lookahead.type == TOKEN_STAR_EQUAL ||
                   lookahead.type == TOKEN_SLASH_EQUAL || lookahead.type == TOKEN_PLUS_PLUS ||
                   lookahead.type == TOKEN_MINUS_MINUS || lookahead.type == TOKEN_LBRACKET) {
            parse_assignment(parser);
        } else if (lookahead.type == TOKEN_LPAREN) {
            parse_function_call_statement(parser);
        } else {
            parser_error_code(parser, ERR_PARSE_INVALID_SYNTAX,
                              "Unexpected token: expected operator, semicolon, or end of statement");
            synchronize(parser);
        }
    } else if (check(parser->tokens, TOKEN_KEYWORD_FOR)) {
        parse_for_loop(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_IF)) {
        parse_if_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_RETURN)) {
        parse_return_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_PRINT)) {
        parse_print_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_PRINTLINE)) {
        parse_printline_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_BREAK)) {
        parse_break_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_CONTINUE)) {
        parse_continue_statement(parser);
    } else if (check(parser->tokens, TOKEN_KEYWORD_FREE)) {
        Token free_token = consume(parser->tokens);
        expect(parser, TOKEN_LPAREN, "Expected '(' after free");

        Token var_token = consume(parser->tokens);
        if (var_token.type != TOKEN_IDENTIFIER) {
            parser_error(parser, "Expected variable name in free()");
            synchronize(parser);
            return;
        }

        Variable *var = find_variable(parser, var_token.value);
        if (var == NULL) {
            semantic_error_at_token(parser, var_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found",
                                    var_token.value);
            synchronize(parser);
            return;
        }

        if (!var->is_pointer) {
            parser_error(parser, "Variable '%s' is not a pointer", var_token.value);
            synchronize(parser);
            return;
        }

        expect(parser, TOKEN_RPAREN, "Expected ')' after pointer in free()");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after free statement");

        code_comment(parser, "Statement: free(%s) at line %d", var_token.value, free_token.line);
        const char **arg_regs = get_arg_registers_64();

        code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, arg_regs[0]);
        generate_stack_align(parser);
        code_printf(parser, "    call free\n");
        generate_stack_restore(parser);
    } else if (check(parser->tokens, TOKEN_STAR)) {
        Token star_token = consume(parser->tokens);

        if (!check(parser->tokens, TOKEN_IDENTIFIER)) {
            parser_error(parser, "Expected identifier after '*' in assignment");
            synchronize(parser);
            return;
        }

        Token ptr_token = consume(parser->tokens);
        Variable *ptr_var = find_variable(parser, ptr_token.value);

        if (!ptr_var) {
            semantic_error_at_token(parser, ptr_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found",
                                    ptr_token.value);
            synchronize(parser);
            return;
        }

        if (!ptr_var->is_pointer) {
            parser_error(parser, "Variable '%s' is not a pointer", ptr_token.value);
            synchronize(parser);
            return;
        }

        // Check for *p.field syntax
        if (check(parser->tokens, TOKEN_DOT)) {
            consume(parser->tokens); // consume dot

            if (ptr_var->struct_type[0] == '\0') {
                parser_error(parser, "Variable '%s' is not a pointer to struct", ptr_token.value);
                synchronize(parser);
                return;
            }

            StructDefinition *struct_def = find_struct(parser, ptr_var->struct_type);
            if (!struct_def) {
                semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found", ptr_var->struct_type);
                synchronize(parser);
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
                parser_error(parser, "Field '%s' not found in struct '%s'", field_name.value, ptr_var->struct_type);
                synchronize(parser);
                return;
            }

            expect(parser, TOKEN_EQUAL, "Expected '=' after *p.field");

            code_comment(parser, "Line %d: *%s.%s = value (pointer-to-struct field assignment)",
                         star_token.line, ptr_token.value, field_name.value);

            parse_expression(parser);

            expect(parser, TOKEN_SEMICOLON, "Expected ';' after *p.field assignment");

            // Load pointer and add field offset
            code_printf(parser, "    movq %d(%%rbp), %%rbx\n", ptr_var->offset);
            code_printf(parser, "    addq $%d, %%rbx\n", field->offset);
            code_printf(parser, "    popq %%rax\n");

            // Store based on field type
            if (field->type == TYPE_FLOAT) {
                code_printf(parser, "    movq %%rax, %%xmm0\n");
                code_printf(parser, "    movss %%xmm0, (%%rbx)\n");
            } else if (field->type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %%rax, %%xmm0\n");
                code_printf(parser, "    movsd %%xmm0, (%%rbx)\n");
            } else if (field->type == TYPE_CHAR || field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                code_printf(parser, "    movb %%al, (%%rbx)\n");
            } else if (field->type == TYPE_STRING) {
                code_printf(parser, "    movq %%rax, (%%rbx)\n");
            } else {
                code_printf(parser, "    movl %%eax, (%%rbx)\n");
            }
        } else {
            // Regular pointer dereference assignment
            expect(parser, TOKEN_EQUAL, "Expected '=' after pointer dereference");

            code_comment(parser, "Line %d: *%s = value (pointer dereference assignment)",
                         star_token.line, ptr_token.value);

            parse_expression(parser);

            expect(parser, TOKEN_SEMICOLON, "Expected ';' after pointer dereference assignment");

            code_printf(parser, "    movq %d(%%rbp), %%rbx\n", ptr_var->offset);
            code_printf(parser, "    popq %%rax\n");

            if (ptr_var->type == TYPE_FLOAT) {
                code_printf(parser, "    movq %%rax, %%xmm0\n");
                code_printf(parser, "    movss %%xmm0, (%%rbx)\n");
            } else if (ptr_var->type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %%rax, %%xmm0\n");
                code_printf(parser, "    movsd %%xmm0, (%%rbx)\n");
            } else if (ptr_var->type == TYPE_CHAR || ptr_var->type == TYPE_BYTE || ptr_var->type == TYPE_BIT) {
                code_printf(parser, "    movb %%al, (%%rbx)\n");
            } else if (ptr_var->type == TYPE_STRING) {
                code_printf(parser, "    movq %%rax, (%%rbx)\n");
            } else {
                code_printf(parser, "    movl %%eax, (%%rbx)\n");
            }
        }
    } else {
        parser_error_code(parser, ERR_PARSE_INVALID_SYNTAX,
                          "Unexpected statement: expected expression or statement keyword");
        synchronize(parser);
    }
}

void parse_variable_declaration(Parser *parser) {
    Token var_token = peek(parser->tokens);
    consume(parser->tokens);

    int is_array = 0;
    int array_size = 0;

    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        is_array = 1;

        if (check(parser->tokens, TOKEN_NUMBER)) {
            Token size_token = consume(parser->tokens);
            array_size = atoi(size_token.value);

            if (array_size <= 0) {
                parser_error(parser, "Array size must be positive");
                return;
            }
        }

        expect(parser, TOKEN_RBRACKET, "Expected ']' after array size");
    }

    Token name_token = consume(parser->tokens);

    DataType var_type = TYPE_INT;
    int has_explicit_type = 0;
    char struct_type_name[MAX_TOKEN] = "";
    int is_pointer = 0;
    int has_initialization = 0;

    if (check(parser->tokens, TOKEN_COLON)) {
        consume(parser->tokens);

        if (check(parser->tokens, TOKEN_STAR)) {
            consume(parser->tokens);
            is_pointer = 1;
        }

        Token type_token = consume(parser->tokens);
        var_type = token_to_datatype(type_token.type);

        if (var_type == TYPE_UNKNOWN) {
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                var_type = TYPE_INT;
                strcpy(struct_type_name, type_token.value);
                has_explicit_type = 1;
            } else {
                parser_error(parser, "Unknown variable type '%s'", type_token.value);
                return;
            }
        } else {
            has_explicit_type = 1;
        }
    }

    if (is_array && !has_explicit_type) {
        parser_error(parser, "Arrays must have explicit type specified");
        return;
    }

    if (is_array && check(parser->tokens, TOKEN_EQUAL)) {
        parser_error(parser, "Array initialization with '=' not supported. Use assignment to elements.");
        return;
    }

    if (!is_array && check(parser->tokens, TOKEN_EQUAL)) {
        consume(parser->tokens);
        has_initialization = 1;

        if (!has_explicit_type) {
            if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
                var_type = TYPE_FLOAT;
            } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
                var_type = TYPE_CHAR;
            } else if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
                var_type = TYPE_STRING;
            }
        }

        if (var_type == TYPE_STRING && check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            char escaped_str[512];
            escape_string_for_comment(str_token.value, escaped_str, sizeof(escaped_str));
            code_comment(parser, "Line %d: var %s:string = \"%s\"",
                         var_token.line, name_token.value, escaped_str);

            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    pushq %%rax\n");
        } else {
            code_comment(parser, "Line %d: var %s:%s = ...",
                         var_token.line, name_token.value, datatype_to_string(var_type));
            parse_expression(parser);
        }
    } else if (!is_array) {
        if (!has_explicit_type) {
            parser_error(parser, "Variable '%s' without type and without initialization", name_token.value);
            return;
        }

        if (struct_type_name[0] != '\0') {
            StructDefinition *struct_def = find_struct(parser, struct_type_name);
            code_comment(parser, "Line %d: var %s:%s (struct, space reserved in stack frame)",
                         var_token.line, name_token.value, struct_type_name);
        } else {
            code_comment(parser, "Line %d: var %s:%s (uninitialized)",
                         var_token.line, name_token.value, datatype_to_string(var_type));
        }
    } else {
        code_comment(parser, "Line %d: var[%d] %s:%s (array)",
                     var_token.line, array_size, name_token.value, datatype_to_string(var_type));
    }

    expect(parser, TOKEN_SEMICOLON, "Expected ';' at end of variable declaration");

    if (parser->var_count >= MAX_VARS) {
        parser_error(parser, "Too many variables (max %d)", MAX_VARS);
        return;
    }

    Variable *var = &parser->vars[parser->var_count];
    strcpy(var->name, name_token.value);
    var->type = var_type;
    var->is_array = is_array;
    var->array_size = array_size;
    strcpy(var->struct_type, struct_type_name);
    var->is_pointer = is_pointer;
    var->is_gc = parser->next_var_is_gc;
    var->is_heap = 0;

    int element_size;
    if (struct_type_name[0] != '\0') {
        StructDefinition *struct_def = find_struct(parser, struct_type_name);
        element_size = struct_def->total_size;
    } else if (is_pointer) {
        element_size = 8;
    } else {
        element_size = datatype_size(var_type);
    }

    if (is_array) {
        if (array_size > 0) {
            var->size = element_size * array_size;
        } else {
            var->size = 8;
        }
    } else {
        var->size = element_size;
    }

    int smallest_offset = 0;

    for (int i = 0; i < parser->var_count; i++) {
        Variable *v = &parser->vars[i];

        if (v->scope >= 1 && v->scope <= parser->current_scope) {
            int var_end = v->offset - v->size;
            if (var_end < smallest_offset) {
                smallest_offset = var_end;
            }
        }
    }

    int new_offset = smallest_offset;

    if (var->size == 8) {
        if ((-new_offset) % 8 != 0) {
            new_offset = -((((-new_offset) / 8) + 1) * 8);
        }
    }

    var->offset = new_offset - var->size;
    var->scope = parser->current_scope;
    parser->var_count++;

    if (has_initialization) {
        if (is_array) {
            code_comment(parser, "Array allocated at offset %d, total size %d bytes",
                         var->offset, var->size);
        } else if (is_pointer) {
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
        } else if (struct_type_name[0] != '\0') {
            code_comment(parser, "Struct variable allocated at offset %d, total size %d bytes",
                         var->offset, var->size);
        } else if (var_type == TYPE_FLOAT) {
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
}

void parse_assignment(Parser *parser) {
    Token assign_token = peek(parser->tokens);
    Token name_token = consume(parser->tokens);
    Variable *var = find_variable(parser, name_token.value);

    if (!var && parser->current_struct_context[0] != '\0') {
        Variable *this_var = find_variable(parser, "this");
        if (this_var && this_var->struct_type[0] != '\0') {
            StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
            if (struct_def) {
                StructField *field = NULL;
                for (int i = 0; i < struct_def->field_count; i++) {
                    if (strcmp(struct_def->fields[i].name, name_token.value) == 0) {
                        field = &struct_def->fields[i];
                        break;
                    }
                }

                if (field) {
                    if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                        consume(parser->tokens);

                        code_comment(parser, "Line %d: %s[index] = value (array field through implicit 'this')",
                                     assign_token.line, name_token.value);

                        parse_expression(parser);

                        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                        expect(parser, TOKEN_EQUAL, "Expected '=' in array field assignment");

                        parse_expression(parser);

                        code_printf(parser, "    popq %%rcx\n");
                        code_printf(parser, "    popq %%rax\n");

                        code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                        int element_size = datatype_size(field->type);
                        code_printf(parser, "    imulq $%d, %%rax\n", element_size);
                        code_printf(parser, "    addq $%d, %%rax\n", field->offset);
                        code_printf(parser, "    addq %%rbx, %%rax\n");

                        if (field->type == TYPE_FLOAT || field->type == TYPE_DOUBLE) {
                            code_printf(parser, "    movq %%rcx, %%xmm0\n");
                            if (field->type == TYPE_FLOAT) {
                                code_printf(parser, "    movss %%xmm0, (%%rax)\n");
                            } else {
                                code_printf(parser, "    movsd %%xmm0, (%%rax)\n");
                            }
                        } else if (field->type == TYPE_CHAR || field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                            code_printf(parser, "    movb %%cl, (%%rax)\n");
                        } else if (field->type == TYPE_STRING) {
                            code_printf(parser, "    movq %%rcx, (%%rax)\n");
                        } else {
                            code_printf(parser, "    movl %%ecx, (%%rax)\n");
                        }

                        expect(parser, TOKEN_SEMICOLON, "Expected ';' after array field assignment");
                        return;
                    } else {
                        expect(parser, TOKEN_EQUAL, "Expected '=' in field assignment");

                        code_comment(parser, "Line %d: %s = value (field through implicit 'this')",
                                     assign_token.line, name_token.value);

                        parse_expression(parser);

                        code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                        if (field->type == TYPE_FLOAT || field->type == TYPE_DOUBLE) {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movq %%rax, %%xmm0\n");
                            if (field->type == TYPE_FLOAT) {
                                code_printf(parser, "    movss %%xmm0, %d(%%rbx)\n", field->offset);
                            } else {
                                code_printf(parser, "    movsd %%xmm0, %d(%%rbx)\n", field->offset);
                            }
                        } else if (field->type == TYPE_CHAR || field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movb %%al, %d(%%rbx)\n", field->offset);
                        } else if (field->type == TYPE_STRING) {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movq %%rax, %d(%%rbx)\n", field->offset);
                        } else {
                            code_printf(parser, "    popq %%rax\n");
                            code_printf(parser, "    movl %%eax, %d(%%rbx)\n", field->offset);
                        }

                        expect(parser, TOKEN_SEMICOLON, "Expected ';' after field assignment");
                        return;
                    }
                }
            }
        }
    }

    if (!var) {
        semantic_error_at_token(parser, name_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found",
                                name_token.value);
        synchronize(parser);
        return;
    }

    if (check(parser->tokens, TOKEN_DOT)) {
        consume(parser->tokens);

        if (var->struct_type[0] == '\0') {
            parser_error(parser, "Variable '%s' is not a struct", name_token.value);
            synchronize(parser);
            return;
        }

        StructDefinition *struct_def = find_struct(parser, var->struct_type);
        if (!struct_def) {
            semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found", var->struct_type);
            synchronize(parser);
            return;
        }

        Token field_name_token = consume(parser->tokens);

        StructField *field = NULL;
        for (int i = 0; i < struct_def->field_count; i++) {
            if (strcmp(struct_def->fields[i].name, field_name_token.value) == 0) {
                field = &struct_def->fields[i];
                break;
            }
        }

        if (!field) {
            parser_error(parser, "Field '%s' not found in struct '%s'", field_name_token.value, var->struct_type);
            synchronize(parser);
            return;
        }

        if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);

            code_comment(parser, "Line %d: %s.%s[index] = value (array field)",
                         assign_token.line, name_token.value, field_name_token.value);

            parse_expression(parser);

            expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
            expect(parser, TOKEN_EQUAL, "Expected '=' in array field assignment");

            parse_expression(parser);

            code_printf(parser, "    popq %%rcx\n");
            code_printf(parser, "    popq %%rax\n");

            code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

            int element_size = datatype_size(field->type);
            code_printf(parser, "    imulq $%d, %%rax\n", element_size);
            code_printf(parser, "    addq $%d, %%rax\n", field->offset);
            code_printf(parser, "    addq %%rbx, %%rax\n");

            if (field->type == TYPE_FLOAT || field->type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %%rcx, %%xmm0\n");
                if (field->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss %%xmm0, (%%rax)\n");
                } else {
                    code_printf(parser, "    movsd %%xmm0, (%%rax)\n");
                }
            } else if (field->type == TYPE_CHAR || field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                code_printf(parser, "    movb %%cl, (%%rax)\n");
            } else if (field->type == TYPE_STRING) {
                code_printf(parser, "    movq %%rcx, (%%rax)\n");
            } else {
                code_printf(parser, "    movl %%ecx, (%%rax)\n");
            }

            expect(parser, TOKEN_SEMICOLON, "Expected ';' after array field assignment");
            return;
        } else {
            if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                code_comment(parser, "Line %d: %s.%s. ... = value (chained)",
                             assign_token.line, name_token.value, field_name_token.value);

                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                code_printf(parser, "    addq $%d, %%rbx\n", field->offset);

                StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                if (!nested_struct_def) {
                    parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                    synchronize(parser);
                    return;
                }

                StructField *final_field = NULL;

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
                        synchronize(parser);
                        return;
                    }

                    if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                        code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                        nested_struct_def = find_struct(parser, nested_field->struct_type);
                        if (!nested_struct_def) {
                            parser_error(parser, "Nested struct type '%s' not found", nested_field->struct_type);
                            synchronize(parser);
                            return;
                        }
                    } else {
                        final_field = nested_field;
                        break;
                    }
                }

                if (!final_field) {
                    parser_error(parser, "Invalid chained field access");
                    synchronize(parser);
                    return;
                }

                expect(parser, TOKEN_EQUAL, "Expected '=' in chained field assignment");

                parse_expression(parser);

                if (final_field->type == TYPE_FLOAT || final_field->type == TYPE_DOUBLE) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %%xmm0\n");
                    if (final_field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %%xmm0, %d(%%rbx)\n", final_field->offset);
                    } else {
                        code_printf(parser, "    movsd %%xmm0, %d(%%rbx)\n", final_field->offset);
                    }
                } else if (final_field->type == TYPE_CHAR || final_field->type == TYPE_BYTE || final_field->type ==
                           TYPE_BIT) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movb %%al, %d(%%rbx)\n", final_field->offset);
                } else if (final_field->type == TYPE_STRING) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %d(%%rbx)\n", final_field->offset);
                } else {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movl %%eax, %d(%%rbx)\n", final_field->offset);
                }

                expect(parser, TOKEN_SEMICOLON, "Expected ';' after chained field assignment");
                return;
            } else {
                expect(parser, TOKEN_EQUAL, "Expected '=' in field assignment");

                code_comment(parser, "Line %d: %s.%s = value",
                             assign_token.line, name_token.value, field_name_token.value);

                parse_expression(parser);

                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

                if (field->type == TYPE_FLOAT || field->type == TYPE_DOUBLE) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %%xmm0\n");
                    if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %%xmm0, %d(%%rbx)\n", field->offset);
                    } else {
                        code_printf(parser, "    movsd %%xmm0, %d(%%rbx)\n", field->offset);
                    }
                } else if (field->type == TYPE_CHAR || field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movb %%al, %d(%%rbx)\n", field->offset);
                } else if (field->type == TYPE_STRING) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %d(%%rbx)\n", field->offset);
                } else {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movl %%eax, %d(%%rbx)\n", field->offset);
                }

                expect(parser, TOKEN_SEMICOLON, "Expected ';' after field assignment");
                return;
            }
        }
    }

    int is_array_access = 0;
    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        is_array_access = 1;

        if (!var->is_array && var->type != TYPE_STRING) {
            parser_error(parser, "Variable '%s' is not an array or string", name_token.value);
            return;
        }

        if (var->type == TYPE_STRING) {
            code_comment(parser, "Line %d: %s[index] = value (string character assignment)",
                         assign_token.line, name_token.value);
        } else {
            code_comment(parser, "Line %d: %s[index] = value (array assignment)",
                         assign_token.line, name_token.value);
        }

        parse_expression(parser);

        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
        expect(parser, TOKEN_EQUAL, "Expected '=' in array assignment");

        parse_expression(parser);

        code_printf(parser, "    popq %%rcx\n");
        code_printf(parser, "    popq %%rax\n");

        if (var->type == TYPE_STRING) {
            code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
            code_printf(parser, "    movb %%cl, (%%rbx, %%rax, 1)\n");
        } else {
            int element_size = datatype_size(var->type);

            if (var->is_array && var->array_size == 0) {
                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
            } else {
                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
            }

            if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
                if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movq %%rcx, %%xmm0\n");
                    code_printf(parser, "    movss %%xmm0, (%%rbx, %%rax, %d)\n", element_size);
                } else {
                    code_printf(parser, "    movq %%rcx, %%xmm0\n");
                    code_printf(parser, "    movsd %%xmm0, (%%rbx, %%rax, %d)\n", element_size);
                }
            } else {
                if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movb %%cl, (%%rbx, %%rax, %d)\n", element_size);
                } else {
                    code_printf(parser, "    movl %%ecx, (%%rbx, %%rax, %d)\n", element_size);
                }
            }
        }

        expect(parser, TOKEN_SEMICOLON, "Expected ';' after array assignment");
        return;
    }

    TokenType op = peek(parser->tokens).type;

    if (var->type == TYPE_STRING) {
        if (op != TOKEN_EQUAL) {
            parser_error(parser, "Compound assignment not allowed for strings");
            return;
        }

        consume(parser->tokens);

        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            code_comment(parser, "Line %d: %s = \"%s\"",
                         assign_token.line, name_token.value, str_token.value);

            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
        } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token src_name = peek(parser->tokens);
            Token lookahead = peek_ahead(parser->tokens, 1);

            if (lookahead.type == TOKEN_LPAREN) {
                consume(parser->tokens);

                Function *func = find_function(parser, src_name.value);
                if (!func) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_FUNCTION, "Function '%s' not found", src_name.value);
                    return;
                }

                if (func->return_type != TYPE_STRING) {
                    parser_error(parser, "Function '%s' does not return string", src_name.value);
                    return;
                }

                code_comment(parser, "Line %d: %s = %s() (string function)",
                             assign_token.line, name_token.value, src_name.value);

                expect(parser, TOKEN_LPAREN, "Expected '('");

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

                const char **arg_regs_64 = get_arg_registers_64();
                const char **arg_regs_32 = get_arg_registers_32();

                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    DataType param_type = func->param_types[i];

                    if (param_type == TYPE_STRING) {
                        code_printf(parser, "    popq %s\n", arg_regs_64[i]);
                    } else if (param_type == TYPE_FLOAT || param_type == TYPE_DOUBLE) {
                        code_printf(parser, "    popq %%rax\n");
                        if (i == 0) code_printf(parser, "    movq %%rax, %%xmm0\n");
                        else if (i == 1) code_printf(parser, "    movq %%rax, %%xmm1\n");
                        else if (i == 2) code_printf(parser, "    movq %%rax, %%xmm2\n");
                        else if (i == 3) code_printf(parser, "    movq %%rax, %%xmm3\n");
                    } else if (param_type == TYPE_CHAR || param_type == TYPE_BYTE || param_type == TYPE_BIT) {
                        const char **call_arg_regs_8 = get_arg_registers_8();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %s\n", call_arg_regs_8[i]);
                    } else {
                        const char **assign_arg_regs_32 = get_arg_registers_32();
                        int assign_max_reg_args = get_max_reg_args();
                        code_printf(parser, "    popq %%rax\n");
                        if (i < assign_max_reg_args) {
                            code_printf(parser, "    movl %%eax, %s\n", assign_arg_regs_32[i]);
                        }
                    }
                }

                int assign_stack_adjust = get_call_stack_space();
                if (assign_stack_adjust > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", assign_stack_adjust);
                }
                code_printf(parser, "    call %s\n", src_name.value);
                if (assign_stack_adjust > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", assign_stack_adjust);
                }

                code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
            } else {
                consume(parser->tokens);

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
            if (var->is_pointer) {
                code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
            } else {
                code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
            }
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

    if (parser->loop_depth >= MAX_LOOP_DEPTH) {
        parser_error(parser, "Maximum loop nesting depth (%d) exceeded", MAX_LOOP_DEPTH);
        return;
    }
    parser->loop_stack[parser->loop_depth].loop_id = loop_id;
    parser->loop_depth++;

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
            semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
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

    parser->loop_depth--;
}

void parse_break_statement(Parser *parser) {
    Token break_token = peek(parser->tokens);
    consume(parser->tokens);

    if (parser->loop_depth == 0) {
        parser_error(parser, "'break' statement not within a loop");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'break'");
        return;
    }

    int current_loop_id = parser->loop_stack[parser->loop_depth - 1].loop_id;

    code_comment(parser, "Break statement (Line %d)", break_token.line);
    code_printf(parser, "    jmp .L_for_end_%d\n", current_loop_id);

    expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'break'");
}

void parse_continue_statement(Parser *parser) {
    Token continue_token = peek(parser->tokens);
    consume(parser->tokens);

    if (parser->loop_depth == 0) {
        parser_error(parser, "'continue' statement not within a loop");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'continue'");
        return;
    }

    int current_loop_id = parser->loop_stack[parser->loop_depth - 1].loop_id;

    code_comment(parser, "Continue statement (Line %d)", continue_token.line);
    code_printf(parser, "    jmp .L_for_increment_%d\n", current_loop_id);

    expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'continue'");
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
        code_printf(parser, ".L_else_%d:\n", if_id);

        if (check(parser->tokens, TOKEN_KEYWORD_IF)) {
            code_comment(parser, "Else-If (Line %d)", else_token.line);

            parse_if_statement(parser);
        } else {
            code_comment(parser, "Else-Block (Line %d)", else_token.line);

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
        }

        code_comment(parser, "If-End (ID: %d)", if_id);
        code_printf(parser, ".L_endif_%d:\n", if_id);
    } else {
        code_comment(parser, "If-End (No else, ID: %d)", if_id);
        code_printf(parser, ".L_else_%d:\n", if_id);
    }
}

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

    // Clean up @gc variables before returning
    cleanup_scope(parser, parser->current_scope);

    /* Windows ABI: Restore non-volatile registers before return */
    if (parser->target_format == TARGET_COFF) {
        emit_pop(parser, "rsi");
        emit_pop(parser, "rdi");
    }

    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n");
}

void parse_print_statement(Parser *parser) {
    Token print_token = peek(parser->tokens);
    code_comment(parser, "Line %d: print(...)", print_token.line);

    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Expected '(' after 'print'");

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            const char **print_regs = get_arg_registers_64();
            code_printf(parser, "    leaq .LC%d(%%rip), %s\n", str_id, print_regs[0]);
            int print_stack_adjust = get_call_stack_space();
            if (print_stack_adjust > 0) {
                code_printf(parser, "    subq $%d, %%rsp\n", print_stack_adjust);
            }
            code_printf(parser, "    call printf\n");
            if (print_stack_adjust > 0) {
                code_printf(parser, "    addq $%d, %%rsp\n", print_stack_adjust);
            }
        } else if (check(parser->tokens, TOKEN_LPAREN)) {
            consume(parser->tokens);
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')' after expression");

            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_NUMBER)) {
            Token num = consume(parser->tokens);
            code_printf(parser, "    movl $%s, %s\n", num.value, get_arg_reg_32(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
            Token num = consume(parser->tokens);
            int float_id = add_float_literal(parser, num.value);

            code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
            Token ch = consume(parser->tokens);
            int char_value = (unsigned char) ch.value[0];
            code_printf(parser, "    movb $%d, %%dl\n", char_value);
            code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token name = peek(parser->tokens);
            Token lookahead = peek_ahead(parser->tokens, 1);

            if (lookahead.type == TOKEN_LPAREN) {
                consume(parser->tokens);

                Function *func = find_function(parser, name.value);
                if (!func) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_FUNCTION, "Function '%s' not found", name.value);
                    return;
                }

                consume(parser->tokens);

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

                const char **arg_regs = get_arg_registers_64();
                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    code_printf(parser, "    popq %s\n", arg_regs[i]);
                }

                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call %s\n", name.value);
                    generate_stack_restore(parser);
                }

                if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (func->return_type == TYPE_STRING) {
                    code_printf(parser, "    movq %%rax, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                } else {
                    code_printf(parser, "    movl %%eax, %s\n", get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                }
                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call printf\n");
                    generate_stack_restore(parser);
                }
            } else if (lookahead.type == TOKEN_DOT) {
                EnumDefinition *enum_def = find_enum(parser, name.value);
                if (enum_def) {
                    Token dot1 = peek_ahead(parser->tokens, 1);
                    Token value_name = peek_ahead(parser->tokens, 2);
                    Token dot2 = peek_ahead(parser->tokens, 3);
                    Token field_name = peek_ahead(parser->tokens, 4);

                    DataType field_type = TYPE_INT;
                    if (dot1.type == TOKEN_DOT && dot2.type == TOKEN_DOT) {
                        for (int i = 0; i < enum_def->field_count; i++) {
                            if (strcmp(enum_def->fields[i].name, field_name.value) == 0) {
                                field_type = enum_def->fields[i].type;
                                break;
                            }
                        }
                    }

                    parse_expression(parser);

                    if (field_type == TYPE_STRING) {
                        code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field_type == TYPE_FLOAT || field_type == TYPE_DOUBLE) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movq %%rax, %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field_type == TYPE_CHAR) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %%dl\n");
                        code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else {
                        code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    }

                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else {
                    consume(parser->tokens);
                    consume(parser->tokens);

                    Variable *var = find_variable(parser, name.value);
                    if (!var) {
                        semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                        return;
                    }

                    if (var->struct_type[0] == '\0') {
                        parser_error(parser, "Variable '%s' is not a struct", name.value);
                        return;
                    }

                    StructDefinition *struct_def = find_struct(parser, var->struct_type);
                    if (!struct_def) {
                        semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found",
                                       var->struct_type);
                        return;
                    }

                    Token member_name = consume(parser->tokens);
                    Token next_token = peek(parser->tokens);

                    if (next_token.type == TOKEN_LPAREN) {
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
                            parse_expression(parser);
                            arg_count++;

                            if (check(parser->tokens, TOKEN_COMMA)) {
                                consume(parser->tokens);
                            }
                        }

                        expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                        if (arg_count != method->param_count) {
                            parser_error(parser, "Method '%s' expects %d arguments", member_name.value,
                                         method->param_count);
                            return;
                        }

                        const char *arg_regs[] = {"%rdx", "%r8", "%r9"};
                        for (int i = arg_count - 1; i >= 0 && i < 3; i--) {
                            code_printf(parser, "    popq %s\n", arg_regs[i]);
                        }

                        const char **this3_regs_64 = get_arg_registers_64();
                        code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this3_regs_64[0]);

                        {
                            generate_stack_align(parser);
                            code_printf(parser, "    call %s\n", mangled_name);
                            generate_stack_restore(parser);
                        }

                        if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else if (method->return_type == TYPE_STRING) {
                            code_printf(parser, "    movq %%rax, %s\n", get_arg_reg_64(1));
                            code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else {
                            code_printf(parser, "    movl %%eax, %s\n", get_arg_reg_32(1));
                            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                        }
                        {
                            generate_stack_align(parser);
                            code_printf(parser, "    call printf\n");
                            generate_stack_restore(parser);
                        }
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

                        code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

                        if (field->type == TYPE_FLOAT) {
                            code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else if (field->type == TYPE_DOUBLE) {
                            code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else if (field->type == TYPE_CHAR) {
                            code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                            code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                            code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else if (field->type == TYPE_STRING) {
                            code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_reg_64(1));
                            code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                        } else {
                            code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                        }
                        {
                            generate_stack_align(parser);
                            code_printf(parser, "    call printf\n");
                            generate_stack_restore(parser);
                        }
                    }
                }
            } else if (lookahead.type == TOKEN_LBRACKET) {
                consume(parser->tokens);
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
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

                if (var->type == TYPE_INT) {
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %s\n", element_size, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                } else {
                    parser_error(parser, "Unsupported array element type in print");
                    return;
                }

                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call printf\n");
                    generate_stack_restore(parser);
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
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                                if (field->type == TYPE_FLOAT) {
                                    code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_DOUBLE) {
                                    code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_CHAR) {
                                    code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                                    code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_STRING) {
                                    code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else {
                                    code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                                }

                                {
                                    int stack_adj_field = get_call_stack_space();
                                    if (stack_adj_field > 0) {
                                        code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_field);
                                    }
                                    code_printf(parser, "    call printf\n");
                                    if (stack_adj_field > 0) {
                                        code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_field);
                                    }
                                }

                                goto print_next_item;
                            }
                        }
                    }
                }

                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                    return;
                }

                if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else {
                    code_printf(parser, "    movl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                }
            }
        } else {
            parse_expression(parser);

            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                int stack_adj_fallback = get_call_stack_space();
                if (stack_adj_fallback > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_fallback);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_fallback > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_fallback);
                }
            }
        }

    print_next_item:
        if (check(parser->tokens, TOKEN_PLUS)) {
            consume(parser->tokens);
        } else {
            break;
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after print arguments");
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after print");
}

void parse_printline_statement(Parser *parser) {
    Token print_token = peek(parser->tokens);
    code_comment(parser, "Line %d: println(...)", print_token.line);

    consume(parser->tokens);
    expect(parser, TOKEN_LPAREN, "Expected '(' after 'println'");

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            const char **print_regs = get_arg_registers_64();
            code_printf(parser, "    leaq .LC%d(%%rip), %s\n", str_id, print_regs[0]);
            int print_stack_adjust = get_call_stack_space();
            if (print_stack_adjust > 0) {
                code_printf(parser, "    subq $%d, %%rsp\n", print_stack_adjust);
            }
            code_printf(parser, "    call printf\n");
            if (print_stack_adjust > 0) {
                code_printf(parser, "    addq $%d, %%rsp\n", print_stack_adjust);
            }
        } else if (check(parser->tokens, TOKEN_LPAREN)) {
            consume(parser->tokens);
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')' after expression");

            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_NUMBER)) {
            Token num = consume(parser->tokens);
            code_printf(parser, "    movl $%s, %s\n", num.value, get_arg_reg_32(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
            Token num = consume(parser->tokens);
            int float_id = add_float_literal(parser, num.value);

            code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
            Token ch = consume(parser->tokens);
            int char_value = (unsigned char) ch.value[0];
            code_printf(parser, "    movb $%d, %%dl\n", char_value);
            code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                generate_stack_align(parser);
                code_printf(parser, "    call printf\n");
                generate_stack_restore(parser);
            }
        } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
            Token name = peek(parser->tokens);
            Token lookahead = peek_ahead(parser->tokens, 1);

            if (lookahead.type == TOKEN_LPAREN) {
                consume(parser->tokens);

                Function *func = find_function(parser, name.value);
                if (!func) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_FUNCTION, "Function '%s' not found", name.value);
                    return;
                }

                consume(parser->tokens);

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

                const char **arg_regs = get_arg_registers_64();
                for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
                    code_printf(parser, "    popq %s\n", arg_regs[i]);
                }

                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call %s\n", name.value);
                    generate_stack_restore(parser);
                }

                if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (func->return_type == TYPE_STRING) {
                    code_printf(parser, "    movq %%rax, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                } else {
                    code_printf(parser, "    movl %%eax, %s\n", get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                }
                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call printf\n");
                    generate_stack_restore(parser);
                }
            } else if (lookahead.type == TOKEN_DOT) {
                consume(parser->tokens);
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                    return;
                }

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
                Token next_token = peek(parser->tokens);

                if (next_token.type == TOKEN_LPAREN) {
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
                        parse_expression(parser);
                        arg_count++;

                        if (check(parser->tokens, TOKEN_COMMA)) {
                            consume(parser->tokens);
                        }
                    }

                    expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                    if (arg_count != method->param_count) {
                        parser_error(parser, "Method '%s' expects %d arguments", member_name.value,
                                     method->param_count);
                        return;
                    }

                    const char *arg_regs[] = {"%rdx", "%r8", "%r9"};
                    for (int i = arg_count - 1; i >= 0 && i < 3; i--) {
                        code_printf(parser, "    popq %s\n", arg_regs[i]);
                    }

                    const char **this3_regs_64 = get_arg_registers_64();
                    code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this3_regs_64[0]);

                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call %s\n", mangled_name);
                        generate_stack_restore(parser);
                    }

                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (method->return_type == TYPE_STRING) {
                        code_printf(parser, "    movq %%rax, %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else {
                        code_printf(parser, "    movl %%eax, %s\n", get_arg_reg_32(1));
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    }
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
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

                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);

                    if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field->type == TYPE_DOUBLE) {
                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field->type == TYPE_CHAR) {
                        code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                        code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                        code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else if (field->type == TYPE_STRING) {
                        code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_reg_64(1));
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                    } else {
                        code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    }
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                }
            } else if (lookahead.type == TOKEN_LBRACKET) {
                consume(parser->tokens);
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
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

                if (var->type == TYPE_INT) {
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %s\n", element_size, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                } else {
                    parser_error(parser, "Unsupported array element type in print");
                    return;
                }

                {
                    generate_stack_align(parser);
                    code_printf(parser, "    call printf\n");
                    generate_stack_restore(parser);
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
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);

                                if (field->type == TYPE_FLOAT) {
                                    code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_DOUBLE) {
                                    code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_CHAR) {
                                    code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                                    code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else if (field->type == TYPE_STRING) {
                                    code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_reg_64(1));
                                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                                } else {
                                    code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_reg_32(1));
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                                }

                                {
                                    int stack_adj_field = get_call_stack_space();
                                    if (stack_adj_field > 0) {
                                        code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_field);
                                    }
                                    code_printf(parser, "    call printf\n");
                                    if (stack_adj_field > 0) {
                                        code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_field);
                                    }
                                }

                                goto print_next_item;
                            }
                        }
                    }
                }

                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                    return;
                }

                if (var->type == TYPE_FLOAT) {
                    code_printf(parser, "    movss %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, get_arg_reg_64(1));
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                } else {
                    code_printf(parser, "    movl %d(%%rbp), %s\n", var->offset, get_arg_reg_32(1));
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
                    {
                        generate_stack_align(parser);
                        code_printf(parser, "    call printf\n");
                        generate_stack_restore(parser);
                    }
                }
            }
        } else {
            parse_expression(parser);

            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_reg_64(0));
            {
                int stack_adj_fallback = get_call_stack_space();
                if (stack_adj_fallback > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_fallback);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_fallback > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_fallback);
                }
            }
        }

    print_next_item:
        if (check(parser->tokens, TOKEN_PLUS)) {
            consume(parser->tokens);
        } else {
            break;
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after print arguments");

    code_printf(parser, "    leaq .LC_newline(%%rip), %s\n", get_arg_reg_64(0));
    {
        int stack_adj_nl = get_call_stack_space();
        if (stack_adj_nl > 0) {
            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_nl);
        }
        code_printf(parser, "    call printf\n");
        if (stack_adj_nl > 0) {
            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_nl);
        }
    }
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after print");
}

void parse_function_call_statement(Parser *parser) {
    Token call_token = peek(parser->tokens);
    Token name = consume(parser->tokens);

    if (check(parser->tokens, TOKEN_DOT)) {
        consume(parser->tokens);

        Variable *var = find_variable(parser, name.value);

        if (!var) {
            StructDefinition *struct_def = find_struct(parser, name.value);
            if (struct_def) {
                Token method_name = consume(parser->tokens);

                char mangled_name[MAX_TOKEN * 2];
                snprintf(mangled_name, sizeof(mangled_name), "%s_%s", name.value, method_name.value);

                Function *method = find_function(parser, mangled_name);
                if (!method) {
                    parser_error(parser, "Static method '%s' not found in struct '%s'", method_name.value, name.value);
                    return;
                }

                if (!method->is_static) {
                    parser_error(parser, "Method '%s' is not static. Use an instance to call it.", method_name.value);
                    return;
                }

                code_comment(parser, "Line %d: %s.%s(...) (static method call)", call_token.line, name.value,
                             method_name.value);

                expect(parser, TOKEN_LPAREN, "Expected '(' after method name");

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
                expect(parser, TOKEN_SEMICOLON, "Expected ';' after static method call");

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
                        const char **static_method_arg_regs_8 = get_arg_registers_8();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %s\n", static_method_arg_regs_8[i]);
                    } else {
                        const char **static_method_arg_regs_32 = get_arg_registers_32();
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movl %%eax, %s\n", static_method_arg_regs_32[i]);
                    }
                }

                {
                    int stack_adj_static = get_call_stack_space();
                    if (stack_adj_static > 0) {
                        code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_static);
                    }
                    code_printf(parser, "    call %s\n", mangled_name);
                    if (stack_adj_static > 0) {
                        code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_static);
                    }
                }

                return;
            }

            parser_error(parser, "Variable or struct type '%s' not found", name.value);
            return;
        }

        if (var->struct_type[0] == '\0') {
            parser_error(parser, "Variable '%s' is not a struct", name.value);
            return;
        }

        StructDefinition *struct_def = find_struct(parser, var->struct_type);
        if (!struct_def) {
            semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found", var->struct_type);
            return;
        }

        Token method_name = consume(parser->tokens);

        char mangled_name[MAX_TOKEN * 2];
        snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, method_name.value);

        Function *method = find_function(parser, mangled_name);
        if (!method) {
            parser_error(parser, "Method '%s' not found in struct '%s'", method_name.value, var->struct_type);
            return;
        }

        code_comment(parser, "Line %d: %s.%s(...) (method call)", call_token.line, name.value, method_name.value);

        expect(parser, TOKEN_LPAREN, "Expected '(' after method name");

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
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after method call");

        if (arg_count != method->param_count) {
            parser_error(parser, "Method '%s' expects %d arguments (excluding implicit 'this')",
                         method_name.value, method->param_count);
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
                const char **method_arg_regs_8 = get_arg_registers_8();
                code_printf(parser, "    popq %%rax\n");
                code_printf(parser, "    movb %%al, %s\n", method_arg_regs_8[reg_idx]);
            } else {
                const char **method_arg_regs_32 = get_arg_registers_32();
                code_printf(parser, "    popq %%rax\n");
                code_printf(parser, "    movl %%eax, %s\n", method_arg_regs_32[reg_idx]);
            }
        }

        const char **this_regs_64 = get_arg_registers_64();
        code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this_regs_64[0]);

        {
            generate_stack_align(parser);
            code_printf(parser, "    call %s\n", mangled_name);
            generate_stack_restore(parser);
        }

        return;
    }

    if (is_syscall_io_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '('");

        if (strcmp(name.value, "io_int_to_str") == 0) {
            code_comment(parser, "Line %d: io_int_to_str(value, buffer, buffer_size)", call_token.line);

            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    popq %%rsi\n");
            generate_int_to_str_code(parser, "%rsi", "%rdi");
            return;
        } else if (strcmp(name.value, "sys_write") == 0) {
            code_comment(parser, "Line %d: sys_write(fd, buffer, count)", call_token.line);

            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rsi\n");
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $1, %%rax\n");
            code_printf(parser, "    syscall\n");
            return;
        } else if (strcmp(name.value, "sys_read") == 0) {
            code_comment(parser, "Line %d: sys_read(fd, buffer, count)", call_token.line);

            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rsi\n");
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $0, %%rax\n");
            code_printf(parser, "    syscall\n");
            return;
        } else if (strcmp(name.value, "sys_open") == 0) {
            code_comment(parser, "Line %d: sys_open(pathname, flags, mode)", call_token.line);

            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_printf(parser, "    popq %%rdx\n");
            code_printf(parser, "    popq %%rsi\n");
            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $2, %%rax\n");
            code_printf(parser, "    syscall\n");
            return;
        } else if (strcmp(name.value, "sys_close") == 0) {
            code_comment(parser, "Line %d: sys_close(fd)", call_token.line);

            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_printf(parser, "    popq %%rdi\n");
            code_printf(parser, "    movq $3, %%rax\n");
            code_printf(parser, "    syscall\n");
            return;
        }
    }

    if (is_builtin_string_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

        if (strcmp(name.value, "strlen") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: strlen() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call strlen\n");
                generate_stack_restore(parser);
            }
            return;
        } else if (strcmp(name.value, "strcmp") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: strcmp() (statement)", call_token.line);
            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call strcmp\n");
                generate_stack_restore(parser);
            }
            return;
        } else if (strcmp(name.value, "strcpy") == 0 || strcmp(name.value, "strcat") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: %s() (statement)", call_token.line, name.value);
            code_printf(parser, "    popq %s\n", get_arg_reg_64(1));
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call %s\n", name.value);
                generate_stack_restore(parser);
            }
            return;
        } else if (strcmp(name.value, "strdup") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: strdup() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call strdup\n");
                generate_stack_restore(parser);
            }
            return;
        }
    }

    if (is_builtin_memory_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

        if (strcmp(name.value, "malloc") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: malloc() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call malloc\n");
                generate_stack_restore(parser);
            }
            return;
        } else if (strcmp(name.value, "free") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");

            code_comment(parser, "Line %d: free() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                generate_stack_align(parser);
                code_printf(parser, "    call free\n");
                generate_stack_restore(parser);
            }
            return;
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

        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

        int arg_count = 0;
        while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
            parse_expression(parser);
            arg_count++;
            if (check(parser->tokens, TOKEN_COMMA)) {
                consume(parser->tokens);
            }
        }

        expect(parser, TOKEN_RPAREN, "Expected ')' after function arguments");

        code_printf(parser, "    xorq %%rax, %%rax  # Error recovery: undefined function\n");
        code_printf(parser, "    pushq %%rax\n");
        return;
    }

    if (is_method_call) {
        code_comment(parser, "Line %d: %s(...) (method-to-method call)", call_token.line, name.value);
    } else {
        code_comment(parser, "Line %d: %s(...)", call_token.line, name.value);
    }

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

    int arg_count = 0;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
            Token str_token = consume(parser->tokens);
            int str_id = add_string_literal(parser, str_token.value);

            char escaped_str[512];
            escape_string_for_comment(str_token.value, escaped_str, sizeof(escaped_str));
            code_comment(parser, "String argument: \"%s\"", escaped_str);
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

    const char **arg_regs = get_arg_registers_64();

    if (is_method_call) {
        int max_reg_args = get_max_reg_args();
        for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
            int reg_idx = i + 1;
            code_printf(parser, "    popq %s\n", arg_regs[reg_idx]);
        }

        Variable *this_var = find_variable(parser, "this");
        if (this_var) {
            code_printf(parser, "    movq %d(%%rbp), %s\n", this_var->offset, arg_regs[0]);
        }

        {
            generate_stack_align(parser);
            code_printf(parser, "    call %s\n", method_mangled_name);
            generate_stack_restore(parser);
        }
    } else {
        for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
            code_printf(parser, "    popq %s\n", arg_regs[i]);
        }

        {
            generate_stack_align(parser);
            code_printf(parser, "    call %s\n", name.value);
            generate_stack_restore(parser);
        }
    }

    if (func->return_type != TYPE_VOID) {
        code_printf(parser, "    pushq %%rax\n");
    }
}
