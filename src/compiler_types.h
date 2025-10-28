#ifndef COMPILER_TYPES_H
#define COMPILER_TYPES_H

#define MAX_TOKEN 512
#define MAX_LINE 1024
#define MAX_VARS 400
#define MAX_FUNCTIONS 200
#define MAX_STRING_LITERALS 5000
#define MAX_FLOAT_LITERALS 5000
#define CODE_BUFFER_SIZE 524288
#define MAX_LOOP_DEPTH 100

// ============================================================================
// TOKEN TYPES
// ============================================================================

typedef enum {
    // Keywords
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_PRINT,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_CONTINUE,

    // Data Types (ERWEITERT!)
    TOKEN_TYPE_INT,
    TOKEN_TYPE_CHAR,        // NEU
    TOKEN_TYPE_BYTE,        // NEU
    TOKEN_TYPE_BIT,         // NEU
    TOKEN_TYPE_FLOAT,       // NEU
    TOKEN_TYPE_DOUBLE,      // NEU
    TOKEN_TYPE_STRING,
    TOKEN_TYPE_VOID,

    // Literals
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_FLOAT_LITERAL,    // NEU: 3.14
    TOKEN_CHAR_LITERAL,     // NEU: 'A'
    TOKEN_STRING_LITERAL,

    // Operators (Arithmetic)
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,

    // Operators (Comparison)
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,

    // Operators (Logical)
    TOKEN_AMP_AMP,
    TOKEN_PIPE_PIPE,
    TOKEN_BANG,

    // Operators (Assignment)
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,

    // Delimiters
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_ARROW,

    // Special
    TOKEN_COMMENT,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

// ============================================================================
// DATA TYPES (ERWEITERT!)
// ============================================================================

typedef enum {
    TYPE_INT,       // 32-bit signed integer
    TYPE_CHAR,      // 8-bit signed character (-128 to 127)
    TYPE_BYTE,      // 8-bit unsigned (0 to 255)
    TYPE_BIT,       // Boolean (0 or 1)
    TYPE_FLOAT,     // 32-bit floating point
    TYPE_DOUBLE,    // 64-bit floating point
    TYPE_STRING,    // String (not yet fully implemented)
    TYPE_VOID,      // No return value
    TYPE_UNKNOWN
} DataType;

// ============================================================================
// STRUCTURES
// ============================================================================

typedef struct {
    TokenType type;
    char value[MAX_TOKEN];
    int line;
    int column;
} Token;

typedef struct {
    Token *tokens;
    int count;
    int current;
    int capacity;
} TokenStream;

typedef struct {
    char name[MAX_TOKEN];
    int offset;           // Stack offset
    int scope;            // Scope level
    DataType type;        // Variable type (ERWEITERT!)
    int size;             // Size in bytes (NEU!)
    int is_array;         // 1 if this is an array, 0 otherwise
    int array_size;       // Number of elements (0 for non-arrays or unknown size)
} Variable;

typedef struct {
    char name[MAX_TOKEN];
    char params[10][MAX_TOKEN];
    DataType param_types[10];  // ERWEITERT!
    int param_is_array[10];    // 1 if parameter is an array
    int param_count;
    DataType return_type;      // ERWEITERT!
    int return_is_array;       // 1 if return type is an array
} Function;

typedef struct {
    int id;
    char text[MAX_LINE];
} StringLiteral;

typedef struct {
    int id;
    char value[MAX_TOKEN];  // "3.14", "2.5e10", etc.
} FloatLiteral;

typedef struct {
    int loop_id;  // The ID of this loop
} LoopContext;

typedef struct {
    TokenStream *tokens;

    Variable vars[MAX_VARS];
    Function functions[MAX_FUNCTIONS];
    StringLiteral string_literals[MAX_STRING_LITERALS];
    FloatLiteral float_literals[MAX_FLOAT_LITERALS];

    char code_buffer[CODE_BUFFER_SIZE];
    char function_code_buffer[CODE_BUFFER_SIZE];

    int var_count;
    int function_count;
    int string_literal_count;
    int float_literal_count;
    int code_pos;
    int function_code_pos;
    int current_scope;
    int scope_depth;
    int has_error;
    int debug_mode;
    int label_counter;
    int loop_counter;
    
    // Loop context stack for break/continue
    LoopContext loop_stack[MAX_LOOP_DEPTH];
    int loop_depth;
} Parser;

#endif