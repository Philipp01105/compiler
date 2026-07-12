#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BorrowRecord {
    size_t owner_symbol;
    size_t field_symbol;
    size_t borrower_symbol;
    size_t borrower_field_symbol;
    AstBorrowKind kind;
    int slice_view;
    int region_known;
    size_t region_start;
    size_t region_end;
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
    int region_known;
    size_t region_start;
    size_t region_end;
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
                                        size_t symbol,
                                        size_t field) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && borrow->borrower_symbol == symbol &&
            borrow->borrower_field_symbol == field)
            return borrow;
    return NULL;
}

static int expression_place(const AstExpression *expression,
                            BorrowPlace *place) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_NAME) {
        place->owner = expression->resolved_symbol_id;
        place->field = AST_SYMBOL_NONE;
        place->region_known = 0;
        place->region_start = 0;
        place->region_end = 0;
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
    if (expression->kind == AST_EXPR_INDEX ||
        expression->kind == AST_EXPR_SUBSLICE)
        return expression_place(expression->left, place);
    if (expression->kind == AST_EXPR_SLICE_DATA)
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
    if (expression->kind == AST_EXPR_INDEX &&
        expression->right != NULL &&
        expression->right->kind == AST_EXPR_LITERAL &&
        expression->right->value_token <
            checker->analyzer->program->token_count) {
        const char *value = ast_program_lexeme(
            checker->analyzer->program, expression->right->value_token);
        char *end = NULL;
        unsigned long long index = strtoull(value, &end, 10);
        if (end != value && *end == '\0' && index < SIZE_MAX) {
            place->region_known = 1;
            place->region_start = (size_t) index;
            place->region_end = (size_t) index + 1;
        }
    }
    BorrowRecord *source = NULL;
    size_t remaining = checker->analyzer->model->symbol_count + 1;
    while (remaining-- != 0 &&
           (source = borrow_by_borrower(checker, place->owner,
                                        place->field)) != NULL) {
        place->through = source;
        place->owner = source->owner_symbol;
        place->field = source->field_symbol;
        if (source->region_known) {
            place->region_known = 1;
            place->region_start = source->region_start;
            place->region_end = source->region_end;
        } else if (source->slice_view) {
            place->region_known = 0;
        }
    }
    return place->owner < checker->analyzer->model->symbol_count;
}

typedef struct {
    size_t symbol;
    size_t field;
    int set;
    int ambiguous;
} ReturnOrigin;

static void merge_return_origin(ReturnOrigin *origin, const BorrowPlace *place) {
    if (!origin->set) {
        origin->symbol = place->owner;
        origin->field = place->field;
        origin->set = 1;
    } else if (origin->symbol != place->owner ||
               origin->field != place->field) {
        origin->ambiguous = 1;
    }
}

static int returned_borrow_place(BorrowChecker *checker,
                                 const AstExpression *call,
                                 BorrowPlace *place);

static int expression_origin(BorrowChecker *checker,
                             const AstExpression *expression,
                             BorrowPlace *place,
                             size_t depth);

static void collect_return_origins(BorrowChecker *checker,
                                   const AstStatement *statement,
                                   ReturnOrigin *origin,
                                   size_t depth) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_RETURN &&
            statement->value != NULL) {
            const AstExpression *value = statement->value;
            if (value->kind == AST_EXPR_UNARY &&
                value->operator_type == TOKEN_AMPERSAND)
                value = value->right;
            BorrowPlace place;
            if (!expression_origin(checker, value, &place, depth + 1)) {
                origin->ambiguous = 1;
            } else merge_return_origin(origin, &place);
        }
        collect_return_origins(checker, statement->initializer, origin, depth);
        collect_return_origins(checker, statement->body, origin, depth);
        collect_return_origins(checker, statement->else_body, origin, depth);
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            collect_return_origins(checker, arm->body, origin, depth);
    }
}

static int expression_origin(BorrowChecker *checker,
                             const AstExpression *expression,
                             BorrowPlace *place,
                             size_t depth) {
    if (expression == NULL || depth > 64) return 0;
    if (expression_place(expression, place)) return 1;
    if (expression->kind == AST_EXPR_SLICE)
        return expression_origin(checker, expression->left, place, depth + 1);
    if (expression->kind == AST_EXPR_CAST ||
        (expression->kind == AST_EXPR_UNARY &&
         expression->operator_type != TOKEN_AMPERSAND))
        return expression_origin(checker,
                                 expression->arguments != NULL
                                     ? expression->arguments : expression->right,
                                 place, depth + 1);
    return expression->kind == AST_EXPR_CALL &&
           returned_borrow_place(checker, expression, place);
}

static int returned_borrow_place(BorrowChecker *checker,
                                 const AstExpression *call,
                                 BorrowPlace *place) {
    if (call == NULL || call->kind != AST_EXPR_CALL ||
        call->resolved_symbol_id >= checker->analyzer->model->symbol_count)
        return 0;
    const SemanticSymbol *function =
        &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION)
        return 0;
    if (function->declaration != NULL) {
        ReturnOrigin origin = {0};
        collect_return_origins(checker, function->declaration->as.function.body,
                               &origin, 0);
        if (origin.set && !origin.ambiguous) {
            const SemanticSymbol *symbol =
                origin.symbol < checker->analyzer->model->symbol_count
                    ? &checker->analyzer->model->symbols[origin.symbol]
                    : NULL;
            if (symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_FIELD &&
                call->left != NULL && call->left->kind == AST_EXPR_MEMBER &&
                call->left->left != NULL) {
                if (!canonical_place(checker, call->left->left, place)) return 0;
                place->field = origin.field == AST_SYMBOL_NONE
                                   ? origin.symbol
                                   : origin.field;
                return 1;
            }
            if (symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_PARAMETER) {
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
            if (symbol != NULL && symbol->scope_depth == 0 &&
                (symbol->kind == SEMANTIC_SYMBOL_VARIABLE ||
                 symbol->kind == SEMANTIC_SYMBOL_CONSTANT)) {
                place->owner = origin.symbol;
                place->field = origin.field;
                place->through = NULL;
                return 1;
            }
        }
    }
    /*
     * Imported method bodies may not have resolved field symbols yet when a
     * caller is checked.  A slice/string returned by an instance method on a
     * move-only owner is therefore conservatively tied to the whole receiver.
     */
    if (call->left != NULL && call->left->kind == AST_EXPR_MEMBER &&
        call->left->left != NULL &&
        function->owner_symbol_id < checker->analyzer->model->symbol_count &&
        semantic_type_is_move_only(checker->analyzer,
                                   function->owner_symbol_id) &&
        (call->resolved_is_slice || call->resolved_type == TYPE_STRING))
        return canonical_place(checker, call->left->left, place);
    return 0;
}

static int places_overlap(const BorrowPlace *left,
                          const BorrowRecord *right) {
    if (left->owner != right->owner_symbol) return 0;
    if (left->field != AST_SYMBOL_NONE &&
        right->field_symbol != AST_SYMBOL_NONE &&
        left->field != right->field_symbol)
        return 0;
    if (left->region_known && right->region_known)
        return left->region_start < right->region_end &&
               right->region_start < left->region_end;
    return 1;
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
            !places_overlap(&place, borrow))
            continue;
        if (access == BORROW_ACCESS_WRITE) {
            if (borrow->slice_view && expression->kind == AST_EXPR_INDEX)
                continue;
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
            !places_overlap(&place, borrow))
            continue;
        if (borrow->slice_view && borrow_expression->right->kind == AST_EXPR_INDEX)
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

static int expression_targets_field(const BorrowChecker *checker,
                                    const SemanticSymbol *function,
                                    const AstExpression *expression) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_NAME) {
        if (expression->resolved_symbol_id <
            checker->analyzer->model->symbol_count)
            return checker->analyzer->model
                       ->symbols[expression->resolved_symbol_id].kind ==
                   SEMANTIC_SYMBOL_FIELD;
        if (function == NULL || function->source_program == NULL ||
            function->owner_symbol_id == AST_SYMBOL_NONE)
            return 0;
        const char *name = ast_program_lexeme(function->source_program,
                                              expression->value_token);
        for (size_t i = 0; i < checker->analyzer->model->symbol_count; i++) {
            const SemanticSymbol *candidate =
                &checker->analyzer->model->symbols[i];
            if (candidate->kind != SEMANTIC_SYMBOL_FIELD ||
                candidate->owner_symbol_id != function->owner_symbol_id ||
                candidate->source_program == NULL)
                continue;
            if (strcmp(name, ast_program_lexeme(candidate->source_program,
                                                candidate->name_token)) == 0)
                return 1;
        }
        return 0;
    }
    if (expression->kind == AST_EXPR_MEMBER ||
        expression->kind == AST_EXPR_INDEX)
        return expression_targets_field(checker, function, expression->left);
    if (expression->kind == AST_EXPR_UNARY)
        return expression_targets_field(checker, function, expression->right);
    return 0;
}

static int statements_mutate_receiver(const BorrowChecker *checker,
                                      const SemanticSymbol *function,
                                      const AstStatement *statement) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_ASSIGNMENT &&
            expression_targets_field(checker, function,
                                     statement->expression))
            return 1;
        if (statements_mutate_receiver(checker, function,
                                       statement->initializer) ||
            statements_mutate_receiver(checker, function, statement->body) ||
            statements_mutate_receiver(checker, function,
                                       statement->else_body))
            return 1;
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            if (statements_mutate_receiver(checker, function, arm->body))
                return 1;
    }
    return 0;
}

static void check_call(BorrowChecker *checker,
                       const AstExpression *expression) {
    const AstParameter *parameter = NULL;
    const SemanticSymbol *function = NULL;
    if (expression->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        function =
            &checker->analyzer->model->symbols[expression->resolved_symbol_id];
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION &&
            function->declaration != NULL)
            parameter = function->declaration->as.function.parameters;
    }
    if (expression->left != NULL &&
        expression->left->kind == AST_EXPR_MEMBER &&
        expression->left->left != NULL && function != NULL &&
        function->declaration != NULL &&
        !function->declaration->as.function.is_static) {
        BorrowAccess receiver_access =
            statements_mutate_receiver(
                checker, function, function->declaration->as.function.body)
                ? BORROW_ACCESS_WRITE : BORROW_ACCESS_READ;
        check_expression(checker, expression->left->left, receiver_access);
    } else {
        check_expression(checker, expression->left, BORROW_ACCESS_READ);
    }
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) {
        BorrowAccess argument_access =
            argument->resolved_is_slice ||
            (parameter != NULL &&
             parameter->type.borrow_kind == AST_BORROW_NONE &&
             semantic_expression_is_move_only(checker->analyzer, argument))
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
    if (expression->kind == AST_EXPR_PROPAGATE) {
        BorrowAccess operand_access =
            semantic_expression_is_move_only(checker->analyzer,
                                             expression->left)
                ? BORROW_ACCESS_WRITE
                : BORROW_ACCESS_READ;
        check_expression(checker, expression->left, operand_access);
        return;
    }
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

static void deactivate_borrower(BorrowChecker *checker,
                                size_t borrower_symbol,
                                size_t borrower_field) {
    for (BorrowRecord *old = checker->borrows; old != NULL; old = old->next)
        if (old->active && old->borrower_symbol == borrower_symbol &&
            (borrower_field == AST_SYMBOL_NONE ||
             old->borrower_field_symbol == borrower_field))
            old->active = 0;
}

static BorrowRecord *add_borrow(BorrowChecker *checker,
                                const AstExpression *borrower,
                                const AstExpression *value,
                                size_t scope_depth) {
    if (value == NULL || borrower == NULL)
        return NULL;
    /*
     * Ownership metadata says who frees a returned descriptor, while
     * provenance says whether it aliases existing storage.  A library method
     * can return an owning type's field through a slice constructor, so do not
     * discard a provable origin merely because an earlier lowering pass marked
     * the descriptor as owning.
     */
    BorrowPlace returned_place;
    int has_returned_place = value->kind == AST_EXPR_CALL &&
                             returned_borrow_place(checker, value,
                                                   &returned_place);
    int slice_view = value->resolved_is_slice &&
                     value->resolved_borrow_kind == AST_BORROW_NONE;
    int string_view = value->resolved_type == TYPE_STRING &&
                      has_returned_place;
    if (!slice_view && !string_view &&
        value->resolved_borrow_kind == AST_BORROW_NONE)
        return NULL;
    BorrowPlace place, borrower_place;
    if (!expression_place(borrower, &borrower_place))
        return NULL;
    const AstExpression *origin = value;
    if (origin->kind == AST_EXPR_UNARY &&
        origin->operator_type == TOKEN_AMPERSAND)
        origin = origin->right;
    if (!canonical_place(checker, origin, &place)) {
        if (!has_returned_place) return NULL;
        place = returned_place;
    }

    deactivate_borrower(checker, borrower_place.owner,
                        borrower_place.field);

    BorrowRecord *borrow = calloc(1, sizeof(*borrow));
    if (borrow == NULL) {
        checker->analyzer->allocation_failed = 1;
        return NULL;
    }
    borrow->owner_symbol = place.owner;
    borrow->field_symbol = place.field;
    borrow->borrower_symbol = borrower_place.owner;
    borrow->borrower_field_symbol = borrower_place.field;
    borrow->kind = slice_view || string_view ? AST_BORROW_IMMUTABLE
                                              : value->resolved_borrow_kind;
    borrow->slice_view = slice_view;
    borrow->region_known = place.region_known;
    borrow->region_start = place.region_start;
    borrow->region_end = place.region_end;
    borrow->scope_depth = scope_depth;
    borrow->active = 1;
    borrow->next = checker->borrows;
    checker->borrows = borrow;
    return borrow;
}

static void clone_aggregate_borrows(BorrowChecker *checker,
                                    const AstExpression *target,
                                    const AstExpression *source,
                                    size_t scope_depth) {
    BorrowPlace target_place, source_place;
    if (!expression_place(target, &target_place) ||
        !expression_place(source, &source_place) ||
        target_place.field != AST_SYMBOL_NONE ||
        source_place.field != AST_SYMBOL_NONE)
        return;
    deactivate_borrower(checker, target_place.owner, AST_SYMBOL_NONE);
    for (BorrowRecord *old = checker->borrows; old != NULL; old = old->next) {
        if (!old->active || old->borrower_symbol != source_place.owner)
            continue;
        BorrowRecord *copy = calloc(1, sizeof(*copy));
        if (copy == NULL) {
            checker->analyzer->allocation_failed = 1;
            return;
        }
        *copy = *old;
        copy->borrower_symbol = target_place.owner;
        copy->scope_depth = scope_depth;
        copy->next = checker->borrows;
        checker->borrows = copy;
    }
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
            AstExpression borrower = {
                .kind = AST_EXPR_NAME,
                .resolved_symbol_id = statement->resolved_symbol_id
            };
            if (add_borrow(checker, &borrower, statement->value,
                           scope_depth) == NULL)
                clone_aggregate_borrows(checker, &borrower,
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
            if (add_borrow(checker, statement->expression,
                           statement->value, scope_depth) == NULL)
                clone_aggregate_borrows(checker, statement->expression,
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
        collect_return_origins(&checker, function->as.function.body,
                               &origin, 0);
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
