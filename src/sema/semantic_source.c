#include "semantic_internal.h"
#include <stdlib.h>
#include <string.h>

/* This registry is deliberately added only after runtime checking. Generic source
 * templates and their binders must not become candidates for runtime overload lookup. */
typedef struct {
    SemanticModel *model;
    const AstProgram *program;
    int failed;
} SourceRegistry;

size_t semantic_source_scope(const SemanticModel *model, const void *node) {
    if (!model || !node)
        return AST_SYMBOL_NONE;
    for (size_t i = 0; i < model->source_scope_count; i++)
        if (model->source_scopes[i].node == node)
            return i;
    return AST_SYMBOL_NONE;
}
size_t semantic_source_global_binding(const SemanticModel *model, const AstProgram *program, const char *name) {
    if (!model || !program || !name)
        return AST_SYMBOL_NONE;
    size_t found = AST_SYMBOL_NONE;
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *symbol = &model->symbols[i];
        if ((symbol->kind != SEMANTIC_SYMBOL_FUNCTION && symbol->kind != SEMANTIC_SYMBOL_VARIABLE &&
             symbol->kind != SEMANTIC_SYMBOL_CONSTANT) ||
            !symbol->declaration || symbol->declaration->generic_origin || symbol->declaration->source_origin ||
            symbol->name_token >= symbol->source_program->token_count ||
            symbol->source_program->tokens[symbol->name_token].span.begin.line == 0 ||
            !symbol_matches_scope(program, symbol, name) ||
            (!same_package(program, symbol->source_program) && !symbol->declaration->is_public))
            continue;
        if (found != AST_SYMBOL_NONE)
            return AST_SYMBOL_NONE;
        found = i;
    }
    return found;
}
static int source_location_compare(AstSourceLocation left, AstSourceLocation right) {
    return left.line == right.line ? (left.column > right.column) - (left.column < right.column)
                                  : (left.line > right.line) - (left.line < right.line);
}
int semantic_source_intrinsic(const SemanticModel *model, const AstProgram *program, size_t scope,
                              size_t token, AstSourceLocation position) {
    if (!model || !program || token >= program->token_count) return 0;
    const char *name = ast_program_lexeme(program, token);
    int primitive = program->tokens[token].type >= TOKEN_TYPE_INT &&
                    program->tokens[token].type <= TOKEN_TYPE_NEVER;
    return (primitive || is_builtin_name(name)) &&
           semantic_source_binding(model, program, scope, token, SEMANTIC_SYMBOL_LOCAL, position) == AST_SYMBOL_NONE &&
           semantic_source_global_binding(model, program, name) == AST_SYMBOL_NONE;
}

size_t semantic_source_scope_at(const SemanticModel *model, const AstProgram *program, size_t owner,
                                 AstSourceLocation position) {
    size_t found = AST_SYMBOL_NONE;
    for (size_t i = 0; model && i < model->source_scope_count; i++) {
        const SemanticSourceScope *scope = &model->source_scopes[i];
        if (scope->program != program || scope->owner != owner || scope->span.begin.line < 1 ||
            source_location_compare(position, scope->span.begin) < 0 ||
            source_location_compare(position, scope->span.end) >= 0) continue;
        if (found == AST_SYMBOL_NONE ||
            source_location_compare(scope->span.begin, model->source_scopes[found].span.begin) > 0 ||
            (source_location_compare(scope->span.begin, model->source_scopes[found].span.begin) == 0 &&
             source_location_compare(scope->span.end, model->source_scopes[found].span.end) < 0)) found = i;
    }
    return found;
}
static size_t scope_add(SourceRegistry *registry, const void *node, AstSourceSpan span, size_t parent, size_t owner) {
    size_t existing = semantic_source_scope(registry->model, node);
    if (existing != AST_SYMBOL_NONE)
        return existing;
    size_t count = registry->model->source_scope_count;
    if (count >= 20000) {
        registry->failed = 1;
        return AST_SYMBOL_NONE;
    }
    SemanticSourceScope *grown = realloc(registry->model->source_scopes, (count + 1) * sizeof(*grown));
    if (!grown) {
        registry->failed = 1;
        return AST_SYMBOL_NONE;
    }
    registry->model->source_scopes = grown;
    grown[count] = (SemanticSourceScope){registry->program, node, span, parent, owner, 0, NULL};
    registry->model->source_scope_count++;
    return count;
}
static size_t symbol_add(SourceRegistry *registry, SemanticSymbolKind kind, size_t token, size_t owner, size_t scope,
                         const AstDeclarationNode *declaration, const void *node, const AstType *type,
                         AstSourceLocation visible) {
    SemanticModel *model = registry->model;
    if (token >= registry->program->token_count || registry->program->tokens[token].span.begin.line < 1)
        return AST_SYMBOL_NONE;
    for (size_t i = 0; i < model->symbol_count; i++) {
        SemanticSymbol *symbol = &model->symbols[i];
        if (symbol->source_program != registry->program || symbol->kind != kind || symbol->name_token != token)
            continue;
        if (declaration ? symbol->declaration != declaration : symbol->owner_symbol_id != owner)
            continue;
        symbol->source_scope = scope;
        symbol->source_visible_from = visible;
        return i;
    }
    if (model->symbol_count >= 100000) {
        registry->failed = 1;
        return AST_SYMBOL_NONE;
    }
    SemanticSymbol symbol = {.source_program = registry->program,
                             .kind = kind,
                             .name_token = token,
                             .owner_token = AST_TOKEN_NONE,
                             .owner_symbol_id = owner,
                             .declared_type =
                                 type ? *type : (AstType){.kind = AST_TYPE_INFERRED, .name_token = AST_TOKEN_NONE},
                             .resolved_type = TYPE_UNKNOWN,
                             .resolved_named_type_token = AST_TOKEN_NONE,
                             .resolved_named_symbol_id = AST_SYMBOL_NONE,
                             .declaration = declaration,
                             .node = node,
                             .source_only = 1,
                             .source_scope = scope,
                             .source_visible_from = visible};
    if (!semantic_append_symbol(model, symbol)) {
        registry->failed = 1;
        return AST_SYMBOL_NONE;
    }
    return model->symbol_count - 1;
}
static void source_type(SourceRegistry *, const AstType *, size_t, size_t, unsigned);
static void binders(SourceRegistry *registry, const AstGenericParameter *generic, const AstLifetimeParameter *lifetimes,
                    size_t scope, size_t owner, unsigned depth) {
    for (const AstGenericParameter *p = generic; p; p = p->next)
        symbol_add(registry, SEMANTIC_SYMBOL_TYPE_PARAMETER, p->name_token, owner, scope, NULL, p, NULL,
                   (AstSourceLocation){0});
    for (const AstLifetimeParameter *p = lifetimes; p; p = p->next)
        symbol_add(registry, SEMANTIC_SYMBOL_LIFETIME_PARAMETER, p->name_token, owner, scope, NULL, p, NULL,
                   (AstSourceLocation){0});
    for (const AstGenericParameter *p = generic; p; p = p->next)
        for (const AstInterfaceBound *bound = p->bounds; bound; bound = bound->next)
            source_type(registry, &bound->type, scope, owner, depth + 1);
}
static void source_type(SourceRegistry *registry, const AstType *type, size_t scope, size_t owner, unsigned depth) {
    if (!type)
        return;
    if (depth > 256) {
        registry->failed = 1;
        return;
    }
    if (type->kind == AST_TYPE_FUNCTION && (type->function_generic_parameters || type->function_lifetime_parameters)) {
        scope = scope_add(registry, type, type->span, scope, owner);
        if (scope < registry->model->source_scope_count)
            registry->model->source_scopes[scope].binder_barriers =
                (type->function_generic_parameters ? 1u : 0u) | (type->function_lifetime_parameters ? 2u : 0u);
        binders(registry, type->function_generic_parameters, type->function_lifetime_parameters, scope, owner,
                depth + 1);
    }
    source_type(registry, type->reference_type, scope, owner, depth + 1);
    source_type(registry, type->element_type, scope, owner, depth + 1);
    for (const AstTypeArgument *argument = type->arguments; argument; argument = argument->next)
        source_type(registry, &argument->type, scope, owner, depth + 1);
    for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
        source_type(registry, &parameter->type, scope, owner, depth + 1);
    source_type(registry, type->function_return_type, scope, owner, depth + 1);
}
static void source_statements(SourceRegistry *, const AstStatement *, size_t, size_t, unsigned);
static void source_expressions(SourceRegistry *registry, const AstExpression *expression, size_t scope, size_t owner,
                               unsigned depth) {
    if (depth > 256) {
        registry->failed = 1;
        return;
    }
    for (; expression; expression = expression->next) {
        source_type(registry, &expression->allocated_type, scope, owner, depth + 1);
        source_expressions(registry, expression->left, scope, owner, depth + 1);
        source_expressions(registry, expression->right, scope, owner, depth + 1);
        source_expressions(registry, expression->arguments, scope, owner, depth + 1);
        source_statements(registry, expression->control, scope, owner, depth + 1);
        const AstDeclarationNode *environment = expression->closure_environment;
        if (environment && !environment->source_origin) {
            const AstDeclarationNode *invoke = environment->closure_consuming_invoke
                                                   ? environment->closure_consuming_invoke
                                                   : environment->as.struct_decl.methods;
            if (!invoke)
                continue;
            size_t id = symbol_add(registry, SEMANTIC_SYMBOL_FUNCTION, invoke->first_token, owner, scope, invoke,
                                   invoke, &invoke->as.function.return_type, (AstSourceLocation){0});
            size_t callable = scope_add(registry, invoke, invoke->span, scope, id);
            if (callable < registry->model->source_scope_count)
                registry->model->source_scopes[callable].closure_captures =
                    environment->closure_captures ? environment->closure_captures : environment->as.struct_decl.fields;
            source_type(registry, &invoke->as.function.return_type, callable, id, depth + 1);
            for (const AstParameter *parameter = invoke->as.function.parameters; parameter;
                 parameter = parameter->next) {
                symbol_add(registry, SEMANTIC_SYMBOL_PARAMETER, parameter->name_token, id, callable, NULL, parameter,
                           &parameter->type, (AstSourceLocation){0});
                source_type(registry, &parameter->type, callable, id, depth + 1);
            }
            source_statements(registry, invoke->as.function.body, callable, id, depth + 1);
        }
    }
}
static void source_statements(SourceRegistry *registry, const AstStatement *statement, size_t scope, size_t owner,
                              unsigned depth) {
    if (depth > 256) {
        registry->failed = 1;
        return;
    }
    for (; statement; statement = statement->next) {
        size_t current = scope;
        if (statement->kind == AST_STMT_BLOCK || statement->kind == AST_STMT_FOR)
            current = scope_add(registry, statement, statement->span, scope, owner);
        if (statement->kind == AST_STMT_VARIABLE)
            symbol_add(registry, statement->is_const ? SEMANTIC_SYMBOL_CONSTANT : SEMANTIC_SYMBOL_LOCAL,
                       statement->name_token, owner, current, NULL, statement->value, &statement->type,
                       statement->span.end);
        source_type(registry, &statement->type, current, owner, depth + 1);
        source_expressions(registry, statement->expression, current, owner, depth + 1);
        source_expressions(registry, statement->value, current, owner, depth + 1);
        source_expressions(registry, statement->condition, current, owner, depth + 1);
        source_expressions(registry, statement->update, current, owner, depth + 1);
        source_expressions(registry, statement->result, current, owner, depth + 1);
        source_statements(registry, statement->initializer, current, owner, depth + 1);
        source_statements(registry, statement->body, current, owner, depth + 1);
        source_statements(registry, statement->else_body, current, owner, depth + 1);
        for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
            size_t arm_scope = scope_add(registry, arm, arm->span, current, owner);
            source_type(registry, &arm->type, arm_scope, owner, depth + 1);
            for (const AstParameter *binding = arm->bindings; binding; binding = binding->next) {
                if (!binding->source_capture)
                    symbol_add(registry, SEMANTIC_SYMBOL_LOCAL, binding->name_token, owner, arm_scope, NULL, NULL,
                               &binding->type, arm->span.begin);
                source_type(registry, &binding->type, arm_scope, owner, depth + 1);
            }
            source_statements(registry, arm->body, arm_scope, owner, depth + 1);
        }
    }
}
static void source_declarations(SourceRegistry *registry, const AstDeclarationNode *declaration, size_t parent,
                                size_t owner, unsigned depth) {
    if (depth > 256) {
        registry->failed = 1;
        return;
    }
    for (; declaration; declaration = declaration->next) {
        if (declaration->generic_origin || declaration->source_origin || declaration->is_closure_environment ||
            declaration->is_source_closure)
            continue;
        SemanticSymbolKind kind;
        switch (declaration->kind) {
        case AST_DECL_FUNCTION:
            kind = SEMANTIC_SYMBOL_FUNCTION;
            break;
        case AST_DECL_STRUCT:
            kind = SEMANTIC_SYMBOL_STRUCT;
            break;
        case AST_DECL_ENUM:
            kind = SEMANTIC_SYMBOL_ENUM;
            break;
        case AST_DECL_INTERFACE:
            kind = SEMANTIC_SYMBOL_INTERFACE;
            break;
        case AST_DECL_CONSTANT:
            kind = SEMANTIC_SYMBOL_CONSTANT;
            break;
        case AST_DECL_VARIABLE:
            kind = SEMANTIC_SYMBOL_VARIABLE;
            break;
        default:
            continue;
        }
        const AstType *type = declaration->kind == AST_DECL_FUNCTION ? &declaration->as.function.return_type
                              : declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE
                                  ? &declaration->as.constant.type
                                  : NULL;
        size_t id = symbol_add(registry, kind, declaration->name_token, owner, parent, declaration, declaration, type,
                               (AstSourceLocation){0});
        if (id == AST_SYMBOL_NONE)
            continue;
        size_t scope = scope_add(registry, declaration, declaration->span, parent, id);
        binders(registry, declaration->generic_parameters, declaration->lifetime_parameters, scope, id, depth + 1);
        for (const AstAutoCondition *condition = declaration->where_conditions; condition;
             condition = condition->next) {
            source_type(registry, &condition->type, scope, id, depth + 1);
            for (const AstInterfaceBound *bound = condition->bounds; bound; bound = bound->next)
                source_type(registry, &bound->type, scope, id, depth + 1);
        }
        if (declaration->kind == AST_DECL_FUNCTION) {
            source_type(registry, type, scope, id, depth + 1);
            for (const AstParameter *p = declaration->as.function.parameters; p; p = p->next) {
                symbol_add(registry, SEMANTIC_SYMBOL_PARAMETER, p->name_token, id, scope, NULL, p, &p->type,
                           (AstSourceLocation){0});
                source_type(registry, &p->type, scope, id, depth + 1);
            }
            source_statements(registry, declaration->as.function.body, scope, id, depth + 1);
        } else if (declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE) {
            source_type(registry, type, scope, id, depth + 1);
            source_expressions(registry, declaration->as.constant.value, scope, id, depth + 1);
        } else {
            const AstField *fields = declaration->kind == AST_DECL_STRUCT ? declaration->as.struct_decl.fields
                                     : declaration->kind == AST_DECL_ENUM ? declaration->as.enum_decl.fields
                                                                          : NULL;
            for (const AstField *field = fields; field; field = field->next) {
                symbol_add(registry, SEMANTIC_SYMBOL_FIELD, field->name_token, id, scope, NULL, field, &field->type,
                           (AstSourceLocation){0});
                source_type(registry, &field->type, scope, id, depth + 1);
            }
            if (declaration->kind == AST_DECL_ENUM)
                for (const AstEnumValue *variant = declaration->as.enum_decl.values; variant; variant = variant->next) {
                    symbol_add(registry, SEMANTIC_SYMBOL_ENUM_VALUE, variant->name_token, id, scope, NULL, variant,
                               NULL, (AstSourceLocation){0});
                    for (const AstTypeArgument *argument = variant->payload_types; argument; argument = argument->next)
                        source_type(registry, &argument->type, scope, id, depth + 1);
                }
            const AstDeclarationNode *methods =
                declaration->kind == AST_DECL_STRUCT ? declaration->as.struct_decl.methods
                : declaration->kind == AST_DECL_ENUM ? declaration->as.enum_decl.methods
                                                     : declaration->as.interface_decl.methods;
            source_declarations(registry, methods, scope, id, depth + 1);
            if (declaration->kind == AST_DECL_STRUCT)
                source_statements(registry, declaration->as.struct_decl.destructor, scope, id, depth + 1);
        }
    }
}
int semantic_register_source_graph(SemanticModel *model) {
    if (!model || model->source_scopes)
        return 0;
    for (size_t i = 0; i < model->symbol_count; i++)
        model->symbols[i].source_scope = AST_SYMBOL_NONE;
    SourceRegistry registry = {.model = model};
    for (size_t i = 0; i <= model->program->owned_import_count; i++) {
        registry.program = i ? model->program->owned_imports[i - 1] : model->program;
        size_t root = scope_add(&registry, registry.program, (AstSourceSpan){0}, AST_SYMBOL_NONE, AST_SYMBOL_NONE);
        source_declarations(&registry, registry.program->root, root, AST_SYMBOL_NONE, 0);
    }
    model->source_registry_complete = !registry.failed;
    return !registry.failed;
}
size_t semantic_source_binding(const SemanticModel *model, const AstProgram *program, size_t scope, size_t token,
                               SemanticSymbolKind kind, AstSourceLocation position) {
    if (!model || token >= program->token_count)
        return AST_SYMBOL_NONE;
    const char *name = ast_program_lexeme(program, token);
    int instance_scope = 1;
    for (unsigned depth = 0; scope < model->source_scope_count && depth < 256; depth++) {
        size_t owner = model->source_scopes[scope].owner;
        if (owner < model->symbol_count && model->symbols[owner].kind == SEMANTIC_SYMBOL_FUNCTION &&
            model->symbols[owner].declaration && model->symbols[owner].declaration->as.function.is_static)
            instance_scope = 0;
        size_t found = AST_SYMBOL_NONE;
        for (size_t i = 0; i < model->symbol_count; i++) {
            const SemanticSymbol *symbol = &model->symbols[i];
            int matching_kind =
                symbol->kind == kind ||
                (kind == SEMANTIC_SYMBOL_LOCAL && (symbol->kind == SEMANTIC_SYMBOL_PARAMETER ||
                                                   (symbol->kind == SEMANTIC_SYMBOL_CONSTANT && !symbol->declaration) ||
                                                   (instance_scope && symbol->kind == SEMANTIC_SYMBOL_FIELD)));
            if (symbol->source_program != program || symbol->source_scope != scope || !matching_kind ||
                strcmp(ast_program_lexeme(program, symbol->name_token), name))
                continue;
            AstSourceLocation visible = symbol->source_visible_from;
            if (visible.line > position.line || (visible.line == position.line && visible.column > position.column))
                continue;
            if (found != AST_SYMBOL_NONE)
                return AST_SYMBOL_NONE;
            found = i;
        }
        if (found != AST_SYMBOL_NONE)
            return found;
        unsigned barriers = model->source_scopes[scope].binder_barriers;
        if ((kind == SEMANTIC_SYMBOL_TYPE_PARAMETER && (barriers & 1u)) ||
            (kind == SEMANTIC_SYMBOL_LIFETIME_PARAMETER && (barriers & 2u)))
            return AST_SYMBOL_NONE;
        if (kind == SEMANTIC_SYMBOL_LOCAL && owner < model->symbol_count && model->symbols[owner].declaration &&
            model->symbols[owner].declaration->is_source_closure &&
            model->source_scopes[scope].node == model->symbols[owner].declaration) {
            const AstField *capture = model->source_scopes[scope].closure_captures;
            for (; capture; capture = capture->next)
                if (!strcmp(ast_program_lexeme(program, capture->name_token), name))
                    break;
            if (!capture)
                return AST_SYMBOL_NONE;
            position = capture->span.begin;
        }
        scope = model->source_scopes[scope].parent;
    }
    return AST_SYMBOL_NONE;
}

typedef struct {
    const SemanticModel *model;
    const AstProgram *query_program;
    const char *name;
    int arity;
    size_t *candidates, capacity, count, steps;
    unsigned dereferences;
    int complete;
} SourceMembers;
static void source_member_type(SourceMembers *, const AstProgram *, size_t, const AstType *, unsigned);
static void source_member_expression(SourceMembers *query, const AstProgram *program, size_t scope,
                                      const AstExpression *receiver, unsigned depth) {
    if (!receiver || depth > 64 || ++query->steps > 50000) { query->complete = 0; return; }
    if (receiver->kind == AST_EXPR_UNARY && receiver->operator_type == TOKEN_STAR) {
        query->dereferences++;
        source_member_expression(query, program, scope, receiver->right, depth + 1);
        query->dereferences--;
        return;
    }
    size_t id = receiver->resolved_symbol_id;
    if (id >= query->model->symbol_count && receiver->kind == AST_EXPR_NAME)
        id = semantic_source_binding(query->model, program, scope, receiver->value_token,
                                     SEMANTIC_SYMBOL_LOCAL, receiver->span.begin);
    if (id < query->model->symbol_count) {
        const SemanticSymbol *symbol = &query->model->symbols[id];
        if (symbol->kind != SEMANTIC_SYMBOL_LOCAL && symbol->kind != SEMANTIC_SYMBOL_PARAMETER &&
            symbol->kind != SEMANTIC_SYMBOL_FIELD && symbol->kind != SEMANTIC_SYMBOL_VARIABLE &&
            symbol->kind != SEMANTIC_SYMBOL_CONSTANT) return;
        if (symbol->declared_type.kind != AST_TYPE_INFERRED)
            source_member_type(query, symbol->source_program, symbol->source_scope, &symbol->declared_type, depth + 1);
        else if (symbol->node && symbol->kind == SEMANTIC_SYMBOL_LOCAL)
            source_member_expression(query, symbol->source_program, symbol->source_scope, symbol->node, depth + 1);
    } else if (receiver->has_resolved_ast_type)
        source_member_type(query, receiver->resolved_type_program ? receiver->resolved_type_program : program,
                           scope, &receiver->resolved_ast_type, depth + 1);
}
static void source_member_type(SourceMembers *query, const AstProgram *program, size_t scope,
                               const AstType *type, unsigned depth) {
    if (!type || depth > 64 || ++query->steps > 50000) { query->complete = 0; return; }
    if (type->reference_type) {
        source_member_type(query, program, scope, type->reference_type, depth + 1);
        return;
    }
    if (type->kind != AST_TYPE_NAMED || type->is_array || type->is_slice) return;
    if (type->pointer_depth + type->outer_pointer_depth > query->dereferences) return;
    size_t binder = semantic_source_binding(query->model, program, scope, type->name_token,
                                            SEMANTIC_SYMBOL_TYPE_PARAMETER, type->span.begin);
    if (binder < query->model->symbol_count) {
        const AstGenericParameter *parameter = query->model->symbols[binder].node;
        for (const AstInterfaceBound *bound = parameter->bounds; bound; bound = bound->next)
            source_member_type(query, program, scope, &bound->type, depth + 1);
        return;
    }
    const char *name = ast_program_lexeme(program, type->name_token);
    size_t owner = AST_SYMBOL_NONE;
    for (size_t i = 0; i < query->model->symbol_count; i++) {
        if (++query->steps > 50000) { query->complete = 0; return; }
        const SemanticSymbol *symbol = &query->model->symbols[i];
        if ((symbol->kind != SEMANTIC_SYMBOL_STRUCT && symbol->kind != SEMANTIC_SYMBOL_ENUM &&
             symbol->kind != SEMANTIC_SYMBOL_INTERFACE) || !symbol->declaration ||
            symbol->declaration->generic_origin || symbol->declaration->source_origin ||
            !symbol_matches_scope(program, symbol, name) ||
            (!same_package(program, symbol->source_program) && !symbol->declaration->is_public)) continue;
        if (owner != AST_SYMBOL_NONE) { query->complete = 0; return; }
        owner = i;
    }
    if (owner >= query->model->symbol_count) return;
    const SemanticSymbol *aggregate = &query->model->symbols[owner];
    size_t arguments = 0, parameters = 0;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next) arguments++;
    for (const AstGenericParameter *p = aggregate->declaration->generic_parameters; p; p = p->next) parameters++;
    if (arguments != parameters) { query->complete = 0; return; }
    for (size_t i = 0; i < query->model->symbol_count; i++) {
        if (++query->steps > 50000) { query->complete = 0; return; }
        const SemanticSymbol *symbol = &query->model->symbols[i];
        if (symbol->owner_symbol_id != owner ||
            (symbol->kind != SEMANTIC_SYMBOL_FUNCTION && symbol->kind != SEMANTIC_SYMBOL_FIELD) ||
            strcmp(ast_program_lexeme(symbol->source_program, symbol->name_token), query->name)) continue;
        if (symbol->declaration && (symbol->declaration->generic_origin || symbol->declaration->source_origin ||
                                    symbol->declaration->as.function.is_static)) continue;
        int public_member = symbol->declaration ? symbol->declaration->is_public :
                            symbol->node && ((const AstField *)symbol->node)->is_public;
        if (aggregate->kind != SEMANTIC_SYMBOL_INTERFACE && !same_package(query->query_program, symbol->source_program) &&
            !public_member) continue;
        if (query->arity >= 0) {
            if (symbol->kind != SEMANTIC_SYMBOL_FUNCTION || !symbol->declaration) continue;
            int count = 0;
            for (const AstParameter *p = symbol->declaration->as.function.parameters; p; p = p->next) count++;
            if (count != query->arity) continue;
        }
        size_t j = 0;
        for (; j < query->count && query->candidates[j] != i; j++) {}
        if (j < query->count) continue;
        if (query->count == query->capacity) { query->complete = 0; return; }
        query->candidates[query->count++] = i;
    }
}
size_t semantic_source_members(const SemanticModel *model, const AstProgram *program, size_t scope,
                                const AstExpression *receiver, size_t member_token, int call_arity,
                                size_t *candidates, size_t capacity, int *complete) {
    SourceMembers query = {.model = model, .query_program = program,
                          .name = ast_program_lexeme(program, member_token), .arity = call_arity,
                          .candidates = candidates, .capacity = capacity, .complete = 1};
    source_member_expression(&query, program, scope, receiver, 0);
    /* Multiple overload/contract alternatives require contextual selection.
     * Export every source candidate, but do not claim exhaustive call binding. */
    *complete = query.complete && query.count <= 1;
    return query.count;
}
