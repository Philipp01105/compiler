static int async_expression_send(Analyzer *, const AstExpression *, size_t);
static int async_graph_future(Analyzer *,const AstExpression *,size_t);

static int async_graph_origin(Analyzer *a,const AstExpression *e,size_t depth) {
    if(!e||depth>128) return 0;
    if(e->kind==AST_EXPR_NAME && e->resolved_symbol_id<a->model->symbol_count) {
        const SemanticSymbol *s=&a->model->symbols[e->resolved_symbol_id];
        if(s->owner_symbol_id!=a->current_function_symbol_id) return 0;
        if(s->kind==SEMANTIC_SYMBOL_PARAMETER && s->declared_type.borrow_kind!=AST_BORROW_NONE)
            return a->async_graph_analysis;
        if(s->kind!=SEMANTIC_SYMBOL_LOCAL && s->kind!=SEMANTIC_SYMBOL_PARAMETER) return 0;
        if(s->declared_type.borrow_kind!=AST_BORROW_NONE || s->resolved_is_slice)
            return async_graph_origin(a,s->node,depth+1);
        unsigned properties=s->declared_type.kind==AST_TYPE_INFERRED &&
            s->resolved_named_symbol_id<a->model->symbol_count
            ? semantic_symbol_type_properties(a->model,s->resolved_named_symbol_id)
            : semantic_declared_type_properties(a,s->source_program,&s->declared_type);
        return !s->resolved_pointer_depth && !s->resolved_outer_pointer_depth &&
            (properties&SEMANTIC_TYPE_SEND)!=0;
    }
    if(e->kind==AST_EXPR_UNARY) return async_graph_origin(a,e->right,depth+1);
    if(e->kind==AST_EXPR_CAST) return async_graph_origin(a,e->arguments,depth+1);
    if(e->kind==AST_EXPR_MEMBER||e->kind==AST_EXPR_SUBSLICE||e->kind==AST_EXPR_SLICE_DATA||
       e->kind==AST_EXPR_SLICE_LENGTH||e->kind==AST_EXPR_INDEX)
        return async_graph_origin(a,e->left,depth+1);
    return 0;
}
static int async_graph_future(Analyzer *a,const AstExpression *e,size_t depth) {
    if(!e||depth>128) return 0;
    if(e->kind==AST_EXPR_AWAIT) return async_graph_future(a,e->right,depth+1);
    if(e->kind==AST_EXPR_NAME && e->resolved_symbol_id<a->model->symbol_count) {
        const SemanticSymbol *s=&a->model->symbols[e->resolved_symbol_id];
        if(a->async_graph_analysis && s->kind==SEMANTIC_SYMBOL_PARAMETER &&
           s->owner_symbol_id==a->current_function_symbol_id && s->declared_type.kind==AST_TYPE_FUTURE)
            return s->declared_type.arguments &&
                (semantic_declared_type_properties(a,s->source_program,&s->declared_type.arguments->type)&SEMANTIC_TYPE_SEND)!=0;
        return async_graph_future(a,s->node,depth+1);
    }
    if(e->async_operation==ASYNC_NATIVE_FUTURE)
        return 1; /* Native bridge promises stable, Send-safe frame storage. */
    if(e->async_operation==ASYNC_CANCEL) return async_graph_future(a,e->arguments,depth+1);
    if(e->kind!=AST_EXPR_CALL||e->resolved_symbol_id>=a->model->symbol_count) return 0;
    const SemanticSymbol *callee=&a->model->symbols[e->resolved_symbol_id];
    if(!(callee->async_graph_properties&SEMANTIC_TYPE_SEND)||!callee->declaration) return 0;
    if(callee->declaration->as.function.receiver_parameter) {
        if(!e->left || e->left->kind!=AST_EXPR_MEMBER ||
           !async_graph_origin(a,e->left->left,depth+1)) return 0;
    }
    const AstParameter *p=callee->declaration->as.function.parameters;
    for(const AstExpression *arg=e->arguments;arg;arg=arg->next) {
        if(!p) return 0;
        if(p->type.borrow_kind!=AST_BORROW_NONE || p->type.is_slice) {
            if(!async_graph_origin(a,arg,depth+1)) return 0;
        } else if(!async_expression_send(a,arg,depth+1)) return 0;
        p=p->next;
    }
    return p==NULL;
}

static void async_prelude_enum(Analyzer *a, const char *name, const char *first, const char *second, int generic) {
    AstProgram *p = a->program;
    for (AstDeclarationNode *d = p->root; d; d = d->next)
        if (!strcmp(ast_program_lexeme(p, d->name_token), name)) return;
    AstDeclarationNode *d = ast_program_alloc(p, sizeof(*d));
    AstEnumValue *v = ast_program_alloc(p, sizeof(*v));
    if (!d || !v) { a->allocation_failed = 1; return; }
    d->kind = AST_DECL_ENUM;
    d->is_public = d->is_async_builtin = 1;
    d->name_token = d->first_token = concrete_token(a, TOKEN_IDENTIFIER, name);
    d->token_count = 1;
    d->resolved_symbol_id = AST_SYMBOL_NONE;
    d->as.enum_decl.is_sum = generic;
    d->as.enum_decl.values = v;
    const char *names[2] = {first, second};
    for (size_t n = 0; n < (second ? 2U : 1U); n++) {
        v->name_token = concrete_token(a, TOKEN_IDENTIFIER, names[n]);
        v->resolved_symbol_id = AST_SYMBOL_NONE;
        v->is_public = 1;
        if (generic) {
            AstTypeArgument *payload = ast_program_alloc(p, sizeof(*payload));
            AstGenericParameter *g = ast_program_alloc(p, sizeof(*g));
            if (!payload || !g) { a->allocation_failed = 1; return; }
            g->name_token = concrete_token(a, TOKEN_IDENTIFIER, n ? "E" : "T");
            payload->type = (AstType){.kind=AST_TYPE_NAMED, .name_token=g->name_token,
                                    .array_length_token=AST_TOKEN_NONE};
            v->payload_types = payload;
            if (!n) d->generic_parameters = g;
            else d->generic_parameters->next = g;
        }
        if (!n && second) {
            v->next = ast_program_alloc(p, sizeof(*v));
            if (!v->next) { a->allocation_failed = 1; return; }
            v = v->next;
        }
    }
    d->next = p->root;
    p->root = d;
}

static void async_prelude(Analyzer *a) {
    if (!semantic_async_enabled(a)) return;
    async_prelude_enum(a, "ShutdownMode", "Drain", "Cancel", 0);
    async_prelude_enum(a, "TaskError", "Cancelled", NULL, 0);
    async_prelude_enum(a, "Result", "Ok", "Err", 1);
}

static int async_statements_send(Analyzer *analyzer, const AstStatement *statement, size_t depth) {
    if (depth > 128) return 0;
    for (; statement != NULL; statement = statement->next) {
        if (!async_expression_send(analyzer, statement->expression, depth + 1) ||
            !async_expression_send(analyzer, statement->value, depth + 1) ||
            !async_expression_send(analyzer, statement->condition, depth + 1) ||
            !async_expression_send(analyzer, statement->update, depth + 1) ||
            !async_expression_send(analyzer, statement->result, depth + 1) ||
            !async_statements_send(analyzer, statement->body, depth + 1) ||
            !async_statements_send(analyzer, statement->else_body, depth + 1) ||
            !async_statements_send(analyzer, statement->initializer, depth + 1)) return 0;
        if (statement->kind == AST_STMT_VARIABLE && statement->type.kind != AST_TYPE_INFERRED &&
            statement->type.kind != AST_TYPE_FUTURE &&
            !(semantic_declared_type_properties(analyzer, analyzer->program, &statement->type) & SEMANTIC_TYPE_SEND) &&
            !((statement->type.is_slice || statement->type.borrow_kind!=AST_BORROW_NONE) &&
              async_graph_origin(analyzer,statement->value,depth+1)))
            return 0;
        for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next)
            if (!async_statements_send(analyzer, arm->body, depth + 1)) return 0;
    }
    return 1;
}

static unsigned async_future_properties(Analyzer *analyzer, const AstExpression *expression, size_t depth) {
    unsigned properties = SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP | SEMANTIC_TYPE_MUST_CONSUME;
    if (depth > 128 || expression == NULL) return properties;
    if(expression->has_resolved_ast_type && expression->resolved_ast_type.kind==AST_TYPE_JOIN)
        return properties | SEMANTIC_TYPE_SEND;
    if (expression->async_operation == ASYNC_CANCEL)
        return properties | (async_future_properties(analyzer,expression->arguments,depth+1) & SEMANTIC_TYPE_SEND);
    if (expression->async_operation == ASYNC_SHUTDOWN) return properties | SEMANTIC_TYPE_SEND;
    if(expression->kind==AST_EXPR_AWAIT || expression->async_operation==ASYNC_BLOCK_ON)
        return properties | (async_future_properties(analyzer,expression->kind==AST_EXPR_AWAIT ? expression->right:expression->arguments,depth+1)&SEMANTIC_TYPE_SEND);
    if (expression->kind == AST_EXPR_CALL && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *callee = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (callee->kind == SEMANTIC_SYMBOL_FUNCTION && callee->declaration != NULL &&
            (callee->declaration->as.function.is_async || callee->declaration->as.function.return_type.kind==AST_TYPE_FUTURE))
            properties |= callee->async_frame_properties & SEMANTIC_TYPE_SEND;
    } else if (expression->kind == AST_EXPR_NAME && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (symbol->kind == SEMANTIC_SYMBOL_LOCAL && symbol->node != NULL)
            properties |= async_future_properties(analyzer, symbol->node, depth + 1) & SEMANTIC_TYPE_SEND;
    }
    return properties;
}

static void check_async_spawns(Analyzer *a, const AstExpression *e, const AstStatement *s) {
    for (; e; e=e->next) {
        if (e->async_operation == ASYNC_SPAWN &&
            !(async_future_properties(a,e->arguments,0) & SEMANTIC_TYPE_SEND))
            semantic_error(a,e->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,
                           "spawn requires a Send Future frame including its output; borrowed or erased frames cannot be spawned");
        check_async_spawns(a,e->left,NULL); check_async_spawns(a,e->right,NULL);
        check_async_spawns(a,e->arguments,e->control);
    }
    for (; s; s=s->next) {
        check_async_spawns(a,s->expression,s->body); check_async_spawns(a,s->value,s->else_body);
        check_async_spawns(a,s->condition,s->initializer); check_async_spawns(a,s->update,NULL);
        check_async_spawns(a,s->result,NULL);
        for (const AstMatchArm *arm=s->match_arms;arm;arm=arm->next) check_async_spawns(a,NULL,arm->body);
    }
}

static int async_expression_send(Analyzer *analyzer, const AstExpression *expression, size_t depth) {
    if (expression == NULL) return 1;
    if (depth > 128) return 0;
    if (expression->async_operation==ASYNC_NATIVE_FUTURE) return 1;
    if (semantic_expression_is_future(expression)) {
        if (!(async_future_properties(analyzer, expression, depth + 1) & SEMANTIC_TYPE_SEND) &&
            !async_graph_future(analyzer,expression,depth+1)) return 0;
    } else if (expression->resolved_borrow_kind != AST_BORROW_NONE) {
        if(async_graph_origin(analyzer,expression,depth+1) && expression->has_resolved_ast_type) {
            AstType referent=expression->resolved_ast_type; referent.borrow_kind=AST_BORROW_NONE;
            if(referent.is_slice) referent=ast_type_element(&referent);
            const AstProgram *source=expression->resolved_type_program?expression->resolved_type_program:analyzer->program;
            unsigned needed=expression->resolved_borrow_kind==AST_BORROW_IMMUTABLE?SEMANTIC_TYPE_SYNC:SEMANTIC_TYPE_SEND;
            return (semantic_declared_type_properties(analyzer,source,&referent)&needed)!=0;
        }
        if (expression->resolved_pointer_depth > 1 ||
            (expression->has_resolved_ast_type && expression->resolved_ast_type.kind == AST_TYPE_FUTURE)) return 0;
        unsigned referent = expression->resolved_named_symbol_id < analyzer->model->symbol_count
            ? semantic_symbol_type_properties(analyzer->model, expression->resolved_named_symbol_id)
            : SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
        unsigned needed = expression->resolved_borrow_kind == AST_BORROW_IMMUTABLE
            ? SEMANTIC_TYPE_SYNC : SEMANTIC_TYPE_SEND;
        if (!(referent & needed)) return 0;
    } else if (expression->has_resolved_ast_type) {
        if((expression->resolved_is_slice || expression->resolved_pointer_depth) &&
           async_graph_origin(analyzer,expression,depth+1)) return 1;
        if(expression->kind==AST_EXPR_CALL && expression->resolved_symbol_id<analyzer->model->symbol_count) {
            const SemanticSymbol *s=&analyzer->model->symbols[expression->resolved_symbol_id];
            if(s->kind==SEMANTIC_SYMBOL_FUNCTION && s->source_program->module_identity &&
               !strcmp(s->source_program->module_identity,"stdlib/memory/raw") &&
               !strcmp(ast_program_lexeme(s->source_program,s->name_token),"core_null")) return 1;
        }
        const AstProgram *source = expression->resolved_type_program != NULL
            ? expression->resolved_type_program : analyzer->program;
        if (!(semantic_declared_type_properties(analyzer, source, &expression->resolved_ast_type) & SEMANTIC_TYPE_SEND)) return 0;
    } else if (expression->resolved_pointer_depth != 0 || expression->resolved_is_slice) return 0;
    int direct = expression->kind == AST_EXPR_CALL &&
                 expression->resolved_symbol_id < analyzer->model->symbol_count &&
                 analyzer->model->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FUNCTION;
    if ((!direct && !async_expression_send(analyzer, expression->left, depth + 1)) ||
        !async_expression_send(analyzer, expression->right, depth + 1) ||
        !async_statements_send(analyzer, expression->control, depth + 1)) return 0;
    for (const AstExpression *argument = expression->arguments; argument; argument = argument->next)
        if (!async_expression_send(analyzer, argument, depth + 1)) return 0;
    return 1;
}

static void derive_async_properties(Analyzer *analyzer) {
    unsigned owned = SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP | SEMANTIC_TYPE_MUST_CONSUME;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_FUNCTION && symbol->declaration != NULL &&
            (symbol->declaration->as.function.is_async || symbol->declaration->as.function.return_type.kind==AST_TYPE_FUTURE))
            symbol->async_frame_properties = symbol->async_graph_properties = owned | SEMANTIC_TYPE_SEND;
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            SemanticSymbol *symbol = &analyzer->model->symbols[i];
            if (!(symbol->async_graph_properties & SEMANTIC_TYPE_SEND)) continue;
            analyzer->program = (AstProgram *) symbol->source_program;
            analyzer->current_function_symbol_id=symbol->id;
            analyzer->async_graph_analysis=1;
            int send = 1;
            int external_loans=0;
            const AstDeclarationNode *function = symbol->declaration;
            const AstParameter *receiver=function->as.function.receiver_parameter;
            if(receiver) {
                external_loans=1;
                AstType referent=receiver->type; referent.borrow_kind=AST_BORROW_NONE;
                unsigned properties=semantic_declared_type_properties(analyzer,symbol->source_program,&referent);
                /* The receiver remains a borrowed child. Its graph can move
                   only when the caller owns the receiver in the pinned graph. */
                if((properties&(SEMANTIC_TYPE_SEND|SEMANTIC_TYPE_SYNC))!=
                   (SEMANTIC_TYPE_SEND|SEMANTIC_TYPE_SYNC)) send=0;
            }
            for (const AstParameter *p = function->as.function.parameters; p; p = p->next)
                if(p->type.borrow_kind!=AST_BORROW_NONE) {
                    external_loans=1;
                    AstType referent=p->type; referent.borrow_kind=AST_BORROW_NONE;
                    if(referent.is_slice) referent=ast_type_element(&referent);
                    unsigned required=p->type.borrow_kind==AST_BORROW_MUTABLE?SEMANTIC_TYPE_SEND:SEMANTIC_TYPE_SYNC;
                    if(!(semantic_declared_type_properties(analyzer,symbol->source_program,&referent)&required)) send=0;
                } else if(p->type.kind==AST_TYPE_FUTURE) {
                    external_loans=1;
                    if(!p->type.arguments || !(semantic_declared_type_properties(analyzer,symbol->source_program,&p->type.arguments->type)&SEMANTIC_TYPE_SEND)) send=0;
                } else if (!(semantic_declared_type_properties(analyzer, symbol->source_program, &p->type) & SEMANTIC_TYPE_SEND)) send=0;
            if ((function->as.function.return_type.kind!=AST_TYPE_FUTURE &&
                 !(semantic_declared_type_properties(analyzer, symbol->source_program,
                    &function->as.function.return_type) & SEMANTIC_TYPE_SEND)) ||
                !async_statements_send(analyzer, function->as.function.body, 0)) send = 0;
            if (!send) { symbol->async_graph_properties = owned; symbol->async_frame_properties=owned; changed = 1; }
            else if(external_loans && (symbol->async_frame_properties&SEMANTIC_TYPE_SEND)) {
                symbol->async_frame_properties=owned; changed=1;
            }
        }
    }
    analyzer->async_graph_analysis=0;
    analyzer->current_function_symbol_id=AST_SYMBOL_NONE;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_LOCAL && symbol->declared_type.kind == AST_TYPE_FUTURE && symbol->node != NULL)
            symbol->type_properties = async_future_properties(analyzer, symbol->node, 0);
    }
    for (size_t i=0;i<analyzer->model->symbol_count;i++) {
        const SemanticSymbol *s=&analyzer->model->symbols[i];
        if (s->kind!=SEMANTIC_SYMBOL_FUNCTION || !s->declaration) continue;
        analyzer->program=(AstProgram *)s->source_program;
        check_async_spawns(analyzer,NULL,s->declaration->as.function.body);
    }
}
