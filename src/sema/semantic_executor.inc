/* Intrinsics preserve the ordinary AST children so ownership and provenance
   passes see the exact consumed value, including the concrete Future frame. */
static int async_owned_kind(const AstExpression *v, AstTypeKind kind) {
    return v && v->has_resolved_ast_type && v->resolved_ast_type.kind == kind &&
           !v->resolved_pointer_depth && !v->resolved_outer_pointer_depth &&
           v->resolved_borrow_kind == AST_BORROW_NONE && !v->resolved_is_array && !v->resolved_is_slice;
}

static AstType async_handle_type(Analyzer *a, AstTypeKind kind, const AstProgram *unit, const AstType *output) {
    AstType type = {.kind=kind, .array_length_token=AST_TOKEN_NONE};
    type.name_token = concrete_token(a, TOKEN_IDENTIFIER, kind == AST_TYPE_JOIN ? "JoinHandle" : "Future");
    AstTypeArgument *arg = ast_program_alloc(a->program, sizeof(*arg));
    if (!arg) { a->allocation_failed=1; return type; }
    arg->type = argument_type_copy(a, unit, *output);
    type.arguments=arg;
    return type;
}

static AstType async_join_result(Analyzer *a, const AstProgram *unit, const AstType *output) {
    const AstProgram *root=a->model->program;
    char result_name[4096], error_name[4096];
    snprintf(result_name,sizeof(result_name),root->package ? "%s::Result" : "%sResult",
             root->package ? root->module_identity : "");
    snprintf(error_name,sizeof(error_name),root->package ? "%s::TaskError" : "%sTaskError",
             root->package ? root->module_identity : "");
    AstType result = {.kind=AST_TYPE_NAMED, .name_token=concrete_token(a,TOKEN_IDENTIFIER,result_name),
                      .array_length_token=AST_TOKEN_NONE};
    AstTypeArgument *value = ast_program_alloc(a->program,sizeof(*value));
    AstTypeArgument *error = ast_program_alloc(a->program,sizeof(*error));
    if (!value || !error) { a->allocation_failed=1; return result; }
    value->type=argument_type_copy(a,unit,*output);
    error->type=(AstType){.kind=AST_TYPE_NAMED,.name_token=concrete_token(a,TOKEN_IDENTIFIER,error_name),
                          .array_length_token=AST_TOKEN_NONE};
    value->next=error; result.arguments=value;
    normalize_generic_type(a,&result,0);
    return result;
}

static int analyze_async_call(Analyzer *a, AstExpression *e) {
    if (e->kind != AST_EXPR_CALL || !e->left) return 0;
    AstExpression *callee=e->left, *receiver=NULL;
    AstAsyncOperation op=ASYNC_NONE;
    if (callee->kind == AST_EXPR_NAME && !find_local(a,callee->value_token) &&
        !scoped_find_global(a->model,a->program,ast_program_lexeme(a->program,callee->value_token),SEMANTIC_SYMBOL_FUNCTION)) {
        const char *name=ast_program_lexeme(a->program,callee->value_token);
        if (!strcmp(name,"spawn")) op=ASYNC_SPAWN;
        if (!strcmp(name,"block_on")) op=ASYNC_BLOCK_ON;
        if (!strcmp(name,"cancel")) op=ASYNC_CANCEL;
        if (!strcmp(name,"nativeFuture")) op=ASYNC_NATIVE_FUTURE;
    } else if (callee->kind == AST_EXPR_MEMBER && callee->left) {
        const char *name=ast_program_lexeme(a->program,callee->value_token);
        if (!strcmp(name,"create") && callee->left->kind == AST_EXPR_NAME &&
            !strcmp(ast_program_lexeme(a->program,callee->left->value_token),"Executor")) op=ASYNC_CREATE;
        else if (!strcmp(name,"spawn") || !strcmp(name,"block_on") || !strcmp(name,"shutdown")) {
            analyze_expression_context(a,callee->left,0);
            if (async_owned_kind(callee->left,AST_TYPE_EXECUTOR)) {
                receiver=callee->left;
                op=!strcmp(name,"spawn") ? ASYNC_SPAWN : !strcmp(name,"block_on") ? ASYNC_BLOCK_ON : ASYNC_SHUTDOWN;
            }
        }
    }
    if (!op) return 0;
    e->async_operation=op;
    if (!semantic_async_enabled(a))
        semantic_error(a,e->first_token,ERROR_CATEGORY_SEMANTIC,ERR_SEM_INVALID_DECLARATION,
                       "Executor operations require the async manifest feature");
    for (AstExpression *arg=e->arguments;arg;arg=arg->next) analyze_expression_context(a,arg,0);
    AstExpression *arg=e->arguments;
    if (!arg || arg->next) {
        semantic_error(a,e->first_token,ERROR_CATEGORY_SEMANTIC,ERR_SEM_WRONG_ARG_COUNT,
                       "Executor operation requires exactly one argument");
        return 1;
    }
    AstType output={.kind=AST_TYPE_NAMED,.name_token=concrete_token(a,TOKEN_TYPE_VOID,"void"),
                    .array_length_token=AST_TOKEN_NONE};
    AstType type;
    if (op==ASYNC_CREATE) {
        if (!integral_expression(arg))
            semantic_error(a,arg->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,"workerCount must be usize");
        if (arg->kind == AST_EXPR_LITERAL &&
            strtoll(ast_program_lexeme(a->program,arg->value_token),NULL,10)<=0)
            semantic_error(a,arg->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,"Executor requires at least one worker");
        type=(AstType){.kind=AST_TYPE_EXECUTOR,.name_token=concrete_token(a,TOKEN_IDENTIFIER,"Executor"),
                       .array_length_token=AST_TOKEN_NONE};
    } else if (op==ASYNC_NATIVE_FUTURE) {
        if (arg->resolved_type != TYPE_VOID || arg->resolved_pointer_depth != 1 ||
            arg->resolved_outer_pointer_depth || arg->resolved_borrow_kind != AST_BORROW_NONE ||
            arg->resolved_is_array || arg->resolved_is_slice)
            semantic_error(a,arg->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,"nativeFuture requires an owned native Future<void> frame pointer (*void)");
        type=async_handle_type(a,AST_TYPE_FUTURE,a->program,&output);
    } else if (op==ASYNC_SHUTDOWN) {
        const SemanticSymbol *mode=scoped_find_global(a->model,a->program,"ShutdownMode",SEMANTIC_SYMBOL_ENUM);
        if (!mode || arg->resolved_named_symbol_id!=mode->id || arg->resolved_pointer_depth)
            semantic_error(a,arg->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,"shutdown requires ShutdownMode");
        type=async_handle_type(a,AST_TYPE_FUTURE,a->program,&output);
        (void)receiver;
    } else {
        int valid=async_owned_kind(arg,AST_TYPE_FUTURE) ||
                  (op==ASYNC_CANCEL && async_owned_kind(arg,AST_TYPE_JOIN));
        if (!valid || !arg->resolved_ast_type.arguments) {
            semantic_error(a,arg->first_token,ERROR_CATEGORY_TYPE,ERR_TYPE_INVALID_OPERATION,
                           op==ASYNC_CANCEL ? "cancel requires an owned Future or JoinHandle" : "operation requires an owned Future");
            return 1;
        }
        const AstProgram *unit=arg->resolved_type_program ? arg->resolved_type_program : a->program;
        if (op==ASYNC_SPAWN) type=async_handle_type(a,AST_TYPE_JOIN,unit,&arg->resolved_ast_type.arguments->type);
        else if (op==ASYNC_CANCEL) type=async_handle_type(a,AST_TYPE_FUTURE,a->program,&output);
        else {
            if (a->current_function && a->current_function->as.function.is_async)
                semantic_error(a,e->first_token,ERROR_CATEGORY_SEMANTIC,ERR_SEM_INVALID_DECLARATION,
                               "block_on is only valid inside a synchronous function");
            type=argument_type_copy(a,unit,arg->resolved_ast_type.arguments->type);
        }
    }
    set_expression_declared_type(a,e,a->program,&type);
    return 1;
}
