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
    int reachable;
} OwnershipFlow;

typedef struct OwnershipLoop {
    OwnershipFlow breaks;
    OwnershipFlow continues;
    struct OwnershipLoop *parent;
} OwnershipLoop;

typedef struct {
    Analyzer *analyzer;
    size_t count;
    OwnershipLoop *loop;
} OwnershipChecker;

static OwnershipFlow flow_new(OwnershipChecker *checker, int reachable) {
    OwnershipFlow flow = {0};
    flow.values = calloc(checker->count, sizeof(*flow.values));
    flow.reachable = reachable;
    if (flow.values == NULL && checker->count != 0)
        checker->analyzer->allocation_failed = 1;
    return flow;
}

static OwnershipFlow flow_clone(OwnershipChecker *checker,
                                const OwnershipFlow *source) {
    OwnershipFlow flow = flow_new(checker, source->reachable);
    if (flow.values != NULL && source->values != NULL)
        memcpy(flow.values, source->values, checker->count);
    return flow;
}

static void flow_free(OwnershipFlow *flow) {
    free(flow->values);
    flow->values = NULL;
    flow->reachable = 0;
}

static void flow_merge(OwnershipChecker *checker, OwnershipFlow *target,
                       const OwnershipFlow *source) {
    if (!source->reachable || source->values == NULL) return;
    if (!target->reachable) {
        if (target->values != NULL)
            memcpy(target->values, source->values, checker->count);
        target->reachable = 1;
        return;
    }
    for (size_t i = 0; i < checker->count; i++)
        target->values[i] |= source->values[i];
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
    semantic_error(checker->analyzer, token, ERROR_CATEGORY_SEMANTIC,
                   ERR_SEM_INVALID_DECLARATION, message);
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
    ownership_error(checker, expression->value_token,
                    "Cannot read, borrow, copy, or move a value after ownership was moved");
    return 0;
}

static void read_expression(OwnershipChecker *checker, OwnershipFlow *flow,
                            const AstExpression *expression);

static void consume_expression(OwnershipChecker *checker,
                               OwnershipFlow *flow,
                               const AstExpression *expression) {
    if (expression == NULL) return;
    if (!semantic_expression_is_move_only(checker->analyzer, expression)) {
        read_expression(checker, flow, expression);
        return;
    }
    if (expression->kind == AST_EXPR_NAME) {
        if (expression_available(checker, flow, expression) &&
            expression->resolved_symbol_id < checker->count)
            flow->values[expression->resolved_symbol_id] = OWNERSHIP_MOVED;
        return;
    }
    if (expression->kind == AST_EXPR_MEMBER ||
        expression->kind == AST_EXPR_INDEX) {
        read_expression(checker, flow, expression);
        ownership_error(checker, expression->first_token,
                        "Partial moves from move-only aggregates are not supported");
        return;
    }
    read_expression(checker, flow, expression);
}

static void read_call(OwnershipChecker *checker, OwnershipFlow *flow,
                      const AstExpression *expression) {
    read_expression(checker, flow, expression->left);
    const AstParameter *parameter = NULL;
    if (expression->resolved_symbol_id < checker->count) {
        const SemanticSymbol *function =
            &checker->analyzer->model->symbols[expression->resolved_symbol_id];
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION &&
            function->declaration != NULL)
            parameter = function->declaration->as.function.parameters;
    }
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) {
        if (parameter != NULL &&
            parameter->type.borrow_kind == AST_BORROW_NONE &&
            semantic_expression_is_move_only(checker->analyzer, argument))
            consume_expression(checker, flow, argument);
        else
            read_expression(checker, flow, argument);
        if (parameter != NULL) parameter = parameter->next;
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
        if (expression->kind == AST_EXPR_CALL) {
            read_call(checker, flow, expression);
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
                                      OwnershipFlow flow);

static OwnershipFlow check_loop(OwnershipChecker *checker,
                                const AstStatement *statement,
                                OwnershipFlow flow, int is_for) {
    if (is_for)
        flow = check_statements(checker, statement->initializer, flow);
    read_expression(checker, &flow, statement->condition);
    OwnershipFlow entry = flow_clone(checker, &flow);
    OwnershipLoop loop = {
        .breaks = flow_new(checker, 0),
        .continues = flow_new(checker, 0),
        .parent = checker->loop
    };
    checker->loop = &loop;
    OwnershipFlow body = flow_clone(checker, &entry);
    body = check_statements(checker, statement->body, body);
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
                                     otherwise);
        flow_free(&result);
        result = otherwise;
    }
    return result;
}

static OwnershipFlow check_match(OwnershipChecker *checker,
                                 const AstStatement *statement,
                                 OwnershipFlow flow) {
    read_expression(checker, &flow, statement->value);
    OwnershipFlow merged = flow_new(checker, 0);
    for (const AstMatchArm *arm = statement->match_arms;
         arm != NULL; arm = arm->next) {
        OwnershipFlow branch = flow_clone(checker, &flow);
        for (const AstParameter *binding = arm->bindings;
             binding != NULL; binding = binding->next)
            if (binding->resolved_symbol_id < checker->count &&
                move_only_symbol(checker, binding->resolved_symbol_id))
                branch.values[binding->resolved_symbol_id] = OWNERSHIP_LIVE;
        branch = check_statements(checker, arm->body, branch);
        flow_merge(checker, &merged, &branch);
        flow_free(&branch);
    }
    if (!statement->match_exhaustive)
        flow_merge(checker, &merged, &flow);
    flow_free(&flow);
    return merged;
}

static OwnershipFlow check_statements(OwnershipChecker *checker,
                                      const AstStatement *statement,
                                      OwnershipFlow flow) {
    for (; statement != NULL && flow.reachable; statement = statement->next) {
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
                    move_only_symbol(checker, statement->resolved_symbol_id))
                    flow.values[statement->resolved_symbol_id] = OWNERSHIP_LIVE;
                break;
            case AST_STMT_ASSIGNMENT:
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
                                     statement->expression->resolved_symbol_id))
                    flow.values[statement->expression->resolved_symbol_id] =
                        OWNERSHIP_LIVE;
                break;
            case AST_STMT_EXPRESSION:
            case AST_STMT_DEFER:
                read_expression(checker, &flow, statement->expression);
                if (statement->kind == AST_STMT_DEFER && statement->body != NULL)
                    flow = check_statements(checker, statement->body, flow);
                break;
            case AST_STMT_RETURN:
                if (statement->value != NULL &&
                    semantic_expression_is_move_only(checker->analyzer,
                                                     statement->value))
                    consume_expression(checker, &flow, statement->value);
                else
                    read_expression(checker, &flow, statement->value);
                flow.reachable = 0;
                break;
            case AST_STMT_BREAK:
                if (checker->loop != NULL)
                    flow_merge(checker, &checker->loop->breaks, &flow);
                flow.reachable = 0;
                break;
            case AST_STMT_CONTINUE:
                if (checker->loop != NULL)
                    flow_merge(checker, &checker->loop->continues, &flow);
                flow.reachable = 0;
                break;
            case AST_STMT_BLOCK:
                flow = check_statements(checker, statement->body, flow);
                break;
            case AST_STMT_IF: {
                read_expression(checker, &flow, statement->condition);
                OwnershipFlow then_flow = flow_clone(checker, &flow);
                OwnershipFlow else_flow = flow_clone(checker, &flow);
                then_flow = check_statements(checker, statement->body,
                                             then_flow);
                if (statement->else_body != NULL)
                    else_flow = check_statements(checker,
                                                 statement->else_body,
                                                 else_flow);
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
                flow = check_loop(checker, statement, flow, 0);
                break;
            case AST_STMT_FOR:
                flow = check_loop(checker, statement, flow, 1);
                break;
            case AST_STMT_MATCH:
                flow = check_match(checker, statement, flow);
                break;
            case AST_STMT_ERROR:
                break;
        }
    }
    return flow;
}

void validate_function_ownership(Analyzer *analyzer,
                                 const AstDeclarationNode *function) {
    if (analyzer == NULL || function == NULL ||
        function->as.function.body == NULL)
        return;
    OwnershipChecker checker = {
        .analyzer = analyzer,
        .count = analyzer->model->symbol_count
    };
    OwnershipFlow flow = flow_new(&checker, 1);
    if (flow.values == NULL) return;
    for (const AstParameter *parameter = function->as.function.parameters;
         parameter != NULL; parameter = parameter->next)
        if (parameter->resolved_symbol_id < checker.count &&
            move_only_symbol(&checker, parameter->resolved_symbol_id))
            flow.values[parameter->resolved_symbol_id] = OWNERSHIP_LIVE;
    flow = check_statements(&checker, function->as.function.body, flow);
    flow_free(&flow);
}
