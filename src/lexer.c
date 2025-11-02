#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define INITIAL_CAPACITY 1000

// ============================================================================
// TOKEN STREAM MANAGEMENT
// ============================================================================

TokenStream *create_token_stream(void) {
    TokenStream *stream = malloc(sizeof(TokenStream));
    stream->capacity = INITIAL_CAPACITY;
    stream->count = 0;
    stream->current = 0;
    stream->tokens = malloc(sizeof(Token) * stream->capacity);
    return stream;
}

void free_token_stream(TokenStream *stream) {
    if (stream) {
        free(stream->tokens);
        free(stream);
    }
}

void add_token(TokenStream *stream, TokenType type, const char *value, int line, int column) {
    if (stream->count >= stream->capacity) {
        stream->capacity *= 2;
        stream->tokens = realloc(stream->tokens, sizeof(Token) * stream->capacity);
    }

    Token *token = &stream->tokens[stream->count];
    token->type = type;
    token->line = line;
    token->column = column;

    if (value) {
        strncpy(token->value, value, MAX_TOKEN - 1);
        token->value[MAX_TOKEN - 1] = '\0';
    } else {
        token->value[0] = '\0';
    }

    stream->count++;
}

// ============================================================================
// TOKEN STREAM NAVIGATION
// ============================================================================

Token peek(TokenStream *stream) {
    if (stream->current < stream->count) {
        return stream->tokens[stream->current];
    }
    Token eof = {TOKEN_EOF, "", 0, 0};
    return eof;
}

Token peek_ahead(TokenStream *stream, int offset) {
    int pos = stream->current + offset;
    if (pos < stream->count) {
        return stream->tokens[pos];
    }
    Token eof = {TOKEN_EOF, "", 0, 0};
    return eof;
}

Token consume(TokenStream *stream) {
    if (stream->current < stream->count) {
        return stream->tokens[stream->current++];
    }
    Token eof = {TOKEN_EOF, "", 0, 0};
    return eof;
}

int check(TokenStream *stream, TokenType type) {
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

int is_at_end(TokenStream *stream) {
    return stream->current >= stream->count ||
           stream->tokens[stream->current].type == TOKEN_EOF;
}

// ============================================================================
// KEYWORD RECOGNITION
// ============================================================================

static TokenType get_keyword_type(const char *str) {
    // Keywords
    if (strcmp(str, "func") == 0) return TOKEN_KEYWORD_FUNC;
    if (strcmp(str, "var") == 0) return TOKEN_KEYWORD_VAR;
    if (strcmp(str, "return") == 0) return TOKEN_KEYWORD_RETURN;
    if (strcmp(str, "for") == 0) return TOKEN_KEYWORD_FOR;
    if (strcmp(str, "print") == 0) return TOKEN_KEYWORD_PRINT;
    if (strcmp(str, "if") == 0) return TOKEN_KEYWORD_IF;
    if (strcmp(str, "else") == 0) return TOKEN_KEYWORD_ELSE;
    if (strcmp(str, "while") == 0) return TOKEN_KEYWORD_WHILE;
    if (strcmp(str, "break") == 0) return TOKEN_KEYWORD_BREAK;
    if (strcmp(str, "continue") == 0) return TOKEN_KEYWORD_CONTINUE;
    if (strcmp(str, "struct") == 0) return TOKEN_KEYWORD_STRUCT;

    // Data Types (ERWEITERT!)
    if (strcmp(str, "int") == 0) return TOKEN_TYPE_INT;
    if (strcmp(str, "char") == 0) return TOKEN_TYPE_CHAR;
    if (strcmp(str, "byte") == 0) return TOKEN_TYPE_BYTE;
    if (strcmp(str, "bit") == 0) return TOKEN_TYPE_BIT;
    if (strcmp(str, "float") == 0) return TOKEN_TYPE_FLOAT;
    if (strcmp(str, "double") == 0) return TOKEN_TYPE_DOUBLE;
    if (strcmp(str, "string") == 0) return TOKEN_TYPE_STRING;
    if (strcmp(str, "void") == 0) return TOKEN_TYPE_VOID;

    return TOKEN_IDENTIFIER;
}

// ============================================================================
// TOKENIZATION
// ============================================================================

TokenStream *tokenize_file(const char *filename, int debug_mode) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Fehler: Datei '%s' konnte nicht geöffnet werden\n", filename);
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *source = malloc(file_size + 1);
    size_t bytes_read = fread(source, 1, file_size, file);
    source[bytes_read] = '\0';
    fclose(file);

    TokenStream *stream = create_token_stream();

    int line = 1;
    int column = 1;
    size_t i = 0;
    size_t length = bytes_read;

    while (i < length) {
        char c = source[i];

        // Skip whitespace
        if (c == ' ' || c == '\t' || c == '\r') {
            i++;
            column++;
            continue;
        }

        // Newline
        if (c == '\n') {
            i++;
            line++;
            column = 1;
            continue;
        }

        // Comments
        if (c == '/' && i + 1 < length && source[i + 1] == '/') {
            while (i < length && source[i] != '\n') {
                i++;
            }
            continue;
        }

        // String literals
        if (c == '"') {
            int start_col = column;
            int start_line = line;
            i++;
            column++;

            char str[MAX_LINE] = {0};
            int j = 0;

            while (i < length && source[i] != '"' && j < MAX_LINE - 1) {
                if (source[i] == '\n') {
                    line++;
                    column = 1;
                } else if (source[i] == '\\' && i + 1 < length) {
                    i++;
                    column++;

                    switch (source[i]) {
                        case 'n': str[j++] = '\n'; break;
                        case 't': str[j++] = '\t'; break;
                        case 'r': str[j++] = '\r'; break;
                        case '0': str[j++] = '\0'; break;
                        case '\\': str[j++] = '\\'; break;
                        case '"': str[j++] = '"'; break;
                        default: str[j++] = source[i]; break;
                    }
                } else {
                    unsigned char byte = (unsigned char)source[i];
                    if (byte >= 0x80) {
                        str[j++] = source[i++];
                        if (i < length && (source[i] & 0xC0) == 0x80) {
                            str[j++] = source[i++];
                        }
                        if (i < length && (source[i] & 0xC0) == 0x80) {
                            str[j++] = source[i++];
                        }
                        column++;
                        continue;
                    } else {
                        str[j++] = source[i];
                    }
                }
                i++;
                column++;
            }

            str[j] = '\0';

            if (i < length && source[i] == '"') {
                i++;
                column++;
                add_token(stream, TOKEN_STRING_LITERAL, str, start_line, start_col);
            } else {
                fprintf(stderr, "Fehler (Zeile %d, Spalte %d): Ungeschlossenes String-Literal\n",
                        start_line, start_col);
            }

            continue;
        }

        // Character Literals - 'A', 'b', '\n', etc.
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
                        case 'n':  ch[j++] = '\n'; break;
                        case 't':  ch[j++] = '\t'; break;
                        case 'r':  ch[j++] = '\r'; break;
                        case '0':  ch[j++] = '\0'; break;
                        case '\\': ch[j++] = '\\'; break;
                        case '\'': ch[j++] = '\''; break;
                        default:
                            fprintf(stderr, "Warnung (Zeile %d): Unbekannte Escape-Sequenz '\\%c'\n",
                                    line, esc);
                            ch[j++] = esc;
                    }
                    i++;
                    column++;
                }
            } else if (i < length && source[i] != '\'') {
                ch[j++] = source[i++];
                column++;
            }

            ch[j] = '\0';

            if (i < length && source[i] == '\'') {
                i++;
                column++;
                add_token(stream, TOKEN_CHAR_LITERAL, ch, start_line, start_col);
            } else {
                fprintf(stderr, "Fehler (Zeile %d, Spalte %d): Ungeschlossenes Character-Literal\n",
                        start_line, start_col);
            }

            continue;
        }

        // Numbers (Integer and Float)
        if (isdigit(c) || (c == '-' && i + 1 < length && isdigit((unsigned char)source[i + 1]))) {
            int start_col = column;
            char num[MAX_TOKEN];
            int j = 0;
            int has_dot = 0;
            int is_negative = 0;

            if (c == '-') {
                num[j++] = c;
                is_negative = 1;
                i++;
                column++;
                c = source[i];
            }

            while (i < length && (isdigit((unsigned char)source[i]) || source[i] == '.') && j < MAX_TOKEN - 1) {
                if (source[i] == '.') {
                    if (has_dot) break;
                    has_dot = 1;
                }
                num[j++] = source[i++];
                column++;
            }

            // Scientific notation
            if (i < length && (source[i] == 'e' || source[i] == 'E')) {
                num[j++] = source[i++];
                column++;

                if (i < length && (source[i] == '+' || source[i] == '-')) {
                    num[j++] = source[i++];
                    column++;
                }

                while (i < length && isdigit((unsigned char)source[i]) && j < MAX_TOKEN - 1) {
                    num[j++] = source[i++];
                    column++;
                }

                has_dot = 1;  // Scientific notation means float
            }

            num[j] = '\0';

            if (has_dot) {
                add_token(stream, TOKEN_FLOAT_LITERAL, num, line, start_col);
            } else {
                add_token(stream, TOKEN_NUMBER, num, line, start_col);
            }

            continue;
        }

        // Identifiers and keywords
        if (isalpha(c) || c == '_') {
            int start_col = column;
            char ident[MAX_TOKEN];
            int j = 0;

            while (i < length && (isalnum((unsigned char)source[i]) || source[i] == '_') && j < MAX_TOKEN - 1) {
                ident[j++] = source[i++];
                column++;
            }

            ident[j] = '\0';

            TokenType type = get_keyword_type(ident);
            add_token(stream, type, ident, line, start_col);

            continue;
        }

        // Two-character operators
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

        // Single-character operators and delimiters
        TokenType type = TOKEN_ERROR;
        switch (c) {
            case '+': type = TOKEN_PLUS; break;
            case '-': type = TOKEN_MINUS; break;
            case '*': type = TOKEN_STAR; break;
            case '/': type = TOKEN_SLASH; break;
            case '%': type = TOKEN_PERCENT; break;
            case '=': type = TOKEN_EQUAL; break;
            case '<': type = TOKEN_LESS; break;
            case '>': type = TOKEN_GREATER; break;
            case '!': type = TOKEN_BANG; break;
            case '(': type = TOKEN_LPAREN; break;
            case ')': type = TOKEN_RPAREN; break;
            case '{': type = TOKEN_LBRACE; break;
            case '}': type = TOKEN_RBRACE; break;
            case '[': type = TOKEN_LBRACKET; break;
            case ']': type = TOKEN_RBRACKET; break;
            case ';': type = TOKEN_SEMICOLON; break;
            case ',': type = TOKEN_COMMA; break;
            case ':': type = TOKEN_COLON; break;
            case '.': type = TOKEN_DOT; break;
        }

        if (type != TOKEN_ERROR) {
            char op[2] = {c, '\0'};
            add_token(stream, type, op, line, column);
            i++;
            column++;
            continue;
        }

        fprintf(stderr, "Warnung (Zeile %d, Spalte %d): Unbekanntes Zeichen '%c' (0x%02X)\n",
                line, column, c, (unsigned char)c);
        i++;
        column++;
    }

    // Cleanup: Remove any garbage tokens after last valid token
    int last_valid = stream->count - 1;
    while (last_valid >= 0) {
        Token *t = &stream->tokens[last_valid];
        if (t->type != TOKEN_ERROR &&
            t->type != TOKEN_EOF &&
            strlen(t->value) > 0 &&
            (t->type == TOKEN_RBRACE ||
             t->type == TOKEN_SEMICOLON ||
             t->type == TOKEN_RPAREN ||
             isalnum((unsigned char)t->value[0]) ||
             t->value[0] == '_' ||
             t->value[0] == '"')) {
            break;
        }
        last_valid--;
    }

    if (last_valid >= 0 && last_valid < stream->count - 1) {
        if (debug_mode) {
            fprintf(stderr, "[LEXER] Entferne %d Token(s) nach letztem gültigen Token\n",
                    stream->count - last_valid - 1);
        }
        stream->count = last_valid + 1;
    }

    add_token(stream, TOKEN_EOF, "", line, column);

    free(source);
    return stream;
}

// ============================================================================
// TOKEN PRINTING
// ============================================================================

const char *token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_KEYWORD_FUNC: return "KEYWORD_FUNC";
        case TOKEN_KEYWORD_VAR: return "KEYWORD_VAR";
        case TOKEN_KEYWORD_RETURN: return "KEYWORD_RETURN";
        case TOKEN_KEYWORD_FOR: return "KEYWORD_FOR";
        case TOKEN_KEYWORD_IF: return "KEYWORD_IF";
        case TOKEN_KEYWORD_ELSE: return "KEYWORD_ELSE";
        case TOKEN_KEYWORD_WHILE: return "KEYWORD_WHILE";
        case TOKEN_KEYWORD_PRINT: return "KEYWORD_PRINT";
        case TOKEN_KEYWORD_STRUCT: return "KEYWORD_STRUCT";
        case TOKEN_KEYWORD_BREAK: return "KEYWORD_BREAK";
        case TOKEN_KEYWORD_CONTINUE: return "KEYWORD_CONTINUE";

        case TOKEN_TYPE_INT: return "TYPE_INT";
        case TOKEN_TYPE_CHAR: return "TYPE_CHAR";
        case TOKEN_TYPE_BYTE: return "TYPE_BYTE";
        case TOKEN_TYPE_BIT: return "TYPE_BIT";
        case TOKEN_TYPE_FLOAT: return "TYPE_FLOAT";
        case TOKEN_TYPE_DOUBLE: return "TYPE_DOUBLE";
        case TOKEN_TYPE_STRING: return "TYPE_STRING";
        case TOKEN_TYPE_VOID: return "TYPE_VOID";

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
        case TOKEN_DOT: return "DOT";

        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void print_tokens(TokenStream *stream) {
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