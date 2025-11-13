#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <stdio.h>
#include <stdarg.h>

// ============================================================================
// ERROR SEVERITY LEVELS
// ============================================================================

typedef enum {
    SEVERITY_DEBUG,      // Debug information for development
    SEVERITY_INFO,       // Informational messages
    SEVERITY_WARNING,    // Warnings that don't prevent compilation
    SEVERITY_ERROR,      // Errors that prevent compilation
    SEVERITY_FATAL       // Fatal errors that stop compilation immediately
} ErrorSeverity;

// ============================================================================
// ERROR CODES
// ============================================================================
// Error codes are organized by category with character prefix:
// L100-L199: Lexer errors
// P100-P199: Parser errors  
// T100-T199: Type errors
// S100-S199: Semantic errors
// G100-G199: Code generation errors
// C100-C199: General compiler errors
// W100-W199: Warnings

// Lexer errors (L100-L199)
#define ERR_LEX_UNCLOSED_STRING         100  // L100
#define ERR_LEX_UNCLOSED_CHAR           101  // L101
#define ERR_LEX_INVALID_ESCAPE          102  // L102
#define ERR_LEX_UNKNOWN_CHAR            103  // L103
#define ERR_LEX_FILE_NOT_FOUND          104  // L104
#define ERR_LEX_FILE_READ_ERROR         105  // L105

// Parser errors (P100-P199)
#define ERR_PARSE_UNEXPECTED_TOKEN      100  // P100
#define ERR_PARSE_EXPECTED_TOKEN        101  // P101
#define ERR_PARSE_INVALID_SYNTAX        102  // P102
#define ERR_PARSE_MISSING_SEMICOLON     103  // P103
#define ERR_PARSE_MISSING_BRACE         104  // P104
#define ERR_PARSE_MISSING_PAREN         105  // P105
#define ERR_PARSE_INVALID_DECLARATION   106  // P106
#define ERR_PARSE_DUPLICATE_DEFINITION  107  // P107
#define ERR_PARSE_TOO_MANY_ERRORS       108  // P108

// Type errors (T100-T199)
#define ERR_TYPE_MISMATCH               100  // T100
#define ERR_TYPE_UNKNOWN                101  // T101
#define ERR_TYPE_INVALID_OPERATION      102  // T102
#define ERR_TYPE_INCOMPATIBLE_TYPES     103  // T103

// Semantic errors (S100-S199)
#define ERR_SEM_UNDEFINED_VARIABLE      100  // S100
#define ERR_SEM_UNDEFINED_FUNCTION      101  // S101
#define ERR_SEM_UNDEFINED_STRUCT        102  // S102
#define ERR_SEM_WRONG_ARG_COUNT         103  // S103
#define ERR_SEM_NOT_ARRAY               104  // S104
#define ERR_SEM_NOT_STRUCT              105  // S105
#define ERR_SEM_FIELD_NOT_FOUND         106  // S106
#define ERR_SEM_METHOD_NOT_FOUND        107  // S107
#define ERR_SEM_NOT_STATIC              108  // S108
#define ERR_SEM_BREAK_OUTSIDE_LOOP      109  // S109
#define ERR_SEM_CONTINUE_OUTSIDE_LOOP   110  // S110

// Code generation errors (G100-G199)
#define ERR_CODEGEN_TOO_MANY_LITERALS   100  // G100
#define ERR_CODEGEN_TOO_MANY_VARIABLES  101  // G101
#define ERR_CODEGEN_TOO_MANY_FUNCTIONS  102  // G102
#define ERR_CODEGEN_OUTPUT_FAILED       103  // G103

// General compiler errors (C100-C199)
#define ERR_COMP_NO_MAIN_FUNCTION       100  // C100
#define ERR_COMP_NO_SOURCE_FILE         101  // C101
#define ERR_COMP_INVALID_OPTION         102  // C102

// Warnings (W100-W199)
#define WARN_UNUSED_VARIABLE            100  // W100
#define WARN_DEPRECATED                 101  // W101

// Error code categories
typedef enum {
    ERROR_CATEGORY_LEXER = 'L',
    ERROR_CATEGORY_PARSER = 'P',
    ERROR_CATEGORY_TYPE = 'T',
    ERROR_CATEGORY_SEMANTIC = 'S',
    ERROR_CATEGORY_CODEGEN = 'G',
    ERROR_CATEGORY_COMPILER = 'C',
    ERROR_CATEGORY_WARNING = 'W'
} ErrorCategory;

// ============================================================================
// ERROR CONTEXT
// ============================================================================

typedef struct ErrorContext {
    ErrorSeverity severity;
    int line;
    int column;
    char error_category;    // Error category character (L, P, T, S, G, C, W)
    int error_code;         // Error code number (100-199)
    char *filename;
    char *message;
    char *source_line;      // The actual source code line where error occurred
    char *token_value;      // The token that caused the error (if applicable)
    char *suggestion;       // Helpful suggestion to fix the error
    struct ErrorContext *parent;  // Parent error (for error trees)
    struct ErrorContext **children;  // Child errors (cascading errors)
    int child_count;
    int child_capacity;
} ErrorContext;

// ============================================================================
// ERROR HANDLER CONFIGURATION
// ============================================================================

typedef struct {
    int use_colors;          // Enable/disable color output
    int show_source_context; // Show source code context around errors
    int show_suggestions;    // Show suggestions for fixing errors
    int max_errors;          // Maximum number of errors before stopping (0 = unlimited)
    int error_count;         // Current error count
    int warning_count;       // Current warning count
    FILE *output_stream;     // Where to write error messages (usually stderr)
    int json_output;         // Output errors in JSON format
    int buffered;            // Buffer errors and show at end
    ErrorContext **buffer;   // Buffer for storing errors
    int buffer_count;        // Number of buffered errors
    int buffer_capacity;     // Buffer capacity
} ErrorHandler;

// ============================================================================
// GLOBAL ERROR HANDLER
// ============================================================================

extern ErrorHandler *global_error_handler;

// ============================================================================
// ERROR HANDLER FUNCTIONS
// ============================================================================

// Initialize the error handler
ErrorHandler *error_handler_init(void);

// Free the error handler
void error_handler_free(ErrorHandler *handler);

// Set global error handler
void error_handler_set_global(ErrorHandler *handler);

// Configure error handler
void error_handler_set_colors(ErrorHandler *handler, int enabled);
void error_handler_set_max_errors(ErrorHandler *handler, int max);
void error_handler_set_json_output(ErrorHandler *handler, int enabled);
void error_handler_set_buffered(ErrorHandler *handler, int enabled);

// Create error context
ErrorContext *error_context_create(
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *message
);

// Add child error to create error tree
void error_context_add_child(ErrorContext *parent, ErrorContext *child);

// Set additional context information
void error_context_set_source_line(ErrorContext *ctx, const char *source_line);
void error_context_set_token(ErrorContext *ctx, const char *token_value);
void error_context_set_suggestion(ErrorContext *ctx, const char *suggestion);

// Free error context
void error_context_free(ErrorContext *ctx);

// Report error using context
void error_report_context(ErrorHandler *handler, ErrorContext *ctx);

// Convenience functions for reporting errors
void error_report(
    ErrorHandler *handler,
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *format,
    ...
);

// Formatted error reporting with suggestions
void error_report_with_suggestion(
    ErrorHandler *handler,
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *suggestion,
    const char *format,
    ...
);

// Flush buffered errors (display all buffered errors)
void error_handler_flush(ErrorHandler *handler);

// Get error/warning counts
int error_handler_get_error_count(ErrorHandler *handler);
int error_handler_get_warning_count(ErrorHandler *handler);

// Check if compilation should stop due to errors
int error_handler_should_stop(ErrorHandler *handler);

// Reset error counts
void error_handler_reset(ErrorHandler *handler);

#endif // ERROR_HANDLER_H
