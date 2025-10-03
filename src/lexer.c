#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Create Token Stream
TokenStream *create_token_stream(int initial_capacity) {
    TokenStream *stream = malloc(sizeof(TokenStream));
    stream->tokens = malloc(sizeof(Token) * initial_capacity);
    stream->count = 0;
    stream->current = 0;
    stream->capacity = initial_capacity;
    return stream;
}

// Add Token to Stream
void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column) {
    if (stream->count >= stream->capacity) {
        stream->capacity *= 2;
        stream->tokens = realloc(stream->tokens, sizeof(Token) * stream->capacity);
    }

    Token *token = &stream->tokens[stream->count++];
    token->type = type;
    strncpy(token->value, value, MAX_TOKEN - 1);
    token->value[MAX_TOKEN - 1] = '\0';
    token->line = line;
    token->column = column;
}

// Check if character is valid identifier start
static int is_identifier_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

// Check if character is valid identifier continuation
static int is_identifier_continue(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// Check if byte is UTF-8 continuation byte
static int is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;  // Starts with 10xxxxxx
}

// Get length of UTF-8 character in bytes
static int get_utf8_char_length(const char *source, int i, int length) {
    if (i >= length) return 0;

    unsigned char c = (unsigned char)source[i];

    // Single byte (ASCII 0-127)
    if (c < 0x80) {
        return 1;
    }
    // 2-byte sequence (110xxxxx 10xxxxxx)
    else if ((c & 0xE0) == 0xC0) {
        if (i + 1 < length && is_utf8_continuation((unsigned char)source[i + 1])) {
            return 2;
        }
        return 1;  // Invalid, treat as single byte
    }
    // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
    else if ((c & 0xF0) == 0xE0) {
        if (i + 2 < length &&
            is_utf8_continuation((unsigned char)source[i + 1]) &&
            is_utf8_continuation((unsigned char)source[i + 2])) {
            return 3;
        }
        return 1;  // Invalid, treat as single byte
    }
    // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
    else if ((c & 0xF8) == 0xF0) {
        if (i + 3 < length &&
            is_utf8_continuation((unsigned char)source[i + 1]) &&
            is_utf8_continuation((unsigned char)source[i + 2]) &&
            is_utf8_continuation((unsigned char)source[i + 3])) {
            return 4;
        }
        return 1;  // Invalid, treat as single byte
    }

    // Invalid UTF-8 start byte
    return 1;
}

// Get keyword type or TOKEN_IDENTIFIER
static TokenType get_keyword_type(const char *str) {
    if (strcmp(str, "func") == 0) return TOKEN_KEYWORD_FUNC;
    if (strcmp(str, "var") == 0) return TOKEN_KEYWORD_VAR;
    if (strcmp(str, "return") == 0) return TOKEN_KEYWORD_RETURN;
    if (strcmp(str, "for") == 0) return TOKEN_KEYWORD_FOR;
    if (strcmp(str, "print") == 0) return TOKEN_KEYWORD_PRINT;
    if (strcmp(str, "if") == 0) return TOKEN_KEYWORD_IF;
    if (strcmp(str, "else") == 0) return TOKEN_KEYWORD_ELSE;
    if (strcmp(str, "while") == 0) return TOKEN_KEYWORD_WHILE;

    if (strcmp(str, "int") == 0) return TOKEN_TYPE_INT;
    if (strcmp(str, "string") == 0) return TOKEN_TYPE_STRING;
    if (strcmp(str, "void") == 0) return TOKEN_TYPE_VOID;

    return TOKEN_IDENTIFIER;
}

// Main Tokenizer
TokenStream *tokenize_source(const char *source) {
    TokenStream *stream = create_token_stream(256);

    int line = 1;
    int column = 1;
    int i = 0;
    int length = strlen(source);

    while (i < length) {
        unsigned char c = (unsigned char)source[i];

        // Check for UTF-8 multibyte characters (non-ASCII) OUTSIDE of strings
        if (c > 127) {
            int bytes = get_utf8_char_length(source, i, length);

            // Extract UTF-8 character for warning message
            char utf8_char[5] = {0};
            for (int j = 0; j < bytes && j < 4; j++) {
                utf8_char[j] = source[i + j];
            }

            fprintf(stderr, "Warnung (Zeile %d, Spalte %d): Unicode-Zeichen wird ignoriert (nicht in String)\n",
                    line, column);

            i += bytes;
            column++;
            continue;
        }

        // Whitespace (except newline)
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            column++;
            continue;
        }

        // Newline
        if (c == '\n') {
            line++;
            column = 1;
            i++;
            continue;
        }

        // Comments - SKIP completely
        if (c == '/' && i + 1 < length && source[i + 1] == '/') {
            i += 2;
            column += 2;

            // Skip until end of line
            while (i < length && source[i] != '\n') {
                i++;
                column++;
            }

            continue;
        }

        // String Literals - UTF-8 ALLOWED inside strings
        if (c == '"') {
            int start_col = column;
            int start_line = line;
            i++;
            column++;

            char str[MAX_TOKEN];
            int j = 0;
            int string_too_long = 0;

            // Parse until closing quote, newline, or end of file
            while (i < length) {
                unsigned char sc = (unsigned char)source[i];

                // Check for closing quote FIRST
                if (sc == '"') {
                    break;
                }

                // Check for newline (unclosed string)
                if (sc == '\n') {
                    break;
                }

                // Check if we have space left in buffer
                if (j >= MAX_TOKEN - 10) {
                    string_too_long = 1;
                    break;
                }

                // Handle UTF-8 multi-byte characters
                if (sc >= 0x80) {
                    // This is a UTF-8 multi-byte character
                    int bytes = get_utf8_char_length(source, i, length);

                    // Copy all bytes of the UTF-8 character to the string
                    for (int b = 0; b < bytes && j < MAX_TOKEN - 10 && i < length; b++) {
                        str[j++] = source[i++];
                    }
                    column++;  // Count as one visual character
                } else {
                    // Regular ASCII character
                    str[j++] = source[i++];
                    column++;
                }
            }

            // Null-terminate the string
            str[j] = '\0';

            // Check why we exited the loop
            if (i < length && source[i] == '"') {
                // Success - found closing quote
                i++;
                column++;
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
            } else if (string_too_long) {
                fprintf(stderr, "Fehler (Zeile %d, Spalte %d): String zu lang (max %d Zeichen)\n",
                        start_line, start_col, MAX_TOKEN - 10);
                // Skip to closing quote or newline
                while (i < length && source[i] != '"' && source[i] != '\n') {
                    unsigned char skip_c = (unsigned char)source[i];
                    if (skip_c >= 0x80) {
                        i += get_utf8_char_length(source, i, length);
                    } else {
                        i++;
                    }
                }
                if (i < length && source[i] == '"') {
                    i++;
                }
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
            } else if (i >= length) {
                fprintf(stderr, "Fehler (Zeile %d, Spalte %d): Ungeschlossene String-Literal (EOF)\n",
                        start_line, start_col);
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
            } else if (i < length && source[i] == '\n') {
                fprintf(stderr, "Fehler (Zeile %d, Spalte %d): Ungeschlossene String-Literal (Zeilenende)\n",
                        start_line, start_col);
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
            }

            continue;
        }

        // Numbers (including negative)
        if (isdigit(c) || (c == '-' && i + 1 < length && isdigit((unsigned char)source[i + 1]))) {
            int start_col = column;
            char num[MAX_TOKEN];
            int j = 0;

            if (c == '-') {
                num[j++] = source[i++];
                column++;
            }

            while (i < length && isdigit((unsigned char)source[i]) && j < MAX_TOKEN - 1) {
                num[j++] = source[i++];
                column++;
            }
            num[j] = '\0';

            add_token(stream, TOKEN_NUMBER, num, line, start_col);
            continue;
        }

        // Identifiers and Keywords
        if (is_identifier_start(c)) {
            int start_col = column;
            char id[MAX_TOKEN];
            int j = 0;

            while (i < length && is_identifier_continue((unsigned char)source[i]) && j < MAX_TOKEN - 1) {
                id[j++] = source[i++];
                column++;
            }
            id[j] = '\0';

            TokenType type = get_keyword_type(id);
            add_token(stream, type, id, line, start_col);
            continue;
        }

        // Two-character operators
        if (i + 1 < length) {
            char next = source[i + 1];
            char two_char[3] = {c, next, '\0'};
            TokenType type = TOKEN_ERROR;

            if (c == '+' && next == '+') type = TOKEN_PLUS_PLUS;
            else if (c == '-' && next == '-') type = TOKEN_MINUS_MINUS;
            else if (c == '+' && next == '=') type = TOKEN_PLUS_EQUAL;
            else if (c == '-' && next == '=') type = TOKEN_MINUS_EQUAL;
            else if (c == '*' && next == '=') type = TOKEN_STAR_EQUAL;
            else if (c == '/' && next == '=') type = TOKEN_SLASH_EQUAL;
            else if (c == '=' && next == '=') type = TOKEN_EQUAL_EQUAL;
            else if (c == '!' && next == '=') type = TOKEN_BANG_EQUAL;
            else if (c == '<' && next == '=') type = TOKEN_LESS_EQUAL;
            else if (c == '>' && next == '=') type = TOKEN_GREATER_EQUAL;
            else if (c == '-' && next == '>') type = TOKEN_ARROW;

            if (type != TOKEN_ERROR) {
                add_token(stream, type, two_char, line, column);
                i += 2;
                column += 2;
                continue;
            }
        }

        // Single-character operators and delimiters
        TokenType type = TOKEN_ERROR;
        char single[2] = {c, '\0'};

        switch (c) {
            case '+': type = TOKEN_PLUS; break;
            case '-': type = TOKEN_MINUS; break;
            case '*': type = TOKEN_STAR; break;
            case '/': type = TOKEN_SLASH; break;
            case '%': type = TOKEN_PERCENT; break;
            case '=': type = TOKEN_EQUAL; break;
            case '<': type = TOKEN_LESS; break;
            case '>': type = TOKEN_GREATER; break;
            case '(': type = TOKEN_LPAREN; break;
            case ')': type = TOKEN_RPAREN; break;
            case '{': type = TOKEN_LBRACE; break;
            case '}': type = TOKEN_RBRACE; break;
            case ';': type = TOKEN_SEMICOLON; break;
            case ',': type = TOKEN_COMMA; break;
            case ':': type = TOKEN_COLON; break;
            case '!':
                // Standalone ! (not part of !=)
                // Skip it silently (it's allowed in strings)
                i++;
                column++;
                continue;
            case '\\':
                // Backslash - skip silently
                i++;
                column++;
                continue;
            default:
                // Unknown ASCII character
                if (c >= 32 && c <= 126) {
                    fprintf(stderr, "Warnung (Zeile %d, Spalte %d): Unbekanntes Zeichen '%c' wird ignoriert\n",
                            line, column, c);
                } else {
                    fprintf(stderr, "Warnung (Zeile %d, Spalte %d): Kontroll-Zeichen (ASCII: %d) wird ignoriert\n",
                            line, column, c);
                }
                i++;
                column++;
                continue;
        }

        if (type != TOKEN_ERROR) {
            add_token(stream, type, single, line, column);
        }

        i++;
        column++;
    }

    // Post-processing: Clean up token stream
    int brace_depth = 0;
    int func_count = 0;
    int last_valid_token = stream->count - 1;

    for (int check = 0; check < stream->count; check++) {
        if (stream->tokens[check].type == TOKEN_KEYWORD_FUNC) {
            func_count++;
        }
        if (stream->tokens[check].type == TOKEN_LBRACE) {
            brace_depth++;
        }
        if (stream->tokens[check].type == TOKEN_RBRACE) {
            brace_depth--;
            if (func_count > 0 && brace_depth == 0) {
                last_valid_token = check;
            }
        }
    }

    // Truncate garbage after last valid function
    if (func_count > 0 && brace_depth == 0 && last_valid_token < stream->count - 1) {
        int has_garbage = 0;
        for (int check = last_valid_token + 1; check < stream->count; check++) {
            TokenType t = stream->tokens[check].type;
            if (t != TOKEN_SEMICOLON) {
                has_garbage = 1;
                break;
            }
        }

        if (has_garbage) {
            fprintf(stderr, "Warnung: Ignoriere %d Token nach Ende des Programms (Zeile %d)\n",
                    stream->count - last_valid_token - 1,
                    stream->tokens[last_valid_token].line);
            stream->count = last_valid_token + 1;
        }
    }

    // Add EOF token
    add_token(stream, TOKEN_EOF, "", line, column);

    return stream;
}

// Free Token Stream
void free_token_stream(TokenStream *stream) {
    if (stream) {
        free(stream->tokens);
        free(stream);
    }
}

// Token Stream Navigation
Token peek(TokenStream *stream) {
    if (stream->current >= stream->count) {
        return stream->tokens[stream->count - 1];
    }
    return stream->tokens[stream->current];
}

Token peek_ahead(TokenStream *stream, int offset) {
    int index = stream->current + offset;
    if (index >= stream->count) {
        return stream->tokens[stream->count - 1];
    }
    return stream->tokens[index];
}

Token consume(TokenStream *stream) {
    if (stream->current >= stream->count) {
        return stream->tokens[stream->count - 1];
    }
    return stream->tokens[stream->current++];
}

int match(TokenStream *stream, TokenType type) {
    if (check(stream, type)) {
        consume(stream);
        return 1;
    }
    return 0;
}

int check(TokenStream *stream, TokenType type) {
    if (is_at_end(stream)) return 0;
    return peek(stream).type == type;
}

int is_at_end(TokenStream *stream) {
    return peek(stream).type == TOKEN_EOF;
}

// Utility Functions
const char *token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD_FUNC: return "KEYWORD_FUNC";
        case TOKEN_KEYWORD_VAR: return "KEYWORD_VAR";
        case TOKEN_KEYWORD_RETURN: return "KEYWORD_RETURN";
        case TOKEN_KEYWORD_FOR: return "KEYWORD_FOR";
        case TOKEN_KEYWORD_PRINT: return "KEYWORD_PRINT";
        case TOKEN_KEYWORD_IF: return "KEYWORD_IF";
        case TOKEN_KEYWORD_ELSE: return "KEYWORD_ELSE";
        case TOKEN_KEYWORD_WHILE: return "KEYWORD_WHILE";
        case TOKEN_TYPE_INT: return "TYPE_INT";
        case TOKEN_TYPE_STRING: return "TYPE_STRING";
        case TOKEN_TYPE_VOID: return "TYPE_VOID";
        case TOKEN_IDENTIFIER: return "IDENTIFIER";
        case TOKEN_NUMBER: return "NUMBER";
        case TOKEN_STRING_LITERAL: return "STRING";
        case TOKEN_PLUS: return "PLUS";
        case TOKEN_MINUS: return "MINUS";
        case TOKEN_STAR: return "STAR";
        case TOKEN_SLASH: return "SLASH";
        case TOKEN_PERCENT: return "PERCENT";
        case TOKEN_PLUS_PLUS: return "PLUS_PLUS";
        case TOKEN_MINUS_MINUS: return "MINUS_MINUS";
        case TOKEN_PLUS_EQUAL: return "PLUS_EQUAL";
        case TOKEN_MINUS_EQUAL: return "MINUS_EQUAL";
        case TOKEN_STAR_EQUAL: return "STAR_EQUAL";
        case TOKEN_SLASH_EQUAL: return "SLASH_EQUAL";
        case TOKEN_EQUAL: return "EQUAL";
        case TOKEN_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TOKEN_BANG_EQUAL: return "BANG_EQUAL";
        case TOKEN_LESS: return "LESS";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER: return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_LPAREN: return "LPAREN";
        case TOKEN_RPAREN: return "RPAREN";
        case TOKEN_LBRACE: return "LBRACE";
        case TOKEN_RBRACE: return "RBRACE";
        case TOKEN_SEMICOLON: return "SEMICOLON";
        case TOKEN_COMMA: return "COMMA";
        case TOKEN_COLON: return "COLON";
        case TOKEN_ARROW: return "ARROW";
        case TOKEN_COMMENT: return "COMMENT";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void print_token(Token token) {
    printf("%-20s %-20s Line: %3d Col: %3d\n",
           token_type_to_string(token.type),
           token.value,
           token.line,
           token.column);
}

void print_all_tokens(TokenStream *stream) {
    printf("\n");
    printf("================================================================\n");
    printf("                        TOKEN STREAM\n");
    printf("================================================================\n\n");
    printf("%-20s %-20s %-10s\n", "Type", "Value", "Position");
    printf("----------------------------------------------------------------\n");

    for (int i = 0; i < stream->count; i++) {
        Token token = stream->tokens[i];
        if (token.type != TOKEN_COMMENT) {
            print_token(token);
        }
    }

    printf("\nTotal Tokens: %d\n", stream->count);
    printf("================================================================\n\n");
}