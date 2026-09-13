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
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
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
    (void) second;
    if (first == TOKEN_KEYWORD_IMPORT) return AST_DECL_IMPORT;
    if (first == TOKEN_KEYWORD_STRUCT) return AST_DECL_STRUCT;
    if (first == TOKEN_KEYWORD_ENUM) return AST_DECL_ENUM;
    if (first == TOKEN_KEYWORD_TRAIT) return AST_DECL_TRAIT;
    if (first == TOKEN_KEYWORD_IMPL) return AST_DECL_IMPL;
    if (first == TOKEN_KEYWORD_FUNC) return AST_DECL_FUNCTION;
    if (first == TOKEN_KEYWORD_CONST) return AST_DECL_CONSTANT;
    return AST_DECL_INVALID;
}

static size_t declaration_end(const AstProgram *program, size_t first) {
    AstDeclarationKind kind = declaration_kind(
        program->tokens[first].type,
        first + 1 < program->token_count ? program->tokens[first + 1].type : TOKEN_EOF);

    if (kind == AST_DECL_IMPORT || kind == AST_DECL_CONSTANT) {
        size_t index = first + 1;
        if (kind == AST_DECL_CONSTANT) {
            while (index < program->token_count &&
                   program->tokens[index].type != TOKEN_SEMICOLON &&
                   program->tokens[index].type != TOKEN_EOF) index++;
            return index < program->token_count &&
                   program->tokens[index].type == TOKEN_SEMICOLON ? index + 1 : index;
        }
        if (index < program->token_count && program->tokens[index].type == TOKEN_LPAREN) {
            while (index < program->token_count && program->tokens[index].type != TOKEN_RPAREN &&
                   program->tokens[index].type != TOKEN_EOF) index++;
            return index < program->token_count ? index + 1 : index;
        }
        if (index < program->token_count && program->tokens[index].type == TOKEN_STRING_LITERAL) return index + 1;
        while (index < program->token_count && program->tokens[index].type != TOKEN_GREATER &&
               program->tokens[index].type != TOKEN_EOF) index++;
        return index < program->token_count ? index + 1 : index;
    }

    size_t index = first;
    while (index < program->token_count && program->tokens[index].type != TOKEN_LBRACE &&
           program->tokens[index].type != TOKEN_EOF) index++;
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

static char *relative_import_path(const char *source_path, const char *import_path) {
    const char *slash = strrchr(source_path, '/');
    const char *backslash = strrchr(source_path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    size_t directory_length = slash == NULL ? 0 : (size_t) (slash - source_path + 1);
    size_t import_length = strlen(import_path);
    if (directory_length > SIZE_MAX - import_length - 1) return NULL;
    char *result = malloc(directory_length + import_length + 1);
    if (result == NULL) return NULL;
    memcpy(result, source_path, directory_length);
    memcpy(result + directory_length, import_path, import_length + 1);
    return result;
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

static char *package_entry_path(char *path) {
    if (!path_is_directory(path)) return path;
    char *entry = joined_path(path, "package.dmm");
    free(path);
    return entry;
}

static char *canonical_existing_path(const char *path) {
    char buffer[4096];
#ifdef _WIN32
    if (_fullpath(buffer, path, sizeof(buffer)) == NULL) return NULL;
#else
    if (realpath(path, buffer) == NULL) return NULL;
#endif
    for (char *p = buffer; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
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

static char *logical_module_identity(const AstProgram *root,
                                     const char *canonical_path) {
    char *root_directory = source_directory(root->source_path);
    char *source_root = canonical_existing_path(DMM_SOURCE_ROOT);
    if (root_directory == NULL || source_root == NULL) {
        free(root_directory);
        free(source_root);
        return NULL;
    }
    const char *base = path_prefix(canonical_path, root_directory)
        ? root_directory : path_prefix(canonical_path, source_root) ? source_root : NULL;
    char *identity = NULL;
    if (base != NULL) {
        const char *relative = canonical_path + strlen(base);
        if (*relative == '/') relative++;
        identity = copy_string(*relative == '\0' ? "." : relative);
    }
    free(root_directory);
    free(source_root);
    return identity;
}

static char *import_path_text(const AstProgram *unit,
                              const AstImportPath *entry) {
    if (entry->path_token != AST_TOKEN_NONE) {
        const char *text = ast_program_lexeme(unit, entry->path_token);
        size_t length = strlen(text);
        char *copy = malloc(length + 1U);
        if (copy != NULL) memcpy(copy, text, length + 1U);
        return copy;
    }
    size_t length = 0;
    size_t first = entry->path_first_token;
    size_t count = entry->path_token_count;
    for (size_t i = 0; i < count && first + i < unit->token_count; i++) {
        const AstToken *token = &unit->tokens[first + i];
        if (token->type == TOKEN_GREATER) break;
        size_t token_length = strlen(token->lexeme);
        if (length > SIZE_MAX - token_length) return NULL;
        length += token_length;
    }
    char *result = malloc(length + 1U);
    if (result == NULL) return NULL;
    size_t offset = 0;
    for (size_t i = 0; i < count && first + i < unit->token_count; i++) {
        const AstToken *token = &unit->tokens[first + i];
        if (token->type == TOKEN_GREATER) break;
        size_t token_length = strlen(token->lexeme);
        memcpy(result + offset, token->lexeme, token_length);
        offset += token_length;
    }
    result[offset] = '\0';
    return result;
}

static AstProgram *known_import(const AstProgram *root, const char *path) {
    if (strcmp(root->source_path, path) == 0) return (AstProgram *) root;
    for (size_t i = 0; i < root->owned_import_count; i++)
        if (strcmp(root->owned_imports[i]->source_path, path) == 0)
            return root->owned_imports[i];
    return NULL;
}

static int append_owned_import(AstProgram *root, AstProgram *imported) {
    if (root->owned_import_count == root->owned_import_capacity) {
        size_t next = root->owned_import_capacity == 0 ? 8 : root->owned_import_capacity * 2;
        if (next < root->owned_import_capacity ||
            next > SIZE_MAX / sizeof(*root->owned_imports)) return 0;
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
        root->loaded_source_paths = grown; root->loaded_source_capacity = next;
    }
    char *copy = copy_string(path);
    if (copy == NULL) return 0;
    root->loaded_source_paths[root->loaded_source_count++] = copy;
    return 1;
}

static int resolve_imports(AstProgram *root, AstProgram *unit,
                           const FrontendOptions *options) {
    for (AstDeclarationNode *declaration = unit->root; declaration != NULL;
         declaration = declaration->next) {
        if (declaration->kind != AST_DECL_IMPORT) continue;
        for (AstImportPath *entry = declaration->as.import_decl.paths; entry != NULL; entry = entry->next) {
            char *import_text = import_path_text(unit, entry);
            if (import_text == NULL) return 0;
            char *path = relative_import_path(unit->source_path, import_text);
            if (path == NULL) {
                free(import_text);
                return 0;
            }
            path = package_entry_path(path);
            if (path == NULL) {
                free(import_text);
                return 0;
            }
            FILE *probe = fopen(path, "rb");
            if (probe == NULL && entry->path_token == AST_TOKEN_NONE) {
                free(path);
                path = joined_path(DMM_SOURCE_ROOT, import_text);
                if (path == NULL) {
                    free(import_text);
                    return 0;
                }
                path = package_entry_path(path);
                if (path == NULL) {
                    free(import_text);
                    return 0;
                }
                probe = fopen(path, "rb");
            }
            if (probe == NULL) {
                error_report(global_error_handler, SEVERITY_ERROR,
                             entry->span.begin.line,
                             entry->span.begin.column,
                             ERROR_CATEGORY_COMPILER, ERR_COMP_IMPORT_NOT_FOUND,
                             unit->source_path, "Failed to open import file '%s': %s", path, strerror(errno));
                free(path);
                free(import_text);
                continue;
            }
            (void) fclose(probe);
            char *canonical = canonical_existing_path(path);
            free(path);
            path = canonical;
            if (path == NULL) {
                free(import_text);
                return 0;
            }
            if (!record_loaded_source(root, path)) {
                free(path); free(import_text); return 0;
            }
            AstProgram *imported = known_import(root, path);
            if (imported == NULL) {
                imported = parse_single_file(path, options, root->strings);
                if (imported == NULL) {
                    free(path);
                    free(import_text);
                    continue;
                }
                if (!append_owned_import(root, imported)) {
                    ast_program_free(imported);
                    free(path);
                    free(import_text);
                    return 0;
                }
                imported->module_identity = logical_module_identity(root, path);
                if (imported->module_identity == NULL) {
                    error_report(global_error_handler, SEVERITY_ERROR,
                                 entry->span.begin.line,
                                 entry->span.begin.column,
                                 ERROR_CATEGORY_COMPILER, ERR_COMP_IMPORT_OUTSIDE_ROOT,
                                 unit->source_path,
                                 "Import escapes the source or package root");
                    free(path);
                    free(import_text);
                    return 0;
                }
                if (!resolve_imports(root, imported, options)) {
                    free(path);
                    free(import_text);
                    return 0;
                }
            }
            entry->resolved_program = imported;
            free(path);
            free(import_text);
        }
    }
    return 1;
}

AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options) {
    StringInterner *strings = string_interner_create();
    if (strings == NULL) return NULL;
    char *canonical_source = canonical_existing_path(source_path);
    AstProgram *program = parse_single_file(canonical_source == NULL ? source_path : canonical_source,
                                            options, strings);
    free(canonical_source);
    if (program == NULL) {
        string_interner_free(strings);
        return NULL;
    }
    program->owns_strings = 1;
    program->module_identity = copy_string(".");
    if (program->module_identity == NULL) {
        ast_program_free(program);
        return NULL;
    }
    if (!resolve_imports(program, program, options)) {
        ast_program_free(program);
        error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_INTERNAL_FAILURE, source_path,
                     "Out of memory while resolving AST imports");
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
