#include "parser.h"
#include "parser_internal.h"
#include "errorHandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * create_parser - Initialize parser with token stream
 * @tokens: Lexical token stream
 *
 * Allocates and initializes parser state including symbol tables,
 * code buffers, and compilation context.
 */
Parser *create_parser(TokenStream *tokens) {
    Parser *parser = malloc(sizeof(Parser));

    parser->tokens = tokens;
    parser->var_count = 0;
    parser->function_count = 0;
    parser->struct_count = 0;
    parser->enum_count = 0;
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
    parser->loop_depth = 0;
    parser->current_struct_context[0] = '\0';
    parser->import_count = 0;
    parser->next_var_is_gc = 0;
    parser->source_content = NULL;
    parser->source_lines = NULL;
    parser->source_line_count = 0;
    parser->source_filename = NULL;
    parser->target_format = TARGET_ELF;  /* Default, will be set by main */
    parser->syntax_mode = SYNTAX_INTEL;  /* Default, will be set by main */

    memset(parser->code_buffer, 0, CODE_BUFFER_SIZE);
    memset(parser->function_code_buffer, 0, CODE_BUFFER_SIZE);

    return parser;
}

void free_parser(Parser *parser) {
    if (parser) {
        if (parser->source_content) {
            free(parser->source_content);
        }
        if (parser->source_lines) {
            free(parser->source_lines);
        }
        free(parser);
    }
}

void parser_load_source(Parser *parser, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        return;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    parser->source_content = malloc(size + 1);
    if (!parser->source_content) {
        fclose(file);
        return;
    }
    
    size_t bytes_read = fread(parser->source_content, 1, size, file);
    parser->source_content[bytes_read] = '\0';
    fclose(file);
    
    int line_count = 1;
    for (size_t i = 0; i < bytes_read; i++) {
        if (parser->source_content[i] == '\n') {
            line_count++;
        }
    }
    
    parser->source_lines = malloc(sizeof(char*) * line_count);
    if (!parser->source_lines) {
        return;
    }
    
    parser->source_lines[0] = parser->source_content;
    parser->source_line_count = 1;
    
    for (size_t i = 0; i < bytes_read; i++) {
        if (parser->source_content[i] == '\n') {
            parser->source_content[i] = '\0';   
            if (i + 1 < bytes_read) {
                parser->source_lines[parser->source_line_count++] = &parser->source_content[i + 1];
            }
        }
    }
    
    parser->source_filename = filename;
}

const char *parser_get_source_line(Parser *parser, int line) {
    if (!parser->source_lines || line < 1 || line > parser->source_line_count) {
        return NULL;
    }
    return parser->source_lines[line - 1];
}

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

void data_printf(Parser *parser, const char *format, ...) {
    va_list args;
    va_start(args, format);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    int len = strlen(buffer);
    if (parser->code_pos + len < CODE_BUFFER_SIZE) {
        strcpy(parser->code_buffer + parser->code_pos, buffer);
        parser->code_pos += len;
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

void parse_printline_statement(Parser *parser);

void parser_error_code(Parser *parser, int error_code, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    if (global_error_handler) {
        ErrorContext *ctx = error_context_create(
            SEVERITY_ERROR,
            current.line,
            current.column,
            ERROR_CATEGORY_PARSER,
            error_code,
            parser->source_filename,
            message
        );
        
        if (ctx) {
             
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            char token_info[512];
            snprintf(token_info, sizeof(token_info), "%s '%s'",
                    token_type_to_string(current.type), current.value);
            error_context_set_token(ctx, token_info);
            
            error_report_context(global_error_handler, ctx);
            
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
         
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", current.line, current.column, message);
        fprintf(stderr, "  At token: %s '%s'\n",
                token_type_to_string(current.type), current.value);
    }

    if (!is_at_end(parser->tokens)) {
        consume(parser->tokens);
    }
}

void parser_error(Parser *parser, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    if (global_error_handler) {
        ErrorContext *ctx = error_context_create(
            SEVERITY_ERROR,
            current.line,
            current.column,
            ERROR_CATEGORY_PARSER,
            ERR_PARSE_UNEXPECTED_TOKEN,
            parser->source_filename,
            message
        );
        
        if (ctx) {
             
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            char token_info[512];
            snprintf(token_info, sizeof(token_info), "%s '%s'",
                    token_type_to_string(current.type), current.value);
            error_context_set_token(ctx, token_info);
            
            error_report_context(global_error_handler, ctx);
            
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
         
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", current.line, current.column, message);
        fprintf(stderr, "  At token: %s '%s'\n",
                token_type_to_string(current.type), current.value);
    }

    if (!is_at_end(parser->tokens)) {
        consume(parser->tokens);
    }
}

void expect(Parser *parser, TokenType type, const char *message) {
    if (!check(parser->tokens, type)) {
         
        int error_code = ERR_PARSE_EXPECTED_TOKEN;
        if (type == TOKEN_SEMICOLON) {
            error_code = ERR_PARSE_MISSING_SEMICOLON;
        } else if (type == TOKEN_LBRACE || type == TOKEN_RBRACE) {
            error_code = ERR_PARSE_MISSING_BRACE;
        } else if (type == TOKEN_LPAREN || type == TOKEN_RPAREN) {
            error_code = ERR_PARSE_MISSING_PAREN;
        }
        parser_error_code(parser, error_code, "%s", message);
        return;
    }
    consume(parser->tokens);
}

void semantic_error(Parser *parser, int error_code, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    if (global_error_handler) {
        ErrorContext *ctx = error_context_create(
            SEVERITY_ERROR,
            current.line,
            current.column,
            ERROR_CATEGORY_SEMANTIC,
            error_code,
            parser->source_filename,
            message
        );
        
        if (ctx) {
             
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            error_report_context(global_error_handler, ctx);
            
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", current.line, current.column, message);
    }

}

void semantic_error_at_token(Parser *parser, Token token, int error_code, const char *format, ...) {
    parser->has_error = 1;
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    if (global_error_handler) {
        ErrorContext *ctx = error_context_create(
            SEVERITY_ERROR,
            token.line,
            token.column,
            ERROR_CATEGORY_SEMANTIC,
            error_code,
            parser->source_filename,
            message
        );
        
        if (ctx) {
             
            const char *source_line = parser_get_source_line(parser, token.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            error_context_set_token(ctx, token.value);
            
            error_report_context(global_error_handler, ctx);
            
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", token.line, token.column, message);
    }

}

void synchronize(Parser *parser) {
     
    while (!is_at_end(parser->tokens)) {
        Token current = peek(parser->tokens);
        
        if (current.type == TOKEN_SEMICOLON) {
            consume(parser->tokens);
            return;
        }
        
        if (current.type == TOKEN_KEYWORD_VAR ||
            current.type == TOKEN_KEYWORD_IF ||
            current.type == TOKEN_KEYWORD_FOR ||
            current.type == TOKEN_KEYWORD_WHILE ||
            current.type == TOKEN_KEYWORD_RETURN ||
            current.type == TOKEN_KEYWORD_PRINT ||
            current.type == TOKEN_KEYWORD_PRINTLINE ||
            current.type == TOKEN_KEYWORD_BREAK ||
            current.type == TOKEN_KEYWORD_CONTINUE ||
            current.type == TOKEN_KEYWORD_FREE ||
            current.type == TOKEN_RBRACE) {
            return;
        }
        
        consume(parser->tokens);
    }
}

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

StructDefinition *find_struct(Parser *parser, const char *name) {
    for (int i = 0; i < parser->struct_count; i++) {
        if (strcmp(parser->structs[i].name, name) == 0) {
            return &parser->structs[i];
        }
    }
    return NULL;
}

EnumDefinition *find_enum(Parser *parser, const char *name) {
    for (int i = 0; i < parser->enum_count; i++) {
        if (strcmp(parser->enums[i].name, name) == 0) {
            return &parser->enums[i];
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
    // First pass: generate free() calls for @gc variables
    int gc_count = 0;
    for (int i = parser->var_count - 1; i >= 0; i--) {
        if (parser->vars[i].scope == scope) {
            Variable *var = &parser->vars[i];
            // Free @gc variables that are heap-allocated pointers
            if (var->is_gc && var->is_pointer) {
                if (gc_count == 0) {
                    code_comment(parser, "GC: Freeing garbage-collected variables");
                }
                code_comment(parser, "GC: free(%s)", var->name);
                const char **arg_regs = get_arg_registers_64();
                code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, arg_regs[0]);
                generate_stack_align(parser);
                code_printf(parser, "    call free\n");
                generate_stack_restore(parser);
                gc_count++;
            }
        } else {
            break;
        }
    }

    // Second pass: mark variables as removed from scope
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
        code_comment(parser, "Cleanup: %d variable(s) from scope %d (GC freed: %d)", removed, scope, gc_count);
    }
}

static int is_already_imported(Parser *parser, const char *filepath) {
    for (int i = 0; i < parser->import_count; i++) {
        if (strcmp(parser->imported_files[i], filepath) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_imported_file(Parser *parser, const char *filepath) {
    if (parser->import_count >= MAX_IMPORTS) {
        parser_error(parser, "Too many imports (max %d)", MAX_IMPORTS);
        return;
    }
    strncpy(parser->imported_files[parser->import_count], filepath, MAX_PATH - 1);
    parser->imported_files[parser->import_count][MAX_PATH - 1] = '\0';
    parser->import_count++;
}

static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static char *resolve_import_path(const char *base_path, const char *import_file) {
    static char resolved_path[MAX_PATH];
    static char temp_path[MAX_PATH];
    
    if (import_file[0] == '/' || (import_file[0] != '\0' && import_file[1] == ':')) {
        strncpy(resolved_path, import_file, MAX_PATH - 1);
        resolved_path[MAX_PATH - 1] = '\0';
        return resolved_path;
    }
    
    strncpy(temp_path, import_file, MAX_PATH - 1);
    temp_path[MAX_PATH - 1] = '\0';
    if (file_exists(temp_path)) {
        strncpy(resolved_path, temp_path, MAX_PATH - 1);
        resolved_path[MAX_PATH - 1] = '\0';
        return resolved_path;
    }
    
    if (base_path && base_path[0] != '\0') {
         
        const char *last_slash = strrchr(base_path, '/');
        const char *last_backslash = strrchr(base_path, '\\');
        const char *separator = last_slash > last_backslash ? last_slash : last_backslash;
        
        if (separator) {
            int dir_len = separator - base_path + 1;
            if (dir_len < MAX_PATH) {
                strncpy(temp_path, base_path, dir_len);
                temp_path[dir_len] = '\0';
                strncat(temp_path, import_file, MAX_PATH - dir_len - 1);
                
                if (file_exists(temp_path)) {
                    strncpy(resolved_path, temp_path, MAX_PATH - 1);
                    resolved_path[MAX_PATH - 1] = '\0';
                    return resolved_path;
                }
            }
        }
    }
    
    strncpy(resolved_path, import_file, MAX_PATH - 1);
    resolved_path[MAX_PATH - 1] = '\0';
    return resolved_path;
}

void parse_import(Parser *parser, const char *base_path) {
    Token import_token = consume(parser->tokens);   
    
    if (!match(parser->tokens, TOKEN_KEYWORD_IMPORT)) {
        parser_error(parser, "Expected 'import' after '#'");
        return;
    }
    
    Token filename_token = peek(parser->tokens);
    if (filename_token.type == TOKEN_LESS) {
         
        consume(parser->tokens);   
        
        char import_filename[MAX_PATH] = {0};
        
        while (!is_at_end(parser->tokens) && !check(parser->tokens, TOKEN_GREATER)) {
            Token token = consume(parser->tokens);
            
            if (token.type == TOKEN_IDENTIFIER || token.type == TOKEN_NUMBER) {
                strncat(import_filename, token.value, MAX_PATH - strlen(import_filename) - 1);
            } else if (token.type == TOKEN_DOT) {
                strncat(import_filename, ".", MAX_PATH - strlen(import_filename) - 1);
            } else if (token.type == TOKEN_SLASH) {
                strncat(import_filename, "/", MAX_PATH - strlen(import_filename) - 1);
            } else if (token.type == TOKEN_MINUS) {
                strncat(import_filename, "-", MAX_PATH - strlen(import_filename) - 1);
            } else {
                parser_error(parser, "Unexpected token in import path: %s", token.value);
                return;
            }
        }
        
        if (!match(parser->tokens, TOKEN_GREATER)) {
            parser_error(parser, "Expected '>' after filename");
            return;
        }
        
        char *resolved_path = resolve_import_path(base_path, import_filename);
        
        if (is_already_imported(parser, resolved_path)) {
            if (parser->debug_mode) {
                printf("[INFO] Skipping already imported file: %s\n", resolved_path);
            }
            return;
        }
        
        add_imported_file(parser, resolved_path);
        
        if (parser->debug_mode) {
            printf("[INFO] Importing file: %s\n", resolved_path);
        }
        
        TokenStream *imported_stream = tokenize_file(resolved_path, parser->debug_mode);
        if (!imported_stream) {
            parser_error(parser, "Failed to open import file: %s", resolved_path);
            return;
        }
        
        TokenStream *original_stream = parser->tokens;
        int original_pos = parser->tokens->current;
        
        parser->tokens = imported_stream;
        
        while (!is_at_end(parser->tokens)) {
            if (check(parser->tokens, TOKEN_HASH)) {
                 
                parse_import(parser, resolved_path);
            } else if (check(parser->tokens, TOKEN_KEYWORD_STRUCT)) {
                parse_struct(parser);
            } else if (check(parser->tokens, TOKEN_KEYWORD_ENUM)) {
                parse_enum(parser);
            } else if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
                parse_function(parser);
            } else {
                parser_error(parser, "Only imports, structs, enums and functions allowed in imported files");
                consume(parser->tokens);
            }
        }
        
        free_token_stream(imported_stream);
        parser->tokens = original_stream;
        
        if (parser->debug_mode) {
            printf("[INFO] Import complete: %s\n", resolved_path);
        }
    } else if (filename_token.type == TOKEN_STRING_LITERAL) {
         
        Token name_token = consume(parser->tokens);
        
        char *resolved_path = resolve_import_path(base_path, name_token.value);
        
        if (is_already_imported(parser, resolved_path)) {
            if (parser->debug_mode) {
                printf("[INFO] Skipping already imported file: %s\n", resolved_path);
            }
            return;
        }
        
        add_imported_file(parser, resolved_path);
        
        if (parser->debug_mode) {
            printf("[INFO] Importing file: %s\n", resolved_path);
        }
        
        TokenStream *imported_stream = tokenize_file(resolved_path, parser->debug_mode);
        if (!imported_stream) {
            parser_error(parser, "Failed to open import file: %s", resolved_path);
            return;
        }
        
        TokenStream *original_stream = parser->tokens;
        int original_pos = parser->tokens->current;
        
        parser->tokens = imported_stream;
        
        while (!is_at_end(parser->tokens)) {
            if (check(parser->tokens, TOKEN_HASH)) {
                parse_import(parser, resolved_path);
            } else if (check(parser->tokens, TOKEN_KEYWORD_STRUCT)) {
                parse_struct(parser);
            } else if (check(parser->tokens, TOKEN_KEYWORD_ENUM)) {
                parse_enum(parser);
            } else if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
                parse_function(parser);
            } else {
                parser_error(parser, "Only imports, structs, enums and functions allowed in imported files");
                consume(parser->tokens);
            }
        }
        
        free_token_stream(imported_stream);
        parser->tokens = original_stream;
        
        if (parser->debug_mode) {
            printf("[INFO] Import complete: %s\n", resolved_path);
        }
    } else {
        parser_error(parser, "Expected '<' or string literal after 'import'");
    }
}

