#include "parser.h"
#include "parser_internal.h"
#include "errorHandler.h"
#include "instruction_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

static int ensure_param_capacity(Parser *parser, Function *func, int needed) {
    if (needed <= func->param_capacity) return 1;
    int capacity = func->param_capacity > 0 ? func->param_capacity : 8;
    while (capacity < needed) {
        if (capacity > INT_MAX / 2) {
            parser_error(parser, "Function has too many parameters");
            return 0;
        }
        capacity *= 2;
    }

    char (*params)[MAX_TOKEN] = realloc(func->params, sizeof(*params) * (size_t) capacity);
    if (!params) { parser_error(parser, "Out of memory while storing function parameters"); return 0; }
    func->params = params;
    DataType *types = realloc(func->param_types, sizeof(*types) * (size_t) capacity);
    if (!types) { parser_error(parser, "Out of memory while storing parameter types"); return 0; }
    func->param_types = types;
    int *arrays = realloc(func->param_is_array, sizeof(*arrays) * (size_t) capacity);
    if (!arrays) { parser_error(parser, "Out of memory while storing array parameters"); return 0; }
    func->param_is_array = arrays;
    int *pointers = realloc(func->param_is_pointer, sizeof(*pointers) * (size_t) capacity);
    if (!pointers) { parser_error(parser, "Out of memory while storing pointer parameters"); return 0; }
    func->param_is_pointer = pointers;
    func->param_capacity = capacity;
    return 1;
}

static void save_function_parameters(Parser *parser, Function *func, int first_var) {
    const char **regs64 = get_arg_registers_64();
    const char **regs32 = get_arg_registers_32();
    const char **regs8 = get_arg_registers_8();
    int integer_index = 0;
    int float_index = 0;
    int stack_index = 0;

    for (int i = 0; i < func->param_count; i++) {
        Variable *var = &parser->vars[first_var + i];
        int is_float = var->type == TYPE_FLOAT || var->type == TYPE_DOUBLE;
        int reg_index;
        int in_register;
        if (parser->target_format == TARGET_COFF) {
            reg_index = i;
            in_register = i < 4;
        } else if (is_float) {
            reg_index = float_index++;
            in_register = reg_index < 8;
        } else {
            reg_index = integer_index++;
            in_register = reg_index < 6;
        }

        if (in_register) {
            if (var->is_array || var->is_pointer || var->type == TYPE_STRING) {
                code_printf(parser, "    movq %s, %d(%%rbp)\n", regs64[reg_index], var->offset);
            } else if (var->type == TYPE_FLOAT) {
                code_printf(parser, "    movss %%xmm%d, %d(%%rbp)\n", reg_index, var->offset);
            } else if (var->type == TYPE_DOUBLE) {
                code_printf(parser, "    movsd %%xmm%d, %d(%%rbp)\n", reg_index, var->offset);
            } else if (var->type == TYPE_CHAR || var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                code_printf(parser, "    movb %s, %d(%%rbp)\n", regs8[reg_index], var->offset);
            } else {
                code_printf(parser, "    movl %s, %d(%%rbp)\n", regs32[reg_index], var->offset);
            }
        } else {
            int source_offset = (parser->target_format == TARGET_COFF ? 48 : 16) + stack_index++ * 8;
            if (var->is_array || var->is_pointer || var->type == TYPE_STRING || var->type == TYPE_DOUBLE) {
                code_printf(parser, "    movq %d(%%rbp), %%rax\n", source_offset);
                code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
            } else {
                code_printf(parser, "    movl %d(%%rbp), %%eax\n", source_offset);
                code_printf(parser, "    movl %%eax, %d(%%rbp)\n", var->offset);
            }
        }
    }
}

static int function_frame_size(Parser *parser, int first_var) {
    int required = 0;
    for (int i = first_var; i < parser->var_count; i++) {
        if (parser->vars[i].offset < 0 && -parser->vars[i].offset > required) {
            required = -parser->vars[i].offset;
        }
    }
    if (required < 16) required = 16;
    return (required + 15) & ~15;
}

static void skip_balanced(const TokenStream *tokens, int *position,
                          TokenType open, TokenType close) {
    if (*position >= tokens->count || tokens->tokens[*position].type != open) return;
    int depth = 0;
    do {
        TokenType type = tokens->tokens[(*position)++].type;
        if (type == open) depth++;
        else if (type == close) depth--;
    } while (*position < tokens->count && depth > 0);
}

static int statement_guarantees_return(const TokenStream *tokens, int *position);

static int block_guarantees_return(const TokenStream *tokens, int *position) {
    if (*position >= tokens->count || tokens->tokens[*position].type != TOKEN_LBRACE) return 0;
    (*position)++;
    int guaranteed = 0;
    while (*position < tokens->count && tokens->tokens[*position].type != TOKEN_RBRACE) {
        int statement_returns = statement_guarantees_return(tokens, position);
        if (statement_returns) guaranteed = 1;
    }
    if (*position < tokens->count && tokens->tokens[*position].type == TOKEN_RBRACE) (*position)++;
    return guaranteed;
}

static int statement_guarantees_return(const TokenStream *tokens, int *position) {
    if (*position >= tokens->count) return 0;
    TokenType type = tokens->tokens[*position].type;
    if (type == TOKEN_KEYWORD_RETURN) {
        while (*position < tokens->count && tokens->tokens[*position].type != TOKEN_SEMICOLON &&
               tokens->tokens[*position].type != TOKEN_RBRACE) (*position)++;
        if (*position < tokens->count && tokens->tokens[*position].type == TOKEN_SEMICOLON) (*position)++;
        return 1;
    }
    if (type == TOKEN_KEYWORD_IF) {
        (*position)++;
        skip_balanced(tokens, position, TOKEN_LPAREN, TOKEN_RPAREN);
        int then_returns = block_guarantees_return(tokens, position);
        if (*position >= tokens->count || tokens->tokens[*position].type != TOKEN_KEYWORD_ELSE) {
            return 0;
        }
        (*position)++;
        int else_returns = tokens->tokens[*position].type == TOKEN_KEYWORD_IF
            ? statement_guarantees_return(tokens, position)
            : block_guarantees_return(tokens, position);
        return then_returns && else_returns;
    }
    if (type == TOKEN_LBRACE) return block_guarantees_return(tokens, position);

    while (*position < tokens->count && tokens->tokens[*position].type != TOKEN_SEMICOLON &&
           tokens->tokens[*position].type != TOKEN_RBRACE) {
        if (tokens->tokens[*position].type == TOKEN_LBRACE) {
            (void) block_guarantees_return(tokens, position);
            return 0;
        }
        (*position)++;
    }
    if (*position < tokens->count && tokens->tokens[*position].type == TOKEN_SEMICOLON) (*position)++;
    return 0;
}

static void collect_function_signatures(Parser *parser) {
    TokenStream *tokens = parser->tokens;
    int depth = 0;
    for (int i = 0; i < tokens->count; i++) {
        Token token = tokens->tokens[i];
        if (token.type == TOKEN_LBRACE) { depth++; continue; }
        if (token.type == TOKEN_RBRACE) { if (depth > 0) depth--; continue; }
        if (depth != 0 || token.type != TOKEN_KEYWORD_FUNC || i + 2 >= tokens->count) continue;

        Token name = tokens->tokens[++i];
        if (name.type != TOKEN_IDENTIFIER || tokens->tokens[++i].type != TOKEN_LPAREN) continue;
        if (find_function(parser, name.value)) continue;
        if (parser->function_count >= MAX_FUNCTIONS) break;

        Function *func = &parser->functions[parser->function_count++];
        memset(func, 0, sizeof(*func));
        strncpy(func->name, name.value, MAX_TOKEN - 1);
        while (i + 1 < tokens->count && tokens->tokens[i + 1].type != TOKEN_RPAREN) {
            if (!ensure_param_capacity(parser, func, func->param_count + 1)) return;
            Token param = tokens->tokens[++i];
            strncpy(func->params[func->param_count], param.value, MAX_TOKEN - 1);
            func->params[func->param_count][MAX_TOKEN - 1] = '\0';
            func->param_is_array[func->param_count] = 0;
            func->param_is_pointer[func->param_count] = 0;
            if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_LBRACKET) {
                func->param_is_array[func->param_count] = 1;
                i += 2;
            }
            if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_COLON) i++;
            if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_STAR) {
                func->param_is_pointer[func->param_count] = 1;
                i++;
            }
            if (i + 1 < tokens->count) {
                func->param_types[func->param_count] = token_to_datatype(tokens->tokens[++i].type);
            }
            func->param_count++;
            if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_COMMA) i++;
        }
        if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_RPAREN) i++;
        if (i + 1 < tokens->count && tokens->tokens[i + 1].type == TOKEN_ARROW) i++;
        if (i + 1 < tokens->count) func->return_type = token_to_datatype(tokens->tokens[++i].type);
    }
}

/*
 * parse_program - Parse entire program
 * @parser: Parser state
 * @source_file: Source file path
 *
 * Top-level parser entry point. Parses imports, structs, enums,
 * and functions. Returns 1 on success, 0 on error.
 */
int parse_program(Parser *parser, const char *source_file) {
    if (!parser_load_source(parser, source_file)) return 0;
    collect_function_signatures(parser);

    for (int i = 1; i + 1 < parser->tokens->count; i++) {
        if (parser->tokens->tokens[i].type == TOKEN_PERCENT &&
            (parser->tokens->tokens[i - 1].type == TOKEN_FLOAT_LITERAL ||
             parser->tokens->tokens[i + 1].type == TOKEN_FLOAT_LITERAL)) {
            semantic_error_at_token(parser, parser->tokens->tokens[i],
                                    ERR_TYPE_INVALID_OPERATION,
                                    "Remainder requires integer operands");
        }
    }
    for (int i = 0; i + 4 < parser->tokens->count; i++) {
        if (parser->tokens->tokens[i].type != TOKEN_KEYWORD_VAR ||
            parser->tokens->tokens[i + 1].type != TOKEN_LBRACKET ||
            parser->tokens->tokens[i + 2].type != TOKEN_NUMBER ||
            parser->tokens->tokens[i + 3].type != TOKEN_RBRACKET ||
            parser->tokens->tokens[i + 4].type != TOKEN_IDENTIFIER) continue;
        unsigned long length = strtoul(parser->tokens->tokens[i + 2].value, NULL, 10);
        const char *array_name = parser->tokens->tokens[i + 4].value;
        for (int j = i + 5; j + 2 < parser->tokens->count; j++) {
            if (parser->tokens->tokens[j].type == TOKEN_IDENTIFIER &&
                strcmp(parser->tokens->tokens[j].value, array_name) == 0 &&
                parser->tokens->tokens[j + 1].type == TOKEN_LBRACKET &&
                parser->tokens->tokens[j + 2].type == TOKEN_NUMBER) {
                unsigned long index = strtoul(parser->tokens->tokens[j + 2].value, NULL, 10);
                if (index >= length) {
                    semantic_error_at_token(parser, parser->tokens->tokens[j + 2], ERR_SEM_NOT_ARRAY,
                                            "Array index %lu is outside declared bounds [0, %lu)",
                                            index, length);
                }
            }
        }
    }

    if (parser->debug_mode) {
        printf("\n================================================================\n");
        printf("           PHASE 2: SYNTAX ANALYSIS (PARSER)\n");
        printf("================================================================\n\n");
    }

    while (!is_at_end(parser->tokens)) {
        if (global_error_handler && error_handler_should_stop(global_error_handler)) break;
        if (check(parser->tokens, TOKEN_HASH)) {
            parse_import(parser, source_file);
        } else if (check(parser->tokens, TOKEN_KEYWORD_STRUCT)) {
            parse_struct(parser);
        } else if (check(parser->tokens, TOKEN_KEYWORD_ENUM)) {
            parse_enum(parser);
        } else if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            parse_function(parser);
        } else {
            parser_error(parser, "Expected function declaration, struct, enum, or import at top level");
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
            memset(func, 0, sizeof(*func));
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

                if (!ensure_param_capacity(parser, func, func->param_count + 1)) return;

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
                } else if (!check(parser->tokens, TOKEN_RPAREN)) {
                    parser_error(parser, "Expected ',' or ')' after parameter");
                    return;
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
            if (parser->target_format == TARGET_COFF) {
                code_printf(parser, "    movq $.L_frame_%s, %%rax\n", mangled_name);
                code_printf(parser, "    call ___chkstk_ms\n");
                code_printf(parser, "    subq %%rax, %%rsp\n");
            } else {
                code_printf(parser, "    subq $.L_frame_%s, %%rsp\n", mangled_name);
            }

            code_comment(parser, "Save ABI non-volatile registers");
            save_nonvolatile_registers(parser);

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

            data_printf(parser, ".set .L_frame_%s, %d\n", mangled_name,
                        function_frame_size(parser, saved_var_count));

            cleanup_scope(parser, parser->current_scope);
            parser->current_scope = 0;
            parser->scope_depth = 0;
            parser->current_struct_context[0] = '\0';
            parser->var_count = saved_var_count;

            if (func->return_type == TYPE_VOID) {
                code_comment(parser, "Function epilogue (void return)");

                restore_nonvolatile_registers(parser);

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

            Token field_name_token = consume(parser->tokens);
            strcpy(field_name, field_name_token.value);

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
                    errno = 0;
                    char *end = NULL;
                    unsigned long parsed_size = strtoul(size_token.value, &end, 10);
                    if (errno == ERANGE || end == size_token.value || *end != '\0' ||
                        parsed_size == 0 || parsed_size > INT_MAX) {
                        parser_error(parser, "Array length must be between 1 and %d", INT_MAX);
                        return;
                    }
                    array_size = (int) parsed_size;
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
                if (element_size <= 0 || array_size > MAX_OBJECT_SIZE / element_size) {
                    parser_error(parser, "Array length produces an object larger than %d bytes",
                                 MAX_OBJECT_SIZE);
                    return;
                }
                field->size = element_size * array_size;
            } else {
                field->size = element_size;
            }

            if (field->size < 0 || current_offset > MAX_OBJECT_SIZE - field->size) {
                parser_error(parser, "Struct storage exceeds %d bytes", MAX_OBJECT_SIZE);
                return;
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
        } else if (!check(parser->tokens, TOKEN_RPAREN)) {
            parser_error(parser, "Expected ',' or ')' after parameter");
            return;
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

        for (int i = 0; i < enum_def->value_count; ++i) {
            if (strcmp(enum_def->values[i].name, value_name_token.value) == 0) {
                parser_error_code(parser, ERR_PARSE_DUPLICATE_DEFINITION,
                                  "Enum member '%s' is already defined", value_name_token.value);
                return;
            }
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

    Function *func = find_function(parser, func_name);
    if (!func && parser->function_count >= MAX_FUNCTIONS) {
        parser_error(parser, "Too many functions (max %d)", MAX_FUNCTIONS);
        return;
    }

    if (!func) {
        func = &parser->functions[parser->function_count++];
        memset(func, 0, sizeof(*func));
        strcpy(func->name, func_name);
    } else if (func->is_defined) {
        parser_error_code(parser, ERR_PARSE_DUPLICATE_DEFINITION,
                          "Duplicate function '%s'", func_name);
        return;
    }
    func->is_defined = 1;
    func->param_count = 0;

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");

    int saved_var_count = parser->var_count;
    int param_offset = 0;

    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
        Token param_token = consume(parser->tokens);

        if (!ensure_param_capacity(parser, func, func->param_count + 1)) return;

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

        if (var->size == 8) param_offset = (param_offset + 7) & ~7;
        param_offset += var->size;
        var->offset = -param_offset;

        var->scope = 1;
        parser->var_count++;

        func->param_count++;

        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        } else if (!check(parser->tokens, TOKEN_RPAREN)) {
            parser_error(parser, "Expected ',' or ')' after parameter");
            return;
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
    if (parser->target_format == TARGET_COFF) {
        code_printf(parser, "    movq $.L_frame_%s, %%rax\n", func_name);
        code_printf(parser, "    call ___chkstk_ms\n");
        code_printf(parser, "    subq %%rax, %%rsp\n");
    } else {
        code_printf(parser, "    subq $.L_frame_%s, %%rsp\n", func_name);
    }

    code_comment(parser, "Save ABI non-volatile registers");
    save_nonvolatile_registers(parser);

    if (func->param_count > 0) {
        code_comment(parser, "Save parameters to stack");
    }

    save_function_parameters(parser, func, saved_var_count);

    parser->current_scope = 1;
    parser->scope_depth = 1;
    parser->current_return_type = func->return_type;

    if (func->return_type != TYPE_VOID) {
        int body_position = parser->tokens->current;
        if (!block_guarantees_return(parser->tokens, &body_position)) {
            parser_error(parser, "Function '%s' does not return on all paths", func_name);
        }
    }

    parse_function_body(parser);

    data_printf(parser, ".set .L_frame_%s, %d\n", func_name,
                function_frame_size(parser, saved_var_count));

    cleanup_scope(parser, parser->current_scope);
    parser->current_scope = 0;
    parser->scope_depth = 0;
    parser->current_return_type = TYPE_VOID;
    parser->var_count = saved_var_count;

    if (func->return_type == TYPE_VOID) {
        code_comment(parser, "Function epilogue (void return)");
        code_printf(parser, "    xorl %%eax, %%eax\n"); // Return 0 for void functions

        restore_nonvolatile_registers(parser);

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
