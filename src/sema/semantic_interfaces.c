#include "semantic_internal.h"

#include <stdio.h>
#include <string.h>

static size_t interface_parameter_count(const AstDeclarationNode *function) {
    size_t count = 0;
    for (const AstParameter *parameter = function->as.function.parameters;
         parameter; parameter = parameter->next) count++;
    return count;
}

static int interface_type_matches_depth(const AstProgram *interface_unit, const AstType *expected,
                                    const AstProgram *actual_unit, const AstType *actual,
                                    const AstProgram *self_unit, const AstType *self,
                                    unsigned depth) {
    if (depth > 64 || expected->kind != actual->kind ||
        expected->pointer_depth != actual->pointer_depth ||
        expected->outer_pointer_depth != actual->outer_pointer_depth ||
        expected->is_array != actual->is_array || expected->is_slice != actual->is_slice ||
        expected->resolved_array_length != actual->resolved_array_length) return 0;
    if (!strcmp(ast_program_lexeme(interface_unit, expected->name_token), "Self")) {
        const char *owner_name = ast_program_lexeme(self_unit, self->name_token);
        const char *actual_name = ast_program_lexeme(actual_unit, actual->name_token);
        char canonical[4096];
        snprintf(canonical, sizeof(canonical), "%s::%s",
                 self_unit->module_identity ? self_unit->module_identity : "", owner_name);
        return !expected->arguments && !actual->arguments &&
               (strcmp(actual_name, owner_name) == 0 || strcmp(actual_name, canonical) == 0);
    }
    if (strcmp(ast_program_lexeme(interface_unit, expected->name_token),
               ast_program_lexeme(actual_unit, actual->name_token))) return 0;
    const AstTypeArgument *x = expected->arguments, *y = actual->arguments;
    for (; x && y; x = x->next, y = y->next)
        if (!interface_type_matches_depth(interface_unit, &x->type, actual_unit, &y->type,
                                      self_unit, self, depth + 1)) return 0;
    return !x && !y;
}

static int interface_type_matches(const AstProgram *interface_unit, AstType expected,
                              const AstProgram *actual_unit, const AstType *actual,
                              const AstProgram *self_unit, const AstType *self) {
    return interface_type_matches_depth(interface_unit, &expected, actual_unit, actual,
                                    self_unit, self, 0);
}

int semantic_implements_interface(const SemanticModel *model, size_t interface_id,
                                  size_t struct_id) {
    if (!model || interface_id >= model->symbol_count || struct_id >= model->symbol_count) return 0;
    const SemanticSymbol *interface = &model->symbols[interface_id];
    const SemanticSymbol *owner = &model->symbols[struct_id];
    if (interface->kind != SEMANTIC_SYMBOL_INTERFACE || owner->kind != SEMANTIC_SYMBOL_STRUCT ||
        !interface->declaration) return 0;
    for (const AstDeclarationNode *required = interface->declaration->as.interface_decl.methods;
         required; required = required->next)
        if (semantic_interface_method(model, required->resolved_symbol_id,
                                      struct_id) == AST_SYMBOL_NONE) return 0;
    return 1;
}

size_t semantic_interface_method(const SemanticModel *model, size_t interface_method_id,
                                 size_t struct_id) {
    if (!model || interface_method_id >= model->symbol_count || struct_id >= model->symbol_count)
        return AST_SYMBOL_NONE;
    const SemanticSymbol *required = &model->symbols[interface_method_id];
    const SemanticSymbol *owner = &model->symbols[struct_id];
    if (required->kind != SEMANTIC_SYMBOL_FUNCTION || !required->declaration ||
        required->owner_symbol_id >= model->symbol_count ||
        model->symbols[required->owner_symbol_id].kind != SEMANTIC_SYMBOL_INTERFACE ||
        owner->kind != SEMANTIC_SYMBOL_STRUCT) return AST_SYMBOL_NONE;
    AstType self = {.kind = AST_TYPE_NAMED, .name_token = owner->name_token,
                    .array_length_token = AST_TOKEN_NONE};
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *actual = &model->symbols[i];
        if (actual->kind != SEMANTIC_SYMBOL_FUNCTION || actual->owner_symbol_id != struct_id ||
            !actual->declaration || actual->declaration->as.function.is_static ||
            !same_name(actual->source_program, actual->name_token,
                       ast_program_lexeme(required->source_program, required->name_token)) ||
            interface_parameter_count(actual->declaration) != interface_parameter_count(required->declaration)) continue;
        if (!interface_type_matches(required->source_program, required->declaration->as.function.return_type,
                                actual->source_program, &actual->declaration->as.function.return_type,
                                owner->source_program, &self)) continue;
        const AstParameter *expected = required->declaration->as.function.parameters;
        const AstParameter *provided = actual->declaration->as.function.parameters;
        for (; expected && provided; expected = expected->next, provided = provided->next)
            if (!interface_type_matches(required->source_program, expected->type, actual->source_program,
                                    &provided->type, owner->source_program, &self)) break;
        if (!expected && !provided) return i;
    }
    return AST_SYMBOL_NONE;
}

