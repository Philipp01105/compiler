#include "generics.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int ast_concrete_type_equal(const AstProgram *a, const AstType *x,
                            const AstProgram *b, const AstType *y) {
    if (x->kind != y->kind || x->pointer_depth != y->pointer_depth ||
        x->outer_pointer_depth != y->outer_pointer_depth || x->is_array != y->is_array ||
        x->is_slice != y->is_slice || x->resolved_array_length != y->resolved_array_length ||
        strcmp(ast_program_lexeme(a, x->name_token), ast_program_lexeme(b, y->name_token)))
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
    if (type.is_array) type.array_length_token = transplant_token(s, type.array_length_token);
    AstTypeArgument *head = NULL, **tail = &head;
    for (const AstTypeArgument *a = type.arguments; a; a = a->next) {
        AstTypeArgument *copy = owned(s, sizeof(*copy));
        if (!copy) break;
        copy->type = concrete_copy(s, a->type);
        *tail = copy;
        tail = &copy->next;
    }
    type.arguments = head;
    return type;
}

static AstType substitute_type(Substitution *s, AstType type) {
    const AstGenericParameter *parameter = s->origin->generic_parameters;
    for (size_t i = 0; parameter && i < s->count; parameter = parameter->next, i++) {
        if (type.kind == AST_TYPE_NAMED &&
            strcmp(ast_program_lexeme(s->program, type.name_token),
                   ast_program_lexeme(s->program, parameter->name_token)) == 0) {
            AstType result = concrete_copy(s, s->arguments[i]);
            result.span = type.span;
            if ((result.is_array || result.is_slice) && !type.is_array && !type.is_slice)
                result.outer_pointer_depth += type.pointer_depth;
            else result.pointer_depth += type.pointer_depth;
            result.outer_pointer_depth += type.outer_pointer_depth;
            if (type.is_array || type.is_slice) {
                if (result.is_array || result.is_slice) result.invalid_substitution = 1;
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

static AstExpression *clone_expression(Substitution *s, const AstExpression *original) {
    if (!original) return NULL;
    AstExpression *e = owned(s, sizeof(*e));
    if (!e) return NULL;
    *e = *original;
    e->left = clone_expression(s, original->left);
    e->right = clone_expression(s, original->right);
    e->arguments = clone_expression(s, original->arguments);
    e->next = clone_expression(s, original->next);
    e->allocated_type = substitute_type(s, original->allocated_type);
    return e;
}

static AstStatement *clone_statement(Substitution *s, const AstStatement *original) {
    if (!original) return NULL;
    AstStatement *v = owned(s, sizeof(*v));
    if (!v) return NULL;
    *v = *original;
    v->type = substitute_type(s, original->type);
    v->expression = clone_expression(s, original->expression);
    v->value = clone_expression(s, original->value);
    v->condition = clone_expression(s, original->condition);
    v->update = clone_expression(s, original->update);
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

static int identity_type(char *key, size_t *used, const AstProgram *program, const AstType *type) {
    char number[64];
    const char *name = ast_program_lexeme(program, type->name_token);
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
    snprintf(number, sizeof(number), "%u_%u_%d_%d_%zu", type->pointer_depth,
             type->outer_pointer_depth, type->is_array, type->is_slice, type->resolved_array_length);
    if (!identity_text(key, used, number)) return 0;
    for (const AstTypeArgument *a = type->arguments; a; a = a->next)
        if (!identity_type(key, used, program, &a->type)) return 0;
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
    result->generic_origin = origin;
    result->resolved_symbol_id = AST_SYMBOL_NONE;
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
        result->as.struct_decl.methods = NULL;
        /* Method specialization follows the aggregate's substitution. */
        AstDeclarationNode **methods = &result->as.struct_decl.methods;
        for (const AstDeclarationNode *m = origin->as.struct_decl.methods; m; m = m->next) {
            AstDeclarationNode *copy = owned(&s, sizeof(*copy));
            if (!copy) break;
            *copy = *m;
            copy->next = NULL;
            copy->generic_origin = origin;
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
    if (origin->kind == AST_DECL_STRUCT || origin->kind == AST_DECL_ENUM) {
        AstToken token = {.type = TOKEN_IDENTIFIER, .lexeme = result->specialization_identity, .span = origin->span};
        AstToken *tokens = realloc(program->tokens, (program->token_count + 1) * sizeof(*tokens));
        if (!tokens) return NULL;
        program->tokens = tokens;
        result->name_token = program->token_count++;
        tokens[result->name_token] = token;
        if (origin->kind == AST_DECL_STRUCT)
            for (AstDeclarationNode *m = result->as.struct_decl.methods; m; m = m->next)
                m->as.function.owner_token = result->name_token;
    }
    AstDeclarationNode **end = &program->root;
    while (*end) end = &(*end)->next;
    *end = result;
    program->structured_declaration_count++;
    return result;
}
