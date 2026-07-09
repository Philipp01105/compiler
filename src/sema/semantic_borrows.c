#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct BorrowRecord {
    size_t owner_symbol;
    size_t field_symbol;
    size_t borrower_symbol;
    AstBorrowKind kind;
    size_t scope_depth;
    int active;
    struct BorrowRecord *next;
} BorrowRecord;

typedef struct {
    Analyzer *analyzer;
    size_t *last_use;
    BorrowRecord *borrows;
} BorrowChecker;

typedef enum {
    BORROW_ACCESS_READ,
    BORROW_ACCESS_WRITE
} BorrowAccess;

typedef struct {
    size_t owner;
    size_t field;
    const BorrowRecord *through;
} BorrowPlace;

static void collect_expression_uses(BorrowChecker *checker,
                                    const AstExpression *expression,
                                    int deferred) {
    for (; expression != NULL; expression = expression->next) {
        if (expression->kind == AST_EXPR_NAME &&
            expression->resolved_symbol_id < checker->analyzer->model->symbol_count) {
            size_t use = deferred ? SIZE_MAX : expression->first_token;
            if (checker->last_use[expression->resolved_symbol_id] < use)
                checker->last_use[expression->resolved_symbol_id] = use;
        }
        collect_expression_uses(checker, expression->left, deferred);
        collect_expression_uses(checker, expression->right, deferred);
        collect_expression_uses(checker, expression->arguments, deferred);
    }
}

static void collect_statement_uses(BorrowChecker *checker,
                                   const AstStatement *statement,
                                   int deferred) {
    for (; statement != NULL; statement = statement->next) {
        int body_deferred = deferred || statement->kind == AST_STMT_DEFER;
        collect_expression_uses(checker, statement->expression, deferred);
        collect_expression_uses(checker, statement->value, deferred);
        collect_expression_uses(checker, statement->condition, deferred);
        collect_expression_uses(checker, statement->update, deferred);
        collect_statement_uses(checker, statement->initializer, deferred);
        collect_statement_uses(checker, statement->body, body_deferred);
        collect_statement_uses(checker, statement->else_body, deferred);
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            collect_statement_uses(checker, arm->body, deferred);
    }
}

static BorrowRecord *borrow_by_borrower(BorrowChecker *checker,
                                        size_t symbol) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && borrow->borrower_symbol == symbol)
            return borrow;
    return NULL;
}

static int expression_place(const AstExpression *expression,
                            BorrowPlace *place) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_NAME) {
        place->owner = expression->resolved_symbol_id;
        place->field = AST_SYMBOL_NONE;
        place->through = NULL;
        return place->owner != AST_SYMBOL_NONE;
    }
    if (expression->kind == AST_EXPR_MEMBER) {
        if (!expression_place(expression->left, place)) return 0;
        if (place->field == AST_SYMBOL_NONE &&
            expression->resolved_symbol_id != AST_SYMBOL_NONE)
            place->field = expression->resolved_symbol_id;
        return 1;
    }
    if (expression->kind == AST_EXPR_INDEX)
        return expression_place(expression->left, place);
    if (expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_STAR)
        return expression_place(expression->right, place);
    return 0;
}

static int canonical_place(BorrowChecker *checker,
                           const AstExpression *expression,
                           BorrowPlace *place) {
    if (!expression_place(expression, place)) return 0;
    BorrowRecord *source = borrow_by_borrower(checker, place->owner);
    if (source != NULL) {
        place->through = source;
        place->owner = source->owner_symbol;
        if (source->field_symbol != AST_SYMBOL_NONE)
            place->field = source->field_symbol;
    }
    return place->owner < checker->analyzer->model->symbol_count;
}

typedef struct {
    size_t symbol;
    size_t field;
    int set;
    int ambiguous;
} ReturnOrigin;

static void collect_return_origins(const AstStatement *statement,
                                   ReturnOrigin *origin) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_RETURN &&
            statement->value != NULL &&
            statement->value->resolved_borrow_kind != AST_BORROW_NONE) {
            const AstExpression *value = statement->value;
            if (value->kind == AST_EXPR_UNARY &&
                value->operator_type == TOKEN_AMPERSAND)
                value = value->right;
            BorrowPlace place;
            if (!expression_place(value, &place)) {
                origin->ambiguous = 1;
            } else if (!origin->set) {
                origin->symbol = place.owner;
                origin->field = place.field;
                origin->set = 1;
            } else if (origin->symbol != place.owner ||
                       origin->field != place.field) {
                origin->ambiguous = 1;
            }
        }
        collect_return_origins(statement->initializer, origin);
        collect_return_origins(statement->body, origin);
        collect_return_origins(statement->else_body, origin);
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            collect_return_origins(arm->body, origin);
    }
}

static int returned_borrow_place(BorrowChecker *checker,
                                 const AstExpression *call,
                                 BorrowPlace *place) {
    if (call == NULL || call->kind != AST_EXPR_CALL ||
        call->resolved_symbol_id >= checker->analyzer->model->symbol_count)
        return 0;
    const SemanticSymbol *function =
        &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION ||
        function->declaration == NULL)
        return 0;
    ReturnOrigin origin = {0};
    collect_return_origins(function->declaration->as.function.body, &origin);
    if (!origin.set || origin.ambiguous) return 0;
    const SemanticSymbol *symbol =
        origin.symbol < checker->analyzer->model->symbol_count
            ? &checker->analyzer->model->symbols[origin.symbol]
            : NULL;
    if (symbol == NULL) return 0;
    if (symbol->kind == SEMANTIC_SYMBOL_PARAMETER) {
        size_t index = 0;
        const AstParameter *parameter =
            function->declaration->as.function.parameters;
        while (parameter != NULL &&
               parameter->resolved_symbol_id != origin.symbol) {
            parameter = parameter->next;
            index++;
        }
        const AstExpression *argument = call->arguments;
        while (argument != NULL && index != 0) {
            argument = argument->next;
            index--;
        }
        if (parameter == NULL || argument == NULL) return 0;
        if (argument->kind == AST_EXPR_UNARY &&
            argument->operator_type == TOKEN_AMPERSAND)
            argument = argument->right;
        if (!canonical_place(checker, argument, place)) return 0;
        if (origin.field != AST_SYMBOL_NONE)
            place->field = origin.field;
        return 1;
    }
    if (symbol->scope_depth == 0 &&
        (symbol->kind == SEMANTIC_SYMBOL_VARIABLE ||
         symbol->kind == SEMANTIC_SYMBOL_CONSTANT)) {
        place->owner = origin.symbol;
        place->field = origin.field;
        place->through = NULL;
        return 1;
    }
    return 0;
}

static int places_overlap(size_t left_owner, size_t left_field,
                          size_t right_owner, size_t right_field) {
    if (left_owner != right_owner) return 0;
    return left_field == AST_SYMBOL_NONE || right_field == AST_SYMBOL_NONE ||
           left_field == right_field;
}

static void expire_borrows(BorrowChecker *checker, size_t token) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && borrow->borrower_symbol != AST_SYMBOL_NONE &&
            borrow->borrower_symbol < checker->analyzer->model->symbol_count &&
            checker->last_use[borrow->borrower_symbol] != SIZE_MAX &&
            checker->last_use[borrow->borrower_symbol] < token)
            borrow->active = 0;
}

static void deactivate_scope(BorrowChecker *checker, size_t scope_depth) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && borrow->scope_depth >= scope_depth)
            borrow->active = 0;
}

static void report_borrow_error(BorrowChecker *checker, size_t token,
                                const char *message) {
    semantic_error(checker->analyzer, token, ERROR_CATEGORY_SEMANTIC,
                   ERR_SEM_INVALID_DECLARATION, message);
}

static void check_place_access(BorrowChecker *checker,
                               const AstExpression *expression,
                               BorrowAccess access) {
    BorrowPlace place;
    if (!canonical_place(checker, expression, &place) || place.through != NULL)
        return;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active ||
            !places_overlap(place.owner, place.field,
                            borrow->owner_symbol, borrow->field_symbol))
            continue;
        if (access == BORROW_ACCESS_WRITE) {
            report_borrow_error(checker, expression->first_token,
                                "Cannot modify or move a value while it is borrowed");
            return;
        }
        if (borrow->kind == AST_BORROW_MUTABLE) {
            report_borrow_error(checker, expression->first_token,
                                "Cannot access a value while it is mutably borrowed");
            return;
        }
    }
}

static void check_new_borrow(BorrowChecker *checker,
                             const AstExpression *borrow_expression) {
    BorrowPlace place;
    if (borrow_expression == NULL || borrow_expression->right == NULL ||
        !canonical_place(checker, borrow_expression->right, &place))
        return;
    AstBorrowKind kind = borrow_expression->mutable_borrow
                             ? AST_BORROW_MUTABLE
                             : AST_BORROW_IMMUTABLE;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active || borrow == place.through ||
            !places_overlap(place.owner, place.field,
                            borrow->owner_symbol, borrow->field_symbol))
            continue;
        if (kind == AST_BORROW_MUTABLE ||
            borrow->kind == AST_BORROW_MUTABLE) {
            report_borrow_error(checker, borrow_expression->first_token,
                                "Conflicting checked borrows");
            return;
        }
    }
}

static void check_expression(BorrowChecker *checker,
                             const AstExpression *expression,
                             BorrowAccess access);

static void check_call(BorrowChecker *checker,
                       const AstExpression *expression) {
    check_expression(checker, expression->left, BORROW_ACCESS_READ);
    const AstParameter *parameter = NULL;
    if (expression->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *function =
            &checker->analyzer->model->symbols[expression->resolved_symbol_id];
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION &&
            function->declaration != NULL)
            parameter = function->declaration->as.function.parameters;
    }
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) {
        BorrowAccess argument_access =
            parameter != NULL &&
            parameter->type.borrow_kind == AST_BORROW_NONE &&
            semantic_expression_is_move_only(checker->analyzer, argument)
                ? BORROW_ACCESS_WRITE
                : BORROW_ACCESS_READ;
        check_expression(checker, argument, argument_access);
        if (parameter != NULL) parameter = parameter->next;
    }
}

static void check_expression(BorrowChecker *checker,
                             const AstExpression *expression,
                             BorrowAccess access) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_AMPERSAND) {
        check_new_borrow(checker, expression);
        if (expression->right != NULL &&
            expression->right->kind == AST_EXPR_INDEX)
            check_expression(checker, expression->right->right,
                             BORROW_ACCESS_READ);
        return;
    }
    if (expression->kind == AST_EXPR_CALL) {
        check_call(checker, expression);
        return;
    }
    if (expression->kind == AST_EXPR_NAME ||
        expression->kind == AST_EXPR_MEMBER ||
        expression->kind == AST_EXPR_INDEX ||
        (expression->kind == AST_EXPR_UNARY &&
         expression->operator_type == TOKEN_STAR)) {
        check_place_access(checker, expression, access);
        if (expression->kind == AST_EXPR_INDEX)
            check_expression(checker, expression->right, BORROW_ACCESS_READ);
        return;
    }
    check_expression(checker, expression->left, BORROW_ACCESS_READ);
    check_expression(checker, expression->right, BORROW_ACCESS_READ);
    check_expression(checker, expression->arguments, BORROW_ACCESS_READ);
    check_expression(checker, expression->next, BORROW_ACCESS_READ);
}

static BorrowRecord *add_borrow(BorrowChecker *checker,
                                size_t borrower_symbol,
                                const AstExpression *value,
                                size_t scope_depth) {
    if (value == NULL || value->resolved_borrow_kind == AST_BORROW_NONE)
        return NULL;
    BorrowPlace place;
    const AstExpression *origin = value;
    if (origin->kind == AST_EXPR_UNARY &&
        origin->operator_type == TOKEN_AMPERSAND)
        origin = origin->right;
    if (!canonical_place(checker, origin, &place) &&
        !returned_borrow_place(checker, value, &place))
        return NULL;

    for (BorrowRecord *old = checker->borrows; old != NULL; old = old->next)
        if (old->active && old->borrower_symbol == borrower_symbol)
            old->active = 0;

    BorrowRecord *borrow = calloc(1, sizeof(*borrow));
    if (borrow == NULL) {
        checker->analyzer->allocation_failed = 1;
        return NULL;
    }
    borrow->owner_symbol = place.owner;
    borrow->field_symbol = place.field;
    borrow->borrower_symbol = borrower_symbol;
    borrow->kind = value->resolved_borrow_kind;
    borrow->scope_depth = scope_depth;
    borrow->active = 1;
    borrow->next = checker->borrows;
    checker->borrows = borrow;
    return borrow;
}

static void check_statement_list(BorrowChecker *checker,
                                 const AstStatement *statement,
                                 size_t scope_depth) {
    for (; statement != NULL; statement = statement->next) {
        expire_borrows(checker, statement->first_token);
        if (statement->kind == AST_STMT_VARIABLE) {
            BorrowAccess value_access =
                semantic_expression_is_move_only(checker->analyzer,
                                                 statement->value)
                    ? BORROW_ACCESS_WRITE
                    : BORROW_ACCESS_READ;
            check_expression(checker, statement->value, value_access);
            add_borrow(checker, statement->resolved_symbol_id,
                       statement->value, scope_depth);
            continue;
        }
        if (statement->kind == AST_STMT_ASSIGNMENT) {
            check_expression(checker, statement->expression,
                             BORROW_ACCESS_WRITE);
            BorrowAccess value_access =
                semantic_expression_is_move_only(checker->analyzer,
                                                 statement->value)
                    ? BORROW_ACCESS_WRITE
                    : BORROW_ACCESS_READ;
            check_expression(checker, statement->value, value_access);
            if (statement->expression != NULL &&
                statement->expression->kind == AST_EXPR_NAME)
                add_borrow(checker,
                           statement->expression->resolved_symbol_id,
                           statement->value, scope_depth);
            continue;
        }
        if (statement->kind == AST_STMT_DEFER) {
            check_expression(checker, statement->expression,
                             BORROW_ACCESS_READ);
            continue;
        }

        check_expression(checker, statement->expression, BORROW_ACCESS_READ);
        check_expression(checker, statement->condition, BORROW_ACCESS_READ);
        check_expression(checker, statement->update, BORROW_ACCESS_READ);
        check_expression(checker, statement->value,
                         statement->kind == AST_STMT_RETURN &&
                         semantic_expression_is_move_only(checker->analyzer,
                                                          statement->value)
                             ? BORROW_ACCESS_WRITE
                             : BORROW_ACCESS_READ);

        check_statement_list(checker, statement->initializer,
                             scope_depth + 1);
        if (statement->kind == AST_STMT_BLOCK ||
            statement->kind == AST_STMT_IF ||
            statement->kind == AST_STMT_WHILE ||
            statement->kind == AST_STMT_FOR) {
            check_statement_list(checker, statement->body, scope_depth + 1);
            deactivate_scope(checker, scope_depth + 1);
        } else {
            check_statement_list(checker, statement->body, scope_depth);
        }
        check_statement_list(checker, statement->else_body,
                             scope_depth + 1);
        deactivate_scope(checker, scope_depth + 1);
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next) {
            check_statement_list(checker, arm->body, scope_depth + 1);
            deactivate_scope(checker, scope_depth + 1);
        }
    }
}

void validate_function_borrows(Analyzer *analyzer,
                               const AstDeclarationNode *function) {
    if (analyzer == NULL || function == NULL ||
        function->as.function.body == NULL)
        return;
    BorrowChecker checker = {0};
    checker.analyzer = analyzer;
    if (function->as.function.return_type.borrow_kind != AST_BORROW_NONE) {
        ReturnOrigin origin = {0};
        collect_return_origins(function->as.function.body, &origin);
        if (origin.ambiguous)
            semantic_error(analyzer, function->name_token,
                           ERROR_CATEGORY_SEMANTIC,
                           ERR_SEM_INVALID_DECLARATION,
                           "Borrowed return must have one statically provable origin");
    }
    checker.last_use =
        calloc(analyzer->model->symbol_count, sizeof(*checker.last_use));
    if (checker.last_use == NULL) {
        analyzer->allocation_failed = 1;
        return;
    }
    collect_statement_uses(&checker, function->as.function.body, 0);
    check_statement_list(&checker, function->as.function.body,
                         analyzer->scope_depth);

    BorrowRecord *borrow = checker.borrows;
    while (borrow != NULL) {
        BorrowRecord *next = borrow->next;
        free(borrow);
        borrow = next;
    }
    free(checker.last_use);
}
