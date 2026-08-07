#include "semantic_internal.h"
#include "generics.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unit-valued sum payloads occupy no storage. Erase their pattern binding and
   constructor argument together, including the standard Result branch method. */
static void erase_unit_expression(AstProgram *unit, AstExpression *e,const char *binding) {
    if(!e) return;
    int erased=0;
    AstExpression **link=&e->arguments;
    while(*link) {
        AstExpression *arg=*link;
        if(arg->kind==AST_EXPR_NAME && !strcmp(ast_program_lexeme(unit,arg->value_token),binding))
            { *link=arg->next; erased=1; }
        else { erase_unit_expression(unit,arg,binding); link=&arg->next; }
    }
    erase_unit_expression(unit,e->left,binding); erase_unit_expression(unit,e->right,binding);
    if(erased && !e->arguments && e->kind==AST_EXPR_CALL && e->left) {
        AstExpression *next=e->next; *e=*e->left; e->next=next;
    }
}
static void erase_unit_patterns(AstProgram *unit,AstStatement *s,const char *variant) {
    for(;s;s=s->next) {
        for(AstMatchArm *arm=s->match_arms;arm;arm=arm->next) {
            if(!arm->wildcard && !strcmp(ast_program_lexeme(unit,arm->variant_token),variant) && arm->bindings && !arm->bindings->next) {
                const char *binding=ast_program_lexeme(unit,arm->bindings->name_token);
                for(AstStatement *body=arm->body;body;body=body->next) {
                    erase_unit_expression(unit,body->value,binding);
                    erase_unit_expression(unit,body->expression,binding);
                    for(AstStatement *inner=body->body;inner;inner=inner->next) {
                        erase_unit_expression(unit,inner->value,binding);
                        erase_unit_expression(unit,inner->expression,binding);
                    }
                }
                arm->bindings=NULL;
            }
            erase_unit_patterns(unit,arm->body,variant);
        }
        erase_unit_patterns(unit,s->body,variant); erase_unit_patterns(unit,s->else_body,variant);
    }
}

size_t concrete_token(Analyzer *analyzer, TokenType kind, const char *text) {
    AstProgram *p = analyzer->program;
    for (size_t i = 0; i < p->token_count; i++)
        if (p->tokens[i].type == kind && !strcmp(p->tokens[i].lexeme, text)) return i;
    const char *interned = string_interner_intern(p->strings, text);
    if (!interned || p->token_count >= SIZE_MAX / sizeof(AstToken) - 1) {
        analyzer->allocation_failed = 1;
        return AST_TOKEN_NONE;
    }
    AstToken *tokens = realloc(p->tokens, (p->token_count + 1) * sizeof(*tokens));
    if (!tokens) {
        analyzer->allocation_failed = 1;
        return AST_TOKEN_NONE;
    }
    p->tokens = tokens;
    size_t index = p->token_count++;
    tokens[index] = (AstToken)
    {
        .type = kind,.lexeme = interned
    };
    return index;
}

static AstType expression_shape_copy(Analyzer *analyzer,
                                     const AstType *shape,
                                     const AstType *base) {
    AstType result = *shape;
    result.kind = base->kind;
    result.name_token = base->name_token;
    result.arguments = base->arguments;
    if (result.is_array) {
        char length[32];
        snprintf(length, sizeof(length), "%zu",
                 result.resolved_array_length);
        result.array_length_token = concrete_token(analyzer, TOKEN_NUMBER,
                                                   length);
    } else result.array_length_token = AST_TOKEN_NONE;
    result.element_type = NULL;
    if (shape->element_type != NULL) {
        AstType *element = ast_program_alloc(analyzer->program,
                                            sizeof(*element));
        if (element == NULL) {
            analyzer->allocation_failed = 1;
            result.invalid_substitution = 1;
        } else {
            *element = expression_shape_copy(analyzer, shape->element_type,
                                             base);
            result.element_type = element;
        }
    }
    return result;
}

AstType inferred_argument_type(Analyzer *analyzer, const AstExpression *value) {
    if (value->has_resolved_ast_type && (value->resolved_ast_type.kind == AST_TYPE_FUTURE ||
        value->resolved_ast_type.kind == AST_TYPE_JOIN || value->resolved_ast_type.kind == AST_TYPE_EXECUTOR))
        return argument_type_copy(analyzer,
            value->resolved_type_program != NULL ? value->resolved_type_program : analyzer->program,
            value->resolved_ast_type);
    static const char *names[] = {DMM_TYPE_NAMES};
    AstType t = {
        .kind = AST_TYPE_NAMED, .array_length_token = AST_TOKEN_NONE,
        .pointer_depth = value->resolved_pointer_depth,
        .outer_pointer_depth = value->resolved_outer_pointer_depth,
        .is_array = value->resolved_is_array, .is_slice = value->resolved_is_slice,
        .resolved_array_length = value->resolved_array_length
    };
    if (value->resolved_named_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *type = &analyzer->model->symbols[value->resolved_named_symbol_id];
        char canonical[4096];
        snprintf(canonical, sizeof(canonical), "%s::%s",
                 type->source_program->module_identity ? type->source_program->module_identity : "",
                 ast_program_lexeme(type->source_program, type->name_token));
        t.name_token = concrete_token(analyzer, TOKEN_IDENTIFIER,
                                      type->source_program->package
                                          ? canonical
                                          : ast_program_lexeme(type->source_program, type->name_token));
    } else if (value->resolved_type < TYPE_UNKNOWN)
        t.name_token = concrete_token(analyzer,
                                      (TokenType) ((int) TOKEN_TYPE_INT + (int) value->resolved_type),
                                      names[value->resolved_type]);
    else t.name_token = AST_TOKEN_NONE;
    if (t.is_array) {
        char length[32];
        snprintf(length, sizeof(length), "%zu", t.resolved_array_length);
        t.array_length_token = concrete_token(analyzer, TOKEN_NUMBER, length);
    }
    if (value->has_resolved_ast_type)
        t = expression_shape_copy(analyzer, &value->resolved_ast_type, &t);
    return t;
}

void normalize_generic_type(Analyzer *analyzer, AstType *type, unsigned depth) {
    if (depth > 64) {
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC,
                       ERR_SEM_COMPLEXITY_LIMIT, "Generic type nesting exceeds 64 instantiations");
        return;
    }
    if (type->kind == AST_TYPE_EXECUTOR) {
        if (!semantic_async_enabled(analyzer))
            semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_INVALID_DECLARATION, "Executor requires the async manifest feature");
        return;
    }
    if (type->kind == AST_TYPE_FUTURE || type->kind == AST_TYPE_JOIN) {
        if (!semantic_async_enabled(analyzer))
            semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_INVALID_DECLARATION,
                           "Future types require the async manifest feature");
        if (type->arguments == NULL || type->arguments->next != NULL) {
            semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_INVALID_OPERATION, "Future requires exactly one result type");
            return;
        }
        normalize_generic_type(analyzer, &type->arguments->type, depth + 1);
        return;
    }
    if (type->invalid_substitution) {
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Generic substitution creates an unsupported nested array or slice type");
        return;
    }
    if (type->element_type != NULL)
        normalize_generic_type(analyzer, type->element_type, depth + 1);
    if (type->kind == AST_TYPE_FUNCTION) {
        for (AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next)
            normalize_generic_type(analyzer, &parameter->type, depth + 1);
        if (type->function_return_type)
            normalize_generic_type(analyzer, type->function_return_type, depth + 1);
        return;
    }
    if (type->is_array && analyzer->model->symbol_count != 0)
        validate_array_shape(analyzer, type);
    if (!type->arguments) {
        if (type->name_token < analyzer->program->token_count && strstr(
                ast_program_lexeme(analyzer->program, type->name_token), "::")) return;
        if (type->kind == AST_TYPE_NAMED && type->name_token < analyzer->program->token_count &&
            analyzer->program->tokens[type->name_token].type == TOKEN_IDENTIFIER && analyzer->program->package) {
            const char *name = ast_program_lexeme(analyzer->program, type->name_token);
            const DmmPackage *target = lookup_package(analyzer->program, &name);
            if (target)
                for (size_t f = 0; f < target->file_count; f++)
                    for (AstDeclarationNode *d = target->files[f]->root; d; d = d->next)
                        if ((d->kind == AST_DECL_STRUCT || d->kind == AST_DECL_ENUM) && !strcmp(
                                name, ast_program_lexeme(target->files[f], d->name_token))) {
                            if (target != analyzer->program->package && !d->is_public) {
                                semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                                               "Type is private to its defining package");
                                return;
                            }
                            char canonical[4096];
                            snprintf(canonical, sizeof(canonical), "%s::%s", target->path, name);
                            type->name_token = concrete_token(analyzer, TOKEN_IDENTIFIER, canonical);
                            return;
                        }
        }
        return;
    }
    if (depth > 64) {
        semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                       "Generic type nesting exceeds 64 instantiations");
        return;
    }
    AstType arguments[DMM_MAX_TYPE_PARAMETERS];
    size_t count = 0;
    for (AstTypeArgument *a = type->arguments; a; a = a->next) {
        if (count == DMM_MAX_TYPE_PARAMETERS) return;
        normalize_generic_type(analyzer, &a->type, depth + 1);
        arguments[count++] = a->type;
    }
    AstProgram *root = (AstProgram *) analyzer->model->program;
    const char *name = ast_program_lexeme(analyzer->program, type->name_token);
    const int qualified = strchr(name, '.') != NULL || strstr(name, "::") != NULL;
    const DmmPackage *target = lookup_package(analyzer->program, &name);
    for (unsigned fallback = 0; fallback < 2; fallback++)
    for (size_t i = 0; i <= root->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? root : root->owned_imports[i - 1];
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            int local = target ? unit->package == target : same_package(analyzer->program, unit);
            if (fallback == 0 ? !local : (qualified || local || !d->is_async_builtin)) continue;
            if ((d->kind != AST_DECL_STRUCT && d->kind != AST_DECL_ENUM) || !d->generic_parameters ||
                strcmp(name, ast_program_lexeme(unit, d->name_token)))
                continue;
            if (!same_package(analyzer->program, unit) && !d->is_public) {
                semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                               "Generic type is private to its defining package");
                return;
            }
            size_t expected = 0;
            for (AstGenericParameter *g = d->generic_parameters; g; g = g->next) expected++;
            if (expected != count) {
                semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                               "Generic type argument count does not match declaration");
                return;
            }
            if (analyzer->model->symbol_count && !generic_bounds_satisfied(analyzer, unit, d, arguments)) {
                semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Generic aggregate type arguments do not satisfy interface bounds");
                return;
            }
            AstDeclarationNode *instance = ast_specialize_function(unit, d, arguments, count, analyzer->program);
            if (!instance) {
                semantic_error(analyzer, type->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                               "Generic aggregate specialization exceeds deterministic limits");
                return;
            }
            instance->is_async_builtin = d->is_async_builtin;
            if (d->is_async_builtin && !strcmp(name, "Result") &&
                primitive_type(analyzer->program, &arguments[0]) == TYPE_VOID &&
                !arguments[0].pointer_depth && !arguments[0].borrow_kind)
                instance->as.enum_decl.values->payload_types = NULL;
            char canonical[4096];
            snprintf(canonical, sizeof(canonical), "%s::%s", unit->module_identity ? unit->module_identity : "",
                     ast_program_lexeme(unit, instance->name_token));
            type->name_token = concrete_token(analyzer, TOKEN_IDENTIFIER,
                                              unit->package
                                                  ? canonical
                                                  : ast_program_lexeme(unit, instance->name_token));
            type->arguments = NULL;
            AstProgram *saved = analyzer->program;
            analyzer->program = unit;
            for (AstAutoRule *r = instance->auto_rules; r; r = r->next)
                for (AstAutoCondition *c = r->conditions; c; c = c->next)
                    normalize_generic_type(analyzer, &c->type, depth + 1);
            if (instance->kind == AST_DECL_STRUCT) {
                for (AstField *f = instance->as.struct_decl.fields; f; f = f->next)
                    normalize_generic_type(analyzer, &f->type, depth + 1);
                AstType self_type = {.kind = AST_TYPE_NAMED, .name_token = instance->name_token,
                                     .array_length_token = AST_TOKEN_NONE};
                for (AstDeclarationNode *m = instance->as.struct_decl.methods; m; m = m->next) {
                    replace_self_type(analyzer, &m->as.function.return_type, &self_type);
                    for (AstParameter *p = m->as.function.parameters; p; p = p->next)
                        replace_self_type(analyzer, &p->type, &self_type);
                    replace_self_statement(analyzer, m->as.function.body, &self_type);
                    normalize_function_types(analyzer, m);
                }
            } else {
                for (AstEnumValue *v = instance->as.enum_decl.values; v; v = v->next)
                    for (AstTypeArgument *p = v->payload_types; p; p = p->next)
                        normalize_generic_type(analyzer, &p->type, depth + 1);
                for(AstEnumValue *v=instance->as.enum_decl.values;v;v=v->next)
                    if(v->payload_types && !v->payload_types->next &&
                       primitive_type(unit,&v->payload_types->type)==TYPE_VOID &&
                       !v->payload_types->type.pointer_depth && !v->payload_types->type.borrow_kind) {
                        v->payload_types=NULL;
                        for(AstDeclarationNode *m=instance->as.enum_decl.methods;m;m=m->next)
                            erase_unit_patterns(unit,m->as.function.body,ast_program_lexeme(unit,v->name_token));
                    }
                AstType self_type = {.kind = AST_TYPE_NAMED, .name_token = instance->name_token,
                                     .array_length_token = AST_TOKEN_NONE};
                for (AstDeclarationNode *m = instance->as.enum_decl.methods; m; m = m->next) {
                    replace_self_type(analyzer, &m->as.function.return_type, &self_type);
                    for (AstParameter *p = m->as.function.parameters; p; p = p->next)
                        replace_self_type(analyzer, &p->type, &self_type);
                    replace_self_statement(analyzer, m->as.function.body, &self_type);
                    normalize_function_types(analyzer, m);
                }
            }
            if (instance->resolved_symbol_id == AST_SYMBOL_NONE && analyzer->model->symbol_count != 0) {
                add_global(analyzer, instance,
                           instance->kind == AST_DECL_STRUCT ? SEMANTIC_SYMBOL_STRUCT : SEMANTIC_SYMBOL_ENUM,
                           AST_TOKEN_NONE);
                if (instance->kind == AST_DECL_STRUCT) {
                    for (AstField *f = instance->as.struct_decl.fields; f; f = f->next)
                        add_member(analyzer, f->name_token, instance->name_token, f->type, SEMANTIC_SYMBOL_FIELD, f,
                                   &f->resolved_symbol_id);
                    for (AstDeclarationNode *m = instance->as.struct_decl.methods; m; m = m->next)
                        add_global(analyzer, m, SEMANTIC_SYMBOL_FUNCTION, instance->name_token);
                } else {
                    AstType enum_type = {
                        .kind = AST_TYPE_NAMED, .name_token = instance->name_token, .array_length_token = AST_TOKEN_NONE
                    };
                    for (AstEnumValue *v = instance->as.enum_decl.values; v; v = v->next)
                        add_member(analyzer, v->name_token, instance->name_token, enum_type, SEMANTIC_SYMBOL_ENUM_VALUE,
                                   v, &v->resolved_symbol_id);
                    for (AstDeclarationNode *m = instance->as.enum_decl.methods; m; m = m->next)
                        add_global(analyzer, m, SEMANTIC_SYMBOL_FUNCTION, instance->name_token);
                }
                /* Late specializations must be classified before their first use. */
                derive_type_properties(analyzer);
            }
            analyzer->program = saved;
            return;
        }
    }
    semantic_error(analyzer, type->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown generic type");
}

static void normalize_expression_types(Analyzer *analyzer, AstExpression *e) {
    for (; e; e = e->next) {
        if (e->explicit_type_arguments || e->explicit_generic_reference) {
            for (AstTypeArgument *argument = e->allocated_type.arguments; argument; argument = argument->next)
                normalize_generic_type(analyzer, &argument->type, 0);
        } else if (e->kind == AST_EXPR_NAME && e->allocated_type.arguments != NULL) {
            normalize_generic_type(analyzer, &e->allocated_type, 0);
            e->value_token = e->allocated_type.name_token;
        } else normalize_generic_type(analyzer, &e->allocated_type, 0);
        normalize_expression_types(analyzer, e->left);
        normalize_expression_types(analyzer, e->right);
        normalize_expression_types(analyzer, e->arguments);
        normalize_statement_types(analyzer, e->control);
    }
}

void normalize_statement_types(Analyzer *analyzer, AstStatement *s) {
    for (; s; s = s->next) {
        normalize_generic_type(analyzer, &s->type, 0);
        normalize_expression_types(analyzer, s->value);
        normalize_expression_types(analyzer, s->expression);
        normalize_expression_types(analyzer, s->condition);
        normalize_expression_types(analyzer, s->update);
        normalize_expression_types(analyzer, s->result);
        normalize_statement_types(analyzer, s->body);
        normalize_statement_types(analyzer, s->else_body);
        normalize_statement_types(analyzer, s->initializer);
        if (!s->value || (s->value->kind != AST_EXPR_TYPE_INFO &&
                          !(s->value->kind == AST_EXPR_MEMBER && same_name(
                                analyzer->program, s->value->value_token, "type"))))
            for (AstMatchArm *a = s->match_arms; a; a = a->next) normalize_statement_types(analyzer, a->body);
    }
}

void normalize_function_types(Analyzer *analyzer, AstDeclarationNode *d) {
    normalize_generic_type(analyzer, &d->as.function.return_type, 0);
    for (AstParameter *p = d->as.function.parameters; p; p = p->next)
        normalize_generic_type(analyzer, &p->type, 0);
    normalize_statement_types(analyzer, d->as.function.body);
}

AstDeclarationNode *find_language_declaration(const AstProgram *root, const AstProgram *file,
                                                     const char *name, AstDeclarationKind kind, AstProgram **source) {
    int canonical = strstr(name, "::") != NULL;
    const DmmPackage *target = lookup_package(file, &name);
    for (size_t i = 0; i <= root->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? (AstProgram *) root : root->owned_imports[i - 1];
        if (target ? unit->package != target : !same_package(file, unit)) continue;
        for (AstDeclarationNode *d = unit->root; d; d = d->next)
            if (d->kind == kind && !strcmp(ast_program_lexeme(unit, d->name_token), name)) {
                if (!canonical && !same_package(file, unit) && !d->is_public) {
                    error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_SEMANTIC,
                                 ERR_PACKAGE_PRIVATE, file->source_path, "Declaration '%s' is private to package '%s'",
                                 name, unit->module_identity);
                    return NULL;
                }
                if (source) *source = unit;
                return d;
            }
    }
    return NULL;
}

void replace_self_type(Analyzer *a, AstType *type, const AstType *self) {
    if (type->kind == AST_TYPE_NAMED && !strcmp(ast_program_lexeme(a->program, type->name_token), "Self"))
        type->name_token = self->name_token;
    for (AstTypeArgument *t = type->arguments; t; t = t->next) replace_self_type(a, &t->type, self);
}

static void replace_self_expression(Analyzer *a, AstExpression *e, const AstType *self) {
    for (; e; e = e->next) {
        replace_self_type(a, &e->allocated_type, self);
        replace_self_expression(a, e->left, self);
        replace_self_expression(a, e->right, self);
        replace_self_expression(a, e->arguments, self);
        replace_self_statement(a, e->control, self);
    }
}

void replace_self_statement(Analyzer *a, AstStatement *s, const AstType *self) {
    for (; s; s = s->next) {
        replace_self_type(a, &s->type, self);
        replace_self_expression(a, s->value, self);
        replace_self_expression(a, s->expression, self);
        replace_self_expression(a, s->condition, self);
        replace_self_expression(a, s->update, self);
        replace_self_expression(a, s->result, self);
        replace_self_statement(a, s->body, self);
        replace_self_statement(a, s->else_body, self);
        replace_self_statement(a, s->initializer, self);
    }
}

static size_t generic_parameter_position(const AstProgram *unit, const AstDeclarationNode *declaration,
                                         const char *name) {
    size_t index = 0;
    for (const AstGenericParameter *g = declaration->generic_parameters; g; g = g->next, index++)
        if (!strcmp(ast_program_lexeme(unit, g->name_token), name)) return index;
    return AST_SYMBOL_NONE;
}

static int generic_pattern_equal(const AstProgram *left_unit, const AstDeclarationNode *left, const AstType *x,
                                 const AstProgram *right_unit, const AstDeclarationNode *right, const AstType *y) {
    if (x->kind != y->kind || x->pointer_depth != y->pointer_depth || x->outer_pointer_depth != y->outer_pointer_depth
        ||
        x->is_array != y->is_array || x->is_slice != y->is_slice || x->resolved_array_length != y->
        resolved_array_length)
        return 0;
    const char *xn = ast_program_lexeme(left_unit, x->name_token), *yn = ast_program_lexeme(right_unit, y->name_token);
    size_t xp = generic_parameter_position(left_unit, left, xn), yp = generic_parameter_position(right_unit, right, yn);
    if (xp != yp || (xp == AST_SYMBOL_NONE && strcmp(xn, yn))) return 0;
    const AstTypeArgument *xa = x->arguments, *ya = y->arguments;
    for (; xa && ya; xa = xa->next, ya = ya->next)
        if (!generic_pattern_equal(left_unit, left, &xa->type, right_unit, right, &ya->type)) return 0;
    return !xa && !ya;
}

static int generic_signature_equal(const AstProgram *left_unit, const AstDeclarationNode *left,
                                   const AstProgram *right_unit, const AstDeclarationNode *right) {
    const AstGenericParameter *lg = left->generic_parameters, *rg = right->generic_parameters;
    for (; lg && rg; lg = lg->next, rg = rg->next) {
    }
    if (lg || rg) return 0;
    const AstParameter *lp = left->as.function.parameters, *rp = right->as.function.parameters;
    for (; lp && rp; lp = lp->next, rp = rp->next)
        if (!generic_pattern_equal(left_unit, left, &lp->type, right_unit, right, &rp->type)) return 0;
    return !lp && !rp;
}

void prepare_interfaces(Analyzer *a, AstProgram *root) {
    for (size_t i = 0; i <= root->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? root : root->owned_imports[i - 1];
        a->program = unit;
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            if (d->kind != AST_DECL_IMPORT) {
                for (size_t j = 0; j <= i; j++) {
                    AstProgram *previous_unit = j == 0 ? root : root->owned_imports[j - 1];
                    if (!same_package(unit, previous_unit)) continue;
                    for (AstDeclarationNode *previous = previous_unit->root; previous && previous != d;
                         previous = previous->next)
                        if (d->kind != AST_DECL_TYPE_RULE && previous->kind != AST_DECL_TYPE_RULE &&
                            (d->generic_parameters || previous->generic_parameters) &&
                            !strcmp(ast_program_lexeme(unit, d->name_token),
                                    ast_program_lexeme(previous_unit, previous->name_token)) &&
                            (d->kind != AST_DECL_FUNCTION || previous->kind != AST_DECL_FUNCTION ||
                             (d->generic_parameters && previous->generic_parameters && generic_signature_equal(
                                  unit, d, previous_unit, previous))))
                            semantic_duplicate(a, d->name_token, previous_unit, previous->name_token,
                                               "Duplicate generic declaration");
                }
            }
            for (AstGenericParameter *g = d->generic_parameters; g; g = g->next) {
                for (AstGenericParameter *p = d->generic_parameters; p != g; p = p->next)
                    if (!strcmp(ast_program_lexeme(unit, p->name_token), ast_program_lexeme(unit, g->name_token)))
                        semantic_error(a, g->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                       "Duplicate generic type parameter");
                for (AstInterfaceBound *b = g->bounds; b; b = b->next) {
                    if (!find_language_declaration(root, unit, ast_program_lexeme(unit, b->name_token), AST_DECL_INTERFACE,
                                                   NULL))
                        semantic_error(a, b->name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN, "Unknown interface bound");
                    for (AstInterfaceBound *p = g->bounds; p != b; p = p->next)
                        if (!strcmp(ast_program_lexeme(unit, p->name_token), ast_program_lexeme(unit, b->name_token)))
                            semantic_error(a, b->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                           "Duplicate interface bound");
                }
            }
            if (d->kind == AST_DECL_INTERFACE) {
                if (find_language_declaration(root, unit, ast_program_lexeme(unit, d->name_token), AST_DECL_INTERFACE,
                                              NULL) != d)
                    semantic_error(a, d->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                   "Duplicate interface declaration");
                for (AstDeclarationNode *m = d->as.interface_decl.methods; m; m = m->next)
                    for (AstDeclarationNode *p = d->as.interface_decl.methods; p != m; p = p->next)
                        if (!strcmp(ast_program_lexeme(unit, p->name_token), ast_program_lexeme(unit, m->name_token)))
                            semantic_error(a, m->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                           "Duplicate interface method");
            }
            if (d->generic_parameters && (d->kind == AST_DECL_STRUCT || d->kind == AST_DECL_ENUM) &&
                find_language_declaration(root, unit, ast_program_lexeme(unit, d->name_token), d->kind, NULL) != d)
                semantic_error(a, d->name_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                               "Duplicate generic aggregate declaration");
        }
    }
    a->program = root;
}

int generic_bounds_satisfied(Analyzer *a, const AstProgram *declaration_unit,
                                     const AstDeclarationNode *d, const AstType *arguments) {
    AstProgram *root = (AstProgram *) a->model->program;
    size_t index = 0;
    for (AstGenericParameter *g = d->generic_parameters; g; g = g->next, index++) {
        for (AstInterfaceBound *bound = g->bounds; bound; bound = bound->next) {
            AstDeclarationNode *required_interface = find_language_declaration(root, declaration_unit,
                                                                           ast_program_lexeme(
                                                                               declaration_unit, bound->name_token),
                                                                           AST_DECL_INTERFACE, NULL);
            size_t owner_id = resolve_named_symbol_id(a, a->program,
                                                       named_type_token(a->program, &arguments[index]));
            if (required_interface && required_interface->is_auto_interface) {
                if (!semantic_satisfies(a->model, a->program, &arguments[index],
                                       required_interface->resolved_symbol_id)) return 0;
                continue;
            }
            if (!required_interface || owner_id >= a->model->symbol_count ||
                (a->model->symbols[owner_id].kind != SEMANTIC_SYMBOL_STRUCT &&
                 a->model->symbols[owner_id].kind != SEMANTIC_SYMBOL_ENUM) ||
                arguments[index].pointer_depth || arguments[index].outer_pointer_depth ||
                arguments[index].is_array || arguments[index].is_slice)
                return 0;
            size_t interface_id = required_interface->resolved_symbol_id;
            AstType concrete_bound = bound->type;
            AstTypeArgument concrete_arguments[DMM_MAX_TYPE_PARAMETERS];
            size_t concrete_count = 0;
            const AstTypeArgument *source_argument = bound->type.arguments;
            while (source_argument != NULL &&
                   concrete_count < DMM_MAX_TYPE_PARAMETERS) {
                AstType concrete = source_argument->type;
                if (concrete.kind == AST_TYPE_NAMED &&
                    concrete.arguments == NULL) {
                    const char *name = ast_program_lexeme(
                        declaration_unit, concrete.name_token);
                    size_t parameter_index = 0;
                    for (const AstGenericParameter *parameter =
                             d->generic_parameters;
                         parameter != NULL;
                         parameter = parameter->next, parameter_index++)
                        if (!strcmp(name, ast_program_lexeme(
                                              declaration_unit,
                                              parameter->name_token))) {
                            concrete = arguments[parameter_index];
                            break;
                        }
                }
                concrete_arguments[concrete_count].type = concrete;
                concrete_arguments[concrete_count].next = NULL;
                if (concrete_count != 0)
                    concrete_arguments[concrete_count - 1].next =
                        &concrete_arguments[concrete_count];
                concrete_count++;
                source_argument = source_argument->next;
            }
            concrete_bound.arguments =
                concrete_count == 0 ? NULL : &concrete_arguments[0];
            if (required_interface->generic_parameters != NULL) {
                if (!semantic_implements_specialized_interface(
                        a->model, interface_id, owner_id, a->program,
                        &concrete_bound))
                    return 0;
            } else if (!semantic_implements_interface(
                           a->model, interface_id, owner_id))
                return 0;
        }
    }
    return 1;
}

AstType argument_type_copy(Analyzer *analyzer, const AstProgram *unit, AstType type) {
    const AstToken *token = ast_program_token(unit, type.name_token);
    if (token) type.name_token = concrete_token(analyzer, token->type, token->lexeme);
    if (type.is_array) {
        char length[32];
        snprintf(length, sizeof(length), "%zu", type.resolved_array_length);
        type.array_length_token = concrete_token(analyzer, TOKEN_NUMBER, length);
    }
    if ((type.kind == AST_TYPE_FUTURE || type.kind == AST_TYPE_JOIN) && type.arguments != NULL) {
        AstTypeArgument *result = ast_program_alloc(analyzer->program, sizeof(*result));
        if (result == NULL) {
            analyzer->allocation_failed = 1;
            type.invalid_substitution = 1;
        } else {
            result->type = argument_type_copy(analyzer, unit, type.arguments->type);
            type.arguments = result;
        }
    }
    return type;
}

static int unify_generic_pattern(Analyzer *analyzer, const AstProgram *pattern_unit,
                                 const AstType *pattern, const AstProgram *actual_unit, AstType actual,
                                 const AstDeclarationNode *declaration, AstType *substitutions, unsigned char *inferred,
                                 unsigned depth) {
    if (depth > 64 || actual.name_token == AST_TOKEN_NONE) return 0;
    size_t index = 0;
    const AstGenericParameter *g = declaration->generic_parameters;
    for (; g; g = g->next, index++)
        if (!strcmp(ast_program_lexeme(pattern_unit, g->name_token),
                    ast_program_lexeme(pattern_unit, pattern->name_token)))
            break;
    if (g && (actual.is_array || actual.is_slice) && !pattern->is_array && !pattern->is_slice) {
        if (actual.outer_pointer_depth < pattern->pointer_depth + pattern->outer_pointer_depth) return 0;
        actual.outer_pointer_depth -= pattern->pointer_depth + pattern->outer_pointer_depth;
    } else {
        if (actual.pointer_depth < pattern->pointer_depth || actual.outer_pointer_depth < pattern->outer_pointer_depth)
            return 0;
        actual.pointer_depth -= pattern->pointer_depth;
        actual.outer_pointer_depth -= pattern->outer_pointer_depth;
    }
    if (pattern->is_array || pattern->is_slice) {
        if (actual.outer_pointer_depth || !(actual.is_array || actual.is_slice) ||
            (pattern->is_array && (!actual.is_array ||
                                   actual.resolved_array_length != pattern->resolved_array_length)))
            return 0;
        actual.is_array = actual.is_slice = 0;
        actual.resolved_array_length = 0;
        actual.array_length_token = AST_TOKEN_NONE;
    }
    if (g) {
        actual = argument_type_copy(analyzer, actual_unit, actual);
        if (inferred[index])
            return ast_concrete_type_equal(analyzer->program,
                                           &substitutions[index], analyzer->program, &actual);
        substitutions[index] = actual;
        inferred[index] = 1;
        return 1;
    }
    if (actual.pointer_depth || actual.outer_pointer_depth || actual.is_array || actual.is_slice) return 0;
    if (pattern->kind == AST_TYPE_FUTURE || pattern->kind == AST_TYPE_JOIN) {
        if (actual.kind != pattern->kind || pattern->arguments == NULL || actual.arguments == NULL)
            return 0;
        return unify_generic_pattern(analyzer, pattern_unit, &pattern->arguments->type,
            actual_unit, actual.arguments->type, declaration, substitutions, inferred, depth + 1);
    }
    if (pattern->arguments) {
        const SemanticSymbol *symbol = scoped_find_global(analyzer->model, analyzer->program,
                                                          ast_program_lexeme(actual_unit, actual.name_token),
                                                          SEMANTIC_SYMBOL_STRUCT);
        if (!symbol)
            symbol = scoped_find_global(analyzer->model, analyzer->program,
                                        ast_program_lexeme(actual_unit, actual.name_token), SEMANTIC_SYMBOL_ENUM);
        AstProgram *root = (AstProgram *) analyzer->model->program;
        AstDeclarationNode *origin = find_language_declaration(root, pattern_unit,
                                                               ast_program_lexeme(pattern_unit, pattern->name_token),
                                                               AST_DECL_STRUCT, NULL);
        if (!origin)
            origin = find_language_declaration(root, pattern_unit,
                                               ast_program_lexeme(pattern_unit, pattern->name_token), AST_DECL_ENUM,
                                               NULL);
        if (!symbol || !symbol->declaration || !symbol->declaration->generic_origin ||
            symbol->declaration->generic_origin != origin)
            return 0;
        const AstTypeArgument *a = pattern->arguments, *b = symbol->declaration->specialization_arguments;
        for (; a && b; a = a->next, b = b->next)
            if (!unify_generic_pattern(analyzer, pattern_unit, &a->type, symbol->source_program,
                                       b->type, declaration, substitutions, inferred, depth + 1))
                return 0;
        return !a && !b;
    }
    size_t expected = resolve_named_symbol_id(analyzer, pattern_unit, pattern->name_token);
    size_t supplied = resolve_named_symbol_id(analyzer, actual_unit, actual.name_token);
    if (expected != AST_SYMBOL_NONE || supplied != AST_SYMBOL_NONE) return expected == supplied;
    return !strcmp(ast_program_lexeme(pattern_unit, pattern->name_token),
                   ast_program_lexeme(actual_unit, actual.name_token));
}

void instantiate_generic_candidates(Analyzer *analyzer, const char *name,
                                           const AstExpression *arguments) {
    AstProgram *root = (AstProgram *) analyzer->model->program;
    const DmmPackage *target = lookup_package(analyzer->program, &name);
    for (size_t unit_index = 0; unit_index <= root->owned_import_count; unit_index++) {
        AstProgram *unit = unit_index == 0 ? root : root->owned_imports[unit_index - 1];
        if (target ? unit->package != target : !same_package(analyzer->program, unit)) continue;
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            if (d->kind != AST_DECL_FUNCTION || !d->generic_parameters ||
                strcmp(ast_program_lexeme(unit, d->name_token), name))
                continue;
            AstType substitutions[DMM_MAX_TYPE_PARAMETERS] = {0};
            unsigned char inferred[DMM_MAX_TYPE_PARAMETERS] = {0};
            size_t count = 0;
            int viable = 1;
            for (const AstGenericParameter *g = d->generic_parameters; g; g = g->next) count++;
            const AstParameter *parameter = d->as.function.parameters;
            const AstExpression *argument = arguments;
            for (; viable && parameter && argument; parameter = parameter->next, argument = argument->next) {
                AstType t = inferred_argument_type(analyzer, argument);
                if (!contains_type_parameter(unit, &parameter->type, d)) {
                    if (!expression_to_declared_type_allowed(analyzer, argument, unit, &parameter->type)) viable = 0;
                } else if (!unify_generic_pattern(analyzer, unit, &parameter->type, analyzer->program, t,
                                                  d, substitutions, inferred, 0))
                    viable = 0;
            }
            if (parameter || argument) viable = 0;
            for (size_t i = 0; i < count; i++) if (!inferred[i]) viable = 0;
            if (!viable || !generic_bounds_satisfied(analyzer, unit, d, substitutions)) continue;
            AstDeclarationNode *instance = ast_specialize_function(unit, d, substitutions, count, analyzer->program);
            if (!instance) {
                semantic_error(analyzer, analyzer->current_function_token, ERROR_CATEGORY_SEMANTIC,
                               ERR_SEM_COMPLEXITY_LIMIT,
                               "Generic specialization exceeds deterministic resource limits");
                continue;
            }
            if (instance->resolved_symbol_id == AST_SYMBOL_NONE) {
                AstProgram *saved = analyzer->program;
                analyzer->program = unit;
                normalize_function_types(analyzer, instance);
                add_global(analyzer, instance, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
                analyzer->program = saved;
            }
        }
    }
}

const SemanticSymbol *explicit_generic_function(Analyzer *analyzer, const char *name, AstExpression *call) {
    AstProgram *root = (AstProgram *) analyzer->model->program;
    int reference = call->kind != AST_EXPR_CALL;
    AstExpression *callee = reference ? call : call->left;
    AstType types[DMM_MAX_TYPE_PARAMETERS];
    size_t count = 0;
    for (AstTypeArgument *argument = callee->allocated_type.arguments; argument; argument = argument->next) {
        if (count == DMM_MAX_TYPE_PARAMETERS) {
            semantic_error(analyzer, call->first_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                           "Too many explicit type arguments");
            return NULL;
        }
        normalize_generic_type(analyzer, &argument->type, 0);
        if (!known_declared_type(analyzer, &argument->type) || argument->type.kind == AST_TYPE_INFERRED ||
            (primitive_type(analyzer->program, &argument->type) == TYPE_VOID && !argument->type.pointer_depth && !
             argument->type.outer_pointer_depth)) {
            semantic_error(analyzer, argument->type.name_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Explicit type argument requires a complete non-void type");
            return NULL;
        }
        types[count++] = argument->type;
    }
    size_t candidates[1024], candidate_count = 0;
    size_t value_count = 0;
    for (const AstExpression *argument = call->arguments; argument; argument = argument->next) value_count++;
    const DmmPackage *target = lookup_package(analyzer->program, &name);
    for (size_t i = 0; i <= root->owned_import_count; i++) {
        AstProgram *unit = i == 0 ? root : root->owned_imports[i - 1];
        if (target ? unit->package != target : !same_package(analyzer->program, unit)) continue;
        for (AstDeclarationNode *d = unit->root; d; d = d->next) {
            if (d->kind != AST_DECL_FUNCTION || !d->generic_parameters || strcmp(
                    ast_program_lexeme(unit, d->name_token), name)) continue;
            if (!same_package(analyzer->program, unit) && !d->is_public) {
                semantic_error(analyzer, call->first_token, ERROR_CATEGORY_SEMANTIC, ERR_PACKAGE_PRIVATE,
                               "Generic function is private to its defining package");
                continue;
            }
            size_t expected = 0;
            for (AstGenericParameter *g = d->generic_parameters; g; g = g->next) expected++;
            if (count != expected || (!reference && parameter_count(d) != value_count) || !generic_bounds_satisfied(
                    analyzer, unit, d, types)) continue;
            AstDeclarationNode *instance = ast_specialize_function(unit, d, types, count, analyzer->program);
            if (!instance) {
                semantic_error(analyzer, call->first_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                               "Generic specialization exceeds resource limits");
                continue;
            }
            if (instance->resolved_symbol_id == AST_SYMBOL_NONE) {
                AstProgram *saved = analyzer->program;
                analyzer->program = unit;
                normalize_function_types(analyzer, instance);
                add_global(analyzer, instance, SEMANTIC_SYMBOL_FUNCTION, AST_TOKEN_NONE);
                analyzer->program = saved;
            }
            const AstParameter *parameter = instance->as.function.parameters;
            const AstExpression *argument = call->arguments;
            int viable = 1;
            if (!reference) {
                for (; parameter && argument; parameter = parameter->next, argument = argument->next)
                    if (!expression_to_declared_type_allowed(analyzer, argument, unit, &parameter->type)) viable = 0;
                if (!viable || parameter || argument) continue;
            }
            if (candidate_count == 1024) {
                semantic_error(analyzer, call->first_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_COMPLEXITY_LIMIT,
                               "Too many generic overload candidates");
                return NULL;
            }
            candidates[candidate_count++] = instance->resolved_symbol_id;
        }
    }
    size_t selected = AST_SYMBOL_NONE;
    if (reference && candidate_count > 1) {
        semantic_error(analyzer, call->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                       "Ambiguous explicit generic function reference");
        return NULL;
    }
    for (size_t i = 0; i < candidate_count; i++) {
        const SemanticSymbol *candidate = &analyzer->model->symbols[candidates[i]];
        int dominated = 0;
        for (size_t j = 0; j < candidate_count; j++)
            if (i != j && function_dominates(analyzer, &analyzer->model->symbols[candidates[j]], candidate,
                                             call->arguments)) dominated = 1;
        if (!dominated) {
            if (selected != AST_SYMBOL_NONE) {
                semantic_error(analyzer, call->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Ambiguous explicit generic function call");
                return NULL;
            }
            selected = candidates[i];
        }
    }
    if (selected == AST_SYMBOL_NONE) {
        semantic_error(analyzer, call->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                       "No generic function matches the explicit type arguments, interface bounds and value arguments");
        return NULL;
    }
    return &analyzer->model->symbols[selected];
}
