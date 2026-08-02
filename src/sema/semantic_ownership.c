#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    OWNERSHIP_UNINITIALIZED = 1u << 0,
    OWNERSHIP_LIVE = 1u << 1,
    OWNERSHIP_MOVED = 1u << 2
};

typedef struct {
    uint8_t *values;
    size_t *move_tokens;
    int reachable;
} OwnershipFlow;

typedef struct OwnershipDefer {
    const AstStatement *statement;
    struct OwnershipDefer *previous;
} OwnershipDefer;

typedef struct OwnershipScope {
    const AstParameter *bindings;
    const AstStatement *statements;
    struct OwnershipScope *previous;
} OwnershipScope;

typedef struct OwnershipLoop {
    OwnershipFlow breaks;
    OwnershipFlow continues;
    const OwnershipDefer *defer_boundary;
    const OwnershipScope *scope_boundary;
    struct OwnershipLoop *parent;
} OwnershipLoop;

typedef struct {
    Analyzer *analyzer;
    size_t count;
    OwnershipLoop *loop;
    size_t deferred_error_token;
    OwnershipScope *scope;
    const OwnershipDefer *active_defers;
} OwnershipChecker;

static OwnershipFlow flow_new(OwnershipChecker *checker, int reachable) {
    OwnershipFlow flow = {0};
    flow.values = calloc(checker->count, sizeof(*flow.values));
    flow.move_tokens = malloc(checker->count * sizeof(*flow.move_tokens));
    flow.reachable = reachable;
    if (checker->count != 0 &&
        (flow.values == NULL || flow.move_tokens == NULL)) {
        checker->analyzer->allocation_failed = 1;
        free(flow.values);
        free(flow.move_tokens);
        flow.values = NULL;
        flow.move_tokens = NULL;
    }
    if (flow.move_tokens != NULL)
        for (size_t i = 0; i < checker->count; i++)
            flow.move_tokens[i] = AST_TOKEN_NONE;
    return flow;
}

static OwnershipFlow flow_clone(OwnershipChecker *checker,
                                const OwnershipFlow *source) {
    OwnershipFlow flow = flow_new(checker, source->reachable);
    if (flow.values != NULL && source->values != NULL &&
        flow.move_tokens != NULL && source->move_tokens != NULL) {
        memcpy(flow.values, source->values, checker->count);
        memcpy(flow.move_tokens, source->move_tokens,
               checker->count * sizeof(*flow.move_tokens));
    }
    return flow;
}

static void flow_free(OwnershipFlow *flow) {
    free(flow->values);
    free(flow->move_tokens);
    flow->values = NULL;
    flow->move_tokens = NULL;
    flow->reachable = 0;
}

static void flow_merge(OwnershipChecker *checker, OwnershipFlow *target,
                       const OwnershipFlow *source) {
    if (!source->reachable || source->values == NULL) return;
    if (!target->reachable) {
        if (target->values != NULL) {
            memcpy(target->values, source->values, checker->count);
            memcpy(target->move_tokens, source->move_tokens,
                   checker->count * sizeof(*target->move_tokens));
        }
        target->reachable = 1;
        return;
    }
    for (size_t i = 0; i < checker->count; i++) {
        if ((source->values[i] & OWNERSHIP_MOVED) != 0 &&
            target->move_tokens[i] == AST_TOKEN_NONE)
            target->move_tokens[i] = source->move_tokens[i];
        target->values[i] |= source->values[i];
    }
}

static int move_only_symbol(const OwnershipChecker *checker, size_t symbol) {
    if (symbol >= checker->count) return 0;
    const SemanticSymbol *value =
        &checker->analyzer->model->symbols[symbol];
    /*
     * Flow state exists for storage bindings, not type/member declarations.
     * In particular, a static call such as Owner.make() names a move-only
     * struct type, but does not read or consume an Owner value.
     */
    if (value->kind != SEMANTIC_SYMBOL_LOCAL &&
        value->kind != SEMANTIC_SYMBOL_PARAMETER &&
        value->kind != SEMANTIC_SYMBOL_VARIABLE)
        return 0;
    if (value->resolved_borrow_kind != AST_BORROW_NONE ||
        value->resolved_pointer_depth != 0 ||
        value->resolved_outer_pointer_depth != 0 || value->resolved_is_slice)
        return 0;
    if (value->resolved_named_symbol_id < checker->count)
        return (semantic_symbol_type_properties(
                    checker->analyzer->model,
                    value->resolved_named_symbol_id) &
                SEMANTIC_TYPE_MOVE_ONLY) != 0;
    return (value->type_properties & SEMANTIC_TYPE_MOVE_ONLY) != 0;
}

static void ownership_error(OwnershipChecker *checker, size_t token,
                            const char *message) {
    if (checker->deferred_error_token != AST_TOKEN_NONE)
        token = checker->deferred_error_token;
    semantic_error(checker->analyzer, token, ERROR_CATEGORY_SEMANTIC,
                   ERR_SEM_INVALID_DECLARATION, message);
}

static int must_consume_symbol(const OwnershipChecker *checker, size_t id) {
    if (id >= checker->count) return 0;
    const SemanticSymbol *symbol = &checker->analyzer->model->symbols[id];
    if (symbol->kind != SEMANTIC_SYMBOL_LOCAL && symbol->kind != SEMANTIC_SYMBOL_PARAMETER)
        return 0;
    if (symbol->resolved_borrow_kind == AST_BORROW_NONE &&
        symbol->resolved_pointer_depth == 0 && symbol->resolved_outer_pointer_depth == 0 &&
        !symbol->resolved_is_slice && symbol->resolved_named_symbol_id < checker->count)
        return (semantic_symbol_type_properties(checker->analyzer->model,
                                                symbol->resolved_named_symbol_id) &
                SEMANTIC_TYPE_MUST_CONSUME) != 0;
    return (semantic_declared_type_properties(checker->analyzer, symbol->source_program,
                                              &symbol->declared_type) &
            SEMANTIC_TYPE_MUST_CONSUME) != 0;
}

static int must_consume_expression(const OwnershipChecker *checker, const AstExpression *expression) {
    if (expression == NULL || expression->resolved_borrow_kind != AST_BORROW_NONE ||
        expression->resolved_pointer_depth != 0 || expression->resolved_outer_pointer_depth != 0 ||
        expression->resolved_is_slice) return 0;
    if (semantic_expression_is_future(expression)) return 1;
    if (expression->has_resolved_ast_type && (expression->resolved_ast_type.kind == AST_TYPE_JOIN ||
        expression->resolved_ast_type.kind == AST_TYPE_EXECUTOR)) return 1;
    return (semantic_symbol_type_properties(checker->analyzer->model,
                                            expression->resolved_named_symbol_id) &
            SEMANTIC_TYPE_MUST_CONSUME) != 0;
}

static void finish_binding(OwnershipChecker *checker, OwnershipFlow *flow,
                            size_t id, size_t token) {
    if (must_consume_symbol(checker, id) && (flow->values[id] & OWNERSHIP_LIVE) != 0) {
        ownership_error(checker, token,
                        "A live Future must be awaited or transferred on every control-flow path; implicit drop is forbidden");
        flow->values[id] = OWNERSHIP_UNINITIALIZED;
    }
}

static void finish_scopes(OwnershipChecker *checker, OwnershipFlow *flow,
                           const OwnershipScope *boundary, size_t token) {
    for (const OwnershipScope *scope = checker->scope; scope != boundary && scope != NULL;
         scope = scope->previous) {
        for(const AstParameter *p=scope->bindings;p;p=p->next) finish_binding(checker,flow,p->resolved_symbol_id,token);
        for (const AstStatement *statement = scope->statements; statement; statement = statement->next)
            if (statement->kind == AST_STMT_VARIABLE)
                finish_binding(checker, flow, statement->resolved_symbol_id, token);
    }
}

static void finish_function(OwnershipChecker *checker, OwnershipFlow *flow, size_t token) {
    finish_scopes(checker, flow, NULL, token);
    for (size_t id = 0; id < checker->count; id++) {
        const SemanticSymbol *symbol = &checker->analyzer->model->symbols[id];
        if (symbol->kind == SEMANTIC_SYMBOL_PARAMETER &&
            symbol->owner_symbol_id == checker->analyzer->current_function_symbol_id)
            finish_binding(checker, flow, id, token);
    }
}

static void deferred_use_after_move_error(OwnershipChecker *checker,
                                          size_t move_token) {
    const AstToken *defer_location = ast_program_token(
        checker->analyzer->program, checker->deferred_error_token);
    const AstToken *move_location = ast_program_token(
        checker->analyzer->program, move_token);
    ErrorContext *context = error_context_create(
        SEVERITY_ERROR,
        defer_location == NULL ? 0 : defer_location->span.begin.line,
        defer_location == NULL ? 0 : defer_location->span.begin.column,
        ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
        checker->analyzer->program->source_path,
        "Deferred closure uses a value after ownership was moved");
    if (defer_location != NULL)
        error_context_set_span(context, defer_location->span.end.line,
                               defer_location->span.end.column);
    if (move_location != NULL) {
        ErrorContext *note = error_context_create(
            SEVERITY_INFO, move_location->span.begin.line,
            move_location->span.begin.column, ERROR_CATEGORY_SEMANTIC,
            ERR_SEM_INVALID_DECLARATION,
            checker->analyzer->program->source_path,
            "Value was consumed here");
        error_context_set_span(note, move_location->span.end.line,
                               move_location->span.end.column);
        error_context_add_child(context, note);
    }
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered)
        error_context_free(context);
    checker->analyzer->model->error_count++;
}

static int expression_available(OwnershipChecker *checker,
                                OwnershipFlow *flow,
                                const AstExpression *expression) {
    if (expression == NULL || expression->kind != AST_EXPR_NAME ||
        !move_only_symbol(checker, expression->resolved_symbol_id))
        return 1;
    size_t symbol = expression->resolved_symbol_id;
    if (symbol >= checker->count || flow->values[symbol] == OWNERSHIP_LIVE)
        return 1;
    if (checker->deferred_error_token != AST_TOKEN_NONE &&
        flow->move_tokens[symbol] != AST_TOKEN_NONE)
        deferred_use_after_move_error(checker, flow->move_tokens[symbol]);
    else
        ownership_error(checker, expression->value_token,
                        "Cannot read, borrow, copy, or move a value after ownership was moved");
    return 0;
}

static void read_expression(OwnershipChecker *checker, OwnershipFlow *flow,
                            const AstExpression *expression);
static OwnershipFlow run_deferred(OwnershipChecker *checker, OwnershipFlow flow,
                                  const OwnershipDefer *defer, const OwnershipDefer *boundary);
static OwnershipFlow check_control_expression(OwnershipChecker *checker,
                                               const AstExpression *expression,
                                               OwnershipFlow flow,
                                               int consuming);

static void consume_expression(OwnershipChecker *checker,
                               OwnershipFlow *flow,
                               const AstExpression *expression) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_CONTROL) {
        *flow = check_control_expression(checker, expression, *flow, 1);
        return;
    }
    if (!semantic_expression_is_move_only(checker->analyzer, expression)) {
        read_expression(checker, flow, expression);
        return;
    }
    if (expression->kind == AST_EXPR_NAME) {
        if (expression_available(checker, flow, expression) &&
            expression->resolved_symbol_id < checker->count) {
            flow->values[expression->resolved_symbol_id] = OWNERSHIP_MOVED;
            flow->move_tokens[expression->resolved_symbol_id] =
                expression->value_token;
        }
        return;
    }
    if (expression->kind == AST_EXPR_MEMBER ||
        expression->kind == AST_EXPR_INDEX ||
        expression->kind == AST_EXPR_ENUM_ACCESS) {
        read_expression(checker, flow, expression);
        ownership_error(checker, expression->first_token,
                        "Partial moves from move-only aggregates are not supported");
        return;
    }
    read_expression(checker, flow, expression);
}

static void read_call(OwnershipChecker *checker, OwnershipFlow *flow,
                      const AstExpression *expression) {
    if (expression->async_operation) {
        if (expression->left && expression->left->kind == AST_EXPR_MEMBER &&
            expression->async_operation != ASYNC_CREATE) {
            if (expression->async_operation == ASYNC_SHUTDOWN)
                consume_expression(checker, flow, expression->left->left);
            else read_expression(checker, flow, expression->left->left);
        }
        for (const AstExpression *arg = expression->arguments; arg; arg = arg->next)
            if (expression->async_operation == ASYNC_SPAWN || expression->async_operation == ASYNC_BLOCK_ON ||
                expression->async_operation == ASYNC_CANCEL) consume_expression(checker, flow, arg);
            else read_expression(checker, flow, arg);
        return;
    }
    read_expression(checker, flow, expression->left);
    const AstParameter *parameter = NULL;
    const AstTypeArgument *callable_parameter = NULL;
    if (expression->resolved_symbol_id < checker->count) {
        const SemanticSymbol *function =
            &checker->analyzer->model->symbols[expression->resolved_symbol_id];
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION &&
            function->declaration != NULL)
            parameter = function->declaration->as.function.parameters;
    }
    if (parameter == NULL && expression->left != NULL && expression->left->has_resolved_ast_type &&
        expression->left->resolved_ast_type.kind == AST_TYPE_FUNCTION)
        callable_parameter = expression->left->resolved_ast_type.function_parameters;
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) {
        const AstType *parameter_type = parameter != NULL ? &parameter->type :
            callable_parameter != NULL ? &callable_parameter->type : NULL;
        if (parameter_type != NULL &&
            parameter_type->borrow_kind == AST_BORROW_NONE &&
            !(parameter_type->is_slice && argument->resolved_is_array) &&
            semantic_expression_is_move_only(checker->analyzer, argument))
            consume_expression(checker, flow, argument);
        else
            read_expression(checker, flow, argument);
        if (parameter != NULL) parameter = parameter->next;
        if (callable_parameter != NULL) callable_parameter = callable_parameter->next;
    }
}

static void read_expression(OwnershipChecker *checker, OwnershipFlow *flow,
                            const AstExpression *expression) {
    for (; expression != NULL; expression = expression->next) {
        if (!flow->reachable) return;
        if (expression->kind == AST_EXPR_NAME) {
            (void) expression_available(checker, flow, expression);
            continue;
        }
        if (expression->kind == AST_EXPR_CONTROL) {
            *flow = check_control_expression(checker, expression, *flow, 0);
            continue;
        }
        if (expression->kind == AST_EXPR_CALL) {
            read_call(checker, flow, expression);
            continue;
        }
        if (expression->kind == AST_EXPR_AWAIT) {
            consume_expression(checker, flow, expression->right);
            continue;
        }
        if (expression->kind == AST_EXPR_PROPAGATE) {
            if (semantic_expression_is_move_only(checker->analyzer,
                                                 expression->left))
                consume_expression(checker, flow, expression->left);
            else
                read_expression(checker, flow, expression->left);
            /* The residual edge returns before the following statement. */
            OwnershipFlow residual = flow_clone(checker, flow);
            if (residual.values != NULL) {
                residual = run_deferred(checker, residual, checker->active_defers, NULL);
                finish_function(checker, &residual, expression->first_token);
            }
            flow_free(&residual);
            continue;
        }
        if (expression->kind == AST_EXPR_ARRAY_LITERAL ||
            expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
            for (const AstExpression *argument = expression->arguments;
                 argument != NULL; argument = argument->next) {
                if (semantic_expression_is_move_only(checker->analyzer,
                                                     argument))
                    consume_expression(checker, flow, argument);
                else
                    read_expression(checker, flow, argument);
            }
            continue;
        }
        read_expression(checker, flow, expression->left);
        read_expression(checker, flow, expression->right);
        read_expression(checker, flow, expression->arguments);
    }
}

static OwnershipFlow check_statements(OwnershipChecker *checker,
                                      const AstStatement *statement,
                                      OwnershipFlow flow,
                                      const OwnershipDefer *inherited_defers);
static OwnershipFlow check_statements_tail(OwnershipChecker *checker,
                                           const AstStatement *statement,
                                           OwnershipFlow flow,
                                           const OwnershipDefer *inherited_defers,
                                           const AstExpression *tail,
                                           int consuming);

static OwnershipFlow run_deferred(OwnershipChecker *checker,
                                  OwnershipFlow flow,
                                  const OwnershipDefer *defer,
                                  const OwnershipDefer *boundary) {
    for (; defer != boundary && defer != NULL; defer = defer->previous) {
        size_t saved_token = checker->deferred_error_token;
        checker->deferred_error_token = defer->statement->first_token;
        flow = check_statements(checker, defer->statement->body, flow, NULL);
        checker->deferred_error_token = saved_token;
    }
    return flow;
}

static OwnershipFlow check_loop(OwnershipChecker *checker,
                                const AstStatement *statement,
                                OwnershipFlow flow, int is_for,
                                const OwnershipDefer *defers) {
    if (is_for)
        flow = check_statements(checker, statement->initializer, flow,
                                defers);
    read_expression(checker, &flow, statement->condition);
    OwnershipFlow entry = flow_clone(checker, &flow);
    OwnershipLoop loop = {
        .breaks = flow_new(checker, 0),
        .continues = flow_new(checker, 0),
        .defer_boundary = defers,
        .scope_boundary = checker->scope,
        .parent = checker->loop
    };
    checker->loop = &loop;
    OwnershipFlow body = flow_clone(checker, &entry);
    body = check_statements(checker, statement->body, body, defers);
    if (is_for && body.reachable) {
        read_expression(checker, &body, statement->update);
    }
    flow_merge(checker, &body, &loop.continues);
    checker->loop = loop.parent;

    if (body.reachable) {
        for (size_t i = 0; i < checker->count; i++) {
            if (entry.values[i] == OWNERSHIP_LIVE &&
                body.values[i] != OWNERSHIP_LIVE &&
                move_only_symbol(checker, i)) {
                ownership_error(checker, statement->first_token,
                                "Loop may consume a move-only value more than once; reassign it on every iteration");
                break;
            }
        }
    }
    OwnershipFlow result = flow_clone(checker, &entry);
    flow_merge(checker, &result, &body);
    flow_merge(checker, &result, &loop.breaks);
    flow_free(&flow);
    flow_free(&entry);
    flow_free(&body);
    flow_free(&loop.breaks);
    flow_free(&loop.continues);
    if (statement->else_body != NULL) {
        OwnershipFlow otherwise = flow_clone(checker, &result);
        otherwise = check_statements(checker, statement->else_body,
                                     otherwise, defers);
        flow_free(&result);
        result = otherwise;
    }
    return result;
}

static OwnershipFlow check_match(OwnershipChecker *checker,
                                 const AstStatement *statement,
                                 OwnershipFlow flow,
                                 const OwnershipDefer *defers) {
    if(statement->is_consuming_match) consume_expression(checker,&flow,statement->value);
    else read_expression(checker, &flow, statement->value);
    OwnershipFlow merged = flow_new(checker, 0);
    for (const AstMatchArm *arm = statement->match_arms;
         arm != NULL; arm = arm->next) {
        OwnershipFlow branch = flow_clone(checker, &flow);
        for (const AstParameter *binding = arm->bindings;
             binding != NULL; binding = binding->next)
            if (binding->resolved_symbol_id < checker->count &&
                move_only_symbol(checker, binding->resolved_symbol_id)) {
                branch.values[binding->resolved_symbol_id] = OWNERSHIP_LIVE;
                branch.move_tokens[binding->resolved_symbol_id] =
                    AST_TOKEN_NONE;
            }
        OwnershipScope bindings={.bindings=arm->bindings,.previous=checker->scope};
        checker->scope=&bindings;
        branch = check_statements(checker, arm->body, branch, defers);
        if(branch.reachable) for(const AstParameter *p=arm->bindings;p;p=p->next) finish_binding(checker,&branch,p->resolved_symbol_id,arm->variant_token);
        checker->scope=bindings.previous;
        flow_merge(checker, &merged, &branch);
        flow_free(&branch);
    }
    if (!statement->match_exhaustive)
        flow_merge(checker, &merged, &flow);
    flow_free(&flow);
    return merged;
}

static OwnershipFlow check_value_block(OwnershipChecker *checker,
                                       const AstStatement *block,
                                       OwnershipFlow flow, int consuming) {
    if (block == NULL) return flow;
    return check_statements_tail(checker, block->body, flow, NULL,
                                 block->result, consuming);
}

static OwnershipFlow check_control_expression(OwnershipChecker *checker,
                                               const AstExpression *expression,
                                               OwnershipFlow flow,
                                               int consuming) {
    const AstStatement *control = expression->control;
    if (control == NULL) return flow;
    if (control->kind == AST_STMT_BLOCK)
        return check_value_block(checker, control, flow, consuming);
    if (control->kind == AST_STMT_IF) {
        read_expression(checker, &flow, control->condition);
        if (control->condition != NULL &&
            control->condition->resolved_type == TYPE_NEVER) {
            flow.reachable = 0;
            return flow;
        }
        OwnershipFlow then_flow = flow_clone(checker, &flow);
        OwnershipFlow else_flow = flow_clone(checker, &flow);
        then_flow = check_value_block(checker, control->body, then_flow,
                                      consuming);
        else_flow = check_value_block(checker, control->else_body,
                                      else_flow, consuming);
        OwnershipFlow merged = flow_new(checker, 0);
        flow_merge(checker, &merged, &then_flow);
        flow_merge(checker, &merged, &else_flow);
        flow_free(&flow);
        flow_free(&then_flow);
        flow_free(&else_flow);
        return merged;
    }
    if (control->kind != AST_STMT_MATCH) return flow;
    if(control->is_consuming_match) consume_expression(checker,&flow,control->value);
    else read_expression(checker, &flow, control->value);
    OwnershipFlow merged = flow_new(checker, 0);
    for (const AstMatchArm *arm = control->match_arms; arm;
         arm = arm->next) {
        if (control->is_type_match && arm != control->selected_type_arm)
            continue;
        OwnershipFlow branch = flow_clone(checker, &flow);
        for (const AstParameter *binding = arm->bindings; binding;
             binding = binding->next)
            if (binding->resolved_symbol_id < checker->count &&
                move_only_symbol(checker, binding->resolved_symbol_id)) {
                branch.values[binding->resolved_symbol_id] = OWNERSHIP_LIVE;
                branch.move_tokens[binding->resolved_symbol_id] =
                    AST_TOKEN_NONE;
            }
        OwnershipScope bindings={.bindings=arm->bindings,.previous=checker->scope};
        checker->scope=&bindings;
        branch = check_value_block(checker, arm->body, branch, consuming);
        if(branch.reachable) for(const AstParameter *p=arm->bindings;p;p=p->next) finish_binding(checker,&branch,p->resolved_symbol_id,arm->variant_token);
        checker->scope=bindings.previous;
        flow_merge(checker, &merged, &branch);
        flow_free(&branch);
    }
    flow_free(&flow);
    return merged;
}

static OwnershipFlow check_statements_tail(OwnershipChecker *checker,
                                           const AstStatement *statement,
                                           OwnershipFlow flow,
                                           const OwnershipDefer *inherited_defers,
                                           const AstExpression *tail,
                                           int consuming) {
    OwnershipScope scope = {.statements = statement, .previous = checker->scope};
    checker->scope = &scope;
    const OwnershipDefer *saved_defers = checker->active_defers;
    const OwnershipDefer *defers = inherited_defers;
    for (; statement != NULL && flow.reachable; statement = statement->next) {
        checker->active_defers = defers;
        switch (statement->kind) {
            case AST_STMT_VARIABLE:
                if (statement->value != NULL) {
                    if (semantic_expression_is_move_only(checker->analyzer,
                                                         statement->value))
                        consume_expression(checker, &flow, statement->value);
                    else
                        read_expression(checker, &flow, statement->value);
                }
                if (statement->resolved_symbol_id < checker->count &&
                    move_only_symbol(checker, statement->resolved_symbol_id)) {
                    flow.values[statement->resolved_symbol_id] = statement->value == NULL &&
                        must_consume_symbol(checker, statement->resolved_symbol_id)
                            ? OWNERSHIP_UNINITIALIZED : OWNERSHIP_LIVE;
                    flow.move_tokens[statement->resolved_symbol_id] =
                        AST_TOKEN_NONE;
                }
                break;
            case AST_STMT_ASSIGNMENT:
                if (statement->assignment_operator == TOKEN_EQUAL &&
                    statement->expression != NULL && statement->value != NULL &&
                    statement->expression->kind == AST_EXPR_NAME &&
                    statement->value->kind == AST_EXPR_NAME &&
                    statement->expression->resolved_symbol_id ==
                        statement->value->resolved_symbol_id &&
                    move_only_symbol(checker,
                                     statement->expression->resolved_symbol_id))
                    ownership_error(checker, statement->first_token,
                                    "Cannot move a value into itself");
                if (statement->expression != NULL &&
                    statement->expression->kind != AST_EXPR_NAME)
                    read_expression(checker, &flow, statement->expression);
                if (statement->value != NULL &&
                    semantic_expression_is_move_only(checker->analyzer,
                                                     statement->value))
                    consume_expression(checker, &flow, statement->value);
                else
                    read_expression(checker, &flow, statement->value);
                if (statement->assignment_operator == TOKEN_EQUAL &&
                    statement->expression != NULL &&
                    statement->expression->kind == AST_EXPR_NAME &&
                    statement->expression->resolved_symbol_id < checker->count &&
                    move_only_symbol(checker,
                                     statement->expression->resolved_symbol_id)) {
                    finish_binding(checker, &flow, statement->expression->resolved_symbol_id,
                                   statement->first_token);
                    flow.values[statement->expression->resolved_symbol_id] =
                        OWNERSHIP_LIVE;
                    flow.move_tokens[
                        statement->expression->resolved_symbol_id] =
                        AST_TOKEN_NONE;
                }
                break;
            case AST_STMT_EXPRESSION:
                read_expression(checker, &flow, statement->expression);
                if (must_consume_expression(checker, statement->expression))
                    ownership_error(checker, statement->first_token,
                                    "A Future expression must be awaited or transferred; implicit drop is forbidden");
                if (statement->expression != NULL &&
                    statement->expression->resolved_type == TYPE_NEVER)
                    flow.reachable = 0;
                break;
            case AST_STMT_DEFER:
                read_expression(checker, &flow, statement->expression);
                if (must_consume_expression(checker, statement->expression))
                    ownership_error(checker, statement->first_token,
                                    "A deferred call cannot discard a Future; await or transfer it in a deferred closure");
                if (statement->body != NULL) {
                    OwnershipDefer *defer = malloc(sizeof(*defer));
                    if (defer == NULL) {
                        checker->analyzer->allocation_failed = 1;
                        break;
                    }
                    defer->statement = statement;
                    defer->previous = (OwnershipDefer *) defers;
                    defers = defer;
                }
                break;
            case AST_STMT_RETURN:
                if (statement->value != NULL &&
                    semantic_expression_is_move_only(checker->analyzer,
                                                     statement->value))
                    consume_expression(checker, &flow, statement->value);
                else
                    read_expression(checker, &flow, statement->value);
                if (statement->value == NULL ||
                    statement->value->resolved_type != TYPE_NEVER)
                    flow = run_deferred(checker, flow, defers, NULL);
                finish_function(checker, &flow, statement->first_token);
                flow.reachable = 0;
                break;
            case AST_STMT_BREAK:
                if (checker->loop != NULL) {
                    flow = run_deferred(checker, flow, defers,
                                        checker->loop->defer_boundary);
                    finish_scopes(checker, &flow, checker->loop->scope_boundary,
                                  statement->first_token);
                    flow_merge(checker, &checker->loop->breaks, &flow);
                }
                flow.reachable = 0;
                break;
            case AST_STMT_CONTINUE:
                if (checker->loop != NULL) {
                    flow = run_deferred(checker, flow, defers,
                                        checker->loop->defer_boundary);
                    finish_scopes(checker, &flow, checker->loop->scope_boundary,
                                  statement->first_token);
                    flow_merge(checker, &checker->loop->continues, &flow);
                }
                flow.reachable = 0;
                break;
            case AST_STMT_BLOCK:
                flow = check_statements(checker, statement->body, flow,
                                        defers);
                break;
            case AST_STMT_IF: {
                read_expression(checker, &flow, statement->condition);
                if (statement->condition != NULL &&
                    statement->condition->resolved_type == TYPE_NEVER) {
                    flow.reachable = 0;
                    break;
                }
                OwnershipFlow then_flow = flow_clone(checker, &flow);
                OwnershipFlow else_flow = flow_clone(checker, &flow);
                then_flow = check_statements(checker, statement->body,
                                             then_flow, defers);
                if (statement->else_body != NULL)
                    else_flow = check_statements(checker,
                                                 statement->else_body,
                                                 else_flow, defers);
                OwnershipFlow merged = flow_new(checker, 0);
                flow_merge(checker, &merged, &then_flow);
                flow_merge(checker, &merged, &else_flow);
                flow_free(&flow);
                flow_free(&then_flow);
                flow_free(&else_flow);
                flow = merged;
                break;
            }
            case AST_STMT_WHILE:
                flow = check_loop(checker, statement, flow, 0, defers);
                break;
            case AST_STMT_FOR:
                flow = check_loop(checker, statement, flow, 1, defers);
                break;
            case AST_STMT_MATCH:
                flow = check_match(checker, statement, flow, defers);
                break;
            case AST_STMT_ERROR:
                break;
        }
    }
    if (flow.reachable && tail != NULL) {
        if (consuming && semantic_expression_is_move_only(checker->analyzer,
                                                            tail))
            consume_expression(checker, &flow, tail);
        else
            read_expression(checker, &flow, tail);
        if (tail->resolved_type == TYPE_NEVER) flow.reachable = 0;
    }
    if (flow.reachable)
        flow = run_deferred(checker, flow, defers, inherited_defers);
    if (flow.reachable)
        finish_scopes(checker, &flow, scope.previous,
                      scope.statements == NULL ? AST_TOKEN_NONE : scope.statements->first_token);
    while (defers != inherited_defers) {
        const OwnershipDefer *previous = defers->previous;
        free((OwnershipDefer *) defers);
        defers = previous;
    }
    checker->scope = scope.previous;
    checker->active_defers = saved_defers;
    return flow;
}

static OwnershipFlow check_statements(OwnershipChecker *checker,
                                      const AstStatement *statement,
                                      OwnershipFlow flow,
                                      const OwnershipDefer *inherited_defers) {
    return check_statements_tail(checker, statement, flow,
                                 inherited_defers, NULL, 0);
}

void validate_function_ownership(Analyzer *analyzer,
                                 const AstDeclarationNode *function) {
    if (analyzer == NULL || function == NULL ||
        function->as.function.body == NULL)
        return;
    OwnershipChecker checker = {
        .analyzer = analyzer,
        .count = analyzer->model->symbol_count,
        .deferred_error_token = AST_TOKEN_NONE
    };
    OwnershipFlow flow = flow_new(&checker, 1);
    if (flow.values == NULL) return;
    for (const AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        if (parameter->resolved_symbol_id < checker.count &&
            move_only_symbol(&checker, parameter->resolved_symbol_id)) {
            flow.values[parameter->resolved_symbol_id] = OWNERSHIP_LIVE;
            flow.move_tokens[parameter->resolved_symbol_id] = AST_TOKEN_NONE;
        }
    flow = check_statements(&checker, function->as.function.body, flow, NULL);
    if (flow.reachable) finish_function(&checker, &flow, function->first_token);
    flow_free(&flow);
}
