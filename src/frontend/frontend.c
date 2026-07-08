#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif

#include "frontend.h"

#include "errorHandler.h"
#include "lexer.h"
#include "syntax_parser.h"

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#ifdef _WIN32
#include <direct.h>
#define TokenType WindowsTokenType
#include <windows.h>
#undef TokenType
#undef SEVERITY_ERROR
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef _MSC_VER
struct dirent { char d_name[MAX_PATH]; };
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAA data;
    struct dirent entry;
    int first;
} DIR;

static DIR *opendir(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    size_t length = strlen(path);
    if (length > SIZE_MAX - 3) return NULL;
    char *pattern = malloc(length + 3);
    DIR *directory = calloc(1, sizeof(*directory));
    if (!pattern || !directory) {
        free(pattern);
        free(directory);
        return NULL;
    }
    memcpy(pattern, path, length);
    if (length && path[length - 1] != '/' && path[length - 1] != '\\') pattern[length++] = '/';
    pattern[length++] = '*';
    pattern[length] = '\0';
    directory->handle = FindFirstFileA(pattern, &directory->data);
    directory->first = 1;
    free(pattern);
    return directory;
}

static struct dirent *readdir(DIR *directory) {
    if (!directory || directory->handle == INVALID_HANDLE_VALUE) return NULL;
    if (directory->first) directory->first = 0;
    else if (!FindNextFileA(directory->handle, &directory->data)) return NULL;
    memcpy(directory->entry.d_name, directory->data.cFileName,
           sizeof(directory->entry.d_name));
    directory->entry.d_name[sizeof(directory->entry.d_name) - 1] = '\0';
    return &directory->entry;
}

static int closedir(DIR *directory) {
    if (!directory) return -1;
    if (directory->handle != INVALID_HANDLE_VALUE) FindClose(directory->handle);
    free(directory);
    return 0;
}
#else
#include <dirent.h>
#endif

static char *copy_string(const char *text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static AstSourceSpan token_span(const Token *token) {
    AstSourceSpan span;
    span.begin.line = token->line;
    span.begin.column = token->column;
    span.end.line = token->end_line;
    span.end.column = token->end_column;
    return span;
}

static AstDeclarationKind declaration_kind(TokenType first, TokenType second) {
    if (first == TOKEN_KEYWORD_PUB) first = second;
    if (first == TOKEN_KEYWORD_IMPORT) return AST_DECL_IMPORT;
    if (first == TOKEN_KEYWORD_STRUCT) return AST_DECL_STRUCT;
    if (first == TOKEN_KEYWORD_ENUM) return AST_DECL_ENUM;
    if (first == TOKEN_KEYWORD_INTERFACE) return AST_DECL_INTERFACE;
    if (first == TOKEN_KEYWORD_FUNC) return AST_DECL_FUNCTION;
    if (first == TOKEN_KEYWORD_CONST) return AST_DECL_CONSTANT;
    if (first == TOKEN_KEYWORD_VAR) return AST_DECL_VARIABLE;
    return AST_DECL_INVALID;
}

static size_t declaration_end(const AstProgram *program, size_t first) {
    AstDeclarationKind kind = declaration_kind(
        program->tokens[first].type,
        first + 1 < program->token_count ? program->tokens[first + 1].type : TOKEN_EOF);

    if (kind == AST_DECL_IMPORT) {
        size_t index = first;
        while (index < program->token_count && program->tokens[index].type != TOKEN_SEMICOLON &&
               program->tokens[index].type != TOKEN_EOF)
            index++;
        return index < program->token_count && program->tokens[index].type == TOKEN_SEMICOLON ? index + 1 : index;
    }
    if (kind == AST_DECL_CONSTANT || kind == AST_DECL_VARIABLE) {
        size_t index = first + 1;
        if (kind == AST_DECL_CONSTANT || kind == AST_DECL_VARIABLE) {
            while (index < program->token_count &&
                   program->tokens[index].type != TOKEN_SEMICOLON &&
                   program->tokens[index].type != TOKEN_EOF)
                index++;
            return index < program->token_count &&
                   program->tokens[index].type == TOKEN_SEMICOLON
                       ? index + 1
                       : index;
        }
        if (index < program->token_count && program->tokens[index].type == TOKEN_LPAREN) {
            while (index < program->token_count && program->tokens[index].type != TOKEN_RPAREN &&
                   program->tokens[index].type != TOKEN_EOF)
                index++;
            return index < program->token_count ? index + 1 : index;
        }
        if (index < program->token_count && program->tokens[index].type == TOKEN_STRING_LITERAL) return index + 1;
        while (index < program->token_count && program->tokens[index].type != TOKEN_GREATER &&
               program->tokens[index].type != TOKEN_EOF)
            index++;
        return index < program->token_count ? index + 1 : index;
    }

    size_t index = first;
    while (index < program->token_count && program->tokens[index].type != TOKEN_LBRACE &&
           program->tokens[index].type != TOKEN_EOF)
        index++;
    if (index == program->token_count || program->tokens[index].type == TOKEN_EOF) return index;

    int depth = 0;
    do {
        if (program->tokens[index].type == TOKEN_LBRACE) depth++;
        if (program->tokens[index].type == TOKEN_RBRACE) depth--;
        index++;
    } while (index < program->token_count && depth > 0 && program->tokens[index].type != TOKEN_EOF);
    return index;
}

static int build_declarations(AstProgram *program) {
    size_t capacity = 0;
    size_t index = 0;
    if (program->token_count >= 3 && program->tokens[0].type == TOKEN_KEYWORD_PACKAGE &&
        program->tokens[2].type == TOKEN_SEMICOLON)
        index = 3;
    while (index < program->token_count && program->tokens[index].type != TOKEN_EOF) {
        size_t end = declaration_end(program, index);
        if (end <= index) end = index + 1;
        if (program->declaration_count == capacity) {
            size_t next = capacity == 0 ? 16 : capacity * 2;
            if (next < capacity || next > SIZE_MAX / sizeof(AstDeclaration)) return 0;
            AstDeclaration *grown = realloc(program->declarations, next * sizeof(*grown));
            if (grown == NULL) return 0;
            program->declarations = grown;
            capacity = next;
        }
        AstDeclaration *declaration = &program->declarations[program->declaration_count++];
        declaration->kind = declaration_kind(
            program->tokens[index].type,
            index + 1 < program->token_count ? program->tokens[index + 1].type : TOKEN_EOF);
        declaration->first_token = index;
        declaration->token_count = end - index;
        declaration->span.begin = program->tokens[index].span.begin;
        declaration->span.end = program->tokens[end - 1].span.end;
        index = end;
    }
    return 1;
}

static AstProgram *parse_stream(TokenStream *stream, const char *source_path,
                                const FrontendOptions *options, StringInterner *strings) {
    if (options != NULL && options->show_tokens) print_tokens(stream);
    if (stream->has_error) {
        free_token_stream(stream);
        return NULL;
    }

    AstProgram *program = calloc(1, sizeof(*program));
    if (program == NULL) goto allocation_failure;
    program->strings = strings;
    program->source_path = copy_string(source_path);
    if (program->source_path == NULL) goto allocation_failure;
    program->token_count = (size_t) stream->count;
    if (program->token_count > SIZE_MAX / sizeof(*program->tokens)) goto allocation_failure;
    program->tokens = calloc(program->token_count, sizeof(*program->tokens));
    if (program->tokens == NULL && program->token_count != 0) goto allocation_failure;

    for (size_t i = 0; i < program->token_count; i++) {
        program->tokens[i].type = stream->tokens[i].type;
        program->tokens[i].lexeme = stream->tokens[i].value;
        program->tokens[i].span = token_span(&stream->tokens[i]);
    }
    free_token_stream(stream);

    if (!build_declarations(program)) {
        ast_program_free(program);
        error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INTERNAL_FAILURE, source_path, "Out of memory while building AST");
        return NULL;
    }
    /* Semantic analysis and IR lowering consume the structured tree. */
    (void) frontend_build_structured_ast_recover(program, options != NULL && options->recover_syntax);
    return program;

allocation_failure:
    free_token_stream(stream);
    ast_program_free(program);
    error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                 ERR_COMP_INTERNAL_FAILURE, source_path, "Out of memory while building AST");
    return NULL;
}

static AstProgram *parse_single_file(const char *source_path, const FrontendOptions *options,
                                     StringInterner *strings) {
    const int debug = options != NULL && options->debug;
    TokenStream *stream = tokenize_file_with_interner(source_path, debug, strings);
    return stream == NULL ? NULL : parse_stream(stream, source_path, options, strings);
}

static char *joined_path(const char *directory, const char *path) {
    size_t directory_length = strlen(directory);
    size_t path_length = strlen(path);
    int separator = directory_length != 0 && directory[directory_length - 1] != '/' &&
                    directory[directory_length - 1] != '\\';
    if (directory_length > SIZE_MAX - path_length - (size_t) separator - 1U) return NULL;
    char *result = malloc(directory_length + path_length + (size_t) separator + 1U);
    if (result == NULL) return NULL;
    memcpy(result, directory, directory_length);
    if (separator) result[directory_length++] = '/';
    memcpy(result + directory_length, path, path_length + 1U);
    return result;
}

static int path_is_directory(const char *path) {
    struct stat status;
    if (path == NULL || stat(path, &status) != 0) return 0;
#ifdef _WIN32
    return (status.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(status.st_mode);
#endif
}

static char *canonical_existing_path(const char *path) {
    char buffer[4096];
#ifdef _WIN32
    HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    DWORD count = GetFinalPathNameByHandleA(handle, buffer, (DWORD) sizeof(buffer), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (!count || count >= sizeof(buffer)) return NULL;
    if (!strncmp(buffer, "\\\\?\\", 4)) memmove(buffer, buffer + 4, strlen(buffer + 4) + 1);
#else
    if (realpath(path, buffer) == NULL) return NULL;
#endif
    for (char *p = buffer; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
    size_t length = strlen(buffer);
    while (length > 3 && buffer[length - 1] == '/') buffer[--length] = '\0';
    return copy_string(buffer);
}

static int path_prefix(const char *path, const char *root) {
    size_t length = strlen(root);
#ifdef _WIN32
    if (_strnicmp(path, root, length) != 0) return 0;
#else
    if (strncmp(path, root, length) != 0) return 0;
#endif
    return path[length] == '\0' || path[length] == '/';
}

static char *source_directory(const char *path) {
    char *copy = copy_string(path);
    if (copy == NULL) return NULL;
    char *slash = strrchr(copy, '/');
    if (slash == NULL) copy[0] = '\0';
    else *slash = '\0';
    return copy;
}

static int append_owned_import(AstProgram *root, AstProgram *imported) {
    if (root->owned_import_count == root->owned_import_capacity) {
        size_t next = root->owned_import_capacity == 0 ? 8 : root->owned_import_capacity * 2;
        if (next < root->owned_import_capacity ||
            next > SIZE_MAX / sizeof(*root->owned_imports))
            return 0;
        AstProgram **grown = realloc(root->owned_imports, next * sizeof(*grown));
        if (grown == NULL) return 0;
        root->owned_imports = grown;
        root->owned_import_capacity = next;
    }
    root->owned_imports[root->owned_import_count++] = imported;
    return 1;
}

static int record_loaded_source(AstProgram *root, const char *path) {
    for (size_t i = 0; i < root->loaded_source_count; i++)
        if (strcmp(root->loaded_source_paths[i], path) == 0) return 1;
    if (root->loaded_source_count == root->loaded_source_capacity) {
        size_t next = root->loaded_source_capacity == 0 ? 8 : root->loaded_source_capacity * 2;
        if (next < root->loaded_source_capacity || next > SIZE_MAX / sizeof(char *)) return 0;
        char **grown = realloc(root->loaded_source_paths, next * sizeof(*grown));
        if (grown == NULL) return 0;
        root->loaded_source_paths = grown;
        root->loaded_source_capacity = next;
    }
    char *copy = copy_string(path);
    if (copy == NULL) return 0;
    root->loaded_source_paths[root->loaded_source_count++] = copy;
    return 1;
}

#include "package_loader.inc"
#include "manifest_commands.inc"

AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options) {
    StringInterner *strings = string_interner_create();
    if (strings == NULL) return NULL;
    char *canonical_source = canonical_existing_path(source_path);
    if (canonical_source && path_is_directory(canonical_source)) {
        DIR *directory = opendir(canonical_source);
        struct dirent *entry;
        char *selected = NULL;
        if (directory) {
            while ((entry = readdir(directory)) != NULL) {
                size_t n = strlen(entry->d_name);
                if (n > 4 && !strcmp(entry->d_name + n - 4, ".dmm")) {
                    char *candidate = joined_path(canonical_source, entry->d_name);
                    if (candidate && !path_is_directory(candidate) && (!selected || strcmp(candidate, selected) < 0)) {
                        free(selected);
                        selected = candidate;
                    } else free(candidate);
                }
            }
            closedir(directory);
        }
        free(canonical_source);
        canonical_source = selected;
        if (!canonical_source) {
            error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_NOT_FOUND,
                         source_path, "Package directory has no DMM source files");
            string_interner_free(strings);
            return NULL;
        }
    }
    AstProgram *program = parse_single_file(canonical_source == NULL ? source_path : canonical_source,
                                            options, strings);
    free(canonical_source);
    if (program == NULL) {
        string_interner_free(strings);
        return NULL;
    }
    program->owns_strings = 1;
    if (!load_module_packages(program, options) && error_handler_get_error_count(global_error_handler) == 0) {
        ast_program_free(program);
        return NULL;
    }
    return program;
}

AstProgram *frontend_parse_source(const char *source, size_t length,
                                  const char *source_name,
                                  const FrontendOptions *options) {
    if (source == NULL && length != 0) return NULL;
    if (source_name == NULL) source_name = "<memory>";
    StringInterner *strings = string_interner_create();
    if (strings == NULL) return NULL;
    TokenStream *stream = tokenize_source_with_interner(source == NULL ? "" : source,
                                                        length, source_name, strings);
    if (stream == NULL) {
        string_interner_free(strings);
        return NULL;
    }
    AstProgram *program = parse_stream(stream, source_name, options, strings);
    if (program == NULL) {
        string_interner_free(strings);
        return NULL;
    }
    program->owns_strings = 1;
    return program;
}
