#include "parser.h"
#include "errorHandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

// ============================================================================
// TYPE HELPER FUNCTIONS
// ============================================================================

// Helper to escape character literals for safe use in comments
static const char *escape_char_for_comment(const char *ch_value) {
    static char buffer[32];
    // ch_value contains the actual character, not the escape sequence
    // We need to convert it back to a printable form for comments
    if (ch_value[0] == '\n') {
        return "\\n";
    } else if (ch_value[0] == '\t') {
        return "\\t";
    } else if (ch_value[0] == '\r') {
        return "\\r";
    } else if (ch_value[0] == '\0') {
        return "\\0";
    } else if (ch_value[0] == '\\') {
        return "\\\\";
    } else if (ch_value[0] == '\'') {
        return "\\'";
    } else if (ch_value[0] >= 32 && ch_value[0] < 127) {
        // Printable ASCII character
        snprintf(buffer, sizeof(buffer), "%c", ch_value[0]);
        return buffer;
    } else {
        // Non-printable character - show as hex
        snprintf(buffer, sizeof(buffer), "\\x%02x", (unsigned char)ch_value[0]);
        return buffer;
    }
}

// Helper to escape strings for safe use in comments
static void escape_string_for_comment(const char *str, char *output, size_t output_size) {
    size_t out_pos = 0;
    for (size_t i = 0; str[i] != '\0' && out_pos < output_size - 5; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == '\n') {
            output[out_pos++] = '\\';
            output[out_pos++] = 'n';
        } else if (c == '\t') {
            output[out_pos++] = '\\';
            output[out_pos++] = 't';
        } else if (c == '\r') {
            output[out_pos++] = '\\';
            output[out_pos++] = 'r';
        } else if (c == '\\') {
            output[out_pos++] = '\\';
            output[out_pos++] = '\\';
        } else if (c == '"') {
            output[out_pos++] = '\\';
            output[out_pos++] = '"';
        } else if (c >= 32 && c < 127) {
            output[out_pos++] = c;
        } else {
            // Non-printable - show as hex
            int written = snprintf(&output[out_pos], output_size - out_pos, "\\x%02x", c);
            if (written > 0) out_pos += written;
        }
    }
    output[out_pos] = '\0';
}

// Platform detection
static int is_windows_platform() {
#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
    return 1;
#else
    return 0;
#endif
}

// Get platform-specific argument registers
// Windows x64: RCX, RDX, R8, R9
// Linux x64 (System V AMD64 ABI): RDI, RSI, RDX, RCX, R8, R9
static const char **get_arg_registers_64() {
    static const char *windows_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};
    static const char *linux_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

static const char **get_arg_registers_32() {
    static const char *windows_regs[] = {"%ecx", "%edx", "%r8d", "%r9d"};
    static const char *linux_regs[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

static const char **get_arg_registers_8() {
    static const char *windows_regs[] = {"%cl", "%dl", "%r8b", "%r9b"};
    static const char *linux_regs[] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

static int get_max_reg_args() {
    return is_windows_platform() ? 4 : 6;
}

// Get the stack alignment requirement
static int get_stack_alignment() {
    return 16;  // Both Windows and Linux require 16-byte stack alignment
}

// Get shadow space size (Windows x64 requires 32 bytes, Linux doesn't need it)
static int get_shadow_space() {
    return is_windows_platform() ? 32 : 0;
}
// Get the stack space needed before a call for proper alignment and shadow space  
// Linux: Stack is already 16-byte aligned after pushq %rbp, no adjustment needed
// Windows: Need 32 bytes shadow space + 8 for alignment = 40 bytes
static int get_call_stack_space() {
    if (is_windows_platform()) {
        return 40;
    } else {
        return 0;  // Linux: no stack adjustment needed before calls
    }
}


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

// Check if a function name is a built-in string function
int is_builtin_string_function(const char *name) {
    return strcmp(name, "strlen") == 0 ||
           strcmp(name, "strcpy") == 0 ||
           strcmp(name, "strcat") == 0 ||
           strcmp(name, "strcmp") == 0 ||
           strcmp(name, "strdup") == 0;
}

// Check if a function name is a built-in memory function
int is_builtin_memory_function(const char *name) {
    return strcmp(name, "malloc") == 0 ||
           strcmp(name, "free") == 0;
}

// Check if a function name is a built-in input function
int is_builtin_input_function(const char *name) {
    return strcmp(name, "scanfInt") == 0 ||
           strcmp(name, "scanfChar") == 0 ||
           strcmp(name, "scanfString") == 0;
}

// Check if a function name is a syscall I/O function
int is_syscall_io_function(const char *name) {
    return strcmp(name, "sys_write") == 0 ||
           strcmp(name, "sys_read") == 0 ||
           strcmp(name, "sys_open") == 0 ||
           strcmp(name, "sys_close") == 0 ||
           strcmp(name, "io_strlen") == 0 ||
           strcmp(name, "io_int_to_str") == 0 ||
           strcmp(name, "io_str_to_int") == 0;
}

// ============================================================================
// SYSCALL-BASED I/O HELPER FUNCTIONS
// ============================================================================

// Generate syscall for writing a string to stdout
// Uses: write(1, buffer, length) - syscall number 1
static void generate_write_syscall(Parser *parser, const char *buffer_reg, const char *length_reg) {
    code_comment(parser, "syscall: write(stdout, buffer, length)");
    code_printf(parser, "    movq $1, %%rax\n");      // syscall number: write
    code_printf(parser, "    movq $1, %%rdi\n");      // file descriptor: stdout
    code_printf(parser, "    movq %s, %%rsi\n", buffer_reg);  // buffer pointer
    code_printf(parser, "    movq %s, %%rdx\n", length_reg);  // byte count
    code_printf(parser, "    syscall\n");
}

// Generate syscall for reading from stdin
// Uses: read(0, buffer, length) - syscall number 0
static void generate_read_syscall(Parser *parser, const char *buffer_reg, const char *length_reg) {
    code_comment(parser, "syscall: read(stdin, buffer, length)");
    code_printf(parser, "    movq $0, %%rax\n");      // syscall number: read
    code_printf(parser, "    movq $0, %%rdi\n");      // file descriptor: stdin
    code_printf(parser, "    movq %s, %%rsi\n", buffer_reg);  // buffer pointer
    code_printf(parser, "    movq %s, %%rdx\n", length_reg);  // byte count
    code_printf(parser, "    syscall\n");
}

// Calculate string length at runtime
static void generate_strlen_code(Parser *parser, const char *str_ptr_reg, const char *result_reg) {
    code_comment(parser, "Calculate string length");
    code_printf(parser, "    xorq %s, %s\n", result_reg, result_reg);  // result = 0
    code_printf(parser, ".Lstrlen_loop_%d:\n", parser->label_counter);
    code_printf(parser, "    movb (%s,%s,1), %%cl\n", str_ptr_reg, result_reg);  // Use %cl instead of %al
    code_printf(parser, "    testb %%cl, %%cl\n");
    code_printf(parser, "    je .Lstrlen_done_%d\n", parser->label_counter);
    code_printf(parser, "    incq %s\n", result_reg);
    code_printf(parser, "    jmp .Lstrlen_loop_%d\n", parser->label_counter);
    code_printf(parser, ".Lstrlen_done_%d:\n", parser->label_counter);
    parser->label_counter++;
}

// Convert integer to string (for printing integers)
static void generate_int_to_str_code(Parser *parser, const char *value_reg, const char *buffer_reg) {
    code_comment(parser, "Convert integer to string");
    int label_num = parser->label_counter++;
    
    // Handle zero specially
    code_printf(parser, "    testq %s, %s\n", value_reg, value_reg);
    code_printf(parser, "    jne .Lint2str_nonzero_%d\n", label_num);
    code_printf(parser, "    movb $48, (%s)\n", buffer_reg);  // '0'
    code_printf(parser, "    movb $0, 1(%s)\n", buffer_reg);
    code_printf(parser, "    jmp .Lint2str_done_%d\n", label_num);
    
    code_printf(parser, ".Lint2str_nonzero_%d:\n", label_num);
    // Check for negative
    code_printf(parser, "    movq %s, %%r10\n", value_reg);  // r10 = value
    code_printf(parser, "    xorq %%r11, %%r11\n");  // r11 = is_negative flag
    code_printf(parser, "    testq %%r10, %%r10\n");
    code_printf(parser, "    jge .Lint2str_positive_%d\n", label_num);
    code_printf(parser, "    negq %%r10\n");  // make positive
    code_printf(parser, "    movq $1, %%r11\n");  // set negative flag
    
    code_printf(parser, ".Lint2str_positive_%d:\n", label_num);
    // Convert digits (right to left)
    code_printf(parser, "    movq %s, %%r12\n", buffer_reg);  // r12 = buffer pointer
    code_printf(parser, "    addq $20, %%r12\n");  // point to end of buffer
    code_printf(parser, "    movb $0, (%%r12)\n");  // null terminator
    
    code_printf(parser, ".Lint2str_loop_%d:\n", label_num);
    code_printf(parser, "    xorq %%rdx, %%rdx\n");
    code_printf(parser, "    movq %%r10, %%rax\n");
    code_printf(parser, "    movq $10, %%rcx\n");
    code_printf(parser, "    divq %%rcx\n");  // rax = quotient, rdx = remainder
    code_printf(parser, "    addb $48, %%dl\n");  // convert to ASCII
    code_printf(parser, "    decq %%r12\n");
    code_printf(parser, "    movb %%dl, (%%r12)\n");
    code_printf(parser, "    movq %%rax, %%r10\n");
    code_printf(parser, "    testq %%r10, %%r10\n");
    code_printf(parser, "    jne .Lint2str_loop_%d\n", label_num);
    
    // Add minus sign if negative
    code_printf(parser, "    testq %%r11, %%r11\n");
    code_printf(parser, "    je .Lint2str_no_sign_%d\n", label_num);
    code_printf(parser, "    decq %%r12\n");
    code_printf(parser, "    movb $45, (%%r12)\n");  // '-'
    
    code_printf(parser, ".Lint2str_no_sign_%d:\n", label_num);
    // Move result to beginning of buffer
    code_printf(parser, "    movq %s, %%rdi\n", buffer_reg);
    code_printf(parser, "    movq %%r12, %%rsi\n");
    code_printf(parser, ".Lint2str_copy_%d:\n", label_num);
    code_printf(parser, "    movb (%%rsi), %%al\n");
    code_printf(parser, "    movb %%al, (%%rdi)\n");
    code_printf(parser, "    testb %%al, %%al\n");
    code_printf(parser, "    je .Lint2str_done_%d\n", label_num);
    code_printf(parser, "    incq %%rsi\n");
    code_printf(parser, "    incq %%rdi\n");
    code_printf(parser, "    jmp .Lint2str_copy_%d\n", label_num);
    
    code_printf(parser, ".Lint2str_done_%d:\n", label_num);
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

// Load source file for error reporting
void parser_load_source(Parser *parser, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        return;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Read file content
    parser->source_content = malloc(size + 1);
    if (!parser->source_content) {
        fclose(file);
        return;
    }
    
    size_t bytes_read = fread(parser->source_content, 1, size, file);
    parser->source_content[bytes_read] = '\0';
    fclose(file);
    
    // Count lines
    int line_count = 1;
    for (size_t i = 0; i < bytes_read; i++) {
        if (parser->source_content[i] == '\n') {
            line_count++;
        }
    }
    
    // Allocate line pointer array
    parser->source_lines = malloc(sizeof(char*) * line_count);
    if (!parser->source_lines) {
        return;
    }
    
    // Build line pointer array
    parser->source_lines[0] = parser->source_content;
    parser->source_line_count = 1;
    
    for (size_t i = 0; i < bytes_read; i++) {
        if (parser->source_content[i] == '\n') {
            parser->source_content[i] = '\0';  // Null terminate each line
            if (i + 1 < bytes_read) {
                parser->source_lines[parser->source_line_count++] = &parser->source_content[i + 1];
            }
        }
    }
    
    parser->source_filename = filename;
}

// Get source line by line number (1-indexed)
const char *parser_get_source_line(Parser *parser, int line) {
    if (!parser->source_lines || line < 1 || line > parser->source_line_count) {
        return NULL;
    }
    return parser->source_lines[line - 1];
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

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

void parse_printline_statement(Parser *parser);

// ============================================================================
// ERROR HANDLING
// ============================================================================

void parser_error_code(Parser *parser, int error_code, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    // Format the error message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Use the new error handler if available
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
            // Add source line context
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            // Add token information
            char token_info[512];
            snprintf(token_info, sizeof(token_info), "%s '%s'",
                    token_type_to_string(current.type), current.value);
            error_context_set_token(ctx, token_info);
            
            // Report the error
            error_report_context(global_error_handler, ctx);
            
            // Free the context only if not buffered
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        // Fallback to old error reporting if error handler not initialized
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", current.line, current.column, message);
        fprintf(stderr, "  At token: %s '%s'\n",
                token_type_to_string(current.type), current.value);
    }

    if (!is_at_end(parser->tokens)) {
        consume(parser->tokens);
    }
}

// Default parser_error uses UNEXPECTED_TOKEN code
void parser_error(Parser *parser, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    // Format the error message  
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Use the new error handler if available
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
            // Add source line context
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            // Add token information
            char token_info[512];
            snprintf(token_info, sizeof(token_info), "%s '%s'",
                    token_type_to_string(current.type), current.value);
            error_context_set_token(ctx, token_info);
            
            // Report the error
            error_report_context(global_error_handler, ctx);
            
            // Free the context only if not buffered
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        // Fallback to old error reporting if error handler not initialized
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
        // Determine error code based on expected token type
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

// ============================================================================
// SEMANTIC ERROR HELPERS
// ============================================================================

void semantic_error(Parser *parser, int error_code, const char *format, ...) {
    parser->has_error = 1;

    Token current = peek(parser->tokens);
    
    // Format the error message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Use the new error handler if available
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
            // Add source line context
            const char *source_line = parser_get_source_line(parser, current.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            // Report the error
            error_report_context(global_error_handler, ctx);
            
            // Free the context only if not buffered
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", current.line, current.column, message);
    }

    // Note: semantic errors don't consume tokens - they're not syntax errors
    // The parser can continue normally
}

// Semantic error with specific token (for better error position)
void semantic_error_at_token(Parser *parser, Token token, int error_code, const char *format, ...) {
    parser->has_error = 1;
    
    // Format the error message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Use the new error handler if available
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
            // Add source line context
            const char *source_line = parser_get_source_line(parser, token.line);
            if (source_line) {
                error_context_set_source_line(ctx, source_line);
            }
            
            // Add token value for better underlining
            error_context_set_token(ctx, token.value);
            
            // Report the error
            error_report_context(global_error_handler, ctx);
            
            // Free the context only if not buffered
            if (!global_error_handler->buffered) {
                error_context_free(ctx);
            }
        }
    } else {
        fprintf(stderr, "[ERROR] Line %d, Col %d: %s\n", token.line, token.column, message);
    }

    // Note: semantic errors don't consume tokens - they're not syntax errors
    // The parser can continue normally
}

// ============================================================================
// ERROR RECOVERY / SYNCHRONIZATION
// ============================================================================

// Synchronize to recover from errors (panic mode error recovery)
// This function skips tokens until we reach a statement boundary or safe recovery point
static void synchronize(Parser *parser) {
    // Skip tokens until we find a safe recovery point
    while (!is_at_end(parser->tokens)) {
        Token current = peek(parser->tokens);
        
        // If we're at a semicolon, consume it and we're done
        if (current.type == TOKEN_SEMICOLON) {
            consume(parser->tokens);
            return;
        }
        
        // If we're at a statement keyword or closing brace, stop (don't consume)
        // These are good recovery points
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
        
        // Otherwise, skip this token and continue
        consume(parser->tokens);
    }
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
// IMPORT HANDLING
// ============================================================================

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
    
    // If import_file is an absolute path, use it directly
    if (import_file[0] == '/' || (import_file[0] != '\0' && import_file[1] == ':')) {
        strncpy(resolved_path, import_file, MAX_PATH - 1);
        resolved_path[MAX_PATH - 1] = '\0';
        return resolved_path;
    }
    
    // Try 1: Relative to current working directory (project root)
    strncpy(temp_path, import_file, MAX_PATH - 1);
    temp_path[MAX_PATH - 1] = '\0';
    if (file_exists(temp_path)) {
        strncpy(resolved_path, temp_path, MAX_PATH - 1);
        resolved_path[MAX_PATH - 1] = '\0';
        return resolved_path;
    }
    
    // Try 2: Relative to the source file's directory
    if (base_path && base_path[0] != '\0') {
        // Find the directory part of base_path
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
    
    // Fallback: use import_file as-is
    strncpy(resolved_path, import_file, MAX_PATH - 1);
    resolved_path[MAX_PATH - 1] = '\0';
    return resolved_path;
}

void parse_import(Parser *parser, const char *base_path) {
    Token import_token = consume(parser->tokens);  // consume '#'
    
    if (!match(parser->tokens, TOKEN_KEYWORD_IMPORT)) {
        parser_error(parser, "Expected 'import' after '#'");
        return;
    }
    
    Token filename_token = peek(parser->tokens);
    if (filename_token.type == TOKEN_LESS) {
        // #import <filename>
        consume(parser->tokens);  // consume '<'
        
        // Build the full path from tokens until we hit '>'
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
        
        // Resolve the path
        char *resolved_path = resolve_import_path(base_path, import_filename);
        
        // Check for duplicate imports
        if (is_already_imported(parser, resolved_path)) {
            if (parser->debug_mode) {
                printf("[INFO] Skipping already imported file: %s\n", resolved_path);
            }
            return;
        }
        
        // Add to imported files
        add_imported_file(parser, resolved_path);
        
        if (parser->debug_mode) {
            printf("[INFO] Importing file: %s\n", resolved_path);
        }
        
        // Tokenize and parse the imported file
        TokenStream *imported_stream = tokenize_file(resolved_path, parser->debug_mode);
        if (!imported_stream) {
            parser_error(parser, "Failed to open import file: %s", resolved_path);
            return;
        }
        
        // Save current token stream
        TokenStream *original_stream = parser->tokens;
        int original_pos = parser->tokens->current;
        
        // Switch to imported file's token stream
        parser->tokens = imported_stream;
        
        // Parse imported file (only functions and structs, no main required)
        while (!is_at_end(parser->tokens)) {
            if (check(parser->tokens, TOKEN_HASH)) {
                // Handle nested imports
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
        
        // Restore original token stream
        free_token_stream(imported_stream);
        parser->tokens = original_stream;
        
        if (parser->debug_mode) {
            printf("[INFO] Import complete: %s\n", resolved_path);
        }
    } else if (filename_token.type == TOKEN_STRING_LITERAL) {
        // #import "filename" - alternative syntax
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

// ============================================================================
// PROGRAM PARSING
// ============================================================================

int parse_program(Parser *parser, const char *source_file) {
    // Load source file for error reporting
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

// ============================================================================
// STRUCT PARSING
// ============================================================================

void parse_struct(Parser *parser) {
    Token struct_token = peek(parser->tokens);
    consume(parser->tokens);  // consume 'struct'

    Token name_token = consume(parser->tokens);
    char struct_name[MAX_TOKEN];
    strcpy(struct_name, name_token.value);

    if (parser->struct_count >= 50) {
        parser_error(parser, "Too many structs (max 50)");
        return;
    }

    // Check if struct already exists
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

    // Parse struct body (fields and methods)
    while (!check(parser->tokens, TOKEN_RBRACE) && !is_at_end(parser->tokens)) {
        // Check for static methods or regular methods
        int is_static = 0;
        if (check(parser->tokens, TOKEN_KEYWORD_STATIC)) {
            is_static = 1;
            consume(parser->tokens);  // consume 'static'
        }
        
        // Check if this is a method (func keyword) or a field
        if (check(parser->tokens, TOKEN_KEYWORD_FUNC)) {
            // Parse method - it's like a regular function but belongs to the struct
            consume(parser->tokens);  // consume 'func'
            
            Token method_name_token = consume(parser->tokens);
            char method_name[MAX_TOKEN];
            strcpy(method_name, method_name_token.value);

            if (parser->function_count >= MAX_FUNCTIONS) {
                parser_error(parser, "Too many functions (max %d)", MAX_FUNCTIONS);
                return;
            }

            // Create mangled function name: StructName_methodName
            char mangled_name[MAX_TOKEN * 2];
            snprintf(mangled_name, sizeof(mangled_name), "%s_%s", struct_name, method_name);

            Function *func = &parser->functions[parser->function_count];
            strcpy(func->name, mangled_name);
            strcpy(func->struct_name, struct_name);
            func->param_count = 0;
            func->is_static = is_static;

            // Add to struct's method list
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

            // For static methods, don't add implicit 'this' parameter
            Variable *this_var = NULL;
            if (!is_static) {
                // First implicit parameter is 'this' pointer to struct instance
                if (parser->var_count >= MAX_VARS) {
                    parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
                    return;
                }

                this_var = &parser->vars[parser->var_count];
                strcpy(this_var->name, "this");
                strcpy(this_var->struct_type, struct_name);
                this_var->type = TYPE_INT;  // Pointer type (we don't have a separate pointer type)
                this_var->size = 8;  // Pointer is 8 bytes
                this_var->offset = -param_offset;
                this_var->is_array = 0;
                this_var->array_size = 0;
                this_var->scope = 1;
                parser->var_count++;
                param_offset += 8;
            }

            // Parse remaining parameters
            while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                Token param_token = consume(parser->tokens);

                if (func->param_count >= 9) {  // 9 because first is implicit 'this'
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

            // Generate method code
            code_comment(parser, "========================================");
            code_comment(parser, "Method: %s::%s (Line %d)", struct_name, method_name, method_name_token.line);
            code_comment(parser, "Mangled name: %s", mangled_name);
            code_comment(parser, "Parameters: %d (+ implicit this), Return: %s",
                         func->param_count,
                         datatype_to_string(func->return_type));
            code_comment(parser, "========================================");

            code_printf(parser, ".globl %s\n", mangled_name);
            code_printf(parser, "%s:\n", mangled_name);

            code_comment(parser, "Function prologue");
            code_printf(parser, "    pushq %%rbp\n");
            code_printf(parser, "    movq %%rsp, %%rbp\n");
            code_printf(parser, "    subq $8192, %%rsp\n");

            const char **param_regs_64 = get_arg_registers_64();
            const char **param_regs_32 = get_arg_registers_32();
            const char *param_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

            if (!is_static) {
                code_comment(parser, "Save implicit 'this' pointer and parameters to stack");
                // Save 'this' pointer (first parameter)
                Variable *this_param = &parser->vars[saved_var_count];
                code_printf(parser, "    movq %s, %d(%%rbp)    # Save 'this' pointer\n", 
                            param_regs_64[0], this_param->offset);
            } else {
                code_comment(parser, "Save parameters to stack (static method - no 'this')");
            }

            // Save remaining parameters
            int max_reg_args = get_max_reg_args();
            int param_start_idx = is_static ? 0 : 1;  // Static methods start at var index 0, non-static at 1
            for (int i = 0; i < func->param_count && i + (is_static ? 0 : 1) < max_reg_args; i++) {
                Variable *var = &parser->vars[saved_var_count + param_start_idx + i];
                int reg_idx = is_static ? i : (i + 1);  // Offset by 1 for non-static (first reg has 'this')

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
                code_printf(parser, "    leave\n");
                code_printf(parser, "    ret\n");
            }

            code_printf(parser, "\n");

        } else {
            // Parse field declaration: ONLY var name:type; syntax allowed
            Token first_token = consume(parser->tokens);
            
            // Struct fields MUST start with 'var' keyword
            if (first_token.type != TOKEN_KEYWORD_VAR) {
                parser_error(parser, "Struct fields must use 'var name:type' syntax");
                return;
            }
            
            DataType field_type = TYPE_UNKNOWN;
            char field_name[MAX_TOKEN];
            int is_array = 0;
            int array_size = 0;
            char field_struct_type[MAX_TOKEN] = "";
            
            // var name:type; style or var name:type[size];
            Token name_token = consume(parser->tokens);
            strcpy(field_name, name_token.value);
            
            expect(parser, TOKEN_COLON, "Expected ':' after field name");
            
            Token type_token = consume(parser->tokens);
            field_type = token_to_datatype(type_token.type);
            
            if (field_type == TYPE_UNKNOWN) {
                // Check if it's a struct type
                StructDefinition *field_struct_def = find_struct(parser, type_token.value);
                if (field_struct_def) {
                    // It's a nested struct
                    field_type = TYPE_INT;  // Use TYPE_INT as placeholder for struct types
                    strcpy(field_struct_type, type_token.value);
                } else {
                    parser_error(parser, "Unknown field type '%s'", type_token.value);
                    return;
                }
            }
            
            // Check for array syntax
            if (check(parser->tokens, TOKEN_LBRACKET)) {
                consume(parser->tokens);  // consume '['
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
            
            // Calculate field size
            int element_size;
            if (field_struct_type[0] != '\0') {
                // Nested struct field
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
                // Array field: size is element_size * array_size
                field->size = element_size * array_size;
            } else {
                field->size = element_size;
            }
            
            field->offset = current_offset;
            struct_def->field_count++;

            current_offset += field->size;
            // Align based on field size (8-byte types need 8-byte alignment)
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

// ============================================================================
// ENUM PARSING
// ============================================================================

void parse_enum(Parser *parser) {
    Token enum_token = peek(parser->tokens);
    consume(parser->tokens);  // consume 'enum'
    
    Token name_token = consume(parser->tokens);
    char enum_name[MAX_TOKEN];
    strcpy(enum_name, name_token.value);
    
    if (parser->enum_count >= 50) {
        parser_error(parser, "Too many enums (max 50)");
        return;
    }
    
    // Check if enum already exists
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
    
    // Parse field definitions (id:int, name:string)
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
            // Check if it's a struct type
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                field_type = TYPE_INT;  // Structs stored as data
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
        // For enums, all fields are stored as 8-byte values (quads) for consistency
        field->size = 8;
        
        current_offset += field->size;
        enum_def->field_count++;
        
        if (check(parser->tokens, TOKEN_COMMA)) {
            consume(parser->tokens);
        }
    }
    
    expect(parser, TOKEN_RPAREN, "Expected ')' after enum fields");
    expect(parser, TOKEN_LBRACE, "Expected '{' after enum declaration");
    
    // Now create a struct for this enum type
    if (parser->struct_count >= 50) {
        parser_error(parser, "Too many structs (max 50)");
        return;
    }
    
    StructDefinition *backing_struct = &parser->structs[parser->struct_count];
    strcpy(backing_struct->name, enum_name);
    backing_struct->field_count = enum_def->field_count;
    backing_struct->method_count = 0;
    backing_struct->total_size = current_offset;
    
    // Copy fields from enum to struct
    for (int i = 0; i < enum_def->field_count; i++) {
        backing_struct->fields[i] = enum_def->fields[i];
    }
    
    // Align total size to 8 bytes
    if (backing_struct->total_size % 8 != 0) {
        backing_struct->total_size = ((backing_struct->total_size + 7) / 8) * 8;
    }
    
    enum_def->struct_index = parser->struct_count;
    parser->struct_count++;
    
    // Parse enum values (In(1, "test"), Out(2, "test2"))
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
        
        // Parse value arguments
        while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
            Token value_token = peek(parser->tokens);
            
            if (enum_value->field_count >= 50) {
                parser_error(parser, "Too many fields in enum value");
                return;
            }
            
            // Store the value as a string for later initialization
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
    
    // Generate assembly for enum value instances
    for (int i = 0; i < enum_def->value_count; i++) {
        EnumValue *enum_value = &enum_def->values[i];
        char global_name[MAX_TOKEN * 2];
        snprintf(global_name, sizeof(global_name), "%s_%s", enum_name, enum_value->name);
        
        // Create a global variable for this enum value
        if (parser->var_count >= MAX_VARS) {
            parser_error_code(parser, ERR_CODEGEN_TOO_MANY_VARIABLES, "Too many variables");
            return;
        }
        
        Variable *var = &parser->vars[parser->var_count];
        strcpy(var->name, global_name);
        strcpy(var->struct_type, enum_name);
        var->type = TYPE_INT;  // It's a struct type
        var->size = backing_struct->total_size;
        var->offset = 0;  // Global variable
        var->is_array = 0;
        var->array_size = 0;
        var->scope = 0;  // Global scope
        parser->var_count++;
        
        // Generate data section for this global
        data_printf(parser, "    .data\n");
        data_printf(parser, "    .align 8\n");
        data_printf(parser, "%s:\n", global_name);
        
        // Initialize each field
        for (int j = 0; j < enum_def->field_count; j++) {
            StructField *field = &enum_def->fields[j];
            const char *value_str = enum_value->values[j];
            
            if (field->type == TYPE_INT) {
                data_printf(parser, "    .quad %s\n", value_str);
            } else if (field->type == TYPE_STRING) {
                // Add string literal
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
    
    // Comments for enum go to code section for documentation
    code_comment(parser, "Enum %s defined with %d values", enum_name, enum_def->value_count);
    data_printf(parser, "\n");
}

// ============================================================================
// FUNCTION PARSING
// ============================================================================

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

        // Check for array parameter: param[]:type
        int is_array_param = 0;
        if (check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);
            is_array_param = 1;
            expect(parser, TOKEN_RBRACKET, "Expected ']' in array parameter");
        }

        expect(parser, TOKEN_COLON, "Expected ':' after parameter");

        // Check for pointer parameter: :*type
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
        var->array_size = 0;  // Unknown size for array parameters
        
        // Array parameters and pointer parameters are passed as pointers (8 bytes)
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
    
    // Check for array return type: -> int[]
    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        func->return_is_array = 1;
        expect(parser, TOKEN_RBRACKET, "Expected ']' after array return type");
    } else {
        func->return_is_array = 0;
    }

    // Check for array return type: -> int[]
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

    const char **param_regs_64 = get_arg_registers_64();
    const char **param_regs_32 = get_arg_registers_32();
    const char *param_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};

    for (int i = 0; i < func->param_count && i < 4; i++) {
        Variable *var = &parser->vars[saved_var_count + i];

        // Array parameters and pointer parameters are passed as pointers (8 bytes)
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
    if (check(parser->tokens, TOKEN_AT)) {
        // Handle @gc annotation
        consume(parser->tokens);
        expect(parser, TOKEN_KEYWORD_GC, "Expected 'gc' after '@'");
        parser->next_var_is_gc = 1;
        if (!check(parser->tokens, TOKEN_KEYWORD_VAR)) {
            parser_error(parser, "Expected 'var' after '@gc'");
            return;
        }
        parse_variable_declaration(parser);
        parser->next_var_is_gc = 0;  // Reset flag after declaration
    } else if (check(parser->tokens, TOKEN_KEYWORD_VAR)) {
        parse_variable_declaration(parser);
    } else if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token lookahead = peek_ahead(parser->tokens, 1);
        if (lookahead.type == TOKEN_DOT) {
            // Could be field assignment (p.x = 10) or method call (p.move())
            Token lookahead2 = peek_ahead(parser->tokens, 2);  // Get the member name
            Token lookahead3 = peek_ahead(parser->tokens, 3);  // Check what follows
            if (lookahead3.type == TOKEN_LPAREN) {
                // Method call: p.method()
                parse_function_call_statement(parser);
            } else {
                // Field assignment: p.field = value
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
            parser_error_code(parser, ERR_PARSE_INVALID_SYNTAX, "Unexpected token: expected operator, semicolon, or end of statement");
            synchronize(parser);  // Skip to statement boundary
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
        // Handle free(ptr) as a statement
        Token free_token = consume(parser->tokens);  // consume 'free'
        expect(parser, TOKEN_LPAREN, "Expected '(' after free");
        
        // Get the variable name
        Token var_token = consume(parser->tokens);
        if (var_token.type != TOKEN_IDENTIFIER) {
            parser_error(parser, "Expected variable name in free()");
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        // Find the variable
        Variable *var = find_variable(parser, var_token.value);
        if (var == NULL) {
            semantic_error_at_token(parser, var_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", var_token.value);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        if (!var->is_pointer) {
            parser_error(parser, "Variable '%s' is not a pointer", var_token.value);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        expect(parser, TOKEN_RPAREN, "Expected ')' after pointer in free()");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after free statement");
        
        // Generate code to call free
        code_comment(parser, "Statement: free(%s) at line %d", var_token.value, free_token.line);
        const char **arg_regs = get_arg_registers_64();
        // Load the pointer value (64-bit) from the variable's stack location
        code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, arg_regs[0]);
        int stack_adj = get_call_stack_space();
        if (stack_adj > 0) {
            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj);
        }
        code_printf(parser, "    call free\n");
        if (stack_adj > 0) {
            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj);
        }
    } else if (check(parser->tokens, TOKEN_STAR)) {
        // Handle pointer dereference assignment: *ptr = value
        Token star_token = consume(parser->tokens);  // consume '*'
        
        if (!check(parser->tokens, TOKEN_IDENTIFIER)) {
            parser_error(parser, "Expected identifier after '*' in assignment");
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        Token ptr_token = consume(parser->tokens);
        Variable *ptr_var = find_variable(parser, ptr_token.value);
        
        if (!ptr_var) {
            semantic_error_at_token(parser, ptr_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", ptr_token.value);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        if (!ptr_var->is_pointer) {
            parser_error(parser, "Variable '%s' is not a pointer", ptr_token.value);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        expect(parser, TOKEN_EQUAL, "Expected '=' after pointer dereference");
        
        code_comment(parser, "Line %d: *%s = value (pointer dereference assignment)",
                     star_token.line, ptr_token.value);
        
        // Parse the value expression
        parse_expression(parser);
        
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after pointer dereference assignment");
        
        // Stack has: [value]
        // Load the pointer address
        code_printf(parser, "    movq %d(%%rbp), %%rbx\n", ptr_var->offset);  // Load pointer address
        code_printf(parser, "    popq %%rax\n");  // Get value to store
        
        // Store value at the address pointed to by the pointer
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
        } else {  // TYPE_INT
            code_printf(parser, "    movl %%eax, (%%rbx)\n");
        }
    } else {
        parser_error_code(parser, ERR_PARSE_INVALID_SYNTAX, "Unexpected statement: expected expression or statement keyword");
        synchronize(parser);  // Skip to statement boundary
    }
}

void parse_variable_declaration(Parser *parser) {
    Token var_token = peek(parser->tokens);
    consume(parser->tokens);
    
    // Check for array syntax: var[size] or var[]
    int is_array = 0;
    int array_size = 0;
    
    if (check(parser->tokens, TOKEN_LBRACKET)) {
        consume(parser->tokens);
        is_array = 1;
        
        // Check if size is specified
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

        // Check for pointer syntax: :*type
        if (check(parser->tokens, TOKEN_STAR)) {
            consume(parser->tokens);
            is_pointer = 1;
        }

        Token type_token = consume(parser->tokens);
        var_type = token_to_datatype(type_token.type);

        if (var_type == TYPE_UNKNOWN) {
            // Check if it's a struct type
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                var_type = TYPE_INT;  // We'll use TYPE_INT as a placeholder for struct types
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

    // Arrays must have explicit types
    if (is_array && !has_explicit_type) {
        parser_error(parser, "Arrays must have explicit type specified");
        return;
    }

    // Arrays cannot be initialized with = syntax (for now)
    if (is_array && check(parser->tokens, TOKEN_EQUAL)) {
        parser_error(parser, "Array initialization with '=' not supported. Use assignment to elements.");
        return;
    }

    if (!is_array && check(parser->tokens, TOKEN_EQUAL)) {
        consume(parser->tokens);
        has_initialization = 1;

        // NEW: Auto-detect type from literal if no explicit type given
        if (!has_explicit_type) {
            if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
                var_type = TYPE_FLOAT;  // Auto-detect float
            } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
                var_type = TYPE_CHAR;  // Auto-detect char
            } else if (check(parser->tokens, TOKEN_STRING_LITERAL)) {
                var_type = TYPE_STRING;  // Auto-detect string
            }
            // else: keep TYPE_INT for numbers
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
            // Struct variable - space already allocated in stack frame by subq
            // No code generation needed - just track the space in offset calculation
            StructDefinition *struct_def = find_struct(parser, struct_type_name);
            code_comment(parser, "Line %d: var %s:%s (struct, space reserved in stack frame)",
                         var_token.line, name_token.value, struct_type_name);
            // No actual code generation - the offset calculation will handle placement
        } else {
            code_comment(parser, "Line %d: var %s:%s (uninitialized)",
                         var_token.line, name_token.value, datatype_to_string(var_type));
            // No code generation for uninitialized variables - space is in stack frame
        }
    } else {
        // Array declaration - just allocate space, no initialization
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
    var->is_heap = 0;  // Will be set to 1 when allocated with reserve

    // Calculate total size: element_size * array_count
    int element_size;
    if (struct_type_name[0] != '\0') {
        // This is a struct variable
        StructDefinition *struct_def = find_struct(parser, struct_type_name);
        element_size = struct_def->total_size;
    } else if (is_pointer) {
        // Pointers are 8 bytes (64-bit addresses)
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

    // Offset calculation (unchanged)
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

    // Code generation - only if there was initialization
    if (has_initialization) {
        if (is_array) {
            code_comment(parser, "Array allocated at offset %d, total size %d bytes", 
                         var->offset, var->size);
        } else if (is_pointer) {
            // Pointers are 8-byte addresses (including pointers to structs)
            code_printf(parser, "    popq %%rax\n");
            code_printf(parser, "    movq %%rax, %d(%%rbp)\n", var->offset);
        } else if (struct_type_name[0] != '\0') {
            // Struct variable (not pointer) - space already allocated by pushes, no need to pop/move
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

    // If variable not found and we're in a method context, check if it's a field
    if (!var && parser->current_struct_context[0] != '\0') {
        // Look for 'this' pointer
        Variable *this_var = find_variable(parser, "this");
        if (this_var && this_var->struct_type[0] != '\0') {
            StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
            if (struct_def) {
                // Check if name is a field
                StructField *field = NULL;
                for (int i = 0; i < struct_def->field_count; i++) {
                    if (strcmp(struct_def->fields[i].name, name_token.value) == 0) {
                        field = &struct_def->fields[i];
                        break;
                    }
                }
                
                if (field) {
                    // Check if this is an array field assignment
                    if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                        consume(parser->tokens);  // consume '['
                        
                        code_comment(parser, "Line %d: %s[index] = value (array field through implicit 'this')",
                                     assign_token.line, name_token.value);
                        
                        // Parse the index expression
                        parse_expression(parser);
                        
                        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                        expect(parser, TOKEN_EQUAL, "Expected '=' in array field assignment");
                        
                        // Parse the value to assign
                        parse_expression(parser);
                        
                        // Stack has: [value, index]
                        code_printf(parser, "    popq %%rcx\n");  // value
                        code_printf(parser, "    popq %%rax\n");  // index
                        
                        // Load 'this' pointer
                        code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                        
                        // Calculate array element address: base + field_offset + (index * element_size)
                        int element_size = datatype_size(field->type);
                        code_printf(parser, "    imulq $%d, %%rax\n", element_size);  // index * element_size
                        code_printf(parser, "    addq $%d, %%rax\n", field->offset);  // + field_offset
                        code_printf(parser, "    addq %%rbx, %%rax\n");  // + base address
                        
                        // Store value at calculated address
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
                        // Regular field assignment
                        expect(parser, TOKEN_EQUAL, "Expected '=' in field assignment");
                        
                        code_comment(parser, "Line %d: %s = value (field through implicit 'this')",
                                     assign_token.line, name_token.value);
                        
                        // Parse the value to assign
                        parse_expression(parser);
                        
                        // Load 'this' pointer
                        code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                        
                        // Pop value and store it in the field
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
        semantic_error_at_token(parser, name_token, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name_token.value);
        synchronize(parser);  // Skip to statement boundary
        return;
    }

    // Check for field assignment: var.field = value
    if (check(parser->tokens, TOKEN_DOT)) {
        consume(parser->tokens);  // consume '.'
        
        if (var->struct_type[0] == '\0') {
            parser_error(parser, "Variable '%s' is not a struct", name_token.value);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        StructDefinition *struct_def = find_struct(parser, var->struct_type);
        if (!struct_def) {
            semantic_error(parser, ERR_SEM_UNDEFINED_STRUCT, "Struct type '%s' not found", var->struct_type);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        Token field_name_token = consume(parser->tokens);
        
        // Find the field
        StructField *field = NULL;
        for (int i = 0; i < struct_def->field_count; i++) {
            if (strcmp(struct_def->fields[i].name, field_name_token.value) == 0) {
                field = &struct_def->fields[i];
                break;
            }
        }
        
        if (!field) {
            parser_error(parser, "Field '%s' not found in struct '%s'", field_name_token.value, var->struct_type);
            synchronize(parser);  // Skip to statement boundary
            return;
        }
        
        // Check if this is an array field assignment
        if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);  // consume '['
            
            code_comment(parser, "Line %d: %s.%s[index] = value (array field)",
                         assign_token.line, name_token.value, field_name_token.value);
            
            // Parse the index expression
            parse_expression(parser);
            
            expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
            expect(parser, TOKEN_EQUAL, "Expected '=' in array field assignment");
            
            // Parse the value to assign
            parse_expression(parser);
            
            // Stack has: [value, index]
            code_printf(parser, "    popq %%rcx\n");  // value
            code_printf(parser, "    popq %%rax\n");  // index
            
            // Load struct base address into %rbx
            code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
            
            // Calculate array element address: base + field_offset + (index * element_size)
            int element_size = datatype_size(field->type);
            code_printf(parser, "    imulq $%d, %%rax\n", element_size);  // index * element_size
            code_printf(parser, "    addq $%d, %%rax\n", field->offset);  // + field_offset
            code_printf(parser, "    addq %%rbx, %%rax\n");  // + base address
            
            // Store value at calculated address
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
            // Regular field assignment or chained field access
            // Check if this is a nested struct field with more dot access
            if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                // Chained field assignment (e.g., rect.topLeft.x = 10)
                code_comment(parser, "Line %d: %s.%s. ... = value (chained)",
                             assign_token.line, name_token.value, field_name_token.value);
                
                // Load struct base address and add first field offset
                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                code_printf(parser, "    addq $%d, %%rbx\n", field->offset);
                
                // Continue with chained access
                StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                if (!nested_struct_def) {
                    parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                    synchronize(parser);  // Skip to statement boundary
                    return;
                }
                
                StructField *final_field = NULL;
                
                // Loop to handle chained field access
                while (check(parser->tokens, TOKEN_DOT)) {
                    consume(parser->tokens);  // consume '.'
                    Token next_field_name = consume(parser->tokens);
                    
                    // Find the field in the nested struct
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
                        synchronize(parser);  // Skip to statement boundary
                        return;
                    }
                    
                    // Check if there's another dot (more chaining)
                    if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                        // Another nested struct - add offset and continue
                        code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                        nested_struct_def = find_struct(parser, nested_field->struct_type);
                        if (!nested_struct_def) {
                            parser_error(parser, "Nested struct type '%s' not found", nested_field->struct_type);
                            synchronize(parser);  // Skip to statement boundary
                            return;
                        }
                    } else {
                        // This is the final field in the chain
                        final_field = nested_field;
                        break;
                    }
                }
                
                if (!final_field) {
                    parser_error(parser, "Invalid chained field access");
                    synchronize(parser);  // Skip to statement boundary
                    return;
                }
                
                expect(parser, TOKEN_EQUAL, "Expected '=' in chained field assignment");
                
                // Parse the value to assign
                parse_expression(parser);
                
                // Pop value and store it in the final field
                if (final_field->type == TYPE_FLOAT || final_field->type == TYPE_DOUBLE) {
                    code_printf(parser, "    popq %%rax\n");
                    code_printf(parser, "    movq %%rax, %%xmm0\n");
                    if (final_field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %%xmm0, %d(%%rbx)\n", final_field->offset);
                    } else {
                        code_printf(parser, "    movsd %%xmm0, %d(%%rbx)\n", final_field->offset);
                    }
                } else if (final_field->type == TYPE_CHAR || final_field->type == TYPE_BYTE || final_field->type == TYPE_BIT) {
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
                // Regular field assignment (no chaining)
                expect(parser, TOKEN_EQUAL, "Expected '=' in field assignment");
                
                code_comment(parser, "Line %d: %s.%s = value",
                             assign_token.line, name_token.value, field_name_token.value);
                
                // Parse the value to assign
                parse_expression(parser);
                
                // Load struct base address into %rbx
                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                
                // Pop value and store it in the field using offset from base address
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

    // Check for array indexing: var[index] = value (including string character assignment)
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
        
        // Parse the index expression
        parse_expression(parser);
        
        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
        expect(parser, TOKEN_EQUAL, "Expected '=' in array assignment");
        
        // Parse the value to assign
        parse_expression(parser);
        
        // Stack now has: [value, index]
        // Pop value into a temp, pop index, compute address, store value
        
        // Pop value into appropriate register
        code_printf(parser, "    popq %%rcx\n");  // value
        code_printf(parser, "    popq %%rax\n");  // index
        
        // Handle string character assignment differently
        if (var->type == TYPE_STRING) {
            // For strings, load the string pointer and write a byte
            code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);  // load string pointer
            code_printf(parser, "    movb %%cl, (%%rbx, %%rax, 1)\n");  // write byte at index
        } else {
            // Handle arrays
            int element_size = datatype_size(var->type);
            
            // For array parameters (pointers), load the pointer first
            if (var->is_array && var->array_size == 0) {
                // Array parameter - it's a pointer, load it
                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);  // load pointer
            } else {
                // Local array - calculate address
                code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);  // base address
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
                } else {  // TYPE_INT
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
    
    // Push loop context for break/continue
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
    
    // Pop loop context
    parser->loop_depth--;
}

// ============================================================================
// BREAK AND CONTINUE STATEMENTS
// ============================================================================

void parse_break_statement(Parser *parser) {
    Token break_token = peek(parser->tokens);
    consume(parser->tokens);
    
    // Check if we're inside a loop
    if (parser->loop_depth == 0) {
        parser_error(parser, "'break' statement not within a loop");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'break'");
        return;
    }
    
    // Get the current loop ID from the loop stack
    int current_loop_id = parser->loop_stack[parser->loop_depth - 1].loop_id;
    
    code_comment(parser, "Break statement (Line %d)", break_token.line);
    code_printf(parser, "    jmp .L_for_end_%d\n", current_loop_id);
    
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'break'");
}

void parse_continue_statement(Parser *parser) {
    Token continue_token = peek(parser->tokens);
    consume(parser->tokens);
    
    // Check if we're inside a loop
    if (parser->loop_depth == 0) {
        parser_error(parser, "'continue' statement not within a loop");
        expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'continue'");
        return;
    }
    
    // Get the current loop ID from the loop stack
    int current_loop_id = parser->loop_stack[parser->loop_depth - 1].loop_id;
    
    code_comment(parser, "Continue statement (Line %d)", continue_token.line);
    code_printf(parser, "    jmp .L_for_increment_%d\n", current_loop_id);
    
    expect(parser, TOKEN_SEMICOLON, "Expected ';' after 'continue'");
}

// ============================================================================
// IF STATEMENT WITH ELSE IF SUPPORT (NEW!)
// ============================================================================

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

    // ========== NEW: ELSE IF SUPPORT ==========
    if (check(parser->tokens, TOKEN_KEYWORD_ELSE)) {
        Token else_token = peek(parser->tokens);
        consume(parser->tokens);

        code_printf(parser, "    jmp .L_endif_%d\n", if_id);
        code_printf(parser, ".L_else_%d:\n", if_id);

        // Check if 'if' follows 'else' (else if chain)
        if (check(parser->tokens, TOKEN_KEYWORD_IF)) {
            code_comment(parser, "Else-If (Line %d)", else_token.line);

            // Recursively parse the if statement (handles else if chains!)
            parse_if_statement(parser);

        } else {
            // Regular else block
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

    code_printf(parser, "    leave\n");
    code_printf(parser, "    ret\n");
}

// ============================================================================
// PRINT STATEMENT
// ============================================================================

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

            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_1 = get_call_stack_space();
                if (stack_adj_1 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_1);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_1 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_1);
                }
            }
        } else if (check(parser->tokens, TOKEN_NUMBER)) {
            Token num = consume(parser->tokens);
            code_printf(parser, "    movl $%s, %s\n", get_arg_registers_32()[1], num.value);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_2 = get_call_stack_space();
                if (stack_adj_2 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_2);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_2 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_2);
                }
            }
        } else if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
            Token num = consume(parser->tokens);
            int float_id = add_float_literal(parser, num.value);

            code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_3 = get_call_stack_space();
                if (stack_adj_3 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_3);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_3 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_3);
                }
            }
        } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
            Token ch = consume(parser->tokens);
            int char_value = (unsigned char) ch.value[0];
            code_printf(parser, "    movb $%d, %%dl\n", char_value);
            code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_4 = get_call_stack_space();
                if (stack_adj_4 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_4);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_4 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_4);
                }
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

                consume(parser->tokens);  // consume '('

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
                int stack_adj_5 = get_call_stack_space();
                if (stack_adj_5 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_5);
                }
                code_printf(parser, "    call %s\n", name.value);
                if (stack_adj_5 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_5);
                }
            }

                // Print the return value
                if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (func->return_type == TYPE_STRING) {
                    code_printf(parser, "    movq %%rax, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else {
                    code_printf(parser, "    movl %%eax, %s\n", get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                }
                {
                int stack_adj_6 = get_call_stack_space();
                if (stack_adj_6 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_6);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_6 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_6);
                }
            }
            } else if (lookahead.type == TOKEN_DOT) {
                // Field access or method call in print: p.x or p.getX()
                // Could also be enum access: EnumName.ValueName.field
                
                // First check if it's an enum - if so, use parse_expression to handle it
                EnumDefinition *enum_def = find_enum(parser, name.value);
                if (enum_def) {
                    // Peek ahead to determine the field type for proper formatting
                    // Format: EnumName.ValueName.field
                    Token dot1 = peek_ahead(parser->tokens, 1);  // Should be '.'
                    Token value_name = peek_ahead(parser->tokens, 2);  // ValueName
                    Token dot2 = peek_ahead(parser->tokens, 3);  // Should be '.'
                    Token field_name = peek_ahead(parser->tokens, 4);  // field
                    
                    // Find the field type
                    DataType field_type = TYPE_INT;  // Default
                    if (dot1.type == TOKEN_DOT && dot2.type == TOKEN_DOT) {
                        for (int i = 0; i < enum_def->field_count; i++) {
                            if (strcmp(enum_def->fields[i].name, field_name.value) == 0) {
                                field_type = enum_def->fields[i].type;
                                break;
                            }
                        }
                    }
                    
                    // Now use parse_expression to handle the full access
                    parse_expression(parser);
                    
                    // The expression result is now on the stack - format based on field type
                    if (field_type == TYPE_STRING) {
                        code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field_type == TYPE_FLOAT || field_type == TYPE_DOUBLE) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movq %%rax, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field_type == TYPE_CHAR) {
                        code_printf(parser, "    popq %%rax\n");
                        code_printf(parser, "    movb %%al, %%dl\n");
                        code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else {
                        // TYPE_INT, TYPE_BYTE, TYPE_BIT and others
                        code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    }
                    
                    {
                        int stack_adj = get_call_stack_space();
                        if (stack_adj > 0) {
                            code_printf(parser, "    subq $%d, %%rsp\n", stack_adj);
                        }
                        code_printf(parser, "    call printf\n");
                        if (stack_adj > 0) {
                            code_printf(parser, "    addq $%d, %%rsp\n", stack_adj);
                        }
                    }
                } else {
                    // Not an enum, treat as variable
                    consume(parser->tokens);  // consume identifier
                    consume(parser->tokens);  // consume '.'
                    
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
                
                // Check if it's a method call
                if (next_token.type == TOKEN_LPAREN) {
                    // Method call
                    consume(parser->tokens);  // consume '('
                    
                    // Find the method
                    char mangled_name[MAX_TOKEN * 2];
                    snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, member_name.value);
                    
                    Function *method = find_function(parser, mangled_name);
                    if (!method) {
                        parser_error(parser, "Method '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Parse method arguments
                    int arg_count = 0;
                    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                        parse_expression(parser);
                        arg_count++;
                        
                        if (check(parser->tokens, TOKEN_COMMA)) {
                            consume(parser->tokens);
                        }
                    }
                    
                    expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                    
                    // Check argument count (param_count already excludes implicit 'this')
                    if (arg_count != method->param_count) {
                        parser_error(parser, "Method '%s' expects %d arguments", member_name.value, method->param_count);
                        return;
                    }
                    
                    // Set up arguments in registers (skipping %rcx which will be 'this')
                    // Note: This only handles up to 3 arguments. Methods with more than 3 args
                    // will need stack-based parameter passing (existing limitation in codebase)
                    const char *arg_regs[] = {"%rdx", "%r8", "%r9"};
                    for (int i = arg_count - 1; i >= 0 && i < 3; i--) {
                        code_printf(parser, "    popq %s\n", arg_regs[i]);
                    }
                    
                    // Load struct address into first register (this pointer)
                    const char **this3_regs_64 = get_arg_registers_64();
                    code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this3_regs_64[0]);
                    
                    // Call method
                    {
                int stack_adj_7 = get_call_stack_space();
                if (stack_adj_7 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_7);
                }
                code_printf(parser, "    call %s\n", mangled_name);
                if (stack_adj_7 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_7);
                }
            }
                    
                    // Print the return value
                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (method->return_type == TYPE_STRING) {
                        code_printf(parser, "    movq %%rax, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else {
                        code_printf(parser, "    movl %%eax, %s\n", get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    }
                    {
                int stack_adj_8 = get_call_stack_space();
                if (stack_adj_8 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_8);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_8 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_8);
                }
            }
                } else {
                    // Field access
                    StructField *field = NULL;
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (strcmp(struct_def->fields[i].name, member_name.value) == 0) {
                            field = &struct_def->fields[i];
                            break;
                        }
                    }
                    
                    if (!field) {
                        parser_error(parser, "Field '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Load struct base address into %rbx
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                    
                    // Access field and print it
                    if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_DOUBLE) {
                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_CHAR) {
                        code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                        code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_STRING) {
                        code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else {  // TYPE_INT and others
                        code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    }
                    {
                int stack_adj_9 = get_call_stack_space();
                if (stack_adj_9 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_9);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_9 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_9);
                }
            }
                }
                }  // Close the else block for non-enum variables
            } else if (lookahead.type == TOKEN_LBRACKET) {
                // Array access in print: arr[index]
                consume(parser->tokens);  // consume identifier
                consume(parser->tokens);  // consume [
                
                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                    return;
                }
                
                // Parse the index expression
                parse_expression(parser);
                
                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                
                // Load array element value
                int element_size = datatype_size(var->type);
                code_printf(parser, "    popq %%rax\n");  // index
                
                // For array parameters (pointers), load the pointer first
                if (var->is_array && var->array_size == 0) {
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
                } else {
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                }
                
                // Load the array element based on type
                if (var->type == TYPE_INT) {
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %s\n", element_size, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else {
                    parser_error(parser, "Unsupported array element type in print");
                    return;
                }
                
                {
                int stack_adj_10 = get_call_stack_space();
                if (stack_adj_10 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_10);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_10 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_10);
                }
            }
            } else {
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                
                // If variable not found and we're in a method context, check if it's a field
                if (!var && parser->current_struct_context[0] != '\0') {
                    // Look for 'this' pointer
                    Variable *this_var = find_variable(parser, "this");
                    if (this_var && this_var->struct_type[0] != '\0') {
                        StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
                        if (struct_def) {
                            // Check if name is a field
                            StructField *field = NULL;
                            for (int i = 0; i < struct_def->field_count; i++) {
                                if (strcmp(struct_def->fields[i].name, name.value) == 0) {
                                    field = &struct_def->fields[i];
                                    break;
                                }
                            }
                            
                            if (field) {
                                // Print field through implicit 'this' pointer
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                                
                                if (field->type == TYPE_FLOAT) {
                                    code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_DOUBLE) {
                                    code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_CHAR) {
                                    code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                                    code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_STRING) {
                                    code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else {
                                    code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
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
                                
                                // Skip the regular variable handling
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
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_11 = get_call_stack_space();
                if (stack_adj_11 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_11);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_11 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_11);
                }
            }
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_12 = get_call_stack_space();
                if (stack_adj_12 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_12);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_12 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_12);
                }
            }
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_13 = get_call_stack_space();
                if (stack_adj_13 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_13);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_13 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_13);
                }
            }
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_14 = get_call_stack_space();
                if (stack_adj_14 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_14);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_14 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_14);
                }
            }
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_15 = get_call_stack_space();
                if (stack_adj_15 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_15);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_15 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_15);
                }
            }
                } else {
                    code_printf(parser, "    movl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_16 = get_call_stack_space();
                if (stack_adj_16 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_16);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_16 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_16);
                }
            }
                }
            }
        } else {
            // Default: parse as expression (handles operators like *, &, etc.)
            parse_expression(parser);
            
            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
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

            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_1 = get_call_stack_space();
                if (stack_adj_1 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_1);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_1 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_1);
                }
            }
        } else if (check(parser->tokens, TOKEN_NUMBER)) {
            Token num = consume(parser->tokens);
            code_printf(parser, "    movl $%s, %s\n", get_arg_registers_32()[1], num.value);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_2 = get_call_stack_space();
                if (stack_adj_2 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_2);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_2 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_2);
                }
            }
        } else if (check(parser->tokens, TOKEN_FLOAT_LITERAL)) {
            Token num = consume(parser->tokens);
            int float_id = add_float_literal(parser, num.value);

            code_printf(parser, "    movss .LC_float_%d(%%rip), %%xmm0\n", float_id);
            code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
            code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_3 = get_call_stack_space();
                if (stack_adj_3 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_3);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_3 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_3);
                }
            }
        } else if (check(parser->tokens, TOKEN_CHAR_LITERAL)) {
            Token ch = consume(parser->tokens);
            int char_value = (unsigned char) ch.value[0];
            code_printf(parser, "    movb $%d, %%dl\n", char_value);
            code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
            {
                int stack_adj_4 = get_call_stack_space();
                if (stack_adj_4 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_4);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_4 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_4);
                }
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

                consume(parser->tokens);  // consume '('

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
                int stack_adj_5 = get_call_stack_space();
                if (stack_adj_5 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_5);
                }
                code_printf(parser, "    call %s\n", name.value);
                if (stack_adj_5 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_5);
                }
            }

                // Print the return value
                if (func->return_type == TYPE_FLOAT || func->return_type == TYPE_DOUBLE) {
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (func->return_type == TYPE_STRING) {
                    code_printf(parser, "    movq %%rax, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else {
                    code_printf(parser, "    movl %%eax, %s\n", get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                }
                {
                int stack_adj_6 = get_call_stack_space();
                if (stack_adj_6 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_6);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_6 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_6);
                }
            }
            } else if (lookahead.type == TOKEN_DOT) {
                // Field access or method call in print: p.x or p.getX()
                consume(parser->tokens);  // consume identifier
                consume(parser->tokens);  // consume '.'
                
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
                
                // Check if it's a method call
                if (next_token.type == TOKEN_LPAREN) {
                    // Method call
                    consume(parser->tokens);  // consume '('
                    
                    // Find the method
                    char mangled_name[MAX_TOKEN * 2];
                    snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, member_name.value);
                    
                    Function *method = find_function(parser, mangled_name);
                    if (!method) {
                        parser_error(parser, "Method '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Parse method arguments
                    int arg_count = 0;
                    while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
                        parse_expression(parser);
                        arg_count++;
                        
                        if (check(parser->tokens, TOKEN_COMMA)) {
                            consume(parser->tokens);
                        }
                    }
                    
                    expect(parser, TOKEN_RPAREN, "Expected ')' after method arguments");
                    
                    // Check argument count (param_count already excludes implicit 'this')
                    if (arg_count != method->param_count) {
                        parser_error(parser, "Method '%s' expects %d arguments", member_name.value, method->param_count);
                        return;
                    }
                    
                    // Set up arguments in registers (skipping %rcx which will be 'this')
                    // Note: This only handles up to 3 arguments. Methods with more than 3 args
                    // will need stack-based parameter passing (existing limitation in codebase)
                    const char *arg_regs[] = {"%rdx", "%r8", "%r9"};
                    for (int i = arg_count - 1; i >= 0 && i < 3; i--) {
                        code_printf(parser, "    popq %s\n", arg_regs[i]);
                    }
                    
                    // Load struct address into first register (this pointer)
                    const char **this3_regs_64 = get_arg_registers_64();
                    code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this3_regs_64[0]);
                    
                    // Call method
                    {
                int stack_adj_7 = get_call_stack_space();
                if (stack_adj_7 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_7);
                }
                code_printf(parser, "    call %s\n", mangled_name);
                if (stack_adj_7 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_7);
                }
            }
                    
                    // Print the return value
                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (method->return_type == TYPE_STRING) {
                        code_printf(parser, "    movq %%rax, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else {
                        code_printf(parser, "    movl %%eax, %s\n", get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    }
                    {
                int stack_adj_8 = get_call_stack_space();
                if (stack_adj_8 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_8);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_8 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_8);
                }
            }
                } else {
                    // Field access
                    StructField *field = NULL;
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (strcmp(struct_def->fields[i].name, member_name.value) == 0) {
                            field = &struct_def->fields[i];
                            break;
                        }
                    }
                    
                    if (!field) {
                        parser_error(parser, "Field '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Load struct base address into %rbx
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                    
                    // Access field and print it
                    if (field->type == TYPE_FLOAT) {
                        code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_DOUBLE) {
                        code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                        code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_CHAR) {
                        code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                        code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else if (field->type == TYPE_STRING) {
                        code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_registers_64()[1]);
                        code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    } else {  // TYPE_INT and others
                        code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                        code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    }
                    {
                int stack_adj_9 = get_call_stack_space();
                if (stack_adj_9 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_9);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_9 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_9);
                }
            }
                }
            } else if (lookahead.type == TOKEN_LBRACKET) {
                // Array access in print: arr[index]
                consume(parser->tokens);  // consume identifier
                consume(parser->tokens);  // consume [
                
                Variable *var = find_variable(parser, name.value);
                if (!var) {
                    semantic_error(parser, ERR_SEM_UNDEFINED_VARIABLE, "Variable '%s' not found", name.value);
                    return;
                }
                
                // Parse the index expression
                parse_expression(parser);
                
                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                
                // Load array element value
                int element_size = datatype_size(var->type);
                code_printf(parser, "    popq %%rax\n");  // index
                
                // For array parameters (pointers), load the pointer first
                if (var->is_array && var->array_size == 0) {
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);
                } else {
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                }
                
                // Load the array element based on type
                if (var->type == TYPE_INT) {
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %s\n", element_size, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl (%%rbx, %%rax, %d), %%edx\n", element_size);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                } else {
                    parser_error(parser, "Unsupported array element type in print");
                    return;
                }
                
                {
                int stack_adj_10 = get_call_stack_space();
                if (stack_adj_10 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_10);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_10 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_10);
                }
            }
            } else {
                consume(parser->tokens);

                Variable *var = find_variable(parser, name.value);
                
                // If variable not found and we're in a method context, check if it's a field
                if (!var && parser->current_struct_context[0] != '\0') {
                    // Look for 'this' pointer
                    Variable *this_var = find_variable(parser, "this");
                    if (this_var && this_var->struct_type[0] != '\0') {
                        StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
                        if (struct_def) {
                            // Check if name is a field
                            StructField *field = NULL;
                            for (int i = 0; i < struct_def->field_count; i++) {
                                if (strcmp(struct_def->fields[i].name, name.value) == 0) {
                                    field = &struct_def->fields[i];
                                    break;
                                }
                            }
                            
                            if (field) {
                                // Print field through implicit 'this' pointer
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                                
                                if (field->type == TYPE_FLOAT) {
                                    code_printf(parser, "    movss %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    cvtss2sd %%xmm0, %%xmm0\n");
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_DOUBLE) {
                                    code_printf(parser, "    movsd %d(%%rbx), %%xmm0\n", field->offset);
                                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_CHAR) {
                                    code_printf(parser, "    movsbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_BYTE || field->type == TYPE_BIT) {
                                    code_printf(parser, "    movzbl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else if (field->type == TYPE_STRING) {
                                    code_printf(parser, "    movq %d(%%rbx), %s\n", field->offset, get_arg_registers_64()[1]);
                                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                                } else {
                                    code_printf(parser, "    movl %d(%%rbx), %s\n", field->offset, get_arg_registers_32()[1]);
                                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
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
                                
                                // Skip the regular variable handling
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
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_11 = get_call_stack_space();
                if (stack_adj_11 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_11);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_11 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_11);
                }
            }
                } else if (var->type == TYPE_DOUBLE) {
                    code_printf(parser, "    movsd %d(%%rbp), %%xmm0\n", var->offset);
                    code_printf(parser, "    movq %%xmm0, %s\n", get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_float_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_12 = get_call_stack_space();
                if (stack_adj_12 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_12);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_12 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_12);
                }
            }
                } else if (var->type == TYPE_CHAR) {
                    code_printf(parser, "    movsbl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_char_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_13 = get_call_stack_space();
                if (stack_adj_13 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_13);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_13 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_13);
                }
            }
                } else if (var->type == TYPE_BYTE || var->type == TYPE_BIT) {
                    code_printf(parser, "    movzbl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_14 = get_call_stack_space();
                if (stack_adj_14 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_14);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_14 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_14);
                }
            }
                } else if (var->type == TYPE_STRING) {
                    code_printf(parser, "    movq %d(%%rbp), %s\n", var->offset, get_arg_registers_64()[1]);
                    code_printf(parser, "    leaq .LC_string_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_15 = get_call_stack_space();
                if (stack_adj_15 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_15);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_15 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_15);
                }
            }
                } else {
                    code_printf(parser, "    movl %d(%%rbp), %s\n", var->offset, get_arg_registers_32()[1]);
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
                    {
                int stack_adj_16 = get_call_stack_space();
                if (stack_adj_16 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_16);
                }
                code_printf(parser, "    call printf\n");
                if (stack_adj_16 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_16);
                }
            }
                }
            }
        } else {
            // Default: parse as expression (handles operators like *, &, etc.)
            parse_expression(parser);
            
            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", get_arg_registers_64()[0]);
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

    // Add newline for println
    code_printf(parser, "    leaq .LC_newline(%%rip), %s\n", get_arg_registers_64()[0]);
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

    // Check if it's a method call: var.method() or StructName.staticMethod()
    if (check(parser->tokens, TOKEN_DOT)) {
        consume(parser->tokens);  // consume '.'
        
        // Try to find as a variable first
        Variable *var = find_variable(parser, name.value);
        
        // If not a variable, check if it's a struct type (for static methods)
        if (!var) {
            StructDefinition *struct_def = find_struct(parser, name.value);
            if (struct_def) {
                // This is a static method call: StructName.methodName()
                Token method_name = consume(parser->tokens);
                
                // Find the static method
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
                
                code_comment(parser, "Line %d: %s.%s(...) (static method call)", call_token.line, name.value, method_name.value);
                
                expect(parser, TOKEN_LPAREN, "Expected '(' after method name");
                
                // Parse arguments (no 'this' pointer for static methods)
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
                
                // Set up arguments in registers (no 'this' pointer offset)
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
                
                // Call the static method
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
        
        // Find the method
        char mangled_name[MAX_TOKEN * 2];
        snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, method_name.value);
        
        Function *method = find_function(parser, mangled_name);
        if (!method) {
            parser_error(parser, "Method '%s' not found in struct '%s'", method_name.value, var->struct_type);
            return;
        }
        
        code_comment(parser, "Line %d: %s.%s(...) (method call)", call_token.line, name.value, method_name.value);
        
        expect(parser, TOKEN_LPAREN, "Expected '(' after method name");
        
        // Parse arguments
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
        
        // Prepare arguments (in reverse order) and implicit 'this' pointer
        const char **arg_regs_64 = get_arg_registers_64();
        const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};
        
        // Pop arguments into registers (in reverse order)
        int max_reg_args = get_max_reg_args();
        for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
            DataType param_type = method->param_types[i];
            int reg_idx = i + 1;  // Offset by 1 because first reg has 'this'
            
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
        
        // Load 'this' pointer (address of struct instance) into first register
        const char **this_regs_64 = get_arg_registers_64();
        code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this_regs_64[0]);  // Address of struct
        
        {
                int stack_adj_18 = get_call_stack_space();
                if (stack_adj_18 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_18);
                }
                code_printf(parser, "    call %s\n", mangled_name);
                if (stack_adj_18 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_18);
                }
            }
        
        return;
    }

    // Check if it's a syscall I/O function (as statement)
    if (is_syscall_io_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '('");
        
        if (strcmp(name.value, "sys_write") == 0) {
            // sys_write(fd, buffer, count) -> int
            code_comment(parser, "Line %d: sys_write(fd, buffer, count)", call_token.line);
            
            parse_expression(parser);  // fd
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // buffer
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // count
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            // Arguments are on stack (top to bottom: count, buffer, fd)
            code_printf(parser, "    popq %%rdx\n");  // count
            code_printf(parser, "    popq %%rsi\n");  // buffer
            code_printf(parser, "    popq %%rdi\n");  // fd
            code_printf(parser, "    movq $1, %%rax\n");  // syscall number: write
            code_printf(parser, "    syscall\n");
            return;
            
        } else if (strcmp(name.value, "sys_read") == 0) {
            // sys_read(fd, buffer, count) -> int
            code_comment(parser, "Line %d: sys_read(fd, buffer, count)", call_token.line);
            
            parse_expression(parser);  // fd
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // buffer
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // count
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            // Arguments are on stack
            code_printf(parser, "    popq %%rdx\n");  // count
            code_printf(parser, "    popq %%rsi\n");  // buffer
            code_printf(parser, "    popq %%rdi\n");  // fd
            code_printf(parser, "    movq $0, %%rax\n");  // syscall number: read
            code_printf(parser, "    syscall\n");
            return;
            
        } else if (strcmp(name.value, "sys_open") == 0) {
            // sys_open(pathname, flags, mode) -> int
            code_comment(parser, "Line %d: sys_open(pathname, flags, mode)", call_token.line);
            
            parse_expression(parser);  // pathname
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // flags
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);  // mode
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            // Arguments are on stack
            code_printf(parser, "    popq %%rdx\n");  // mode
            code_printf(parser, "    popq %%rsi\n");  // flags
            code_printf(parser, "    popq %%rdi\n");  // pathname
            code_printf(parser, "    movq $2, %%rax\n");  // syscall number: open
            code_printf(parser, "    syscall\n");
            return;
            
        } else if (strcmp(name.value, "sys_close") == 0) {
            // sys_close(fd) -> int
            code_comment(parser, "Line %d: sys_close(fd)", call_token.line);
            
            parse_expression(parser);  // fd
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            // Argument is on stack
            code_printf(parser, "    popq %%rdi\n");  // fd
            code_printf(parser, "    movq $3, %%rax\n");  // syscall number: close
            code_printf(parser, "    syscall\n");
            return;
        }
    }

    // Check if it's a built-in string function
    if (is_builtin_string_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");
        
        if (strcmp(name.value, "strlen") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: strlen() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_19 = get_call_stack_space();
                if (stack_adj_19 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_19);
                }
                code_printf(parser, "    call strlen\n");
                if (stack_adj_19 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_19);
                }
            }
            return;
        } else if (strcmp(name.value, "strcmp") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: strcmp() (statement)", call_token.line);
            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_20 = get_call_stack_space();
                if (stack_adj_20 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_20);
                }
                code_printf(parser, "    call strcmp\n");
                if (stack_adj_20 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_20);
                }
            }
            return;
        } else if (strcmp(name.value, "strcpy") == 0 || strcmp(name.value, "strcat") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_COMMA, "Expected ','");
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: %s() (statement)", call_token.line, name.value);
            code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_21 = get_call_stack_space();
                if (stack_adj_21 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_21);
                }
                code_printf(parser, "    call %s\n", name.value);
                if (stack_adj_21 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_21);
                }
            }
            return;
        } else if (strcmp(name.value, "strdup") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: strdup() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_22 = get_call_stack_space();
                if (stack_adj_22 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_22);
                }
                code_printf(parser, "    call strdup\n");
                if (stack_adj_22 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_22);
                }
            }
            return;
        }
    }
    
    // Check if it's a built-in memory function
    if (is_builtin_memory_function(name.value)) {
        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");
        
        if (strcmp(name.value, "malloc") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: malloc() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_23 = get_call_stack_space();
                if (stack_adj_23 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_23);
                }
                code_printf(parser, "    call malloc\n");
                if (stack_adj_23 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_23);
                }
            }
            return;
        } else if (strcmp(name.value, "free") == 0) {
            parse_expression(parser);
            expect(parser, TOKEN_RPAREN, "Expected ')'");
            expect(parser, TOKEN_SEMICOLON, "Expected ';'");
            
            code_comment(parser, "Line %d: free() (statement)", call_token.line);
            code_printf(parser, "    popq %%rcx\n");
            {
                int stack_adj_24 = get_call_stack_space();
                if (stack_adj_24 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_24);
                }
                code_printf(parser, "    call free\n");
                if (stack_adj_24 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_24);
                }
            }
            return;
        }
    }

    // Check if we're in a method context and this might be a method call
    int is_method_call = 0;
    char method_mangled_name[MAX_TOKEN * 2];
    Function *func = find_function(parser, name.value);
    
    if (!func && parser->current_struct_context[0] != '\0') {
        // Try to find it as a method of the current struct
        snprintf(method_mangled_name, sizeof(method_mangled_name), "%s_%s", 
                 parser->current_struct_context, name.value);
        func = find_function(parser, method_mangled_name);
        if (func) {
            is_method_call = 1;
        }
    }
    
    if (!func) {
        semantic_error(parser, ERR_SEM_UNDEFINED_FUNCTION, "Function '%s' not found", name.value);
        
        // Error recovery: skip the function call to avoid cascading errors
        // We still parse the arguments to catch any errors in them
        expect(parser, TOKEN_LPAREN, "Expected '(' after function name");
        
        int arg_count = 0;
        while (!check(parser->tokens, TOKEN_RPAREN) && !is_at_end(parser->tokens)) {
            parse_expression(parser);  // Parse arguments to check for errors in them
            arg_count++;
            if (check(parser->tokens, TOKEN_COMMA)) {
                consume(parser->tokens);
            }
        }
        
        expect(parser, TOKEN_RPAREN, "Expected ')' after function arguments");
        
        // Push a dummy value so expression parsing can continue
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
        // This is a method call on the same struct - need to pass 'this' as first arg
        // Pop arguments into registers (offset by 1 because first reg has 'this')
        int max_reg_args = get_max_reg_args();
        for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
            int reg_idx = i + 1;  // Offset by 1 because first reg has 'this'
            code_printf(parser, "    popq %s\n", arg_regs[reg_idx]);
        }

        // Load 'this' pointer into first register
        Variable *this_var = find_variable(parser, "this");
        if (this_var) {
            code_printf(parser, "    movq %d(%%rbp), %s\n", this_var->offset, arg_regs[0]);
        }

        {
            int stack_adj_25 = get_call_stack_space();
            if (stack_adj_25 > 0) {
                code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_25);
            }
            code_printf(parser, "    call %s\n", method_mangled_name);
            if (stack_adj_25 > 0) {
                code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_25);
            }
        }
    } else {
        // Regular function call
        for (int i = arg_count - 1; i >= 0 && i < 4; i--) {
            code_printf(parser, "    popq %s\n", arg_regs[i]);
        }

        {
            int stack_adj_25 = get_call_stack_space();
            if (stack_adj_25 > 0) {
                code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_25);
            }
            code_printf(parser, "    call %s\n", name.value);
            if (stack_adj_25 > 0) {
                code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_25);
            }
        }
    }

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

    while (check(parser->tokens, TOKEN_STAR) || check(parser->tokens, TOKEN_SLASH) || check(parser->tokens, TOKEN_PERCENT)) {
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

    // Dereference operator: *pointer
    if (match(parser->tokens, TOKEN_STAR)) {
        code_comment(parser, "Dereference operator (*)");
        parse_unary(parser);  // Parse the pointer expression
        code_printf(parser, "    popq %%rax\n");  // Get the pointer address (64-bit)
        code_printf(parser, "    movl (%%rax), %%eax\n");  // Dereference: load value at address (32-bit for now)
        code_printf(parser, "    pushq %%rax\n");  // Push the dereferenced value
        return;
    }

    // Address-of operator: &variable
    if (match(parser->tokens, TOKEN_AMPERSAND)) {
        code_comment(parser, "Address-of operator (&)");
        // Must be followed by a variable
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
        // Calculate the address of the variable (rbp + offset)
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
                case 'n': char_value = '\n'; break;
                case 't': char_value = '\t'; break;
                case 'r': char_value = '\r'; break;
                case '0': char_value = '\0'; break;
                case '\\': char_value = '\\'; break;
                case '\'': char_value = '\''; break;
                default: char_value = ch.value[1]; break;
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

        // Check for string literal indexing: "hello"[0]
        if (check(parser->tokens, TOKEN_LBRACKET)) {
            consume(parser->tokens);
            
            code_comment(parser, "String literal indexing: \"%s\"[...]", escaped_str);
            
            // Parse the index expression
            parse_expression(parser);
            
            expect(parser, TOKEN_RBRACKET, "Expected ']' after string index");
            
            // Stack has: [index]
            code_printf(parser, "    popq %%rax\n");  // index
            code_printf(parser, "    leaq .LC%d(%%rip), %%rbx\n", str_id);  // base address of string
            code_printf(parser, "    movzbl (%%rbx, %%rax, 1), %%eax\n");  // load character (byte)
            code_printf(parser, "    pushq %%rax\n");
        } else {
            code_comment(parser, "String literal: \"%s\"", escaped_str);
            code_printf(parser, "    leaq .LC%d(%%rip), %%rax\n", str_id);
            code_printf(parser, "    pushq %%rax\n");
        }
        return;
    }

    // Handle reserve() as a special keyword function
    if (check(parser->tokens, TOKEN_KEYWORD_RESERVE)) {
        consume(parser->tokens);
        expect(parser, TOKEN_LPAREN, "Expected '(' after reserve");
        
        // reserve(type) -> pointer
        // Takes a type name and allocates appropriate size
        Token type_token = consume(parser->tokens);
        DataType type = token_to_datatype(type_token.type);
        
        if (type == TYPE_UNKNOWN) {
            // Check if it's a struct type
            StructDefinition *struct_def = find_struct(parser, type_token.value);
            if (struct_def != NULL) {
                // Allocate space for struct
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
        
        // Now call malloc with the size we pushed
        const char **arg_regs = get_arg_registers_64();
        code_printf(parser, "    popq %s\n", arg_regs[0]);  // size (first argument)
        {
            int stack_adj = get_call_stack_space();
            if (stack_adj > 0) {
                code_printf(parser, "    subq $%d, %%rsp\n", stack_adj);
            }
            code_printf(parser, "    call malloc\n");
            if (stack_adj > 0) {
                code_printf(parser, "    addq $%d, %%rsp\n", stack_adj);
            }
        }
        code_printf(parser, "    pushq %%rax\n");  // return pointer
        return;
    }

    if (check(parser->tokens, TOKEN_IDENTIFIER)) {
        Token name = peek(parser->tokens);
        Token lookahead = peek_ahead(parser->tokens, 1);

        if (lookahead.type == TOKEN_LPAREN) {
            consume(parser->tokens);

            // Check if it's a built-in string function
            if (is_builtin_string_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");
                
                // Parse arguments for built-in functions
                if (strcmp(name.value, "strlen") == 0) {
                    // strlen(str) -> int
                    parse_expression(parser);  // String argument
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: strlen()");
                    code_printf(parser, "    popq %%rcx\n");  // string pointer
                    {
                int stack_adj_26 = get_call_stack_space();
                if (stack_adj_26 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_26);
                }
                code_printf(parser, "    call strlen\n");
                if (stack_adj_26 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_26);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // result
                    return;
                } else if (strcmp(name.value, "strcmp") == 0) {
                    // strcmp(str1, str2) -> int
                    parse_expression(parser);  // First string
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcmp");
                    parse_expression(parser);  // Second string
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: strcmp()");
                    code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);  // str2
                    code_printf(parser, "    popq %%rcx\n");  // str1
                    {
                int stack_adj_27 = get_call_stack_space();
                if (stack_adj_27 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_27);
                }
                code_printf(parser, "    call strcmp\n");
                if (stack_adj_27 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_27);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // result
                    return;
                } else if (strcmp(name.value, "strcpy") == 0) {
                    // strcpy(dst, src) -> void (modifies dst)
                    parse_expression(parser);  // Destination
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcpy");
                    parse_expression(parser);  // Source
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: strcpy()");
                    code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);  // src
                    code_printf(parser, "    popq %%rcx\n");  // dst
                    {
                int stack_adj_28 = get_call_stack_space();
                if (stack_adj_28 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_28);
                }
                code_printf(parser, "    call strcpy\n");
                if (stack_adj_28 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_28);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // return dst
                    return;
                } else if (strcmp(name.value, "strcat") == 0) {
                    // strcat(dst, src) -> void (modifies dst)
                    parse_expression(parser);  // Destination
                    expect(parser, TOKEN_COMMA, "Expected ',' in strcat");
                    parse_expression(parser);  // Source
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: strcat()");
                    code_printf(parser, "    popq %s\n", get_arg_registers_64()[1]);  // src
                    code_printf(parser, "    popq %%rcx\n");  // dst
                    {
                int stack_adj_29 = get_call_stack_space();
                if (stack_adj_29 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_29);
                }
                code_printf(parser, "    call strcat\n");
                if (stack_adj_29 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_29);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // return dst
                    return;
                } else if (strcmp(name.value, "strdup") == 0) {
                    // strdup(str) -> string (allocates new string on heap)
                    parse_expression(parser);  // String to duplicate
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: strdup()");
                    code_printf(parser, "    popq %%rcx\n");  // source string
                    {
                int stack_adj_30 = get_call_stack_space();
                if (stack_adj_30 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_30);
                }
                code_printf(parser, "    call strdup\n");
                if (stack_adj_30 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_30);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // return new string
                    return;
                }
            }
            
            // Check if it's a built-in memory function
            if (is_builtin_memory_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");
                
                if (strcmp(name.value, "malloc") == 0) {
                    // malloc(size) -> pointer
                    parse_expression(parser);  // Size in bytes
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: malloc()");
                    code_printf(parser, "    popq %%rcx\n");  // size
                    {
                int stack_adj_31 = get_call_stack_space();
                if (stack_adj_31 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_31);
                }
                code_printf(parser, "    call malloc\n");
                if (stack_adj_31 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_31);
                }
            }
                    code_printf(parser, "    pushq %%rax\n");  // return pointer
                    return;
                } else if (strcmp(name.value, "free") == 0) {
                    // free(ptr) -> void
                    parse_expression(parser);  // Pointer to free
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    code_comment(parser, "Built-in: free()");
                    const char **arg_regs = get_arg_registers_64();
                    code_printf(parser, "    popq %s\n", arg_regs[0]);  // pointer (first argument)
                    {
                int stack_adj_32 = get_call_stack_space();
                if (stack_adj_32 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_32);
                }
                code_printf(parser, "    call free\n");
                if (stack_adj_32 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_32);
                }
            }
                    return;
                }
            }
            
            // Check if it's a built-in input function
            if (is_builtin_input_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");
                expect(parser, TOKEN_RPAREN, "Expected ')' - input functions take no arguments");
                
                if (strcmp(name.value, "scanfInt") == 0) {
                    // scanfInt() -> int (reads int from stdin, returns value)
                    code_comment(parser, "Built-in: scanfInt()");
                    
                    // Allocate space on stack for the input value
                    code_printf(parser, "    subq $16, %%rsp\n");  // Allocate 16 bytes (aligned)
                    code_printf(parser, "    movq %%rsp, %%r15\n");  // Save stack pointer in callee-saved register
                    
                    const char **scanf_regs = get_arg_registers_64();
                    code_printf(parser, "    movq %%r15, %s\n", scanf_regs[1]);  // Address on stack
                    code_printf(parser, "    leaq .LC_int_format(%%rip), %s\n", scanf_regs[0]);  // Format string
                    code_printf(parser, "    xor %%eax, %%eax\n");  // Clear EAX (for variadic functions)
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
                    // Load the value that was read
                    code_printf(parser, "    movl (%%r15), %%eax\n");  // Load int from saved address
                    code_printf(parser, "    addq $16, %%rsp\n");  // Deallocate stack space
                    code_printf(parser, "    cltq\n");  // Sign extend to 64 bits
                    code_printf(parser, "    pushq %%rax\n");  // Return value
                    return;
                } else if (strcmp(name.value, "scanfChar") == 0) {
                    // scanfChar() -> char (reads char from stdin, returns value)
                    code_comment(parser, "Built-in: scanfChar()");
                    
                    // Allocate space on stack for the input value
                    code_printf(parser, "    subq $16, %%rsp\n");  // Allocate 16 bytes (aligned)
                    code_printf(parser, "    movq %%rsp, %%r15\n");  // Save stack pointer in callee-saved register
                    
                    const char **scanf_regs = get_arg_registers_64();
                    code_printf(parser, "    movq %%r15, %s\n", scanf_regs[1]);  // Address on stack
                    code_printf(parser, "    leaq .LC_char_input_format(%%rip), %s\n", scanf_regs[0]);  // Format string
                    code_printf(parser, "    xor %%eax, %%eax\n");  // Clear EAX (for variadic functions)
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
                    // Load the value that was read
                    code_printf(parser, "    movzbl (%%r15), %%eax\n");  // Load char from saved address
                    code_printf(parser, "    addq $16, %%rsp\n");  // Deallocate stack space
                    code_printf(parser, "    pushq %%rax\n");  // Return value
                    return;
                } else if (strcmp(name.value, "scanfString") == 0) {
                    // scanfString() -> string (reads string from stdin, returns pointer to static buffer)
                    // Reads until newline character
                    code_comment(parser, "Built-in: scanfString()");
                    
                    // Save callee-saved registers we'll use
                    code_printf(parser, "    pushq %%r15\n");
                    code_printf(parser, "    pushq %%r14\n");
                    code_printf(parser, "    pushq %%r13\n");
                    
                    // Use a static buffer
                    code_printf(parser, "    leaq _scanfString_buffer(%%rip), %%r15\n");  // Get address of static buffer
                    code_printf(parser, "    movq %%r15, %%r14\n");  // Save start address
                    code_printf(parser, "    movl $255, %%r13d\n");  // Counter (max 255 chars)
                    
                    // Read loop
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
                    code_printf(parser, "    cmp $10, %%eax\n");  // Check for newline
                    code_printf(parser, "    je .Lread_done_%d\n", parser->label_counter);
                    code_printf(parser, "    cmp $-1, %%eax\n");  // Check for EOF
                    code_printf(parser, "    je .Lread_done_%d\n", parser->label_counter);
                    code_printf(parser, "    movb %%al, (%%r15)\n");  // Store character
                    code_printf(parser, "    incq %%r15\n");  // Move to next position
                    code_printf(parser, "    decl %%r13d\n");  // Decrement counter
                    code_printf(parser, "    jnz .Lread_loop_%d\n", parser->label_counter);  // Continue if space left
                    
                    // Done reading
                    code_printf(parser, ".Lread_done_%d:\n", parser->label_counter);
                    code_printf(parser, "    movb $0, (%%r15)\n");  // Null terminate
                    code_printf(parser, "    movq %%r14, %%rax\n");  // Return start address
                    
                    // Restore callee-saved registers
                    code_printf(parser, "    popq %%r13\n");
                    code_printf(parser, "    popq %%r14\n");
                    code_printf(parser, "    popq %%r15\n");
                    
                    code_printf(parser, "    pushq %%rax\n");  // Push return value
                    parser->label_counter++;
                    return;
                }
            }

            // Check if it's a syscall I/O function
            if (is_syscall_io_function(name.value)) {
                expect(parser, TOKEN_LPAREN, "Expected '('");
                
                if (strcmp(name.value, "sys_write") == 0) {
                    // sys_write(fd, buffer, count) -> int
                    code_comment(parser, "Built-in syscall: sys_write(fd, buffer, count)");
                    
                    // Parse arguments: fd, buffer, count
                    parse_expression(parser);  // fd
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // buffer
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // count
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Arguments are on stack (top to bottom: count, buffer, fd)
                    code_printf(parser, "    popq %%rdx\n");  // count
                    code_printf(parser, "    popq %%rsi\n");  // buffer
                    code_printf(parser, "    popq %%rdi\n");  // fd
                    code_printf(parser, "    movq $1, %%rax\n");  // syscall number: write
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");  // return bytes written
                    return;
                    
                } else if (strcmp(name.value, "sys_read") == 0) {
                    // sys_read(fd, buffer, count) -> int
                    code_comment(parser, "Built-in syscall: sys_read(fd, buffer, count)");
                    
                    // Parse arguments: fd, buffer, count
                    parse_expression(parser);  // fd
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // buffer
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // count
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Arguments are on stack (top to bottom: count, buffer, fd)
                    code_printf(parser, "    popq %%rdx\n");  // count
                    code_printf(parser, "    popq %%rsi\n");  // buffer
                    code_printf(parser, "    popq %%rdi\n");  // fd
                    code_printf(parser, "    movq $0, %%rax\n");  // syscall number: read
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");  // return bytes read
                    return;
                    
                } else if (strcmp(name.value, "sys_open") == 0) {
                    // sys_open(pathname, flags, mode) -> int
                    code_comment(parser, "Built-in syscall: sys_open(pathname, flags, mode)");
                    
                    // Parse arguments: pathname, flags, mode
                    parse_expression(parser);  // pathname
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // flags
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // mode
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Arguments are on stack (top to bottom: mode, flags, pathname)
                    code_printf(parser, "    popq %%rdx\n");  // mode
                    code_printf(parser, "    popq %%rsi\n");  // flags
                    code_printf(parser, "    popq %%rdi\n");  // pathname
                    code_printf(parser, "    movq $2, %%rax\n");  // syscall number: open
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");  // return fd
                    return;
                    
                } else if (strcmp(name.value, "sys_close") == 0) {
                    // sys_close(fd) -> int
                    code_comment(parser, "Built-in syscall: sys_close(fd)");
                    
                    // Parse argument: fd
                    parse_expression(parser);  // fd
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Argument is on stack
                    code_printf(parser, "    popq %%rdi\n");  // fd
                    code_printf(parser, "    movq $3, %%rax\n");  // syscall number: close
                    code_printf(parser, "    syscall\n");
                    code_printf(parser, "    pushq %%rax\n");  // return status
                    return;
                    
                } else if (strcmp(name.value, "io_strlen") == 0) {
                    // io_strlen(s) -> int
                    code_comment(parser, "Built-in: io_strlen(s)");
                    
                    // Parse argument: string
                    parse_expression(parser);  // string pointer
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // String pointer is on stack
                    code_printf(parser, "    popq %%rdi\n");  // string pointer
                    generate_strlen_code(parser, "%rdi", "%rax");
                    code_printf(parser, "    pushq %%rax\n");  // return length
                    return;
                    
                } else if (strcmp(name.value, "io_int_to_str") == 0) {
                    // io_int_to_str(value, buffer, buffer_size) -> int
                    code_comment(parser, "Built-in: io_int_to_str(value, buffer, buffer_size)");
                    
                    // Parse arguments
                    parse_expression(parser);  // value
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // buffer
                    expect(parser, TOKEN_COMMA, "Expected ','");
                    parse_expression(parser);  // buffer_size
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Arguments on stack: buffer_size, buffer, value
                    code_printf(parser, "    popq %%rdx\n");  // buffer_size (unused for now)
                    code_printf(parser, "    popq %%rdi\n");  // buffer
                    code_printf(parser, "    popq %%rsi\n");  // value
                    generate_int_to_str_code(parser, "%rsi", "%rdi");
                    // Calculate and return length
                    generate_strlen_code(parser, "%rdi", "%rax");
                    code_printf(parser, "    pushq %%rax\n");  // return length
                    return;
                    
                } else if (strcmp(name.value, "io_str_to_int") == 0) {
                    // io_str_to_int(s) -> int
                    code_comment(parser, "Built-in: io_str_to_int(s)");
                    
                    // Parse argument: string
                    parse_expression(parser);  // string
                    expect(parser, TOKEN_RPAREN, "Expected ')'");
                    
                    // Simple ASCII to integer conversion
                    int label_num = parser->label_counter++;
                    code_printf(parser, "    popq %%rdi\n");  // string pointer
                    code_printf(parser, "    xorq %%rax, %%rax\n");  // result = 0
                    code_printf(parser, "    xorq %%rcx, %%rcx\n");  // sign = 0
                    code_printf(parser, "    movzbl (%%rdi), %%edx\n");  // first char
                    code_printf(parser, "    cmpb $45, %%dl\n");  // check for '-'
                    code_printf(parser, "    jne .Lstr2int_loop_%d\n", label_num);
                    code_printf(parser, "    movq $1, %%rcx\n");  // sign = 1
                    code_printf(parser, "    incq %%rdi\n");  // skip '-'
                    code_printf(parser, ".Lstr2int_loop_%d:\n", label_num);
                    code_printf(parser, "    movzbl (%%rdi), %%edx\n");  // get char
                    code_printf(parser, "    testb %%dl, %%dl\n");  // check for null
                    code_printf(parser, "    je .Lstr2int_done_%d\n", label_num);
                    code_printf(parser, "    subb $48, %%dl\n");  // convert to digit
                    code_printf(parser, "    cmpb $9, %%dl\n");  // check valid digit
                    code_printf(parser, "    ja .Lstr2int_done_%d\n", label_num);
                    code_printf(parser, "    imulq $10, %%rax\n");  // result *= 10
                    code_printf(parser, "    movzbl %%dl, %%edx\n");
                    code_printf(parser, "    addq %%rdx, %%rax\n");  // result += digit
                    code_printf(parser, "    incq %%rdi\n");  // next char
                    code_printf(parser, "    jmp .Lstr2int_loop_%d\n", label_num);
                    code_printf(parser, ".Lstr2int_done_%d:\n", label_num);
                    code_printf(parser, "    testq %%rcx, %%rcx\n");  // check sign
                    code_printf(parser, "    je .Lstr2int_positive_%d\n", label_num);
                    code_printf(parser, "    negq %%rax\n");  // negate if needed
                    code_printf(parser, ".Lstr2int_positive_%d:\n", label_num);
                    code_printf(parser, "    pushq %%rax\n");  // return value
                    return;
                }
            }

            // Check if we're in a method context and this might be a method call
            int is_method_call = 0;
            char method_mangled_name[MAX_TOKEN * 2];
            Function *func = find_function(parser, name.value);
            
            if (!func && parser->current_struct_context[0] != '\0') {
                // Try to find it as a method of the current struct
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
                // This is a method call on the same struct - need to pass 'this' as first arg
                // Pop arguments into registers (offset by 1 because first reg has 'this')
                int max_reg_args = get_max_reg_args();
                for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
                    DataType param_type = func->param_types[i];
                    int reg_idx = i + 1;  // Offset by 1 because first reg has 'this'

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

                // Load 'this' pointer into first register
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
                // Regular function call
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
            
            // If variable not found and we're in a method context, check if it's a field
            if (!var && parser->current_struct_context[0] != '\0') {
                // Look for 'this' pointer
                Variable *this_var = find_variable(parser, "this");
                if (this_var && this_var->struct_type[0] != '\0') {
                    StructDefinition *struct_def = find_struct(parser, this_var->struct_type);
                    if (struct_def) {
                        // Check if name is a field
                        StructField *field = NULL;
                        for (int i = 0; i < struct_def->field_count; i++) {
                            if (strcmp(struct_def->fields[i].name, name.value) == 0) {
                                field = &struct_def->fields[i];
                                break;
                            }
                        }
                        
                        if (field) {
                            // Check if this is an array field with indexing
                            if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                                consume(parser->tokens);  // consume '['
                                
                                code_comment(parser, "Array field access through implicit 'this': %s[...]", name.value);
                                
                                // Parse the index expression
                                parse_expression(parser);
                                
                                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                                
                                // Stack has: [index]
                                // Load 'this' pointer
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                                
                                // Calculate array element address: base + field_offset + (index * element_size)
                                code_printf(parser, "    popq %%rax\n");  // index
                                int element_size = datatype_size(field->type);
                                code_printf(parser, "    imulq $%d, %%rax\n", element_size);  // index * element_size
                                code_printf(parser, "    addq $%d, %%rax\n", field->offset);  // + field_offset
                                code_printf(parser, "    addq %%rbx, %%rax\n");  // + base address
                                
                                // Load value from calculated address
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
                                // Regular field access (not array or no indexing)
                                code_comment(parser, "Field access through implicit 'this': %s", name.value);
                                
                                // Load 'this' pointer
                                code_printf(parser, "    movq %d(%%rbp), %%rbx\n", this_var->offset);
                                
                                // Check if this is a nested struct field with more dot access
                                if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                    // This is a nested struct field, and there's another dot
                                    // Calculate address of nested struct field
                                    code_printf(parser, "    addq $%d, %%rbx\n", field->offset);
                                    
                                    // Continue with chained access
                                    StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                                    if (!nested_struct_def) {
                                        parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                                        return;
                                    }
                                    
                                    // Loop to handle chained field access
                                    while (check(parser->tokens, TOKEN_DOT)) {
                                        consume(parser->tokens);  // consume '.'
                                        Token next_field_name = consume(parser->tokens);
                                        
                                        // Find the field in the nested struct
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
                                        
                                        // Check if there's another dot (more chaining)
                                        if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                            // Another nested struct - add offset and continue
                                            code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                                            nested_struct_def = find_struct(parser, nested_field->struct_type);
                                            if (!nested_struct_def) {
                                                parser_error(parser, "Nested struct type '%s' not found", nested_field->struct_type);
                                                return;
                                            }
                                        } else {
                                            // Final field - load its value
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
                                    // Load field value (no chaining)
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
            
            // If variable not found, check if it's an enum type (for enum value access)
            if (!var) {
                EnumDefinition *enum_def = find_enum(parser, name.value);
                if (enum_def && check(parser->tokens, TOKEN_DOT)) {
                    // This is enum value access: EnumName.ValueName.field
                    consume(parser->tokens);  // consume '.'
                    Token value_name_token = consume(parser->tokens);
                    
                    // Find the enum value
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
                    
                    // Now we need to access a field on this enum value
                    if (!check(parser->tokens, TOKEN_DOT)) {
                        parser_error(parser, "Expected '.' after enum value '%s'", value_name_token.value);
                        return;
                    }
                    
                    consume(parser->tokens);  // consume '.'
                    Token field_name_token = consume(parser->tokens);
                    
                    // Find the field in the enum definition
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
                    
                    // Generate code to load the field from the global enum value variable
                    char global_name[MAX_TOKEN * 2];
                    snprintf(global_name, sizeof(global_name), "%s_%s", name.value, value_name_token.value);
                    
                    code_comment(parser, "Access enum field: %s.%s.%s", 
                                name.value, value_name_token.value, field_name_token.value);
                    code_printf(parser, "    leaq %s(%%rip), %%rbx\n", global_name);
                    
                    // Load the field value
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
                    // This is a static method call in an expression: StructName.methodName()
                    consume(parser->tokens);  // consume '.'
                    Token method_name = consume(parser->tokens);
                    
                    if (!check(parser->tokens, TOKEN_LPAREN)) {
                        parser_error(parser, "Expected '(' after static method name");
                        return;
                    }
                    
                    consume(parser->tokens);  // consume '('
                    
                    // Find the static method
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
                    
                    // Parse arguments (no 'this' pointer for static methods)
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
                    
                    // Set up arguments in registers (no 'this' pointer offset)
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
                    
                    // Call the static method
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
                    
                    // Push return value
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

            // Check for dot notation: var.field or var.method()
            if (check(parser->tokens, TOKEN_DOT)) {
                consume(parser->tokens);  // consume '.'
                
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
                
                // Check if it's a method call
                if (check(parser->tokens, TOKEN_LPAREN)) {
                    consume(parser->tokens);  // consume '('
                    
                    // Find the method
                    char mangled_name[MAX_TOKEN * 2];
                    snprintf(mangled_name, sizeof(mangled_name), "%s_%s", var->struct_type, member_name.value);
                    
                    Function *method = find_function(parser, mangled_name);
                    if (!method) {
                        parser_error(parser, "Method '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Parse arguments
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
                    
                    // Prepare arguments (in reverse order) and implicit 'this' pointer
                    const char **arg_regs_64 = get_arg_registers_64();
                    const char *arg_regs_float[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};
                    
                    // Pop arguments into registers (in reverse order)
                    int max_reg_args = get_max_reg_args();
                    for (int i = arg_count - 1; i >= 0 && i + 1 < max_reg_args; i--) {
                        DataType param_type = method->param_types[i];
                        int reg_idx = i + 1;  // Offset by 1 because first reg has 'this'
                        
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
                    
                    // Load 'this' pointer (address of struct instance) into first register
                    code_comment(parser, "Method call: %s.%s()", name.value, member_name.value);
                    const char **this_regs2_64 = get_arg_registers_64();
                    code_printf(parser, "    leaq %d(%%rbp), %s\n", var->offset, this_regs2_64[0]);  // Address of struct
                    
                    {
                int stack_adj_33 = get_call_stack_space();
                if (stack_adj_33 > 0) {
                    code_printf(parser, "    subq $%d, %%rsp\n", stack_adj_33);
                }
                code_printf(parser, "    call %s\n", mangled_name);
                if (stack_adj_33 > 0) {
                    code_printf(parser, "    addq $%d, %%rsp\n", stack_adj_33);
                }
            }
                    
                    if (method->return_type == TYPE_FLOAT || method->return_type == TYPE_DOUBLE) {
                        code_printf(parser, "    movq %%xmm0, %%rax\n");
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        code_printf(parser, "    pushq %%rax\n");
                    }
                    
                    return;
                } else {
                    // Field access
                    StructField *field = NULL;
                    for (int i = 0; i < struct_def->field_count; i++) {
                        if (strcmp(struct_def->fields[i].name, member_name.value) == 0) {
                            field = &struct_def->fields[i];
                            break;
                        }
                    }
                    
                    if (!field) {
                        parser_error(parser, "Field '%s' not found in struct '%s'", member_name.value, var->struct_type);
                        return;
                    }
                    
                    // Check if this is an array field with indexing
                    if (field->is_array && check(parser->tokens, TOKEN_LBRACKET)) {
                        consume(parser->tokens);  // consume '['
                        
                        code_comment(parser, "Array field access: %s.%s[...]", name.value, member_name.value);
                        
                        // Parse the index expression
                        parse_expression(parser);
                        
                        expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                        
                        // Stack has: [index]
                        // Load struct base address into %rbx
                        code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                        
                        // Calculate array element address: base + field_offset + (index * element_size)
                        code_printf(parser, "    popq %%rax\n");  // index
                        int element_size = datatype_size(field->type);
                        code_printf(parser, "    imulq $%d, %%rax\n", element_size);  // index * element_size
                        code_printf(parser, "    addq $%d, %%rax\n", field->offset);  // + field_offset
                        code_printf(parser, "    addq %%rbx, %%rax\n");  // + base address
                        
                        // Load value from calculated address
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
                        // Regular field access (not array or no indexing)
                        code_comment(parser, "Field access: %s.%s", name.value, member_name.value);
                        
                        // Load struct base address into %rbx
                        code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);
                        
                        // Check if this is a nested struct field with more dot access
                        if (field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                            // This is a nested struct field, and there's another dot
                            // Calculate address of nested struct field
                            code_printf(parser, "    addq $%d, %%rbx\n", field->offset);
                            
                            // Continue with chained access
                            StructDefinition *nested_struct_def = find_struct(parser, field->struct_type);
                            if (!nested_struct_def) {
                                parser_error(parser, "Nested struct type '%s' not found", field->struct_type);
                                return;
                            }
                            
                            // Loop to handle chained field access
                            while (check(parser->tokens, TOKEN_DOT)) {
                                consume(parser->tokens);  // consume '.'
                                Token next_field_name = consume(parser->tokens);
                                
                                // Find the field in the nested struct
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
                                
                                // Check if there's another dot (more chaining)
                                if (nested_field->struct_type[0] != '\0' && check(parser->tokens, TOKEN_DOT)) {
                                    // Another nested struct - add offset and continue
                                    code_printf(parser, "    addq $%d, %%rbx\n", nested_field->offset);
                                    nested_struct_def = find_struct(parser, nested_field->struct_type);
                                    if (!nested_struct_def) {
                                        parser_error(parser, "Nested struct type '%s' not found", nested_field->struct_type);
                                        return;
                                    }
                                } else {
                                    // Final field - load its value
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
                            // Access field using offset from base address (no chaining)
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

            // Check for array indexing: var[index]
            if (check(parser->tokens, TOKEN_LBRACKET)) {
                consume(parser->tokens);
                
                if (!var->is_array && var->type != TYPE_STRING) {
                    parser_error(parser, "Variable '%s' is not an array or string", name.value);
                    return;
                }
                
                // Parse the index expression
                parse_expression(parser);
                
                expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
                
                // Stack now has: [index]
                int element_size = datatype_size(var->type);
                
                code_printf(parser, "    popq %%rax\n");  // index
                
                // For array parameters (pointers), load the pointer first
                if (var->is_array && var->array_size == 0) {
                    // Array parameter - it's a pointer, load it
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);  // load pointer
                } else {
                    // Local array - calculate address
                    code_printf(parser, "    leaq %d(%%rbp), %%rbx\n", var->offset);  // base address
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
                    // For strings, we need to load the pointer first, then index into it
                    code_printf(parser, "    movq %d(%%rbp), %%rbx\n", var->offset);  // load string pointer
                    code_printf(parser, "    movzbl (%%rbx, %%rax, 1), %%eax\n");  // index into string (byte access)
                    code_printf(parser, "    pushq %%rax\n");
                } else {  // TYPE_INT
                    code_printf(parser, "    movl (%%rbx, %%rax, %d), %%eax\n", element_size);
                    code_printf(parser, "    pushq %%rax\n");
                }
            } else {
                // Regular variable access (not array indexing)
                // For arrays used as function arguments, we need to pass the address
                if (var->is_array) {
                    // For array parameters (pointers), load the pointer
                    if (var->array_size == 0) {
                        code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
                        code_printf(parser, "    pushq %%rax\n");
                    } else {
                        // For local arrays, load the address
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
                } else if (var->is_pointer) {
                    // Pointers are 64-bit addresses
                    code_printf(parser, "    movq %d(%%rbp), %%rax\n", var->offset);
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