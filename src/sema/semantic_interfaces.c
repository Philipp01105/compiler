#include "semantic_internal.h"
#include "generics.h"
#include "errorHandler.h"

#include <stdio.h>
#include <string.h>

size_t semantic_auto_role(const SemanticModel *model, unsigned role) {
    const char *name = role == SEMANTIC_TYPE_SEND
                           ? "Send"
                           : role == SEMANTIC_TYPE_SYNC
                                 ? "Sync"
                                 : role == SEMANTIC_TYPE_COPYABLE
                                       ? "Copy"
                                       : NULL;
    if (!name) return AST_SYMBOL_NONE;
    for (size_t i = 0; i < model->symbol_count; i++) {
        const SemanticSymbol *s = &model->symbols[i];
        if (s->kind == SEMANTIC_SYMBOL_INTERFACE && s->declaration &&
            s->declaration->is_auto_interface && s->source_program->package &&
            !strcmp(s->source_program->package->path, "stdlib/core") &&
            !strcmp(ast_program_lexeme(s->source_program, s->name_token), name))
            return i;
    }
    return AST_SYMBOL_NONE;
}

static int explicit_init_depth(const SemanticModel *model, const AstProgram *unit,
                               const AstType *type, size_t depth) {
    if (!type || depth > model->symbol_count + 64) return 0;
    if (type->borrow_kind) return 1;
    if (type->outer_pointer_depth || type->is_slice ||
        (!type->is_array && type->pointer_depth) || type->kind != AST_TYPE_NAMED)
        return 0;
    if (type->is_array) {
        if (!type->resolved_array_length) return 0;
        AstType element = ast_type_element(type);
        return explicit_init_depth(model, unit, &element, depth + 1);
    }
    Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) unit};
    size_t id = resolve_named_symbol_id(&lookup, unit, type->name_token);
    if (id >= model->symbol_count) return 0;
    const SemanticSymbol *s = &model->symbols[id];
    const AstDeclarationNode *d = s->declaration;
    if (!d || (d->kind != AST_DECL_STRUCT && d->kind != AST_DECL_ENUM)) return 0;
    if (d->no_default) return 1;
    const AstField *fields = d->kind == AST_DECL_STRUCT ? d->as.struct_decl.fields : d->as.enum_decl.fields;
    for (const AstField *f = fields; f; f = f->next)
        if (explicit_init_depth(model, s->source_program, &f->type, depth + 1)) return 1;
    if (d->kind == AST_DECL_ENUM && d->as.enum_decl.values)
        for (const AstTypeArgument *p = d->as.enum_decl.values->payload_types; p; p = p->next)
            if (explicit_init_depth(model, s->source_program, &p->type, depth + 1)) return 1;
    return 0;
}

int semantic_requires_explicit_init(const SemanticModel *model, const AstProgram *unit,
                                    const AstType *type) {
    return explicit_init_depth(model, unit, type, 0);
}

typedef struct {
    const AstProgram *unit;
    AstType type;
    size_t interface_id;
    int result;
} AutoFact;

typedef struct {
    AutoFact facts[128];
    size_t count;
} AutoEvaluation;

typedef struct AutoProof {
    const AstProgram *unit;
    const AstType *type;
    size_t interface_id;
    const struct AutoProof *previous;
    AutoEvaluation *evaluation;
} AutoProof;

static int satisfies_depth(const SemanticModel *, const AstProgram *, const AstType *,
                           size_t, const AutoProof *, size_t);

/* Only the canonical language interfaces have fundamental primitive facts. */
static int fundamental_auto_fact(const SemanticModel *model, const AstProgram *unit,
                                 const AstType *type, size_t interface_id) {
    if (type->kind != AST_TYPE_NAMED || type->borrow_kind || type->pointer_depth ||
        type->outer_pointer_depth || type->is_array || type->is_slice ||
        primitive_type(unit, type) == TYPE_UNKNOWN)
        return -1;
    if (interface_id == semantic_auto_role(model, SEMANTIC_TYPE_SEND) ||
        interface_id == semantic_auto_role(model, SEMANTIC_TYPE_SYNC))
        return 1;
    return -1;
}

static int auto_rule_result(const SemanticModel *model, const AstProgram *unit,
                            const AstAutoRule *rule, const AutoProof *proof, size_t depth) {
    Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) unit};
    for (const AstAutoCondition *c = rule->conditions; c; c = c->next)
        for (const AstInterfaceBound *b = c->bounds; b; b = b->next) {
            size_t id = resolve_named_symbol_id(&lookup, unit, b->name_token);
            if (id < model->symbol_count && model->symbols[id].declaration &&
                !model->symbols[id].declaration->is_auto_interface && b->type.arguments) {
                size_t owner = resolve_named_symbol_id(&lookup, unit, c->type.name_token);
                if (c->type.pointer_depth || c->type.outer_pointer_depth || c->type.is_array || c->type.is_slice ||
                    !semantic_implements_specialized_interface(model, id, owner, unit, &b->type))
                    return 0;
                continue;
            }
            if (!satisfies_depth(model, unit, &c->type, id, proof, depth + 1)) return 0;
        }
    return 1;
}

static const AstAutoRule *find_auto_rule(const SemanticModel *model, const AstProgram *unit,
                                         const AstDeclarationNode *d, size_t interface_id) {
    Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) unit};
    for (const AstAutoRule *r = d->auto_rules; r; r = r->next)
        for (const AstInterfaceBound *b = r->interfaces; b; b = b->next)
            if (resolve_named_symbol_id(&lookup, unit, b->name_token) == interface_id) return r;
    return NULL;
}

static int same_rule_target(const SemanticModel *model, const AstProgram *left_unit,
                            const AstDeclarationNode *left, const AstProgram *right_unit,
                            const AstDeclarationNode *right) {
    AstType x = left->kind == AST_DECL_TYPE_RULE ? left->rule_target : (AstType)
    {
        .kind = AST_TYPE_NAMED, .name_token = left->name_token
    };
    AstType y = right->kind == AST_DECL_TYPE_RULE ? right->rule_target : (AstType)
    {
        .kind = AST_TYPE_NAMED, .name_token = right->name_token
    };
    if (x.kind != y.kind || x.borrow_kind != y.borrow_kind || x.pointer_depth != y.pointer_depth ||
        x.outer_pointer_depth != y.outer_pointer_depth || x.is_array != y.is_array || x.is_slice != y.is_slice)
        return 0;
    if (x.is_array && x.resolved_array_length != y.resolved_array_length) return 0;
    if (x.element_type || y.element_type) return ast_concrete_type_equal(left_unit, &x, right_unit, &y);
    Analyzer lookup = {.model = (SemanticModel *) model};
    size_t a = resolve_named_symbol_id(&lookup, left_unit, x.name_token);
    size_t b = resolve_named_symbol_id(&lookup, right_unit, y.name_token);
    if (a != AST_SYMBOL_NONE || b != AST_SYMBOL_NONE) return a != AST_SYMBOL_NONE && a == b;
    return ast_concrete_type_equal(left_unit, &x, right_unit, &y);
}

static int satisfies_uncached(const SemanticModel *model, const AstProgram *unit,
                              const AstType *type, size_t interface_id,
                              const AutoProof *previous, size_t depth) {
    if (interface_id >= model->symbol_count || depth > model->symbol_count + 64) return 0;
    const SemanticSymbol *interface = &model->symbols[interface_id];
    if (interface->kind != SEMANTIC_SYMBOL_INTERFACE || !interface->declaration) return 0;
    Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) unit};
    if (interface_id == semantic_auto_role(model, SEMANTIC_TYPE_COPYABLE)) {
        /* Copy never evaluates trusted Send/Sync rules. Those rules can themselves
           depend on Copy, so calling the full property evaluator here would cycle. */
        if (type->borrow_kind || type->outer_pointer_depth || type->is_slice ||
            (!type->is_array && type->pointer_depth))
            return 1;
        if (type->is_array) {
            AstType element = ast_type_element(type);
            return satisfies_depth(model, unit, &element, interface_id, previous, depth + 1);
        }
        if (type->callable_mode == 2) return 0;
        if (type->kind == AST_TYPE_FUNCTION) return 1;
        if (type->kind != AST_TYPE_NAMED) return 0;
        if (primitive_type(unit, type) != TYPE_UNKNOWN) return 1;
        size_t id = resolve_named_symbol_id(&lookup, unit, type->name_token);
        return (semantic_symbol_type_properties(model, id) & SEMANTIC_TYPE_COPYABLE) != 0;
    }
    size_t owner = resolve_named_symbol_id(&lookup, unit, type->name_token);
    if (!interface->declaration->is_auto_interface)
        return !type->pointer_depth && !type->outer_pointer_depth && !type->is_array &&
               !type->is_slice && semantic_implements_interface(model, interface_id, owner);
    for (const AutoProof *p = previous; p; p = p->previous)
        if (p->type && p->interface_id == interface_id && ast_concrete_type_equal(p->unit, p->type, unit, type))
            return 0; /* A condition cycle is not independent evidence. */
    AutoProof proof = {unit, type, interface_id, previous, previous->evaluation};
    const AstDeclarationNode *decl = owner < model->symbol_count ? model->symbols[owner].declaration : NULL;
    if (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array &&
        !type->is_slice && !type->borrow_kind && decl) {
        const AstAutoRule *rule = find_auto_rule(model, model->symbols[owner].source_program, decl, interface_id);
        if (rule) return auto_rule_result(model, model->symbols[owner].source_program, rule, &proof, depth);
    }
    const AstProgram *root = model->program;
    for (size_t u = 0; u <= root->owned_import_count; u++) {
        const AstProgram *rule_unit = u ? root->owned_imports[u - 1] : root;
        for (const AstDeclarationNode *d = rule_unit->root; d; d = d->next) {
            if (d->kind != AST_DECL_TYPE_RULE) continue;
            int matches = ast_concrete_type_equal(rule_unit, &d->rule_target, unit, type);
            if (!matches && !type->pointer_depth && !type->outer_pointer_depth && !type->is_array &&
                !type->is_slice && !type->borrow_kind && owner < model->symbol_count)
                matches = resolve_named_symbol_id(&lookup, rule_unit, d->rule_target.name_token) == owner;
            const AstAutoRule *rule = matches ? find_auto_rule(model, rule_unit, d, interface_id) : NULL;
            if (rule) return auto_rule_result(model, rule_unit, rule, &proof, depth);
        }
    }
    int fundamental = fundamental_auto_fact(model, unit, type, interface_id);
    if (fundamental >= 0) return fundamental;
    size_t send = semantic_auto_role(model, SEMANTIC_TYPE_SEND);
    size_t sync = semantic_auto_role(model, SEMANTIC_TYPE_SYNC);
    int language_role = interface_id == send || interface_id == sync;
    if (type->borrow_kind) {
        if (!language_role) return 0;
        AstType referent = *type;
        referent.borrow_kind = AST_BORROW_NONE;
        if (type->borrow_kind == AST_BORROW_IMMUTABLE)
            return satisfies_depth(model, unit, &referent, sync, &proof, depth + 1);
        return interface_id == send && satisfies_depth(model, unit, &referent, send, &proof, depth + 1);
    }
    if (type->outer_pointer_depth || type->is_slice || (!type->is_array && type->pointer_depth)) return 0;
    if (type->is_array) {
        AstType element = ast_type_element(type);
        return satisfies_depth(model, unit, &element, interface_id, &proof, depth + 1);
    }
    if (type->kind == AST_TYPE_JOIN || type->kind == AST_TYPE_EXECUTOR)
        return interface_id == send;
    if (type->kind != AST_TYPE_NAMED) return 0;
    if (primitive_type(unit, type) != TYPE_UNKNOWN) return 0;
    if (!decl || decl->is_opaque || (decl->kind != AST_DECL_STRUCT && decl->kind != AST_DECL_ENUM)) return 0;
    const AstProgram *owner_unit = model->symbols[owner].source_program;
    const AstField *fields = decl->kind == AST_DECL_STRUCT ? decl->as.struct_decl.fields : decl->as.enum_decl.fields;
    for (const AstField *f = fields; f; f = f->next)
        if (!satisfies_depth(model, owner_unit, &f->type, interface_id, &proof, depth + 1)) return 0;
    if (decl->kind == AST_DECL_ENUM)
        for (const AstEnumValue *v = decl->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next)
                if (!satisfies_depth(model, owner_unit, &p->type, interface_id, &proof, depth + 1)) return 0;
    return 1;
}

static int satisfies_depth(const SemanticModel *model, const AstProgram *unit,
                           const AstType *type, size_t interface_id,
                           const AutoProof *previous, size_t depth) {
    AutoEvaluation *evaluation = previous->evaluation;
    for (size_t i = 0; i < evaluation->count; i++) {
        const AutoFact *fact = &evaluation->facts[i];
        if (fact->interface_id == interface_id &&
            ast_concrete_type_equal(fact->unit, &fact->type, unit, type))
            return fact->result;
    }
    /* All requirements are conjunctions. A cycle without a seed is false in the
       least fixed point; explicit rules cut off structural fallback. */
    int result = satisfies_uncached(model, unit, type, interface_id, previous, depth);
    if (evaluation->count < sizeof(evaluation->facts) / sizeof(evaluation->facts[0])) {
        AutoFact *fact = &evaluation->facts[evaluation->count++];
        *fact = (AutoFact){unit, *type, interface_id, result};
    }
    return result;
}

int semantic_satisfies(const SemanticModel *model, const AstProgram *unit,
                       const AstType *type, size_t interface_id) {
    if (!model || !unit || !type) return 0;
    AutoEvaluation evaluation = {0};
    AutoProof root = {.interface_id = AST_SYMBOL_NONE, .evaluation = &evaluation};
    return satisfies_depth(model, unit, type, interface_id, &root, 0);
}

int semantic_method_constraints_satisfied(const Analyzer *a, const AstProgram *unit,
                                          const AstDeclarationNode *method) {
    for (const AstAutoCondition *c = method->where_conditions; c; c = c->next)
        for (const AstInterfaceBound *b = c->bounds; b; b = b->next) {
            size_t id = resolve_named_symbol_id(a, unit, b->name_token);
            if (!semantic_satisfies(a->model, unit, &c->type, id)) return 0;
        }
    return 1;
}

void validate_auto_rules(Analyzer *a) {
    AstProgram *saved = a->program;
    const AstProgram *root = a->model->program;
    for (size_t u = 0; u <= root->owned_import_count; u++) {
        a->program = (AstProgram *) (u ? root->owned_imports[u - 1] : root);
        for (AstDeclarationNode *d = a->program->root; d; d = d->next) {
            if (d->generic_origin) continue;
            if (d->kind == AST_DECL_TYPE_RULE) validate_array_shape(a, &d->rule_target);
            for (AstAutoRule *r = d->auto_rules; r; r = r->next) {
                for (AstInterfaceBound *b = r->interfaces; b; b = b->next) {
                    size_t id = resolve_named_symbol_id(a, a->program, b->name_token);
                    if (id != AST_SYMBOL_NONE && id == semantic_auto_role(a->model, SEMANTIC_TYPE_COPYABLE)) {
                        semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "core.Copy is derived from value semantics and cannot be overridden");
                        continue;
                    }
                    if (id >= a->model->symbol_count || a->model->symbols[id].kind != SEMANTIC_SYMBOL_INTERFACE ||
                        !a->model->symbols[id].declaration->is_auto_interface || b->type.arguments) {
                        semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                       "Type property rule requires an auto interface");
                        continue;
                    }
                    for (AstAutoRule *prior = d->auto_rules; prior; prior = prior->next) {
                        for (AstInterfaceBound *other = prior->interfaces; other; other = other->next) {
                            if (other == b) goto duplicates_done;
                            if (resolve_named_symbol_id(a, a->program, other->name_token) == id)
                                semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                               "Duplicate auto interface rule for target type");
                        }
                    }
                duplicates_done:
                    /* Coherence is global to the package graph, including rules in other files. */
                    for (size_t prior_unit_index = 0; prior_unit_index <= u; prior_unit_index++) {
                        const AstProgram *prior_unit = prior_unit_index
                                                           ? root->owned_imports[prior_unit_index - 1]
                                                           : root;
                        for (const AstDeclarationNode *prior_decl = prior_unit->root; prior_decl;
                             prior_decl = prior_decl->next) {
                            if (prior_decl == d) break;
                            if (prior_decl->generic_origin || !prior_decl->auto_rules ||
                                !same_rule_target(a->model, prior_unit, prior_decl, a->program, d))
                                continue;
                            if (find_auto_rule(a->model, prior_unit, prior_decl, id))
                                semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                               "Duplicate auto interface rule for target type");
                        }
                    }
                    if (d->kind == AST_DECL_TYPE_RULE) {
                        size_t owner = resolve_named_symbol_id(a, a->program, d->rule_target.name_token);
                        const AstProgram *interface_unit = a->model->symbols[id].source_program;
                        int owns_interface = same_package(a->program, interface_unit);
                        int owns_type = owner < a->model->symbol_count &&
                                        same_package(a->program, a->model->symbols[owner].source_program);
                        if (owner < a->model->symbol_count && a->model->symbols[owner].declaration &&
                            a->model->symbols[owner].declaration->generic_origin &&
                            find_auto_rule(a->model, a->model->symbols[owner].source_program,
                                           a->model->symbols[owner].declaration, id))
                            semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                           "Duplicate auto interface rule for target type");
                        if (!owns_interface && !owns_type)
                            semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                           "External rule package must own the type or auto interface");
                        if (d->rule_target.arguments || (owner == AST_SYMBOL_NONE &&
                                                         primitive_type(a->program, &d->rule_target) == TYPE_UNKNOWN))
                            semantic_error(a, d->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                           "External rule requires an existing concrete type");
                    }
                }
                for (AstAutoCondition *c = r->conditions; c; c = c->next) {
                    if (!contains_type_parameter(a->program, &c->type, d)) {
                        normalize_generic_type(a, &c->type, 0);
                        validate_array_shape(a, &c->type);
                    }
                    if (!contains_type_parameter(a->program, &c->type, d) &&
                        !known_declared_type(a, &c->type))
                        semantic_error(a, c->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                       "Unknown type in auto rule condition");
                    for (AstInterfaceBound *b = c->bounds; b; b = b->next) {
                        size_t id = resolve_named_symbol_id(a, a->program, b->name_token);
                        if (id >= a->model->symbol_count || a->model->symbols[id].kind != SEMANTIC_SYMBOL_INTERFACE)
                            semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                                           "Unknown interface in auto rule condition");
                    }
                }
            }
        }
    }
    a->program = saved;
}

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
        expected->borrow_kind != actual->borrow_kind ||
        expected->pointer_depth != actual->pointer_depth ||
        expected->outer_pointer_depth != actual->outer_pointer_depth ||
        expected->is_array != actual->is_array || expected->is_slice != actual->is_slice ||
        expected->resolved_array_length != actual->resolved_array_length)
        return 0;
    if (expected->kind == AST_TYPE_FUNCTION) {
        if (expected->callable_mode != actual->callable_mode ||
            expected->is_native_function != actual->is_native_function ||
            !expected->function_return_type || !actual->function_return_type)
            return 0;
        if (expected->function_generic_parameters || actual->function_generic_parameters)
            return ast_polymorphic_callable_compatible(interface_unit, expected, actual_unit, actual) &&
                   ast_polymorphic_callable_compatible(actual_unit, actual, interface_unit, expected);
        const AstTypeArgument *left = expected->function_parameters;
        const AstTypeArgument *right = actual->function_parameters;
        for (; left && right; left = left->next, right = right->next)
            if (!interface_type_matches_depth(model, interface_unit, &left->type,
                                              actual_unit, &right->type, self_unit, self,
                                              substitution, depth + 1))
                return 0;
        return !left && !right && interface_type_matches_depth(model, interface_unit,
                                                               expected->function_return_type, actual_unit,
                                                               actual->function_return_type,
                                                               self_unit, self, substitution, depth + 1);
    }
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
    Analyzer lookup = {.model = (SemanticModel *) model};
    size_t expected_symbol = resolve_named_symbol_id(&lookup, interface_unit, expected->name_token);
    size_t actual_type_symbol = resolve_named_symbol_id(&lookup, actual_unit, actual->name_token);
    if (strcmp(expected_name, actual_name) &&
        (expected_symbol == AST_SYMBOL_NONE || expected_symbol != actual_type_symbol)) {
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
                    ? actual_symbol->declaration->generic_origin
                    : NULL;
        if (origin == NULL ||
            (expected_symbol != origin->resolved_symbol_id &&
             strcmp(expected_name, ast_program_lexeme(
                        actual_symbol->source_program,
                        origin->name_token))))
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
                                          depth + 1))
            return 0;
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
        !interface->declaration)
        return 0;
    /* Auto properties never create implementations, vtables or erased values. */
    if (interface->declaration->is_auto_interface) return 0;
    for (const AstDeclarationNode *required = interface->declaration->as.interface_decl.methods;
         required; required = required->next)
        if (interface_method(model, required->resolved_symbol_id, struct_id,
                             NULL) == AST_SYMBOL_NONE)
            return 0;
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
        !owner->declaration)
        return AST_SYMBOL_NONE;
    AstType self = {
        .kind = AST_TYPE_NAMED, .name_token = owner->name_token,
        .array_length_token = AST_TOKEN_NONE
    };
    const char *required_name = ast_program_lexeme(required->source_program, required->name_token);
    size_t required_parameters = parameter_count(required->declaration);
    const AstDeclarationNode *methods = owner->kind == SEMANTIC_SYMBOL_STRUCT
                                            ? owner->declaration->as.struct_decl.methods
                                            : owner->declaration->as.enum_decl.methods;
    int consuming_closure = owner->declaration->is_closure_environment &&
                            owner->declaration->closure_consuming_invoke != NULL;
    if (consuming_closure) methods = owner->declaration->closure_consuming_invoke;
    for (const AstDeclarationNode *method = methods;
         method; method = method->next) {
        if (method->resolved_symbol_id >= model->symbol_count) continue;
        const SemanticSymbol *actual = &model->symbols[method->resolved_symbol_id];
        Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) actual->source_program};
        if (!semantic_method_constraints_satisfied(&lookup, actual->source_program, method)) continue;
        if (!strcmp(required_name, "__invoke") && owner->declaration->is_closure_environment) {
            unsigned mode = consuming_closure ? 2U : semantic_function_mutates_receiver(&lookup, actual->id) ? 1U : 0U;
            if (mode > required->declaration->as.function.receiver_mode) continue;
        }
        /* Erasure and generic bounds must not weaken an explicit receiver
           contract, regardless of the method name. A once interface may
           consume a shared/mutable implementation and clean it up afterward. */
        if (actual->declaration && !consuming_closure &&
            actual->declaration->as.function.receiver_mode > required->declaration->as.function.receiver_mode)
            continue;
        if (actual->kind != SEMANTIC_SYMBOL_FUNCTION || (!consuming_closure && actual->owner_symbol_id != struct_id) ||
            !actual->declaration ||
            (!consuming_closure && actual->declaration->as.function.is_static !=
             required->declaration->as.function.is_static) ||
            (!consuming_closure && !same_name(actual->source_program, actual->name_token, required_name)) ||
            (consuming_closure && strcmp(required_name, "__invoke")) ||
            parameter_count(actual->declaration) != required_parameters + (consuming_closure ? 1U : 0U))
            continue;
        if (!interface_type_matches(model, required->source_program, required->declaration->as.function.return_type,
                                    actual->source_program, &actual->declaration->as.function.return_type,
                                    owner->source_program, &self,
                                    substitution))
            continue;
        const AstParameter *expected = required->declaration->as.function.parameters;
        const AstParameter *provided = actual->declaration->as.function.parameters;
        if (consuming_closure && provided) provided = provided->next;
        for (; expected && provided; expected = expected->next, provided = provided->next)
            if (!interface_type_matches(model, required->source_program, expected->type, actual->source_program,
                                        &provided->type, owner->source_program, &self,
                                        substitution))
                break;
        if (!expected && !provided) return actual->id;
    }
    return AST_SYMBOL_NONE;
}

size_t semantic_interface_method(const SemanticModel *model,
                                 size_t interface_method_id,
                                 size_t struct_id) {
    return interface_method(model, interface_method_id, struct_id, NULL);
}
