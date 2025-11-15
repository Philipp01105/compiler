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
#define MAX_IMPORTS 100
#define MAX_PATH 512

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

typedef enum {
    TYPE_INT,        
    TYPE_CHAR,       
    TYPE_BYTE,       
    TYPE_BIT,        
    TYPE_FLOAT,      
    TYPE_DOUBLE,     
    TYPE_STRING,     
    TYPE_VOID,       
    TYPE_UNKNOWN
} DataType;

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
    int offset;            
    int scope;             
    DataType type;         
    int size;              
    int is_array;          
    int array_size;        
    char struct_type[MAX_TOKEN];   
    int is_pointer;        
    int is_gc;             
    int is_heap;           
} Variable;

typedef struct {
    char name[MAX_TOKEN];
    char params[10][MAX_TOKEN];
    DataType param_types[10];   
    int param_is_array[10];     
    int param_is_pointer[10];   
    int param_count;
    DataType return_type;       
    int return_is_array;        
    char struct_name[MAX_TOKEN];   
    int is_static;              
} Function;

typedef struct {
    char name[MAX_TOKEN];      
    DataType type;             
    int offset;                
    int size;                  
    int is_array;              
    int array_size;            
    char struct_type[MAX_TOKEN];   
} StructField;

typedef struct {
    char name[MAX_TOKEN];      
    StructField fields[50];    
    int field_count;
    int methods[MAX_FUNCTIONS];   
    int method_count;
    int total_size;            
} StructDefinition;

typedef struct {
    char name[MAX_TOKEN];      
    char values[50][MAX_TOKEN];  
    int field_count;           
} EnumValue;

typedef struct {
    char name[MAX_TOKEN];      
    StructField fields[50];    
    int field_count;
    EnumValue values[50];      
    int value_count;
    int struct_index;          
} EnumDefinition;

typedef struct {
    int id;
    char text[MAX_LINE];
} StringLiteral;

typedef struct {
    int id;
    char value[MAX_TOKEN];   
} FloatLiteral;

typedef struct {
    int loop_id;   
} LoopContext;

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
    
    char current_struct_context[MAX_TOKEN];   
    
    char imported_files[MAX_IMPORTS][MAX_PATH];
    int import_count;
    
    int next_var_is_gc;   
    
    char *source_content;   
    char **source_lines;    
    int source_line_count;  
    const char *source_filename;  
} Parser;

#endif