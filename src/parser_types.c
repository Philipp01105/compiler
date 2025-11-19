#include "parser.h"
#include "parser_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * escape_char_for_comment - Convert character to printable escape sequence
 * @ch_value: Character to escape
 *
 * Returns escape sequence string for assembly comments.
 */
const char *escape_char_for_comment(const char *ch_value) {
    static char buffer[32];

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
        snprintf(buffer, sizeof(buffer), "%c", ch_value[0]);
        return buffer;
    } else {
        snprintf(buffer, sizeof(buffer), "\\x%02x", (unsigned char) ch_value[0]);
        return buffer;
    }
}

/*
 * escape_string_for_comment - Escape string for assembly comments
 * @str: Input string to escape
 * @output: Output buffer for escaped string
 * @output_size: Size of output buffer
 *
 * Converts special characters to escape sequences for safe comment generation.
 */
void escape_string_for_comment(const char *str, char *output, size_t output_size) {
    size_t out_pos = 0;
    for (size_t i = 0; str[i] != '\0' && out_pos < output_size - 5; i++) {
        unsigned char c = (unsigned char) str[i];
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
            int written = snprintf(&output[out_pos], output_size - out_pos, "\\x%02x", c);
            if (written > 0) out_pos += written;
        }
    }
    output[out_pos] = '\0';
}

/*
 * Platform detection for calling convention differences
 */
static int is_windows_platform() {
#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
    return 1;
#else
    return 0;
#endif
}

/*
 * get_arg_registers_64 - Get 64-bit argument registers for platform
 *
 * Returns platform-specific argument passing registers.
 * Windows x64: RCX, RDX, R8, R9
 * Linux x64: RDI, RSI, RDX, RCX, R8, R9
 */
const char **get_arg_registers_64() {
    static const char *windows_regs[] = {"%rcx", "%rdx", "%r8", "%r9"};
    static const char *linux_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

const char **get_arg_registers_32() {
    static const char *windows_regs[] = {"%ecx", "%edx", "%r8d", "%r9d"};
    static const char *linux_regs[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

const char **get_arg_registers_8() {
    static const char *windows_regs[] = {"%cl", "%dl", "%r8b", "%r9b"};
    static const char *linux_regs[] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
    return is_windows_platform() ? windows_regs : linux_regs;
}

int get_max_reg_args() {
    return is_windows_platform() ? 4 : 6;
}

int get_stack_alignment() {
    return 16;
}

int get_shadow_space() {
    return is_windows_platform() ? 32 : 0;
}

int get_call_stack_space() {
    if (is_windows_platform()) {
        return 40;
    } else {
        return 0;
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

int is_builtin_string_function(const char *name) {
    return strcmp(name, "strlen") == 0 ||
           strcmp(name, "strcpy") == 0 ||
           strcmp(name, "strcat") == 0 ||
           strcmp(name, "strcmp") == 0 ||
           strcmp(name, "strdup") == 0;
}

int is_builtin_memory_function(const char *name) {
    return strcmp(name, "malloc") == 0 ||
           strcmp(name, "free") == 0;
}

int is_builtin_input_function(const char *name) {
    return strcmp(name, "scanfInt") == 0 ||
           strcmp(name, "scanfChar") == 0 ||
           strcmp(name, "scanfString") == 0;
}

int is_syscall_io_function(const char *name) {
    return strcmp(name, "sys_write") == 0 ||
           strcmp(name, "sys_read") == 0 ||
           strcmp(name, "sys_open") == 0 ||
           strcmp(name, "sys_close") == 0 ||
           strcmp(name, "io_strlen") == 0 ||
           strcmp(name, "io_int_to_str") == 0 ||
           strcmp(name, "io_str_to_int") == 0 ||
           strcmp(name, "read") == 0;
}
