#ifndef COMPILER_TYPES_H
#define COMPILER_TYPES_H

#define MAX_VARS 200
#define MAX_LINE 1024
#define MAX_TOKEN 512
#define MAX_FUNCTIONS 100
#define MAX_TOKENS 10000

// Data Types
typedef enum {
    TYPE_INT,
    TYPE_STRING,
    TYPE_VOID,
    TYPE_UNKNOWN
} DataType;

// Token Types
typedef enum {
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_PRINT,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_WHILE,

    TOKEN_TYPE_INT,
    TOKEN_TYPE_STRING,
    TOKEN_TYPE_VOID,

    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING_LITERAL,

    TOKEN_PLUS,           // +
    TOKEN_MINUS,          // -
    TOKEN_STAR,           // *
    TOKEN_SLASH,          // /
    TOKEN_PERCENT,        // %

    TOKEN_PLUS_PLUS,      // ++
    TOKEN_MINUS_MINUS,    // --
    TOKEN_PLUS_EQUAL,     // +=
    TOKEN_MINUS_EQUAL,    // -=
    TOKEN_STAR_EQUAL,     // *=
    TOKEN_SLASH_EQUAL,    // /=

    TOKEN_EQUAL,          // =
    TOKEN_EQUAL_EQUAL,    // ==
    TOKEN_BANG_EQUAL,     // !=
    TOKEN_LESS,           // <
    TOKEN_LESS_EQUAL,     // <=
    TOKEN_GREATER,        // >
    TOKEN_GREATER_EQUAL,  // >=

    TOKEN_LPAREN,         // (
    TOKEN_RPAREN,         // )
    TOKEN_LBRACE,         // {
    TOKEN_RBRACE,         // }
    TOKEN_SEMICOLON,      // ;
    TOKEN_COMMA,          // ,
    TOKEN_COLON,          // :
    TOKEN_ARROW,          // ->

    TOKEN_COMMENT,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

// Token Structure
typedef struct {
    TokenType type;
    char value[MAX_TOKEN];
    int line;
    int column;
} Token;

// Token Stream
typedef struct {
    Token *tokens;
    int count;
    int current;
    int capacity;
} TokenStream;

// Variable
typedef struct {
    char name[MAX_TOKEN];
    int offset;
    int scope;
    DataType type;
} Variable;

// String Literal
typedef struct {
    int id;
    char text[MAX_LINE];
} StringLiteral;

// Function
typedef struct {
    char name[MAX_TOKEN];
    int param_count;
    char params[10][MAX_TOKEN];
    DataType param_types[10];
    DataType return_type;
} Function;

#endif