/* Iterator syntax is lowered before ownership/borrow analysis and IR. */
static AstExpression *iterator_expression(Analyzer *a, AstExpressionKind kind, const AstStatement *s) {
    AstExpression *e = ast_program_alloc(a->program, sizeof(*e));
    if (!e) {
        a->allocation_failed = 1;
        return NULL;
    }
    e->kind = kind;
    e->span = s->span;
    e->first_token = s->first_token;
    e->token_count = s->token_count;
    e->resolved_symbol_id = AST_SYMBOL_NONE;
    e->resolved_named_symbol_id = AST_SYMBOL_NONE;
    e->initializer_name_token = AST_TOKEN_NONE;
    return e;
}
static AstStatement *iterator_statement(Analyzer *a, AstStatementKind kind, const AstStatement *s) {
    AstStatement *v = ast_program_alloc(a->program, sizeof(*v));
    if (!v) {
        a->allocation_failed = 1;
        return NULL;
    }
    v->kind = kind;
    v->span = s->span;
    v->first_token = s->first_token;
    v->token_count = s->token_count;
    v->type = (AstType){
        .kind = AST_TYPE_INFERRED, .name_token = AST_TOKEN_NONE, .array_length_token = AST_TOKEN_NONE};
    v->resolved_symbol_id = AST_SYMBOL_NONE;
    return v;
}
static AstExpression *iterator_name(Analyzer *a, size_t token, const AstStatement *s) {
    AstExpression *e = iterator_expression(a, AST_EXPR_NAME, s);
    if (e)
        e->value_token = token;
    return e;
}
static AstExpression *iterator_call(Analyzer *a, size_t receiver, const char *method, unsigned mode,
                                    const AstStatement *s) {
    AstExpression *call = iterator_expression(a, AST_EXPR_CALL, s);
    AstExpression *member = iterator_expression(a, AST_EXPR_MEMBER, s);
    AstExpression *name = iterator_name(a, receiver, s);
    if (!call || !member || !name)
        return NULL;
    member->left = name;
    member->value_token = concrete_token(a, TOKEN_IDENTIFIER, method);
    call->left = member;
    call->value_token = member->value_token;
    call->iterator_protocol_mode = mode;
    return call;
}
static int iterator_source_is_place(const AstExpression *source) {
    if (!source)
        return 0;
    if (source->kind == AST_EXPR_NAME)
        return 1;
    if (source->kind == AST_EXPR_MEMBER || source->kind == AST_EXPR_INDEX)
        return iterator_source_is_place(source->left);
    return source->kind == AST_EXPR_UNARY && source->operator_type == TOKEN_STAR;
}
static int lower_iterator_for(Analyzer *a, AstStatement *s) {
    if (!s->initializer || !s->initializer->value || !s->body)
        return 0;
    unsigned mode = s->iterator_mode;
    AstExpression *source = s->initializer->value;
    analyze_expression(a, source);
    char name[96];
    snprintf(name, sizeof(name), "$iteration_source_%zu_%zu", s->first_token, a->model->symbol_count);
    size_t source_token = concrete_token(a, TOKEN_IDENTIFIER, name);
    snprintf(name, sizeof(name), "$iteration_iterator_%zu_%zu", s->first_token, a->model->symbol_count);
    size_t iterator_token = concrete_token(a, TOKEN_IDENTIFIER, name);
    AstStatement *source_binding = iterator_statement(a, AST_STMT_VARIABLE, s);
    AstStatement *iterator_binding = iterator_statement(a, AST_STMT_VARIABLE, s);
    AstStatement *loop = iterator_statement(a, AST_STMT_WHILE, s);
    AstStatement *match = iterator_statement(a, AST_STMT_MATCH, s);
    AstStatement *end = iterator_statement(a, AST_STMT_BREAK, s);
    AstMatchArm *some = ast_program_alloc(a->program, sizeof(*some));
    AstMatchArm *none = ast_program_alloc(a->program, sizeof(*none));
    AstParameter *item = ast_program_alloc(a->program, sizeof(*item));
    if (!source_binding || !iterator_binding || !loop || !match || !end || !some || !none || !item) {
        a->allocation_failed = 1;
        return 0;
    }
    source_binding->name_token = source_token;
    int place = iterator_source_is_place(source);
    if (source->resolved_borrow_kind != AST_BORROW_NONE) {
        AstExpression *deref = iterator_expression(a, AST_EXPR_UNARY, s);
        if (!deref)
            return 0;
        deref->operator_type = TOKEN_STAR;
        deref->right = source;
        source = deref;
        place = 1;
    }
    AstStatement *first = source_binding;
    if (!place) {
        AstStatement *temporary = iterator_statement(a, AST_STMT_VARIABLE, s);
        snprintf(name, sizeof(name), "$iteration_owner_%zu_%zu", s->first_token, a->model->symbol_count);
        size_t owner_token = concrete_token(a, TOKEN_IDENTIFIER, name);
        if (!temporary)
            return 0;
        temporary->name_token = owner_token;
        temporary->value = source;
        temporary->next = source_binding;
        first = temporary;
        source = iterator_name(a, owner_token, s);
    }
    AstExpression *borrow = iterator_expression(a, AST_EXPR_UNARY, s);
    if (!borrow)
        return 0;
    borrow->operator_type = TOKEN_AMPERSAND;
    borrow->mutable_borrow = mode == 3;
    borrow->right = source;
    source_binding->value = borrow;
    source_binding->next = iterator_binding;
    iterator_binding->name_token = iterator_token;
    iterator_binding->value = iterator_call(a, source_token,
                                            mode == 3   ? "iterMut"
                                            : mode == 2 ? "iterRef"
                                                        : "iter",
                                            mode, s);
    iterator_binding->next = loop;
    loop->condition = iterator_expression(a, AST_EXPR_LITERAL, s);
    if (!loop->condition || !iterator_binding->value)
        return 0;
    loop->condition->value_token = concrete_token(a, TOKEN_NUMBER, "1");
    loop->body = match;
    match->value = iterator_call(a, iterator_token, "next", mode + 3, s);
    match->match_arms = some;
    some->variant_token = concrete_token(a, TOKEN_IDENTIFIER, "Some");
    some->span = s->span;
    none->variant_token = concrete_token(a, TOKEN_IDENTIFIER, "None");
    none->span = s->span;
    some->next = none;
    some->body = s->body;
    some->bindings = item;
    none->body = end;
    item->name_token = s->initializer->name_token;
    item->span = s->span;
    item->type = (AstType){
        .kind = AST_TYPE_INFERRED, .name_token = AST_TOKEN_NONE, .array_length_token = AST_TOKEN_NONE};
    item->iteration_borrow = 1;
    s->kind = AST_STMT_BLOCK;
    s->body = first;
    s->initializer = NULL;
    s->iterator_mode = 0;
    return match->value != NULL;
}
