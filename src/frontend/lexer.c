#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include "errorHandler.h"

#define INITIAL_CAPACITY 1000

/*
 * create_token_stream - Allocate new token stream
 *
 * Returns dynamically allocated token stream with initial capacity.
 */
TokenStream *create_token_stream(void) {
    return create_token_stream_with_interner(NULL);
}

TokenStream *create_token_stream_with_interner(StringInterner *interner) {
    TokenStream *stream = malloc(sizeof(TokenStream));
    if (!stream) return NULL;
    stream->capacity = INITIAL_CAPACITY;
    stream->count = 0;
    stream->current = 0;
    stream->strings = interner == NULL ? string_interner_create() : interner;
    stream->owns_strings = interner == NULL;
    if (stream->strings == NULL) {
        free(stream);
        return NULL;
    }
    stream->tokens = malloc(sizeof(Token) * (size_t) stream->capacity);
    stream->has_error = stream->tokens == NULL;
    if (!stream->tokens) {
        if (stream->owns_strings) string_interner_free(stream->strings);
        free(stream);
        return NULL;
    }
    return stream;
}

void free_token_stream(TokenStream *stream) {
    if (stream) {
        free(stream->tokens);
        if (stream->owns_strings) string_interner_free(stream->strings);
        free(stream);
    }
}

void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column) {
    if (stream == NULL) return;

    if (stream->count >= stream->capacity) {
        if (stream->capacity > INT_MAX / 2) {
            stream->has_error = 1;
            error_report(global_error_handler, SEVERITY_FATAL, line, column, ERROR_CATEGORY_LEXER,
                         ERR_LEX_FILE_READ_ERROR, NULL, "Token stream capacity overflow");
            return;
        }
        int new_capacity = stream->capacity * 2;
        Token *new_tokens = realloc(stream->tokens, sizeof(Token) * (size_t) new_capacity);
        if (!new_tokens) {
            stream->has_error = 1;
            error_report(global_error_handler, SEVERITY_FATAL, line, column, ERROR_CATEGORY_LEXER,
                         ERR_LEX_FILE_READ_ERROR, NULL, "Out of memory while growing token stream");
            return;
        }
        stream->capacity = new_capacity;
        stream->tokens = new_tokens;
    }

    Token *token = &stream->tokens[stream->count];
    token->type = type;
    token->line = line;
    token->column = column;
    token->end_line = line;
    token->end_column = column + (int) strlen(value == NULL ? "" : value);

    token->value = string_interner_intern(stream->strings, value == NULL ? "" : value);
    if (token->value == NULL) {
        stream->has_error = 1;
        error_report(global_error_handler, SEVERITY_FATAL, line, column, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, NULL, "Out of memory while interning token text");
        return;
    }

    stream->count++;
}

Token peek(const TokenStream *stream) {
    if (stream->current < stream->count) {
        return stream->tokens[stream->current];
    }
    const Token eof = {.type = TOKEN_EOF, .value = ""};
    return eof;
}

Token peek_ahead(const TokenStream *stream, int offset) {
    long long position = (long long) stream->current + (long long) offset;
    if (position >= 0 && position < stream->count) {
        return stream->tokens[(size_t) position];
    }
    Token eof = {.type = TOKEN_EOF, .value = ""};
    return eof;
}

Token consume(TokenStream *stream) {
    if (stream->current < stream->count) {
        return stream->tokens[stream->current++];
    }
    Token eof = {.type = TOKEN_EOF, .value = ""};
    return eof;
}

int check(const TokenStream *stream, TokenType type) {
    if (stream->current >= stream->count) {
        return 0;
    }
    return stream->tokens[stream->current].type == type;
}

int match(TokenStream *stream, TokenType type) {
    if (check(stream, type)) {
        stream->current++;
        return 1;
    }
    return 0;
}

int is_at_end(const TokenStream *stream) {
    return stream->current >= stream->count ||
           stream->tokens[stream->current].type == TOKEN_EOF;
}

static TokenType get_keyword_type(const char *str) {
    if (strcmp(str, "interface") == 0) return TOKEN_KEYWORD_INTERFACE;
    if (strcmp(str, "match") == 0) return TOKEN_KEYWORD_MATCH;
    if (strcmp(str, "func") == 0) return TOKEN_KEYWORD_FUNC;
    if (strcmp(str, "extern") == 0) return TOKEN_KEYWORD_EXTERN;
    if (strcmp(str, "from") == 0) return TOKEN_KEYWORD_FROM;
    if (strcmp(str, "export") == 0) return TOKEN_KEYWORD_EXPORT;
    if (strcmp(str, "union") == 0) return TOKEN_KEYWORD_UNION;
    if (strcmp(str, "async") == 0) return TOKEN_KEYWORD_ASYNC;
    if (strcmp(str, "await") == 0) return TOKEN_KEYWORD_AWAIT;
    if (strcmp(str, "var") == 0) return TOKEN_KEYWORD_VAR;
    if (strcmp(str, "return") == 0) return TOKEN_KEYWORD_RETURN;
    if (strcmp(str, "for") == 0) return TOKEN_KEYWORD_FOR;
    if (strcmp(str, "if") == 0) return TOKEN_KEYWORD_IF;
    if (strcmp(str, "else") == 0) return TOKEN_KEYWORD_ELSE;
    if (strcmp(str, "while") == 0) return TOKEN_KEYWORD_WHILE;
    if (strcmp(str, "break") == 0) return TOKEN_KEYWORD_BREAK;
    if (strcmp(str, "continue") == 0) return TOKEN_KEYWORD_CONTINUE;
    if (strcmp(str, "struct") == 0) return TOKEN_KEYWORD_STRUCT;
    if (strcmp(str, "enum") == 0) return TOKEN_KEYWORD_ENUM;
    if (strcmp(str, "import") == 0) return TOKEN_KEYWORD_IMPORT;
    if (strcmp(str, "static") == 0) return TOKEN_KEYWORD_STATIC;
    if (strcmp(str, "reserve") == 0) return TOKEN_KEYWORD_RESERVE;
    if (strcmp(str, "free") == 0) return TOKEN_KEYWORD_FREE;
    if (strcmp(str, "const") == 0) return TOKEN_KEYWORD_CONST;
    if (strcmp(str, "package") == 0) return TOKEN_KEYWORD_PACKAGE;
    if (strcmp(str, "pub") == 0) return TOKEN_KEYWORD_PUB;
    if (strcmp(str, "sizeof") == 0) return TOKEN_KEYWORD_SIZEOF;
    if (strcmp(str, "typeof") == 0) return TOKEN_KEYWORD_TYPEOF;
    if (strcmp(str, "case") == 0) return TOKEN_KEYWORD_CASE;
    if (strcmp(str, "alignof") == 0) return TOKEN_KEYWORD_ALIGNOF;
    if (strcmp(str, "slice") == 0) return TOKEN_KEYWORD_SLICE;
    if (strcmp(str, "destructor") == 0) return TOKEN_KEYWORD_DESTRUCTOR;
    if (strcmp(str, "defer") == 0) return TOKEN_KEYWORD_DEFER;
    if (strcmp(str, "mut") == 0) return TOKEN_KEYWORD_MUT;

    if (strcmp(str, "int") == 0) return TOKEN_TYPE_INT;
    if (strcmp(str, "i8") == 0) return TOKEN_TYPE_I8;
    if (strcmp(str, "u8") == 0) return TOKEN_TYPE_U8;
    if (strcmp(str, "i16") == 0) return TOKEN_TYPE_I16;
    if (strcmp(str, "u16") == 0) return TOKEN_TYPE_U16;
    if (strcmp(str, "i32") == 0) return TOKEN_TYPE_I32;
    if (strcmp(str, "u32") == 0) return TOKEN_TYPE_U32;
    if (strcmp(str, "i64") == 0) return TOKEN_TYPE_I64;
    if (strcmp(str, "u64") == 0) return TOKEN_TYPE_U64;
    if (strcmp(str, "isize") == 0) return TOKEN_TYPE_ISIZE;
    if (strcmp(str, "usize") == 0) return TOKEN_TYPE_USIZE;
    if (strcmp(str, "char") == 0) return TOKEN_TYPE_CHAR;
    if (strcmp(str, "byte") == 0) return TOKEN_TYPE_BYTE;
    if (strcmp(str, "bit") == 0) return TOKEN_TYPE_BIT;
    if (strcmp(str, "float") == 0) return TOKEN_TYPE_FLOAT;
    if (strcmp(str, "double") == 0) return TOKEN_TYPE_DOUBLE;
    if (strcmp(str, "string") == 0) return TOKEN_TYPE_STRING;
    if (strcmp(str, "void") == 0) return TOKEN_TYPE_VOID;
    if (strcmp(str, "never") == 0) return TOKEN_TYPE_NEVER;

    return TOKEN_IDENTIFIER;
}

TokenStream *tokenize_file(const char *filename, int debug_mode) {
    return tokenize_file_with_interner(filename, debug_mode, NULL);
}

TokenStream *tokenize_file_with_interner(const char *filename, int debug_mode,
                                         StringInterner *interner) {
    (void) debug_mode;
    FILE *file = fopen(error_handler_source_path(filename), "rb");
    if (!file) {
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_NOT_FOUND, filename, "Could not open source file '%s': %s", filename,
                     strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        int saved_errno = errno;
        fclose(file);
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, filename, "Could not seek in source file '%s': %s", filename,
                     strerror(saved_errno));
        return NULL;
    }
    long file_size = ftell(file);
    if (file_size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        int saved_errno = errno;
        fclose(file);
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, filename, "Could not determine source file size for '%s': %s", filename,
                     strerror(saved_errno));
        return NULL;
    }

    char *source = malloc((size_t) file_size + 1);
    if (!source) {
        fclose(file);
        error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, filename, "Out of memory while reading source file");
        return NULL;
    }
    size_t bytes_read = fread(source, 1, (size_t) file_size, file);
    if (bytes_read != (size_t) file_size && ferror(file)) {
        int saved_errno = errno;
        fclose(file);
        free(source);
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, filename, "Could not read source file '%s': %s", filename,
                     strerror(saved_errno));
        return NULL;
    }
    source[bytes_read] = '\0';
    fclose(file);

    TokenStream *stream = tokenize_source_with_interner(source, bytes_read, filename, interner);
    free(source);
    return stream;
}

TokenStream *tokenize_source(const char *source, size_t length, const char *filename) {
    return tokenize_source_with_interner(source, length, filename, NULL);
}

TokenStream *tokenize_source_with_interner(const char *source, size_t length,
                                           const char *filename,
                                           StringInterner *interner) {
    TokenStream *stream = create_token_stream_with_interner(interner);
    if (!stream) {
        error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_LEXER,
                     ERR_LEX_FILE_READ_ERROR, filename, "Out of memory while creating token stream");
        return NULL;
    }

    int line = 1;
    int column = 1;
    size_t i = 0;
    while (i < length) {
        char c = source[i];

        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            column++;
            continue;
        }

        if (c == '\n') {
            i++;
            line++;
            column = 1;
            continue;
        }

        if (c == '/' && i + 1 < length && source[i + 1] == '*') {
            int start_line = line, start_col = column;
            i += 2;
            column += 2;
            while (i < length && !(source[i] == '*' && i + 1 < length && source[i + 1] == '/')) {
                if (source[i++] == '\n') {
                    line++;
                    column = 1;
                } else column++;
            }
            if (i == length) {
                error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                             ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_SYNTAX, filename, "Unclosed block comment");
                stream->has_error = 1;
            } else {
                i += 2;
                column += 2;
            }
            continue;
        }

        if (c == '/' && i + 1 < length && source[i + 1] == '/') {
            while (i < length && source[i] != '\n') {
                i++;
            }
            continue;
        }

        if (c == '"') {
            int start_col = column;
            int start_line = line;
            i++;
            column++;

            char str[MAX_TOKEN] = {0};
            int j = 0;

            while (i < length && source[i] != '"' && source[i] != '\n' && source[i] != '\r' && j < MAX_TOKEN - 1) {
                if (source[i] == '\\' && i + 1 < length && source[i + 1] != '\n' && source[i + 1] != '\r') {
                    i++;
                    column++;

                    switch (source[i]) {
                        case 'n': str[j++] = '\n';
                            break;
                        case 't': str[j++] = '\t';
                            break;
                        case 'r': str[j++] = '\r';
                            break;
                        case '0': str[j++] = '\0';
                            break;
                        case '\\': str[j++] = '\\';
                            break;
                        case '"': str[j++] = '"';
                            break;
                        case '\'': str[j++] = '\'';
                            break;
                        default:
                            error_report_with_suggestion(global_error_handler, SEVERITY_ERROR, line, column - 1,
                                                         ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_ESCAPE, filename,
                                                         "Supported string escapes: \\n, \\t, \\r, \\0, \\\\, and \\\"",
                                                         "Unknown escape sequence '\\%c'", source[i]);
                            stream->has_error = 1;
                            str[j++] = source[i];
                            break;
                    }
                } else {
                    unsigned char byte = (unsigned char) source[i];
                    if (byte >= 0x80) {
                        str[j++] = source[i++];
                        int continuation_count = 0;
                        while (i < length && continuation_count < 3 &&
                               ((unsigned char) source[i] & 0xC0) == 0x80 && j < MAX_TOKEN - 1) {
                            str[j++] = source[i++];
                            continuation_count++;
                        }
                        column++;
                        continue;
                    }
                    str[j++] = source[i];
                }
                i++;
                column++;
            }

            str[j] = '\0';

            if (j == MAX_TOKEN - 1 && i < length && source[i] != '"') {
                error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                             ERROR_CATEGORY_LEXER, ERR_LEX_TOKEN_TOO_LONG, filename,
                             "Token exceeds maximum length of %d bytes", MAX_TOKEN - 1);
                stream->has_error = 1;
                while (i < length && source[i] != '"') {
                    i++;
                    column++;
                }
                if (i < length) {
                    i++;
                    column++;
                }
            } else if (i < length && source[i] == '"') {
                i++;
                column++;
                int previous_count = stream->count;
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
                if (stream->count > previous_count) {
                    stream->tokens[stream->count - 1].end_line = line;
                    stream->tokens[stream->count - 1].end_column = column;
                }
            } else {
                error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                             ERROR_CATEGORY_LEXER, ERR_LEX_UNCLOSED_STRING, filename,
                             "Unclosed string literal");
                stream->has_error = 1;
            }

            continue;
        }

        if (c == '\'') {
            int start_col = column;
            int start_line = line;
            i++;
            column++;

            char ch[4] = {0};
            int j = 0;

            if (i < length && source[i] == '\\') {
                i++;
                column++;

                if (i < length) {
                    char esc = source[i];
                    switch (esc) {
                        case 'n': ch[j++] = '\n';
                            break;
                        case 't': ch[j++] = '\t';
                            break;
                        case 'r': ch[j++] = '\r';
                            break;
                        case '0': ch[j++] = '\0';
                            break;
                        case '\\': ch[j++] = '\\';
                            break;
                        case '"': ch[j++] = '"';
                            break;
                        case '\'': ch[j++] = '\'';
                            break;
                        default:
                            error_report_with_suggestion(global_error_handler, SEVERITY_ERROR, line, column - 1,
                                                         ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_ESCAPE, filename,
                                                         "Use a supported escape such as \\n, \\t, \\\\, or \\'",
                                                         "Unknown escape sequence '\\%c'", esc);
                            stream->has_error = 1;
                            ch[j++] = esc;
                    }
                    i++;
                    column++;
                }
            } else if (i < length && source[i] != '\'' && source[i] != '\n' && source[i] != '\r') {
                ch[j++] = source[i++];
                column++;
            }

            ch[j] = '\0';

            if (i < length && source[i] == '\'') {
                i++;
                column++;
                if (j != 1) {
                    error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                                 ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_CHAR_LITERAL, filename,
                                 "Character literal must contain exactly one byte");
                    stream->has_error = 1;
                } else {
                    int previous_count = stream->count;
                    add_token(stream, TOKEN_CHAR_LITERAL, ch, start_line, start_col);
                    if (stream->count > previous_count) {
                        stream->tokens[stream->count - 1].end_line = line;
                        stream->tokens[stream->count - 1].end_column = column;
                    }
                }
            } else {
                size_t end = i;
                while (end < length && source[end] != '\'' && source[end] != '\n' && source[end] != '\r') end++;
                if (end < length && source[end] == '\'') {
                    error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                                 ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_CHAR_LITERAL, filename,
                                 "Character literal must contain exactly one byte; use a string for multiple characters");
                    column++; // Closing quote; UTF-8 continuation bytes do not add columns.
                    for (size_t k = i; k < end; k++)
                        if (((unsigned char) source[k] & 0xC0) != 0x80) column++;
                    i = end + 1;
                    stream->has_error = 1;
                    continue;
                }
                error_report(global_error_handler, SEVERITY_ERROR, start_line, start_col,
                             ERROR_CATEGORY_LEXER, ERR_LEX_UNCLOSED_CHAR, filename,
                             "Unclosed character literal");
                stream->has_error = 1;
            }

            continue;
        }

        if (isdigit((unsigned char) c)) {
            int start_col = column;
            char num[MAX_TOKEN];
            int j = 0;
            int has_dot = 0;
            while (i < length && (isdigit((unsigned char) source[i]) || source[i] == '.') && j < MAX_TOKEN - 1) {
                if (source[i] == '.') {
                    if (i + 1 < length && source[i + 1] == '(') break;
                    if (has_dot) break;
                    has_dot = 1;
                }
                num[j++] = source[i++];
                column++;
            }

            if (j < MAX_TOKEN - 2 && i < length && (source[i] == 'e' || source[i] == 'E')) {
                num[j++] = source[i++];
                column++;

                if (j < MAX_TOKEN - 1 && i < length && (source[i] == '+' || source[i] == '-')) {
                    num[j++] = source[i++];
                    column++;
                }

                size_t exponent_start = i;
                while (i < length && isdigit((unsigned char) source[i]) && j < MAX_TOKEN - 1) {
                    num[j++] = source[i++];
                    column++;
                }

                if (i == exponent_start) {
                    error_report(global_error_handler, SEVERITY_ERROR, line, start_col,
                                 ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_SYNTAX, filename,
                                 "Invalid floating-point literal: exponent requires digits");
                    stream->has_error = 1;
                }

                has_dot = 1;
            }

            num[j] = '\0';

            if (!has_dot) {
                errno = 0;
                char *end = NULL;
                (void) strtoull(num, &end, 10);
                if (errno == ERANGE || end == num || *end != '\0') {
                    error_report(global_error_handler, SEVERITY_ERROR, line, start_col,
                                 ERROR_CATEGORY_LEXER, ERR_LEX_INVALID_SYNTAX, filename,
                                 "Integer literal is outside unsigned 64-bit range");
                    stream->has_error = 1;
                }
            }

            if (i < length && (isdigit((unsigned char) source[i]) ||
                               (source[i] == '.' && !(i + 1 < length && source[i + 1] == '(')) ||
                               (j >= MAX_TOKEN - 2 && (source[i] == 'e' || source[i] == 'E')))) {
                error_report(global_error_handler, SEVERITY_ERROR, line, start_col,
                             ERROR_CATEGORY_LEXER, j >= MAX_TOKEN - 2 ? ERR_LEX_TOKEN_TOO_LONG : ERR_LEX_INVALID_SYNTAX,
                             filename,
                             j >= MAX_TOKEN - 2
                                 ? "Numeric token exceeds maximum length"
                                 : "Invalid numeric literal: multiple decimal points are not allowed");
                stream->has_error = 1;
                while (i < length && (isdigit((unsigned char) source[i]) || source[i] == '.')) {
                    i++;
                    column++;
                }
            }

            if (has_dot) {
                add_token(stream, TOKEN_FLOAT_LITERAL, num, line, start_col);
            } else {
                add_token(stream, TOKEN_NUMBER, num, line, start_col);
            }

            continue;
        }

        if (isalpha((unsigned char) c) || c == '_') {
            int start_col = column;
            char ident[MAX_TOKEN];
            int j = 0;

            while (i < length && (isalnum((unsigned char) source[i]) || source[i] == '_') && j < MAX_TOKEN - 1) {
                ident[j++] = source[i++];
                column++;
            }

            ident[j] = '\0';

            if (i < length && (isalnum((unsigned char) source[i]) || source[i] == '_')) {
                error_report(global_error_handler, SEVERITY_ERROR, line, start_col,
                             ERROR_CATEGORY_LEXER, ERR_LEX_TOKEN_TOO_LONG, filename,
                             "Token exceeds maximum length");
                stream->has_error = 1;
                while (i < length && (isalnum((unsigned char) source[i]) || source[i] == '_')) {
                    i++;
                    column++;
                }
            }

            TokenType type = get_keyword_type(ident);
            add_token(stream, type, ident, line, start_col);

            continue;
        }

        if (i + 1 < length && source[i] == '=' && source[i + 1] == '>') {
            add_token(stream, TOKEN_FAT_ARROW, "=>", line, column);
            i += 2;
            column += 2;
            continue;
        }
        if (i + 1 < length) {
            char next = source[i + 1];
            TokenType type = TOKEN_ERROR;

            if (c == '=' && next == '=') type = TOKEN_EQUAL_EQUAL;
            else if (c == '!' && next == '=') type = TOKEN_BANG_EQUAL;
            else if (c == '<' && next == '=') type = TOKEN_LESS_EQUAL;
            else if (c == '>' && next == '=') type = TOKEN_GREATER_EQUAL;
            else if (c == '&' && next == '&') type = TOKEN_AMP_AMP;
            else if (c == '|' && next == '|') type = TOKEN_PIPE_PIPE;
            else if (c == '+' && next == '=') type = TOKEN_PLUS_EQUAL;
            else if (c == '-' && next == '=') type = TOKEN_MINUS_EQUAL;
            else if (c == '*' && next == '=') type = TOKEN_STAR_EQUAL;
            else if (c == '/' && next == '=') type = TOKEN_SLASH_EQUAL;
            else if (c == '+' && next == '+') type = TOKEN_PLUS_PLUS;
            else if (c == '-' && next == '-') type = TOKEN_MINUS_MINUS;
            else if (c == '-' && next == '>') type = TOKEN_ARROW;

            if (type != TOKEN_ERROR) {
                char op[3] = {c, next, '\0'};
                add_token(stream, type, op, line, column);
                i += 2;
                column += 2;
                continue;
            }
        }

        TokenType type = TOKEN_ERROR;
        switch (c) {
            case '+': type = TOKEN_PLUS;
                break;
            case '-': type = TOKEN_MINUS;
                break;
            case '*': type = TOKEN_STAR;
                break;
            case '/': type = TOKEN_SLASH;
                break;
            case '%': type = TOKEN_PERCENT;
                break;
            case '=': type = TOKEN_EQUAL;
                break;
            case '<': type = TOKEN_LESS;
                break;
            case '>': type = TOKEN_GREATER;
                break;
            case '!': type = TOKEN_BANG;
                break;
            case '?': type = TOKEN_QUESTION;
                break;
            case '(': type = TOKEN_LPAREN;
                break;
            case ')': type = TOKEN_RPAREN;
                break;
            case '{': type = TOKEN_LBRACE;
                break;
            case '}': type = TOKEN_RBRACE;
                break;
            case '[': type = TOKEN_LBRACKET;
                break;
            case ']': type = TOKEN_RBRACKET;
                break;
            case ';': type = TOKEN_SEMICOLON;
                break;
            case ',': type = TOKEN_COMMA;
                break;
            case ':': type = TOKEN_COLON;
                break;
            case '.': type = TOKEN_DOT;
                break;
            case '#': type = TOKEN_HASH;
                break;
            case '@': type = TOKEN_AT;
                break;
            case '&': type = TOKEN_AMPERSAND;
                break;
            default:
                break;
        }

        if (type != TOKEN_ERROR) {
            char op[2] = {c, '\0'};
            add_token(stream, type, op, line, column);
            i++;
            column++;
            continue;
        }

        unsigned char byte = (unsigned char) c;
        unsigned codepoint = byte;
        size_t width = byte >= 0xC2 && byte <= 0xDF
                           ? 2
                           : byte >= 0xE0 && byte <= 0xEF
                                 ? 3
                                 : byte >= 0xF0 && byte <= 0xF4
                                       ? 4
                                       : 1;
        int valid_utf8 = width > 1 && i + width <= length;
        if (valid_utf8) {
            codepoint = byte & (width == 2 ? 0x1F : width == 3 ? 0x0F : 0x07);
            for (size_t k = 1; k < width; k++) {
                unsigned char part = (unsigned char) source[i + k];
                if ((part & 0xC0) != 0x80) valid_utf8 = 0;
                codepoint = (codepoint << 6) | (part & 0x3F);
            }
            if ((width == 3 && codepoint < 0x800) || (width == 4 && codepoint < 0x10000) ||
                codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
                valid_utf8 = 0;
        }
        if (valid_utf8)
            error_report(global_error_handler, SEVERITY_ERROR, line, column,
                         ERROR_CATEGORY_LEXER, ERR_LEX_UNKNOWN_CHAR, filename,
                         "Unknown character U+%04X; identifiers use ASCII letters, digits and '_'", codepoint);
        else if (byte >= 0x80 || byte < 0x20 || byte == 0x7F) {
            width = 1;
            error_report(global_error_handler, SEVERITY_ERROR, line, column,
                         ERROR_CATEGORY_LEXER, ERR_LEX_UNKNOWN_CHAR, filename,
                         "Unknown character byte 0x%02X%s", byte, byte >= 0x80 ? " (invalid UTF-8)" : "");
        } else {
            width = 1;
            error_report(global_error_handler, SEVERITY_ERROR, line, column,
                         ERROR_CATEGORY_LEXER, ERR_LEX_UNKNOWN_CHAR, filename,
                         "Unknown character '%c' (0x%02X)", c, byte);
        }
        stream->has_error = 1;
        i += width;
        column++;
    }

    add_token(stream, TOKEN_EOF, "", line, column);

    return stream;
}

const char *token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD_PACKAGE: return "KEYWORD_PACKAGE";
        case TOKEN_KEYWORD_PUB: return "KEYWORD_PUB";
        case TOKEN_KEYWORD_SIZEOF: return "KEYWORD_SIZEOF";
        case TOKEN_KEYWORD_TYPEOF: return "KEYWORD_TYPEOF";
        case TOKEN_KEYWORD_CASE: return "KEYWORD_CASE";
        case TOKEN_KEYWORD_ALIGNOF: return "KEYWORD_ALIGNOF";
        case TOKEN_KEYWORD_SLICE: return "KEYWORD_SLICE";
        case TOKEN_KEYWORD_FUNC: return "KEYWORD_FUNC";
        case TOKEN_KEYWORD_EXTERN: return "KEYWORD_EXTERN";
        case TOKEN_KEYWORD_FROM: return "KEYWORD_FROM";
        case TOKEN_KEYWORD_EXPORT: return "KEYWORD_EXPORT";
        case TOKEN_KEYWORD_UNION: return "KEYWORD_UNION";
        case TOKEN_KEYWORD_ASYNC: return "KEYWORD_ASYNC";
        case TOKEN_KEYWORD_AWAIT: return "KEYWORD_AWAIT";
        case TOKEN_KEYWORD_VAR: return "KEYWORD_VAR";
        case TOKEN_KEYWORD_RETURN: return "KEYWORD_RETURN";
        case TOKEN_KEYWORD_FOR: return "KEYWORD_FOR";
        case TOKEN_KEYWORD_IF: return "KEYWORD_IF";
        case TOKEN_KEYWORD_ELSE: return "KEYWORD_ELSE";
        case TOKEN_KEYWORD_WHILE: return "KEYWORD_WHILE";
        case TOKEN_KEYWORD_STRUCT: return "KEYWORD_STRUCT";
        case TOKEN_KEYWORD_ENUM: return "KEYWORD_ENUM";
        case TOKEN_KEYWORD_BREAK: return "KEYWORD_BREAK";
        case TOKEN_KEYWORD_CONTINUE: return "KEYWORD_CONTINUE";
        case TOKEN_KEYWORD_IMPORT: return "KEYWORD_IMPORT";
        case TOKEN_KEYWORD_STATIC: return "KEYWORD_STATIC";
        case TOKEN_KEYWORD_RESERVE: return "KEYWORD_RESERVE";
        case TOKEN_KEYWORD_FREE: return "KEYWORD_FREE";
        case TOKEN_KEYWORD_CONST: return "KEYWORD_CONST";
        case TOKEN_KEYWORD_INTERFACE: return "KEYWORD_INTERFACE";
        case TOKEN_KEYWORD_MATCH: return "KEYWORD_MATCH";
        case TOKEN_KEYWORD_DESTRUCTOR: return "KEYWORD_DESTRUCTOR";
        case TOKEN_KEYWORD_DEFER: return "KEYWORD_DEFER";
        case TOKEN_KEYWORD_MUT: return "KEYWORD_MUT";

        case TOKEN_TYPE_INT: return "TYPE_INT";
        case TOKEN_TYPE_I8: return "TYPE_I8";
        case TOKEN_TYPE_U8: return "TYPE_U8";
        case TOKEN_TYPE_I16: return "TYPE_I16";
        case TOKEN_TYPE_U16: return "TYPE_U16";
        case TOKEN_TYPE_I32: return "TYPE_I32";
        case TOKEN_TYPE_U32: return "TYPE_U32";
        case TOKEN_TYPE_I64: return "TYPE_I64";
        case TOKEN_TYPE_U64: return "TYPE_U64";
        case TOKEN_TYPE_ISIZE: return "TYPE_ISIZE";
        case TOKEN_TYPE_USIZE: return "TYPE_USIZE";
        case TOKEN_TYPE_CHAR: return "TYPE_CHAR";
        case TOKEN_TYPE_BYTE: return "TYPE_BYTE";
        case TOKEN_TYPE_BIT: return "TYPE_BIT";
        case TOKEN_TYPE_FLOAT: return "TYPE_FLOAT";
        case TOKEN_TYPE_DOUBLE: return "TYPE_DOUBLE";
        case TOKEN_TYPE_STRING: return "TYPE_STRING";
        case TOKEN_TYPE_VOID: return "TYPE_VOID";
        case TOKEN_TYPE_NEVER: return "TYPE_NEVER";

        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_FLOAT_LITERAL: return "FLOAT";
        case TOKEN_CHAR_LITERAL: return "CHAR";
        case TOKEN_STRING_LITERAL: return "STRING";

        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_PERCENT: return "PERCENT";

        case TOKEN_EQUAL: return "EQUAL";
        case TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TOKEN_BANG_EQUAL: return "BANG_EQUAL";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";

        case TOKEN_AMP_AMP: return "AMP_AMP";
        case TOKEN_PIPE_PIPE: return "PIPE_PIPE";
        case TOKEN_BANG: return "BANG";
        case TOKEN_AMPERSAND: return "AMPERSAND";

        case TOKEN_PLUS_EQUAL: return "PLUS_EQUAL";
        case TOKEN_MINUS_EQUAL: return "MINUS_EQUAL";
        case TOKEN_STAR_EQUAL: return "STAR_EQUAL";
        case TOKEN_SLASH_EQUAL: return "SLASH_EQUAL";
        case TOKEN_PLUS_PLUS: return "PLUS_PLUS";
        case TOKEN_MINUS_MINUS: return "MINUS_MINUS";

        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_LBRACKET: return "LBRACKET";
        case TOKEN_RBRACKET: return "RBRACKET";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_COLON: return "COLON";
        case TOKEN_ARROW: return "ARROW";
        case TOKEN_FAT_ARROW: return "FAT_ARROW";
        case TOKEN_DOT: return "DOT";
        case TOKEN_QUESTION: return "QUESTION";
        case TOKEN_HASH: return "HASH";
        case TOKEN_AT: return "AT";
        case TOKEN_COMMENT: return "COMMENT";
        case TOKEN_NEWLINE: return "NEWLINE";

        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void print_tokens(const TokenStream *stream) {
    printf("\n");
    printf("================================================================\n");
    printf("                        TOKEN STREAM\n");
    printf("================================================================\n");
    printf("\n");
    printf("%-20s %-20s %-10s\n", "Type", "Value", "Position");
    printf("----------------------------------------------------------------\n");

    for (int i = 0; i < stream->count; i++) {
        Token *token = &stream->tokens[i];
        printf("%-20s %-20s Line: %3d Col: %3d\n",
               token_type_to_string(token->type),
               token->value,
               token->line,
               token->column);
    }

    printf("\nTotal Tokens: %d\n", stream->count);
    printf("================================================================\n");
    printf("\n");
}
