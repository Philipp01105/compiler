#include "semantic_internal.h"
#include "generics.h"
#include "errorHandler.h"

#include <stdio.h>
#include <stdlib.h>
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
    AstType *inferred_types;
    unsigned char *inferred;
    const AstDeclarationNode *selected_method;
    int *incomplete;
} InterfaceSubstitution;

/* Invert the source wrappers around a parameter. T may itself be a reference,
 * pointer, callable or container; compare/infer its entire remaining shape. */
static int interface_argument_shape(const AstType *pattern, const AstType *actual, AstType *argument) {
    *argument = *actual;
    if (pattern->borrow_kind != AST_BORROW_NONE) {
        if (argument->borrow_kind != pattern->borrow_kind) return 0;
        if (argument->reference_type) *argument = *argument->reference_type;
        else {
            argument->borrow_kind = AST_BORROW_NONE;
            argument->lifetime_token = AST_TOKEN_NONE;
        }
    }
    if (pattern->is_array || pattern->is_slice) {
        if (argument->is_array != pattern->is_array || argument->is_slice != pattern->is_slice ||
            (pattern->is_array && argument->resolved_array_length != pattern->resolved_array_length) ||
            argument->outer_pointer_depth != pattern->outer_pointer_depth) return 0;
        if (argument->element_type) *argument = *argument->element_type;
        else {
            argument->is_array = argument->is_slice = 0;
            argument->resolved_array_length = 0;
            argument->array_length_token = AST_TOKEN_NONE;
            argument->outer_pointer_depth = 0;
        }
    } else if (argument->is_array || argument->is_slice) {
        unsigned pointers = pattern->pointer_depth + pattern->outer_pointer_depth;
        if (argument->outer_pointer_depth < pointers) return 0;
        argument->outer_pointer_depth -= pointers;
        return 1;
    }
    if (argument->pointer_depth < pattern->pointer_depth ||
        argument->outer_pointer_depth < pattern->outer_pointer_depth) return 0;
    argument->pointer_depth -= pattern->pointer_depth;
    argument->outer_pointer_depth -= pattern->outer_pointer_depth;
    return 1;
}

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
    if (depth > 64) return 0;
    if (substitution && expected->kind == AST_TYPE_NAMED && !expected->arguments &&
        !expected->reference_type && !expected->element_type) {
        size_t index = 0;
        const AstTypeArgument *argument = substitution->arguments;
        for (const AstGenericParameter *parameter = substitution->parameters; parameter;
             parameter = parameter->next, index++, argument = argument ? argument->next : NULL) {
            if (strcmp(ast_program_lexeme(interface_unit, expected->name_token),
                       ast_program_lexeme(interface_unit, parameter->name_token))) continue;
            AstType remaining;
            if (!interface_argument_shape(expected, actual, &remaining)) return 0;
            if (substitution->inferred) {
                if (index >= DMM_MAX_TYPE_PARAMETERS) return 0;
                if (substitution->inferred[index])
                    return ast_concrete_type_equal(substitution->argument_unit, &substitution->inferred_types[index],
                                                   actual_unit, &remaining);
                substitution->inferred_types[index] = remaining;
                substitution->inferred[index] = 1;
                return 1;
            }
            return argument && ast_concrete_type_equal(substitution->argument_unit, &argument->type,
                                                       actual_unit, &remaining);
        }
    }
    if (expected->kind != actual->kind ||
        expected->borrow_kind != actual->borrow_kind ||
        expected->pointer_depth != actual->pointer_depth ||
        expected->outer_pointer_depth != actual->outer_pointer_depth ||
        expected->is_array != actual->is_array || expected->is_slice != actual->is_slice ||
        expected->resolved_array_length != actual->resolved_array_length)
        return 0;
    if ((expected->reference_type != NULL) != (actual->reference_type != NULL) ||
        (expected->element_type != NULL) != (actual->element_type != NULL)) return 0;
    if (expected->reference_type)
        return interface_type_matches_depth(model, interface_unit, expected->reference_type, actual_unit,
                                            actual->reference_type, self_unit, self, substitution, depth + 1);
    if (expected->element_type)
        return interface_type_matches_depth(model, interface_unit, expected->element_type, actual_unit,
                                            actual->element_type, self_unit, self, substitution, depth + 1);
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

static size_t interface_declaration_symbol(const SemanticModel *model, const AstDeclarationNode *declaration) {
    if (declaration->resolved_symbol_id < model->symbol_count &&
        model->symbols[declaration->resolved_symbol_id].declaration == declaration)
        return declaration->resolved_symbol_id;
    size_t scope = semantic_source_scope(model, declaration);
    return scope < model->source_scope_count ? model->source_scopes[scope].owner : AST_SYMBOL_NONE;
}

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
        if (interface_method(model, interface_declaration_symbol(model, required), struct_id,
                             NULL) == AST_SYMBOL_NONE)
            return 0;
    return 1;
}

static int implements_specialized_interface(
    const SemanticModel *model, size_t interface_id, size_t owner_id,
    const AstProgram *argument_unit, const AstType *interface_type, unsigned wildcard_parameters) {
    if (!model || !argument_unit || interface_id >= model->symbol_count ||
        owner_id >= model->symbol_count || interface_type == NULL)
        return 0;
    const SemanticSymbol *interface = &model->symbols[interface_id];
    if (interface->kind != SEMANTIC_SYMBOL_INTERFACE ||
        interface->declaration == NULL || interface->declaration->is_auto_interface)
        return 0;
    const AstGenericParameter *parameter = interface->declaration->generic_parameters;
    const AstTypeArgument *argument = interface_type->arguments;
    for (; parameter && argument; parameter = parameter->next, argument = argument->next) {}
    if (parameter || argument) return 0;
    int bounded = 0;
    for (parameter = interface->declaration->generic_parameters; parameter; parameter = parameter->next)
        bounded |= parameter->bounds != NULL;
    if (bounded) {
        SemanticModel *checking = (SemanticModel *)model;
        if (checking->interface_check_depth >= 64) {
            checking->interface_contexts_incomplete = 1;
            return 0;
        }
        AstType arguments[DMM_MAX_TYPE_PARAMETERS];
        AstGenericParameter proven_parameters[DMM_MAX_TYPE_PARAMETERS];
        size_t count = 0;
        for (argument = interface_type->arguments; argument; argument = argument->next) {
            if (count == DMM_MAX_TYPE_PARAMETERS) return 0;
            arguments[count++] = argument->type;
        }
        const AstDeclarationNode *checking_declaration = interface->declaration;
        AstDeclarationNode proven_declaration = *interface->declaration;
        if (model->symbols[owner_id].source_only || wildcard_parameters) {
            const SemanticSymbol *owner = &model->symbols[owner_id];
            const AstGenericParameter *required_parameter = interface->declaration->generic_parameters;
            InterfaceSubstitution proof = {.parameters = required_parameter, .arguments = interface_type->arguments,
                                            .argument_unit = argument_unit};
            AstType self = {.kind = AST_TYPE_NAMED, .name_token = owner->name_token};
            for (size_t i = 0; i < count; i++, required_parameter = required_parameter->next) {
                proven_parameters[i] = *required_parameter;
                proven_parameters[i].next = i + 1 < count ? &proven_parameters[i + 1] : NULL;
                /* This parameter is absent from every required signature. The
                 * relation quantifies over valid interface arguments, retaining
                 * its declared bounds as the domain rather than inventing a witness. */
                if (wildcard_parameters & (1u << i)) {
                    proven_parameters[i].bounds = NULL;
                    continue;
                }
                if (!owner->source_only) continue;
                size_t scope = semantic_source_scope(model, owner->declaration);
                size_t binder = semantic_source_binding(model, argument_unit, scope, arguments[i].name_token,
                    SEMANTIC_SYMBOL_TYPE_PARAMETER, arguments[i].span.begin);
                if (binder >= model->symbol_count || arguments[i].kind != AST_TYPE_NAMED ||
                    arguments[i].borrow_kind || arguments[i].pointer_depth || arguments[i].outer_pointer_depth ||
                    arguments[i].is_array || arguments[i].is_slice || arguments[i].arguments) continue;
                const AstGenericParameter *provided_parameter = model->symbols[binder].node;
                for (const AstInterfaceBound *bound = required_parameter->bounds; bound; bound = bound->next) {
                    const AstInterfaceBound *provided = provided_parameter->bounds;
                    for (; provided; provided = provided->next)
                        if (interface_type_matches(model, interface->source_program, bound->type,
                            argument_unit, &provided->type, owner->source_program, &self, &proof)) break;
                    if (!provided) return 0;
                }
                proven_parameters[i].bounds = NULL;
            }
            proven_declaration.generic_parameters = count ? proven_parameters : NULL;
            checking_declaration = &proven_declaration;
        }
        Analyzer lookup = {.model = checking, .program = (AstProgram *)argument_unit};
        checking->interface_check_depth++;
        int satisfied = generic_bounds_satisfied(&lookup, interface->source_program,
                                                 checking_declaration, arguments);
        checking->interface_check_depth--;
        if (lookup.allocation_failed) checking->interface_contexts_incomplete = 1;
        if (!satisfied || lookup.allocation_failed) return 0;
    }
    InterfaceSubstitution substitution = {
        .parameters = interface->declaration->generic_parameters,
        .arguments = interface_type->arguments,
        .argument_unit = argument_unit
    };
    for (const AstDeclarationNode *required =
                 interface->declaration->as.interface_decl.methods;
         required != NULL; required = required->next)
        if (interface_method(model, interface_declaration_symbol(model, required), owner_id,
                             &substitution) == AST_SYMBOL_NONE)
            return 0;
    return 1;
}
int semantic_implements_specialized_interface(const SemanticModel *model, size_t interface_id, size_t owner_id,
                                               const AstProgram *argument_unit, const AstType *interface_type) {
    return implements_specialized_interface(model, interface_id, owner_id, argument_unit, interface_type, 0);
}
int semantic_implements_interface_context(const SemanticModel *model, const SemanticInterfaceContext *context,
                                           size_t owner_id) {
    if (!context) return 0;
    AstType type = {.arguments = context->arguments};
    return implements_specialized_interface(model, context->interface_id, owner_id, context->argument_program,
                                            &type, context->wildcard_parameters);
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
        if (substitution && substitution->selected_method && method != substitution->selected_method) continue;
        size_t method_id = interface_declaration_symbol(model, method);
        if (method_id >= model->symbol_count) continue;
        const SemanticSymbol *actual = &model->symbols[method_id];
        Analyzer lookup = {.model = (SemanticModel *) model, .program = (AstProgram *) actual->source_program};
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
        if (expected || provided) continue;
        if (actual->source_only && (method->generic_parameters || method->lifetime_parameters || method->where_conditions)) {
            if (substitution && substitution->incomplete) *substitution->incomplete = 1;
            continue;
        }
        if (!semantic_method_constraints_satisfied(&lookup, actual->source_program, method)) continue;
        if (!strcmp(required_name, "__invoke") && owner->declaration->is_closure_environment) {
            unsigned mode = consuming_closure ? 2U : semantic_function_mutates_receiver(&lookup, actual->id) ? 1U : 0U;
            if (mode > required->declaration->as.function.receiver_mode) continue;
        }
        if (!consuming_closure) {
            unsigned receiver_mode;
            if (!semantic_receiver_contract(model, actual->id, &receiver_mode)) {
                if (substitution && substitution->incomplete) *substitution->incomplete = 1;
                continue;
            }
            if (receiver_mode > required->declaration->as.function.receiver_mode) continue;
        }
        return actual->id;
    }
    return AST_SYMBOL_NONE;
}

size_t semantic_interface_method(const SemanticModel *model,
                                 size_t interface_method_id,
                                 size_t struct_id) {
    return interface_method(model, interface_method_id, struct_id, NULL);
}

size_t semantic_specialized_interface_method(const SemanticModel *model, size_t interface_method_id,
                                            size_t struct_id, const AstProgram *argument_program,
                                            const AstType *interface_type) {
    if (!model || !argument_program || !interface_type || interface_method_id >= model->symbol_count)
        return AST_SYMBOL_NONE;
    size_t owner = model->symbols[interface_method_id].owner_symbol_id;
    if (owner >= model->symbol_count || !model->symbols[owner].declaration)
        return AST_SYMBOL_NONE;
    const AstGenericParameter *parameter = model->symbols[owner].declaration->generic_parameters;
    const AstTypeArgument *argument = interface_type->arguments;
    for (; parameter && argument; parameter = parameter->next, argument = argument->next) {}
    if (parameter || argument) return AST_SYMBOL_NONE;
    InterfaceSubstitution substitution = {
        .parameters = model->symbols[owner].declaration->generic_parameters,
        .arguments = interface_type->arguments,
        .argument_unit = argument_program
    };
    return interface_method(model, interface_method_id, struct_id, &substitution);
}

static void record_interface_context(SemanticModel *model, size_t interface_id,
                                     const AstProgram *argument_program, const AstType *interface_type,
                                     unsigned wildcard_parameters, size_t source_owner) {
    if (!model || !argument_program || !interface_type) return;
    for (size_t i = 0; i < model->interface_context_count; i++) {
        const SemanticInterfaceContext *context = &model->interface_contexts[i];
        if (context->interface_id != interface_id || context->wildcard_parameters != wildcard_parameters ||
            context->source_owner != source_owner) continue;
        const AstTypeArgument *left = context->arguments, *right = interface_type->arguments;
        for (; left && right; left = left->next, right = right->next)
            if (!ast_concrete_type_equal(context->argument_program, &left->type,
                                         argument_program, &right->type)) break;
        if (!left && !right) return;
    }
    size_t count = 0;
    for (const AstTypeArgument *argument = interface_type->arguments; argument; argument = argument->next)
        count++;
    if (model->interface_context_count >= 4096 || count > DMM_MAX_TYPE_PARAMETERS) {
        model->interface_contexts_incomplete = 1;
        return;
    }
    AstTypeArgument *arguments = count ? calloc(count, sizeof(*arguments)) : NULL;
    if (count && !arguments) {
        model->interface_contexts_incomplete = 1;
        return;
    }
    SemanticInterfaceContext *grown = realloc(model->interface_contexts,
        (model->interface_context_count + 1) * sizeof(*grown));
    if (!grown) {
        free(arguments);
        model->interface_contexts_incomplete = 1;
        return;
    }
    size_t index = 0;
    for (const AstTypeArgument *argument = interface_type->arguments; argument; argument = argument->next, index++) {
        arguments[index].type = argument->type;
        arguments[index].next = index + 1 < count ? &arguments[index + 1] : NULL;
    }
    model->interface_contexts = grown;
    grown[model->interface_context_count++] =
        (SemanticInterfaceContext){interface_id, argument_program, arguments, wildcard_parameters, source_owner};
}

void semantic_record_interface_context(SemanticModel *model, size_t interface_id,
                                       const AstProgram *argument_program, const AstType *interface_type) {
    record_interface_context(model, interface_id, argument_program, interface_type, 0, AST_SYMBOL_NONE);
}

typedef struct {
    SemanticModel *model;
    size_t interface_id;
    size_t owner_id;
    size_t parameter_count;
    size_t *steps;
    int incomplete;
} InterfaceInference;

static void infer_interface_methods(InterfaceInference *proof, const AstDeclarationNode *required,
                                     const AstType *types, const unsigned char *inferred, unsigned depth) {
    if (++*proof->steps > 50000 || depth > 256) {
        proof->incomplete = 1;
        return;
    }
    const SemanticSymbol *interface = &proof->model->symbols[proof->interface_id];
    const SemanticSymbol *owner = &proof->model->symbols[proof->owner_id];
    if (!required) {
        AstTypeArgument arguments[DMM_MAX_TYPE_PARAMETERS] = {0};
        unsigned wildcards = 0;
        Analyzer lookup = {.model = proof->model, .program = (AstProgram *)owner->source_program};
        const AstGenericParameter *parameter = interface->declaration->generic_parameters;
        for (size_t i = 0; i < proof->parameter_count; i++, parameter = parameter->next) {
            arguments[i].type = types[i];
            arguments[i].next = i + 1 < proof->parameter_count ? &arguments[i + 1] : NULL;
            if (inferred[i]) continue;
            /* An unused parameter quantifies over valid interface arguments. */
            wildcards |= 1u << i;
            arguments[i].type = (AstType){.kind = AST_TYPE_NAMED,
                .name_token = concrete_token(&lookup, TOKEN_IDENTIFIER,
                    ast_program_lexeme(interface->source_program, parameter->name_token)),
                .array_length_token = AST_TOKEN_NONE, .lifetime_token = AST_TOKEN_NONE};
        }
        if (lookup.allocation_failed) {
            proof->incomplete = 1;
            return;
        }
        AstType specialized = {.arguments = proof->parameter_count ? arguments : NULL};
        if (implements_specialized_interface(proof->model, proof->interface_id, proof->owner_id,
                                              owner->source_program, &specialized, wildcards))
            record_interface_context(proof->model, proof->interface_id, owner->source_program,
                                      &specialized, wildcards, owner->source_only ? proof->owner_id : AST_SYMBOL_NONE);
        else if (owner->source_only || wildcards) proof->incomplete = 1;
        return;
    }
    const AstDeclarationNode *methods = owner->kind == SEMANTIC_SYMBOL_STRUCT
        ? owner->declaration->as.struct_decl.methods : owner->declaration->as.enum_decl.methods;
    for (const AstDeclarationNode *method = methods; method; method = method->next) {
        if (++*proof->steps > 50000) { proof->incomplete = 1; return; }
        size_t method_id = interface_declaration_symbol(proof->model, method);
        if (method_id >= proof->model->symbol_count) continue;
        const SemanticSymbol *actual = &proof->model->symbols[method_id];
        if (actual->source_program != owner->source_program) { proof->incomplete = 1; continue; }
        AstType alternatives[DMM_MAX_TYPE_PARAMETERS];
        unsigned char known[DMM_MAX_TYPE_PARAMETERS];
        memcpy(alternatives, types, sizeof(alternatives));
        memcpy(known, inferred, sizeof(known));
        InterfaceSubstitution substitution = {
            .parameters = interface->declaration->generic_parameters,
            .argument_unit = owner->source_program,
            .inferred_types = alternatives, .inferred = known, .selected_method = method,
            .incomplete = &proof->incomplete
        };
        if (interface_method(proof->model, interface_declaration_symbol(proof->model, required), proof->owner_id, &substitution)
            != AST_SYMBOL_NONE)
            infer_interface_methods(proof, required->next, alternatives, known, depth + 1);
    }
}

void semantic_discover_interface_contexts(SemanticModel *model) {
    if (!model || model->error_count || model->interface_discovery_coverage) return;
    size_t count = model->symbol_count;
    model->interface_discovery_coverage = calloc(count ? count : 1, 1);
    if (!model->interface_discovery_coverage) { model->interface_contexts_incomplete = 1; return; }
    model->interface_discovery_count = count;
    size_t steps = 0;
    for (size_t i = 0; i < count; i++) {
        if (steps > 50000) break;
        const SemanticSymbol *interface = &model->symbols[i];
        if (interface->kind != SEMANTIC_SYMBOL_INTERFACE || !interface->declaration ||
            interface->declaration->is_auto_interface || !interface->declaration->generic_parameters) continue;
        size_t parameters = 0;
        for (const AstGenericParameter *p = interface->declaration->generic_parameters; p; p = p->next) parameters++;
        int complete = parameters <= DMM_MAX_TYPE_PARAMETERS;
        for (size_t j = 0; parameters <= DMM_MAX_TYPE_PARAMETERS && j < count; j++) {
            const SemanticSymbol *owner = &model->symbols[j];
            if ((owner->kind != SEMANTIC_SYMBOL_STRUCT && owner->kind != SEMANTIC_SYMBOL_ENUM) ||
                !owner->declaration || owner->declaration->is_closure_environment) continue;
            if (++steps > 50000) { complete = 0; break; }
            if (owner->source_only) {
                const AstDeclarationNode *required = interface->declaration->as.interface_decl.methods;
                const AstDeclarationNode *methods = owner->kind == SEMANTIC_SYMBOL_STRUCT
                    ? owner->declaration->as.struct_decl.methods : owner->declaration->as.enum_decl.methods;
                int possible = 1;
                for (; possible && required; required = required->next) {
                    const AstDeclarationNode *method = methods;
                    for (; method; method = method->next) {
                        if (++steps > 50000) break;
                        if (parameter_count(method) == parameter_count(required) &&
                            !strcmp(ast_program_lexeme(owner->source_program, method->name_token),
                                    ast_program_lexeme(interface->source_program, required->name_token))) break;
                    }
                    if (steps > 50000) { complete = 0; break; }
                    possible = method != NULL;
                }
                if (!possible) continue;
            }
            AstType types[DMM_MAX_TYPE_PARAMETERS] = {0};
            unsigned char known[DMM_MAX_TYPE_PARAMETERS] = {0};
            InterfaceInference proof = {model, i, j, parameters, &steps, 0};
            infer_interface_methods(&proof, interface->declaration->as.interface_decl.methods, types, known, 0);
            if (proof.incomplete) complete = 0;
        }
        model->interface_discovery_coverage[i] = complete;
    }
    if (steps > 50000 || model->symbol_count != count) model->interface_contexts_incomplete = 1;
}
