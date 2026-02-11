#ifndef COMPILER_TYPES_H
#define COMPILER_TYPES_H

/*
 * Maximum sizes for compiler data structures
 */
#define MAX_TOKEN 512               /* Maximum token length */
#define MAX_LINE 1024               /* Maximum line length */
#define MAX_VARS 400                /* Maximum variables per scope */
#define MAX_FUNCTIONS 200           /* Maximum functions in program */
#define MAX_STRING_LITERALS 5000    /* Maximum string literals */
#define MAX_FLOAT_LITERALS 5000     /* Maximum float literals */
#define CODE_BUFFER_SIZE 524288     /* Assembly code buffer size */
#define MAX_LOOP_DEPTH 100          /* Maximum nested loop depth */
#define MAX_IMPORTS 100             /* Maximum import statements */
#define MAX_PATH 512                /* Maximum file path length */

/*
 * Token types for lexical analysis
 */
typedef enum {
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_PRINT,
    TOKEN_KEYWORD_PRINTLINE,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_CONTINUE,
    TOKEN_KEYWORD_STRUCT,
    TOKEN_KEYWORD_ENUM,
    TOKEN_KEYWORD_IMPORT,
    TOKEN_KEYWORD_STATIC,
    TOKEN_KEYWORD_RESERVE,
    TOKEN_KEYWORD_FREE,
    TOKEN_KEYWORD_GC,

    TOKEN_TYPE_INT,
    TOKEN_TYPE_CHAR,
    TOKEN_TYPE_BYTE,
    TOKEN_TYPE_BIT,
    TOKEN_TYPE_FLOAT,
    TOKEN_TYPE_DOUBLE,
    TOKEN_TYPE_STRING,
    TOKEN_TYPE_VOID,

    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_FLOAT_LITERAL,
    TOKEN_CHAR_LITERAL,
    TOKEN_STRING_LITERAL,

    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,

    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_BANG_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,

    TOKEN_AMP_AMP,
    TOKEN_PIPE_PIPE,
    TOKEN_BANG,
    TOKEN_AMPERSAND,

    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PLUS_PLUS,
    TOKEN_MINUS_MINUS,

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
    TOKEN_DOT,
    TOKEN_HASH,
    TOKEN_AT,

    TOKEN_COMMENT,
    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

/*
 * Data types supported by the compiler
 */
typedef enum {
    TYPE_INT, /* 32-bit signed integer */
    TYPE_CHAR, /* 8-bit signed character */
    TYPE_BYTE, /* 8-bit unsigned */
    TYPE_BIT, /* Boolean (0 or 1) */
    TYPE_FLOAT, /* 32-bit floating point */
    TYPE_DOUBLE, /* 64-bit floating point */
    TYPE_STRING, /* String type */
    TYPE_VOID, /* No return value */
    TYPE_UNKNOWN
} DataType;

/*
 * Target format for assembly output
 */
typedef enum {
    TARGET_ELF, /* Linux/Unix ELF format (default) */
    TARGET_COFF /* Windows COFF format */
} TargetFormat;

/*
 * Assembly syntax mode
 */
typedef enum {
    SYNTAX_ATT, /* AT&T syntax (legacy) */
    SYNTAX_INTEL /* Intel syntax (default) */
} SyntaxMode;

/*
 * Lexical token with position information
 */
typedef struct {
    TokenType type;
    char value[MAX_TOKEN];
    int line;
    int column;
} Token;

/*
 * Token stream for parser consumption
 */
typedef struct {
    Token *tokens;
    int count; /* Total number of tokens */
    int current; /* Current position in stream */
    int capacity; /* Allocated capacity */
} TokenStream;

/*
 * Variable with type and scope information
 */
typedef struct {
    char name[MAX_TOKEN];
    int offset; /* Stack offset */
    int scope; /* Scope level */
    DataType type; /* Variable type */
    int size; /* Size in bytes */
    int is_array; /* Array flag */
    int array_size; /* Number of elements */
    char struct_type[MAX_TOKEN]; /* Struct type name */
    int is_pointer; /* Pointer flag */
    int is_gc; /* Garbage collected flag */
    int is_heap; /* Heap allocated flag */
} Variable;

/*
 * Function signature and metadata
 */
typedef struct {
    char name[MAX_TOKEN];
    char params[10][MAX_TOKEN];
    DataType param_types[10]; /* Parameter types */
    int param_is_array[10]; /* Array parameter flags */
    int param_is_pointer[10]; /* Pointer parameter flags */
    int param_count;
    DataType return_type; /* Return value type */
    int return_is_array; /* Array return flag */
    char struct_name[MAX_TOKEN]; /* Struct for methods */
    int is_static; /* Static method flag */
} Function;

/*
 * Struct field definition
 */
typedef struct {
    char name[MAX_TOKEN];
    DataType type;
    int offset; /* Offset within struct */
    int size; /* Size in bytes */
    int is_array; /* Array field flag */
    int array_size; /* Number of elements */
    char struct_type[MAX_TOKEN]; /* Nested struct type */
} StructField;

/*
 * Struct definition with fields and methods
 */
typedef struct {
    char name[MAX_TOKEN];
    StructField fields[50];
    int field_count;
    int methods[MAX_FUNCTIONS]; /* Method indices */
    int method_count;
    int total_size; /* Total size in bytes */
} StructDefinition;

/*
 * Enum value instance
 */
typedef struct {
    char name[MAX_TOKEN];
    char values[50][MAX_TOKEN]; /* Field values */
    int field_count;
} EnumValue;

/*
 * Enum definition with values
 */
typedef struct {
    char name[MAX_TOKEN];
    StructField fields[50];
    int field_count;
    EnumValue values[50];
    int value_count;
    int struct_index; /* Generated struct index */
} EnumDefinition;

/*
 * String literal in code section
 */
typedef struct {
    int id;
    char text[MAX_LINE];
} StringLiteral;

/*
 * Float literal in data section
 */
typedef struct {
    int id;
    char value[MAX_TOKEN];
} FloatLiteral;

/*
 * Loop context for break/continue
 */
typedef struct {
    int loop_id;
} LoopContext;

/*
 * Parser state and symbol tables
 */
typedef struct {
    TokenStream *tokens;

    Variable vars[MAX_VARS];
    Function functions[MAX_FUNCTIONS];
    StructDefinition structs[50];
    EnumDefinition enums[50];
    StringLiteral string_literals[MAX_STRING_LITERALS];
    FloatLiteral float_literals[MAX_FLOAT_LITERALS];

    char code_buffer[CODE_BUFFER_SIZE];
    char function_code_buffer[CODE_BUFFER_SIZE];

    int var_count;
    int function_count;
    int struct_count;
    int enum_count;
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

    LoopContext loop_stack[MAX_LOOP_DEPTH];
    int loop_depth;

    char current_struct_context[MAX_TOKEN]; /* Current method context */

    char imported_files[MAX_IMPORTS][MAX_PATH];
    int import_count;

    int next_var_is_gc; /* Next var GC flag */

    char *source_content; /* Full source file */
    char **source_lines; /* Line pointers */
    int source_line_count; /* Number of lines */
    const char *source_filename; /* Source file name */

    TargetFormat target_format; /* ELF or COFF output */
    SyntaxMode syntax_mode; /* AT&T or Intel syntax */
} Parser;

#endif
