#include "semantic_internal.h"
#include "generics.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const AstGenericParameter *parameters;
    const AstTypeArgument *arguments;
    const AstProgram *argument_unit;
} InterfaceSubstitution;

static int symbol_has_type_name(const SemanticSymbol *symbol,
                                const char *name) {
    const char *symbol_name = ast_program_lexeme(symbol->source_program,
                                                  symbol->name_token);
    if (!strcmp(name, symbol_name)) return 1;

    char canonical[4096];
    snprintf(canonical, sizeof(canonical), "%s::%s",
             symbol->source_program->module_identity != NULL
                 ? symbol->source_program->module_identity
                 : "",
             symbol_name);
    return !strcmp(name, canonical);
}

static int interface_type_matches_depth(const SemanticModel *model,
                                     const AstProgram *interface_unit, const AstType *expected,
                                     const AstProgram *actual_unit, const AstType *actual,
                                     const AstProgram *self_unit, const AstType *self,
                                     const InterfaceSubstitution *substitution,
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
    if (substitution != NULL && expected->kind == AST_TYPE_NAMED &&
        expected->arguments == NULL) {
        const char *expected_name =
            ast_program_lexeme(interface_unit, expected->name_token);
        const AstGenericParameter *parameter = substitution->parameters;
        const AstTypeArgument *argument = substitution->arguments;
        for (; parameter != NULL && argument != NULL;
             parameter = parameter->next, argument = argument->next)
            if (!strcmp(expected_name,
                        ast_program_lexeme(interface_unit,
                                           parameter->name_token)))
                return ast_concrete_type_equal(substitution->argument_unit,
                                               &argument->type,
                                               actual_unit, actual);
    }
    const char *expected_name =
        ast_program_lexeme(interface_unit, expected->name_token);
    const char *actual_name =
        ast_program_lexeme(actual_unit, actual->name_token);
    const AstTypeArgument *actual_arguments = actual->arguments;
    const AstProgram *actual_argument_unit = actual_unit;
    if (strcmp(expected_name, actual_name)) {
        const SemanticSymbol *actual_symbol = NULL;
        for (size_t i = 0; i < model->symbol_count; i++)
            if ((model->symbols[i].kind == SEMANTIC_SYMBOL_STRUCT ||
                 model->symbols[i].kind == SEMANTIC_SYMBOL_ENUM) &&
                symbol_has_type_name(&model->symbols[i], actual_name)) {
                actual_symbol = &model->symbols[i];
                break;
            }
        const AstDeclarationNode *origin =
            actual_symbol != NULL && actual_symbol->declaration != NULL
                ? actual_symbol->declaration->generic_origin : NULL;
        if (origin == NULL ||
            strcmp(expected_name, ast_program_lexeme(
                                      actual_symbol->source_program,
                                      origin->name_token)))
            return 0;
        actual_arguments =
            actual_symbol->declaration->specialization_arguments;
        actual_argument_unit = actual_symbol->source_program;
    }
    const AstTypeArgument *x = expected->arguments, *y = actual_arguments;
    for (; x && y; x = x->next, y = y->next)
        if (!interface_type_matches_depth(model, interface_unit, &x->type,
                                       actual_argument_unit, &y->type,
                                       self_unit, self, substitution,
                                       depth + 1)) return 0;
    return !x && !y;
}

static int interface_type_matches(const SemanticModel *model,
                               const AstProgram *interface_unit, AstType expected,
                               const AstProgram *actual_unit, const AstType *actual,
                               const AstProgram *self_unit, const AstType *self,
                               const InterfaceSubstitution *substitution) {
    return interface_type_matches_depth(model, interface_unit, &expected, actual_unit, actual,
                                     self_unit, self, substitution, 0);
}

static size_t interface_method(const SemanticModel *model,
                               size_t interface_method_id, size_t struct_id,
                               const InterfaceSubstitution *substitution);

int semantic_implements_interface(const SemanticModel *model, size_t interface_id,
                                  size_t struct_id) {
    if (!model || interface_id >= model->symbol_count || struct_id >= model->symbol_count) return 0;
    const SemanticSymbol *interface = &model->symbols[interface_id];
    const SemanticSymbol *owner = &model->symbols[struct_id];
    if (interface->kind != SEMANTIC_SYMBOL_INTERFACE ||
        (owner->kind != SEMANTIC_SYMBOL_STRUCT && owner->kind != SEMANTIC_SYMBOL_ENUM) ||
        !interface->declaration) return 0;
    for (const AstDeclarationNode *required = interface->declaration->as.interface_decl.methods;
         required; required = required->next)
        if (interface_method(model, required->resolved_symbol_id, struct_id,
                             NULL) == AST_SYMBOL_NONE) return 0;
    return 1;
}

int semantic_implements_specialized_interface(
    const SemanticModel *model, size_t interface_id, size_t owner_id,
    const AstProgram *argument_unit, const AstType *interface_type) {
    if (!model || interface_id >= model->symbol_count ||
        owner_id >= model->symbol_count || interface_type == NULL)
        return 0;
    const SemanticSymbol *interface = &model->symbols[interface_id];
    if (interface->kind != SEMANTIC_SYMBOL_INTERFACE ||
        interface->declaration == NULL)
        return 0;
    InterfaceSubstitution substitution = {
        .parameters = interface->declaration->generic_parameters,
        .arguments = interface_type->arguments,
        .argument_unit = argument_unit
    };
    for (const AstDeclarationNode *required =
             interface->declaration->as.interface_decl.methods;
         required != NULL; required = required->next)
        if (interface_method(model, required->resolved_symbol_id, owner_id,
                             &substitution) == AST_SYMBOL_NONE)
            return 0;
    return 1;
}

static size_t interface_method(const SemanticModel *model,
                               size_t interface_method_id, size_t struct_id,
                               const InterfaceSubstitution *substitution) {
    if (!model || interface_method_id >= model->symbol_count || struct_id >= model->symbol_count)
        return AST_SYMBOL_NONE;
    const SemanticSymbol *required = &model->symbols[interface_method_id];
    const SemanticSymbol *owner = &model->symbols[struct_id];
    if (required->kind != SEMANTIC_SYMBOL_FUNCTION || !required->declaration ||
        required->owner_symbol_id >= model->symbol_count ||
        model->symbols[required->owner_symbol_id].kind != SEMANTIC_SYMBOL_INTERFACE ||
        (owner->kind != SEMANTIC_SYMBOL_STRUCT && owner->kind != SEMANTIC_SYMBOL_ENUM) ||
        !owner->declaration) return AST_SYMBOL_NONE;
    AstType self = {.kind = AST_TYPE_NAMED, .name_token = owner->name_token,
                    .array_length_token = AST_TOKEN_NONE};
    const char *required_name = ast_program_lexeme(required->source_program, required->name_token);
    size_t required_parameters = parameter_count(required->declaration);
    const AstDeclarationNode *methods = owner->kind == SEMANTIC_SYMBOL_STRUCT
                                            ? owner->declaration->as.struct_decl.methods
                                            : owner->declaration->as.enum_decl.methods;
    for (const AstDeclarationNode *method = methods;
         method; method = method->next) {
        if (method->resolved_symbol_id >= model->symbol_count) continue;
        const SemanticSymbol *actual = &model->symbols[method->resolved_symbol_id];
        if (actual->kind != SEMANTIC_SYMBOL_FUNCTION || actual->owner_symbol_id != struct_id ||
            !actual->declaration ||
            actual->declaration->as.function.is_static !=
                required->declaration->as.function.is_static ||
            !same_name(actual->source_program, actual->name_token, required_name) ||
            parameter_count(actual->declaration) != required_parameters) continue;
        if (!interface_type_matches(model, required->source_program, required->declaration->as.function.return_type,
                                 actual->source_program, &actual->declaration->as.function.return_type,
                                 owner->source_program, &self,
                                 substitution)) continue;
        const AstParameter *expected = required->declaration->as.function.parameters;
        const AstParameter *provided = actual->declaration->as.function.parameters;
        for (; expected && provided; expected = expected->next, provided = provided->next)
            if (!interface_type_matches(model, required->source_program, expected->type, actual->source_program,
                                     &provided->type, owner->source_program, &self,
                                     substitution)) break;
        if (!expected && !provided) return actual->id;
    }
    return AST_SYMBOL_NONE;
}

size_t semantic_interface_method(const SemanticModel *model,
                                 size_t interface_method_id,
                                 size_t struct_id) {
    return interface_method(model, interface_method_id, struct_id, NULL);
}
