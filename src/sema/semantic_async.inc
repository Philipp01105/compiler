static int async_expression_send(Analyzer *, const AstExpression *, size_t);

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
            !(semantic_declared_type_properties(analyzer, analyzer->program, &statement->type) & SEMANTIC_TYPE_SEND))
            return 0;
        for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next)
            if (!async_statements_send(analyzer, arm->body, depth + 1)) return 0;
    }
    return 1;
}

static unsigned async_future_properties(Analyzer *analyzer, const AstExpression *expression, size_t depth) {
    unsigned properties = SEMANTIC_TYPE_MOVE_ONLY | SEMANTIC_TYPE_NEEDS_DROP | SEMANTIC_TYPE_MUST_CONSUME;
    if (depth > 128 || expression == NULL) return properties;
    if (expression->kind == AST_EXPR_CALL && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *callee = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (callee->kind == SEMANTIC_SYMBOL_FUNCTION && callee->declaration != NULL &&
            callee->declaration->as.function.is_async)
            properties |= callee->async_frame_properties & SEMANTIC_TYPE_SEND;
    } else if (expression->kind == AST_EXPR_NAME && expression->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[expression->resolved_symbol_id];
        if (symbol->kind == SEMANTIC_SYMBOL_LOCAL && symbol->node != NULL)
            properties |= async_future_properties(analyzer, symbol->node, depth + 1) & SEMANTIC_TYPE_SEND;
    }
    return properties;
}

static int async_expression_send(Analyzer *analyzer, const AstExpression *expression, size_t depth) {
    if (expression == NULL) return 1;
    if (depth > 128) return 0;
    if (semantic_expression_is_future(expression)) {
        if (!(async_future_properties(analyzer, expression, depth + 1) & SEMANTIC_TYPE_SEND)) return 0;
    } else if (expression->resolved_borrow_kind != AST_BORROW_NONE) {
        if (expression->resolved_pointer_depth > 1 ||
            (expression->has_resolved_ast_type && expression->resolved_ast_type.kind == AST_TYPE_FUTURE)) return 0;
        unsigned referent = expression->resolved_named_symbol_id < analyzer->model->symbol_count
            ? semantic_symbol_type_properties(analyzer->model, expression->resolved_named_symbol_id)
            : SEMANTIC_TYPE_SEND | SEMANTIC_TYPE_SYNC;
        unsigned needed = expression->resolved_borrow_kind == AST_BORROW_IMMUTABLE
            ? SEMANTIC_TYPE_SYNC : SEMANTIC_TYPE_SEND;
        if (!(referent & needed)) return 0;
    } else if (expression->has_resolved_ast_type) {
        const AstProgram *source = expression->resolved_type_program != NULL
            ? expression->resolved_type_program : analyzer->program;
        if (!(semantic_declared_type_properties(analyzer, source, &expression->resolved_ast_type) & SEMANTIC_TYPE_SEND))
            return 0;
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
            symbol->declaration->as.function.is_async)
            symbol->async_frame_properties = owned | SEMANTIC_TYPE_SEND;
    }
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
            SemanticSymbol *symbol = &analyzer->model->symbols[i];
            if (!(symbol->async_frame_properties & SEMANTIC_TYPE_SEND)) continue;
            analyzer->program = (AstProgram *) symbol->source_program;
            int send = symbol->owner_symbol_id == AST_SYMBOL_NONE;
            const AstDeclarationNode *function = symbol->declaration;
            for (const AstParameter *p = function->as.function.parameters; p; p = p->next)
                if (p->type.borrow_kind != AST_BORROW_NONE ||
                    !(semantic_declared_type_properties(analyzer, symbol->source_program, &p->type) & SEMANTIC_TYPE_SEND))
                    send = 0;
            if (!(semantic_declared_type_properties(analyzer, symbol->source_program,
                    &function->as.function.return_type) & SEMANTIC_TYPE_SEND) ||
                !async_statements_send(analyzer, function->as.function.body, 0)) send = 0;
            if (!send) { symbol->async_frame_properties = owned; changed = 1; }
        }
    }
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind == SEMANTIC_SYMBOL_LOCAL && symbol->declared_type.kind == AST_TYPE_FUTURE && symbol->node != NULL)
            symbol->type_properties = async_future_properties(analyzer, symbol->node, 0);
    }
}
