#include "frontend.h"

#include "errorHandler.h"
#include "lexer.h"
#include "syntax_parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    span.end.line = token->line;
    span.end.column = token->column + (int) strlen(token->value);
    return span;
}

static AstDeclarationKind declaration_kind(TokenType first, TokenType second) {
    if (first == TOKEN_HASH && second == TOKEN_KEYWORD_IMPORT) return AST_DECL_IMPORT;
    if (first == TOKEN_KEYWORD_STRUCT) return AST_DECL_STRUCT;
    if (first == TOKEN_KEYWORD_ENUM) return AST_DECL_ENUM;
    if (first == TOKEN_KEYWORD_FUNC) return AST_DECL_FUNCTION;
    return AST_DECL_INVALID;
}

static size_t declaration_end(const AstProgram *program, size_t first) {
    AstDeclarationKind kind = declaration_kind(
        program->tokens[first].type,
        first + 1 < program->token_count ? program->tokens[first + 1].type : TOKEN_EOF);

    if (kind == AST_DECL_IMPORT) {
        size_t index = first + 2;
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

AstProgram *frontend_parse_file(const char *source_path, const FrontendOptions *options) {
    const int debug = options != NULL && options->debug;
    TokenStream *stream = tokenize_file(source_path, debug);
    if (stream == NULL) return NULL;
    if (options != NULL && options->show_tokens) print_tokens(stream);
    if (stream->has_error) {
        free_token_stream(stream);
        return NULL;
    }

    AstProgram *program = calloc(1, sizeof(*program));
    if (program == NULL) goto allocation_failure;
    program->source_path = copy_string(source_path);
    if (program->source_path == NULL) goto allocation_failure;
    program->token_count = (size_t) stream->count;
    if (program->token_count > SIZE_MAX / sizeof(*program->tokens)) goto allocation_failure;
    program->tokens = calloc(program->token_count, sizeof(*program->tokens));
    if (program->tokens == NULL && program->token_count != 0) goto allocation_failure;

    for (size_t i = 0; i < program->token_count; i++) {
        program->tokens[i].type = stream->tokens[i].type;
        memcpy(program->tokens[i].lexeme, stream->tokens[i].value, MAX_TOKEN);
        program->tokens[i].span = token_span(&stream->tokens[i]);
    }
    free_token_stream(stream);

    if (!build_declarations(program)) {
        ast_program_free(program);
        error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_CODEGEN_OUTPUT_FAILED, source_path, "Out of memory while building AST");
        return NULL;
    }
    /* The compatibility declaration index above remains available to the old
       emitter; all new compiler stages consume the structured tree. */
    (void) frontend_build_structured_ast(program);
    return program;

allocation_failure:
    free_token_stream(stream);
    ast_program_free(program);
    error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                 ERR_CODEGEN_OUTPUT_FAILED, source_path, "Out of memory while building AST");
    return NULL;
}
