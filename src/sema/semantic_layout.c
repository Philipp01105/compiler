#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                                                   analyzer, child, target_symbol, depth + 1))) return 1;
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
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE) {
        size_t largest = 1;
        for (size_t i = 0; i < analyzer->model->symbol_count; i++)
            if (semantic_implements_interface(analyzer->model, symbol_id, i)) {
                size_t slots = semantic_symbol_slots(analyzer, i, depth + 1);
                if (slots > largest) largest = slots;
            }
        return largest == SIZE_MAX ? SIZE_MAX : largest + 1;
    }
    if (symbol->kind == SEMANTIC_SYMBOL_ENUM && symbol->declaration) {
        size_t largest = 0;
        for (const AstEnumValue *v = symbol->declaration->as.enum_decl.values; v; v = v->next) {
            size_t payload = 0;
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next) {
                size_t slots = declared_type_slots(
                    analyzer, symbol->source_program, &p->type, depth + 1);
                if (slots > SIZE_MAX - payload) return SIZE_MAX;
                payload += slots;
            }
            if (payload > largest) largest = payload;
        }
        return largest == SIZE_MAX ? SIZE_MAX : largest + 1;
    }
    if (symbol->kind != SEMANTIC_SYMBOL_STRUCT || symbol->declaration == NULL) return 1;
    size_t slots = 0;
    for (const AstField *field = symbol->declaration->as.struct_decl.fields;
         field != NULL; field = field->next) {
        size_t field_slots = declared_type_slots(
            analyzer, symbol->source_program, &field->type, depth + 1);
        if (slots > SIZE_MAX - field_slots) return SIZE_MAX;
        slots += field_slots;
    }
    return slots == 0 ? 1 : slots;
}

size_t semantic_type_slots(const Analyzer *analyzer, const AstProgram *program,
                                   const AstType *type,
                                   const AstExpression *inferred) {
    if (type->kind == AST_TYPE_INFERRED && inferred != NULL &&
        inferred->has_resolved_ast_type)
        return declared_type_slots(
            analyzer,
            inferred->resolved_type_program != NULL
                ? inferred->resolved_type_program : program,
            &inferred->resolved_ast_type, 0);
    if (type->kind == AST_TYPE_INFERRED && inferred && inferred->resolved_is_slice && !inferred->
        resolved_outer_pointer_depth) return 2;
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

size_t layout_size(Analyzer *analyzer, AstType *type, size_t depth) {
    if (depth > analyzer->model->symbol_count + 64 || type->kind == AST_TYPE_INFERRED) return 0;
    if (type->kind == AST_TYPE_FUNCTION) return type->function_generic_parameters ? 0 : 8;
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
    if (primitive != TYPE_UNKNOWN) return primitive == TYPE_VOID ? 0 : data_type_bytes(primitive);
    size_t id = resolve_named_symbol_id(analyzer, analyzer->program, type->name_token);
    if (id == AST_SYMBOL_NONE) return 0;
    const SemanticSymbol *symbol = &analyzer->model->symbols[id];
    AstProgram *unit = (AstProgram *) symbol->source_program;
    AstDeclarationNode *declaration = (AstDeclarationNode *) symbol->declaration;
    if (!declaration) return 0;
    if (symbol->kind == SEMANTIC_SYMBOL_INTERFACE) {
        size_t slots = semantic_symbol_slots(analyzer, id, depth + 1);
        return slots == SIZE_MAX || slots > SIZE_MAX / 8 ? 0 : slots * 8;
    }
    AstProgram *saved = analyzer->program;
    analyzer->program = unit;
    size_t size = 0;
    int valid = 1;
    if (declaration->kind == AST_DECL_STRUCT) {
        for (AstField *field = declaration->as.struct_decl.fields; field; field = field->next) {
            size_t bytes = layout_size(analyzer, &field->type, depth + 1);
            if (!bytes || bytes > SIZE_MAX - 7) {
                valid = 0;
                break;
            }
            bytes = (bytes + 7) & ~(size_t) 7;
            if (bytes > SIZE_MAX - size) {
                valid = 0;
                break;
            }
            size += bytes;
        }
        if (!size) size = 8;
    } else if (declaration->kind == AST_DECL_ENUM) {
        size_t largest = 0;
        for (AstEnumValue *variant = declaration->as.enum_decl.values; variant; variant = variant->next) {
            size_t payload = 0;
            for (AstTypeArgument *argument = variant->payload_types; argument; argument = argument->next) {
                size_t bytes = layout_size(analyzer, &argument->type, depth + 1);
                if (!bytes || bytes > SIZE_MAX - 7) {
                    valid = 0;
                    break;
                }
                bytes = (bytes + 7) & ~(size_t) 7;
                if (bytes > SIZE_MAX - payload) {
                    valid = 0;
                    break;
                }
                payload += bytes;
            }
            if (payload > largest) largest = payload;
        }
        if (largest > SIZE_MAX - 8) valid = 0;
        else size = largest + 8;
    } else valid = 0;
    analyzer->program = saved;
    return valid ? size : 0;
}
