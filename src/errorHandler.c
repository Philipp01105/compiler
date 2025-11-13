#include "errorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// ANSI COLOR CODES
// ============================================================================

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

// ============================================================================
// GLOBAL ERROR HANDLER
// ============================================================================

ErrorHandler *global_error_handler = NULL;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static int is_terminal(FILE *stream) {
    int fd = fileno(stream);
    return isatty(fd);
}

static const char *get_severity_label(ErrorSeverity severity) {
    switch (severity) {
        case SEVERITY_DEBUG:   return "[DEBUG]";
        case SEVERITY_INFO:    return "[INFO]";
        case SEVERITY_WARNING: return "[WARNING]";
        case SEVERITY_ERROR:   return "[ERROR]";
        case SEVERITY_FATAL:   return "[FATAL]";
        default:               return "[UNKNOWN]";
    }
}

static const char *get_severity_color(ErrorSeverity severity) {
    switch (severity) {
        case SEVERITY_DEBUG:   return COLOR_CYAN;
        case SEVERITY_INFO:    return COLOR_GREEN;
        case SEVERITY_WARNING: return COLOR_YELLOW;
        case SEVERITY_ERROR:   return COLOR_RED;
        case SEVERITY_FATAL:   return COLOR_MAGENTA;
        default:               return COLOR_RESET;
    }
}

// ============================================================================
// ERROR HANDLER INITIALIZATION
// ============================================================================

ErrorHandler *error_handler_init(void) {
    ErrorHandler *handler = (ErrorHandler *)malloc(sizeof(ErrorHandler));
    if (!handler) {
        return NULL;
    }
    
    handler->output_stream = stderr;
    handler->use_colors = is_terminal(handler->output_stream);
    handler->show_source_context = 1;
    handler->show_suggestions = 1;
    handler->max_errors = 10;  // Stop after 10 errors by default
    handler->error_count = 0;
    handler->warning_count = 0;
    handler->json_output = 0;
    handler->buffered = 1;  // Buffer errors by default
    handler->buffer = NULL;
    handler->buffer_count = 0;
    handler->buffer_capacity = 0;
    
    return handler;
}

void error_handler_free(ErrorHandler *handler) {
    if (handler) {
        // Free buffered errors
        for (int i = 0; i < handler->buffer_count; i++) {
            error_context_free(handler->buffer[i]);
        }
        free(handler->buffer);
        free(handler);
    }
}

void error_handler_set_global(ErrorHandler *handler) {
    global_error_handler = handler;
}

void error_handler_set_colors(ErrorHandler *handler, int enabled) {
    if (handler) {
        handler->use_colors = enabled && is_terminal(handler->output_stream);
    }
}

void error_handler_set_max_errors(ErrorHandler *handler, int max) {
    if (handler) {
        handler->max_errors = max;
    }
}

void error_handler_set_json_output(ErrorHandler *handler, int enabled) {
    if (handler) {
        handler->json_output = enabled;
        if (enabled) {
            handler->use_colors = 0;  // Disable colors for JSON output
        }
    }
}

void error_handler_set_buffered(ErrorHandler *handler, int enabled) {
    if (handler) {
        handler->buffered = enabled;
    }
}

// ============================================================================
// ERROR CONTEXT MANAGEMENT
// ============================================================================

ErrorContext *error_context_create(
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *message
) {
    ErrorContext *ctx = (ErrorContext *)malloc(sizeof(ErrorContext));
    if (!ctx) {
        return NULL;
    }
    
    ctx->severity = severity;
    ctx->line = line;
    ctx->column = column;
    ctx->error_category = error_category;
    ctx->error_code = error_code;
    ctx->filename = filename ? strdup(filename) : NULL;
    ctx->message = message ? strdup(message) : NULL;
    ctx->source_line = NULL;
    ctx->token_value = NULL;
    ctx->suggestion = NULL;
    ctx->parent = NULL;
    ctx->children = NULL;
    ctx->child_count = 0;
    ctx->child_capacity = 0;
    
    return ctx;
}

void error_context_add_child(ErrorContext *parent, ErrorContext *child) {
    if (!parent || !child) {
        return;
    }
    
    // Resize children array if needed
    if (parent->child_count >= parent->child_capacity) {
        int new_capacity = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        ErrorContext **new_children = (ErrorContext **)realloc(
            parent->children,
            sizeof(ErrorContext *) * new_capacity
        );
        if (!new_children) {
            return;  // Failed to allocate
        }
        parent->children = new_children;
        parent->child_capacity = new_capacity;
    }
    
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void error_context_set_source_line(ErrorContext *ctx, const char *source_line) {
    if (ctx && source_line) {
        if (ctx->source_line) {
            free(ctx->source_line);
        }
        ctx->source_line = strdup(source_line);
    }
}

void error_context_set_token(ErrorContext *ctx, const char *token_value) {
    if (ctx && token_value) {
        if (ctx->token_value) {
            free(ctx->token_value);
        }
        ctx->token_value = strdup(token_value);
    }
}

void error_context_set_suggestion(ErrorContext *ctx, const char *suggestion) {
    if (ctx && suggestion) {
        if (ctx->suggestion) {
            free(ctx->suggestion);
        }
        ctx->suggestion = strdup(suggestion);
    }
}

void error_context_free(ErrorContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Free child contexts
    for (int i = 0; i < ctx->child_count; i++) {
        error_context_free(ctx->children[i]);
    }
    
    free(ctx->filename);
    free(ctx->message);
    free(ctx->source_line);
    free(ctx->token_value);
    free(ctx->suggestion);
    free(ctx->children);
    free(ctx);
}

// ============================================================================
// ERROR REPORTING
// ============================================================================

// Helper function to escape strings for JSON
static void json_escape_string(FILE *out, const char *str) {
    if (!str) {
        fprintf(out, "null");
        return;
    }
    
    fprintf(out, "\"");
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  fprintf(out, "\\\""); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '\b': fprintf(out, "\\b"); break;
            case '\f': fprintf(out, "\\f"); break;
            case '\n': fprintf(out, "\\n"); break;
            case '\r': fprintf(out, "\\r"); break;
            case '\t': fprintf(out, "\\t"); break;
            default:
                if (*p < 32) {
                    fprintf(out, "\\u%04x", (unsigned char)*p);
                } else {
                    fputc(*p, out);
                }
        }
    }
    fprintf(out, "\"");
}

// Helper function to format error code
static void format_error_code(char *buffer, size_t size, char category, int code) {
    snprintf(buffer, size, "%c%d", category, code);
}

static void print_source_context(
    ErrorHandler *handler,
    ErrorContext *ctx,
    int indent_level
) {
    if (!handler->show_source_context || !ctx->source_line) {
        return;
    }
    
    FILE *out = handler->output_stream;
    const char *color_blue = handler->use_colors ? COLOR_BLUE : "";
    const char *reset = handler->use_colors ? COLOR_RESET : "";
    const char *error_color = handler->use_colors ? get_severity_color(ctx->severity) : "";
    
    // Print line number with --> pointer (Rust style)
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, " %s-->%s ", color_blue, reset);
    if (ctx->filename) {
        fprintf(out, "%s:%d:%d\n", ctx->filename, ctx->line, ctx->column);
    } else {
        fprintf(out, "line %d:%d\n", ctx->line, ctx->column);
    }
    
    // Print empty line with vertical bar
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, "  %s|%s\n", color_blue, reset);
    
    // Print line number and source line
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, "%s%d |%s %s\n", color_blue, ctx->line, reset, ctx->source_line);
    
    // Print squiggly underline (Rust style)
    if (ctx->column > 0) {
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        fprintf(out, "  %s|%s ", color_blue, reset);
        
        // Spaces before the underline
        for (int i = 1; i < ctx->column; i++) {
            fprintf(out, " ");
        }
        
        // Squiggly underline
        fprintf(out, "%s", error_color);
        int underline_len = 1;
        if (ctx->token_value) {
            underline_len = strlen(ctx->token_value);
            if (underline_len > 20) underline_len = 20;
            if (underline_len < 1) underline_len = 1;
        }
        for (int i = 0; i < underline_len; i++) {
            fprintf(out, "^");
        }
        fprintf(out, "%s\n", reset);
    }
}

static void print_error_context_recursive(
    ErrorHandler *handler,
    ErrorContext *ctx,
    int indent_level
) {
    if (!ctx) {
        return;
    }
    
    FILE *out = handler->output_stream;
    const char *severity_color = handler->use_colors ? get_severity_color(ctx->severity) : "";
    const char *reset = handler->use_colors ? COLOR_RESET : "";
    const char *bold = handler->use_colors ? COLOR_BOLD : "";
    
    // Format error code
    char error_code_str[16];
    format_error_code(error_code_str, sizeof(error_code_str), ctx->error_category, ctx->error_code);
    
    // Print severity label with error code (Rust style: "error[E0123]: message")
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    
    // Convert severity to lowercase for Rust style
    const char *severity_label = get_severity_label(ctx->severity);
    char lowercase_severity[32] = {0};
    for (int i = 0; severity_label[i] && i < 31; i++) {
        if (severity_label[i] == '[' || severity_label[i] == ']') continue;
        lowercase_severity[strlen(lowercase_severity)] = 
            (severity_label[i] >= 'A' && severity_label[i] <= 'Z') ? 
            severity_label[i] + 32 : severity_label[i];
    }
    
    fprintf(out, "%s%s%s", severity_color, bold, lowercase_severity);
    fprintf(out, "[%s]%s: %s\n", error_code_str, reset, ctx->message);
    
    // Print source context
    if (handler->show_source_context && ctx->source_line) {
        print_source_context(handler, ctx, indent_level);
    }
    
    // Print suggestion if available (Rust style with "help:" prefix)
    if (handler->show_suggestions && ctx->suggestion) {
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        const char *color_cyan = handler->use_colors ? COLOR_CYAN : "";
        const char *color_blue = handler->use_colors ? COLOR_BLUE : "";
        fprintf(out, "  %s|%s\n", color_blue, reset);
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        fprintf(out, "  %s= %shelp:%s %s\n", 
                color_blue, color_cyan, reset, ctx->suggestion);
    }
    
    // Print child errors (cascading errors)
    if (ctx->child_count > 0) {
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        const char *color_cyan = handler->use_colors ? COLOR_CYAN : "";
        const char *color_blue = handler->use_colors ? COLOR_BLUE : "";
        fprintf(out, "  %s= %snote:%s caused by:\n", 
                color_blue, color_cyan, reset);
        
        for (int i = 0; i < ctx->child_count; i++) {
            print_error_context_recursive(handler, ctx->children[i], indent_level + 1);
        }
    }
    
    fprintf(out, "\n");
}

// Print error context in JSON format
static void print_error_context_json(
    ErrorHandler *handler,
    ErrorContext *ctx,
    int indent_level,
    int is_last
) {
    if (!ctx) {
        return;
    }
    
    FILE *out = handler->output_stream;
    char error_code_str[16];
    format_error_code(error_code_str, sizeof(error_code_str), ctx->error_category, ctx->error_code);
    
    // Print indent
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, "  ");
    }
    
    fprintf(out, "{\n");
    
    // Severity
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"severity\": \"%s\",\n", get_severity_label(ctx->severity));
    
    // Error code
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"errorCode\": \"%s\",\n", error_code_str);
    
    // Location
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"line\": %d,\n", ctx->line);
    
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"column\": %d,\n", ctx->column);
    
    // Filename
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"filename\": ");
    json_escape_string(out, ctx->filename);
    fprintf(out, ",\n");
    
    // Message
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"message\": ");
    json_escape_string(out, ctx->message);
    fprintf(out, ",\n");
    
    // Source line
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"sourceLine\": ");
    json_escape_string(out, ctx->source_line);
    fprintf(out, ",\n");
    
    // Token
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"token\": ");
    json_escape_string(out, ctx->token_value);
    fprintf(out, ",\n");
    
    // Suggestion
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"suggestion\": ");
    json_escape_string(out, ctx->suggestion);
    
    // Children
    if (ctx->child_count > 0) {
        fprintf(out, ",\n");
        for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
        fprintf(out, "\"children\": [\n");
        for (int i = 0; i < ctx->child_count; i++) {
            print_error_context_json(handler, ctx->children[i], indent_level + 2, i == ctx->child_count - 1);
        }
        for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
        fprintf(out, "]\n");
    } else {
        fprintf(out, "\n");
    }
    
    for (int i = 0; i < indent_level; i++) {
        fprintf(out, "  ");
    }
    fprintf(out, "}%s\n", is_last ? "" : ",");
}

void error_report_context(ErrorHandler *handler, ErrorContext *ctx) {
    if (!handler || !ctx) {
        return;
    }
    
    // Update counters
    if (ctx->severity == SEVERITY_ERROR || ctx->severity == SEVERITY_FATAL) {
        handler->error_count++;
    } else if (ctx->severity == SEVERITY_WARNING) {
        handler->warning_count++;
    }
    
    // If buffering is enabled, add to buffer instead of printing
    if (handler->buffered) {
        // Resize buffer if needed
        if (handler->buffer_count >= handler->buffer_capacity) {
            int new_capacity = handler->buffer_capacity == 0 ? 16 : handler->buffer_capacity * 2;
            ErrorContext **new_buffer = (ErrorContext **)realloc(
                handler->buffer,
                sizeof(ErrorContext *) * new_capacity
            );
            if (!new_buffer) {
                return;  // Failed to allocate
            }
            handler->buffer = new_buffer;
            handler->buffer_capacity = new_capacity;
        }
        
        handler->buffer[handler->buffer_count++] = ctx;
        
        // Check if we should stop
        if (ctx->severity == SEVERITY_FATAL || error_handler_should_stop(handler)) {
            error_handler_flush(handler);
        }
        return;
    }
    
    // Print immediately if not buffering
    if (handler->json_output) {
        print_error_context_json(handler, ctx, 0, 1);
    } else {
        print_error_context_recursive(handler, ctx, 0);
    }
    
    // Check if we should stop
    if (ctx->severity == SEVERITY_FATAL || error_handler_should_stop(handler)) {
        fprintf(handler->output_stream, "\n%s[FATAL]%s Too many errors, stopping compilation.\n\n",
                handler->use_colors ? COLOR_MAGENTA : "",
                handler->use_colors ? COLOR_RESET : "");
    }
}

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
) {
    if (!handler) {
        handler = global_error_handler;
    }
    if (!handler) {
        return;
    }
    
    // Format the message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Create error context
    ErrorContext *ctx = error_context_create(severity, line, column, error_category, error_code, filename, message);
    if (!ctx) {
        return;
    }
    
    // Report the error
    error_report_context(handler, ctx);
    
    // Free the context if not buffered
    if (!handler->buffered) {
        error_context_free(ctx);
    }
}

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
) {
    if (!handler) {
        handler = global_error_handler;
    }
    if (!handler) {
        return;
    }
    
    // Format the message
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Create error context
    ErrorContext *ctx = error_context_create(severity, line, column, error_category, error_code, filename, message);
    if (!ctx) {
        return;
    }
    
    // Set suggestion
    error_context_set_suggestion(ctx, suggestion);
    
    // Report the error
    error_report_context(handler, ctx);
    
    // Free the context if not buffered
    if (!handler->buffered) {
        error_context_free(ctx);
    }
}

void error_handler_flush(ErrorHandler *handler) {
    if (!handler || handler->buffer_count == 0) {
        return;
    }
    
    FILE *out = handler->output_stream;
    
    if (handler->json_output) {
        // Print all errors as JSON array
        fprintf(out, "{\n");
        fprintf(out, "  \"errors\": [\n");
        for (int i = 0; i < handler->buffer_count; i++) {
            print_error_context_json(handler, handler->buffer[i], 2, i == handler->buffer_count - 1);
        }
        fprintf(out, "  ],\n");
        fprintf(out, "  \"summary\": {\n");
        fprintf(out, "    \"errorCount\": %d,\n", handler->error_count);
        fprintf(out, "    \"warningCount\": %d\n", handler->warning_count);
        fprintf(out, "  }\n");
        fprintf(out, "}\n");
    } else {
        // Print all buffered errors (no header, Rust style)
        for (int i = 0; i < handler->buffer_count; i++) {
            print_error_context_recursive(handler, handler->buffer[i], 0);
        }
        
        // Print summary (Rust style - compact)
        const char *color_red = handler->use_colors ? COLOR_RED : "";
        const char *color_yellow = handler->use_colors ? COLOR_YELLOW : "";
        const char *color_reset = handler->use_colors ? COLOR_RESET : "";
        const char *color_bold = handler->use_colors ? COLOR_BOLD : "";
        
        if (handler->error_count > 0) {
            fprintf(out, "%serror%s: could not compile due to ", color_red, color_reset);
            fprintf(out, "%s%d error%s%s", color_bold, handler->error_count, 
                    handler->error_count != 1 ? "s" : "", color_reset);
            if (handler->warning_count > 0) {
                fprintf(out, "; %s%d warning%s emitted%s", 
                        color_yellow, handler->warning_count,
                        handler->warning_count != 1 ? "s" : "", color_reset);
            }
            fprintf(out, "\n\n");
        } else if (handler->warning_count > 0) {
            fprintf(out, "%swarning%s: %s%d warning%s emitted%s\n\n",
                    color_yellow, color_reset, color_bold,
                    handler->warning_count,
                    handler->warning_count != 1 ? "s" : "", color_reset);
        }
    }
}

int error_handler_get_error_count(ErrorHandler *handler) {
    return handler ? handler->error_count : 0;
}

int error_handler_get_warning_count(ErrorHandler *handler) {
    return handler ? handler->warning_count : 0;
}

int error_handler_should_stop(ErrorHandler *handler) {
    if (!handler || handler->max_errors == 0) {
        return 0;
    }
    return handler->error_count >= handler->max_errors;
}

void error_handler_reset(ErrorHandler *handler) {
    if (handler) {
        handler->error_count = 0;
        handler->warning_count = 0;
    }
}
