#include "ast_optimize.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

static int known_truth(const AstProgram *program, const AstExpression *expression, int *truth) {
    if (!expression) return 0;
    DataType type = expression->resolved_type;
    if (!data_type_integral(type) && type != TYPE_FLOAT && type != TYPE_DOUBLE) return 0;
    const char *spelling = expression->folded_constant.lexeme;
    const AstToken *token = expression->folded_constant.lexeme ? &expression->folded_constant :
                            (expression->kind == AST_EXPR_NAME || expression->kind == AST_EXPR_LITERAL)
                                ? ast_program_token(program, expression->value_token) : NULL;
    if (!spelling && token) spelling = token->lexeme;
    if (spelling && !strcmp(spelling, "true")) { *truth = 1; return 1; }
    if (spelling && !strcmp(spelling, "false")) { *truth = 0; return 1; }
    if (token && spelling && (token->type == TOKEN_NUMBER || token->type == TOKEN_FLOAT_LITERAL)) {
        char *end = NULL;
        double value = strtod(spelling, &end);
        if (end != spelling && *end == '\0') {
            *truth = value != 0.0;
            return 1;
        }
    }
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_BANG &&
        known_truth(program, expression->right, truth)) {
        *truth = !*truth;
        return 1;
    }
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_MINUS)
        return known_truth(program, expression->right, truth);
    if (expression->kind == AST_EXPR_BINARY &&
        (expression->operator_type == TOKEN_AMP_AMP || expression->operator_type == TOKEN_PIPE_PIPE)) {
        int left;
        if (!known_truth(program, expression->left, &left)) return 0;
        if ((expression->operator_type == TOKEN_AMP_AMP && !left) ||
            (expression->operator_type == TOKEN_PIPE_PIPE && left)) {
            *truth = left;
            return 1;
        }
        return known_truth(program, expression->right, truth);
    }
    return 0;
}

static void optimize_expression(AstProgram *program, AstExpression *expression, AstOptimizationStats *stats) {
    for (; expression; expression = expression->next) {
        optimize_expression(program, expression->left, stats);
        optimize_expression(program, expression->right, stats);
        optimize_expression(program, expression->arguments, stats);
        if (expression->kind != AST_EXPR_BINARY ||
            (expression->operator_type != TOKEN_AMP_AMP && expression->operator_type != TOKEN_PIPE_PIPE) ||
            expression->folded_constant.lexeme || expression->resolved_type != TYPE_BIT)
            continue;
        int left;
        if (!known_truth(program, expression->left, &left)) continue;
        if ((expression->operator_type == TOKEN_AMP_AMP && !left) ||
            (expression->operator_type == TOKEN_PIPE_PIPE && left)) {
            expression->folded_constant = (AstToken){
                .type = TOKEN_IDENTIFIER,
                .lexeme = left ? "true" : "false",
                .span = expression->span
            };
            stats->short_circuits++;
        }
    }
}

static size_t remaining_count(const AstStatement *statement) {
    size_t count = 0;
    for (; statement; statement = statement->next) count++;
    return count;
}

static int terminates(const AstStatement *statement) {
    if (!statement) return 0;
    if (statement->kind == AST_STMT_RETURN || statement->kind == AST_STMT_BREAK ||
        statement->kind == AST_STMT_CONTINUE) return 1;
    if (statement->kind == AST_STMT_BLOCK) {
        const AstStatement *last = statement->body;
        if (!last) return 0;
        while (last->next) last = last->next;
        return terminates(last);
    }
    if (statement->kind == AST_STMT_IF && statement->else_body) {
        const AstStatement *then_last = statement->body, *else_last = statement->else_body;
        if (!then_last) return 0;
        while (then_last->next) then_last = then_last->next;
        while (else_last->next) else_last = else_last->next;
        return terminates(then_last) && terminates(else_last);
    }
    return 0;
}

static int int_literal(const AstProgram *program, const AstExpression *expression, int64_t *value) {
    if (!expression || expression->resolved_type != TYPE_INT || expression->resolved_pointer_depth ||
        expression->resolved_is_array || expression->resolved_is_slice) return 0;
    const AstToken *token = expression->folded_constant.lexeme ? &expression->folded_constant :
                            expression->kind == AST_EXPR_LITERAL
                                ? ast_program_token(program, expression->value_token) : NULL;
    if (!token || token->type != TOKEN_NUMBER || !token->lexeme) return 0;
    char *end = NULL;
    long long parsed = strtoll(token->lexeme, &end, 10);
    if (end == token->lexeme || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) return 0;
    *value = parsed;
    return 1;
}

static int local_name(const AstExpression *expression, size_t symbol) {
    return expression && expression->kind == AST_EXPR_NAME &&
           expression->resolved_symbol_id == symbol && expression->resolved_type == TYPE_INT &&
           !expression->resolved_pointer_depth;
}

static char *new_integer_spelling(AstProgram *program, int64_t value) {
    char *text = ast_program_alloc(program, 32);
    if (text) (void) snprintf(text, 32, "%lld", (long long) value);
    return text;
}

/* Fold a side-effect-free counted loop to its final local updates. The
   induction variable must have an immediately preceding literal initializer;
   the body may additionally accumulate a loop-invariant integer. */
static int fold_counted_loop(AstProgram *program, AstStatement *loop,
                             const AstStatement *previous, const AstStatement *before_previous) {
    if (loop->kind != AST_STMT_WHILE || !previous || !loop->condition ||
        loop->condition->kind != AST_EXPR_BINARY ||
        (loop->condition->operator_type != TOKEN_LESS &&
         loop->condition->operator_type != TOKEN_LESS_EQUAL)) return 0;
    const AstStatement *induction = previous;
    const AstStatement *accumulator = NULL;
    if (previous->kind != AST_STMT_VARIABLE ||
        !local_name(loop->condition->left, previous->resolved_symbol_id)) {
        induction = before_previous;
        accumulator = previous;
    }
    if (!induction || induction->kind != AST_STMT_VARIABLE ||
        !local_name(loop->condition->left, induction->resolved_symbol_id) ||
        (accumulator && accumulator->kind != AST_STMT_VARIABLE)) return 0;
    int64_t start, bound;
    if (!int_literal(program, induction->value, &start) ||
        !int_literal(program, loop->condition->right, &bound)) return 0;
    if (accumulator) {
        int64_t initial_accumulator;
        if (!int_literal(program, accumulator->value, &initial_accumulator)) return 0;
    }
    AstStatement *update = loop->body;
    if (update && update->kind == AST_STMT_BLOCK && !update->next)
        update = update->body;
    AstStatement *sum = NULL;
    if (!update || update->kind != AST_STMT_ASSIGNMENT ||
        !local_name(update->expression, induction->resolved_symbol_id)) {
        sum = update;
        update = update ? update->next : NULL;
    }
    if (!update || update->next || update->kind != AST_STMT_ASSIGNMENT ||
        update->assignment_operator != TOKEN_PLUS_EQUAL ||
        !local_name(update->expression, induction->resolved_symbol_id)) return 0;
    int64_t step;
    if (!int_literal(program, update->value, &step) || step <= 0) return 0;
    int64_t delta = 0;
    if (sum) {
        if (!accumulator || sum->kind != AST_STMT_ASSIGNMENT || sum->next != update ||
            sum->assignment_operator != TOKEN_PLUS_EQUAL ||
            accumulator->resolved_symbol_id == induction->resolved_symbol_id ||
            !local_name(sum->expression, accumulator->resolved_symbol_id) ||
            !int_literal(program, sum->value, &delta)) return 0;
    }
    int64_t distance = bound - start + (loop->condition->operator_type == TOKEN_LESS_EQUAL);
    int64_t iterations = distance > 0 ? (distance + step - 1) / step : 0;
    int64_t induction_delta = iterations * step;
    int64_t total_delta = iterations * delta;
    if (induction_delta > INT32_MAX || start + induction_delta > INT32_MAX ||
        total_delta < INT32_MIN || total_delta > INT32_MAX) return 0;
    char *step_text = NULL, *sum_text = NULL;
    if (iterations) {
        step_text = new_integer_spelling(program, induction_delta);
        if (sum) sum_text = new_integer_spelling(program, total_delta);
        if (!step_text || (sum && !sum_text)) return 0;
        update->value->folded_constant = (AstToken){TOKEN_NUMBER, step_text, update->value->span};
        if (sum) sum->value->folded_constant = (AstToken){TOKEN_NUMBER, sum_text, sum->value->span};
    }
    loop->kind = AST_STMT_BLOCK;
    loop->body = iterations ? loop->body : NULL;
    loop->condition = NULL;
    return 1;
}

static void optimize_statements(AstProgram *program, AstStatement *statement, AstOptimizationStats *stats) {
    AstStatement *previous = NULL, *before_previous = NULL;
    for (; statement; statement = statement->next) {
        optimize_expression(program, statement->expression, stats);
        optimize_expression(program, statement->value, stats);
        optimize_expression(program, statement->condition, stats);
        optimize_expression(program, statement->update, stats);
        optimize_statements(program, statement->initializer, stats);
        optimize_statements(program, statement->body, stats);
        optimize_statements(program, statement->else_body, stats);
        for (AstMatchArm *arm = statement->match_arms; arm; arm = arm->next)
            optimize_statements(program, arm->body, stats);
        if (fold_counted_loop(program, statement, previous, before_previous))
            stats->constant_loops++;
        if (statement->kind == AST_STMT_IF || statement->kind == AST_STMT_WHILE ||
            statement->kind == AST_STMT_FOR) {
            int truth;
            if (known_truth(program, statement->condition, &truth) &&
                (statement->kind == AST_STMT_IF || !truth)) {
                AstStatement *selected = statement->kind == AST_STMT_IF
                                             ? (truth ? statement->body : statement->else_body)
                                             : statement->initializer;
                statement->kind = AST_STMT_BLOCK;
                statement->body = selected;
                statement->initializer = NULL;
                statement->else_body = NULL;
                statement->condition = NULL;
                statement->update = NULL;
                stats->constant_branches++;
            }
        }
        if (terminates(statement) && statement->next) {
            stats->unreachable_statements += remaining_count(statement->next);
            statement->next = NULL;
        }
        before_previous = previous;
        previous = statement;
    }
}

static void optimize_declarations(AstProgram *program, AstDeclarationNode *declaration,
                                  AstOptimizationStats *stats) {
    for (; declaration; declaration = declaration->next) {
        if (declaration->kind == AST_DECL_FUNCTION)
            optimize_statements(program, declaration->as.function.body, stats);
        else if (declaration->kind == AST_DECL_STRUCT)
            optimize_declarations(program, declaration->as.struct_decl.methods, stats);
        else if (declaration->kind == AST_DECL_INTERFACE)
            optimize_declarations(program, declaration->as.interface_decl.methods, stats);
    }
}

void ast_optimize_program(AstProgram *program, AstOptimizationStats *stats) {
    AstOptimizationStats ignored = {0};
    if (!stats) stats = &ignored;
    if (!program) return;
    optimize_declarations(program, program->root, stats);
    for (size_t i = 0; i < program->owned_import_count; i++)
        ast_optimize_program(program->owned_imports[i], stats);
}
