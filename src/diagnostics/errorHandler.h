#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <stdio.h>
#include <stdarg.h>

typedef enum {
    SEVERITY_DEBUG,
    SEVERITY_INFO,
    SEVERITY_WARNING,
    SEVERITY_ERROR,
    SEVERITY_FATAL
} ErrorSeverity;

#define ERR_LEX_UNCLOSED_STRING         100
#define ERR_LEX_UNCLOSED_CHAR           101
#define ERR_LEX_INVALID_ESCAPE          102
#define ERR_LEX_UNKNOWN_CHAR            103
#define ERR_LEX_FILE_NOT_FOUND          104
#define ERR_LEX_FILE_READ_ERROR         105
#define ERR_LEX_INVALID_SYNTAX          106
#define ERR_LEX_INVALID_CHAR_LITERAL    107
#define ERR_LEX_TOKEN_TOO_LONG          108

#define ERR_PARSE_UNEXPECTED_TOKEN      100
#define ERR_PARSE_EXPECTED_TOKEN        101
#define ERR_PARSE_INVALID_SYNTAX        102
#define ERR_PARSE_MISSING_SEMICOLON     103
#define ERR_PARSE_MISSING_BRACE         104
#define ERR_PARSE_MISSING_PAREN         105
#define ERR_PARSE_INVALID_DECLARATION   106
#define ERR_PARSE_DUPLICATE_DEFINITION  107
#define ERR_PARSE_TOO_MANY_ERRORS       108

#define ERR_TYPE_MISMATCH               100
#define ERR_TYPE_UNKNOWN                101
#define ERR_TYPE_INVALID_OPERATION      102
#define ERR_TYPE_INCOMPATIBLE_TYPES     103

#define ERR_SEM_UNDEFINED_VARIABLE      100
#define ERR_SEM_UNDEFINED_FUNCTION      101
#define ERR_SEM_UNDEFINED_STRUCT        102
#define ERR_SEM_WRONG_ARG_COUNT         103
#define ERR_SEM_NOT_ARRAY               104
#define ERR_SEM_NOT_STRUCT              105
#define ERR_SEM_FIELD_NOT_FOUND         106
#define ERR_SEM_METHOD_NOT_FOUND        107
#define ERR_SEM_NOT_STATIC              108
#define ERR_SEM_BREAK_OUTSIDE_LOOP      109
#define ERR_SEM_CONTINUE_OUTSIDE_LOOP   110
#define ERR_SEM_METHOD_REFERENCE       111
#define ERR_SEM_DUPLICATE_DEFINITION    112
#define ERR_SEM_INVALID_DECLARATION     113
#define ERR_SEM_COMPLEXITY_LIMIT        114
#define ERR_SEM_STORAGE_LIMIT           115
#define ERR_SEM_INDEX_OUT_OF_BOUNDS     116
#define ERR_PACKAGE_DECLARATION        120
#define ERR_PACKAGE_NAME               121
#define ERR_MODULE_MANIFEST            122
#define ERR_PACKAGE_NOT_FOUND          123
#define ERR_PACKAGE_CYCLE              124
#define ERR_PACKAGE_PRIVATE            125
#define ERR_PACKAGE_INTERNAL           126
#define ERR_PACKAGE_ALIAS              127
#define ERR_MODULE_VERSION             128
#define ERR_MODULE_DEPENDENCY          129

#define ERR_CODEGEN_TOO_MANY_LITERALS   100
#define ERR_CODEGEN_TOO_MANY_VARIABLES  101
#define ERR_CODEGEN_TOO_MANY_FUNCTIONS  102
#define ERR_CODEGEN_OUTPUT_FAILED       103

#define ERR_COMP_NO_MAIN_FUNCTION       100
#define ERR_COMP_NO_SOURCE_FILE         101
#define ERR_COMP_INVALID_OPTION         102
#define ERR_COMP_INTERNAL_FAILURE       103
#define ERR_COMP_IMPORT_NOT_FOUND       104
#define ERR_COMP_IMPORT_OUTSIDE_ROOT    105
#define ERR_COMP_DUMP_FAILED            106

#define WARN_UNUSED_VARIABLE            100
#define WARN_DEPRECATED                 101

typedef enum {
    ERROR_CATEGORY_LEXER = 'L',
    ERROR_CATEGORY_PARSER = 'P',
    ERROR_CATEGORY_TYPE = 'T',
    ERROR_CATEGORY_SEMANTIC = 'S',
    ERROR_CATEGORY_CODEGEN = 'G',
    ERROR_CATEGORY_COMPILER = 'C',
    ERROR_CATEGORY_WARNING = 'W'
} ErrorCategory;

typedef struct ErrorContext {
    ErrorSeverity severity;
    int line;
    int column;
    int end_line;
    int end_column;
    char error_category;
    int error_code;
    char *filename;
    char *message;
    char *source_line;
    char *token_value;
    char *suggestion;
    char *fix_replacement;
    int fix_line;
    int fix_column;
    int fix_end_line;
    int fix_end_column;
    struct ErrorContext *parent;
    struct ErrorContext **children;
    int child_count;
    int child_capacity;
} ErrorContext;

typedef struct {
    int use_colors;
    int show_source_context;
    int show_suggestions;
    int max_errors;
    int error_count;
    int warning_count;
    int suppressed_error_count;
    const char *source_override_name;
    const char *source_override_path;
    FILE *output_stream;
    int json_output;
    int buffered;
    ErrorContext **buffer;
    int buffer_count;
    int buffer_capacity;
} ErrorHandler;

extern ErrorHandler *global_error_handler;
const char *error_handler_source_path(const char *filename);

ErrorHandler *error_handler_init(void);

void error_handler_free(ErrorHandler *handler);

void error_handler_set_global(ErrorHandler *handler);

void error_handler_set_colors(ErrorHandler *handler, int enabled);

void error_handler_set_max_errors(ErrorHandler *handler, int max);

void error_handler_set_json_output(ErrorHandler *handler, int enabled);

void error_handler_set_buffered(ErrorHandler *handler, int enabled);

ErrorContext *error_context_create(
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *message
);

void error_context_add_child(ErrorContext *parent, ErrorContext *child);

void error_context_set_source_line(ErrorContext *ctx, const char *source_line);

void error_context_set_token(ErrorContext *ctx, const char *token_value);

void error_context_set_suggestion(ErrorContext *ctx, const char *suggestion);
void error_context_set_span(ErrorContext *ctx, int end_line, int end_column);
void error_context_set_fix(ErrorContext *ctx, int line, int column,
                           int end_line, int end_column, const char *replacement);

void error_context_free(ErrorContext *ctx);

void error_report_context(ErrorHandler *handler, ErrorContext *ctx);

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

void error_handler_flush(ErrorHandler *handler);

int error_handler_get_error_count(const ErrorHandler *handler);

int error_handler_get_warning_count(const ErrorHandler *handler);

int error_handler_should_stop(ErrorHandler *handler);

void error_handler_reset(ErrorHandler *handler);

#endif
