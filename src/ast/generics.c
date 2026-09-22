#include "generics.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *canonical_type_name(const AstProgram *program,
                                       size_t token, char *buffer,
                                       size_t capacity) {
    const char *name = ast_program_lexeme(program, token);
    if (program->package == NULL || token >= program->token_count ||
        program->tokens[token].type != TOKEN_IDENTIFIER ||
        strstr(name, "::") != NULL)
        return name;
    const DmmPackage *target = program->package;
    const char *package = target->path;
    const char *plain = name;
    const char *dot = strchr(name, '.');
    if (dot != NULL)
        for (AstDeclarationNode *declaration = program->root; declaration;
             declaration = declaration->next)
            if (declaration->kind == AST_DECL_IMPORT)
                for (AstImportPath *path = declaration->as.import_decl.paths;
                     path; path = path->next)
                    if (path->alias != NULL &&
                        path->resolved_program != NULL &&
                        strlen(path->alias) == (size_t) (dot - name) &&
                        !strncmp(path->alias, name,
                                 (size_t) (dot - name))) {
                        target = path->resolved_program->package;
                        package = target->path;
                        plain = dot + 1;
                    }
    AstProgram *origin = NULL;
    if (ast_package_declaration(target, plain, &origin) && origin && origin->package)
        package = origin->package->path;
    snprintf(buffer, capacity, "%s::%s", package, plain);
    return buffer;
}

static int alpha_parameter_index(const AstProgram *program, const AstType *type,
                                 const AstGenericParameter *parameters) {
    if (type->kind != AST_TYPE_NAMED) return -1;
    int index = 0;
    for (const AstGenericParameter *parameter = parameters; parameter;
         parameter = parameter->next, index++)
        if (!strcmp(ast_program_lexeme(program, type->name_token),
                    ast_program_lexeme(program, parameter->name_token))) return index;
    return -1;
}

static int alpha_type_equal(const AstProgram *a, const AstType *x,
                            const AstGenericParameter *gx,
                            const AstProgram *b, const AstType *y,
                            const AstGenericParameter *gy) {
    int xi = alpha_parameter_index(a, x, gx);
    int yi = alpha_parameter_index(b, y, gy);
    if (xi >= 0 || yi >= 0)
        return xi >= 0 && xi == yi && x->borrow_kind == y->borrow_kind &&
               x->pointer_depth == y->pointer_depth &&
               x->outer_pointer_depth == y->outer_pointer_depth &&
               x->is_array == y->is_array && x->is_slice == y->is_slice &&
               x->resolved_array_length == y->resolved_array_length;
    if (x->kind != y->kind || x->is_native_function != y->is_native_function || x->borrow_kind != y->borrow_kind ||
        x->pointer_depth != y->pointer_depth ||
        x->outer_pointer_depth != y->outer_pointer_depth ||
        x->is_array != y->is_array || x->is_slice != y->is_slice ||
        x->resolved_array_length != y->resolved_array_length) return 0;
    if (x->kind != AST_TYPE_FUNCTION && x->kind != AST_TYPE_FUTURE && x->kind!=AST_TYPE_JOIN && x->kind!=AST_TYPE_EXECUTOR) {
        char x_name[2048], y_name[2048];
        if (strcmp(canonical_type_name(a, x->name_token, x_name,
                                       sizeof(x_name)),
                   canonical_type_name(b, y->name_token, y_name,
                                       sizeof(y_name)))) return 0;
    }
    if ((x->element_type == NULL) != (y->element_type == NULL)) return 0;
    if (x->element_type && !alpha_type_equal(a, x->element_type, gx,
                                             b, y->element_type, gy)) return 0;
    const AstTypeArgument *xa = x->arguments, *ya = y->arguments;
    for (; xa && ya; xa = xa->next, ya = ya->next)
        if (!alpha_type_equal(a, &xa->type, gx, b, &ya->type, gy)) return 0;
    if (xa || ya) return 0;
    if (x->kind == AST_TYPE_FUNCTION) {
        if (x->callable_mode != y->callable_mode) return 0;
        const AstGenericParameter *nested_x = x->function_generic_parameters;
        const AstGenericParameter *nested_y = y->function_generic_parameters;
        const AstTypeArgument *xp = x->function_parameters, *yp = y->function_parameters;
        for (; xp && yp; xp = xp->next, yp = yp->next)
            if (!alpha_type_equal(a, &xp->type, nested_x, b, &yp->type, nested_y)) return 0;
        return xp == NULL && yp == NULL && x->function_return_type && y->function_return_type &&
               alpha_type_equal(a, x->function_return_type, nested_x,
                                b, y->function_return_type, nested_y);
    }
    return 1;
}

int ast_concrete_type_equal(const AstProgram *a, const AstType *x,
                              const AstProgram *b, const AstType *y) {
    if (x->kind != y->kind || x->is_native_function != y->is_native_function || x->borrow_kind != y->borrow_kind ||
        x->pointer_depth != y->pointer_depth ||
        x->outer_pointer_depth != y->outer_pointer_depth || x->is_array != y->is_array ||
        x->is_slice != y->is_slice || x->resolved_array_length != y->resolved_array_length)
        return 0;
    if (x->kind == AST_TYPE_FUNCTION) {
        if (x->callable_mode != y->callable_mode) return 0;
        const AstGenericParameter *gx = x->function_generic_parameters;
        const AstGenericParameter *gy = y->function_generic_parameters;
        for (; gx && gy; gx = gx->next, gy = gy->next) {
            const AstInterfaceBound *bx = gx->bounds, *by = gy->bounds;
            for (; bx && by; bx = bx->next, by = by->next)
                if (strcmp(ast_program_lexeme(a, bx->name_token),
                           ast_program_lexeme(b, by->name_token))) return 0;
            if (bx || by) return 0;
        }
        if (gx || gy) return 0;
        const AstTypeArgument *px = x->function_parameters;
        const AstTypeArgument *py = y->function_parameters;
        for (; px && py; px = px->next, py = py->next)
            if (!alpha_type_equal(a, &px->type, x->function_generic_parameters,
                                  b, &py->type, y->function_generic_parameters)) return 0;
        return px == NULL && py == NULL && x->function_return_type && y->function_return_type &&
               alpha_type_equal(a, x->function_return_type, x->function_generic_parameters,
                                b, y->function_return_type, y->function_generic_parameters);
    }
    char x_name[2048], y_name[2048];
    if (x->kind != AST_TYPE_FUTURE && x->kind!=AST_TYPE_JOIN && x->kind!=AST_TYPE_EXECUTOR && strcmp(canonical_type_name(a, x->name_token, x_name,
                                   sizeof(x_name)),
               canonical_type_name(b, y->name_token, y_name,
                                   sizeof(y_name)))) return 0;
    if ((x->element_type == NULL) != (y->element_type == NULL)) return 0;
    if (x->element_type != NULL &&
        !ast_concrete_type_equal(a, x->element_type, b, y->element_type))
        return 0;
    const AstTypeArgument *u = x->arguments, *v = y->arguments;
    for (; u && v; u = u->next, v = v->next)
        if (!ast_concrete_type_equal(a, &u->type, b, &v->type)) return 0;
    return u == NULL && v == NULL;
}

typedef struct {
    AstProgram *program;
    const AstDeclarationNode *origin;
    const AstType *arguments;
    const AstProgram *argument_program;
    size_t count;
    int failed;
    struct {
        size_t source;
        size_t target;
        int valid;
    } token_cache[64];
} Substitution;

static void *owned(Substitution *s, size_t size) {
    void *result = ast_program_alloc(s->program, size);
    if (!result) s->failed = 1;
    return result;
}

static size_t transplant_token(Substitution *s, size_t index) {
    const AstToken *source = ast_program_token(s->argument_program, index);
    if (!source) return AST_TOKEN_NONE;
    AstToken token = *source;
    size_t cache_slot = index % (sizeof(s->token_cache) / sizeof(s->token_cache[0]));
    if (s->token_cache[cache_slot].valid && s->token_cache[cache_slot].source == index)
        return s->token_cache[cache_slot].target;
    const char *text = string_interner_intern(s->program->strings, token.lexeme);
    if (!text) {
        s->failed = 1;
        return AST_TOKEN_NONE;
    }
    for (size_t i = 0; i < s->program->token_count; i++)
        if (s->program->tokens[i].type == token.type &&
            (s->program->tokens[i].lexeme == text ||
             strcmp(s->program->tokens[i].lexeme, text) == 0)) {
            s->token_cache[cache_slot].source = index;
            s->token_cache[cache_slot].target = i;
            s->token_cache[cache_slot].valid = 1;
            return i;
        }
    if (s->program->token_count >= SIZE_MAX / sizeof(AstToken) - 1) {
        s->failed = 1;
        return AST_TOKEN_NONE;
    }
    AstToken *tokens = realloc(s->program->tokens, (s->program->token_count + 1) * sizeof(*tokens));
    if (!tokens) {
        s->failed = 1;
        return AST_TOKEN_NONE;
    }
    token.lexeme = text;
    s->program->tokens = tokens;
    size_t result = s->program->token_count++;
    tokens[result] = token;
    s->token_cache[cache_slot].source = index;
    s->token_cache[cache_slot].target = result;
    s->token_cache[cache_slot].valid = 1;
    return result;
}

static AstType concrete_copy(Substitution *s, AstType type) {
    type.name_token = transplant_token(s, type.name_token);
    const AstToken *lifetime = ast_program_token(s->argument_program, type.lifetime_token);
    type.lifetime_token = lifetime && lifetime->type == TOKEN_LIFETIME
        ? transplant_token(s, type.lifetime_token) : AST_TOKEN_NONE;
    AstLifetimeParameter *lifetimes = NULL, **lifetime_tail = &lifetimes;
    for (const AstLifetimeParameter *p = type.lifetime_arguments; p; p = p->next) {
        AstLifetimeParameter *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->name_token = transplant_token(s, p->name_token);
        *lifetime_tail = copy;
        lifetime_tail = &copy->next;
    }
    type.lifetime_arguments = lifetimes;
    if (type.is_array) type.array_length_token = transplant_token(s, type.array_length_token);
    if (type.element_type != NULL) {
        AstType *element = owned(s, sizeof(*element));
        if (element != NULL) *element = concrete_copy(s, *type.element_type);
        type.element_type = element;
    }
    AstTypeArgument *head = NULL, **tail = &head;
    for (const AstTypeArgument *a = type.arguments; a; a = a->next) {
        AstTypeArgument *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->type = concrete_copy(s, a->type);
        *tail = copy;
        tail = &copy->next;
    }
    type.arguments = head;
    AstTypeArgument *parameters = NULL, **parameter_tail = &parameters;
    for (const AstTypeArgument *a = type.function_parameters; a; a = a->next) {
        AstTypeArgument *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->type = concrete_copy(s, a->type);
        *parameter_tail = copy;
        parameter_tail = &copy->next;
    }
    type.function_parameters = parameters;
    if (type.function_return_type != NULL) {
        AstType *result = owned(s, sizeof(*result));
        if (result != NULL) *result = concrete_copy(s, *type.function_return_type);
        type.function_return_type = result;
    }
    return type;
}

static AstType substitute_type(Substitution *s, AstType type) {
    if (type.element_type != NULL) {
        AstType *element = owned(s, sizeof(*element));
        if (element != NULL) *element = substitute_type(s, *type.element_type);
        type.element_type = element;
        return type;
    }
    if (type.kind == AST_TYPE_FUNCTION) {
        AstTypeArgument *head = NULL, **tail = &head;
        for (const AstTypeArgument *a = type.function_parameters; a; a = a->next) {
            AstTypeArgument *copy = owned(s, sizeof(*copy));
            if (!copy) break;
            copy->type = substitute_type(s, a->type);
            *tail = copy;
            tail = &copy->next;
        }
        type.function_parameters = head;
        if (type.function_return_type) {
            AstType *result = owned(s, sizeof(*result));
            if (result) *result = substitute_type(s, *type.function_return_type);
            type.function_return_type = result;
        }
        return type;
    }
    const AstGenericParameter *parameter = s->origin->generic_parameters;
    for (size_t i = 0; parameter && i < s->count; parameter = parameter->next, i++) {
        if (type.kind == AST_TYPE_NAMED &&
            strcmp(ast_program_lexeme(s->program, type.name_token),
                   ast_program_lexeme(s->program, parameter->name_token)) == 0) {
            AstType result = concrete_copy(s, s->arguments[i]);
            result.span = type.span;
            if (type.borrow_kind != AST_BORROW_NONE) {
                result.borrow_kind = type.borrow_kind;
                result.lifetime_token = type.lifetime_token;
            }
            if ((result.is_array || result.is_slice) && !type.is_array && !type.is_slice)
                result.outer_pointer_depth += type.pointer_depth;
            else result.pointer_depth += type.pointer_depth;
            result.outer_pointer_depth += type.outer_pointer_depth;
            if (type.is_array || type.is_slice) {
                if (result.is_array || result.is_slice) {
                    AstType *element = owned(s, sizeof(*element));
                    if (element == NULL) {
                        result.invalid_substitution = 1;
                        return result;
                    }
                    *element = result;
                    element->outer_pointer_depth -=
                            type.outer_pointer_depth;
                    if (type.borrow_kind != AST_BORROW_NONE)
                        element->borrow_kind = type.borrow_kind;
                    result.borrow_kind = AST_BORROW_NONE;
                    result.pointer_depth = 0;
                    result.outer_pointer_depth = type.outer_pointer_depth;
                    result.element_type = element;
                }
                result.is_array = type.is_array;
                result.is_slice = type.is_slice;
                result.array_length_token = type.array_length_token;
                result.resolved_array_length = type.resolved_array_length;
            }
            return result;
        }
    }
    AstTypeArgument *head = NULL, **tail = &head;
    for (const AstTypeArgument *a = type.arguments; a; a = a->next) {
        AstTypeArgument *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->type = substitute_type(s, a->type);
        *tail = copy;
        tail = &copy->next;
    }
    type.arguments = head;
    return type;
}

static AstStatement *clone_statement(Substitution *s,
                                     const AstStatement *original);

static AstDeclarationNode *clone_closure_environment(Substitution *s,
                                                     const AstDeclarationNode *original) {
    AstDeclarationNode *environment = owned(s, sizeof(*environment));
    if (!environment) return NULL;
    *environment = *original;
    environment->name_token = AST_TOKEN_NONE;
    environment->resolved_symbol_id = AST_SYMBOL_NONE;
    environment->next = NULL;
    environment->lifetime_parameters = NULL;
    environment->kind = AST_DECL_STRUCT;
    environment->closure_mode = 0;
    environment->closure_captures = NULL;
    environment->closure_consuming_invoke = NULL;
    environment->as.struct_decl.fields = NULL;
    AstField **field_tail = &environment->as.struct_decl.fields;
    for (const AstField *field = original->closure_captures ? original->closure_captures :
         original->as.struct_decl.fields; field; field = field->next) {
        AstField *copy = owned(s, sizeof(*copy));
        if (!copy) return environment;
        *copy = *field;
        copy->next = NULL;
        copy->resolved_symbol_id = AST_SYMBOL_NONE;
        copy->type = substitute_type(s, field->type);
        *field_tail = copy;
        field_tail = &copy->next;
    }
    AstDeclarationNode *invoke = owned(s, sizeof(*invoke));
    if (!invoke) return environment;
    const AstDeclarationNode *source_invoke = original->closure_consuming_invoke ?
        original->closure_consuming_invoke : original->as.struct_decl.methods;
    *invoke = *source_invoke;
    invoke->name_token = AST_TOKEN_NONE;
    invoke->resolved_symbol_id = AST_SYMBOL_NONE;
    invoke->semantic_body_checked = 0;
    invoke->as.function.owner_token = AST_TOKEN_NONE;
    invoke->as.function.return_type = substitute_type(s, invoke->as.function.return_type);
    invoke->as.function.body = clone_statement(s, original->closure_consuming_invoke ?
        source_invoke->as.function.body->match_arms->body : source_invoke->as.function.body);
    invoke->as.function.parameters = NULL;
    AstParameter **parameter_tail = &invoke->as.function.parameters;
    for (const AstParameter *parameter = original->closure_consuming_invoke ?
         source_invoke->as.function.parameters->next : source_invoke->as.function.parameters;
         parameter; parameter = parameter->next) {
        AstParameter *copy = owned(s, sizeof(*copy));
        if (!copy) return environment;
        *copy = *parameter;
        copy->next = NULL;
        copy->resolved_symbol_id = AST_SYMBOL_NONE;
        copy->type = substitute_type(s, parameter->type);
        *parameter_tail = copy;
        parameter_tail = &copy->next;
    }
    environment->as.struct_decl.methods = invoke;
    return environment;
}

static AstExpression *clone_expression(Substitution *s, const AstExpression *original) {
    if (!original) return NULL;
    AstExpression *e = owned(s, sizeof(*e));
    if (!e) return NULL;
    *e = *original;
    e->left = clone_expression(s, original->left);
    e->right = clone_expression(s, original->right);
    e->arguments = clone_expression(s, original->arguments);
    if (original->closure_consuming_call) {
        e->left = clone_expression(s, original->arguments);
        if(e->left) e->left->next=NULL;
        e->arguments=clone_expression(s,original->arguments ? original->arguments->next : NULL);
        e->closure_consuming_call=0;
    }
    e->control = clone_statement(s, original->control);
    e->next = clone_expression(s, original->next);
    e->allocated_type = substitute_type(s, original->allocated_type);
    if (original->closure_environment) {
        e->closure_environment = clone_closure_environment(s, original->closure_environment);
        e->kind=AST_EXPR_STRUCT_LITERAL;
        e->left=NULL;
        e->allocated_type=(AstType){.kind=AST_TYPE_INFERRED,.name_token=AST_TOKEN_NONE,
            .array_length_token=AST_TOKEN_NONE};
    }
    return e;
}

static AstStatement *clone_statement(Substitution *s, const AstStatement *original) {
    if (!original) return NULL;
    AstStatement *v = owned(s, sizeof(*v));
    if (!v) return NULL;
    *v = *original;
    v->type = substitute_type(s, original->closure_callable_annotation ?
        *original->closure_callable_annotation : original->type);
    v->closure_callable_annotation = NULL;
    v->expression = clone_expression(s, original->expression);
    v->value = clone_expression(s, original->value);
    v->condition = clone_expression(s, original->condition);
    v->update = clone_expression(s, original->update);
    v->result = clone_expression(s, original->result);
    v->body = clone_statement(s, original->body);
    v->else_body = clone_statement(s, original->else_body);
    v->initializer = clone_statement(s, original->initializer);
    v->next = clone_statement(s, original->next);
    v->match_arms = NULL;
    AstMatchArm **tail = &v->match_arms;
    for (const AstMatchArm *a = original->match_arms; a; a = a->next) {
        AstMatchArm *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        *copy = *a;
        copy->next = NULL;
        copy->body = clone_statement(s, a->body);
        copy->bindings = NULL;
        copy->type = substitute_type(s, a->type);
        AstParameter **bindings = &copy->bindings;
        for (const AstParameter *p = a->bindings; p; p = p->next) {
            AstParameter *binding = owned(s, sizeof(*binding));
            if (!binding) break;
            *binding = *p;
            binding->next = NULL;
            binding->type = substitute_type(s, p->type);
            *bindings = binding;
            bindings = &binding->next;
        }
        *tail = copy;
        tail = &copy->next;
    }

    return v;
}

static int identity_text(char *key, size_t *used, const char *text) {
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        if (*used + 3 >= 4096) return 0;
        key[(*used)++] = hex[*p >> 4];
        key[(*used)++] = hex[*p & 15];
    }
    key[(*used)++] = '_';
    key[*used] = 0;
    return 1;
}

int ast_polymorphic_callable_compatible(const AstProgram *target_program,
                                        const AstType *target,
                                        const AstProgram *source_program,
                                        const AstType *source) {
    if (target == NULL || source == NULL ||
        target->kind != AST_TYPE_FUNCTION ||
        source->kind != AST_TYPE_FUNCTION ||
        target->is_native_function != source->is_native_function ||
        source->callable_mode > target->callable_mode ||
        target->function_generic_parameters == NULL ||
        source->function_generic_parameters == NULL ||
        target->pointer_depth != source->pointer_depth ||
        target->outer_pointer_depth != source->outer_pointer_depth ||
        target->borrow_kind != source->borrow_kind ||
        target->is_array != source->is_array ||
        target->is_slice != source->is_slice)
        return 0;
    const AstGenericParameter *target_generic =
        target->function_generic_parameters;
    const AstGenericParameter *source_generic =
        source->function_generic_parameters;
    for (; target_generic != NULL && source_generic != NULL;
         target_generic = target_generic->next,
         source_generic = source_generic->next) {
        for (const AstInterfaceBound *required = source_generic->bounds;
             required != NULL; required = required->next) {
            int guaranteed = 0;
            for (const AstInterfaceBound *bound = target_generic->bounds;
                 bound != NULL; bound = bound->next)
                if (!strcmp(ast_program_lexeme(source_program,
                                               required->name_token),
                            ast_program_lexeme(target_program,
                                               bound->name_token))) {
                    guaranteed = 1;
                    break;
                }
            if (!guaranteed) return 0;
        }
    }
    if (target_generic != NULL || source_generic != NULL) return 0;
    const AstTypeArgument *target_parameter = target->function_parameters;
    const AstTypeArgument *source_parameter = source->function_parameters;
    for (; target_parameter != NULL && source_parameter != NULL;
         target_parameter = target_parameter->next,
         source_parameter = source_parameter->next)
        if (!alpha_type_equal(target_program, &target_parameter->type,
                              target->function_generic_parameters,
                              source_program, &source_parameter->type,
                              source->function_generic_parameters))
            return 0;
    return target_parameter == NULL && source_parameter == NULL &&
           target->function_return_type != NULL &&
           source->function_return_type != NULL &&
           alpha_type_equal(target_program, target->function_return_type,
                            target->function_generic_parameters,
                            source_program, source->function_return_type,
                            source->function_generic_parameters);
}

static int identity_type(char *key, size_t *used, const AstProgram *program, const AstType *type) {
    char number[64];
    const char *name = type->kind == AST_TYPE_FUNCTION ? "func" : ast_program_lexeme(program, type->name_token);
    char canonical[2048];
    if (program->package && type->name_token < program->token_count && program->tokens[type->name_token].type ==
        TOKEN_IDENTIFIER && !strstr(name, "::")) {
        const char *package = program->package->path, *plain = name, *dot = strchr(name, '.');
        if (dot)
            for (AstDeclarationNode *d = program->root; d; d = d->next)
                if (d->kind == AST_DECL_IMPORT)
                    for (AstImportPath *p = d->as.import_decl.paths; p; p = p->next)
                        if (p->alias && p->resolved_program && strlen(p->alias) == (size_t) (dot - name) && !strncmp(
                                p->alias, name, (size_t) (dot - name))) {
                            package = p->resolved_program->package->path;
                            plain = dot + 1;
                        }
        snprintf(canonical, sizeof(canonical), "%s::%s", package, plain);
        name = canonical;
    }
    if (!identity_text(key, used, name)) return 0;
    snprintf(number, sizeof(number), "%u_%u_%d_%d_%zu_%d_%u", type->pointer_depth,
             type->outer_pointer_depth, type->is_array, type->is_slice, type->resolved_array_length,
             type->borrow_kind, type->callable_mode);
    if (!identity_text(key, used, number)) return 0;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        if (!identity_type(key, used, program, &a->type)) return 0;
    for (const AstTypeArgument *a = type->function_parameters; a; a = a->next)
        if (!identity_type(key, used, program, &a->type)) return 0;
    if (type->function_return_type && !identity_type(key, used, program, type->function_return_type)) return 0;
    return identity_text(key, used, "end");
}

static const char *specialization_identity(Substitution *s) {
    char key[4096] = "__dmm_generic_";
    size_t used = strlen(key);
    if (!identity_text(key, &used, s->program->module_identity ? s->program->module_identity : ".")) return NULL;
    if (!identity_text(key, &used, ast_declaration_kind_name(s->origin->kind)) ||
        !identity_text(key, &used, ast_program_lexeme(s->program, s->origin->name_token)))
        return NULL;
    if (s->origin->kind == AST_DECL_FUNCTION) {
        for (const AstParameter *p = s->origin->as.function.parameters; p; p = p->next)
            if (!identity_type(key, &used, s->program, &p->type)) return NULL;
        if (!identity_text(key, &used, "arguments")) return NULL;
    }
    for (size_t i = 0; i < s->count; i++)
        if (!identity_type(key, &used, s->argument_program, &s->arguments[i])) return NULL;
    return string_interner_intern(s->program->strings, key);
}

static AstAutoCondition *substitute_conditions(Substitution *s, const AstAutoCondition *source) {
    AstAutoCondition *head = NULL, **tail = &head;
    for (; source; source = source->next) {
        AstAutoCondition *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->type = substitute_type(s, source->type);
        AstInterfaceBound **bounds = &copy->bounds;
        for (const AstInterfaceBound *b = source->bounds; b; b = b->next) {
            AstInterfaceBound *bound = owned(s, sizeof(*bound));
            if (!bound) break;
            bound->type = substitute_type(s, b->type);
            bound->name_token = bound->type.name_token;
            *bounds = bound;
            bounds = &bound->next;
        }
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

AstDeclarationNode *ast_specialize_function(AstProgram *program,
                                            const AstDeclarationNode *origin, const AstType *arguments, size_t count,
                                            const AstProgram *argument_program) {
    size_t instances = 0;
    for (AstDeclarationNode *d = program->root; d; d = d->next) {
        if (d->generic_origin) instances++;
        if (d->generic_origin != origin) continue;
        const AstTypeArgument *a = d->specialization_arguments;
        size_t i = 0;
        for (; a && i < count; a = a->next, i++)
            if (!ast_concrete_type_equal(program, &a->type, argument_program, &arguments[i])) break;
        if (i == count && !a) return d;
    }
    if (count > DMM_MAX_TYPE_PARAMETERS || instances >= DMM_MAX_SPECIALIZATIONS) return NULL;
    Substitution s = {
        .program = program, .origin = origin, .arguments = arguments,
        .count = count, .argument_program = argument_program
    };
    AstDeclarationNode *result = owned(&s, sizeof(*result));
    if (!result) return NULL;
    *result = *origin;
    result->next = NULL;
    result->generic_parameters = NULL;
    result->where_conditions = substitute_conditions(&s, origin->where_conditions);
    result->generic_origin = origin;
    result->resolved_symbol_id = AST_SYMBOL_NONE;
    result->auto_rules = NULL;
    AstAutoRule **rule_tail = &result->auto_rules;
    for (const AstAutoRule *rule = origin->auto_rules; rule; rule = rule->next) {
        AstAutoRule *copy = owned(&s, sizeof(*copy));
        if (!copy) break;
        copy->interfaces = rule->interfaces;
        AstAutoCondition **tail = &copy->conditions;
        for (const AstAutoCondition *condition = rule->conditions; condition; condition = condition->next) {
            AstAutoCondition *child = owned(&s, sizeof(*child));
            if (!child) break;
            child->type = substitute_type(&s, condition->type);
            AstInterfaceBound **bound_tail = &child->bounds;
            for (const AstInterfaceBound *bound = condition->bounds; bound; bound = bound->next) {
                AstInterfaceBound *concrete = owned(&s, sizeof(*concrete));
                if (!concrete) break;
                concrete->type = substitute_type(&s, bound->type);
                concrete->name_token = concrete->type.name_token;
                *bound_tail = concrete;
                bound_tail = &concrete->next;
            }
            *tail = child;
            tail = &child->next;
        }
        *rule_tail = copy;
        rule_tail = &copy->next;
    }
    if (origin->kind == AST_DECL_FUNCTION) {
        AstParameter *head = NULL, **tail = &head;
        for (const AstParameter *p = origin->as.function.parameters; p; p = p->next) {
            AstParameter *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *p;
            copy->next = NULL;
            copy->type = substitute_type(&s, p->type);
            *tail = copy;
            tail = &copy->next;
        }
        result->as.function.parameters = head;
        result->as.function.return_type = substitute_type(&s, origin->as.function.return_type);
        result->as.function.body = clone_statement(&s, origin->as.function.body);
    } else if (origin->kind == AST_DECL_STRUCT) {
        AstField *head = NULL, **tail = &head;
        for (const AstField *f = origin->as.struct_decl.fields; f; f = f->next) {
            AstField *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *f;
            copy->next = NULL;
            copy->type = substitute_type(&s, f->type);
            *tail = copy;
            tail = &copy->next;
        }
        result->as.struct_decl.fields = head;
        result->as.struct_decl.destructor =
            clone_statement(&s, origin->as.struct_decl.destructor);
        result->as.struct_decl.methods = NULL;
        /* Method specialization follows the aggregate's substitution. */
        AstDeclarationNode **methods = &result->as.struct_decl.methods;
        for (const AstDeclarationNode *m = origin->as.struct_decl.methods; m; m = m->next) {
            AstDeclarationNode *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *m;
            copy->next = NULL;
            copy->generic_origin = origin;
            copy->where_conditions = substitute_conditions(&s, m->where_conditions);
            copy->as.function.return_type = substitute_type(&s, m->as.function.return_type);
            copy->as.function.body = clone_statement(&s, m->as.function.body);
            AstParameter *parameter_head = NULL, **parameter_tail = &parameter_head;
            for (const AstParameter *p = m->as.function.parameters; p; p = p->next) {
                AstParameter *v = owned(&s, sizeof(*v));
                if (!v) break;
                *v = *p;
                v->next = NULL;
                v->type = substitute_type(&s, p->type);
                *parameter_tail = v;
                parameter_tail = &v->next;
            }
            copy->as.function.parameters = parameter_head;
            *methods = copy;
            methods = &copy->next;
        }
    } else if (origin->kind == AST_DECL_INTERFACE) {
        AstDeclarationNode **methods = &result->as.interface_decl.methods;
        result->as.interface_decl.methods = NULL;
        for (const AstDeclarationNode *m = origin->as.interface_decl.methods; m; m = m->next) {
            AstDeclarationNode *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *m;
            copy->next = NULL;
            copy->as.function.return_type = substitute_type(&s, m->as.function.return_type);
            AstParameter **parameters = &copy->as.function.parameters;
            copy->as.function.parameters = NULL;
            for (const AstParameter *p = m->as.function.parameters; p; p = p->next) {
                AstParameter *parameter = owned(&s, sizeof(*parameter));
                if (!parameter) break;
                *parameter = *p;
                parameter->next = NULL;
                parameter->type = substitute_type(&s, p->type);
                *parameters = parameter;
                parameters = &parameter->next;
            }
            *methods = copy;
            methods = &copy->next;
        }
    } else if (origin->kind == AST_DECL_ENUM) {
        AstEnumValue *head = NULL, **tail = &head;
        for (const AstEnumValue *v = origin->as.enum_decl.values; v; v = v->next) {
            AstEnumValue *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *v;
            copy->next = NULL;
            copy->arguments = clone_expression(&s, v->arguments);
            AstTypeArgument **payload = &copy->payload_types;
            copy->payload_types = NULL;
            for (const AstTypeArgument *a = v->payload_types; a; a = a->next) {
                AstTypeArgument *argument = owned(&s, sizeof(*argument));
                if (!argument) break;
                argument->type = substitute_type(&s, a->type);
                *payload = argument;
                payload = &argument->next;
            }
            *tail = copy;
            tail = &copy->next;
        }
        result->as.enum_decl.values = head;
        AstDeclarationNode **methods = &result->as.enum_decl.methods;
        result->as.enum_decl.methods = NULL;
        for (const AstDeclarationNode *m = origin->as.enum_decl.methods; m; m = m->next) {
            AstDeclarationNode *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *m;
            copy->next = NULL;
            copy->generic_origin = origin;
            copy->where_conditions = substitute_conditions(&s, m->where_conditions);
            copy->as.function.return_type = substitute_type(&s, m->as.function.return_type);
            copy->as.function.body = clone_statement(&s, m->as.function.body);
            AstParameter *parameter_head = NULL, **parameter_tail = &parameter_head;
            for (const AstParameter *p = m->as.function.parameters; p; p = p->next) {
                AstParameter *v = owned(&s, sizeof(*v));
                if (!v) break;
                *v = *p;
                v->next = NULL;
                v->type = substitute_type(&s, p->type);
                *parameter_tail = v;
                parameter_tail = &v->next;
            }
            copy->as.function.parameters = parameter_head;
            *methods = copy;
            methods = &copy->next;
        }
    } else return NULL;
    AstTypeArgument **args = &result->specialization_arguments;
    for (size_t i = 0; i < count; i++) {
        AstTypeArgument *a = owned(&s, sizeof(*a));
        if (!a) break;
        a->type = concrete_copy(&s, arguments[i]);
        *args = a;
        args = &a->next;
    }
    result->specialization_identity = specialization_identity(&s);
    if (s.failed || result->specialization_identity == NULL) return NULL;
    if (origin->kind == AST_DECL_STRUCT || origin->kind == AST_DECL_ENUM || origin->kind == AST_DECL_INTERFACE) {
        AstToken token = {.type = TOKEN_IDENTIFIER, .lexeme = result->specialization_identity, .span = origin->span};
        AstToken *tokens = realloc(program->tokens, (program->token_count + 1) * sizeof(*tokens));
        if (!tokens) return NULL;
        program->tokens = tokens;
        result->name_token = program->token_count++;
        tokens[result->name_token] = token;
        if (origin->kind == AST_DECL_STRUCT || origin->kind == AST_DECL_ENUM) {
            AstDeclarationNode *methods = origin->kind == AST_DECL_STRUCT
                                              ? result->as.struct_decl.methods
                                              : result->as.enum_decl.methods;
            for (AstDeclarationNode *m = methods; m; m = m->next)
                m->as.function.owner_token = result->name_token;
        }
    }
    AstDeclarationNode **end = &program->root;
    while (*end) end = &(*end)->next;
    *end = result;
    program->structured_declaration_count++;
    return result;
}

static const char *callable_consumer_identity(AstProgram *program,
                                              const AstDeclarationNode *origin,
                                              const AstExpression *arguments) {
    char key[4096] = "__dmm_callable_";
    size_t used = strlen(key);
    if (!identity_text(key, &used, program->module_identity != NULL
                                       ? program->module_identity : ".") ||
        !identity_text(key, &used, origin->specialization_identity != NULL ?
            origin->specialization_identity : ast_program_lexeme(program,origin->name_token)))
        return NULL;
    const AstExpression *argument = arguments;
    for (const AstParameter *parameter = origin->as.function.parameters;
         parameter != NULL && argument != NULL;
         parameter = parameter->next, argument = argument->next) {
        if (parameter->type.kind != AST_TYPE_FUNCTION ||
            (parameter->type.function_generic_parameters == NULL &&
             !(argument->has_resolved_ast_type && argument->resolved_ast_type.kind==AST_TYPE_NAMED &&
               argument->resolved_callable))) continue;
        if(argument->has_resolved_ast_type && argument->resolved_ast_type.kind==AST_TYPE_NAMED) {
            const AstProgram *unit=argument->resolved_type_program ? argument->resolved_type_program : program;
            if(!identity_text(key,&used,unit->module_identity ? unit->module_identity : ".") ||
               !identity_text(key,&used,ast_program_lexeme(unit,argument->resolved_ast_type.name_token))) return NULL;
            continue;
        }
        if (argument->resolved_callable == NULL ||
            argument->resolved_callable_program == NULL)
            return NULL;
        const AstProgram *callable_program =
            argument->resolved_callable_program;
        const AstDeclarationNode *callable = argument->resolved_callable;
        if (!identity_text(key, &used,
                           callable_program->module_identity != NULL
                               ? callable_program->module_identity : ".") ||
            !identity_text(key, &used,
                           callable->specialization_identity != NULL
                               ? callable->specialization_identity
                               : ast_program_lexeme(callable_program,
                                                    callable->name_token)))
            return NULL;
    }
    return string_interner_intern(program->strings, key);
}

AstDeclarationNode *ast_specialize_callable_consumer(
        AstProgram *program, const AstDeclarationNode *origin,
        const AstExpression *arguments) {
    const char *identity = callable_consumer_identity(program, origin,
                                                       arguments);
    if (identity == NULL) return NULL;
    size_t instances = 0;
    for (AstDeclarationNode *declaration = program->root; declaration;
         declaration = declaration->next) {
        if (declaration->generic_origin != NULL) instances++;
        if (declaration->generic_origin == origin &&
            declaration->specialization_identity != NULL &&
            !strcmp(declaration->specialization_identity, identity))
            return declaration;
    }
    if (instances >= DMM_MAX_SPECIALIZATIONS) return NULL;
    Substitution substitution = {
        .program = program,
        .origin = origin,
        .argument_program = program
    };
    AstDeclarationNode *result = owned(&substitution, sizeof(*result));
    if (result == NULL) return NULL;
    *result = *origin;
    result->next = NULL;
    result->generic_parameters = NULL;
    result->generic_origin = origin;
    result->specialization_arguments = NULL;
    result->specialization_identity = identity;
    result->resolved_symbol_id = AST_SYMBOL_NONE;
    result->semantic_body_checked = 0;

    AstParameter *head = NULL, **tail = &head;
    const AstExpression *argument = arguments;
    for (const AstParameter *parameter = origin->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        AstParameter *copy = owned(&substitution, sizeof(*copy));
        if (copy == NULL) break;
        *copy = *parameter;
        copy->next = NULL;
        copy->resolved_symbol_id = AST_SYMBOL_NONE;
        copy->type = concrete_copy(&substitution, parameter->type);
        copy->compile_time_value = NULL;
        if(parameter->type.kind==AST_TYPE_FUNCTION && argument &&
           argument->has_resolved_ast_type && argument->resolved_ast_type.kind==AST_TYPE_NAMED &&
           argument->resolved_callable) {
            const AstProgram *saved=substitution.argument_program;
            substitution.argument_program=argument->resolved_type_program ? argument->resolved_type_program : program;
            copy->type=concrete_copy(&substitution,argument->resolved_ast_type);
            /* The concrete environment keeps the invocation contract of the
               abstract callback parameter after specialization. */
            copy->type.callable_mode=parameter->type.callable_mode;
            substitution.argument_program=saved;
        }
        if (parameter->type.kind == AST_TYPE_FUNCTION &&
            parameter->type.function_generic_parameters != NULL &&
            !(argument && argument->has_resolved_ast_type && argument->resolved_ast_type.kind==AST_TYPE_NAMED)) {
            if (argument == NULL || argument->resolved_callable == NULL) {
                substitution.failed = 1;
                break;
            }
            copy->compile_time_value = clone_expression(&substitution,
                                                        argument);
        }
        *tail = copy;
        tail = &copy->next;
        if (argument != NULL) argument = argument->next;
    }
    result->as.function.parameters = head;
    result->as.function.return_type = concrete_copy(
        &substitution, origin->as.function.return_type);
    result->as.function.body = clone_statement(&substitution,
                                                origin->as.function.body);
    if (substitution.failed) return NULL;
    AstDeclarationNode **end = &program->root;
    while (*end != NULL) end = &(*end)->next;
    *end = result;
    program->structured_declaration_count++;
    return result;
}
