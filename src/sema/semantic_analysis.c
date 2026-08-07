#include "semantic_internal.h"
#include "generics.h"
#include "core_intrinsics.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "semantic_async.inc"

static int invalid_never_type(const AstProgram *program, const AstType *type,
                              int allow_direct_return) {
    if (type == NULL) return 0;
    if (primitive_type(program, type) == TYPE_NEVER)
        return !allow_direct_return || type->pointer_depth != 0 ||
               type->outer_pointer_depth != 0 ||
               type->borrow_kind != AST_BORROW_NONE || type->is_array ||
               type->is_slice;
    if (type->kind == AST_TYPE_FUNCTION) {
        for (const AstTypeArgument *parameter = type->function_parameters;
             parameter != NULL; parameter = parameter->next)
            if (invalid_never_type(program, &parameter->type, 0)) return 1;
        if (invalid_never_type(program, type->function_return_type, 1)) return 1;
    }
    for (const AstTypeArgument *argument = type->arguments;
         argument != NULL; argument = argument->next)
        if (invalid_never_type(program, &argument->type, 0)) return 1;
    return 0;
}

static int returned_slice_expression_owns(const SemanticModel *model,
                                          const AstExpression *expression) {
    if (expression == NULL || !expression->resolved_is_slice) return 0;
    if (expression->owns_slice_backing) return 1;
    if (expression->kind != AST_EXPR_CALL ||
        expression->resolved_symbol_id >= model->symbol_count) return 0;
    const SemanticSymbol *callee = &model->symbols[expression->resolved_symbol_id];
    return callee->kind == SEMANTIC_SYMBOL_FUNCTION &&
           callee->declaration != NULL &&
           callee->declaration->as.function.returns_owned_slice_backing;
}

static int statement_returns_owned_slice(const SemanticModel *model,
                                         const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_RETURN &&
            returned_slice_expression_owns(model, statement->value)) return 1;
        if (statement->result != NULL &&
            returned_slice_expression_owns(model, statement->result)) return 1;
        if (statement_returns_owned_slice(model, statement->body) ||
            statement_returns_owned_slice(model, statement->else_body) ||
            statement_returns_owned_slice(model, statement->initializer)) return 1;
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            if (statement_returns_owned_slice(model, arm->body)) return 1;
    }
    return 0;
}

static void refresh_owned_slice_statements(const SemanticModel *model,
                                           AstStatement *statement);

static void refresh_owned_slice_expression(const SemanticModel *model,
                                           AstExpression *expression) {
    for (; expression != NULL; expression = expression->next) {
        refresh_owned_slice_expression(model, expression->left);
        refresh_owned_slice_expression(model, expression->right);
        refresh_owned_slice_expression(model, expression->arguments);
        refresh_owned_slice_statements(model, expression->control);
        if (expression->kind == AST_EXPR_CALL &&
            returned_slice_expression_owns(model, expression))
            expression->owns_slice_backing = 1;
    }
}

static void refresh_owned_slice_statements(const SemanticModel *model,
                                           AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        refresh_owned_slice_expression(model, statement->expression);
        refresh_owned_slice_expression(model, statement->value);
        refresh_owned_slice_expression(model, statement->condition);
        refresh_owned_slice_expression(model, statement->update);
        refresh_owned_slice_expression(model, statement->result);
        refresh_owned_slice_statements(model, statement->body);
        refresh_owned_slice_statements(model, statement->else_body);
        refresh_owned_slice_statements(model, statement->initializer);
        for (AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            refresh_owned_slice_statements(model, arm->body);
    }
}

static void finalize_owned_slice_returns(SemanticModel *model) {
    int changed;
    do {
        changed = 0;
        for (size_t i = 0; i < model->symbol_count; i++) {
            SemanticSymbol *symbol = &model->symbols[i];
            AstDeclarationNode *function = symbol->kind == SEMANTIC_SYMBOL_FUNCTION
                                               ? (AstDeclarationNode *) symbol->declaration
                                               : NULL;
            if (function == NULL ||
                function->as.function.returns_owned_slice_backing) continue;
            if (statement_returns_owned_slice(model,
                                              function->as.function.body)) {
                function->as.function.returns_owned_slice_backing = 1;
                changed = 1;
            }
        }
    } while (changed);
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION &&
            symbol->declaration != NULL)
            refresh_owned_slice_statements(
                model, symbol->declaration->as.function.body);
        else if (symbol->kind == SEMANTIC_SYMBOL_VARIABLE &&
                 symbol->declaration != NULL)
            refresh_owned_slice_expression(
                model, symbol->declaration->as.constant.value);
    }
}

int semantic_type_is_move_only(const Analyzer *analyzer,
                               size_t type_symbol_id) {
    return (semantic_symbol_type_properties(analyzer->model, type_symbol_id) &
            SEMANTIC_TYPE_MOVE_ONLY) != 0;
}

int semantic_type_needs_drop(const Analyzer *analyzer,
                             size_t type_symbol_id) {
    return (semantic_symbol_type_properties(analyzer->model, type_symbol_id) &
            SEMANTIC_TYPE_NEEDS_DROP) != 0;
}

int semantic_async_enabled(const Analyzer *analyzer) {
    const DmmModule *module = analyzer->model->program->module;
    if (module != NULL && module->graph != NULL) module = module->graph;
    for (const DmmFeature *feature = module == NULL ? NULL : module->features;
         feature != NULL; feature = feature->next)
        if (strcmp(feature->name, "async") == 0) return 1;
    return 0;
}

int semantic_expression_is_future(const AstExpression *expression) {
    return expression != NULL && expression->has_resolved_ast_type &&
           expression->resolved_ast_type.kind == AST_TYPE_FUTURE;
}

static unsigned derived_declared_type_properties(const Analyzer *analyzer,
                                       const AstProgram *program,
                                       const AstType *type) {
    if (type->pointer_depth != 0 || type->outer_pointer_depth != 0 || type->is_slice)
        return SEMANTIC_TYPE_COPYABLE;
    if (type->borrow_kind != AST_BORROW_NONE) {
        AstType referent = *type;
        referent.borrow_kind = AST_BORROW_NONE;
        unsigned nested = semantic_declared_type_properties(analyzer, program, &referent);
        if (type->borrow_kind == AST_BORROW_IMMUTABLE)
            return SEMANTIC_TYPE_COPYABLE |
                   ((nested & SEMANTIC_TYPE_SYNC)
                        ? SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC : 0);
        return SEMANTIC_TYPE_COPYABLE |
               ((nested & SEMANTIC_TYPE_SEND) ? SEMANTIC_TYPE_SEND : 0);
    }
    if (type->is_array) {
        AstType element = ast_type_element(type);
        return semantic_declared_type_properties(analyzer, program, &element);
    }
    /* Send for a Future depends on its complete frame, not only its output. */
    if (type->kind == AST_TYPE_JOIN || type->kind == AST_TYPE_EXECUTOR)
        return SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP |
               SEMANTIC_TYPE_MUST_CONSUME | SEMANTIC_TYPE_SEND;
    if (type->kind == AST_TYPE_FUTURE)
        return SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP |
               SEMANTIC_TYPE_MUST_CONSUME;
    if (type->kind == AST_TYPE_FUNCTION)
        return SEMANTIC_TYPE_COPYABLE;
    if (primitive_type(program, type) != TYPE_UNKNOWN)
        return SEMANTIC_TYPE_COPYABLE | SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
    size_t nested = resolve_named_symbol_id(
        analyzer, program, named_type_token(program, type));
    return semantic_symbol_type_properties(analyzer->model, nested);
}

unsigned semantic_declared_type_properties(const Analyzer *analyzer,
                                           const AstProgram *program,
                                           const AstType *type) {
    unsigned properties = derived_declared_type_properties(analyzer, program, type);
    if (type->kind == AST_TYPE_FUTURE && !type->is_array && !type->is_slice &&
        !type->pointer_depth && !type->outer_pointer_depth && !type->borrow_kind) return properties;
    size_t send = semantic_auto_role(analyzer->model, SEMANTIC_TYPE_SEND);
    size_t sync = semantic_auto_role(analyzer->model, SEMANTIC_TYPE_SYNC);
    if (send != AST_SYMBOL_NONE) {
        properties &= ~(unsigned)SEMANTIC_TYPE_SEND;
        if (semantic_satisfies(analyzer->model, program, type, send)) properties |= SEMANTIC_TYPE_SEND;
    }
    if (sync != AST_SYMBOL_NONE) {
        properties &= ~(unsigned)SEMANTIC_TYPE_SYNC;
        if (semantic_satisfies(analyzer->model, program, type, sync)) properties |= SEMANTIC_TYPE_SYNC;
    }
    return properties;
}

void derive_type_properties(Analyzer *analyzer) {
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_STRUCT &&
            symbol->kind != SEMANTIC_SYMBOL_ENUM)
            continue;
        symbol->type_properties = SEMANTIC_TYPE_COPYABLE |
                                  SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
        if (symbol->kind == SEMANTIC_SYMBOL_STRUCT &&
            symbol->declaration != NULL &&
            symbol->declaration->as.struct_decl.destructor != NULL)
            symbol->type_properties =
                SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP |
                SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
    }

    int changed;
    do {
        changed = 0;
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            SemanticSymbol *symbol = &analyzer->model->symbols[i];
            if ((symbol->kind != SEMANTIC_SYMBOL_STRUCT &&
                 symbol->kind != SEMANTIC_SYMBOL_ENUM) ||
                symbol->declaration == NULL)
                continue;
            unsigned derived = symbol->type_properties;
            const AstField *field = symbol->kind == SEMANTIC_SYMBOL_STRUCT
                                        ? symbol->declaration->as.struct_decl.fields
                                        : symbol->declaration->as.enum_decl.fields;
            for (; field != NULL; field = field->next) {
                unsigned nested = semantic_declared_type_properties(
                    analyzer, symbol->source_program, &field->type);
                if (nested & SEMANTIC_TYPE_MOVE_ONLY)
                    derived = (derived & ~(unsigned) SEMANTIC_TYPE_COPYABLE) |
                              SEMANTIC_TYPE_MOVE_ONLY;
                if (nested & SEMANTIC_TYPE_NEEDS_DROP)
                    derived |= SEMANTIC_TYPE_NEEDS_DROP;
                if (nested & SEMANTIC_TYPE_MUST_CONSUME)
                    derived |= SEMANTIC_TYPE_MUST_CONSUME;
                if (!(nested & SEMANTIC_TYPE_SEND)) derived &= ~(unsigned) SEMANTIC_TYPE_SEND;
                if (!(nested & SEMANTIC_TYPE_SYNC)) derived &= ~(unsigned) SEMANTIC_TYPE_SYNC;
            }
            if (symbol->kind == SEMANTIC_SYMBOL_ENUM) {
                for (const AstEnumValue *value =
                         symbol->declaration->as.enum_decl.values;
                     value != NULL; value = value->next) {
                    for (const AstTypeArgument *payload = value->payload_types;
                         payload != NULL; payload = payload->next) {
                        unsigned nested = semantic_declared_type_properties(
                            analyzer, symbol->source_program, &payload->type);
                        if (nested & SEMANTIC_TYPE_MOVE_ONLY)
                            derived =
                                (derived & ~(unsigned) SEMANTIC_TYPE_COPYABLE) |
                                SEMANTIC_TYPE_MOVE_ONLY;
                        if (nested & SEMANTIC_TYPE_NEEDS_DROP)
                            derived |= SEMANTIC_TYPE_NEEDS_DROP;
                        if (nested & SEMANTIC_TYPE_MUST_CONSUME)
                            derived |= SEMANTIC_TYPE_MUST_CONSUME;
                        if (!(nested & SEMANTIC_TYPE_SEND)) derived &= ~(unsigned) SEMANTIC_TYPE_SEND;
                        if (!(nested & SEMANTIC_TYPE_SYNC)) derived &= ~(unsigned) SEMANTIC_TYPE_SYNC;
                    }
                }
            }
            if (derived != symbol->type_properties) {
                symbol->type_properties = derived;
                changed = 1;
            }
        }
    } while (changed);
    /* Language roles bind only to the canonical core declarations. */
    size_t send = semantic_auto_role(analyzer->model, SEMANTIC_TYPE_SEND);
    size_t sync = semantic_auto_role(analyzer->model, SEMANTIC_TYPE_SYNC);
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_STRUCT && symbol->kind != SEMANTIC_SYMBOL_ENUM) continue;
        AstType type = {.kind = AST_TYPE_NAMED, .name_token = symbol->name_token};
        if (send != AST_SYMBOL_NONE) {
            symbol->type_properties &= ~(unsigned)SEMANTIC_TYPE_SEND;
            if (semantic_satisfies(analyzer->model, symbol->source_program, &type, send))
                symbol->type_properties |= SEMANTIC_TYPE_SEND;
        }
        if (sync != AST_SYMBOL_NONE) {
            symbol->type_properties &= ~(unsigned)SEMANTIC_TYPE_SYNC;
            if (semantic_satisfies(analyzer->model, symbol->source_program, &type, sync))
                symbol->type_properties |= SEMANTIC_TYPE_SYNC;
        }
    }
}

int semantic_expression_is_move_only(const Analyzer *analyzer,
                                     const AstExpression *expression) {
    if (expression == NULL || expression->resolved_borrow_kind != AST_BORROW_NONE ||
        expression->resolved_pointer_depth != 0 ||
        expression->resolved_outer_pointer_depth != 0 ||
        expression->resolved_is_slice)
        return 0;
    if (semantic_expression_is_future(expression)) return 1;
    if (expression->has_resolved_ast_type && (expression->resolved_ast_type.kind == AST_TYPE_JOIN ||
        expression->resolved_ast_type.kind == AST_TYPE_EXECUTOR)) return 1;
    if (expression->resolved_named_symbol_id < analyzer->model->symbol_count)
        return semantic_type_is_move_only(analyzer,
                                          expression->resolved_named_symbol_id);
    return expression->resolved_symbol_id < analyzer->model->symbol_count &&
           (analyzer->model->symbols[expression->resolved_symbol_id].
                type_properties & SEMANTIC_TYPE_MOVE_ONLY) != 0;
}

static void consume_owned_expression(Analyzer *analyzer,
                                     const AstExpression *expression) {
    if (!semantic_expression_is_move_only(analyzer, expression)) return;
    if (analyzer->in_destructor &&
        expression->resolved_symbol_id < analyzer->model->symbol_count &&
        analyzer->model->symbols[expression->resolved_symbol_id].kind ==
            SEMANTIC_SYMBOL_FIELD) {
        semantic_error(analyzer, expression->first_token,
                       ERROR_CATEGORY_SEMANTIC,
                       ERR_SEM_INVALID_DECLARATION,
                       "A destructor cannot move ownership out of self");
        return;
    }
    if (expression->kind == AST_EXPR_NAME) {
        LocalSymbol *local =
            find_local_by_symbol(analyzer, expression->resolved_symbol_id);
        if (local == NULL && expression->resolved_symbol_id <
                       analyzer->model->symbol_count &&
                   analyzer->model->symbols[
                       expression->resolved_symbol_id].kind ==
                       SEMANTIC_SYMBOL_VARIABLE) {
            semantic_error(analyzer, expression->value_token,
                           ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_INVALID_DECLARATION,
                           "Cannot move ownership out of package storage");
        }
    } else if (expression->kind == AST_EXPR_MEMBER) {
        semantic_error(analyzer, expression->value_token,
                       ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                       "Partial moves from move-only structs are not supported");
    }
}

static void consume_call_arguments(Analyzer *analyzer,
                                   const AstExpression *expression) {
    if (expression == NULL) return;
    consume_call_arguments(analyzer, expression->left);
    consume_call_arguments(analyzer, expression->right);
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next)
        consume_call_arguments(analyzer, argument);
    if (expression->kind != AST_EXPR_CALL ||
        expression->resolved_symbol_id >= analyzer->model->symbol_count)
        return;
    const SemanticSymbol *function =
        &analyzer->model->symbols[expression->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION ||
        function->declaration == NULL)
        return;
    const AstExpression *argument = expression->arguments;
    const AstParameter *parameter =
        function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
         argument = argument->next, parameter = parameter->next)
        if (parameter->type.borrow_kind == AST_BORROW_NONE &&
            !(parameter->type.is_slice && argument->resolved_is_array))
            consume_owned_expression(analyzer, argument);
}

static int safe_borrow_return_origin(const Analyzer *analyzer,
                                     const AstExpression *expression) {
    if (expression == NULL) return 0;
    while (expression->kind == AST_EXPR_UNARY &&
           expression->operator_type == TOKEN_AMPERSAND)
        expression = expression->right;
    while ((expression->kind == AST_EXPR_MEMBER ||
            expression->kind == AST_EXPR_INDEX) &&
           expression->left != NULL)
        expression = expression->left;
    if (expression->kind != AST_EXPR_NAME ||
        expression->resolved_symbol_id >= analyzer->model->symbol_count)
        return 0;
    const SemanticSymbol *origin =
        &analyzer->model->symbols[expression->resolved_symbol_id];
    if (origin->kind == SEMANTIC_SYMBOL_FIELD &&
        origin->owner_symbol_id < analyzer->model->symbol_count &&
        analyzer->model->symbols[origin->owner_symbol_id].name_token == analyzer->current_owner_token &&
        semantic_type_is_move_only(analyzer, origin->owner_symbol_id)) return 1;
    if (origin->scope_depth == 0 &&
        (origin->kind == SEMANTIC_SYMBOL_VARIABLE ||
         origin->kind == SEMANTIC_SYMBOL_CONSTANT))
        return 1;
    return origin->kind == SEMANTIC_SYMBOL_PARAMETER &&
           origin->declared_type.borrow_kind != AST_BORROW_NONE;
}

static void analyze_statement(Analyzer *analyzer, AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_MATCH) {
            analyze_expression(analyzer, statement->value);
            if (statement->value && statement->value->kind == AST_EXPR_TYPE_INFO) {
                validate_expression(analyzer, statement->value, 1);
                statement->is_type_match = 1;
                statement->selected_type_arm = NULL;
                int wildcard = 0;
                for (AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                    if (wildcard) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                 ERR_SEM_INVALID_DECLARATION,
                                                 "Unreachable type match arm after wildcard");
                    if (arm->bindings) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                      ERR_SEM_INVALID_DECLARATION,
                                                      "Type match cannot bind enum payloads");
                    if (arm->wildcard) {
                        wildcard = 1;
                        if (!statement->selected_type_arm) statement->selected_type_arm = arm;
                        continue;
                    }
                    if (!arm->is_type_pattern) {
                        semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_PARSER, ERR_PARSE_INVALID_SYNTAX,
                                       "Type match requires case Type -> statement");
                        continue;
                    }
                    normalize_generic_type(analyzer, &arm->type, 0);
                    if (!known_declared_type(analyzer, &arm->type))
                        semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown type match pattern");
                    for (AstMatchArm *previous = statement->match_arms; previous != arm; previous = previous->next)
                        if (previous->is_type_pattern && !previous->wildcard && ast_concrete_type_equal(
                                analyzer->program, &previous->type, analyzer->program, &arm->type))
                            semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                           ERR_SEM_DUPLICATE_DEFINITION, "Duplicate type match case");
                    if (!statement->selected_type_arm && ast_concrete_type_equal(
                            analyzer->program, &statement->value->allocated_type, analyzer->program,
                            &arm->type)) statement->selected_type_arm = arm;
                }
                statement->match_exhaustive = statement->selected_type_arm != NULL;
                if (!statement->match_exhaustive)
                    semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_SEMANTIC,
                                   ERR_SEM_INVALID_DECLARATION, "Type match has no matching case; add a wildcard");
                else {
                    LocalSymbol *saved = analyzer->locals;
                    analyzer->scope_depth++;
                    normalize_statement_types(analyzer, statement->selected_type_arm->body);
                    analyze_statement(analyzer, statement->selected_type_arm->body);
                    analyzer->scope_depth--;
                    pop_to(analyzer, saved);
                }
                continue;
            }
            validate_expression(analyzer, statement->value, 0);
            size_t nominal = statement->value ? statement->value->resolved_named_symbol_id : AST_SYMBOL_NONE;
            const SemanticSymbol *enum_symbol = nominal < analyzer->model->symbol_count
                                                    ? &analyzer->model->symbols[nominal]
                                                    : NULL;
            if (!enum_symbol || enum_symbol->kind != SEMANTIC_SYMBOL_ENUM || pointer_expression(statement->value)) {
                semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "match requires an enum value");
                continue;
            }
            const AstDeclarationNode *enumeration = enum_symbol->declaration;
            const AstProgram *enum_unit = enum_symbol->source_program;
            const AstDeclarationNode *origin=enumeration->generic_origin;
            int standard_result=origin && enum_unit->module_identity && !strcmp(enum_unit->module_identity,"stdlib") &&
                (!strcmp(ast_program_lexeme(enum_unit,origin->name_token),"Result") ||
                 !strcmp(ast_program_lexeme(enum_unit,origin->name_token),"Propagation"));
            statement->is_consuming_match=(enumeration->is_async_builtin||standard_result) && enumeration->as.enum_decl.is_sum &&
                semantic_expression_is_move_only(analyzer,statement->value);
            size_t variants = 0, covered = 0;
            int wildcard = 0;
            for (const AstEnumValue *v = enumeration->as.enum_decl.values; v; v = v->next) variants++;
            for (AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                if (arm->is_type_pattern) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_TYPE,
                                                         ERR_TYPE_INVALID_OPERATION,
                                                         "Type cases require a type metadata match, not an enum value");
                if (wildcard || covered == variants)
                    semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                                   "Unreachable match arm");
                LocalSymbol *saved = analyzer->locals;
                analyzer->scope_depth++;
                if (arm->wildcard) {
                    if(statement->is_consuming_match)
                        semantic_error(analyzer,arm->variant_token,ERROR_CATEGORY_SEMANTIC,ERR_SEM_INVALID_DECLARATION,
                            "Consuming enum matches must bind every variant payload");
                    wildcard = 1;
                    if (arm->bindings) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_PARSER,
                                                      ERR_PARSE_INVALID_SYNTAX,
                                                      "Wildcard pattern cannot bind payloads");
                } else {
                    const AstEnumValue *v = find_enum_value_by_symbol(analyzer, nominal, arm->variant_token);
                    if (!v) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                           ERR_SEM_FIELD_NOT_FOUND, "Unknown enum match variant");
                    else {
                        if (!same_package(analyzer->program, enum_unit) && !v->is_public)
                            semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                                           "Enum variant is private to its defining package");
                        arm->resolved_variant_symbol = v->resolved_symbol_id;
                        int duplicate = 0;
                        for (AstMatchArm *previous = statement->match_arms; previous != arm; previous = previous->next)
                            if (previous->resolved_variant_symbol == arm->resolved_variant_symbol) duplicate = 1;
                        if (duplicate) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                      ERR_SEM_DUPLICATE_DEFINITION, "Duplicate match variant");
                        else covered++;
                        const AstTypeArgument *p = v->payload_types;
                        AstParameter *binding = arm->bindings;
                        for (; p && binding; p = p->next, binding = binding->next) {
                            binding->type = argument_type_copy(analyzer, enum_unit, p->type);
                            validate_array_shape(analyzer, &binding->type);
                            if (!statement->is_consuming_match && (semantic_declared_type_properties(analyzer, analyzer->program,
                                                       &binding->type) &
                                 SEMANTIC_TYPE_MOVE_ONLY) != 0)
                                semantic_error(analyzer, binding->name_token,
                                               ERROR_CATEGORY_SEMANTIC,
                                               ERR_SEM_INVALID_DECLARATION,
                                               "Move-only enum payload bindings require consuming pattern support");
                            for (const LocalSymbol *existing = analyzer->locals; existing != saved;
                                 existing = existing->next)
                                if (same_name(analyzer->program, existing->name_token,
                                              ast_program_lexeme(analyzer->program, binding->name_token)))
                                    semantic_error(analyzer, binding->name_token, ERROR_CATEGORY_SEMANTIC,
                                                   ERR_SEM_DUPLICATE_DEFINITION, "Duplicate pattern binding");
                            LocalSymbol *local = push_local(analyzer, binding->name_token, binding->type,
                                                            SEMANTIC_SYMBOL_LOCAL, NULL, 0);
                            if (local) binding->resolved_symbol_id = local->symbol_id;
                        }
                        if (p || binding) semantic_error(analyzer, arm->variant_token, ERROR_CATEGORY_SEMANTIC,
                                                         ERR_SEM_WRONG_ARG_COUNT,
                                                         "Match pattern payload binding count mismatch");
                    }
                }
                normalize_statement_types(analyzer, arm->body);
                analyze_statement(analyzer, arm->body);
                analyzer->scope_depth--;
                pop_to(analyzer, saved);
            }
            statement->match_exhaustive = wildcard || covered == variants;
            if (!statement->match_exhaustive) semantic_error(analyzer, statement->first_token, ERROR_CATEGORY_SEMANTIC,
                                                             ERR_SEM_INVALID_DECLARATION,
                                                             "Non-exhaustive enum match requires every variant or a wildcard");
            continue;
        }

        LocalSymbol *scope = analyzer->locals;
        if (statement->kind == AST_STMT_VARIABLE) {
            size_t errors_before = analyzer->model->error_count;
            validate_array_shape(analyzer, &statement->type);
            if (statement->value != NULL &&
                (statement->value->kind == AST_EXPR_ARRAY_LITERAL ||
                 statement->value->kind == AST_EXPR_CONTROL))
                statement->value->allocated_type = statement->type;
            if (statement->value != NULL && statement->value->kind == AST_EXPR_NAME &&
                statement->type.kind == AST_TYPE_FUNCTION &&
                !statement->value->explicit_type_arguments &&
                !statement->value->explicit_generic_reference)
                statement->value->allocated_type = statement->type;
            analyze_expression(analyzer, statement->value);
            validate_expression(analyzer, statement->value, 0);
            const AstType *effective_type = statement->type.kind == AST_TYPE_INFERRED &&
                                            statement->value && statement->value->has_resolved_ast_type
                                                ? &statement->value->resolved_ast_type : &statement->type;
            if (ast_type_contains_polymorphic_callable(effective_type)) {
                if (effective_type->kind != AST_TYPE_FUNCTION || effective_type->pointer_depth ||
                    effective_type->outer_pointer_depth || effective_type->borrow_kind != AST_BORROW_NONE ||
                    effective_type->is_array || effective_type->is_slice)
                    semantic_error(analyzer, statement->name_token, ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Polymorphic callable values cannot have runtime storage or indirection");
                if (statement->value == NULL)
                    semantic_error(analyzer, statement->name_token, ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Polymorphic callable locals require an initializer");
            }
            consume_call_arguments(analyzer, statement->value);
            if (!known_declared_type(analyzer, &statement->type))
                semantic_error(analyzer, statement->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                               "Unknown variable type");
            if (statement->type.kind == AST_TYPE_INFERRED && statement->value == NULL)
                semantic_error(analyzer, statement->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                               "Inferred variable requires an initializer");
            if (statement->is_const && statement->value == NULL)
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Constant requires an initializer");
            if (statement->is_const && statement->value != NULL &&
                !constant_expression_allowed(analyzer, statement->value))
                semantic_error(analyzer, statement->value->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Constant initializer is not a constant expression");
            if (statement->is_const &&
                (statement->type.pointer_depth != 0 ||
                 statement->type.outer_pointer_depth != 0 || statement->type.is_array ||
                 statement->type.is_slice ||
                 (statement->value != NULL &&
                  statement->value->resolved_named_symbol_id != AST_SYMBOL_NONE)))
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Constants require a primitive or string type");
            if (primitive_type(analyzer->program, &statement->type) == TYPE_VOID &&
                statement->type.pointer_depth == 0)
                semantic_error(analyzer, statement->type.name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Variable cannot have type void");
            if ((statement->type.kind != AST_TYPE_INFERRED &&
                 invalid_never_type(analyzer->program, &statement->type, 0)) ||
                (statement->type.kind == AST_TYPE_INFERRED && statement->value != NULL &&
                 statement->value->resolved_type == TYPE_NEVER))
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Variable cannot have type never");
            if (statement->type.kind != AST_TYPE_INFERRED && statement->value != NULL) {
                if (!expression_to_declared_type_allowed(analyzer, statement->value,
                                                         analyzer->program,
                                                         &statement->type))
                    conversion_error(analyzer, statement->value, analyzer->program, &statement->type,
                                     NULL, "Cannot implicitly convert initializer");
            }
            consume_owned_expression(analyzer, statement->value);
            if (statement->is_const && statement->value != NULL &&
                analyzer->model->error_count == errors_before &&
                constant_expression_allowed(analyzer, statement->value) &&
                effective_type->kind != AST_TYPE_FUNCTION)
                (void) fold_constant(analyzer, statement->value,
                                     statement->type.kind == AST_TYPE_INFERRED
                                         ? statement->value->resolved_type
                                         : primitive_type(analyzer->program, &statement->type));
            if (same_name(analyzer->program, statement->name_token, "true") ||
                same_name(analyzer->program, statement->name_token, "false"))
                semantic_error(analyzer, statement->name_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Reserved name cannot be declared");
            if (statement->type.is_array &&
                statement->type.array_length_token < analyzer->program->token_count) {
                unsigned long long length = statement->type.resolved_array_length;
                if (length == 0 || length > 1048576ULL)
                    semantic_error(analyzer, statement->type.array_length_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Array length must be positive and fit local storage");
            }
            size_t slots = semantic_type_slots(analyzer, analyzer->program,
                                               &statement->type, statement->value);
            size_t storage_limit = 8U * 1024U * 1024U;
            if (slots == SIZE_MAX || slots > storage_limit / 8U || analyzer->local_storage >
                storage_limit - slots * 8U) {
                if (!analyzer->storage_error_reported)
                    semantic_error(analyzer, statement->name_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_STORAGE_LIMIT,
                                   "Function local storage exceeds supported limit");
                analyzer->storage_error_reported = 1;
            } else analyzer->local_storage += slots * 8U;
            for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
                 existing = existing->next) {
                if (existing->scope_depth == analyzer->scope_depth &&
                    same_name(analyzer->program, existing->name_token,
                              ast_program_lexeme(analyzer->program, statement->name_token))) {
                    semantic_duplicate(analyzer, statement->name_token, analyzer->program, existing->name_token,
                                       "Duplicate variable");
                    break;
                }
            }
            LocalSymbol *local = push_local(analyzer, statement->name_token, statement->type,
                                            statement->is_const ? SEMANTIC_SYMBOL_CONSTANT : SEMANTIC_SYMBOL_LOCAL,
                                            statement->value, statement->is_const);
            if (local != NULL) statement->resolved_symbol_id = local->symbol_id;
        } else if (statement->kind == AST_STMT_FOR) {
            analyzer->scope_depth++;
            analyze_statement(analyzer, statement->initializer);
            analyze_expression(analyzer, statement->condition);
            validate_expression(analyzer, statement->condition, 0);
            if (statement->condition != NULL &&
                !plain_numeric_expression(statement->condition))
                operand_error(analyzer, statement->condition,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Condition requires a numeric or bit expression");
            analyzer->loop_depth++;
            analyze_statement(analyzer, statement->body);
            analyzer->loop_depth--;
            analyze_statement(analyzer, statement->else_body);
            pop_to(analyzer, scope);
            analyzer->scope_depth--;
        } else {
            if (statement->kind == AST_STMT_ASSIGNMENT) {
                analyzer->assignment_target = statement->expression;
                analyze_expression(analyzer, statement->expression);
                analyzer->assignment_target = NULL;
            } else analyze_expression(analyzer, statement->expression);
            if (statement->value != NULL &&
                (statement->value->kind == AST_EXPR_ARRAY_LITERAL ||
                 statement->value->kind == AST_EXPR_CONTROL)) {
                if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL)
                    statement->value->allocated_type = inferred_argument_type(analyzer, statement->expression);
                else if (statement->kind == AST_STMT_RETURN && analyzer->current_function != NULL)
                    statement->value->allocated_type =
                        analyzer->current_function->as.function.return_type;
            }
            analyze_expression(analyzer, statement->value);
            analyze_expression(analyzer, statement->condition);
            analyze_expression(analyzer, statement->update);
            validate_expression(analyzer, statement->expression, 0);
            validate_expression(analyzer, statement->value, 0);
            validate_expression(analyzer, statement->condition, 0);
            validate_expression(analyzer, statement->update, 0);
            consume_call_arguments(analyzer, statement->expression);
            consume_call_arguments(analyzer, statement->value);
            consume_call_arguments(analyzer, statement->condition);
            consume_call_arguments(analyzer, statement->update);
            if ((statement->kind == AST_STMT_IF || statement->kind == AST_STMT_WHILE) &&
                statement->condition != NULL &&
                !plain_numeric_expression(statement->condition))
                operand_error(analyzer, statement->condition,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Condition requires a numeric or bit expression");
            if (statement->kind == AST_STMT_ASSIGNMENT &&
                !assignable_expression(analyzer, statement->expression))
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "Unexpected statement: Assignment requires an assignable target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->value != NULL &&
                !expression_assignment_allowed(analyzer, statement->value,
                                               statement->expression)) {
                conversion_error(analyzer, statement->value, NULL, NULL,
                                 statement->expression,
                                 "Cannot implicitly convert assigned value");
            }
            if (statement->kind == AST_STMT_ASSIGNMENT &&
                statement->assignment_operator == TOKEN_EQUAL) {
                consume_owned_expression(analyzer, statement->value);
                if (statement->expression != NULL &&
                    statement->expression->kind == AST_EXPR_NAME) {
                    LocalSymbol *target = find_local_by_symbol(
                        analyzer, statement->expression->resolved_symbol_id);
                    if (target != NULL) {
                        target->moved = 0;
                        target->initialized = 1;
                    }
                }
            }
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_named_symbol_id != AST_SYMBOL_NONE &&
                statement->expression->resolved_named_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[
                    statement->expression->resolved_named_symbol_id].kind ==
                SEMANTIC_SYMBOL_STRUCT &&
                statement->assignment_operator != TOKEN_EQUAL)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Structures only support simple assignment");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->assignment_operator != TOKEN_EQUAL &&
                !plain_numeric_expression(statement->expression))
                operand_error(analyzer, statement->expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Compound assignment requires a numeric target");
            if (statement->kind == AST_STMT_ASSIGNMENT && statement->expression != NULL &&
                statement->expression->resolved_type == TYPE_BIT &&
                statement->assignment_operator != TOKEN_EQUAL)
                operand_error(analyzer, statement->expression,
                              ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                              "Boolean values do not support arithmetic assignment");
            if ((statement->kind == AST_STMT_BREAK ||
                 statement->kind == AST_STMT_CONTINUE) &&
                analyzer->in_defer_closure)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "Deferred anonymous functions cannot alter enclosing control flow");
            else if (statement->kind == AST_STMT_BREAK && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_BREAK_OUTSIDE_LOOP, "Break used outside a loop");
            if (statement->kind == AST_STMT_CONTINUE && analyzer->loop_depth == 0)
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_CONTINUE_OUTSIDE_LOOP, "Continue used outside a loop");
            if (statement->kind == AST_STMT_RETURN &&
                analyzer->in_defer_closure) {
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "A deferred anonymous function cannot return");
            } else if (statement->kind == AST_STMT_RETURN && analyzer->in_destructor) {
                semantic_error(analyzer, statement->first_token,
                               ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                               "A destructor cannot return");
            } else if (statement->kind == AST_STMT_RETURN && analyzer->current_function != NULL) {
                if (statement->value != NULL && statement->value->resolved_is_slice &&
                    statement->value->owns_slice_backing)
                    ((AstDeclarationNode *) analyzer->current_function)->as.function.
                        returns_owned_slice_backing = 1;
                DataType expected = primitive_type(analyzer->program,
                                                   &analyzer->current_function->as.function.return_type);
                if (analyzer->current_function->as.function.return_type.pointer_depth ||
                    analyzer->current_function->as.function.return_type.outer_pointer_depth)
                    expected = TYPE_UNKNOWN;
                if (expected == TYPE_NEVER)
                    semantic_error(analyzer, statement->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Never-returning function cannot use return");
                else if (expected == TYPE_VOID && statement->value != NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Void function cannot return a value");
                else if (expected != TYPE_VOID && statement->value == NULL)
                    semantic_error(analyzer, statement->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                                   "Function must return a value");
                else if (statement->value != NULL &&
                         !expression_to_declared_type_allowed(analyzer, statement->value,
                                                              analyzer->program,
                                                              &analyzer->current_function->as.function.return_type))
                    conversion_error(analyzer, statement->value, analyzer->program,
                                     &analyzer->current_function->as.function.return_type,
                                     NULL, "Cannot implicitly convert returned value");
                consume_owned_expression(analyzer, statement->value);
                if (analyzer->current_function->as.function.return_type.borrow_kind !=
                        AST_BORROW_NONE &&
                    statement->value != NULL &&
                    !safe_borrow_return_origin(analyzer, statement->value))
                    semantic_error(analyzer, statement->value->first_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                                   "Returned borrow may outlive its owner");
            }
            if (statement->kind == AST_STMT_DEFER &&
                statement->expression != NULL &&
                statement->expression->kind != AST_EXPR_CALL)
                semantic_error(analyzer, statement->expression->first_token,
                               ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "A deferred operation must be a function call");
            analyze_statement(analyzer, statement->initializer);
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth++;
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE)
                analyzer->scope_depth++;
            int saved_defer_closure = analyzer->in_defer_closure;
            if (statement->kind == AST_STMT_DEFER &&
                statement->body != NULL)
                analyzer->in_defer_closure = 1;
            analyze_statement(analyzer, statement->body);
            if (statement->kind == AST_STMT_BLOCK && statement->result != NULL) {
                analyze_expression(analyzer, statement->result);
                validate_expression(analyzer, statement->result, 0);
                consume_call_arguments(analyzer, statement->result);
            }
            analyzer->in_defer_closure = saved_defer_closure;
            if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
                statement->kind == AST_STMT_WHILE)
                analyzer->scope_depth--;
            if (statement->kind == AST_STMT_WHILE) analyzer->loop_depth--;
            analyze_statement(analyzer, statement->else_body);
        }
        if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_IF ||
            statement->kind == AST_STMT_WHILE)
            pop_to(analyzer, scope);
    }
}

static const AstExpression *borrow_origin_name(const AstExpression *expression) {
    while (expression != NULL &&
           ((expression->kind == AST_EXPR_UNARY &&
             expression->operator_type == TOKEN_AMPERSAND) ||
            expression->kind == AST_EXPR_MEMBER ||
            expression->kind == AST_EXPR_INDEX))
        expression = expression->kind == AST_EXPR_UNARY
                         ? expression->right : expression->left;
    return expression != NULL && expression->kind == AST_EXPR_NAME
               ? expression : NULL;
}

static size_t control_branch_count(const AstStatement *control) {
    if (control->kind == AST_STMT_BLOCK) return 1;
    if (control->kind == AST_STMT_IF) return 2;
    if (control->kind == AST_STMT_MATCH && control->is_type_match)
        return control->selected_type_arm == NULL ? 0 : 1;
    size_t count = 0;
    for (const AstMatchArm *arm = control->match_arms; arm; arm = arm->next)
        count++;
    return count;
}

static const AstStatement *control_branch_at(const AstStatement *control,
                                             size_t index) {
    if (control->kind == AST_STMT_BLOCK) return control;
    if (control->kind == AST_STMT_IF)
        return index == 0 ? control->body : control->else_body;
    if (control->is_type_match)
        return control->selected_type_arm == NULL ? NULL
            : control->selected_type_arm->body;
    const AstMatchArm *arm = control->match_arms;
    while (arm != NULL && index-- != 0) arm = arm->next;
    return arm == NULL ? NULL : arm->body;
}

static const AstExpression *reachable_branch_result(const AstStatement *block) {
    if (block == NULL || block->result == NULL ||
        block->result->resolved_type == TYPE_NEVER ||
        !statement_may_fall_through(block->body)) return NULL;
    return block->result;
}

static void validate_slice_branch_lifetime(Analyzer *analyzer,
                                           const AstExpression *result,
                                           const AstType *target) {
    if (result == NULL || target == NULL || !target->is_slice ||
        !result->resolved_is_array) return;
    const AstExpression *origin = borrow_origin_name(result);
    if (origin != NULL && origin->resolved_symbol_id <
                              analyzer->model->symbol_count &&
        analyzer->model->symbols[origin->resolved_symbol_id].scope_depth >
            analyzer->scope_depth)
        semantic_error(analyzer, result->first_token,
                       ERROR_CATEGORY_SEMANTIC,
                       ERR_SEM_INVALID_DECLARATION,
                       "Value branch cannot expose a slice of a local array");
}

static void validate_control_branch(Analyzer *analyzer,
                                    const AstExpression *control,
                                    const AstStatement *block) {
    if (block == NULL) return;
    const AstExpression *result = block->result;
    if (result == NULL) {
        if (statement_may_fall_through(block->body))
            semantic_error(analyzer, block->first_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_INCOMPATIBLE_TYPES,
                           "Value branch must end with an expression");
        return;
    }
    if (reachable_branch_result(block) == NULL) return;
    if (result->resolved_type == TYPE_VOID)
        semantic_error(analyzer, result->first_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Value branch cannot end with a void expression");
    if (result->resolved_borrow_kind != AST_BORROW_NONE) {
        const AstExpression *origin = borrow_origin_name(result);
        if (origin != NULL && origin->resolved_symbol_id <
                                  analyzer->model->symbol_count &&
            analyzer->model->symbols[origin->resolved_symbol_id].scope_depth >
                analyzer->scope_depth)
            semantic_error(analyzer, result->first_token,
                           ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_INVALID_DECLARATION,
                           "Value branch cannot return a borrow of a local binding");
    }
    if (control->allocated_type.kind != AST_TYPE_INFERRED) {
        validate_slice_branch_lifetime(analyzer, result,
                                       &control->allocated_type);
        if (!expression_to_declared_type_allowed(analyzer, result,
                                                 analyzer->program,
                                                 &control->allocated_type))
            semantic_error(analyzer, result->first_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                           "Value branch cannot convert to the expected type");
    }
}

void analyze_control_expression(Analyzer *analyzer, AstExpression *expression) {
    AstStatement *control = expression->control;
    if (control == NULL) return;
    if (expression->allocated_type.kind != AST_TYPE_INFERRED) {
        if (control->kind == AST_STMT_BLOCK) {
            if (control->result != NULL)
                control->result->allocated_type = expression->allocated_type;
        } else if (control->kind == AST_STMT_IF) {
            if (control->body != NULL && control->body->result != NULL)
                control->body->result->allocated_type = expression->allocated_type;
            if (control->else_body != NULL &&
                control->else_body->result != NULL)
                control->else_body->result->allocated_type =
                    expression->allocated_type;
        } else if (control->kind == AST_STMT_MATCH) {
            for (AstMatchArm *arm = control->match_arms; arm; arm = arm->next)
                if (arm->body != NULL && arm->body->result != NULL)
                    arm->body->result->allocated_type =
                        expression->allocated_type;
        }
    }
    analyze_statement(analyzer, control);
    size_t branch_count = control_branch_count(control);
    size_t live_count = 0;
    for (size_t i = 0; i < branch_count; i++) {
        const AstStatement *block = control_branch_at(control, i);
        validate_control_branch(analyzer, expression, block);
        if (reachable_branch_result(block) != NULL) live_count++;
    }
    if (live_count == 0) {
        expression->resolved_type = TYPE_NEVER;
        return;
    }
    if (expression->allocated_type.kind != AST_TYPE_INFERRED) {
        set_expression_declared_type(analyzer, expression, analyzer->program,
                                     &expression->allocated_type);
        if (expression->allocated_type.is_slice) {
            int backing_ownership = -1;
            for (size_t i = 0; i < branch_count; i++) {
                const AstExpression *result = reachable_branch_result(
                    control_branch_at(control, i));
                if (result == NULL) continue;
                int owns = result->owns_slice_backing != 0;
                if (backing_ownership >= 0 && backing_ownership != owns)
                    semantic_error(analyzer, result->first_token,
                                   ERROR_CATEGORY_SEMANTIC,
                                   ERR_SEM_INVALID_DECLARATION,
                                   "Value branches must agree on slice backing ownership");
                backing_ownership = owns;
            }
            expression->owns_slice_backing = backing_ownership > 0;
        }
        return;
    }
    const AstExpression *chosen = NULL;
    for (size_t i = 0; i < branch_count; i++) {
        const AstExpression *candidate = reachable_branch_result(
            control_branch_at(control, i));
        if (candidate == NULL) continue;
        AstType target = inferred_argument_type(analyzer, candidate);
        int accepts_all = 1;
        for (size_t j = 0; j < branch_count; j++) {
            const AstExpression *source = reachable_branch_result(
                control_branch_at(control, j));
            if (source != NULL &&
                !expression_to_declared_type_allowed(analyzer, source,
                                                     analyzer->program,
                                                     &target)) {
                accepts_all = 0;
                break;
            }
        }
        if (!accepts_all) continue;
        if (chosen != NULL) {
            AstType previous = inferred_argument_type(analyzer, chosen);
            if (!expression_to_declared_type_allowed(analyzer, chosen,
                                                     analyzer->program,
                                                     &target) ||
                expression_to_declared_type_allowed(analyzer, candidate,
                                                    analyzer->program,
                                                    &previous))
                continue;
        }
        chosen = candidate;
    }
    if (chosen == NULL) {
        semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Value branches have incompatible types");
        return;
    }
    AstType selected_type = inferred_argument_type(analyzer, chosen);
    int backing_ownership = -1;
    for (size_t i = 0; i < branch_count; i++) {
        const AstExpression *result = reachable_branch_result(
            control_branch_at(control, i));
        if (result == NULL) continue;
        validate_slice_branch_lifetime(analyzer, result, &selected_type);
        if (selected_type.is_slice) {
            int owns = result->owns_slice_backing != 0;
            if (backing_ownership >= 0 && backing_ownership != owns)
                semantic_error(analyzer, result->first_token,
                               ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "Value branches must agree on slice backing ownership");
            backing_ownership = owns;
        }
    }
    expression->resolved_type = chosen->resolved_type;
    expression->resolved_borrow_kind = chosen->resolved_borrow_kind;
    expression->resolved_pointer_depth = chosen->resolved_pointer_depth;
    expression->resolved_outer_pointer_depth = chosen->resolved_outer_pointer_depth;
    expression->resolved_named_type_token = chosen->resolved_named_type_token;
    expression->resolved_named_symbol_id = chosen->resolved_named_symbol_id;
    expression->resolved_is_array = chosen->resolved_is_array;
    expression->resolved_is_slice = chosen->resolved_is_slice;
    expression->resolved_array_length = chosen->resolved_array_length;
    expression->resolved_ast_type = chosen->resolved_ast_type;
    expression->resolved_type_program = chosen->resolved_type_program;
    expression->has_resolved_ast_type = chosen->has_resolved_ast_type;
    expression->owns_slice_backing = chosen->owns_slice_backing;
}

static void analyze_function(Analyzer *analyzer, AstDeclarationNode *function) {
    if (function->generic_parameters != NULL || function->semantic_body_checked) return;
    function->semantic_body_checked = 1;
    LocalSymbol *saved = analyzer->locals;
    size_t saved_function = analyzer->current_function_token;
    size_t saved_function_symbol = analyzer->current_function_symbol_id;
    size_t saved_owner = analyzer->current_owner_token;
    const AstDeclarationNode *saved_declaration = analyzer->current_function;
    analyzer->current_function_token = function->name_token;
    analyzer->current_function_symbol_id = function->resolved_symbol_id;
    analyzer->current_owner_token = function->as.function.owner_token;
    analyzer->current_function = function;
    analyzer->local_storage = 0;
    analyzer->storage_error_reported = 0;
    analyzer->complexity_error_reported = 0;
    if (!known_declared_type(analyzer, &function->as.function.return_type))
        semantic_error(analyzer, function->as.function.return_type.name_token,
                       ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown function return type");
    if (invalid_never_type(analyzer->program,
                           &function->as.function.return_type, 1))
        semantic_error(analyzer, function->as.function.return_type.name_token,
                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "never is only valid as a direct function return type");
    validate_array_shape(analyzer, &function->as.function.return_type);
    analyzer->scope_depth++;
    for (AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        if (!known_declared_type(analyzer, &parameter->type))
            semantic_error(analyzer, parameter->type.name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown parameter type");
        validate_array_shape(analyzer, &parameter->type);
        if (primitive_type(analyzer->program, &parameter->type) == TYPE_VOID &&
            parameter->type.pointer_depth == 0)
            semantic_error(analyzer, parameter->type.name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Parameter cannot have type void");
        if (invalid_never_type(analyzer->program, &parameter->type, 0))
            semantic_error(analyzer, parameter->type.name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Parameter cannot have type never");
        for (const LocalSymbol *existing = analyzer->locals; existing != NULL;
             existing = existing->next)
            if (existing->scope_depth == analyzer->scope_depth &&
                same_name(analyzer->program, existing->name_token,
                          ast_program_lexeme(analyzer->program,
                                             parameter->name_token))) {
                semantic_duplicate(analyzer, parameter->name_token, analyzer->program, existing->name_token,
                                   "Duplicate parameter");
                break;
            }
        if (same_name(analyzer->program, parameter->name_token, "true") ||
            same_name(analyzer->program, parameter->name_token, "false"))
            semantic_error(analyzer, parameter->name_token,
                           ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                           "Reserved name cannot be declared");
        LocalSymbol *local = push_local(analyzer, parameter->name_token, parameter->type,
                                        SEMANTIC_SYMBOL_PARAMETER,
                                        parameter->compile_time_value, 0);
        if (local != NULL) parameter->resolved_symbol_id = local->symbol_id;
    }
    if (!function->is_native) {
        analyze_statement(analyzer, function->as.function.body);
        validate_function_ownership(analyzer, function);
        validate_function_borrows(analyzer, function);
    }
    DataType return_type = primitive_type(analyzer->program,
                                          &function->as.function.return_type);
    if (function->as.function.return_type.pointer_depth || function->as.function.return_type.outer_pointer_depth)
        return_type = TYPE_UNKNOWN;
    if (!function->is_native && return_type != TYPE_VOID &&
        statement_may_fall_through(function->as.function.body))
        semantic_error(analyzer, function->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                       return_type == TYPE_NEVER
                           ? "Never-returning function can complete normally"
                           : "Function does not return on all paths");
    pop_to(analyzer, saved);
    analyzer->scope_depth--;
    analyzer->current_function_token = saved_function;
    analyzer->current_function_symbol_id = saved_function_symbol;
    analyzer->current_owner_token = saved_owner;
    analyzer->current_function = saved_declaration;
}

static void analyze_destructor(Analyzer *analyzer, AstDeclarationNode *resource) {
    if (resource->as.struct_decl.destructor == NULL) return;
    LocalSymbol *saved = analyzer->locals;
    size_t saved_function = analyzer->current_function_token;
    size_t saved_function_symbol = analyzer->current_function_symbol_id;
    size_t saved_owner = analyzer->current_owner_token;
    const AstDeclarationNode *saved_declaration = analyzer->current_function;
    int saved_destructor = analyzer->in_destructor;
    analyzer->current_function_token = AST_TOKEN_NONE;
    analyzer->current_function_symbol_id = AST_SYMBOL_NONE;
    analyzer->current_owner_token = resource->name_token;
    analyzer->current_function = NULL;
    analyzer->in_destructor = 1;
    analyzer->scope_depth++;
    analyze_statement(analyzer, resource->as.struct_decl.destructor);
    AstDeclarationNode destructor_function = {.kind = AST_DECL_FUNCTION, .name_token = resource->name_token};
    destructor_function.as.function.body = resource->as.struct_decl.destructor;
    validate_function_ownership(analyzer, &destructor_function);
    validate_function_borrows(analyzer, &destructor_function);
    pop_to(analyzer, saved);
    analyzer->scope_depth--;
    analyzer->current_function_token = saved_function;
    analyzer->current_function_symbol_id = saved_function_symbol;
    analyzer->current_owner_token = saved_owner;
    analyzer->current_function = saved_declaration;
    analyzer->in_destructor = saved_destructor;
}

void analyze_constant_declaration(Analyzer *analyzer,
                                         AstDeclarationNode *declaration) {
    if (declaration->semantic_body_checked) return;
    declaration->semantic_body_checked = 2;
    size_t errors_before = analyzer->model->error_count;
    AstExpression *value = declaration->as.constant.value;
    AstType *type = &declaration->as.constant.type;
    if (declaration->kind == AST_DECL_VARIABLE &&
        ast_type_contains_polymorphic_callable(type))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Package variables cannot store polymorphic callable values");
    if (declaration->kind == AST_DECL_VARIABLE &&
        type->borrow_kind != AST_BORROW_NONE)
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Checked borrowed references cannot be stored in package variables");
    if (invalid_never_type(analyzer->program, type, 0) ||
        (type->kind == AST_TYPE_INFERRED && value != NULL &&
         value->resolved_type == TYPE_NEVER))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Package value cannot have type never");
    if (declaration->kind == AST_DECL_VARIABLE && !value) {
        if (type->kind == AST_TYPE_INFERRED || !known_declared_type(analyzer, type) ||
            (primitive_type(analyzer->program, type) == TYPE_VOID && !type->pointer_depth && !type->
             outer_pointer_depth))
            semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Package variable needs a concrete non-void type or a constant initializer");
        declaration->semantic_body_checked = 1;
        return;
    }
    if (value != NULL && (value->kind == AST_EXPR_ARRAY_LITERAL ||
                          value->kind == AST_EXPR_CONTROL))
        value->allocated_type = *type;
    analyze_expression(analyzer, value);
    validate_expression(analyzer, value, 0);
    if (declaration->kind == AST_DECL_VARIABLE && value != NULL &&
        value->has_resolved_ast_type &&
        ast_type_contains_polymorphic_callable(&value->resolved_ast_type))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Package variables cannot store polymorphic callable values");
    if (declaration->kind == AST_DECL_CONSTANT &&
        (value == NULL || !constant_expression_allowed(analyzer, value)))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Constant initializer is not a constant expression");
    if (!known_declared_type(analyzer, type))
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                       "Unknown constant type");
    if (declaration->kind == AST_DECL_CONSTANT &&
        (type->pointer_depth != 0 || type->outer_pointer_depth != 0 ||
         type->is_array || type->is_slice ||
         (value != NULL && value->resolved_named_symbol_id != AST_SYMBOL_NONE)))
        semantic_error(analyzer, declaration->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Constants require a primitive or string type");
    if (type->kind != AST_TYPE_INFERRED && value != NULL &&
        !expression_to_declared_type_allowed(analyzer, value, analyzer->program, type))
        conversion_error(analyzer, value, analyzer->program, type,
                         NULL, "Cannot implicitly convert constant initializer");
    if (value != NULL && analyzer->model->error_count == errors_before &&
        constant_expression_allowed(analyzer, value) &&
        !(value->has_resolved_ast_type &&
          value->resolved_ast_type.kind == AST_TYPE_FUNCTION))
        (void) fold_constant(analyzer, value,
                             type->kind == AST_TYPE_INFERRED
                                 ? value->resolved_type
                                 : primitive_type(analyzer->program, type));
    if (declaration->resolved_symbol_id < analyzer->model->symbol_count && value != NULL &&
        type->kind == AST_TYPE_INFERRED) {
        SemanticSymbol *symbol =
                &analyzer->model->symbols[declaration->resolved_symbol_id];
        symbol->resolved_type = value->resolved_type;
        symbol->resolved_pointer_depth = value->resolved_pointer_depth;
        symbol->resolved_outer_pointer_depth = value->resolved_outer_pointer_depth;
        symbol->resolved_named_type_token = value->resolved_named_type_token;
        symbol->resolved_named_symbol_id = value->resolved_named_symbol_id;
        symbol->resolved_is_array = value->resolved_is_array;
        symbol->resolved_is_slice = value->resolved_is_slice;
    }
    declaration->semantic_body_checked = 1;
}

static void analyze_unit_constants(Analyzer *analyzer, AstProgram *root,
                                   AstProgram *unit, unsigned char *states) {
    size_t index = 0;
    if (unit != root) {
        for (index = 1; index <= root->owned_import_count; index++)
            if (root->owned_imports[index - 1] == unit) break;
        if (index > root->owned_import_count) return;
    }
    if (states[index] != 0) return;
    states[index] = 1;
    for (AstDeclarationNode *declaration = unit->root; declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_IMPORT)
            for (AstImportPath *path = declaration->as.import_decl.paths; path != NULL; path = path->next)
                if (path->resolved_program != NULL)
                    analyze_unit_constants(analyzer, root, path->resolved_program, states);
    analyzer->program = unit;
    for (AstDeclarationNode *declaration = unit->root; declaration != NULL; declaration = declaration->next)
        if (declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE)
            analyze_constant_declaration(analyzer, declaration);
    states[index] = 2;
}

static void check_package_initializer_cycle(Analyzer *analyzer, size_t symbol_id,
                                            unsigned char *states);

static void check_initializer_expression(Analyzer *analyzer,
                                         const AstProgram *package_program,
                                         const AstExpression *expression,
                                         unsigned char *states) {
    for (const AstExpression *current = expression; current != NULL;
         current = current->next) {
        if (current->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *dependency =
                &analyzer->model->symbols[current->resolved_symbol_id];
            if (dependency->kind == SEMANTIC_SYMBOL_VARIABLE &&
                same_package(package_program, dependency->source_program)) {
                if (states[dependency->id] == 1)
                    semantic_error(analyzer, current->value_token,
                                   ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Package variable initializer contains a dependency cycle");
                else if (states[dependency->id] == 0)
                    check_package_initializer_cycle(analyzer, dependency->id,
                                                    states);
            }
        }
        check_initializer_expression(analyzer, package_program, current->left,
                                     states);
        check_initializer_expression(analyzer, package_program, current->right,
                                     states);
        check_initializer_expression(analyzer, package_program,
                                     current->arguments, states);
    }
}

static void check_package_initializer_cycle(Analyzer *analyzer, size_t symbol_id,
                                            unsigned char *states) {
    if (symbol_id >= analyzer->model->symbol_count || states[symbol_id] != 0)
        return;
    const SemanticSymbol *symbol = &analyzer->model->symbols[symbol_id];
    if (symbol->kind != SEMANTIC_SYMBOL_VARIABLE || symbol->declaration == NULL)
        return;
    states[symbol_id] = 1;
    check_initializer_expression(analyzer, symbol->source_program,
                                 symbol->declaration->as.constant.value, states);
    states[symbol_id] = 2;
}

static int check_package_initializer_cycles(Analyzer *analyzer) {
    unsigned char *states = calloc(analyzer->model->symbol_count, 1);
    if (states == NULL && analyzer->model->symbol_count != 0) return 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++)
        if (analyzer->model->symbols[i].kind == SEMANTIC_SYMBOL_VARIABLE)
            check_package_initializer_cycle(analyzer, i, states);
    free(states);
    return 1;
}

SemanticModel *semantic_analyze(AstProgram *program) {
#ifdef _WIN32
    return semantic_analyze_target(program, TARGET_COFF);
#else
    return semantic_analyze_target(program, TARGET_ELF);
#endif
}

SemanticModel *semantic_analyze_target(AstProgram *program, TargetFormat target) {
    if (program == NULL || !program->structured_ast_complete) return NULL;
    SemanticModel *model = calloc(1, sizeof(*model));
    if (model == NULL) return NULL;
    model->program = program;
    model->target_format = target;
    Analyzer analyzer = {
        .model = model,
        .program = program,
        .current_function_token = AST_TOKEN_NONE,
        .current_function_symbol_id = AST_SYMBOL_NONE,
        .current_owner_token = AST_TOKEN_NONE
    };
    async_prelude(&analyzer);
    /* Parsing async syntax must never make an unsupported function run eagerly. */
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root; declaration != NULL;
             declaration = declaration->next) {
            if (declaration->kind == AST_DECL_FUNCTION && declaration->as.function.is_async &&
                !semantic_async_enabled(&analyzer))
                semantic_error(&analyzer, declaration->first_token, ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "async functions require the async manifest feature");
            AstDeclarationNode *methods = declaration->kind == AST_DECL_STRUCT
                                              ? declaration->as.struct_decl.methods
                                              : declaration->kind == AST_DECL_ENUM
                                                    ? declaration->as.enum_decl.methods
                                                    : declaration->kind == AST_DECL_INTERFACE
                                                          ? declaration->as.interface_decl.methods : NULL;
            for (AstDeclarationNode *method = methods; method != NULL; method = method->next)
                if (method->as.function.is_async && !semantic_async_enabled(&analyzer))
                    semantic_error(&analyzer, method->first_token, ERROR_CATEGORY_SEMANTIC,
                                   ERR_SEM_INVALID_DECLARATION,
                                   "async functions require the async manifest feature");
        }
    }
    for (size_t i = 0; i <= program->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? program : program->owned_imports[i - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            d->semantic_body_checked = 0;
            if (d->generic_parameters && d->kind != AST_DECL_INTERFACE)
                continue;
            if (d->kind == AST_DECL_FUNCTION) normalize_function_types(&analyzer, d);
            else if (d->kind == AST_DECL_TYPE_RULE) normalize_generic_type(&analyzer, &d->rule_target, 0);
            else if (d->kind == AST_DECL_VARIABLE || d->kind == AST_DECL_CONSTANT) normalize_generic_type(
                &analyzer, &d->as.constant.type, 0);
            else if (d->kind == AST_DECL_INTERFACE &&
                     d->generic_parameters == NULL) {
                for (AstDeclarationNode *m = d->as.interface_decl.methods; m; m = m->next)
                    normalize_function_types(&analyzer, m);
            }
            else if (d->kind == AST_DECL_ENUM) {
                for (AstEnumValue *v = d->as.enum_decl.values; v; v = v->next)
                    for (AstTypeArgument *p = v->payload_types; p; p = p->next)
                        normalize_generic_type(&analyzer, &p->type, 0);
                AstType self_type = {.kind = AST_TYPE_NAMED, .name_token = d->name_token,
                                     .array_length_token = AST_TOKEN_NONE};
                for (AstDeclarationNode *m = d->as.enum_decl.methods; m; m = m->next) {
                    m->semantic_body_checked = 0;
                    replace_self_type(&analyzer, &m->as.function.return_type, &self_type);
                    for (AstParameter *p = m->as.function.parameters; p; p = p->next)
                        replace_self_type(&analyzer, &p->type, &self_type);
                    replace_self_statement(&analyzer, m->as.function.body, &self_type);
                    normalize_function_types(&analyzer, m);
                }
            } else if (d->kind == AST_DECL_STRUCT) {
                for (AstField *f = d->as.struct_decl.fields; f; f = f->next)
                    normalize_generic_type(&analyzer, &f->type, 0);
                AstType self_type = {.kind = AST_TYPE_NAMED, .name_token = d->name_token,
                                     .array_length_token = AST_TOKEN_NONE};
                for (AstDeclarationNode *m = d->as.struct_decl.methods; m; m = m->next) {
                    m->semantic_body_checked = 0;
                    replace_self_type(&analyzer, &m->as.function.return_type, &self_type);
                    for (AstParameter *p = m->as.function.parameters; p; p = p->next)
                        replace_self_type(&analyzer, &p->type, &self_type);
                    replace_self_statement(&analyzer, m->as.function.body, &self_type);
                    normalize_function_types(&analyzer, m);
                }
            }
        }
    }
    analyzer.program = program;
    prepare_interfaces(&analyzer, program);
    collect_declarations(&analyzer, program);
    for (size_t i = 0; i < program->owned_import_count; i++)
        collect_declarations(&analyzer, program->owned_imports[i]);
    derive_type_properties(&analyzer);
    analyzer.program = program;
    unsigned char *constant_states = calloc(program->owned_import_count + 1, 1);
    if (constant_states == NULL) {
        semantic_model_free(model);
        return NULL;
    }
    analyze_unit_constants(&analyzer, program, program, constant_states);
    for (size_t i = 0; i < program->owned_import_count; i++)
        analyze_unit_constants(&analyzer, program, program->owned_imports[i], constant_states);
    free(constant_states);
    if (!check_package_initializer_cycles(&analyzer)) {
        semantic_model_free(model);
        return NULL;
    }
    /* Complete declaration type shapes before bodies use forward declarations. */
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        analyzer.program = (AstProgram *) symbol->source_program;
        if (symbol->kind == SEMANTIC_SYMBOL_FIELD) {
            AstField *field = (AstField *) symbol->node;
            validate_array_shape(&analyzer, &field->type);
        } else if (symbol->kind == SEMANTIC_SYMBOL_VARIABLE) {
            AstDeclarationNode *variable = (AstDeclarationNode *) symbol->declaration;
            validate_array_shape(&analyzer, &variable->as.constant.type);
            symbol->declared_type = variable->as.constant.type;
            symbol->resolved_named_symbol_id = resolve_named_symbol_id(
                &analyzer, symbol->source_program,
                named_type_token(symbol->source_program,
                                 &symbol->declared_type));
            symbol->type_properties = semantic_declared_type_properties(
                &analyzer, symbol->source_program, &symbol->declared_type);
        } else if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration != NULL) {
            AstDeclarationNode *function = (AstDeclarationNode *) symbol->declaration;
            validate_array_shape(&analyzer, &function->as.function.return_type);
            symbol->declared_type = function->as.function.return_type;
            for (AstParameter *parameter = function->as.function.parameters;
                 parameter != NULL; parameter = parameter->next)
                validate_array_shape(&analyzer, &parameter->type);
        }
    }
    analyzer.program = program;
    validate_overload_sets(&analyzer);
    validate_auto_rules(&analyzer);
    derive_type_properties(&analyzer);
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *s = &model->symbols[i];
        const AstDeclarationNode *d = s->declaration;
        if (!d || !d->generic_origin ||
            (s->kind != SEMANTIC_SYMBOL_STRUCT && s->kind != SEMANTIC_SYMBOL_ENUM)) continue;
        AstType arguments[DMM_MAX_TYPE_PARAMETERS];
        size_t count = 0;
        for (const AstTypeArgument *arg = d->specialization_arguments; arg && count < DMM_MAX_TYPE_PARAMETERS; arg = arg->next)
            arguments[count++] = arg->type;
        analyzer.program = (AstProgram *)s->source_program;
        if (!generic_bounds_satisfied(&analyzer, s->source_program, d->generic_origin, arguments))
            semantic_error(&analyzer, d->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Generic aggregate type arguments do not satisfy interface bounds");
    }
    analyzer.program = program;
    validate_native_declarations(&analyzer);
    for (size_t unit_index = 0; unit_index <= program->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? program : program->owned_imports[unit_index - 1];
        analyzer.program = unit;
        for (AstDeclarationNode *declaration = unit->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->generic_parameters != NULL) continue;
            if (declaration->kind == AST_DECL_FUNCTION)
                analyze_function(&analyzer, declaration);
            else if (declaration->kind == AST_DECL_STRUCT) {
                for (AstField *field = declaration->as.struct_decl.fields;
                     field != NULL; field = field->next) {
                    if (ast_type_contains_polymorphic_callable(&field->type))
                        semantic_error(&analyzer, field->name_token, ERROR_CATEGORY_TYPE,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Struct fields cannot store polymorphic callable values");
                    if (!known_declared_type(&analyzer, &field->type))
                        semantic_error(&analyzer, field->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown field type");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Field cannot have type void");
                    if (invalid_never_type(unit, &field->type, 0))
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Field cannot have type never");
                    if (field->type.borrow_kind != AST_BORROW_NONE)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Checked borrowed references cannot be stored in struct fields");
                    validate_array_shape(&analyzer, &field->type);
                }
                analyze_destructor(&analyzer, declaration);
                for (AstDeclarationNode *method = declaration->as.struct_decl.methods;
                     method != NULL; method = method->next)
                    analyze_function(&analyzer, method);
            } else if (declaration->kind == AST_DECL_ENUM) {
                for (AstField *field = declaration->as.enum_decl.fields;
                     field != NULL; field = field->next) {
                    if (!known_declared_type(&analyzer, &field->type))
                        semantic_error(&analyzer, field->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown enum field type");
                    if (field->type.is_slice)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Slice fields are not supported");
                    if (field->type.borrow_kind != AST_BORROW_NONE)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Checked borrowed references cannot be stored in enum fields");
                    if (primitive_type(unit, &field->type) == TYPE_VOID &&
                        field->type.pointer_depth == 0)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum field cannot have type void");
                    if (invalid_never_type(unit, &field->type, 0))
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum field cannot have type never");
                    if (primitive_type(unit, &field->type) == TYPE_UNKNOWN ||
                        field->type.pointer_depth != 0 || field->type.is_array)
                        semantic_error(&analyzer, field->type.name_token,
                                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum fields require scalar primitive types");
                    validate_array_shape(&analyzer, &field->type);
                }
                size_t field_count = 0;
                for (AstField *field = declaration->as.enum_decl.fields;
                     field != NULL; field = field->next)
                    field_count++;
                for (AstEnumValue *value = declaration->as.enum_decl.values;
                     value != NULL; value = value->next) {
                    for (AstTypeArgument *p = value->payload_types; p; p = p->next) {
                        if (ast_type_contains_polymorphic_callable(&p->type))
                            semantic_error(&analyzer, p->type.name_token, ERROR_CATEGORY_TYPE,
                                           ERR_TYPE_INVALID_OPERATION,
                                           "Enum payloads cannot store polymorphic callable values");
                        if (!known_declared_type(&analyzer, &p->type) ||
                            (primitive_type(unit, &p->type) == TYPE_VOID && !p->type.pointer_depth) ||
                            invalid_never_type(unit, &p->type, 0))
                            semantic_error(&analyzer, p->type.name_token, ERROR_CATEGORY_TYPE,
                                           ERR_TYPE_INVALID_OPERATION,
                                           "Enum payload requires a complete non-void concrete type");
                        validate_array_shape(&analyzer, &p->type);
                    }

                    size_t argument_count = 0;
                    for (AstExpression *argument = value->arguments;
                         argument != NULL; argument = argument->next) {
                        analyze_expression(&analyzer, argument);
                        validate_expression(&analyzer, argument, 0);
                        argument_count++;
                    }
                    if (argument_count != field_count) {
                        char message[128];
                        (void) snprintf(message, sizeof(message),
                                        "Enum value expects %zu arguments but received %zu",
                                        field_count, argument_count);
                        semantic_error(&analyzer, value->name_token,
                                       ERROR_CATEGORY_SEMANTIC, ERR_SEM_WRONG_ARG_COUNT, message);
                    } else {
                        AstExpression *argument = value->arguments;
                        AstField *field = declaration->as.enum_decl.fields;
                        for (; argument != NULL && field != NULL;
                               argument = argument->next, field = field->next) {
                            if (!enum_constant_expression(&analyzer, argument))
                                semantic_error(&analyzer, argument->first_token,
                                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                               "Enum arguments must be compile-time scalar constants");
                            if (!expression_to_declared_type_allowed(&analyzer, argument,
                                                                     unit, &field->type))
                                conversion_error(&analyzer, argument, unit, &field->type, NULL,
                                                 "Cannot implicitly convert enum argument");
                        }
                    }
                }
                for (AstDeclarationNode *method = declaration->as.enum_decl.methods;
                     method != NULL; method = method->next)
                    analyze_function(&analyzer, method);
            }
        }
    }
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration != NULL) {
            if (symbol->owner_symbol_id < model->symbol_count &&
                model->symbols[symbol->owner_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE)
                continue;
            analyzer.program = (AstProgram *) symbol->source_program;
            analyze_function(&analyzer, (AstDeclarationNode *) symbol->declaration);
        }
    }
    finalize_owned_slice_returns(model);
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if ((symbol->kind != SEMANTIC_SYMBOL_STRUCT && symbol->kind != SEMANTIC_SYMBOL_ENUM) || !symbol->declaration)
            continue;
        analyzer.program = (AstProgram *) symbol->source_program;
        if (aggregate_reaches(&analyzer, symbol->id, symbol->id, 0))
            semantic_error(&analyzer, symbol->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           symbol->kind == SEMANTIC_SYMBOL_ENUM
                               ? "Recursive enum payload requires a pointer"
                               : "Recursive structure requires a pointer field");
        const AstDeclarationNode *declaration = symbol->declaration;
        if (declaration->generic_origin) {
            AstType arguments[DMM_MAX_TYPE_PARAMETERS];
            size_t count = 0;
            for (const AstTypeArgument *p = declaration->specialization_arguments; p && count < DMM_MAX_TYPE_PARAMETERS;
                 p = p->next)
                arguments[count++] = p->type;
            if (!generic_bounds_satisfied(&analyzer, symbol->source_program, declaration->generic_origin, arguments))
                semantic_error(&analyzer, symbol->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                               "Generic aggregate type arguments do not satisfy interface bounds");
        }
        if (symbol->kind == SEMANTIC_SYMBOL_ENUM)
            for (AstEnumValue *v = declaration->as.enum_decl.values; v; v = v->next)
                for (AstTypeArgument *p = v->payload_types; p; p = p->next) {
                    validate_array_shape(&analyzer, &p->type);
                    if (!known_declared_type(&analyzer, &p->type) ||
                        (primitive_type(analyzer.program, &p->type) == TYPE_VOID && !p->type.pointer_depth) ||
                        invalid_never_type(analyzer.program, &p->type, 0))
                        semantic_error(&analyzer, p->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Enum payload requires a complete non-void concrete type");
                }
    }
    const SemanticSymbol *main_symbol = semantic_find_global(model, "main",
                                                             SEMANTIC_SYMBOL_FUNCTION);
    if (program->executable_build && program->package_name && strcmp(program->package_name, "main")) {
        analyzer.program = program;
        semantic_error(&analyzer, program->package_token, ERROR_CATEGORY_COMPILER, ERR_COMP_NO_MAIN_FUNCTION,
                       "Executable builds require package main");
    } else if (program->package_name && strcmp(program->package_name, "main")) {
        /* Library packages have no entry-point requirement. */
    } else if (main_symbol == NULL || main_symbol->declaration == NULL) {
        analyzer.program = program;
        semantic_error(&analyzer, AST_TOKEN_NONE, ERROR_CATEGORY_COMPILER, ERR_COMP_NO_MAIN_FUNCTION,
                       "Program must define main: func main() -> int or func main() -> void");
    } else {
        analyzer.program = (AstProgram *) main_symbol->source_program;
        const AstDeclarationNode *main_declaration = main_symbol->declaration;
        DataType main_type = primitive_type(main_symbol->source_program,
                                            &main_declaration->as.function.return_type);
        if (main_declaration->as.function.is_async || main_declaration->as.function.parameters != NULL ||
            main_declaration->as.function.return_type.pointer_depth != 0 ||
            main_declaration->as.function.return_type.outer_pointer_depth != 0 ||
            main_declaration->as.function.return_type.is_array || main_declaration->as.function.return_type.is_slice ||
            (main_type != TYPE_VOID && main_type != TYPE_INT))
            semantic_error(&analyzer, main_symbol->name_token,
                           ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                           "main must have no parameters and return void or int");
    }
    derive_async_properties(&analyzer);
    pop_to(&analyzer, NULL);
    if (analyzer.allocation_failed) {
        semantic_model_free(model);
        return NULL;
    }
    return model;
}
