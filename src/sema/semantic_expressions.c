#include "semantic_internal.h"
#include "generics.h"
#include "core_intrinsics.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void metadata_name(DiagnosticText *text, const Analyzer *analyzer, const AstProgram *unit, const AstType *type,
                          unsigned depth) {
    if (depth > 64) {
        text->failed = 1;
        return;
    }
    if (type->element_type != NULL) {
        for (unsigned i = 0;
             i < type->pointer_depth + type->outer_pointer_depth +
                     (type->borrow_kind != AST_BORROW_NONE);
             i++)
            diagnostic_append(text, "*");
        metadata_name(text, analyzer, unit, type->element_type, depth + 1);
        if (type->is_slice) diagnostic_append(text, "[]");
        else if (type->is_array)
            diagnostic_append(text, "[%zu]", type->resolved_array_length);
        return;
    }
    for (unsigned i = 0; i < type->outer_pointer_depth; i++) diagnostic_append(text, "*");
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, "(");
    for (unsigned i = 0; i < type->pointer_depth; i++) diagnostic_append(text, "*");
    if (type->kind == AST_TYPE_FUNCTION) {
        diagnostic_append(text, "func(");
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next) {
            metadata_name(text, analyzer, unit, &parameter->type, depth + 1);
            if (parameter->next) diagnostic_append(text, ",");
        }
        diagnostic_append(text, ")->");
        if (type->function_return_type)
            metadata_name(text, analyzer, unit, type->function_return_type, depth + 1);
        else diagnostic_append(text, "unknown");
        return;
    }
    size_t id = resolve_named_symbol_id(analyzer, unit, named_type_token(unit, type));
    if (id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[id];
        const AstDeclarationNode *decl = symbol->declaration;
        const AstProgram *owner = symbol->source_program;
        diagnostic_append(text, "%s.%s", owner->module_identity,
                          ast_program_lexeme(owner, decl && decl->generic_origin
                                                        ? decl->generic_origin->name_token
                                                        : symbol->name_token));
        if (decl && decl->specialization_arguments) {
            diagnostic_append(text, "<");
            for (const AstTypeArgument *a = decl->specialization_arguments; a; a = a->next) {
                metadata_name(text, analyzer, owner, &a->type, depth + 1);
                if (a->next) diagnostic_append(text, ",");
            }
            diagnostic_append(text, ">");
        }
    } else diagnostic_append(text, "%s", ast_program_lexeme(unit, type->name_token));
    if (type->is_slice) diagnostic_append(text, "[]");
    if (type->is_array) diagnostic_append(text, "[%zu]", type->resolved_array_length);
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, ")");
}

static int array_literal_element_allowed(const Analyzer *analyzer,
                                         const AstExpression *element,
                                         const AstType *element_type) {
    return expression_to_declared_type_allowed(analyzer, element,
                                               analyzer->program, element_type);
}

void set_expression_declared_type(Analyzer *analyzer,
                                         AstExpression *expression,
                                         const AstProgram *program,
                                         const AstType *type) {
    if (expression == NULL || type == NULL) return;
    expression->resolved_type = primitive_type(program, type);
    expression->resolved_borrow_kind = type->borrow_kind;
    expression->resolved_pointer_depth = type->pointer_depth +
                                         (type->borrow_kind != AST_BORROW_NONE);
    expression->resolved_outer_pointer_depth = type->outer_pointer_depth;
    expression->resolved_named_type_token = named_type_token(program, type);
    expression->resolved_named_symbol_id = resolve_named_symbol_id(
        analyzer, program, expression->resolved_named_type_token);
    expression->resolved_is_array = type->is_array;
    expression->resolved_is_slice = type->is_slice;
    expression->resolved_array_length = type->resolved_array_length;
    expression->resolved_ast_type = *type;
    expression->resolved_type_program = program;
    expression->has_resolved_ast_type = 1;
}

static AstType callable_type(Analyzer *analyzer, const SemanticSymbol *function,
                             int include_receiver) {
    AstType type = {0};
    type.kind = AST_TYPE_FUNCTION;
    type.name_token = function->declaration->first_token;
    type.array_length_token = AST_TOKEN_NONE;
    type.function_generic_parameters = function->declaration->generic_parameters;
    AstTypeArgument **tail = &type.function_parameters;
    if (include_receiver) {
        AstTypeArgument *receiver = ast_program_alloc(analyzer->program, sizeof(*receiver));
        if (receiver != NULL) {
            receiver->type = (AstType) {
                .kind = AST_TYPE_NAMED,
                .name_token = function->declaration->as.function.owner_token,
                .pointer_depth = 1,
                .array_length_token = AST_TOKEN_NONE
            };
            *tail = receiver;
            tail = &receiver->next;
        } else analyzer->allocation_failed = 1;
    }
    for (const AstParameter *parameter = function->declaration->as.function.parameters;
         parameter; parameter = parameter->next) {
        AstTypeArgument *copy = ast_program_alloc(analyzer->program, sizeof(*copy));
        if (copy == NULL) {
            analyzer->allocation_failed = 1;
            break;
        }
        copy->type = parameter->type;
        *tail = copy;
        tail = &copy->next;
    }
    AstType *result = ast_program_alloc(analyzer->program, sizeof(*result));
    if (result != NULL) {
        *result = function->declaration->as.function.return_type;
        type.function_return_type = result;
    } else analyzer->allocation_failed = 1;
    return type;
}

static size_t callable_overload_count(const Analyzer *analyzer, const char *name,
                                      size_t owner_symbol_id) {
    size_t count = 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION &&
            symbol->owner_symbol_id == owner_symbol_id &&
            symbol_matches_scope(analyzer->program, symbol, name)) count++;
    }
    return count;
}

static const AstDeclarationNode *unique_generic_callable(const Analyzer *analyzer,
                                                         const char *name,
                                                         const AstProgram **unit) {
    const AstDeclarationNode *found = NULL;
    const AstProgram *found_unit = NULL;
    for (size_t u = 0; u <= analyzer->program->owned_import_count; u++) {
        const AstProgram *candidate_unit = u == 0 ? analyzer->program : analyzer->program->owned_imports[u - 1];
        for (const AstDeclarationNode *declaration = candidate_unit->root; declaration; declaration = declaration->next) {
            if (declaration->kind != AST_DECL_FUNCTION || declaration->generic_parameters == NULL ||
                strcmp(ast_program_lexeme(candidate_unit, declaration->name_token), name)) continue;
            if (candidate_unit != analyzer->program && !declaration->is_public) continue;
            if (found != NULL) return NULL;
            found = declaration;
            found_unit = candidate_unit;
        }
    }
    if (unit) *unit = found_unit;
    return found;
}

static const AstDeclarationNode *contextual_polymorphic_declaration(
        Analyzer *analyzer, const char *name, const AstType *expected,
        const AstProgram **unit, int *ambiguous) {
    const AstDeclarationNode *found = NULL;
    const AstProgram *found_unit = NULL;
    *ambiguous = 0;
    for (size_t u = 0; u <= analyzer->program->owned_import_count; u++) {
        const AstProgram *candidate_unit = u == 0
                                               ? analyzer->program
                                               : analyzer->program->owned_imports[u - 1];
        for (const AstDeclarationNode *declaration = candidate_unit->root;
             declaration != NULL; declaration = declaration->next) {
            if (declaration->kind != AST_DECL_FUNCTION ||
                declaration->generic_parameters == NULL ||
                strcmp(ast_program_lexeme(candidate_unit,
                                           declaration->name_token), name) ||
                (candidate_unit != analyzer->program &&
                 !declaration->is_public))
                continue;
            SemanticSymbol temporary = {
                .source_program = candidate_unit,
                .declaration = declaration
            };
            AstType source = callable_type(analyzer, &temporary, 0);
            if (!ast_polymorphic_callable_compatible(
                    analyzer->program, expected, candidate_unit, &source))
                continue;
            if (found != NULL) {
                *ambiguous = 1;
                return NULL;
            }
            found = declaration;
            found_unit = candidate_unit;
        }
    }
    if (unit != NULL) *unit = found_unit;
    return found;
}

static int infer_callable_type(const AstProgram *unit, const AstDeclarationNode *declaration,
                               const AstType *pattern, const AstProgram *actual_unit,
                               const AstType *actual, AstType *arguments,
                               unsigned char *inferred) {
    size_t index = 0;
    for (const AstGenericParameter *generic = declaration->generic_parameters; generic;
         generic = generic->next, index++)
        if (pattern->kind == AST_TYPE_NAMED &&
            !strcmp(ast_program_lexeme(unit, pattern->name_token),
                    ast_program_lexeme(unit, generic->name_token))) {
            if (inferred[index])
                return ast_concrete_type_equal(actual_unit, &arguments[index], actual_unit, actual);
            arguments[index] = *actual;
            inferred[index] = 1;
            return 1;
        }
    if (pattern->kind != actual->kind || pattern->pointer_depth != actual->pointer_depth ||
        pattern->outer_pointer_depth != actual->outer_pointer_depth ||
        pattern->is_array != actual->is_array || pattern->is_slice != actual->is_slice)
        return 0;
    if (pattern->kind == AST_TYPE_FUNCTION) {
        const AstTypeArgument *p = pattern->function_parameters;
        const AstTypeArgument *a = actual->function_parameters;
        for (; p && a; p = p->next, a = a->next)
            if (!infer_callable_type(unit, declaration, &p->type, actual_unit,
                                     &a->type, arguments, inferred)) return 0;
        return p == NULL && a == NULL && pattern->function_return_type &&
               actual->function_return_type &&
               infer_callable_type(unit, declaration, pattern->function_return_type,
                                   actual_unit, actual->function_return_type,
                                   arguments, inferred);
    }
    if (pattern->kind == AST_TYPE_NAMED &&
        strcmp(ast_program_lexeme(unit, pattern->name_token),
               ast_program_lexeme(actual_unit, actual->name_token))) return 0;
    const AstTypeArgument *p = pattern->arguments;
    const AstTypeArgument *a = actual->arguments;
    for (; p && a; p = p->next, a = a->next)
        if (!infer_callable_type(unit, declaration, &p->type, actual_unit,
                                 &a->type, arguments, inferred)) return 0;
    return p == NULL && a == NULL;
}

static const SemanticSymbol *contextual_generic_callable(Analyzer *analyzer,
                                                         const AstDeclarationNode *declaration,
                                                         const AstProgram *unit,
                                                         const AstType *expected) {
    AstType arguments[DMM_MAX_TYPE_PARAMETERS] = {0};
    unsigned char inferred[DMM_MAX_TYPE_PARAMETERS] = {0};
    size_t count = 0;
    for (const AstGenericParameter *g = declaration->generic_parameters; g; g = g->next) count++;
    if (count > DMM_MAX_TYPE_PARAMETERS) return NULL;
    const AstParameter *pattern = declaration->as.function.parameters;
    const AstTypeArgument *actual = expected->function_parameters;
    for (; pattern && actual; pattern = pattern->next, actual = actual->next)
        if (!infer_callable_type(unit, declaration, &pattern->type, analyzer->program,
                                 &actual->type, arguments, inferred)) return NULL;
    if (pattern || actual || !declaration->as.function.return_type.kind ||
        !expected->function_return_type ||
        !infer_callable_type(unit, declaration, &declaration->as.function.return_type,
                             analyzer->program, expected->function_return_type,
                             arguments, inferred)) return NULL;
    for (size_t i = 0; i < count; i++) if (!inferred[i]) return NULL;
    if (!generic_bounds_satisfied(analyzer, unit, declaration, arguments)) return NULL;
    AstDeclarationNode *instance = ast_specialize_function((AstProgram *) unit, declaration,
                                                            arguments, count,
                                                            analyzer->program);
    if (!instance) return NULL;
    if (instance->resolved_symbol_id == AST_SYMBOL_NONE) {
        AstProgram *saved = analyzer->program;
        analyzer->program = (AstProgram *) unit;
        normalize_function_types(analyzer, instance);
        add_global(analyzer, instance, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
        analyzer->program = saved;
    }
    return instance->resolved_symbol_id < analyzer->model->symbol_count
               ? &analyzer->model->symbols[instance->resolved_symbol_id] : NULL;
}

static size_t specialize_callable_consumer(Analyzer *analyzer,
                                           size_t function_id,
                                           AstExpression *arguments,
                                           size_t diagnostic_token) {
    if (function_id >= analyzer->model->symbol_count) return function_id;
    const SemanticSymbol *function = &analyzer->model->symbols[function_id];
    if (function->declaration == NULL) return function_id;
    int needs_specialization = 0;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    AstExpression *argument = arguments;
    for (; parameter != NULL && argument != NULL;
         parameter = parameter->next, argument = argument->next) {
        if (parameter->type.kind != AST_TYPE_FUNCTION ||
            parameter->type.function_generic_parameters == NULL)
            continue;
        needs_specialization = 1;
        if (argument->resolved_callable == NULL ||
            argument->resolved_callable_program == NULL) {
            semantic_error(analyzer, diagnostic_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_INVALID_OPERATION,
                           "Polymorphic callable argument must have one statically known template identity");
            return function_id;
        }
    }
    if (!needs_specialization) return function_id;
    AstProgram *unit = (AstProgram *) function->source_program;
    const AstDeclarationNode *origin = function->declaration;
    AstDeclarationNode *instance = ast_specialize_callable_consumer(
        unit, origin, arguments);
    if (instance == NULL) {
        semantic_error(analyzer, diagnostic_token, ERROR_CATEGORY_SEMANTIC,
                       ERR_SEM_COMPLEXITY_LIMIT,
                       "Callable consumer specialization exceeds deterministic resource limits");
        return function_id;
    }
    if (instance->resolved_symbol_id == AST_SYMBOL_NONE) {
        AstProgram *saved = analyzer->program;
        analyzer->program = unit;
        normalize_function_types(analyzer, instance);
        add_global(analyzer, instance, SEMANTIC_SYMBOL_FUNCTION,
                   AST_TOKEN_NONE);
        analyzer->program = saved;
    }
    return instance->resolved_symbol_id;
}

typedef struct {
    const AstDeclarationNode *declaration;
    const AstProgram *program;
    int seen;
    int divergent;
} CallableReturnIdentity;

static void merge_callable_return(CallableReturnIdentity *identity,
                                  const AstDeclarationNode *declaration,
                                  const AstProgram *program) {
    if (declaration == NULL || program == NULL) {
        identity->divergent = 1;
        return;
    }
    if (!identity->seen) {
        identity->declaration = declaration;
        identity->program = program;
        identity->seen = 1;
    } else if (identity->declaration != declaration ||
               identity->program != program) {
        identity->divergent = 1;
    }
}

static void collect_callable_returns(const AstProgram *program,
                                     const AstDeclarationNode *function,
                                     const AstStatement *statement,
                                     CallableReturnIdentity *identity) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_RETURN) {
            const AstExpression *value = statement->value;
            if (value != NULL && value->resolved_callable != NULL) {
                merge_callable_return(identity, value->resolved_callable,
                                      value->resolved_callable_program);
            } else if (value != NULL && value->kind == AST_EXPR_NAME) {
                const char *name = ast_program_lexeme(program,
                                                       value->value_token);
                const AstParameter *parameter =
                    function->as.function.parameters;
                for (; parameter != NULL; parameter = parameter->next)
                    if (!strcmp(name, ast_program_lexeme(
                                          program, parameter->name_token))) {
                        const AstExpression *bound =
                            parameter->compile_time_value;
                        merge_callable_return(identity,
                            bound != NULL ? bound->resolved_callable : NULL,
                            bound != NULL
                                ? bound->resolved_callable_program : NULL);
                        break;
                    }
                if (parameter == NULL)
                    merge_callable_return(identity, NULL, NULL);
            } else {
                merge_callable_return(identity, NULL, NULL);
            }
        }
        collect_callable_returns(program, function, statement->body,
                                 identity);
        collect_callable_returns(program, function, statement->else_body,
                                 identity);
        collect_callable_returns(program, function, statement->initializer,
                                 identity);
        for (const AstMatchArm *arm = statement->match_arms; arm;
             arm = arm->next)
            collect_callable_returns(program, function, arm->body, identity);
    }
}

static void set_polymorphic_call_result(Analyzer *analyzer,
                                        AstExpression *expression,
                                        const SemanticSymbol *function) {
    if (function == NULL || function->declaration == NULL ||
        function->declaration->as.function.return_type.kind !=
            AST_TYPE_FUNCTION ||
        function->declaration->as.function.return_type.
            function_generic_parameters == NULL)
        return;
    CallableReturnIdentity identity = {0};
    collect_callable_returns(function->source_program, function->declaration,
                             function->declaration->as.function.body,
                             &identity);
    if (!identity.seen || identity.divergent) {
        semantic_error(analyzer, expression->first_token,
                       ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Polymorphic callable return paths must resolve to one template identity");
        return;
    }
    expression->resolved_callable = identity.declaration;
    expression->resolved_callable_program = identity.program;
}

static int dereference_ast_type(AstType *type) {
    if (type->borrow_kind != AST_BORROW_NONE) {
        type->borrow_kind = AST_BORROW_NONE;
        return 1;
    }
    if (type->outer_pointer_depth != 0) {
        type->outer_pointer_depth--;
        return 1;
    }
    if (type->pointer_depth != 0) {
        type->pointer_depth--;
        return 1;
    }
    return 0;
}

static AstType slice_of_type(Analyzer *analyzer, AstType element) {
    if (element.is_array || element.is_slice) {
        AstType *nested = ast_program_alloc(analyzer->program,
                                           sizeof(*nested));
        if (nested == NULL) {
            analyzer->allocation_failed = 1;
            element.invalid_substitution = 1;
            return element;
        }
        *nested = element;
        element.element_type = nested;
        element.borrow_kind = AST_BORROW_NONE;
        element.pointer_depth = 0;
        element.outer_pointer_depth = 0;
        element.is_array = 0;
        element.is_slice = 0;
        element.array_length_token = AST_TOKEN_NONE;
        element.resolved_array_length = 0;
    }
    element.is_slice = 1;
    return element;
}

static void contextualize_direct_call_literals(Analyzer *analyzer,
                                               AstExpression *call) {
    if (call == NULL || call->kind != AST_EXPR_CALL || call->left == NULL ||
        call->left->kind != AST_EXPR_NAME) return;
    const char *name = ast_program_lexeme(analyzer->program,
                                         call->left->value_token);
    const SemanticSymbol *function = scoped_find_global(
        analyzer->model, analyzer->program, name, SEMANTIC_SYMBOL_FUNCTION);
    if (function == NULL || function->declaration == NULL ||
        function->declaration->generic_parameters != NULL) return;
    AstExpression *argument = call->arguments;
    const AstParameter *parameter =
        function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
         argument = argument->next, parameter = parameter->next)
        if (argument->kind == AST_EXPR_ARRAY_LITERAL ||
            argument->kind == AST_EXPR_CONTROL)
            argument->allocated_type = parameter->type;
}

static const char *aggregate_base_name(const SemanticSymbol *symbol) {
    if (symbol == NULL || symbol->declaration == NULL) return "";
    const AstDeclarationNode *declaration = symbol->declaration;
    const AstDeclarationNode *origin = declaration->generic_origin != NULL
                                           ? declaration->generic_origin
                                           : declaration;
    return ast_program_lexeme(symbol->source_program, origin->name_token);
}

static const SemanticSymbol *unique_static_method(Analyzer *analyzer,
                                                   size_t owner_symbol_id,
                                                   const char *name,
                                                   const AstType *argument,
                                                   int *ambiguous) {
    const SemanticSymbol *selected = NULL;
    if (ambiguous != NULL) *ambiguous = 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *candidate = &analyzer->model->symbols[i];
        if (candidate->kind != SEMANTIC_SYMBOL_FUNCTION ||
            candidate->owner_symbol_id != owner_symbol_id ||
            candidate->declaration == NULL ||
            !candidate->declaration->as.function.is_static ||
            !same_name(candidate->source_program, candidate->name_token, name))
            continue;
        const AstParameter *parameter =
            candidate->declaration->as.function.parameters;
        if (parameter == NULL || parameter->next != NULL) continue;
        if (argument != NULL &&
            !ast_concrete_type_equal(candidate->source_program,
                                     &parameter->type,
                                     analyzer->program, argument)) {
            DataType expected = primitive_type(candidate->source_program,
                                               &parameter->type);
            DataType actual = primitive_type(analyzer->program, argument);
            if (expected == TYPE_UNKNOWN || expected != actual ||
                parameter->type.pointer_depth != argument->pointer_depth ||
                parameter->type.outer_pointer_depth !=
                    argument->outer_pointer_depth ||
                parameter->type.is_array != argument->is_array ||
                parameter->type.is_slice != argument->is_slice)
                continue;
        }
        if (selected != NULL) {
            if (ambiguous != NULL) *ambiguous = 1;
            return NULL;
        }
        selected = candidate;
    }
    return selected;
}

static size_t named_variant(Analyzer *analyzer, size_t enum_symbol_id,
                            const char *name) {
    if (enum_symbol_id >= analyzer->model->symbol_count) return AST_SYMBOL_NONE;
    const SemanticSymbol *enumeration =
        &analyzer->model->symbols[enum_symbol_id];
    if (enumeration->kind != SEMANTIC_SYMBOL_ENUM ||
        enumeration->declaration == NULL) return AST_SYMBOL_NONE;
    for (const AstEnumValue *value = enumeration->declaration->as.enum_decl.values;
         value != NULL; value = value->next)
        if (same_name(enumeration->source_program, value->name_token, name))
            return value->resolved_symbol_id;
    return AST_SYMBOL_NONE;
}

static const SemanticSymbol *declared_static_method(
    Analyzer *analyzer, const SemanticSymbol *owner, const char *name,
    int *ambiguous) {
    if (owner == NULL || owner->declaration == NULL) return NULL;
    const AstDeclarationNode *methods =
        owner->kind == SEMANTIC_SYMBOL_STRUCT
            ? owner->declaration->as.struct_decl.methods
            : owner->kind == SEMANTIC_SYMBOL_ENUM
                  ? owner->declaration->as.enum_decl.methods : NULL;
    const SemanticSymbol *selected = NULL;
    for (const AstDeclarationNode *method = methods; method != NULL;
         method = method->next) {
        if (!method->as.function.is_static ||
            strcmp(ast_program_lexeme(owner->source_program,
                                      method->name_token), name) ||
            method->resolved_symbol_id >= analyzer->model->symbol_count)
            continue;
        if (selected != NULL) {
            if (ambiguous != NULL) *ambiguous = 1;
            return NULL;
        }
        selected =
            &analyzer->model->symbols[method->resolved_symbol_id];
    }
    return selected;
}

static void analyze_propagation(Analyzer *analyzer, AstExpression *expression) {
    AstExpression *operand = expression->left;
    size_t question = expression->value_token;
    if (analyzer->current_function == NULL || analyzer->in_destructor ||
        analyzer->in_defer_closure) {
        semantic_error(analyzer, question, ERROR_CATEGORY_SEMANTIC,
                       ERR_TYPE_INVALID_OPERATION,
                       "Error propagation with '?' is only allowed directly in functions and methods");
        return;
    }
    if (operand == NULL ||
        operand->resolved_named_symbol_id >= analyzer->model->symbol_count) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Operand of '?' does not provide a Propagate specialization");
        return;
    }
    const SemanticSymbol *operand_type =
        &analyzer->model->symbols[operand->resolved_named_symbol_id];
    int ambiguous = 0;
    const SemanticSymbol *branch = unique_static_method(
        analyzer, operand_type->id, "branch",
        operand->has_resolved_ast_type ? &operand->resolved_ast_type : NULL,
        &ambiguous);
    if (branch == NULL) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       ambiguous
                           ? "Operand of '?' has multiple matching Propagate contracts"
                           : "Operand of '?' does not provide static branch(Self)");
        return;
    }
    const AstType *branch_result = &branch->declaration->as.function.return_type;
    size_t propagation_id = resolve_named_symbol_id(
        analyzer, branch->source_program, branch_result->name_token);
    const SemanticSymbol *propagation =
        propagation_id < analyzer->model->symbol_count
            ? &analyzer->model->symbols[propagation_id] : NULL;
    const AstTypeArgument *contract_arguments =
        propagation != NULL && propagation->declaration != NULL
            ? propagation->declaration->specialization_arguments : NULL;
    if (branch_result->kind != AST_TYPE_NAMED || propagation == NULL ||
        strcmp(aggregate_base_name(propagation), "Propagation") ||
        contract_arguments == NULL || contract_arguments->next == NULL ||
        contract_arguments->next->next != NULL) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Propagate.branch must return Propagation<Output,Residual>");
        return;
    }
    AstType output = contract_arguments->type;
    AstType residual = contract_arguments->next->type;
    size_t continue_id = named_variant(analyzer, propagation_id, "Continue");
    size_t break_id = named_variant(analyzer, propagation_id, "Break");
    if (continue_id == AST_SYMBOL_NONE || break_id == AST_SYMBOL_NONE) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INVALID_OPERATION,
                       "Propagation result must define Continue and Break variants");
        return;
    }

    const AstType *return_type =
        &analyzer->current_function->as.function.return_type;
    size_t return_id = resolve_named_symbol_id(analyzer, analyzer->program,
                                                return_type->name_token);
    if (return_id >= analyzer->model->symbol_count) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INCOMPATIBLE_TYPES,
                       "The enclosing return type cannot accept a residual");
        return;
    }
    const SemanticSymbol *return_symbol = &analyzer->model->symbols[return_id];
    const char *operand_base = aggregate_base_name(operand_type);
    const char *return_base = aggregate_base_name(return_symbol);
    if ((!strcmp(operand_base, "Option") && strcmp(return_base, "Option")) ||
        (!strcmp(operand_base, "Result") && strcmp(return_base, "Result"))) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INCOMPATIBLE_TYPES,
                       "Option and Result residuals cannot be propagated across standard container kinds");
        return;
    }
    const AstProgram *saved_program = analyzer->program;
    analyzer->program = (AstProgram *) branch->source_program;
    const SemanticSymbol *from = unique_static_method(
        analyzer, return_id, "fromResidual", &residual, &ambiguous);
    analyzer->program = (AstProgram *) saved_program;
    if (from == NULL && !ambiguous && strcmp(return_base, "Result"))
        from = declared_static_method(analyzer, return_symbol,
                                      "fromResidual", &ambiguous);
    size_t return_variant = AST_SYMBOL_NONE;
    if (from == NULL && !ambiguous && !strcmp(return_base, "Result") &&
        return_symbol->declaration != NULL) {
        const AstTypeArgument *return_arguments =
            return_symbol->declaration->specialization_arguments;
        if (return_arguments != NULL && return_arguments->next != NULL) {
            const AstType *error_type = &return_arguments->next->type;
            size_t error_id = resolve_named_symbol_id(
                analyzer, return_symbol->source_program,
                error_type->name_token);
            analyzer->program = (AstProgram *) branch->source_program;
            from = unique_static_method(analyzer, error_id, "fromResidual",
                                        &residual, &ambiguous);
            analyzer->program = (AstProgram *) saved_program;
            if (from == NULL && !ambiguous &&
                error_id < analyzer->model->symbol_count)
                from = declared_static_method(
                    analyzer, &analyzer->model->symbols[error_id],
                    "fromResidual", &ambiguous);
            if (from != NULL)
                return_variant = named_variant(analyzer, return_id, "Err");
        }
    }
    if (from == NULL) {
        semantic_error(analyzer, question, ERROR_CATEGORY_TYPE,
                       ERR_TYPE_INCOMPATIBLE_TYPES,
                       ambiguous
                           ? "Return type has multiple matching FromResidual conversions"
                           : "Return type does not provide FromResidual for this residual");
        return;
    }
    expression->propagation_branch_symbol_id = branch->id;
    expression->propagation_continue_symbol_id = continue_id;
    expression->propagation_break_symbol_id = break_id;
    expression->propagation_from_residual_symbol_id = from->id;
    expression->propagation_return_variant_symbol_id = return_variant;
    expression->propagation_output_type = output;
    expression->propagation_residual_type = residual;
    expression->propagation_contract_program = propagation->source_program;
    set_expression_declared_type(analyzer, expression,
                                 propagation->source_program,
                                 &output);
}

void analyze_expression(Analyzer *analyzer, AstExpression *expression) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_CONTROL) {
        analyze_control_expression(analyzer, expression);
        return;
    }
    if (expression->kind == AST_EXPR_TYPE_INFO && !expression->left &&
        !expression->allocated_type.pointer_depth && !expression->allocated_type.outer_pointer_depth &&
        !expression->allocated_type.is_array && !expression->allocated_type.is_slice && !expression->allocated_type.
        arguments) {
        const SemanticSymbol *global = scoped_find_global(analyzer->model, analyzer->program,
                                                          ast_program_lexeme(
                                                              analyzer->program, expression->value_token),
                                                          SEMANTIC_SYMBOL_VARIABLE);
        if (!global)
            global = scoped_find_global(analyzer->model, analyzer->program,
                                        ast_program_lexeme(analyzer->program, expression->value_token),
                                        SEMANTIC_SYMBOL_CONSTANT);
        if (global || find_local(analyzer, expression->value_token)) {
            expression->kind = AST_EXPR_NAME;
            expression->allocated_type = (AstType)
            {
                .kind = AST_TYPE_INFERRED,.name_token = AST_TOKEN_NONE
            };
        }
    }
    if (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind == AST_EXPR_NAME &&
        !find_local(analyzer, expression->left->value_token)) {
        const char *alias = ast_program_lexeme(analyzer->program, expression->left->value_token);
        for (AstDeclarationNode *d = analyzer->program->root; d; d = d->next)
            if (d->kind == AST_DECL_IMPORT)
                for (AstImportPath *p = d->as.import_decl.paths; p; p = p->next)
                    if (p->alias && !strcmp(alias, p->alias)) {
                        char qualified[2048];
                        snprintf(qualified, sizeof(qualified), "%s.%s", alias,
                                 ast_program_lexeme(analyzer->program, expression->value_token));
                        expression->value_token = concrete_token(analyzer, TOKEN_IDENTIFIER, qualified);
                        expression->kind = AST_EXPR_NAME;
                        expression->left = NULL;
                    }
    }
    analyze_expression(analyzer, expression->left);
    analyze_expression(analyzer, expression->right);
    contextualize_direct_call_literals(analyzer, expression);
    if (expression->kind == AST_EXPR_ARRAY_LITERAL &&
        expression->allocated_type.kind != AST_TYPE_INFERRED &&
        (expression->allocated_type.is_array || expression->allocated_type.is_slice)) {
        AstType element_type = ast_type_element(&expression->allocated_type);
        for (AstExpression *argument = expression->arguments;
             argument != NULL; argument = argument->next)
            if (argument->kind == AST_EXPR_ARRAY_LITERAL)
                argument->allocated_type = element_type;
    }
    for (AstExpression *argument = expression->arguments; argument != NULL; argument = argument->next)
        analyze_expression(analyzer, argument);

    if (expression->kind == AST_EXPR_PROPAGATE) {
        analyze_propagation(analyzer, expression);
        return;
    }

    if (expression->kind == AST_EXPR_ENUM_ACCESS) return;

    if (expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        const AstExpression *left = expression->left;
        if (left && left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
            expression->resolved_symbol_id = left->resolved_symbol_id;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        } else if (left) {
            const AstEnumValue *variant = find_enum_value_by_symbol(analyzer, left->resolved_named_symbol_id,
                                                                    expression->value_token);
            expression->resolved_symbol_id = variant ? variant->resolved_symbol_id : AST_SYMBOL_NONE;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        }
        return;
    }
    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_borrow_kind = AST_BORROW_NONE;
    expression->resolved_pointer_depth = 0;
    expression->resolved_outer_pointer_depth = 0;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_is_array = 0;
    expression->resolved_is_slice = 0;
    expression->has_resolved_ast_type = 0;
    expression->resolved_type_program = NULL;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_callable = NULL;
    expression->resolved_callable_program = NULL;
    expression->owns_slice_backing = 0;
    if (expression->kind == AST_EXPR_MEMBER && expression->left && same_name(
            analyzer->program, expression->value_token, "type")) {
        if (expression->left->kind == AST_EXPR_TYPE_INFO ||
            (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
             (analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
              analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM)))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Types expose name, size and align directly; .type is for values");
        expression->kind = AST_EXPR_TYPE_INFO;
        expression->allocated_type = inferred_argument_type(analyzer, expression->left);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Value type metadata requires a known static type");
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_TYPE_INFO) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Type metadata requires a known type");
        expression->resolved_type = TYPE_VOID; /* Compile-time entity, never an IR value. */
    } else if (expression->kind == AST_EXPR_TYPE_PROPERTY ||
               (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind ==
                AST_EXPR_TYPE_INFO)) {
        expression->kind = AST_EXPR_TYPE_PROPERTY;
        const char *property = ast_program_lexeme(analyzer->program, expression->value_token);
        AstType *type = &expression->left->allocated_type;
        if (!strcmp(property, "name")) {
            DiagnosticText text = {0};
            metadata_name(&text, analyzer, analyzer->program, type, 0);
            if (text.failed) analyzer->allocation_failed = 1;
            else expression->folded_constant = (AstToken)
            {
                .type = TOKEN_STRING_LITERAL,
                .lexeme = string_interner_intern(analyzer->program->strings, text.text),.span = expression->span
            };
            free(text.text);
            expression->resolved_type = TYPE_STRING;
        } else if (!strcmp(property, "size") || !strcmp(property, "align")) {
            DataType primitive = primitive_type(analyzer->program, type);
            int is_void = primitive == TYPE_VOID && !type->pointer_depth && !type->outer_pointer_depth && !type->
                          is_array && !type->is_slice;
            size_t size = is_void ? 0 : layout_size(analyzer, type, 0);
            if ((!size && !is_void) || size > INT64_MAX)
                semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Type metadata layout requires a complete, non-recursive sized type");
            size_t align = is_void
                               ? 1
                               : (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->
                                  is_slice && primitive != TYPE_UNKNOWN)
                                     ? data_type_bytes(primitive)
                                     : 8;
            char text[32];
            snprintf(text, sizeof(text), "%zu", !strcmp(property, "size") ? size : align);
            expression->folded_constant = (AstToken)
            {
                .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
                span = expression->span
            };
            expression->resolved_type = TYPE_USIZE;
        } else semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_FIELD_NOT_FOUND,
                              "Unknown type metadata property; expected name, size or align");
    } else if (expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        size_t size = layout_size(analyzer, &expression->allocated_type, 0);
        expression->resolved_type = TYPE_USIZE;
        if (!size || size > (size_t) INT64_MAX) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Layout query requires a complete, non-recursive, sized non-void type");
            return;
        }
        const AstType *type = &expression->allocated_type;
        DataType primitive = primitive_type(analyzer->program, type);
        size_t alignment = (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->is_slice &&
                            primitive != TYPE_UNKNOWN)
                               ? data_type_bytes(primitive)
                               : 8;
        char text[32];
        snprintf(text, sizeof(text), "%zu", expression->kind == AST_EXPR_SIZEOF ? size : alignment);
        expression->folded_constant = (AstToken)
        {
            .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
            span = expression->span
        };
    } else if (expression->kind == AST_EXPR_SLICE) {
        const AstExpression *data = expression->left;
        if (data) {
            if (data->has_resolved_ast_type) {
                AstType element = data->resolved_ast_type;
                (void) dereference_ast_type(&element);
                AstType slice = slice_of_type(analyzer, element);
                set_expression_declared_type(
                    analyzer, expression,
                    data->resolved_type_program != NULL
                        ? data->resolved_type_program : analyzer->program,
                    &slice);
            } else {
                expression->resolved_type = data->resolved_type;
                expression->resolved_named_type_token =
                        data->resolved_named_type_token;
                expression->resolved_named_symbol_id =
                        data->resolved_named_symbol_id;
                expression->resolved_pointer_depth =
                        data->resolved_pointer_depth
                            ? data->resolved_pointer_depth - 1 : 0;
                expression->resolved_is_slice = 1;
            }
        }
    } else if (expression->kind == AST_EXPR_ARRAY_LITERAL) {
        AstType expected = expression->allocated_type;
        normalize_generic_type(analyzer, &expected, 0);
        validate_array_shape(analyzer, &expected);
        if (expected.kind == AST_TYPE_INFERRED ||
            (!expected.is_array && !expected.is_slice) ||
            expected.outer_pointer_depth != 0) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_UNKNOWN,
                           "Array literal requires an expected fixed-array or slice type");
            return;
        }

        size_t pattern_count = 0;
        for (const AstExpression *element = expression->arguments; element != NULL;
             element = element->next)
            pattern_count++;
        size_t element_count = pattern_count;
        if (expression->right != NULL) {
            if (!constant_expression_allowed(analyzer, expression->right) ||
                !fold_constant(analyzer, expression->right, TYPE_USIZE)) {
                semantic_error(analyzer, expression->right->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Array literal repetition count must be a positive compile-time integer constant");
                element_count = 0;
            } else {
                const char *text = expression->right->folded_constant.lexeme;
                char *end = NULL;
                unsigned long long value = strtoull(text ? text : "", &end, 10);
                if (text == NULL || end == text || *end != '\0' || value == 0 ||
                    value > SIZE_MAX) {
                    semantic_error(analyzer, expression->right->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Array literal repetition count must be a positive compile-time integer constant");
                    element_count = 0;
                } else element_count = (size_t) value;
            }
        }
        if (expected.is_array && element_count != expected.resolved_array_length)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_INCOMPATIBLE_TYPES,
                           "Array literal element count must match the fixed-array length");

        AstType element_type = ast_type_element(&expected);
        size_t interface_symbol = resolve_named_symbol_id(
            analyzer, analyzer->program,
            named_type_token(analyzer->program, &element_type));
        int interface_elements = interface_symbol < analyzer->model->symbol_count &&
            analyzer->model->symbols[interface_symbol].kind == SEMANTIC_SYMBOL_INTERFACE;
        for (AstExpression *element = expression->arguments; element != NULL;
             element = element->next) {
            if (interface_elements &&
                semantic_implements_interface(analyzer->model, interface_symbol,
                                              element->resolved_named_symbol_id) &&
                semantic_expression_is_move_only(analyzer, element)) {
                semantic_error(analyzer, element->first_token, ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_INVALID_DECLARATION,
                               "Cannot erase a move-only value into a copyable interface");
            } else if (!array_literal_element_allowed(analyzer, element, &element_type))
                conversion_error(analyzer, element, analyzer->program, &element_type,
                                 NULL, "Cannot implicitly convert array literal element");
        }
        if (expression->right != NULL && element_count > pattern_count)
            for (AstExpression *element = expression->arguments; element != NULL;
                 element = element->next)
                if (semantic_expression_is_move_only(analyzer, element)) {
                    semantic_error(analyzer, element->first_token,
                                   ERROR_CATEGORY_SEMANTIC,
                                   ERR_SEM_INVALID_DECLARATION,
                                   "Array repetition cannot duplicate a move-only element");
                    break;
                }

        set_expression_declared_type(analyzer, expression, analyzer->program,
                                     &expected);
        expression->literal_element_count = element_count;
        expression->owns_slice_backing = expected.is_slice;
        expression->resolved_named_type_token = named_type_token(analyzer->program, &expected);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(
            analyzer, analyzer->program, expression->resolved_named_type_token);
        expression->allocated_type = expected;
    } else if (expression->kind == AST_EXPR_LITERAL) {
        TokenType token = expression->value_token < analyzer->program->token_count
                              ? analyzer->program->tokens[expression->value_token].type
                              : TOKEN_ERROR;
        if (token == TOKEN_NUMBER) {
            unsigned long long value = strtoull(ast_program_lexeme(analyzer->program, expression->value_token), NULL,
                                                10);
            expression->resolved_type = value > INT64_MAX ? TYPE_U64 : value > INT32_MAX ? TYPE_I64 : TYPE_INT;
        } else if (token == TOKEN_FLOAT_LITERAL) expression->resolved_type = TYPE_DOUBLE;
        else if (token == TOKEN_CHAR_LITERAL) expression->resolved_type = TYPE_CHAR;
        else if (token == TOKEN_STRING_LITERAL) expression->resolved_type = TYPE_STRING;
    } else if (expression->kind == AST_EXPR_NAME) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        TokenType name_type = expression->value_token < analyzer->program->token_count
                                  ? analyzer->program->tokens[expression->value_token].type
                                  : TOKEN_ERROR;
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            expression->resolved_type = TYPE_BIT;
        } else if (name_type >= TOKEN_TYPE_INT && name_type <= TOKEN_TYPE_NEVER) {
            AstType type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            expression->resolved_type = primitive_type(analyzer->program, &type);
        } else if (is_builtin_name(name)) {
            expression->resolved_type = builtin_result_type(name);
            if (strcmp(name, "malloc") == 0) expression->resolved_pointer_depth = 1;
            const CoreIntrinsic *core = core_intrinsic_find(name);
            if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
        } else {
            if (expression->explicit_generic_reference && expression->allocated_type.arguments) {
                const SemanticSymbol *specialized = explicit_generic_function(analyzer, name, expression);
                if (specialized != NULL) {
                    expression->resolved_symbol_id = specialized->id;
                    AstType callable = callable_type(analyzer, specialized, 0);
                    set_expression_declared_type(analyzer, expression,
                                                 specialized->source_program, &callable);
                    expression->resolved_callable = specialized->declaration;
                    expression->resolved_callable_program = specialized->source_program;
                    return;
                }
            }
            if (expression->allocated_type.kind == AST_TYPE_FUNCTION &&
                expression->allocated_type.function_generic_parameters != NULL) {
                const AstProgram *generic_unit = NULL;
                int ambiguous = 0;
                const AstDeclarationNode *generic =
                    contextual_polymorphic_declaration(
                        analyzer, name, &expression->allocated_type,
                        &generic_unit, &ambiguous);
                if (ambiguous)
                    semantic_error(analyzer, expression->first_token,
                                   ERROR_CATEGORY_TYPE,
                                   ERR_TYPE_INVALID_OPERATION,
                                   "Expected polymorphic callable type does not select one overload");
                if (generic != NULL) {
                    SemanticSymbol temporary = {
                        .source_program = generic_unit,
                        .declaration = generic
                    };
                    AstType callable = callable_type(analyzer, &temporary, 0);
                    set_expression_declared_type(analyzer, expression,
                                                 generic_unit, &callable);
                    expression->resolved_callable = generic;
                    expression->resolved_callable_program = generic_unit;
                    return;
                }
            } else if (expression->allocated_type.kind == AST_TYPE_FUNCTION) {
                const AstProgram *generic_unit = NULL;
                const AstDeclarationNode *generic = unique_generic_callable(analyzer, name, &generic_unit);
                const SemanticSymbol *specialized = generic == NULL ? NULL :
                    contextual_generic_callable(analyzer, generic, generic_unit,
                                                &expression->allocated_type);
                if (specialized != NULL) {
                    expression->resolved_symbol_id = specialized->id;
                    AstType callable = callable_type(analyzer, specialized, 0);
                    set_expression_declared_type(analyzer, expression,
                                                 specialized->source_program, &callable);
                    expression->resolved_callable = specialized->declaration;
                    expression->resolved_callable_program = specialized->source_program;
                    return;
                }
            }
            const LocalSymbol *local = find_local(analyzer, expression->value_token);
            if (local != NULL) {
                expression->resolved_symbol_id = local->symbol_id;
                if (local->has_resolved_ast_type)
                    set_expression_declared_type(analyzer, expression,
                                                 local->resolved_type_program,
                                                 &local->resolved_ast_type);
                else {
                    expression->resolved_type = local->resolved_type;
                    expression->resolved_borrow_kind = local->resolved_borrow_kind;
                    expression->resolved_pointer_depth = local->resolved_pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            local->resolved_outer_pointer_depth;
                    expression->resolved_named_type_token =
                            local->resolved_named_type_token;
                    expression->resolved_named_symbol_id =
                            local->resolved_named_symbol_id;
                    expression->resolved_is_array = local->resolved_is_array;
                    expression->resolved_is_slice = local->resolved_is_slice;
                }
                if (local->symbol_id < analyzer->model->symbol_count) {
                    const SemanticSymbol *local_symbol = &analyzer->model->symbols[local->symbol_id];
                    const AstExpression *initializer = local_symbol->node;
                    if (initializer != NULL) {
                        expression->resolved_callable = initializer->resolved_callable;
                        expression->resolved_callable_program = initializer->resolved_callable_program;
                    }
                }
            } else {
                const AstField *implicit_field = analyzer->current_owner_token == AST_TOKEN_NONE
                                                     ? NULL
                                                     : find_field(analyzer, analyzer->current_owner_token,
                                                                  expression->value_token);
                const SemanticSymbol *structure = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                     SEMANTIC_SYMBOL_STRUCT);
                const SemanticSymbol *enumeration = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                       SEMANTIC_SYMBOL_ENUM);
                const SemanticSymbol *interface = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                     SEMANTIC_SYMBOL_INTERFACE);
                const SemanticSymbol *function = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_FUNCTION);
                const SemanticSymbol *constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_CONSTANT);
                if (!constant) constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                             SEMANTIC_SYMBOL_VARIABLE);
                if (implicit_field != NULL) {
                    expression->resolved_symbol_id = implicit_field->resolved_symbol_id;
                    set_expression_declared_type(analyzer, expression,
                                                 analyzer->program,
                                                 &implicit_field->type);
                } else if (structure != NULL) {
                    expression->resolved_symbol_id = structure->id;
                    expression->resolved_named_type_token = structure->name_token;
                    expression->resolved_named_symbol_id = structure->id;
                } else if (enumeration != NULL) {
                    expression->resolved_symbol_id = enumeration->id;
                    expression->resolved_named_type_token = enumeration->name_token;
                    expression->resolved_named_symbol_id = enumeration->id;
                } else if (interface != NULL) {
                    expression->resolved_symbol_id = interface->id;
                    expression->resolved_named_type_token = interface->name_token;
                    expression->resolved_named_symbol_id = interface->id;
                } else if (function != NULL) {
                    if (expression->direct_call_target) {
                        expression->resolved_symbol_id = function->id;
                        expression->resolved_type = primitive_type(function->source_program,
                                                                   &function->declared_type);
                        expression->resolved_pointer_depth = function->declared_type.pointer_depth;
                        expression->resolved_outer_pointer_depth =
                            function->declared_type.outer_pointer_depth;
                        return;
                    }
                    if (expression->allocated_type.kind == AST_TYPE_INFERRED) {
                        const AstProgram *generic_unit = NULL;
                        const AstDeclarationNode *generic =
                            unique_generic_callable(analyzer, name,
                                                    &generic_unit);
                        if (generic != NULL) {
                            SemanticSymbol temporary = {
                                .source_program = generic_unit,
                                .declaration = generic
                            };
                            AstType callable = callable_type(analyzer,
                                                             &temporary, 0);
                            set_expression_declared_type(analyzer, expression,
                                                         generic_unit,
                                                         &callable);
                            expression->resolved_symbol_id = AST_SYMBOL_NONE;
                            expression->resolved_callable = generic;
                            expression->resolved_callable_program =
                                generic_unit;
                            return;
                        }
                    }
                    if (expression->allocated_type.kind == AST_TYPE_FUNCTION) {
                        const SemanticSymbol *match = NULL;
                        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
                            const SemanticSymbol *candidate = &analyzer->model->symbols[i];
                            if (candidate->kind != SEMANTIC_SYMBOL_FUNCTION || candidate->owner_symbol_id != AST_SYMBOL_NONE ||
                                !symbol_matches_scope(analyzer->program, candidate, name) || !candidate->declaration) continue;
                            AstType candidate_type = callable_type(analyzer, candidate, 0);
                            if (ast_concrete_type_equal(analyzer->program, &expression->allocated_type,
                                                        candidate->source_program, &candidate_type)) {
                                if (match != NULL) { match = NULL; break; }
                                match = candidate;
                            }
                        }
                        if (match != NULL) function = match;
                    }
                    expression->resolved_symbol_id = function->id;
                    if (callable_overload_count(analyzer, name, AST_SYMBOL_NONE) == 1 ||
                        expression->allocated_type.kind == AST_TYPE_FUNCTION) {
                        AstType callable = callable_type(analyzer, function, 0);
                        set_expression_declared_type(analyzer, expression,
                                                     function->source_program, &callable);
                        expression->resolved_callable = function->declaration;
                        expression->resolved_callable_program = function->source_program;
                    }
                } else if (constant != NULL) {
                    size_t constant_id = constant->id;
                    if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && !constant->declaration->
                        semantic_body_checked) {
                        AstProgram *saved = analyzer->program;
                        analyzer->program = (AstProgram *) constant->source_program;
                        analyze_constant_declaration(analyzer, (AstDeclarationNode *) constant->declaration);
                        analyzer->program = saved;
                        constant = &analyzer->model->symbols[constant_id];
                    } else if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && constant->
                               declaration->semantic_body_checked == 2) {
                        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE,
                                       ERR_TYPE_INVALID_OPERATION, "Constant initializer contains a dependency cycle");
                    }
                    expression->resolved_symbol_id = constant->id;
                    const AstExpression *initializer =
                        constant->declaration != NULL
                            ? constant->declaration->as.constant.value : NULL;
                    if (initializer != NULL &&
                        initializer->has_resolved_ast_type)
                        set_expression_declared_type(
                            analyzer, expression,
                            initializer->resolved_type_program != NULL
                                ? initializer->resolved_type_program
                                : constant->source_program,
                            &initializer->resolved_ast_type);
                    else if (constant->declared_type.kind != AST_TYPE_INFERRED)
                        set_expression_declared_type(analyzer, expression,
                                                     constant->source_program,
                                                     &constant->declared_type);
                    else {
                        expression->resolved_type = constant->resolved_type;
                        expression->resolved_borrow_kind =
                                constant->resolved_borrow_kind;
                        expression->resolved_pointer_depth =
                                constant->resolved_pointer_depth;
                        expression->resolved_outer_pointer_depth =
                                constant->resolved_outer_pointer_depth;
                        expression->resolved_named_type_token =
                                constant->resolved_named_type_token;
                        expression->resolved_named_symbol_id =
                                constant->resolved_named_symbol_id;
                        expression->resolved_is_array =
                                constant->resolved_is_array;
                        expression->resolved_is_slice =
                                constant->resolved_is_slice;
                    }
                    if (initializer != NULL) {
                        expression->resolved_callable =
                            initializer->resolved_callable;
                        expression->resolved_callable_program =
                            initializer->resolved_callable_program;
                    }
                } else {
                    const AstProgram *generic_unit = NULL;
                    const AstDeclarationNode *generic = unique_generic_callable(analyzer, name, &generic_unit);
                    if (generic != NULL && !expression->direct_call_target) {
                        SemanticSymbol temporary = {
                            .source_program = generic_unit,
                            .declaration = generic
                        };
                        AstType callable = callable_type(analyzer, &temporary, 0);
                        set_expression_declared_type(analyzer, expression, generic_unit, &callable);
                        expression->resolved_callable = generic;
                        expression->resolved_callable_program = generic_unit;
                    }
                }
            }
        }
    } else if (expression->kind == AST_EXPR_UNARY) {
        if (expression->operator_type == TOKEN_BANG) expression->resolved_type = TYPE_BIT;
        else if (expression->right != NULL) {
            expression->resolved_type = expression->right->resolved_type;
            expression->resolved_borrow_kind = expression->right->resolved_borrow_kind;
            expression->resolved_pointer_depth = expression->right->resolved_pointer_depth;
            expression->resolved_outer_pointer_depth =
                    expression->right->resolved_outer_pointer_depth;
            expression->resolved_named_type_token = expression->right->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->right->resolved_named_symbol_id;
            expression->resolved_is_array = expression->right->resolved_is_array;
            expression->resolved_is_slice = expression->right->resolved_is_slice;
            expression->resolved_ast_type = expression->right->resolved_ast_type;
            expression->resolved_type_program = expression->right->resolved_type_program;
            expression->has_resolved_ast_type = expression->right->has_resolved_ast_type;
            if (expression->operator_type == TOKEN_AMPERSAND) {
                expression->resolved_borrow_kind = expression->mutable_borrow
                                                       ? AST_BORROW_MUTABLE
                                                       : AST_BORROW_IMMUTABLE;
                if (expression->resolved_is_array || expression->resolved_is_slice)
                    expression->resolved_outer_pointer_depth++;
                else expression->resolved_pointer_depth++;
                if (expression->has_resolved_ast_type) {
                    expression->resolved_ast_type.borrow_kind =
                        expression->resolved_borrow_kind;
                }
            } else if (expression->operator_type == TOKEN_STAR) {
                expression->resolved_borrow_kind = AST_BORROW_NONE;
                if (expression->resolved_outer_pointer_depth > 0)
                    expression->resolved_outer_pointer_depth--;
                else if (expression->resolved_pointer_depth > 0)
                    expression->resolved_pointer_depth--;
                if (expression->has_resolved_ast_type) {
                    if (expression->resolved_ast_type.borrow_kind !=
                        AST_BORROW_NONE)
                        expression->resolved_ast_type.borrow_kind =
                            AST_BORROW_NONE;
                    else if (expression->resolved_ast_type.outer_pointer_depth >
                             0)
                        expression->resolved_ast_type.outer_pointer_depth--;
                    else if (expression->resolved_ast_type.pointer_depth > 0)
                        expression->resolved_ast_type.pointer_depth--;
                }
            }
        }
    } else if (expression->kind == AST_EXPR_BINARY) {
        TokenType operation = expression->operator_type;
        if ((operation >= TOKEN_EQUAL_EQUAL && operation <= TOKEN_GREATER_EQUAL) ||
            operation == TOKEN_AMP_AMP || operation == TOKEN_PIPE_PIPE) {
            expression->resolved_type = TYPE_BIT;
        } else if (expression->left != NULL && expression->right != NULL) {
            if (operation == TOKEN_PLUS && (expression->left->resolved_type == TYPE_STRING ||
                                            expression->right->resolved_type == TYPE_STRING))
                expression->resolved_type = TYPE_STRING;
            else if (expression->left->resolved_type == TYPE_FLOAT &&
                     expression->right->resolved_type == TYPE_DOUBLE &&
                     expression->right->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else if (expression->right->resolved_type == TYPE_FLOAT &&
                     expression->left->resolved_type == TYPE_DOUBLE &&
                     expression->left->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else
                expression->resolved_type = promoted_numeric(expression->left->resolved_type,
                                                             expression->right->resolved_type);
        }
    } else if (expression->kind == AST_EXPR_CAST) {
        validate_array_shape(analyzer, &expression->allocated_type);
        const AstType *cast_type = &expression->allocated_type;
        set_expression_declared_type(analyzer, expression, analyzer->program,
                                     cast_type);
    } else if (expression->kind == AST_EXPR_FREE) {
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_CALL) {
        if (expression->left != NULL && expression->left->has_resolved_ast_type &&
            expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION &&
            expression->left->kind != AST_EXPR_NAME) {
            const AstType *callable = &expression->left->resolved_ast_type;
            if (callable->function_return_type)
                set_expression_declared_type(analyzer, expression,
                    expression->left->resolved_type_program != NULL
                        ? expression->left->resolved_type_program : analyzer->program,
                    callable->function_return_type);
        }
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program, expression->left->value_token);
            TokenType callee_type = expression->left->value_token < analyzer->program->token_count
                                        ? analyzer->program->tokens[expression->left->value_token].type
                                        : TOKEN_ERROR;
            AstType cast_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->left->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            if (expression->left->has_resolved_ast_type &&
                expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION) {
                const AstType *callable = &expression->left->resolved_ast_type;
                if (callable->function_return_type)
                    set_expression_declared_type(analyzer, expression,
                        expression->left->resolved_type_program != NULL
                            ? expression->left->resolved_type_program : analyzer->program,
                        callable->function_return_type);
                if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                    analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
                        SEMANTIC_SYMBOL_FUNCTION)
                    expression->resolved_symbol_id = expression->left->resolved_symbol_id;
                if (callable->function_generic_parameters && expression->left->resolved_callable) {
                    const char *template_name = ast_program_lexeme(
                        expression->left->resolved_callable_program,
                        expression->left->resolved_callable->name_token);
                    instantiate_generic_candidates(analyzer, template_name, expression->arguments);
                    int ambiguous = 0;
                    const SemanticSymbol *specialized = resolve_overload(
                        analyzer, template_name, AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
                    if (specialized) {
                        expression->resolved_symbol_id = specialized->id;
                        set_expression_declared_type(analyzer, expression,
                            specialized->source_program, &specialized->declared_type);
                    }
                }
            } else if (callee_type >= TOKEN_TYPE_INT && callee_type <= TOKEN_TYPE_NEVER) {
                expression->resolved_type = primitive_type(analyzer->program, &cast_type);
            } else {
                int ambiguous = 0;
                const SemanticSymbol *function;
                if (expression->left->explicit_type_arguments) function = explicit_generic_function(
                                                                   analyzer, name, expression);
                else {
                    instantiate_generic_candidates(analyzer, name, expression->arguments);
                    function = resolve_overload(analyzer, name, AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
                }
                (void) ambiguous;
                if (function != NULL) {
                    size_t function_id = specialize_callable_consumer(
                        analyzer, function->id, expression->arguments,
                        expression->first_token);
                    function = function_id < analyzer->model->symbol_count
                                   ? &analyzer->model->symbols[function_id]
                                   : NULL;
                }
                if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    set_expression_declared_type(analyzer, expression,
                                                 function->source_program,
                                                 &function->declared_type);
                    set_polymorphic_call_result(analyzer, expression,
                                                function);
                } else if (is_builtin_name(name)) {
                    expression->resolved_type = builtin_result_type(name);
                    if (strcmp(name, "malloc") == 0)
                        expression->resolved_pointer_depth = 1;
                    const CoreIntrinsic *core = core_intrinsic_find(name);
                    if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
                    if (strcmp(name, "read") == 0 && expression->arguments != NULL &&
                        expression->arguments->next != NULL) {
                        const AstExpression *format = expression->arguments->next;
                        const char *format_text = format->kind == AST_EXPR_LITERAL
                                                      ? ast_program_lexeme(analyzer->program, format->value_token)
                                                      : "";
                        expression->resolved_type = strcmp(format_text, "%c") == 0
                                                        ? TYPE_CHAR
                                                        : TYPE_INT;
                    }
                } else if (expression->left->resolved_named_type_token != AST_TOKEN_NONE) {
                    expression->resolved_named_type_token =
                            expression->left->resolved_named_type_token;
                    expression->resolved_named_symbol_id =
                            expression->left->resolved_named_symbol_id;
                }
            }
        } else if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
                   expression->left->left != NULL &&
                   !(expression->left->has_resolved_ast_type &&
                     expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION &&
                     expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                     analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
                         SEMANTIC_SYMBOL_FIELD)) {
            const AstExpression *receiver = expression->left->left;
            int enum_type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                     analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                     SEMANTIC_SYMBOL_ENUM;
            const AstEnumValue *access_variant = !enum_type_receiver &&
                                                 receiver->resolved_pointer_depth == 0 && !receiver->resolved_is_array
                                                 && !receiver->resolved_is_slice
                                                     ? find_enum_value_by_symbol(
                                                         analyzer, receiver->resolved_named_symbol_id,
                                                         expression->left->value_token)
                                                     : NULL;
            if (access_variant != NULL) {
                const SemanticSymbol *variant = &analyzer->model->symbols[access_variant->resolved_symbol_id];
                const AstTypeArgument *payload = access_variant->payload_types;
                expression->kind = AST_EXPR_ENUM_ACCESS;
                expression->resolved_symbol_id = variant->id;
                if (!payload || payload->next) {
                    semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Enum payload accessor requires a variant with exactly one payload");
                    return;
                }
                set_expression_declared_type(analyzer, expression,
                                             variant->source_program,
                                             &payload->type);
                return;
            }
            if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
                expression->resolved_symbol_id = expression->left->resolved_symbol_id;
                expression->resolved_named_symbol_id = receiver->resolved_named_symbol_id;
                expression->resolved_named_type_token = receiver->resolved_named_type_token;
                return;
            }
            int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_STRUCT ||
                                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_ENUM);
            int ambiguous = 0;
            const SemanticSymbol *method = resolve_overload(analyzer,
                                                            ast_program_lexeme(
                                                                analyzer->program, expression->left->value_token),
                                                            receiver->resolved_named_symbol_id, type_receiver,
                                                            expression->arguments, &ambiguous);
            (void) ambiguous;
            if (method != NULL) {
                expression->resolved_symbol_id = method->id;
                set_expression_declared_type(analyzer, expression,
                                             method->source_program,
                                             &method->declared_type);
            }
        }
        if (expression->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *callee =
                &analyzer->model->symbols[expression->resolved_symbol_id];
            if (callee->kind == SEMANTIC_SYMBOL_FUNCTION &&
                callee->declaration != NULL &&
                callee->declaration->as.function.returns_owned_slice_backing)
                expression->owns_slice_backing = 1;
        }
    } else if (expression->kind == AST_EXPR_SUBSLICE &&
               expression->left != NULL) {
        if (expression->left->has_resolved_ast_type &&
            (expression->left->resolved_ast_type.is_array ||
             expression->left->resolved_ast_type.is_slice) &&
            expression->left->resolved_ast_type.outer_pointer_depth == 0) {
            AstType element =
                ast_type_element(&expression->left->resolved_ast_type);
            AstType result = slice_of_type(analyzer, element);
            set_expression_declared_type(
                analyzer, expression,
                expression->left->resolved_type_program != NULL
                    ? expression->left->resolved_type_program
                    : analyzer->program,
                &result);
        } else {
            expression->resolved_type = expression->left->resolved_type;
            expression->resolved_pointer_depth =
                expression->left->resolved_pointer_depth;
            expression->resolved_named_type_token =
                expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id =
                expression->left->resolved_named_symbol_id;
            expression->resolved_is_slice = 1;
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        if (expression->left->has_resolved_ast_type &&
            (expression->left->resolved_ast_type.is_array ||
             expression->left->resolved_ast_type.is_slice) &&
            expression->left->resolved_ast_type.outer_pointer_depth == 0) {
            AstType element = ast_type_element(
                &expression->left->resolved_ast_type);
            set_expression_declared_type(
                analyzer, expression,
                expression->left->resolved_type_program != NULL
                    ? expression->left->resolved_type_program : analyzer->program,
                &element);
        } else {
        expression->resolved_type = expression->left->resolved_type;
        expression->resolved_borrow_kind = AST_BORROW_NONE;
        expression->resolved_pointer_depth = expression->left->resolved_pointer_depth;
        expression->resolved_outer_pointer_depth =
                expression->left->resolved_outer_pointer_depth;
        expression->resolved_named_type_token = expression->left->resolved_named_type_token;
        expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
        expression->resolved_is_array = expression->left->resolved_is_array;
        expression->resolved_is_slice = expression->left->resolved_is_slice;
        if (expression->resolved_outer_pointer_depth > 0)
            expression->resolved_outer_pointer_depth--;
        else if (expression->resolved_is_array || expression->resolved_is_slice) {
            expression->resolved_is_array = 0;
            expression->resolved_is_slice = 0;
        } else if (expression->resolved_pointer_depth > 0)
            expression->resolved_pointer_depth--;
        }
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL) {
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "length")) {
            expression->kind = AST_EXPR_SLICE_LENGTH;
            expression->resolved_type = TYPE_USIZE;
            return;
        }
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "data")) {
            expression->kind = AST_EXPR_SLICE_DATA;
            if (expression->left->has_resolved_ast_type) {
                AstType element = ast_type_element(
                    &expression->left->resolved_ast_type);
                if (element.is_array || element.is_slice)
                    element.outer_pointer_depth++;
                else element.pointer_depth++;
                set_expression_declared_type(
                    analyzer, expression,
                    expression->left->resolved_type_program != NULL
                        ? expression->left->resolved_type_program
                        : analyzer->program,
                    &element);
            } else {
                expression->resolved_type = expression->left->resolved_type;
                expression->resolved_pointer_depth =
                        expression->left->resolved_pointer_depth + 1;
                expression->resolved_named_type_token =
                        expression->left->resolved_named_type_token;
                expression->resolved_named_symbol_id =
                        expression->left->resolved_named_symbol_id;
            }
            return;
        }
        const AstEnumValue *value = NULL;
        if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
            SEMANTIC_SYMBOL_ENUM)
            value = find_enum_value_by_symbol(analyzer,
                                              expression->left->resolved_named_symbol_id, expression->value_token);
        if (value != NULL) {
            expression->resolved_symbol_id = value->resolved_symbol_id;
            expression->resolved_named_type_token =
                    expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
            const SemanticSymbol *enum_symbol = &analyzer->model->symbols[expression->resolved_named_symbol_id];
            if (enum_symbol->declaration->as.enum_decl.is_sum && value->payload_types == NULL)
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
            return;
        }
        const SemanticSymbol *method = find_method(analyzer,
                                                   expression->left->resolved_named_symbol_id, expression->value_token);
        if (method != NULL) {
            expression->resolved_symbol_id = method->id;
            int type_receiver = expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                (analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
                 analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM ||
                 analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_INTERFACE);
            if (type_receiver) {
                AstType callable = callable_type(analyzer, method,
                    !method->declaration->as.function.is_static);
                set_expression_declared_type(analyzer, expression, method->source_program, &callable);
                expression->resolved_callable = method->declaration;
                expression->resolved_callable_program = method->source_program;
            }
            return;
        }
        const AstField *field = find_field_by_symbol(analyzer,
                                                     expression->left->resolved_named_symbol_id,
                                                     expression->value_token);
        if (field != NULL) {
            const SemanticSymbol *field_symbol = field->resolved_symbol_id <
                                                 analyzer->model->symbol_count
                                                     ? &analyzer->model->symbols[field->resolved_symbol_id]
                                                     : NULL;
            const AstProgram *field_program = field_symbol == NULL
                                                  ? analyzer->program
                                                  : field_symbol->source_program;
            set_expression_declared_type(analyzer, expression, field_program,
                                         &field->type);
            expression->resolved_symbol_id = field->resolved_symbol_id;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        validate_array_shape(analyzer, &expression->allocated_type);
        const AstType *reserved_type = &expression->allocated_type;
        set_expression_declared_type(analyzer, expression, analyzer->program,
                                     reserved_type);
        if (reserved_type->is_array || reserved_type->is_slice)
            expression->resolved_outer_pointer_depth++;
        else expression->resolved_pointer_depth++;
        if (expression->has_resolved_ast_type) {
            if (reserved_type->is_array || reserved_type->is_slice)
                expression->resolved_ast_type.outer_pointer_depth++;
            else expression->resolved_ast_type.pointer_depth++;
        }
    }
    if (expression->kind == AST_EXPR_UNARY && expression->right != NULL)
        expression->resolved_array_length = expression->right->resolved_array_length;
    else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL &&
             !expression->has_resolved_ast_type)
        expression->resolved_array_length = expression->resolved_is_array ? expression->left->resolved_array_length : 0;
    else if (expression->kind == AST_EXPR_CAST)
        expression->resolved_array_length = expression->allocated_type.resolved_array_length;
    else if (expression->kind == AST_EXPR_RESERVE)
        expression->resolved_array_length = expression->allocated_type.resolved_array_length;
    else if (expression->resolved_symbol_id < analyzer->model->symbol_count)
        expression->resolved_array_length = analyzer->model->symbols[expression->resolved_symbol_id].declared_type.
                resolved_array_length;
    if (expression->resolved_type == TYPE_UNKNOWN &&
        expression->resolved_named_type_token == AST_TOKEN_NONE &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE &&
        !(expression->has_resolved_ast_type &&
          expression->resolved_ast_type.kind == AST_TYPE_FUNCTION) &&
        !(expression->kind == AST_EXPR_MEMBER && expression->left != NULL &&
          find_enum_value_by_symbol(analyzer,
                                    expression->left->resolved_named_symbol_id,
                                    expression->value_token) != NULL) &&
        expression->kind != AST_EXPR_RESERVE &&
        !(expression->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program, expression->value_token))) &&
        !(expression->kind == AST_EXPR_CALL && expression->left != NULL &&
          expression->left->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program,
                                             expression->left->value_token))))
        analyzer->model->unresolved_expression_count++;
}
