#include "ast.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct AstArenaBlock {
    struct AstArenaBlock *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
} AstArenaBlock;

void *ast_program_alloc(AstProgram *program, size_t size) {
    if (program == NULL || size == 0) return NULL;
    const size_t alignment = _Alignof(max_align_t);
    if (size > SIZE_MAX - (alignment - 1)) return NULL;
    size = (size + alignment - 1) & ~(alignment - 1);
    AstArenaBlock *block = program->arena;
    if (block == NULL || size > block->capacity - block->used) {
        size_t capacity = 64U * 1024U;
        if (capacity < size) capacity = size;
        if (capacity > SIZE_MAX - sizeof(*block)) return NULL;
        AstArenaBlock *created = calloc(1, sizeof(*created) + capacity);
        if (created == NULL) return NULL;
        created->capacity = capacity;
        created->next = block;
        program->arena = created;
        block = created;
    }
    void *result = block->data + block->used;
    block->used += size;
    return result;
}

void ast_program_free(AstProgram *program) {
    if (program == NULL) return;
    AstArenaBlock *block = program->arena;
    while (block != NULL) {
        AstArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    free(program->source_path);
    free(program->tokens);
    free(program->declarations);
    free(program);
}

const AstToken *ast_program_token(const AstProgram *program, size_t index) {
    if (program == NULL || index >= program->token_count) return NULL;
    return &program->tokens[index];
}

const char *ast_program_lexeme(const AstProgram *program, size_t index) {
    const AstToken *token = ast_program_token(program, index);
    return token == NULL ? "" : token->lexeme;
}

const char *ast_declaration_kind_name(AstDeclarationKind kind) {
    switch (kind) {
        case AST_DECL_IMPORT: return "import";
        case AST_DECL_STRUCT: return "struct";
        case AST_DECL_ENUM: return "enum";
        case AST_DECL_FUNCTION: return "function";
        case AST_DECL_INVALID: return "invalid";
    }
    return "invalid";
}
