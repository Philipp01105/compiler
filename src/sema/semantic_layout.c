#include "semantic_internal.h"
#include "generics.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int native_layout_depth(const SemanticModel *model, const AstProgram *program,
                               const AstType *type, NativeTypeLayout *layout, size_t depth) {
    if (!model || !program || !type || depth > model->symbol_count + 64 ||
        type->borrow_kind != AST_BORROW_NONE)
        return 0;
    if (type->outer_pointer_depth ||
        (type->pointer_depth && !type->is_array && !type->is_slice)) {
        *layout = (NativeTypeLayout)
        {
            8, 8
        };
        return 1;
    }
    if (type->is_array && type->kind == AST_TYPE_FUNCTION) {
        AstType element = ast_type_element(type);
        size_t length = type->resolved_array_length;
        if (!length && type->array_length_token < program->token_count)
            length = (size_t) strtoull(ast_program_lexeme(program, type->array_length_token), NULL, 10);
        if (!length || !native_layout_depth(model, program, &element, layout, depth + 1) ||
            length > SIZE_MAX / layout->size)
            return 0;
        layout->size *= length;
        return 1;
    }
    if (type->kind == AST_TYPE_FUNCTION && type->is_native_function &&
        !type->function_generic_parameters && !type->is_array && !type->is_slice) {
        for (const AstTypeArgument *p = type->function_parameters; p; p = p->next) {
            NativeTypeLayout ignored;
            if (p->type.borrow_kind != AST_BORROW_NONE || p->type.is_array || p->type.is_slice ||
                !native_layout_depth(model, program, &p->type, &ignored, depth + 1))
                return 0;
        }
        const AstType *result = type->function_return_type;
        NativeTypeLayout ignored;
        if (!result || result->borrow_kind != AST_BORROW_NONE || result->is_array || result->is_slice ||
            (!native_layout_depth(model, program, result, &ignored, depth + 1) &&
             !(result->kind == AST_TYPE_NAMED && !result->pointer_depth && primitive_type(program, result) ==
               TYPE_VOID)))
            return 0;
        *layout = (NativeTypeLayout)
        {
            8, 8
        };
        return 1;
    }
    if (type->is_slice || type->kind != AST_TYPE_NAMED || type->arguments) return 0;
    if (type->is_array) {
        AstType element = ast_type_element(type);
        size_t length = type->resolved_array_length;
        if (!length && type->array_length_token < program->token_count &&
            program->tokens[type->array_length_token].type == TOKEN_NUMBER)
            length = (size_t) strtoull(ast_program_lexeme(program, type->array_length_token), NULL, 10);
        if (!length || !native_layout_depth(model, program, &element, layout, depth + 1) ||
            length > SIZE_MAX / layout->size)
            return 0;
        layout->size *= length;
        return 1;
    }
    DataType primitive = primitive_type(program, type);
    if (data_type_fixed_integer(primitive) || primitive == TYPE_BIT ||
        primitive == TYPE_FLOAT || primitive == TYPE_DOUBLE) {
        size_t size = data_type_bytes(primitive);
        *layout = (NativeTypeLayout)
        {
            size, size
        };
        return 1;
    }
    if (primitive != TYPE_UNKNOWN) return 0;
    Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) program};
    size_t id = resolve_named_symbol_id(&lookup, program, type->name_token);
    if (id >= model->symbol_count) return 0;
    const SemanticSymbol *symbol = &model->symbols[id];
    const AstDeclarationNode *decl = symbol->declaration;
    if (!decl || symbol->kind != SEMANTIC_SYMBOL_STRUCT || !decl->is_native || decl->is_opaque)
        return 0;
    size_t size = 0, alignment = 1;
    for (const AstField *field = decl->as.struct_decl.fields; field; field = field->next) {
        NativeTypeLayout child;
        if (!native_layout_depth(model, symbol->source_program, &field->type, &child, depth + 1) ||
            size > SIZE_MAX - (child.alignment - 1))
            return 0;
        if (decl->native_pack && child.alignment > decl->native_pack) child.alignment = decl->native_pack;
        if (!decl->is_native_union) size = (size + child.alignment - 1) & ~(child.alignment - 1);
        if (child.size > SIZE_MAX - size) return 0;
        if (decl->is_native_union) { if (child.size > size) size = child.size; } else size += child.size;
        if (child.alignment > alignment) alignment = child.alignment;
    }
    if (decl->native_alignment > alignment) alignment = decl->native_alignment;
    /* Empty C structs are an extension with incompatible compiler layouts. */
    if (!size || size > SIZE_MAX - (alignment - 1)) return 0;
    *layout = (NativeTypeLayout)
    {
        (size + alignment - 1) & ~(alignment - 1), alignment
    };
    return 1;
}

int semantic_native_layout(const SemanticModel *model, const AstProgram *program,
                           const AstType *type, NativeTypeLayout *layout) {
    return layout && native_layout_depth(model, program, type, layout, 0);
}

int semantic_native_field_offset(const SemanticModel *model, size_t symbol_id,
                                 size_t field_index, size_t *offset) {
    if (!model || !offset || symbol_id >= model->symbol_count) return 0;
    const SemanticSymbol *symbol = &model->symbols[symbol_id];
    const AstDeclarationNode *decl = symbol->declaration;
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || !decl || !decl->is_native || decl->is_opaque)
        return 0;
    size_t size = 0, index = 0;
    for (const AstField *field = decl->as.struct_decl.fields; field; field = field->next, index++) {
        NativeTypeLayout child;
        if (!semantic_native_layout(model, symbol->source_program, &field->type, &child) ||
            size > SIZE_MAX - (child.alignment - 1))
            return 0;
        if (decl->native_pack && child.alignment > decl->native_pack) child.alignment = decl->native_pack;
        size = (size + child.alignment - 1) & ~(child.alignment - 1);
        if (index == field_index) {
            *offset = decl->is_native_union ? 0 : size;
            return 1;
        }
        if (child.size > SIZE_MAX - size) return 0;
        size += child.size;
    }
    return 0;
}

static int native_signature_type(Analyzer *analyzer, const AstType *type, int result) {
    if (!known_declared_type(analyzer, type) || type->borrow_kind != AST_BORROW_NONE) return 0;
    if (type->kind == AST_TYPE_INFERRED) return 0;
    if (type->outer_pointer_depth) return 1;
    if (type->is_array || type->is_slice) return 0;
    if (type->pointer_depth) return 1;
    if (result && type->kind == AST_TYPE_NAMED && !type->arguments &&
        primitive_type(analyzer->program, type) == TYPE_VOID)
        return 1;
    NativeTypeLayout layout;
    return semantic_native_layout(analyzer->model, analyzer->program, type, &layout);
}

static int native_identifier(const char *name, int library) {
    if (!name || (!isalpha((unsigned char) *name) && *name != '_')) return 0;
    for (name++; *name; name++)
        if (!isalnum((unsigned char) *name) && *name != '_' && !(library && *name == '-')) return 0;
    return 1;
}

static int same_native_source_type(const Analyzer *analyzer, const AstProgram *a, const AstType *x,
                                   const AstProgram *b, const AstType *y) {
    if (!ast_concrete_type_equal(a, x, b, y)) return 0;
    if (x->element_type)
        return same_native_source_type(analyzer, a, x->element_type, b, y->element_type);
    if (x->kind == AST_TYPE_FUNCTION) {
        if (!same_native_source_type(analyzer, a, x->function_return_type, b, y->function_return_type)) return 0;
        const AstTypeArgument *p = x->function_parameters, *q = y->function_parameters;
        for (; p && q; p = p->next, q = q->next)
            if (!same_native_source_type(analyzer, a, &p->type, b, &q->type)) return 0;
        return !p && !q;
    }
    if (x->kind == AST_TYPE_NAMED && primitive_type(a, x) == TYPE_UNKNOWN) {
        size_t left = resolve_named_symbol_id(analyzer, a, x->name_token);
        size_t right = resolve_named_symbol_id(analyzer, b, y->name_token);
        return left != AST_SYMBOL_NONE && left == right;
    }
    return 1;
}

void validate_native_declarations(Analyzer *analyzer) {
    AstProgram *saved = analyzer->program;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        const AstDeclarationNode *decl = symbol->declaration;
        if (!decl || (!decl->is_native && !decl->is_native_export) ||
            (symbol->kind != SEMANTIC_SYMBOL_STRUCT && symbol->kind != SEMANTIC_SYMBOL_FUNCTION))
            continue;
        analyzer->program = (AstProgram *) symbol->source_program;
        if (symbol->kind == SEMANTIC_SYMBOL_STRUCT) {
            if (decl->is_opaque) continue;
            for (const AstField *field = decl->as.struct_decl.fields; field; field = field->next) {
                NativeTypeLayout layout;
                if (!known_declared_type(analyzer, &field->type) ||
                    !semantic_native_layout(analyzer->model, analyzer->program, &field->type, &layout))
                    semantic_error(analyzer, field->type.name_token, ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION, "Native field requires a complete FFI-safe type");
            }
            AstType type = {.kind = AST_TYPE_NAMED, .name_token = decl->name_token};
            NativeTypeLayout layout;
            if (!semantic_native_layout(analyzer->model, analyzer->program, &type, &layout))
                semantic_error(analyzer, decl->name_token, ERROR_CATEGORY_TYPE,
                               ERR_TYPE_INVALID_OPERATION,
                               "Native struct layout is empty, incomplete, recursive or too large");
        } else {
            const char *library = decl->is_native_export
                                      ? ""
                                      : ast_program_lexeme(analyzer->program, decl->native_library_token);
            size_t name_token = decl->is_native_export ? decl->name_token : decl->native_name_token;
            const char *name = ast_program_lexeme(analyzer->program, name_token);
            if (decl->is_native_export && (decl->as.function.is_async || decl->generic_parameters))
                semantic_error(analyzer, decl->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Native exports must be synchronous and non-generic");
            if (decl->is_native && !native_identifier(library, 1))
                semantic_error(analyzer, decl->native_library_token, ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "from requires a logical library ID (letters, digits, underscore or hyphen), not a file or linker option");
            if (!native_identifier(name, 0))
                semantic_error(analyzer, name_token, ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION, "Native symbol alias must be a C identifier");
            for (size_t j = 0; j < i; j++) {
                const SemanticSymbol *other = &analyzer->model->symbols[j];
                const AstDeclarationNode *previous = other->declaration;
                size_t previous_name = previous && previous->is_native_export
                                           ? previous->name_token
                                           : previous
                                                 ? previous->native_name_token
                                                 : AST_TOKEN_NONE;
                if (other->kind != SEMANTIC_SYMBOL_FUNCTION || !previous ||
                    (!previous->is_native && !previous->is_native_export) ||
                    strcmp(name, ast_program_lexeme(other->source_program, previous_name)))
                    continue;
                const char *previous_library = previous->is_native_export
                                                   ? ""
                                                   : ast_program_lexeme(
                                                       other->source_program, previous->native_library_token);
                int conflict = decl->is_native_export || previous->is_native_export || strcmp(library, previous_library)
                               != 0 ||
                               !same_native_source_type(analyzer, analyzer->program, &decl->as.function.return_type,
                                                        other->source_program, &previous->as.function.return_type);
                const AstParameter *a = decl->as.function.parameters, *b = previous->as.function.parameters;
                for (; a && b; a = a->next, b = b->next)
                    if (!same_native_source_type(analyzer, analyzer->program, &a->type, other->source_program,
                                                 &b->type)) conflict = 1;
                if (a || b) conflict = 1;
                if (conflict)
                    semantic_duplicate(analyzer, name_token, other->source_program,
                                       previous_name,
                                       "Native symbol has conflicting signatures or library assignments");
            }
            if (!native_signature_type(analyzer, &decl->as.function.return_type, 1))
                semantic_error(analyzer, decl->as.function.return_type.name_token, ERROR_CATEGORY_TYPE,
                               ERR_TYPE_INVALID_OPERATION,
                               "Native result must be an FFI-safe value, raw pointer or void");
            for (const AstParameter *p = decl->as.function.parameters; p; p = p->next)
                if (!native_signature_type(analyzer, &p->type, 0))
                    semantic_error(analyzer, p->type.name_token, ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Native parameter must be an FFI-safe value or raw pointer");
        }
    }
    analyzer->program = saved;
}

static size_t layout_alignment_depth(const Analyzer *analyzer, const AstProgram *program,
                                     const AstType *type, size_t depth) {
    if (depth > analyzer->model->symbol_count + 64) return 8;
    if (!type->is_array && !type->is_slice) {
        NativeTypeLayout native;
        if (semantic_native_layout(analyzer->model, program, type, &native)) return native.alignment;
    }
    if (type->outer_pointer_depth || type->borrow_kind != AST_BORROW_NONE || type->is_slice ||
        (!type->is_array && type->pointer_depth) || type->kind != AST_TYPE_NAMED)
        return 8;
    if (type->is_array) {
        AstType element = ast_type_element(type);
        size_t alignment = layout_alignment_depth(analyzer, program, &element, depth + 1);
        return alignment > 8 ? alignment : 8;
    }
    DataType primitive = primitive_type(program, type);
    if (primitive != TYPE_UNKNOWN) return data_type_bytes(primitive);
    size_t id = resolve_named_symbol_id(analyzer, program, type->name_token);
    if (id >= analyzer->model->symbol_count) return 8;
    const SemanticSymbol *symbol = &analyzer->model->symbols[id];
    const AstDeclarationNode *decl = symbol->declaration;
    size_t alignment = 8;
    if (decl && decl->kind == AST_DECL_STRUCT)
        for (const AstField *field = decl->as.struct_decl.fields; field; field = field->next) {
            size_t child = layout_alignment_depth(analyzer, symbol->source_program, &field->type, depth + 1);
            if (child > alignment) alignment = child;
        }
    if (decl && decl->kind == AST_DECL_ENUM)
        for (const AstEnumValue *v = decl->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                size_t child = layout_alignment_depth(analyzer, symbol->source_program, &p->type, depth + 1);
                if (child > alignment) alignment = child;
            }
    return alignment;
}

size_t layout_alignment(Analyzer *analyzer, const AstType *type) {
    return layout_alignment_depth(analyzer, analyzer->program, type, 0);
}

static int type_directly_embeds_value(const AstType *type) {
    if (type == NULL || type->outer_pointer_depth != 0 || type->is_slice)
        return 0;
    if (type->is_array) {
        AstType element = ast_type_element(type);
        return type_directly_embeds_value(&element);
    }
    return type->borrow_kind == AST_BORROW_NONE &&
           type->pointer_depth == 0;
}

int aggregate_reaches(const Analyzer *analyzer, size_t current_symbol,
                      size_t target_symbol, size_t depth) {
    if (current_symbol >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count)
        return 1;
    const SemanticSymbol *current = &analyzer->model->symbols[current_symbol];
    if (current->kind == SEMANTIC_SYMBOL_ENUM && current->declaration != NULL) {
        for (const AstEnumValue *v = current->declaration->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                if (!type_directly_embeds_value(&p->type)) continue;
                size_t child = resolve_named_symbol_id(analyzer, current->source_program,
                                                       named_type_token(current->source_program, &p->type));
                if (child == target_symbol || (child != AST_SYMBOL_NONE && aggregate_reaches(
                                                   analyzer, child, target_symbol, depth + 1)))
                    return 1;
            }
        return 0;
    }
    if (current->kind != SEMANTIC_SYMBOL_STRUCT || current->declaration == NULL) return 0;
    for (const AstField *field = current->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        if (!type_directly_embeds_value(&field->type))
            continue;
        size_t named = named_type_token(current->source_program, &field->type);
        size_t child = resolve_named_symbol_id(analyzer, current->source_program, named);
        if (child == target_symbol ||
            (child != AST_SYMBOL_NONE &&
             aggregate_reaches(analyzer, child, target_symbol, depth + 1U)))
            return 1;
    }
    return 0;
}

static size_t semantic_symbol_slots(const Analyzer *analyzer, size_t symbol_id,
                                    size_t depth);

static size_t declared_type_slots(const Analyzer *analyzer,
                                  const AstProgram *program,
                                  const AstType *type, size_t depth) {
    if (type == NULL || depth > analyzer->model->symbol_count + 64)
        return SIZE_MAX;
    if (type->kind == AST_TYPE_FUNCTION) return type->function_generic_parameters ? 0 : 1;
    if (type->kind == AST_TYPE_FUTURE || type->kind == AST_TYPE_JOIN || type->kind == AST_TYPE_EXECUTOR) return 1;
    if (type->outer_pointer_depth != 0) return 1;
    if (type->is_slice) return 2;
    if (type->is_array) {
        AstType element = ast_type_element(type);
        size_t slots = declared_type_slots(analyzer, program, &element,
                                           depth + 1);
        size_t length = type->resolved_array_length;
        if (length == 0 && type->array_length_token < program->token_count)
            length = (size_t) strtoull(
                ast_program_lexeme(program, type->array_length_token), NULL,
                10);
        if (slots == SIZE_MAX || (slots != 0 && length > SIZE_MAX / slots))
            return SIZE_MAX;
        return slots * length;
    }
    if (type->pointer_depth != 0 ||
        type->borrow_kind != AST_BORROW_NONE)
        return 1;
    size_t named = resolve_named_symbol_id(
        analyzer, program, named_type_token(program, type));
    return named == AST_SYMBOL_NONE
               ? 1
               : semantic_symbol_slots(analyzer, named, depth + 1);
}

static size_t semantic_symbol_slots(const Analyzer *analyzer, size_t symbol_id,
                                    size_t depth) {
    if (symbol_id >= analyzer->model->symbol_count ||
        depth > analyzer->model->symbol_count)
        return SIZE_MAX;
    const SemanticSymbol *symbol = &analyzer->model->symbols[symbol_id];
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE) return 2;
    if (symbol->kind == SEMANTIC_SYMBOL_ENUM && symbol->declaration) {
        size_t largest = 1;
        for (const AstEnumValue *v = symbol->declaration->as.enum_decl.values; v; v = v->next) {
            size_t payload = 1;
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                size_t slots = declared_type_slots(
                    analyzer, symbol->source_program, &p->type, depth + 1);
                if (slots > SIZE_MAX - payload) return SIZE_MAX;
                size_t alignment = layout_alignment_depth(analyzer, symbol->source_program, &p->type, depth + 1);
                size_t align_slots = alignment > 8 ? alignment / 8 : 1;
                if (payload > SIZE_MAX - (align_slots - 1)) return SIZE_MAX;
                payload = (payload + align_slots - 1) & ~(align_slots - 1);
                if (slots > SIZE_MAX - payload) return SIZE_MAX;
                payload += slots;
            }
            if (payload > largest) largest = payload;
        }
        AstType type = {.kind = AST_TYPE_NAMED, .name_token = symbol->name_token};
        size_t align_slots = layout_alignment_depth(analyzer, symbol->source_program, &type, depth + 1) / 8;
        if (largest > SIZE_MAX - (align_slots - 1)) return SIZE_MAX;
        return (largest + align_slots - 1) & ~(align_slots - 1);
    }
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL) return 1;
    if (symbol->declaration->is_native) {
        AstType type = {.kind = AST_TYPE_NAMED, .name_token = symbol->name_token};
        NativeTypeLayout layout;
        if (!semantic_native_layout(analyzer->model, symbol->source_program, &type, &layout) ||
            layout.size > SIZE_MAX - 7)
            return SIZE_MAX;
        return (layout.size + 7) / 8;
    }
    size_t slots = 0;
    for (const AstField *field = symbol->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        size_t field_slots = declared_type_slots(
            analyzer, symbol->source_program, &field->type, depth + 1);
        if (slots > SIZE_MAX - field_slots) return SIZE_MAX;
        size_t alignment = layout_alignment_depth(analyzer, symbol->source_program, &field->type, depth + 1);
        size_t align_slots = alignment > 8 ? alignment / 8 : 1;
        if (slots > SIZE_MAX - (align_slots - 1)) return SIZE_MAX;
        slots = (slots + align_slots - 1) & ~(align_slots - 1);
        if (field_slots > SIZE_MAX - slots) return SIZE_MAX;
        slots += field_slots;
    }
    AstType type = {.kind = AST_TYPE_NAMED, .name_token = symbol->name_token};
    size_t align_slots = layout_alignment_depth(analyzer, symbol->source_program, &type, depth + 1) / 8;
    if (slots > SIZE_MAX - (align_slots - 1)) return SIZE_MAX;
    return slots == 0 ? 1 : (slots + align_slots - 1) & ~(align_slots - 1);
}

size_t semantic_type_slots(const Analyzer *analyzer, const AstProgram *program,
                           const AstType *type,
                           const AstExpression *inferred) {
    if (type->kind == AST_TYPE_INFERRED && inferred != NULL &&
        inferred->has_resolved_ast_type)
        return declared_type_slots(
            analyzer,
            inferred->resolved_type_program != NULL
                ? inferred->resolved_type_program
                : program,
            &inferred->resolved_ast_type, 0);
    if (type->kind == AST_TYPE_INFERRED && inferred && inferred->resolved_is_slice && !inferred->
        resolved_outer_pointer_depth)
        return 2;
    if (type->outer_pointer_depth != 0 ||
        (type->pointer_depth != 0 && !type->is_array && !type->is_slice) || type->is_slice ||
        (type->kind == AST_TYPE_INFERRED && inferred != NULL &&
         (inferred->resolved_pointer_depth != 0 ||
          inferred->resolved_outer_pointer_depth != 0)))
        return type->is_slice && !type->outer_pointer_depth ? 2U : 1U;
    return declared_type_slots(analyzer, program, type, 0);
}

int assignable_expression(const Analyzer *analyzer,
                          const AstExpression *expression) {
    if (expression != NULL && expression->kind == AST_EXPR_MEMBER &&
        expression->left != NULL && expression->left->resolved_is_slice &&
        same_name(analyzer->program, expression->value_token, "length"))
        return 0;
    return expression != NULL &&
           !expression_is_constant_symbol(analyzer, expression) &&
           (expression->kind == AST_EXPR_NAME || expression->kind == AST_EXPR_INDEX ||
            expression->kind == AST_EXPR_MEMBER ||
            (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR));
}

void validate_array_shape(Analyzer *analyzer, AstType *type) {
    if (type != NULL && type->element_type != NULL)
        validate_array_shape(analyzer, type->element_type);
    if (type == NULL || !type->is_array) return;
    if (type->array_length_token >= analyzer->program->token_count) {
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be a compile-time integer");
        return;
    }
    const char *text = ast_program_lexeme(analyzer->program, type->array_length_token);
    if (analyzer->program->tokens[type->array_length_token].type == TOKEN_IDENTIFIER) {
        const LocalSymbol *local = find_local(analyzer, type->array_length_token);
        const SemanticSymbol *symbol = local != NULL
                                           ? &analyzer->model->symbols[local->symbol_id]
                                           : scoped_find_global(analyzer->model, analyzer->program, text,
                                                                SEMANTIC_SYMBOL_CONSTANT);
        if (symbol && symbol->kind == SEMANTIC_SYMBOL_CONSTANT && symbol->declaration && !symbol->declaration->
            semantic_body_checked) {
            size_t id = symbol->id;
            AstProgram *saved = analyzer->program;
            analyzer->program = (AstProgram *) symbol->source_program;
            analyze_constant_declaration(analyzer, (AstDeclarationNode *) symbol->declaration);
            analyzer->program = saved;
            symbol = &analyzer->model->symbols[id];
        }
        const AstExpression *value = symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_CONSTANT
                                         ? constant_initializer(symbol)
                                         : NULL;
        if (value == NULL || !data_type_integral(value->resolved_type) || value->folded_constant.lexeme == NULL) {
            semantic_error(analyzer, type->array_length_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Array length must be a visible evaluated integral constant");
            return;
        }
        text = value->folded_constant.lexeme;
    }
    unsigned long long length = strtoull(text, NULL, 10);
    if (length == 0 || length > 1048576ULL)
        semantic_error(analyzer, type->array_length_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Array length must be positive and fit local storage");
    else type->resolved_array_length = (size_t) length;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FIELD && symbol->source_program == analyzer->program &&
            &((const AstField *) symbol->node)->type == type)
            symbol->declared_type = *type;
    }
}

static void prepare_native_shapes(Analyzer *analyzer, size_t symbol_id, size_t depth) {
    if (symbol_id >= analyzer->model->symbol_count || depth > analyzer->model->symbol_count) return;
    const SemanticSymbol *symbol = &analyzer->model->symbols[symbol_id];
    const AstDeclarationNode *decl = symbol->declaration;
    if (!decl || symbol->kind != SEMANTIC_SYMBOL_STRUCT || !decl->is_native || decl->is_opaque) return;
    AstProgram *saved = analyzer->program;
    analyzer->program = (AstProgram *) symbol->source_program;
    for (AstField *field = decl->as.struct_decl.fields; field; field = field->next) {
        validate_array_shape(analyzer, &field->type);
        AstType element = field->type;
        while (element.is_array && !element.outer_pointer_depth) element = ast_type_element(&element);
        if (element.pointer_depth || element.outer_pointer_depth) continue;
        size_t child = resolve_named_symbol_id(analyzer, analyzer->program, element.name_token);
        prepare_native_shapes(analyzer, child, depth + 1);
    }
    analyzer->program = saved;
}

size_t layout_size(Analyzer *analyzer, AstType *type, size_t depth) {
    if (depth > analyzer->model->symbol_count + 64 || type->kind == AST_TYPE_INFERRED) return 0;
    if (type->kind == AST_TYPE_FUNCTION) return type->function_generic_parameters ? 0 : 8;
    if (type->kind == AST_TYPE_FUTURE || type->kind == AST_TYPE_JOIN || type->kind == AST_TYPE_EXECUTOR) return 8;
    if (type->outer_pointer_depth) return 8;
    if (type->is_slice) return 16;
    if (type->is_array) {
        validate_array_shape(analyzer, type);
        if (!type->resolved_array_length) return 0;
        AstType element = ast_type_element(type);
        size_t size = layout_size(analyzer, &element, depth + 1);
        if (!size || size > SIZE_MAX - 7) return 0;
        size = (size + 7) & ~(size_t) 7;
        return type->resolved_array_length > SIZE_MAX / size ? 0 : size * type->resolved_array_length;
    }
    if (type->pointer_depth) return 8;
    DataType primitive = primitive_type(analyzer->program, type);
    if (primitive != TYPE_UNKNOWN)
        return data_type_has_value(primitive) ? data_type_bytes(primitive) : 0;
    size_t id = resolve_named_symbol_id(analyzer, analyzer->program, type->name_token);
    if (id == AST_SYMBOL_NONE) return 0;
    const SemanticSymbol *symbol = &analyzer->model->symbols[id];
    AstDeclarationNode *declaration = (AstDeclarationNode *) symbol->declaration;
    if (!declaration) return 0;
    if (declaration->is_native) {
        prepare_native_shapes(analyzer, id, 0);
        NativeTypeLayout native;
        return semantic_native_layout(analyzer->model, analyzer->program, type, &native) ? native.size : 0;
    }
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE || symbol->kind == SEMANTIC_SYMBOL_STRUCT || symbol->kind ==
        SEMANTIC_SYMBOL_ENUM) {
        size_t slots = semantic_symbol_slots(analyzer, id, depth + 1);
        return slots == SIZE_MAX || slots > SIZE_MAX / 8 ? 0 : slots * 8;
    }
    return 0;
}
