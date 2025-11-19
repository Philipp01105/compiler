#include "parser.h"
#include "parser_internal.h"
#include "errorHandler.h"
#include "instruction_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * parse_program - Parse entire program
 * @parser: Parser state
 * @source_file: Source file path
 *
 * Top-level parser entry point. Parses imports, structs, enums,
 * and functions. Returns 1 on success, 0 on error.
 */
int parse_program(Parser *parser, const char *source_file) {
    parser_load_source(parser, source_file);

    if (parser->debug_mode) {
        printf("\n================================================================\n");
        printf("           PHASE 2: SYNTAX ANALYSIS (PARSER)\n");
        printf("================================================================\n\n");
    }

    while (!is_at_end(parser->tokens)) {
        if (check(parser->tokens, TOKEN_HASH)) {
            parse_import(parser, source_file);
        } else if (check(parser->tokens, TOKEN_KEYWORD_STRUCT)) {
            parse_struct(parser);
        } else if (check(parser->tokens, TOKEN_KEYWORD_ENUM)) {
            parse_enum(parser);
        } else if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            parse_function(parser);
        } else {
            parser_error(parser, "Only imports, structs, enums and functions allowed at top level");
            consume(parser->tokens);
        }
    }

    if (parser->has_error) {
        return 0;
    }

    Function *main_func = find_function(parser, "main");
    if (!main_func) {
        if (global_error_handler) {
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_NO_MAIN_FUNCTION, source_file,
                         "No main() function found");
        } else {
            fprintf(stderr, "Error: No main() function found\n");
        }
        return 0;
    }

    if (parser->debug_mode) {
        printf("[+] Parsing successful\n");
        printf("    - Structs: %d\n", parser->struct_count);
        printf("    - Functions: %d\n", parser->function_count);
        printf("    - String literals: %d\n", parser->string_literal_count);
        printf("    - Float literals: %d\n", parser->float_literal_count);
    }

    return 1;
}

void parse_struct(Parser *parser) {
    Token struct_token = peek(parser->tokens);
    consume(parser->tokens);

    Token name_token = consume(parser->tokens);
    char struct_name[MAX_TOKEN];
    strcpy(struct_name, name_token.value);

    if (parser->struct_count >= 50) {
        parser_error(parser, "Too many structs (max 50)");
        return;
    }

    if (find_struct(parser, struct_name) != NULL) {
        parser_error_code(parser, ERR_PARSE_DUPLICATE_DEFINITION, "Struct '%s' already defined", struct_name);
        return;
    }

    StructDefinition *struct_def = &parser->structs[parser->struct_count];
    strcpy(struct_def->name, struct_name);
    struct_def->field_count = 0;
    struct_def->method_count = 0;
    struct_def->total_size = 0;
    parser->struct_count++;

    expect(parser, TOKEN_LBRACE, "Expected '{' after struct name");

    code_comment(parser, "========================================");
    code_comment(parser, "Struct: %s (Line %d)", struct_name, struct_token.line);
    code_comment(parser, "========================================");

    int current_offset = 0;

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        int is_static = 0;
        if (check(parser->tokens, TOKEN_KEYWORD_STATIC)) {
            is_static = 1;
            consume(parser->tokens);
        }

        if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            consume(parser->tokens);

            Token method_name_token = consume(parser->tokens);
            char method_name[MAX_TOKEN];
            strcpy(method_name, method_name_token.value);

            if (parser->function_count >= MAX_FUNCTIONS) {
                parser_error(parser, "Too many functions (max %d)", MAX_FUNCTIONS);
                return;
            }

            char mangled_name[MAX_TOKEN * 2];
            snprintf(mangled_name, sizeof(mangled_name), "%s_%s", struct_name, method_name);

            Function *func = &parser->functions[parser->function_count];
            strcpy(func->name, mangled_name);
            strcpy(func->struct_name, struct_name);
            func->param_count = 0;
            func->is_static = is_static;

            if (struct_def->method_count >= MAX_FUNCTIONS) {
                parser_error(parser, "Too many methods in struct");
                return;
            }
            struct_def->methods[struct_def->method_count] = parser->function_count;
            struct_def->method_count++;
            parser->function_count++;

            expect(parser, TOKEN_LPAREN, "Expected '(' after method name");

            int saved_var_count = parser->var_count;
            int param_offset = 8;

            Variable *this_var = NULL;
            if (!is_static) {
                if (parser->var_count >= MAX_VARS) {
                    parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
                    return;
                }

                this_var = &parser->vars[parser->var_count];
                strcpy(this_var->name, "this");
                strcpy(this_var->struct_type, struct_name);
                this_var->type = TYPE_INT;
                this_var->size = 8;
                this_var->offset = -param_offset;
                this_var->is_array = 0;
                this_var->array_size = 0;
                this_var->scope = 1;
                parser->var_count++;
                param_offset += 8;
            }

            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                Token param_token = consume(parser->tokens);

                if (func->param_count >= 9) {
                    parser_error(parser, "Too many parameters (max 8 explicit parameters plus implicit 'this')");
                    return;
                }

                strcpy(func->params[func->param_count], param_token.value);

                int is_array_param = 0;
                if (check(parser->tokens, TOKEN_LBRACKET)) {
                    consume(parser->tokens);
                    is_array_param = 1;
                    expect(parser, TOKEN_RBRACKET, "Expected ']' in array parameter");
                }

                expect(parser, TOKEN_COLON, "Expected ':' after parameter");

                Token type_token = consume(parser->tokens);
                DataType param_type = token_to_datatype(type_token.type);

                if (param_type == TYPE_UNKNOWN) {
                    parser_error(parser, "Unknown parameter type");
                    return;
                }

                func->param_types[func->param_count] = param_type;
                func->param_is_array[func->param_count] = is_array_param;

                if (parser->var_count >= MAX_VARS) {
                    parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
                    return;
                }

                Variable *var = &parser->vars[parser->var_count];
                strcpy(var->name, param_token.value);
                var->type = param_type;
                var->is_array = is_array_param;
                var->array_size = 0;
                var->struct_type[0] = '\0';

                if (is_array_param) {
                    var->size = 8;
                } else {
                    var->size = datatype_size(param_type);
                }

                var->offset = -param_offset;
                param_offset += var->size;

                if (var->size == 8) {
                    param_offset = ((param_offset + 7) / 8) * 8;
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
            func->return_is_array = 0;

            if (check(parser->tokens, TOKEN_LBRACKET)) {
                consume(parser->tokens);
                func->return_is_array = 1;
                expect(parser, TOKEN_RBRACKET, "Expected ']' after array return type");
            }

            code_comment(parser, "========================================");
            code_comment(parser, "Method: %s::%s (Line %d)", struct_name, method_name, method_name_token.line);
            code_comment(parser, "Mangled name: %s", mangled_name);
            code_comment(parser, "Parameters: %d (+ implicit this), Return: %s",
                         func->param_count,
                         datatype_to_string(func->return_type));
            code_comment(parser, "========================================");

            /* Add COFF function definition block for Windows */
            if (parser->target_format == TARGET_COFF) {
                code_printf(parser, "    .def    %s; .scl    2; .type   32; .endef\n", mangled_name);
            }

            code_printf(parser, ".globl %s\n", mangled_name);
            code_printf(parser, "%s:\n", mangled_name);

            code_comment(parser, "Function prologue");
            emit_push(parser, "rbp");
            emit_mov_reg_reg(parser, "rbp", "rsp");
            emit_sub_reg_imm(parser, "rsp", 8192);

            /* Windows ABI: Save non-volatile registers RDI and RSI */
            if (parser->target_format == TARGET_COFF) {
                code_comment(parser, "Save Windows non-volatile registers");
                emit_push(parser, "rdi");
                emit_push(parser, "rsi");
            }

            const char **param_regs_64 = get_arg_registers_64();
            const char **param_regs_32 = get_arg_registers_32();
            const char *param_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

            if (!is_static) {
                code_comment(parser, "Save implicit 'this' pointer and parameters to stack");

                Variable *this_param = &parser->vars[saved_var_count];
                code_printf(parser, "    movq %s, %d(%%rbp)    # Save 'this' pointer\n",
                            param_regs_64[0], this_param->offset);
            } else {
                code_comment(parser, "Save parameters to stack (static method - no 'this')");
            }

            int max_reg_args = get_max_reg_args();
            int param_start_idx = is_static ? 0 : 1;
            for (int i = 0; i < func->param_count && i + (is_static ? 0 : 1) < max_reg_args; i++) {
                Variable *var = &parser->vars[saved_var_count + param_start_idx + i];
                int reg_idx = is_static ? i : (i + 1);

                if (var->is_array) {
                    code_printf(parser, "    movq %s, %d(%%rbp)\n", param_regs_64[reg_idx], var->offset);
                } else if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
                    if (var->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %s, %d(%%rbp)\n", param_regs_float[reg_idx], var->offset);
                    } else {
                        code_printf(parser, "    movsd %s, %d(%%rbp)\n", param_regs_float[reg_idx], var->offset);
                    }
                } else if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    const char **method_param_regs_8 = get_arg_registers_8();
                    code_printf(parser, "    movb %s, %d(%%rbp)\n", method_param_regs_8[reg_idx], var->offset);
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %s, %d(%%rbp)\n", param_regs_64[reg_idx], var->offset);
                } else {
                    code_printf(parser, "    movl %s, %d(%%rbp)\n", param_regs_32[reg_idx], var->offset);
                }
            }

            parser->current_scope = 1;
            parser->scope_depth = 1;
            strcpy(parser->current_struct_context, struct_name);

            parse_function_body(parser);

            cleanup_scope(parser, parser->current_scope);
            parser->current_scope = 0;
            parser->scope_depth = 0;
            parser->current_struct_context[0] = '\0';
            parser->var_count = saved_var_count;

            if (func->return_type == TYPE_VOID) {
                code_comment(parser, "Function epilogue (void return)");

                /* Windows ABI: Restore non-volatile registers */
                if (parser->target_format == TARGET_COFF) {
                    emit_pop(parser, "rsi");
                    emit_pop(parser, "rdi");
                }

                code_printf(parser, "    leave\n");
                code_printf(parser, "    ret\n");
            }

            code_printf(parser, "\n");
        } else {
            Token first_token = consume(parser->tokens);

            if (first_token.type != TOKEN_KEYWORD_VAR) {
                parser_error(parser, "Struct fields must use 'var name:type' syntax");
                return;
            }

            DataType field_type = TYPE_UNKNOWN;
            char field_name[MAX_TOKEN];
            int is_array = 0;
            int array_size = 0;
            char field_struct_type[MAX_TOKEN] = "";

            Token name_token = consume(parser->tokens);
            strcpy(field_name, name_token.value);

            expect(parser, TOKEN_COLON, "Expected ':' after field name");

            Token type_token = consume(parser->tokens);
            field_type = token_to_datatype(type_token.type);

            if (field_type == TYPE_UNKNOWN) {
                StructDefinition *field_struct_def = find_struct(parser, type_token.value);
                if (field_struct_def) {
                    field_type = TYPE_INT;
                    strcpy(field_struct_type, type_token.value);
                } else {
                    parser_error(parser, "Unknown field type '%s'", type_token.value);
                    return;
                }
            }

            if (check(parser->tokens, TOKEN_LBRACKET)) {
                consume(parser->tokens);
                is_array = 1;

                if (check(parser->tokens, TOKEN_NUMBER)) {
                    Token size_token = consume(parser->tokens);
                    array_size = atoi(size_token.value);
                } else {
                    parser_error(parser, "Expected array size");
                    return;
                }

                expect(parser, TOKEN_RBRACKET, "Expected ']' after array size");
            }

            expect(parser, TOKEN_SEMICOLON, "Expected ';' after field declaration");

            if (struct_def->field_count >= 50) {
                parser_error(parser, "Too many fields in struct (max 50)");
                return;
            }

            StructField *field = &struct_def->fields[struct_def->field_count];
            strcpy(field->name, field_name);
            field->type = field_type;
            field->is_array = is_array;
            field->array_size = array_size;
            strcpy(field->struct_type, field_struct_type);

            int element_size;
            if (field_struct_type[0] != '\0') {
                StructDefinition *nested_struct = find_struct(parser, field_struct_type);
                if (nested_struct) {
                    element_size = nested_struct->total_size;
                } else {
                    element_size = datatype_size(field_type);
                }
            } else {
                element_size = datatype_size(field_type);
            }

            if (is_array) {
                field->size = element_size * array_size;
            } else {
                field->size = element_size;
            }

            field->offset = current_offset;
            struct_def->field_count++;

            current_offset += field->size;

            int alignment = (field->size >= 8) ? 8 : 4;
            if (current_offset % alignment != 0) {
                current_offset = ((current_offset + alignment - 1) / alignment) * alignment;
            }
        }
    }

    expect(parser, TOKEN_RBRACE, "Expected '}' at end of struct");

    struct_def->total_size = current_offset;
    if (struct_def->total_size % 8 != 0) {
        struct_def->total_size = ((struct_def->total_size + 7) / 8) * 8;
    }

    code_comment(parser, "End of struct %s (size: %d bytes, fields: %d, methods: %d)",
                 struct_name, struct_def->total_size, struct_def->field_count, struct_def->method_count);
    code_printf(parser, "\n");
}

void parse_enum(Parser *parser) {
    Token enum_token = peek(parser->tokens);
    consume(parser->tokens);

    Token name_token = consume(parser->tokens);
    char enum_name[MAX_TOKEN];
    strcpy(enum_name, name_token.value);

    if (parser->enum_count >= 50) {
        parser_error(parser, "Too many enums (max 50)");
        return;
    }

    if (find_enum(parser, enum_name) != NULL) {
        parser_error_code(parser, ERR_PARSE_DUPLICATE_DEFINITION, "Enum '%s' already defined", enum_name);
        return;
    }

    EnumDefinition *enum_def = &parser->enums[parser->enum_count];
    strcpy(enum_def->name, enum_name);
    enum_def->field_count = 0;
    enum_def->value_count = 0;
    parser->enum_count++;

    expect(parser, TOKEN_LPAREN, "Expected '(' after enum name");

    int current_offset = 0;
    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token field_name_token = consume(parser->tokens);

        if (enum_def->field_count >= 50) {
            parser_error(parser, "Too many fields in enum (max 50)");
            return;
        }

        StructField *field = &enum_def->fields[enum_def->field_count];
        strcpy(field->name, field_name_token.value);

        expect(parser, TOKEN_COLON, "Expected ':' after field name");

        Token type_token = consume(parser->tokens);
        DataType field_type = token_to_datatype(type_token.type);

        if (field_type == TYPE_UNKNOWN) {
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                field_type = TYPE_INT;
                strcpy(field->struct_type, type_token.value);
            } else {
                parser_error(parser, "Unknown field type '%s'", type_token.value);
                return;
            }
        }

        field->type = field_type;
        field->is_array = 0;
        field->array_size = 0;
        field->offset = current_offset;

        field->size = 8;

        current_offset += field->size;
        enum_def->field_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after enum fields");
    expect(parser, TOKEN_LBRACE, "Expected '{' after enum declaration");

    if (parser->struct_count >= 50) {
        parser_error(parser, "Too many structs (max 50)");
        return;
    }

    StructDefinition *backing_struct = &parser->structs[parser->struct_count];
    strcpy(backing_struct->name, enum_name);
    backing_struct->field_count = enum_def->field_count;
    backing_struct->method_count = 0;
    backing_struct->total_size = current_offset;

    for (int i = 0; i < enum_def->field_count; i++) {
        backing_struct->fields[i] = enum_def->fields[i];
    }

    if (backing_struct->total_size % 8 != 0) {
        backing_struct->total_size = ((backing_struct->total_size + 7) / 8) * 8;
    }

    enum_def->struct_index = parser->struct_count;
    parser->struct_count++;

    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        Token value_name_token = consume(parser->tokens);

        if (enum_def->value_count >= 50) {
            parser_error(parser, "Too many enum values (max 50)");
            return;
        }

        EnumValue *enum_value = &enum_def->values[enum_def->value_count];
        strcpy(enum_value->name, value_name_token.value);
        enum_value->field_count = 0;

        expect(parser, TOKEN_LPAREN, "Expected '(' after enum value name");

        while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
            Token value_token = peek(parser->tokens);

            if (enum_value->field_count >= 50) {
                parser_error(parser, "Too many fields in enum value");
                return;
            }

            strcpy(enum_value->values[enum_value->field_count], value_token.value);
            enum_value->field_count++;

            consume(parser->tokens);

            if (check(parser->tokens, TOKEN_COMMA)) {
                consume(parser->tokens);
            }
        }

        expect(parser, TOKEN_RPAREN, "Expected ')' after enum value arguments");

        if (enum_value->field_count != enum_def->field_count) {
            parser_error(parser, "Enum value '%s' has %d fields, expected %d",
                         enum_value->name, enum_value->field_count, enum_def->field_count);
            return;
        }

        enum_def->value_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }

    expect(parser, TOKEN_RBRACE, "Expected '}' after enum values");

    for (int i = 0; i < enum_def->value_count; i++) {
        EnumValue *enum_value = &enum_def->values[i];
        char global_name[MAX_TOKEN * 2];
        snprintf(global_name, sizeof(global_name), "%s_%s", enum_name, enum_value->name);

        if (parser->var_count >= MAX_VARS) {
            parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
            return;
        }

        Variable *var = &parser->vars[parser->var_count];
        strcpy(var->name, global_name);
        strcpy(var->struct_type, enum_name);
        var->type = TYPE_INT;
        var->size = backing_struct->total_size;
        var->offset = 0;
        var->is_array = 0;
        var->array_size = 0;
        var->scope = 0;
        parser->var_count++;

        data_printf(parser, "    .data\n");
        data_printf(parser, "    .align 8\n");
        data_printf(parser, "%s:\n", global_name);

        for (int j = 0; j < enum_def->field_count; j++) {
            StructField *field = &enum_def->fields[j];
            const char *value_str = enum_value->values[j];

            if (field->type == TYPE_INT) {
                data_printf(parser, "    .quad %s\n", value_str);
            } else if (field->type == TYPE_STRING) {
                int str_id = add_string_literal(parser, value_str);
                data_printf(parser, "    .quad .LC%d\n", str_id);
            } else if (field->type == TYPE_FLOAT || field->type == TYPE_DOUBLE) {
                int float_id = add_float_literal(parser, value_str);
                data_printf(parser, "    .quad .LC_float_%d\n", float_id);
            } else {
                data_printf(parser, "    .quad %s\n", value_str);
            }
        }

        data_printf(parser, "\n");
    }

    code_comment(parser, "Enum %s defined with %d values", enum_name, enum_def->value_count);
    data_printf(parser, "\n");
}

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
    int param_offset = 8;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token param_token = consume(parser->tokens);

        if (func->param_count >= 10) {
            parser_error(parser, "Too many parameters (max 10)");
            return;
        }

        strcpy(func->params[func->param_count], param_token.value);

        int is_array_param = 0;
        if (check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);
            is_array_param = 1;
            expect(parser, TOKEN_RBRACKET, "Expected ']' in array parameter");
        }

        expect(parser, TOKEN_COLON, "Expected ':' after parameter");

        int is_pointer_param = 0;
        if (check(parser->tokens, TOKEN_STAR)) {
            consume(parser->tokens);
            is_pointer_param = 1;
        }

        Token type_token = consume(parser->tokens);
        DataType param_type = token_to_datatype(type_token.type);

        if (param_type == TYPE_UNKNOWN) {
            parser_error(parser, "Unknown parameter type");
            return;
        }

        func->param_types[func->param_count] = param_type;
        func->param_is_array[func->param_count] = is_array_param;
        func->param_is_pointer[func->param_count] = is_pointer_param;

        if (parser->var_count >= MAX_VARS) {
            parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
            return;
        }

        Variable *var = &parser->vars[parser->var_count];
        strcpy(var->name, param_token.value);
        var->type = param_type;
        var->is_array = is_array_param;
        var->is_pointer = is_pointer_param;
        var->array_size = 0;

        if (is_array_param || is_pointer_param) {
            var->size = 8;
        } else {
            var->size = datatype_size(param_type);
        }

        var->offset = -param_offset;
        param_offset += var->size;

        if (var->size == 8) {
            param_offset = ((param_offset + 7) / 8) * 8;
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

    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        func->return_is_array = 1;
        expect(parser, TOKEN_RBRACKET, "Expected ']' after array return type");
    } else {
        func->return_is_array = 0;
    }

    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        func->return_is_array = 1;
        expect(parser, TOKEN_RBRACKET, "Expected ']' after array return type");
    } else {
        func->return_is_array = 0;
    }

    code_comment(parser, "========================================");
    code_comment(parser, "Function: %s (Line %d)", func_name, func_token.line);
    code_comment(parser, "Parameters: %d, Return: %s",
                 func->param_count,
                 datatype_to_string(func->return_type));
    code_comment(parser, "========================================");

    /* Add COFF function definition block for Windows */
    if (parser->target_format == TARGET_COFF) {
        code_printf(parser, "    .def    %s; .scl    2; .type   32; .endef\n",
                    strcmp(func_name, "main") == 0 ? "main" : func_name);
    }

    if (strcmp(func_name, "main") == 0) {
        code_printf(parser, ".globl main\n");
        code_printf(parser, "main:\n");
    } else {
        code_printf(parser, ".globl %s\n", func_name);
        code_printf(parser, "%s:\n", func_name);
    }

    code_comment(parser, "Function prologue");
    emit_push(parser, "rbp");
    emit_mov_reg_reg(parser, "rbp", "rsp");
    emit_sub_reg_imm(parser, "rsp", 8192);

    /* Windows ABI: Save non-volatile registers RDI and RSI */
    if (parser->target_format == TARGET_COFF) {
        code_comment(parser, "Save Windows non-volatile registers");
        emit_push(parser, "rdi");
        emit_push(parser, "rsi");
    }

    if (func->param_count > 0) {
        code_comment(parser, "Save parameters to stack");
    }

    const char **param_regs_64 = get_arg_registers_64();
    const char **param_regs_32 = get_arg_registers_32();
    const char *param_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

    for (int i = 0; i < func->param_count && i < 4; i++) {
        Variable *var = &parser->vars[saved_var_count + i];

        if (var->is_array || var->is_pointer) {
            code_printf(parser, "    movq %s, %d(%%rbp)\n", param_regs_64[i], var->offset);
        } else if (var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE) {
            if (var->type == TYPE_FLOAT) {
                code_printf(parser, "    movss %s, %d(%%rbp)\n", param_regs_float[i], var->offset);
            } else {
                code_printf(parser, "    movsd %s, %d(%%rbp)\n", param_regs_float[i], var->offset);
            }
        } else if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
            const char **param_regs_8 = get_arg_registers_8();
            code_printf(parser, "    movb %s, %d(%%rbp)\n", param_regs_8[i], var->offset);
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
        code_printf(parser, "    xorl %%eax, %%eax\n"); // Return 0 for void functions

        /* Windows ABI: Restore non-volatile registers */
        if (parser->target_format == TARGET_COFF) {
            emit_pop(parser, "rsi");
            emit_pop(parser, "rdi");
        }

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
