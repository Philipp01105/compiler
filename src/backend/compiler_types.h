#ifndef COMPILER_TYPES_H
#define COMPILER_TYPES_H

#include "token.h"

/*
 * Maximum sizes for compiler data structures
 */
#define MAX_LINE 1024               /* Maximum line length */
#define MAX_VARS 400                /* Maximum variables per scope */
#define MAX_FUNCTIONS 200           /* Maximum functions in program */
#define MAX_STRING_LITERALS 5000    /* Maximum string literals */
#define MAX_FLOAT_LITERALS 5000     /* Maximum float literals */
#define CODE_BUFFER_SIZE 524288     /* Assembly code buffer size */
#define MAX_LOOP_DEPTH 100          /* Maximum nested loop depth */
#define MAX_IMPORTS 100             /* Maximum import statements */
#define MAX_PATH 512                /* Maximum file path length */
#define MAX_LOCAL_STORAGE (8 * 1024 * 1024) /* Maximum stack storage per function */
#define MAX_OBJECT_SIZE (8 * 1024 * 1024)   /* Maximum size of one array/struct */
#define MAX_EXPRESSION_TOKENS 512    /* Maximum tokens in one expression */

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
    char (*params)[MAX_TOKEN];
    DataType *param_types;
    int *param_is_array;
    int *param_is_pointer;
    int param_count;
    int param_capacity;
    DataType return_type; /* Return value type */
    int return_is_array; /* Array return flag */
    char struct_name[MAX_TOKEN]; /* Struct for methods */
    int is_static; /* Static method flag */
    int is_defined; /* Function body has been parsed */
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
    DataType expression_type; /* Type of the most recently parsed expression */
    int expression_is_pointer; /* Most recent expression denotes an address value */
    DataType current_return_type; /* Return type of the function being generated */
} Parser;

#endif
