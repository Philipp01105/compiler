#define _POSIX_C_SOURCE 200809L

#include "errorHandler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#define strdup _strdup
#else
#include <unistd.h>
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

ErrorHandler *global_error_handler = NULL;

static int is_terminal(FILE *stream) {
#ifdef _WIN32
    return _isatty(_fileno(stream));
#else
    return isatty(fileno(stream));
#endif
}

static const char *get_severity_label(ErrorSeverity severity) {
    switch (severity) {
        case SEVERITY_DEBUG: return "[DEBUG]";
        case SEVERITY_INFO: return "[INFO]";
        case SEVERITY_WARNING: return "[WARNING]";
        case SEVERITY_ERROR: return "[ERROR]";
        case SEVERITY_FATAL: return "[FATAL]";
        default: return "[UNKNOWN]";
    }
}

static const char *get_severity_color(ErrorSeverity severity) {
    switch (severity) {
        case SEVERITY_DEBUG: return COLOR_CYAN;
        case SEVERITY_INFO: return COLOR_GREEN;
        case SEVERITY_WARNING: return COLOR_YELLOW;
        case SEVERITY_ERROR: return COLOR_RED;
        case SEVERITY_FATAL: return COLOR_MAGENTA;
        default: return COLOR_RESET;
    }
}

ErrorHandler *error_handler_init(void) {
    ErrorHandler *handler = (ErrorHandler *) malloc(sizeof(ErrorHandler));
    if (!handler) {
        return NULL;
    }

    handler->output_stream = stderr;
    handler->use_colors = is_terminal(handler->output_stream);
    handler->show_source_context = 1;
    handler->show_suggestions = 1;
    handler->max_errors = 10;
    handler->error_count = 0;
    handler->warning_count = 0;
    handler->suppressed_error_count = 0;
    handler->source_override_name = NULL;
    handler->source_override_path = NULL;
    handler->json_output = 0;
    handler->buffered = 1;
    handler->buffer = NULL;
    handler->buffer_count = 0;
    handler->buffer_capacity = 0;

    return handler;
}

void error_handler_free(ErrorHandler *handler) {
    if (handler) {
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
            handler->use_colors = 0;
        }
    }
}

void error_handler_set_buffered(ErrorHandler *handler, int enabled) {
    if (handler) {
        handler->buffered = enabled;
    }
}

ErrorContext *error_context_create(
    ErrorSeverity severity,
    int line,
    int column,
    char error_category,
    int error_code,
    const char *filename,
    const char *message
) {
    ErrorContext *ctx = (ErrorContext *) calloc(1, sizeof(ErrorContext));
    if (!ctx) {
        return NULL;
    }

    ctx->severity = severity;
    ctx->line = line;
    ctx->column = column;
    ctx->end_line = line;
    ctx->end_column = column > 0 ? column + 1 : 0;
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

    if (parent->child_count >= parent->child_capacity) {
        int new_capacity = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        ErrorContext **new_children = (ErrorContext **) realloc(
            parent->children,
            sizeof(ErrorContext *) * (size_t) new_capacity
        );
        if (!new_children) {
            return;
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

    for (int i = 0; i < ctx->child_count; i++) {
        error_context_free(ctx->children[i]);
    }

    free(ctx->filename);
    free(ctx->message);
    free(ctx->source_line);
    free(ctx->token_value);
    free(ctx->suggestion);
    free(ctx->fix_replacement);
    free(ctx->children);
    free(ctx);
}

void error_context_set_span(ErrorContext *ctx, int end_line, int end_column) {
    if (ctx == NULL) return;
    ctx->end_line = end_line;
    ctx->end_column = end_column;
}

void error_context_set_fix(ErrorContext *ctx, int line, int column,
                           int end_line, int end_column, const char *replacement) {
    if (ctx == NULL || replacement == NULL || line < 1 || column < 1 ||
        end_line < line || (end_line == line && end_column < column))
        return;
    char *copy = strdup(replacement);
    if (copy == NULL) return;
    free(ctx->fix_replacement);
    ctx->fix_replacement = copy;
    ctx->fix_line = line;
    ctx->fix_column = column;
    ctx->fix_end_line = end_line;
    ctx->fix_end_column = end_column;
}

/* Capture source context when the diagnostic is reported, including imported units.
 * Failure to read context must never hide the original diagnostic. */
const char *error_handler_source_path(const char *filename) {
    if (filename != NULL && global_error_handler != NULL && global_error_handler->source_override_name != NULL &&
        global_error_handler->source_override_path != NULL &&
#ifdef _WIN32
        _stricmp(filename, global_error_handler->source_override_name) == 0)
#else
        strcmp(filename, global_error_handler->source_override_name) == 0)
#endif
        return global_error_handler->source_override_path;
    return filename;
}

static void capture_source_line(ErrorContext *ctx) {
    for (int i = 0; i < ctx->child_count; i++) capture_source_line(ctx->children[i]);
    if (ctx->source_line != NULL || ctx->filename == NULL || ctx->line < 1) return;
    FILE *source = fopen(error_handler_source_path(ctx->filename), "rb");
    if (source == NULL) return;
    int line = 1;
    int ch;
    while (line < ctx->line && (ch = fgetc(source)) != EOF) {
        if (ch == '\n') line++;
    }
    if (line == ctx->line) {
        size_t size = 0, capacity = 128;
        char *text = malloc(capacity);
        if (text != NULL) {
            while ((ch = fgetc(source)) != EOF && ch != '\n') {
                if (size + 1 >= capacity) {
                    if (capacity > (size_t) -1 / 2) {
                        free(text);
                        text = NULL;
                        break;
                    }
                    capacity *= 2;
                    char *grown = realloc(text, capacity);
                    if (grown == NULL) {
                        free(text);
                        text = NULL;
                        break;
                    }
                    text = grown;
                }
                text[size++] = (char) ch;
            }
            if (text != NULL) {
                while (size > 0 && text[size - 1] == '\r') size--;
                text[size] = '\0';
                ctx->source_line = text;
            }
        }
    }
    fclose(source);
}

static void json_escape_string(FILE *out, const char *str) {
    if (!str) {
        fprintf(out, "null");
        return;
    }

    fprintf(out, "\"");
    for (const char *p = str; *p; p++) {
        unsigned char byte = (unsigned char) *p;
        if (byte >= 0x80) {
            int width = byte >= 0xC2 && byte <= 0xDF
                            ? 2
                            : byte >= 0xE0 && byte <= 0xEF
                                  ? 3
                                  : byte >= 0xF0 && byte <= 0xF4
                                        ? 4
                                        : 0;
            unsigned value = byte & (width == 2 ? 0x1F : width == 3 ? 0x0F : 0x07);
            int valid = width != 0;
            for (int k = 1; valid && k < width; k++) {
                unsigned char part = (unsigned char) p[k];
                if ((part & 0xC0) != 0x80) valid = 0;
                else value = (value << 6) | (part & 0x3F);
            }
            if ((width == 3 && value < 0x800) || (width == 4 && value < 0x10000) ||
                value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
                valid = 0;
            if (valid) {
                fwrite(p, 1, (size_t) width, out);
                p += width - 1;
            } else fputs("\\uFFFD", out);
            continue;
        }
        switch (*p) {
            case '"': fprintf(out, "\\\"");
                break;
            case '\\': fprintf(out, "\\\\");
                break;
            case '\b': fprintf(out, "\\b");
                break;
            case '\f': fprintf(out, "\\f");
                break;
            case '\n': fprintf(out, "\\n");
                break;
            case '\r': fprintf(out, "\\r");
                break;
            case '\t': fprintf(out, "\\t");
                break;
            default:
                if ((unsigned char) *p < 32) {
                    fprintf(out, "\\u%04x", (unsigned char) *p);
                } else {
                    fputc(*p, out);
                }
        }
    }
    fprintf(out, "\"");
}

static void format_error_code(char *buffer, size_t size, char category, int code) {
    snprintf(buffer, size, "%c%d", category, code);
}

static void print_source_context(
    const ErrorHandler *handler,
    const ErrorContext *ctx,
    int indent_level
) {
    if (!handler->show_source_context || !ctx->source_line) {
        return;
    }

    FILE *out = handler->output_stream;
    const char *color_blue = handler->use_colors ? COLOR_BLUE : "";
    const char *reset = handler->use_colors ? COLOR_RESET : "";
    const char *error_color = handler->use_colors ? get_severity_color(ctx->severity) : "";

    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, " %s-->%s ", color_blue, reset);
    if (ctx->filename) {
        fprintf(out, "%s:%d:%d\n", ctx->filename, ctx->line, ctx->column);
    } else {
        fprintf(out, "line %d:%d\n", ctx->line, ctx->column);
    }

    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, "  %s|%s\n", color_blue, reset);

    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }
    fprintf(out, "%s%4d |%s %s\n", color_blue, ctx->line, reset, ctx->source_line);

    if (ctx->column > 0) {
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        fprintf(out, "     %s|%s ", color_blue, reset);

        const unsigned char *cursor = (const unsigned char *) ctx->source_line;
        for (int i = 1; i < ctx->column && *cursor != 0; i++) {
            fputc(*cursor == '\t' ? '\t' : ' ', out);
            cursor++;
            while ((*cursor & 0xC0) == 0x80) cursor++;
        }

        fprintf(out, "%s", error_color);
        int underline_len = 1;
        if (ctx->end_line == ctx->line && ctx->end_column > ctx->column) {
            underline_len = ctx->end_column - ctx->column;
        } else if (ctx->token_value) {
            underline_len = (int) strlen(ctx->token_value);
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

    char error_code_str[16];
    format_error_code(error_code_str, sizeof(error_code_str), ctx->error_category, ctx->error_code);

    for (int i = 0; i < indent_level; i++) {
        fprintf(out, " ");
    }

    const char *severity_label = get_severity_label(ctx->severity);
    char lowercase_severity[32] = {0};
    for (int i = 0; severity_label[i] && i < 31; i++) {
        if (severity_label[i] == '[' || severity_label[i] == ']') continue;
        lowercase_severity[strlen(lowercase_severity)] =
                (severity_label[i] >= 'A' && severity_label[i] <= 'Z') ? severity_label[i] + 32 : severity_label[i];
    }

    fprintf(out, "%s%s%s", severity_color, bold, lowercase_severity);
    fprintf(out, "[%s]%s: %s\n", error_code_str, reset, ctx->message);

    if (handler->show_source_context && ctx->source_line) {
        print_source_context(handler, ctx, indent_level);
    } else if (ctx->filename != NULL) {
        if (ctx->line > 0) fprintf(out, " --> %s:%d:%d\n", ctx->filename, ctx->line, ctx->column);
        else fprintf(out, " --> %s\n", ctx->filename);
    } else if (ctx->line > 0) {
        fprintf(out, " --> line %d:%d\n", ctx->line, ctx->column);
    }

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

    if (ctx->child_count > 0) {
        for (int i = 0; i < indent_level; i++) {
            fprintf(out, " ");
        }
        const char *color_cyan = handler->use_colors ? COLOR_CYAN : "";
        const char *color_blue = handler->use_colors ? COLOR_BLUE : "";
        fprintf(out, "  %s= %snote:%s related locations:\n",
                color_blue, color_cyan, reset);

        for (int i = 0; i < ctx->child_count; i++) {
            print_error_context_recursive(handler, ctx->children[i], indent_level + 1);
        }
    }

    fprintf(out, "\n");
}

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

    for (int i = 0; i < indent_level; i++) {
        fprintf(out, "  ");
    }

    fprintf(out, "{\n");

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"severity\": \"%s\",\n", get_severity_label(ctx->severity));

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"errorCode\": \"%s\",\n", error_code_str);

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"category\": \"%c\",\n", ctx->error_category);

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"line\": %d,\n", ctx->line);

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"column\": %d,\n", ctx->column);

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"endLine\": %d,\n", ctx->end_line);
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"endColumn\": %d,\n", ctx->end_column);

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"filename\": ");
    json_escape_string(out, ctx->filename);
    fprintf(out, ",\n");

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"message\": ");
    json_escape_string(out, ctx->message);
    fprintf(out, ",\n");

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"sourceLine\": ");
    json_escape_string(out, ctx->source_line);
    fprintf(out, ",\n");

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"token\": ");
    json_escape_string(out, ctx->token_value);
    fprintf(out, ",\n");

    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"suggestion\": ");
    json_escape_string(out, ctx->suggestion);
    fprintf(out, ",\n");
    for (int i = 0; i < indent_level + 1; i++) fprintf(out, "  ");
    fprintf(out, "\"fix\": ");
    if (ctx->fix_replacement == NULL || ctx->filename == NULL) fprintf(out, "null");
    else {
        fprintf(out, "{\"filename\": ");
        json_escape_string(out, ctx->filename);
        fprintf(out, ", \"line\": %d, \"column\": %d, \"endLine\": %d, \"endColumn\": %d, \"replacement\": ",
                ctx->fix_line, ctx->fix_column, ctx->fix_end_line, ctx->fix_end_column);
        json_escape_string(out, ctx->fix_replacement);
        fprintf(out, "}");
    }

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
    if (ctx->severity == SEVERITY_ERROR || ctx->severity == SEVERITY_FATAL) {
        handler->error_count++;
        if (!handler->json_output && handler->max_errors > 0 && handler->error_count > handler->max_errors) {
            handler->suppressed_error_count++;
            if (handler->buffered) error_context_free(ctx);
            return;
        }
    } else if (ctx->severity == SEVERITY_WARNING) {
        handler->warning_count++;
    }
    capture_source_line(ctx);

    if (handler->buffered) {
        if (handler->buffer_count >= handler->buffer_capacity) {
            int new_capacity = handler->buffer_capacity == 0 ? 16 : handler->buffer_capacity * 2;
            ErrorContext **new_buffer = (ErrorContext **) realloc(
                handler->buffer,
                sizeof(ErrorContext *) * (size_t) new_capacity
            );
            if (!new_buffer) {
                return;
            }
            handler->buffer = new_buffer;
            handler->buffer_capacity = new_capacity;
        }

        handler->buffer[handler->buffer_count++] = ctx;

        return;
    }

    if (handler->json_output) {
        print_error_context_json(handler, ctx, 0, 1);
    } else {
        print_error_context_recursive(handler, ctx, 0);
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

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    ErrorContext *ctx = error_context_create(severity, line, column, error_category, error_code, filename, message);
    if (!ctx) {
        return;
    }

    error_report_context(handler, ctx);

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

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    ErrorContext *ctx = error_context_create(severity, line, column, error_category, error_code, filename, message);
    if (!ctx) {
        return;
    }

    error_context_set_suggestion(ctx, suggestion);

    error_report_context(handler, ctx);

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
        fprintf(out, "{\n");
        fprintf(out, "  \"errors\": [\n");
        for (int i = 0; i < handler->buffer_count; i++) {
            print_error_context_json(handler, handler->buffer[i], 2, i == handler->buffer_count - 1);
        }
        fprintf(out, "  ],\n");
        fprintf(out, "  \"summary\": {\n");
        fprintf(out, "    \"errorCount\": %d,\n", handler->error_count);
        fprintf(out, "    \"warningCount\": %d,\n", handler->warning_count);
        fprintf(out, "    \"suppressedErrorCount\": %d\n", handler->suppressed_error_count);
        fprintf(out, "  }\n");
        fprintf(out, "}\n");
    } else {
        for (int i = 0; i < handler->buffer_count; i++) {
            print_error_context_recursive(handler, handler->buffer[i], 0);
        }
        if (handler->suppressed_error_count > 0)
            fprintf(out, "%d additional errors omitted (display limit: %d).\n\n",
                    handler->suppressed_error_count, handler->max_errors);

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

    for (int i = 0; i < handler->buffer_count; i++) {
        error_context_free(handler->buffer[i]);
        handler->buffer[i] = NULL;
    }
    handler->buffer_count = 0;
}

int error_handler_get_error_count(const ErrorHandler *handler) {
    return handler ? handler->error_count : 0;
}

int error_handler_get_warning_count(const ErrorHandler *handler) {
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
        for (int i = 0; i < handler->buffer_count; i++) error_context_free(handler->buffer[i]);
        handler->buffer_count = 0;
        handler->error_count = 0;
        handler->warning_count = 0;
        handler->suppressed_error_count = 0;
    }
}
