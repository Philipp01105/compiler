#include "frontend.h"

#include "lexer.h"

static int quoted(FILE *output, const char *text) {
    if (fputc('"', output) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *) (text == NULL ? "" : text); *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', output) == EOF || fputc(*p, output) == EOF) return 0;
        } else if (*p == '\n') {
            if (fputs("\\n", output) == EOF) return 0;
        } else if (*p == '\r') {
            if (fputs("\\r", output) == EOF) return 0;
        } else if (*p == '\t') {
            if (fputs("\\t", output) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(output, "\\x%02x", *p) < 0) return 0;
        } else if (fputc(*p, output) == EOF) return 0;
    }
    return fputc('"', output) != EOF;
}

static int dump_unit(FILE *output, const AstProgram *program, size_t unit, const char *role) {
    if (fprintf(output, "unit #%zu role=%s path=", unit, role) < 0 ||
        !quoted(output, program->source_path) ||
        fprintf(output, " tokens=%zu\n", program->token_count) < 0)
        return 0;
    for (size_t i = 0; i < program->token_count; i++) {
        const AstToken *token = &program->tokens[i];
        if (fprintf(output, "  token #%zu type=%s span=%d:%d-%d:%d text=", i,
                    token_type_to_string(token->type), token->span.begin.line, token->span.begin.column,
                    token->span.end.line, token->span.end.column) < 0 ||
            !quoted(output, token->lexeme) || fputc('\n', output) == EOF)
            return 0;
    }
    return 1;
}

int frontend_dump_tokens(FILE *output, const AstProgram *program) {
    if (output == NULL || program == NULL || fputs("dmm-tokens-v1\n", output) == EOF ||
        !dump_unit(output, program, 0, "root"))
        return 0;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (!dump_unit(output, program->owned_imports[i], i + 1U, "import")) return 0;
    return !ferror(output);
}
