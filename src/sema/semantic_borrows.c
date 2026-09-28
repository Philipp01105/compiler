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
    int captured_by_future;
    int captured_by_aggregate;
    int retained_until_drop;
    int package_storage;
    int payload_known;
    size_t payload_enum;
    size_t payload_variant;
    size_t payload_index;
    const char *origin_lifetime;
    const struct BorrowRecord *parent;
    struct BorrowRecord *next;
} BorrowRecord;

typedef struct {
    Analyzer *analyzer;
    size_t *last_use;
    BorrowRecord *borrows;
    size_t current_scope_depth;
    unsigned aggregate_capture_depth;
    unsigned return_origin_depth;
    const AstExpression *deferred_call;
    size_t temporary_borrowers;
    int reserving_loans;
} BorrowChecker;

typedef struct {
    BorrowRecord *loan;
    int active;
} FutureLoanState;

typedef struct FutureLoanBranch {
    FutureLoanState *state;
    size_t count;
    struct FutureLoanBranch *next;
} FutureLoanBranch;

static FutureLoanBranch future_loan_snapshot(BorrowChecker *checker) {
    FutureLoanBranch branch = {0};
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        branch.count++;
    branch.state = branch.count ? calloc(branch.count, sizeof(*branch.state)) : NULL;
    if (branch.count && branch.state == NULL) {
        checker->analyzer->allocation_failed = 1;
        branch.count = 0;
        return branch;
    }
    size_t index = 0;
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        branch.state[index++] = (FutureLoanState) {.loan = loan, .active = loan->active};
    return branch;
}

static void future_loan_restore(BorrowChecker *checker, const FutureLoanBranch *branch) {
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        loan->active = 0;
    if (branch != NULL)
        for (size_t i = 0; i < branch->count; i++) branch->state[i].loan->active = branch->state[i].active;
}

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

static void collect_statement_uses(BorrowChecker *checker,
                                   const AstStatement *statement,
                                   int deferred);

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
        collect_statement_uses(checker, expression->control, deferred);
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
        collect_expression_uses(checker, statement->result, deferred);
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
    /* Aggregate returns can retain an input loan on the whole result before
       the callee's field mapping is available. Field access reborrows it. */
    if (field != AST_SYMBOL_NONE)
        for (BorrowRecord *borrow = checker->borrows; borrow; borrow = borrow->next)
            if (borrow->active && borrow->borrower_symbol == symbol &&
                borrow->borrower_field_symbol == AST_SYMBOL_NONE && !borrow->captured_by_future)
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
    const AstExpression *leaf = expression;
    while (leaf && ((leaf->kind == AST_EXPR_UNARY && leaf->operator_type == TOKEN_STAR) ||
                   leaf->kind == AST_EXPR_INDEX || leaf->kind == AST_EXPR_SUBSLICE))
        leaf = leaf->kind == AST_EXPR_UNARY ? leaf->right : leaf->left;
    size_t terminal_field = leaf && leaf->kind == AST_EXPR_MEMBER
        ? leaf->resolved_symbol_id : place->field;
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
        /* A Future owns a loan; using its handle does not access the referent. */
        if (source->captured_by_future) break;
        if (source->captured_by_aggregate &&
            (place->field == AST_SYMBOL_NONE || terminal_field == AST_SYMBOL_NONE ||
             terminal_field >= checker->analyzer->model->symbol_count ||
             checker->analyzer->model->symbols[terminal_field].declared_type.borrow_kind == AST_BORROW_NONE)) break;
        if (!place->through) place->through = source;
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

static void report_borrow_error(BorrowChecker *checker, size_t token, const char *message);

static const char *borrow_lifetime(const AstProgram *unit, const AstType *type) {
    return type && type->lifetime_token < unit->token_count &&
           unit->tokens[type->lifetime_token].type == TOKEN_LIFETIME
        ? ast_program_lexeme(unit, type->lifetime_token) : NULL;
}

static int type_mentions_lifetime(const AstProgram *unit, const AstType *type, const char *name) {
    const char *direct = borrow_lifetime(unit, type);
    if (direct && !strcmp(direct, name)) return 1;
    for (const AstLifetimeParameter *p = type ? type->lifetime_arguments : NULL; p; p = p->next)
        if (!strcmp(ast_program_lexeme(unit, p->name_token), name)) return 1;
    for (const AstTypeArgument *p = type ? type->arguments : NULL; p; p = p->next)
        if (type_mentions_lifetime(unit, &p->type, name)) return 1;
    return type && type_mentions_lifetime(unit, type->element_type, name);
}

static void check_static_borrow(BorrowChecker *checker, const AstExpression *value) {
    const AstExpression *source = value;
    if (source && source->kind == AST_EXPR_UNARY && source->operator_type == TOKEN_AMPERSAND)
        source = source->right;
    BorrowPlace place;
    if (!canonical_place(checker, source, &place)) {
        report_borrow_error(checker, value->first_token,
                           "A static borrow requires permanently live storage");
        return;
    }
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[place.owner];
    if (owner->scope_depth == 0 &&
        (owner->kind == SEMANTIC_SYMBOL_VARIABLE || owner->kind == SEMANTIC_SYMBOL_CONSTANT)) return;
    if ((owner->kind == SEMANTIC_SYMBOL_PARAMETER || owner->kind == SEMANTIC_SYMBOL_FIELD) &&
        type_mentions_lifetime(owner->source_program, &owner->declared_type, "'static")) return;
    report_borrow_error(checker, value->first_token,
                       "A static borrow requires permanently live storage");
}

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

static int returned_borrow_place_impl(BorrowChecker *checker,
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
                if (origin.field != AST_SYMBOL_NONE) {
                    AstExpression field = {.kind = AST_EXPR_MEMBER,
                        .left = (AstExpression *)argument, .resolved_symbol_id = origin.field};
                    return canonical_place(checker, &field, place);
                }
                return canonical_place(checker, argument, place);
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
        (call->resolved_is_slice || call->resolved_type == TYPE_STRING ||
         call->resolved_borrow_kind != AST_BORROW_NONE))
        return canonical_place(checker, call->left->left, place);
    return 0;
}

static int returned_borrow_place(BorrowChecker *checker,
                                 const AstExpression *call,
                                 BorrowPlace *place) {
    /* Recursive library calls must not recursively re-analyze return origins
     * without a bound. An unresolved cycle supplies no provable borrow origin. */
    if (checker->return_origin_depth >= 32) return 0;
    checker->return_origin_depth++;
    int found = returned_borrow_place_impl(checker, call, place);
    checker->return_origin_depth--;
    return found;
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

static int borrower_is_lent(const BorrowChecker *checker, const BorrowRecord *loan, size_t token) {
    for (const BorrowRecord *child = checker->borrows; child; child = child->next)
        if (child != loan && child->active && child->owner_symbol == loan->borrower_symbol &&
            child->borrower_symbol < checker->analyzer->model->symbol_count &&
            (child->retained_until_drop || child->captured_by_future ||
             checker->last_use[child->borrower_symbol] >= token)) return 1;
    return 0;
}

static void expire_borrows(BorrowChecker *checker, size_t token) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && borrow->borrower_symbol != AST_SYMBOL_NONE &&
            borrow->borrower_symbol < checker->analyzer->model->symbol_count &&
            !borrow->package_storage && !borrow->captured_by_future && !borrow->retained_until_drop &&
            checker->last_use[borrow->borrower_symbol] != SIZE_MAX &&
            checker->last_use[borrow->borrower_symbol] < token &&
            !borrower_is_lent(checker, borrow, token))
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

static int loan_ancestor(const BorrowRecord *candidate, const BorrowRecord *loan) {
    for (size_t depth = 0; loan && depth < 128; depth++, loan = loan->parent)
        if (loan == candidate) return 1;
    return 0;
}

static int package_borrow_holder(BorrowChecker *, const SemanticSymbol *);

static int place_through_shared_reference(const AstExpression *expression) {
    if (!expression) return 0;
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR)
        return expression->right &&
            (expression->right->resolved_borrow_kind == AST_BORROW_IMMUTABLE ||
             place_through_shared_reference(expression->right));
    if (expression->kind == AST_EXPR_MEMBER || expression->kind == AST_EXPR_INDEX ||
        expression->kind == AST_EXPR_SUBSLICE)
        return expression->left &&
            (expression->left->resolved_borrow_kind == AST_BORROW_IMMUTABLE ||
             place_through_shared_reference(expression->left));
    return 0;
}

static void check_place_access(BorrowChecker *checker,
                               const AstExpression *expression,
                               BorrowAccess access) {
    if ((access == BORROW_ACCESS_WRITE ||
         (expression && expression->resolved_borrow_kind == AST_BORROW_MUTABLE)) &&
        place_through_shared_reference(expression)) {
        report_borrow_error(checker, expression->first_token,
                           "Cannot obtain mutable access through a shared checked reference");
        return;
    }
    BorrowPlace storage;
    if (expression_place(expression, &storage)) {
        if (access == BORROW_ACCESS_WRITE && storage.owner < checker->analyzer->model->symbol_count &&
            package_borrow_holder(checker, &checker->analyzer->model->symbols[storage.owner])) {
            report_borrow_error(checker, expression->first_token,
                "Package storage holding checked borrows cannot be rebound, moved, or modified");
            return;
        }
        for (const BorrowRecord *child = checker->borrows; child; child = child->next) {
            if (!child->active || !child->parent || child->borrower_symbol == storage.owner) continue;
            for (const BorrowRecord *parent = child->parent; parent; parent = parent->parent) {
                if (parent->borrower_symbol == storage.owner &&
                    (storage.field == AST_SYMBOL_NONE || parent->borrower_field_symbol == AST_SYMBOL_NONE ||
                     parent->borrower_field_symbol == storage.field) &&
                    (access == BORROW_ACCESS_WRITE || child->kind == AST_BORROW_MUTABLE)) {
                    report_borrow_error(checker, expression->first_token,
                                       "Cannot access a value while a conflicting reborrow is live");
                    return;
                }
            }
        }
    }
    BorrowPlace place;
    if (!canonical_place(checker, expression, &place))
        return;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active || loan_ancestor(borrow, place.through) ||
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
    if (borrow_expression == NULL || borrow_expression->right == NULL)
        return;
    AstBorrowKind kind = borrow_expression->mutable_borrow
                             ? AST_BORROW_MUTABLE
                             : AST_BORROW_IMMUTABLE;
    if (kind == AST_BORROW_MUTABLE && place_through_shared_reference(borrow_expression->right)) {
        report_borrow_error(checker, borrow_expression->first_token,
                           "Cannot obtain mutable access through a shared checked reference");
        return;
    }
    if (!canonical_place(checker, borrow_expression->right, &place)) return;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active || loan_ancestor(borrow, place.through) ||
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
static void check_statement_list(BorrowChecker *checker,
                                 const AstStatement *statement,
                                 size_t scope_depth);

static void check_place_evaluation(BorrowChecker *checker, const AstExpression *expression) {
    if (!expression || expression->kind == AST_EXPR_NAME) return;
    if (expression->kind == AST_EXPR_MEMBER) {
        check_place_evaluation(checker, expression->left);
    } else if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR) {
        check_place_evaluation(checker, expression->right);
    } else if (expression->kind == AST_EXPR_INDEX) {
        check_place_evaluation(checker, expression->left);
        check_expression(checker, expression->right, BORROW_ACCESS_READ);
    } else check_expression(checker, expression, BORROW_ACCESS_READ);
}

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

static int statements_mutate_receiver_depth(const BorrowChecker *checker,
                                             const SemanticSymbol *function,
                                             const AstStatement *statement, unsigned depth);
static int statements_mutate_receiver(const BorrowChecker *checker,
                                       const SemanticSymbol *function, const AstStatement *statement) {
    return statements_mutate_receiver_depth(checker, function, statement, 0);
}

static int expressions_mutate_receiver(const BorrowChecker *checker,
                                        const SemanticSymbol *function,
                                        const AstExpression *expression, unsigned depth) {
    for (; expression; expression = expression->next) {
        if (expression->lifetime_operation && expression->arguments &&
            expression_targets_field(checker, function, expression->arguments)) return 1;
        if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_AMPERSAND &&
            expression->mutable_borrow && expression_targets_field(checker, function, expression->right)) return 1;
        if (expression->kind == AST_EXPR_CALL && expression->resolved_symbol_id < checker->analyzer->model->symbol_count) {
            const SemanticSymbol *callee = &checker->analyzer->model->symbols[expression->resolved_symbol_id];
            if (callee->kind == SEMANTIC_SYMBOL_FUNCTION && callee->declaration) {
                const AstParameter *parameter = callee->declaration->as.function.parameters;
                for (const AstExpression *argument = expression->arguments; argument && parameter;
                     argument = argument->next, parameter = parameter->next)
                    if (parameter->type.borrow_kind == AST_BORROW_MUTABLE &&
                        expression_targets_field(checker, function, argument)) return 1;
                if (expression->left && expression->left->kind == AST_EXPR_MEMBER &&
                    expression_targets_field(checker, function, expression->left->left) &&
                    (depth >= 32 || statements_mutate_receiver_depth(checker, callee,
                        callee->declaration->as.function.body, depth + 1))) return 1;
            }
        }
        if (expressions_mutate_receiver(checker, function, expression->left, depth) ||
            expressions_mutate_receiver(checker, function, expression->right, depth) ||
            expressions_mutate_receiver(checker, function, expression->arguments, depth)) return 1;
    }
    return 0;
}

static int statements_mutate_receiver_depth(const BorrowChecker *checker,
                                      const SemanticSymbol *function,
                                      const AstStatement *statement, unsigned depth) {
    for (; statement != NULL; statement = statement->next) {
        if (statement->kind == AST_STMT_ASSIGNMENT &&
            expression_targets_field(checker, function,
                                     statement->expression))
            return 1;
        if (expressions_mutate_receiver(checker, function, statement->expression, depth) ||
            expressions_mutate_receiver(checker, function, statement->value, depth) ||
            expressions_mutate_receiver(checker, function, statement->result, depth) ||
            statements_mutate_receiver_depth(checker, function, statement->initializer, depth) ||
            statements_mutate_receiver_depth(checker, function, statement->body, depth) ||
            statements_mutate_receiver_depth(checker, function, statement->else_body, depth))
            return 1;
        for (const AstMatchArm *arm = statement->match_arms;
             arm != NULL; arm = arm->next)
            if (statements_mutate_receiver_depth(checker, function, arm->body, depth))
                return 1;
    }
    return 0;
}

int semantic_function_mutates_receiver(const Analyzer *analyzer, size_t function_id) {
    if(function_id>=analyzer->model->symbol_count) return 0;
    const SemanticSymbol *function=&analyzer->model->symbols[function_id];
    BorrowChecker checker={.analyzer=(Analyzer *)analyzer};
    return function->declaration && statements_mutate_receiver(&checker,function,function->declaration->as.function.body);
}

static void deactivate_borrower(BorrowChecker *checker, size_t borrower_symbol,
                                size_t borrower_field);
static void clone_aggregate_borrows(BorrowChecker *checker, const AstExpression *target,
                                    const AstExpression *source, size_t scope_depth);
static BorrowRecord *add_borrow_mode(BorrowChecker *, const AstExpression *, const AstExpression *, size_t, int);
static BorrowRecord *add_lifetime_parameter_borrows(BorrowChecker *, const AstExpression *,
    const AstExpression *, const AstProgram *, const AstType *, const char *, AstBorrowKind, size_t);
static int type_has_view_arguments(const BorrowChecker *checker, const AstType *type);
static int type_has_view_depth(const BorrowChecker *, const AstProgram *, const AstType *, size_t);
static int type_has_mutable_view(const BorrowChecker *, const AstProgram *, const AstType *, unsigned);
static const char *payload_lifetime(BorrowChecker *, const AstProgram *, const AstType *, size_t, size_t);
static void check_future_return(BorrowChecker *, const AstExpression *);

static void reserve_argument_borrows(BorrowChecker *checker, size_t temporary,
                                     const AstExpression *value) {
    AstExpression reservation = {.kind = AST_EXPR_NAME, .resolved_symbol_id = temporary};
    int saved_reserving = checker->reserving_loans;
    checker->reserving_loans = 1;
    checker->aggregate_capture_depth++;
    if (!add_borrow_mode(checker, &reservation, value, checker->current_scope_depth, 1))
        clone_aggregate_borrows(checker, &reservation, value, checker->current_scope_depth);
    checker->aggregate_capture_depth--;
    checker->reserving_loans = saved_reserving;
}

static void release_argument_borrows(BorrowChecker *checker, size_t temporary) {
    BorrowRecord **cursor = &checker->borrows;
    while (*cursor) {
        BorrowRecord *loan = *cursor;
        if (loan->borrower_symbol == temporary) { *cursor = loan->next; free(loan); }
        else cursor = &loan->next;
    }
}

static void release_future_value(BorrowChecker *checker, const AstExpression *value) {
    if (!semantic_expression_is_future(value)) return;
    if (value->lifetime_operation == LIFETIME_TAKE) {
        const AstExpression *ptr = value->arguments;
        const AstExpression *place = ptr && ptr->kind == AST_EXPR_UNARY &&
            ptr->operator_type == TOKEN_AMPERSAND ? ptr->right : NULL;
        if (place) deactivate_borrower(checker, place->resolved_symbol_id, AST_SYMBOL_NONE);
        return;
    }
    if (value->kind == AST_EXPR_NAME)
        deactivate_borrower(checker, value->resolved_symbol_id, AST_SYMBOL_NONE);
    else if (value->kind == AST_EXPR_CALL)
        for (const AstExpression *argument = value->arguments; argument; argument = argument->next)
            release_future_value(checker, argument);
    else if (value->kind == AST_EXPR_AWAIT)
        release_future_value(checker, value->right);
}

/* Argument loans coexist for the duration of a call. Checking each temporary
 * & expression alone only compares it with stored loans, missing sibling args. */
static void check_call_argument_borrows(BorrowChecker *checker,
                                       const AstExpression *arguments) {
    for (const AstExpression *left = arguments; left; left = left->next) {
        if (left->resolved_borrow_kind == AST_BORROW_NONE) continue;
        const AstExpression *left_origin = left;
        if (left->kind == AST_EXPR_UNARY && left->operator_type == TOKEN_AMPERSAND)
            left_origin = left->right;
        BorrowPlace left_place;
        if (!canonical_place(checker, left_origin, &left_place) &&
            !returned_borrow_place(checker, left_origin, &left_place)) continue;
        for (const AstExpression *right = left->next; right; right = right->next) {
            if (right->resolved_borrow_kind == AST_BORROW_NONE ||
                (left->resolved_borrow_kind != AST_BORROW_MUTABLE &&
                 right->resolved_borrow_kind != AST_BORROW_MUTABLE)) continue;
            const AstExpression *right_origin = right;
            if (right->kind == AST_EXPR_UNARY && right->operator_type == TOKEN_AMPERSAND)
                right_origin = right->right;
            BorrowPlace right_place;
            if (!canonical_place(checker, right_origin, &right_place) &&
                !returned_borrow_place(checker, right_origin, &right_place)) continue;
            BorrowRecord loan = {
                .owner_symbol = right_place.owner,
                .field_symbol = right_place.field,
                .region_known = right_place.region_known,
                .region_start = right_place.region_start,
                .region_end = right_place.region_end
            };
            if (places_overlap(&left_place, &loan)) {
                report_borrow_error(checker, right->first_token,
                                    "Conflicting checked borrows in simultaneous arguments");
                break;
            }
        }
    }
}

static void clone_aggregate_borrows(BorrowChecker *checker, const AstExpression *target,
                                    const AstExpression *source, size_t scope_depth);

static void check_call(BorrowChecker *checker,
                       const AstExpression *expression) {
    if (expression->lifetime_operation) {
        const AstExpression *ptr = expression->arguments;
        if (expression->lifetime_operation == LIFETIME_REPLACE)
            check_expression(checker, ptr ? ptr->next : NULL, BORROW_ACCESS_WRITE);
        const AstExpression *place = ptr && ptr->kind == AST_EXPR_UNARY &&
            ptr->operator_type == TOKEN_AMPERSAND ? ptr->right : NULL;
        if (place) check_place_access(checker, place, BORROW_ACCESS_WRITE);
        else {
            BorrowPlace origin;
            if (ptr && canonical_place(checker, ptr, &origin) && origin.through)
                report_borrow_error(checker, expression->first_token,
                                   "Cannot change a value's lifetime while it is borrowed");
            check_expression(checker, ptr, BORROW_ACCESS_READ);
        }
        if (expression->lifetime_operation == LIFETIME_INITIALIZE)
            check_expression(checker, ptr ? ptr->next : NULL, BORROW_ACCESS_WRITE);
        if (expression->lifetime_operation == LIFETIME_REPLACE && place &&
            place->kind == AST_EXPR_NAME && place->resolved_symbol_id < checker->analyzer->model->symbol_count) {
            checker->aggregate_capture_depth++;
            clone_aggregate_borrows(checker, place, ptr->next,
                checker->analyzer->model->symbols[place->resolved_symbol_id].scope_depth);
            checker->aggregate_capture_depth--;
        }
        return;
    }
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
        const AstExpression *receiver = expression->left->left;
        unsigned properties = receiver->has_resolved_ast_type
            ? semantic_declared_type_properties(checker->analyzer,
                receiver->resolved_type_program ? receiver->resolved_type_program : checker->analyzer->program,
                &receiver->resolved_ast_type) : 0;
        int temporary = receiver->kind == AST_EXPR_STRUCT_LITERAL || receiver->kind == AST_EXPR_CALL ||
            receiver->kind == AST_EXPR_CONTROL;
        if (temporary && (properties & SEMANTIC_TYPE_MUST_CONSUME))
            report_borrow_error(checker, receiver->first_token,
                "Temporary receiver owns a live Future; bind it and complete or cancel its captures");
        if (temporary &&
            (semantic_expression_is_future(expression) || expression->resolved_borrow_kind != AST_BORROW_NONE ||
             (expression->resolved_is_slice && !expression->owns_slice_backing) ||
             ((properties & SEMANTIC_TYPE_NEEDS_DROP) && expression->resolved_type == TYPE_STRING) ||
             (expression->has_resolved_ast_type && type_has_view_arguments(checker, &expression->resolved_ast_type))))
            report_borrow_error(checker, receiver->first_token,
                "A borrowing method result requires a named receiver whose lifetime can be retained");
        BorrowAccess receiver_access =
            (expression->left->left->has_resolved_ast_type &&
             expression->left->left->resolved_ast_type.callable_mode == 2) || statements_mutate_receiver(
                checker, function, function->declaration->as.function.body) ||
            function->declaration->as.function.is_async ||
            type_has_mutable_view(checker, function->source_program,
                &function->declaration->as.function.return_type, 0)
                ? BORROW_ACCESS_WRITE : BORROW_ACCESS_READ;
        if (receiver_access == BORROW_ACCESS_WRITE &&
            receiver->resolved_borrow_kind == AST_BORROW_IMMUTABLE)
            report_borrow_error(checker, receiver->first_token,
                               "Cannot obtain mutable access through a shared checked reference");
        check_expression(checker, expression->left->left, receiver_access);
    } else {
        check_expression(checker, expression->left,
            expression->left && expression->left->has_resolved_ast_type &&
            expression->left->resolved_ast_type.callable_mode == 2
                ? BORROW_ACCESS_WRITE : BORROW_ACCESS_READ);
    }
    check_call_argument_borrows(checker, expression->arguments);
    /* A returned checked reference establishes a loan just as an explicit &
       does, even when the callee only takes the address of a receiver field. */
    if (expression->resolved_borrow_kind != AST_BORROW_NONE) {
        BorrowPlace origin;
        if (returned_borrow_place(checker, expression, &origin)) {
            for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next) {
                if (!loan->active || loan == origin.through || !places_overlap(&origin, loan)) continue;
                if (expression->resolved_borrow_kind == AST_BORROW_MUTABLE || loan->kind == AST_BORROW_MUTABLE) {
                    report_borrow_error(checker, expression->first_token, "Conflicting checked borrows in returned reference");
                    break;
                }
            }
        }
    }
    size_t temporary_arguments = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
    for (const AstExpression *argument = expression->arguments;
         argument != NULL; argument = argument->next) {
        if (semantic_expression_is_future(expression) && argument->owns_slice_backing)
            report_borrow_error(checker, argument->first_token,
                "A Future cannot capture temporary slice backing; bind the slice to an owner first");
        BorrowAccess argument_access =
            (argument->resolved_is_slice && argument->owns_slice_backing) ||
            (parameter != NULL &&
             parameter->type.borrow_kind == AST_BORROW_NONE &&
             semantic_expression_is_move_only(checker->analyzer, argument))
                ? BORROW_ACCESS_WRITE
                : BORROW_ACCESS_READ;
        check_expression(checker, argument, argument_access);
        const char *lifetime = parameter ? borrow_lifetime(
            function && function->source_program ? function->source_program
                : checker->analyzer->program, &parameter->type) : NULL;
        if (lifetime && !strcmp(lifetime, "'static")) check_static_borrow(checker, argument);
        reserve_argument_borrows(checker, temporary_arguments, argument);
        if (parameter != NULL) parameter = parameter->next;
    }
    release_argument_borrows(checker, temporary_arguments);
    if (function && function->declaration && !function->declaration->as.function.is_static &&
        expression->left && expression->left->kind == AST_EXPR_MEMBER && expression->left->left &&
        statements_mutate_receiver(checker, function, function->declaration->as.function.body)) {
        const AstParameter *owned = function->declaration->as.function.parameters;
        BorrowPlace receiver;
        if (canonical_place(checker, expression->left->left, &receiver)) {
            AstExpression owner = {.kind = AST_EXPR_NAME, .resolved_symbol_id = receiver.owner};
            AstExpression field = {.kind = AST_EXPR_MEMBER, .left = &owner, .resolved_symbol_id = receiver.field};
            const AstExpression *target = receiver.field == AST_SYMBOL_NONE ? &owner : &field;
            for (const AstExpression *argument = expression->arguments; argument && owned;
                 argument = argument->next, owned = owned->next) {
                if (owned->type.borrow_kind != AST_BORROW_NONE || argument->resolved_borrow_kind != AST_BORROW_NONE ||
                    (!semantic_expression_is_move_only(checker->analyzer, argument) &&
                     !(argument->has_resolved_ast_type && type_has_view_arguments(checker, &argument->resolved_ast_type)))) continue;
                checker->aggregate_capture_depth++;
                clone_aggregate_borrows(checker, target, argument, checker->current_scope_depth);
                checker->aggregate_capture_depth--;
            }
        }
    }
    /* A synchronous consuming call with a plain scalar/void result finishes
       its owned arguments. Aggregate owners may carry loans in heap storage
       even if their visible fields contain only raw allocation pointers. */
    if (checker->deferred_call != expression && function && function->declaration && !function->declaration->as.function.is_async &&
        expression->resolved_borrow_kind==AST_BORROW_NONE && !expression->resolved_is_slice &&
        !expression->resolved_is_array && !expression->resolved_pointer_depth &&
        !expression->resolved_outer_pointer_depth && expression->resolved_type!=TYPE_STRING &&
        expression->resolved_type!=TYPE_UNKNOWN && expression->resolved_named_symbol_id==AST_SYMBOL_NONE &&
        !semantic_expression_is_future(expression) &&
        !(expression->has_resolved_ast_type &&
          (type_has_view_arguments(checker,&expression->resolved_ast_type) ||
           (semantic_declared_type_properties(checker->analyzer,
                expression->resolved_type_program ? expression->resolved_type_program : checker->analyzer->program,
                &expression->resolved_ast_type) & SEMANTIC_TYPE_MUST_CONSUME)))) {
        const AstParameter *owned=function->declaration->as.function.parameters;
        for(const AstExpression *argument=expression->arguments;argument && owned;
            argument=argument->next,owned=owned->next)
            if(owned->type.borrow_kind==AST_BORROW_NONE &&
               semantic_expression_is_move_only(checker->analyzer,argument)) {
                if(argument->kind==AST_EXPR_NAME)
                    deactivate_borrower(checker,argument->resolved_symbol_id,AST_SYMBOL_NONE);
                else release_future_value(checker,argument);
            }
    }
}

static void check_expression(BorrowChecker *checker,
                             const AstExpression *expression,
                             BorrowAccess access) {
    if (expression == NULL) return;
    if (expression->async_operation == ASYNC_BLOCK_ON) {
        check_call(checker,expression);
        if (!semantic_expression_is_future(expression) && expression->resolved_borrow_kind==AST_BORROW_NONE && !expression->resolved_is_slice &&
            !(semantic_symbol_type_properties(checker->analyzer->model, expression->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME))
            release_future_value(checker,expression->arguments);
        return;
    }
    if (expression->kind == AST_EXPR_AWAIT) {
        check_expression(checker, expression->right, BORROW_ACCESS_WRITE);
        if (!semantic_expression_is_future(expression) &&
            expression->resolved_borrow_kind == AST_BORROW_NONE && !expression->resolved_is_slice &&
            !(semantic_symbol_type_properties(checker->analyzer->model, expression->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME))
            release_future_value(checker, expression->right);
        return;
    }
    if (expression->kind == AST_EXPR_CONTROL) {
        check_statement_list(checker, expression->control,
                             checker->current_scope_depth + 1);
        return;
    }
    if (expression->kind == AST_EXPR_PROPAGATE) {
        BorrowAccess operand_access =
            semantic_expression_is_move_only(checker->analyzer,
                                             expression->left)
                ? BORROW_ACCESS_WRITE
                : BORROW_ACCESS_READ;
        check_expression(checker, expression->left, operand_access);
        if (type_has_view_depth(checker, expression->propagation_contract_program,
                                &expression->propagation_residual_type, 0)) {
            /* The Break path is an implicit return. Validate its possible
               origins without transferring loans off the continuing path. */
            AstExpression residual = *expression;
            residual.has_resolved_ast_type = 1;
            residual.resolved_ast_type = expression->propagation_residual_type;
            residual.resolved_borrow_kind = residual.resolved_ast_type.borrow_kind;
            residual.resolved_is_slice = residual.resolved_ast_type.is_slice;
            residual.resolved_named_symbol_id = residual.resolved_ast_type.kind == AST_TYPE_NAMED
                ? resolve_named_symbol_id(checker->analyzer, expression->propagation_contract_program,
                    residual.resolved_ast_type.name_token) : AST_SYMBOL_NONE;
            int saved_reserving = checker->reserving_loans;
            checker->reserving_loans = 1;
            checker->aggregate_capture_depth++;
            check_future_return(checker, &residual);
            checker->aggregate_capture_depth--;
            checker->reserving_loans = saved_reserving;
        }
        return;
    }
    if (expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_AMPERSAND) {
        check_new_borrow(checker, expression);
        check_expression(checker, expression->right, BORROW_ACCESS_READ);
        return;
    }
    if (expression->kind == AST_EXPR_CALL) {
        check_call(checker, expression);
        return;
    }
    if (expression->kind == AST_EXPR_STRUCT_LITERAL || expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        check_call_argument_borrows(checker, expression->arguments);
        size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
        size_t index = 0;
        for (const AstExpression *value = expression->arguments; value != NULL; value = value->next, index++) {
            check_expression(checker, value,
                semantic_expression_is_move_only(checker->analyzer, value)
                    ? BORROW_ACCESS_WRITE : BORROW_ACCESS_READ);
            if (value->initializer_field_symbol_id < checker->analyzer->model->symbol_count) {
                const SemanticSymbol *field = &checker->analyzer->model->symbols[value->initializer_field_symbol_id];
                const char *lifetime = borrow_lifetime(field->source_program, &field->declared_type);
                if (lifetime && !strcmp(lifetime, "'static")) check_static_borrow(checker, value);
            }
            if (expression->kind == AST_EXPR_ENUM_CONSTRUCT && expression->left &&
                expression->left->kind == AST_EXPR_MEMBER && expression->left->left &&
                expression->left->left->allocated_type.lifetime_arguments) {
                const char *lifetime = payload_lifetime(checker, checker->analyzer->program,
                    &expression->left->left->allocated_type, expression->resolved_symbol_id, index);
                if (lifetime && !strcmp(lifetime, "'static")) check_static_borrow(checker, value);
            }
            /* Earlier arguments already own their views while later arguments
               are evaluated, including views inside returned aggregates. */
            reserve_argument_borrows(checker, temporary, value);
        }
        release_argument_borrows(checker, temporary);
        return;
    }
    if (expression->kind == AST_EXPR_NAME ||
        expression->kind == AST_EXPR_MEMBER ||
        expression->kind == AST_EXPR_INDEX ||
        (expression->kind == AST_EXPR_UNARY &&
         expression->operator_type == TOKEN_STAR)) {
        check_place_access(checker, expression, access);
        check_place_evaluation(checker, expression);
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
        if (old->active && !old->package_storage && old->borrower_symbol == borrower_symbol &&
            (borrower_field == AST_SYMBOL_NONE ||
             old->borrower_field_symbol == borrower_field))
            old->active = 0;
}

static BorrowRecord *add_borrow_mode(BorrowChecker *checker,
                                     const AstExpression *borrower,
                                     const AstExpression *value,
                                     size_t scope_depth,
                                     int preserve_existing) {
    if (value == NULL || borrower == NULL)
        return NULL;
    if (value->kind == AST_EXPR_PROPAGATE) {
        BorrowRecord *boundary = checker->borrows;
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return checker->borrows != boundary ? checker->borrows : NULL;
    }
    /* An explicit result lifetime can be shared by multiple input origins.
       Retain all of them; runtime control flow chooses the actual referent. */
    if (value->kind == AST_EXPR_CALL && value->resolved_borrow_kind != AST_BORROW_NONE &&
        value->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *function = &checker->analyzer->model->symbols[value->resolved_symbol_id];
        const AstDeclarationNode *declaration = function->kind == SEMANTIC_SYMBOL_FUNCTION ? function->declaration : NULL;
        const char *lifetime = declaration ? borrow_lifetime(function->source_program,
            &declaration->as.function.return_type) : NULL;
        if (lifetime) {
            BorrowRecord *first = NULL;
            BorrowPlace target;
            if (!expression_place(borrower, &target)) return NULL;
            if (!preserve_existing) deactivate_borrower(checker, target.owner, target.field);
            const AstExpression *argument = value->arguments;
            for (const AstParameter *p = declaration->as.function.parameters; p && argument; p = p->next, argument = argument->next) {
                if (!type_mentions_lifetime(function->source_program, &p->type, lifetime)) continue;
                BorrowRecord *loan = add_lifetime_parameter_borrows(checker, borrower, argument,
                    function->source_program, &p->type, lifetime, value->resolved_borrow_kind, scope_depth);
                if (!first) first = loan;
            }
            if (first) return first;
        }
    }
    if (value->kind == AST_EXPR_CONTROL && value->control != NULL) {
        BorrowPlace borrower_place;
        if (!expression_place(borrower, &borrower_place)) return NULL;
        if (!preserve_existing)
            deactivate_borrower(checker, borrower_place.owner,
                                borrower_place.field);
        const AstStatement *control = value->control;
        BorrowRecord *first = NULL;
        if (control->kind == AST_STMT_BLOCK) {
            first = add_borrow_mode(checker, borrower, control->result,
                                    scope_depth, 1);
        } else if (control->kind == AST_STMT_IF) {
            const AstStatement *branches[2] = {control->body,
                                               control->else_body};
            for (size_t i = 0; i < 2; i++)
                if (branches[i] != NULL && branches[i]->result != NULL &&
                    statement_may_fall_through(branches[i]->body)) {
                    BorrowRecord *record = add_borrow_mode(
                        checker, borrower, branches[i]->result,
                        scope_depth, 1);
                    if (first == NULL) first = record;
                }
        } else if (control->kind == AST_STMT_MATCH) {
            for (const AstMatchArm *arm = control->match_arms; arm;
                 arm = arm->next) {
                if (control->is_type_match && arm != control->selected_type_arm)
                    continue;
                if (arm->body == NULL || arm->body->result == NULL ||
                    !statement_may_fall_through(arm->body->body)) continue;
                BorrowRecord *record = add_borrow_mode(
                    checker, borrower, arm->body->result,
                    scope_depth, 1);
                if (first == NULL) first = record;
            }
        }
        return first;
    }
    /* Copying/reborrowing a reference must preserve every possible referent.
       canonical_place supplies one place for ordinary, unambiguous access. */
    const AstExpression *referent = value;
    int follows_referent = value->resolved_borrow_kind != AST_BORROW_NONE || value->resolved_is_slice;
    if (value->kind == AST_EXPR_UNARY && value->operator_type == TOKEN_AMPERSAND) {
        referent = value->right;
        follows_referent = referent && referent->kind == AST_EXPR_UNARY &&
            referent->operator_type == TOKEN_STAR;
    }
    BorrowPlace storage;
    if (follows_referent && referent && expression_place(referent, &storage)) {
        size_t count = 0;
        for (const BorrowRecord *old = checker->borrows; old; old = old->next)
            if (old->active && !old->captured_by_future && old->borrower_symbol == storage.owner &&
                (storage.field == AST_SYMBOL_NONE || old->borrower_field_symbol == storage.field ||
                 old->borrower_field_symbol == AST_SYMBOL_NONE)) count++;
        if (count > 1) {
            size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
            BorrowRecord *boundary = checker->borrows;
            for (const BorrowRecord *old = boundary; old; old = old->next) {
                if (!old->active || old->captured_by_future || old->borrower_symbol != storage.owner ||
                    (storage.field != AST_SYMBOL_NONE && old->borrower_field_symbol != storage.field &&
                     old->borrower_field_symbol != AST_SYMBOL_NONE)) continue;
                BorrowRecord *copy = malloc(sizeof(*copy));
                if (!copy) { checker->analyzer->allocation_failed = 1; break; }
                *copy = *old;
                copy->borrower_symbol = temporary;
                copy->borrower_field_symbol = AST_SYMBOL_NONE;
                copy->parent = old;
                copy->package_storage = 0;
                copy->payload_known = 0;
                copy->kind = value->resolved_borrow_kind == AST_BORROW_NONE
                    ? AST_BORROW_IMMUTABLE : value->resolved_borrow_kind;
                copy->next = checker->borrows;
                checker->borrows = copy;
            }
            AstExpression source = {.kind = AST_EXPR_NAME, .resolved_symbol_id = temporary,
                .resolved_borrow_kind = AST_BORROW_IMMUTABLE};
            clone_aggregate_borrows(checker, borrower, &source, scope_depth);
            release_argument_borrows(checker, temporary);
            return checker->borrows != boundary ? checker->borrows : NULL;
        }
    }
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
    const AstExpression *completed=value->kind==AST_EXPR_AWAIT ? value->right :
                                   value->async_operation==ASYNC_BLOCK_ON ? value->arguments:NULL;
    if (completed != NULL) {
        if (completed->kind == AST_EXPR_CALL)
            has_returned_place = returned_borrow_place(checker, completed, &returned_place);
        else if (completed->kind == AST_EXPR_NAME) {
            ReturnOrigin origin = {0};
            for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                if (loan->active && loan->captured_by_future &&
                    loan->borrower_symbol == completed->resolved_symbol_id) {
                    BorrowPlace captured = {.owner = loan->owner_symbol, .field = loan->field_symbol};
                    merge_return_origin(&origin, &captured);
                }
            if (origin.set && !origin.ambiguous) {
                returned_place = (BorrowPlace) {.owner = origin.symbol, .field = origin.field};
                has_returned_place = 1;
            }
        }
    }
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

    if (!preserve_existing)
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
    borrow->parent = place.through;
    borrow->origin_lifetime = place.through ? place.through->origin_lifetime : NULL;
    if (borrower_place.owner < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *target = &checker->analyzer->model->symbols[borrower_place.owner];
        borrow->retained_until_drop = target->resolved_borrow_kind == AST_BORROW_NONE &&
            target->resolved_named_symbol_id < checker->analyzer->model->symbol_count &&
            !slice_view && !string_view &&
            (semantic_symbol_type_properties(checker->analyzer->model, target->resolved_named_symbol_id) &
             SEMANTIC_TYPE_NEEDS_DROP) != 0;
        borrow->captured_by_aggregate = !slice_view && !string_view &&
            target->resolved_borrow_kind == AST_BORROW_NONE && !target->resolved_pointer_depth &&
            target->resolved_named_symbol_id < checker->analyzer->model->symbol_count;
    }
    if (borrower_place.owner < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *target = &checker->analyzer->model->symbols[borrower_place.owner];
        const SemanticSymbol *owner = &checker->analyzer->model->symbols[place.owner];
        if (target->kind == SEMANTIC_SYMBOL_LOCAL || target->kind == SEMANTIC_SYMBOL_VARIABLE ||
            target->kind == SEMANTIC_SYMBOL_PARAMETER) borrow->scope_depth = target->scope_depth;
        if (!slice_view && !string_view &&
            (target->kind == SEMANTIC_SYMBOL_LOCAL || target->kind == SEMANTIC_SYMBOL_VARIABLE) &&
            owner->kind == SEMANTIC_SYMBOL_LOCAL && owner->scope_depth > target->scope_depth)
            report_borrow_error(checker, value->first_token,
                               "A stored borrow cannot outlive its source");
        if (!slice_view && !string_view && borrower_place.owner == place.owner &&
            borrower_place.field != AST_SYMBOL_NONE)
            report_borrow_error(checker, value->first_token,
                               "Self-referential borrowed aggregates are not supported");
    }
    borrow->next = checker->borrows;
    checker->borrows = borrow;
    return borrow;
}

static void clone_aggregate_borrows(BorrowChecker *checker, const AstExpression *target,
                                    const AstExpression *source, size_t scope_depth);

static int type_has_borrow_depth(const BorrowChecker *checker, const AstProgram *unit,
                                const AstType *type, size_t depth, int include_slices) {
    if (depth > 64) return 0;
    if (type == NULL) return 0;
    if (type->borrow_kind != AST_BORROW_NONE) return 1;
    if (type->pointer_depth || type->outer_pointer_depth) return 0;
    if (include_slices && type->is_slice) return 1;
    for (const AstTypeArgument *argument = type->arguments; argument; argument = argument->next)
        if (type_has_borrow_depth(checker, unit, &argument->type, depth + 1, include_slices)) return 1;
    if (type_has_borrow_depth(checker, unit, type->element_type, depth + 1, include_slices)) return 1;
    if (type->kind != AST_TYPE_NAMED || primitive_type(unit, type) != TYPE_UNKNOWN) return 0;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, type->name_token);
    if (id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *symbol = &checker->analyzer->model->symbols[id];
    const AstDeclarationNode *decl = symbol->declaration;
    if (!decl || (decl->kind != AST_DECL_STRUCT && decl->kind != AST_DECL_ENUM)) return 0;
    const AstField *fields = decl->kind == AST_DECL_STRUCT ? decl->as.struct_decl.fields : decl->as.enum_decl.fields;
    for (const AstField *f = fields; f; f = f->next)
        if ((!f->type.is_slice || f->type.borrow_kind != AST_BORROW_NONE) &&
            type_has_borrow_depth(checker, symbol->source_program, &f->type, depth + 1, include_slices)) return 1;
    if (decl->kind == AST_DECL_ENUM)
        for (const AstEnumValue *v = decl->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next)
                if (type_has_borrow_depth(checker, symbol->source_program, &p->type, depth + 1, include_slices)) return 1;
    return 0;
}

static int type_has_view_depth(const BorrowChecker *checker, const AstProgram *unit,
                              const AstType *type, size_t depth) {
    return type_has_borrow_depth(checker, unit, type, depth, 1);
}

static int type_has_view_arguments(const BorrowChecker *checker, const AstType *type) {
    return type_has_view_depth(checker, checker->analyzer->program, type, 0);
}

static int type_has_mutable_view(const BorrowChecker *checker, const AstProgram *unit,
                                 const AstType *type, unsigned depth) {
    if (!type || depth > 64) return 0;
    if (type->borrow_kind == AST_BORROW_MUTABLE) return 1;
    if (type->borrow_kind || type->pointer_depth || type->outer_pointer_depth) return 0;
    for (const AstTypeArgument *p = type->arguments; p; p = p->next)
        if (type_has_mutable_view(checker, unit, &p->type, depth + 1)) return 1;
    if (type_has_mutable_view(checker, unit, type->element_type, depth + 1)) return 1;
    if (type->kind != AST_TYPE_NAMED || primitive_type(unit, type) != TYPE_UNKNOWN) return 0;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, type->name_token);
    if (id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *symbol = &checker->analyzer->model->symbols[id];
    const AstDeclarationNode *d = symbol->declaration;
    if (!d || (d->kind != AST_DECL_STRUCT && d->kind != AST_DECL_ENUM)) return 0;
    for (const AstField *f = d->kind == AST_DECL_STRUCT ? d->as.struct_decl.fields : d->as.enum_decl.fields; f; f = f->next)
        if (type_has_mutable_view(checker, symbol->source_program, &f->type, depth + 1)) return 1;
    if (d->kind == AST_DECL_ENUM)
        for (const AstEnumValue *v = d->as.enum_decl.values; v; v = v->next)
            for (const AstTypeArgument *p = v->payload_types; p; p = p->next)
                if (type_has_mutable_view(checker, symbol->source_program, &p->type, depth + 1)) return 1;
    return 0;
}

static int call_returns_view_aggregate(const BorrowChecker *checker, const AstExpression *call) {
    if (call == NULL || call->kind != AST_EXPR_CALL ||
        call->resolved_symbol_id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[call->resolved_symbol_id];
    return function->kind == SEMANTIC_SYMBOL_FUNCTION && function->declaration != NULL &&
           !function->declaration->as.function.return_type.is_slice &&
           function->declaration->as.function.return_type.borrow_kind == AST_BORROW_NONE &&
           type_has_view_depth(checker, function->source_program,
                               &function->declaration->as.function.return_type, 0);
}

static void capture_future_borrows(BorrowChecker *checker,
                                   const AstExpression *borrower,
                                   const AstExpression *value,
                                   size_t scope_depth) {
    if (!semantic_expression_is_future(value)) return;
    if (value->lifetime_operation == LIFETIME_TAKE || value->lifetime_operation == LIFETIME_REPLACE) {
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return;
    }
    if (value->kind == AST_EXPR_NAME) {
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return;
    }
    if (value->kind == AST_EXPR_AWAIT || value->async_operation==ASYNC_BLOCK_ON) {
        capture_future_borrows(checker, borrower, value->kind==AST_EXPR_AWAIT ? value->right:value->arguments, scope_depth);
        return;
    }
    if (value->kind == AST_EXPR_CONTROL && value->control != NULL) {
        const AstStatement *control = value->control;
        if (control->kind == AST_STMT_BLOCK)
            capture_future_borrows(checker, borrower, control->result, scope_depth);
        else if (control->kind == AST_STMT_IF) {
            if (control->body != NULL)
                capture_future_borrows(checker, borrower, control->body->result, scope_depth);
            if (control->else_body != NULL)
                capture_future_borrows(checker, borrower, control->else_body->result, scope_depth);
        } else if (control->kind == AST_STMT_MATCH)
            for (const AstMatchArm *arm = control->match_arms; arm; arm = arm->next)
                if (arm->body != NULL)
                    capture_future_borrows(checker, borrower, arm->body->result, scope_depth);
        return;
    }
    if (value->kind != AST_EXPR_CALL) return;
    const AstParameter *parameter = NULL;
    if (value->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *function = &checker->analyzer->model->symbols[value->resolved_symbol_id];
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION && function->declaration != NULL)
            parameter = function->declaration->as.function.parameters;
        if (function->kind == SEMANTIC_SYMBOL_FUNCTION && function->declaration != NULL &&
            function->owner_symbol_id != AST_SYMBOL_NONE && !function->declaration->as.function.is_static &&
            value->left != NULL && value->left->kind == AST_EXPR_MEMBER && value->left->left != NULL) {
            AstExpression receiver = {.kind = AST_EXPR_UNARY, .operator_type = TOKEN_AMPERSAND,
                .right = value->left->left, .first_token = value->first_token,
                .resolved_borrow_kind = AST_BORROW_MUTABLE, .mutable_borrow = 1};
            check_new_borrow(checker, &receiver);
            BorrowRecord *loan = add_borrow_mode(checker, borrower, &receiver, scope_depth, 1);
            if (loan != NULL) {
                loan->captured_by_future = 1;
                const SemanticSymbol *owner = &checker->analyzer->model->symbols[loan->owner_symbol];
                if (loan->borrower_symbol < checker->analyzer->model->symbol_count) {
                    loan->scope_depth = checker->analyzer->model->symbols[loan->borrower_symbol].scope_depth;
                    if (owner->kind == SEMANTIC_SYMBOL_LOCAL && owner->scope_depth > loan->scope_depth)
                        report_borrow_error(checker, value->first_token,
                            "A Future cannot outlive the origin of a captured borrow");
                }
            }
        }
    }
    for (const AstExpression *argument = value->arguments; argument; argument = argument->next) {
        if (!semantic_expression_is_future(argument) && argument->resolved_borrow_kind == AST_BORROW_NONE &&
            argument->resolved_named_symbol_id != AST_SYMBOL_NONE) {
            BorrowRecord *boundary = checker->borrows;
            clone_aggregate_borrows(checker, borrower, argument, scope_depth);
            for (BorrowRecord *copy = checker->borrows; copy != boundary; copy = copy->next) {
                copy->captured_by_future = 1;
                copy->payload_known = 0;
                if (copy->borrower_symbol < checker->analyzer->model->symbol_count) {
                    copy->scope_depth = checker->analyzer->model->symbols[copy->borrower_symbol].scope_depth;
                    const SemanticSymbol *origin = &checker->analyzer->model->symbols[copy->owner_symbol];
                    if (origin->kind == SEMANTIC_SYMBOL_LOCAL && origin->scope_depth > copy->scope_depth)
                        report_borrow_error(checker, argument->first_token,
                            "A Future cannot outlive the origin of a captured borrow");
                }
            }
        }
        if (semantic_expression_is_future(argument) && argument->kind != AST_EXPR_NAME)
            capture_future_borrows(checker, borrower, argument, scope_depth);
        if (argument->resolved_borrow_kind != AST_BORROW_NONE ||
            (parameter != NULL && parameter->type.is_slice)) {
            if (argument->kind == AST_EXPR_UNARY && argument->operator_type == TOKEN_AMPERSAND)
                check_new_borrow(checker, argument);
            BorrowRecord *loan = add_borrow_mode(checker, borrower, argument, scope_depth, 1);
            if (loan != NULL) {
                loan->captured_by_future = 1;
                if (loan->borrower_symbol < checker->analyzer->model->symbol_count)
                    loan->scope_depth = checker->analyzer->model->symbols[loan->borrower_symbol].scope_depth;
                const SemanticSymbol *owner = &checker->analyzer->model->symbols[loan->owner_symbol];
                if (loan->borrower_symbol < checker->analyzer->model->symbol_count &&
                    owner->kind == SEMANTIC_SYMBOL_LOCAL &&
                    owner->scope_depth > checker->analyzer->model->symbols[loan->borrower_symbol].scope_depth)
                    report_borrow_error(checker, value->first_token,
                                       "A Future cannot outlive the origin of a captured borrow");
            }
        }
        if (semantic_expression_is_future(argument) && argument->kind == AST_EXPR_NAME) {
            for (BorrowRecord *old = checker->borrows; old; old = old->next) {
                if (!old->active || !old->captured_by_future ||
                    old->borrower_symbol != argument->resolved_symbol_id) continue;
                BorrowRecord *copy = calloc(1, sizeof(*copy));
                if (copy == NULL) {
                    checker->analyzer->allocation_failed = 1;
                    return;
                }
                *copy = *old;
                copy->payload_known = 0;
                copy->borrower_symbol = borrower->resolved_symbol_id;
                copy->scope_depth = scope_depth;
                copy->next = checker->borrows;
                checker->borrows = copy;
                if (!checker->reserving_loans) old->active = 0;
            }
        }
        if (parameter != NULL) parameter = parameter->next;
    }
}

static const char *aggregate_lifetime(BorrowChecker *checker, const AstProgram *unit,
                                     const AstType *type, const char *declared) {
    if (!declared || !type || type->kind != AST_TYPE_NAMED) return NULL;
    if (!strcmp(declared, "'static")) return declared;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, type->name_token);
    if (id >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[id];
    if (!owner->declaration) return NULL;
    const AstLifetimeParameter *argument = type->lifetime_arguments;
    for (const AstLifetimeParameter *parameter = owner->declaration->lifetime_parameters;
         parameter && argument; parameter = parameter->next, argument = argument->next)
        if (!strcmp(declared, ast_program_lexeme(owner->source_program, parameter->name_token)))
            return ast_program_lexeme(unit, argument->name_token);
    return NULL;
}

static const char *payload_lifetime(BorrowChecker *checker, const AstProgram *unit,
                                   const AstType *type, size_t variant, size_t index) {
    if (!type || type->kind != AST_TYPE_NAMED) return NULL;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, type->name_token);
    if (id >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[id];
    if (!owner->declaration || owner->declaration->kind != AST_DECL_ENUM) return NULL;
    for (const AstEnumValue *value = owner->declaration->as.enum_decl.values; value; value = value->next) {
        if (value->resolved_symbol_id != variant) continue;
        const AstTypeArgument *payload = value->payload_types;
        for (size_t i = 0; payload && i < index; i++) payload = payload->next;
        if (!payload) return NULL;
        const char *declared = borrow_lifetime(owner->source_program, &payload->type);
        if (!declared && payload->type.kind == AST_TYPE_NAMED &&
            !payload->type.pointer_depth && !payload->type.outer_pointer_depth &&
            payload->type.lifetime_arguments && !payload->type.lifetime_arguments->next)
            declared = ast_program_lexeme(owner->source_program, payload->type.lifetime_arguments->name_token);
        return aggregate_lifetime(checker, unit, type, declared);
    }
    return NULL;
}

static const char *result_field_lifetime(BorrowChecker *checker, const AstProgram *unit,
                                        const AstType *result, size_t field_id) {
    if (!result || result->kind != AST_TYPE_NAMED ||
        field_id >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *field = &checker->analyzer->model->symbols[field_id];
    size_t owner = resolve_named_symbol_id(checker->analyzer, unit, result->name_token);
    if (field->kind != SEMANTIC_SYMBOL_FIELD || field->owner_symbol_id != owner ||
        owner >= checker->analyzer->model->symbol_count) return NULL;
    const AstDeclarationNode *aggregate = checker->analyzer->model->symbols[owner].declaration;
    const char *declared = borrow_lifetime(field->source_program, &field->declared_type);
    /* A nested aggregate with one lifetime retains that relationship even
       while its individual inner field paths are represented conservatively. */
    if (!declared && field->declared_type.kind == AST_TYPE_NAMED &&
        !field->declared_type.pointer_depth && !field->declared_type.outer_pointer_depth &&
        field->declared_type.lifetime_arguments && !field->declared_type.lifetime_arguments->next)
        declared = ast_program_lexeme(field->source_program,
            field->declared_type.lifetime_arguments->name_token);
    if (!aggregate || !declared) return NULL;
    return aggregate_lifetime(checker, unit, result, declared);
}

/* A reference returned from an aggregate parameter follows the reference fields
   with the declared lifetime, rather than borrowing the aggregate's storage. */
static BorrowRecord *add_parameter_field_borrows(BorrowChecker *checker,
    const AstExpression *target, const AstExpression *argument, const AstProgram *unit,
    const AstType *parameter, const char *lifetime, AstBorrowKind kind, size_t scope_depth) {
    if (parameter->kind != AST_TYPE_NAMED || parameter->pointer_depth ||
        parameter->outer_pointer_depth) return NULL;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, parameter->name_token);
    if (id >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[id];
    const AstDeclarationNode *aggregate = owner->declaration;
    if (!aggregate || aggregate->kind != AST_DECL_STRUCT) return NULL;
    for (const AstField *field = aggregate->as.struct_decl.fields; field; field = field->next)
        if (field->type.borrow_kind == AST_BORROW_NONE &&
            (type_has_view_depth(checker, owner->source_program, &field->type, 0) ||
             (semantic_declared_type_properties(checker->analyzer, owner->source_program,
                &field->type) & SEMANTIC_TYPE_MOVE_ONLY))) return NULL;
    const AstExpression *base = argument;
    if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND)
        base = base->right;
    BorrowPlace storage;
    if (!expression_place(base, &storage)) return NULL;
    BorrowRecord *first = NULL;
    for (const AstField *field = aggregate->as.struct_decl.fields; field; field = field->next) {
        if (field->type.borrow_kind == AST_BORROW_NONE) continue;
        const char *field_lifetime = result_field_lifetime(checker, unit, parameter,
            field->resolved_symbol_id);
        if (!field_lifetime || strcmp(field_lifetime, lifetime)) continue;
        AstExpression input = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)base,
            .resolved_symbol_id = field->resolved_symbol_id, .resolved_borrow_kind = kind,
            .first_token = argument->first_token};
        BorrowRecord *loan = add_borrow_mode(checker, target, &input, scope_depth, 1);
        if (!first) first = loan;
    }
    return first;
}

static BorrowRecord *add_lifetime_parameter_borrows(BorrowChecker *checker,
    const AstExpression *target, const AstExpression *argument, const AstProgram *unit,
    const AstType *parameter, const char *lifetime, AstBorrowKind kind, size_t scope_depth) {
    BorrowRecord *fields = add_parameter_field_borrows(checker, target, argument,
        unit, parameter, lifetime, kind, scope_depth);
    const char *direct = borrow_lifetime(unit, parameter);
    int storage_lifetime = direct && !strcmp(direct, lifetime);
    if (fields && !storage_lifetime) return fields;
    if (!fields && parameter->kind == AST_TYPE_NAMED) {
        const AstExpression *base = argument;
        if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND)
            base = base->right;
        AstExpression preserved = *base;
        preserved.next = NULL;
        preserved.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
        BorrowRecord *boundary = checker->borrows;
        checker->aggregate_capture_depth++;
        clone_aggregate_borrows(checker, target, &preserved, scope_depth);
        checker->aggregate_capture_depth--;
        if (checker->borrows != boundary) {
            fields = checker->borrows;
            if (!storage_lifetime) return fields;
        }
    }
    AstExpression input = *argument;
    input.next = NULL;
    input.resolved_borrow_kind = kind;
    BorrowRecord *storage = add_borrow_mode(checker, target, &input, scope_depth, 1);
    return fields ? fields : storage;
}

static void check_future_return(BorrowChecker *checker, const AstExpression *value) {
    if (value == NULL || (!semantic_expression_is_future(value) &&
        !semantic_expression_is_move_only(checker->analyzer, value) &&
        !value->resolved_is_slice && value->resolved_type != TYPE_STRING &&
        value->resolved_borrow_kind == AST_BORROW_NONE &&
        value->kind != AST_EXPR_ENUM_CONSTRUCT &&
        !call_returns_view_aggregate(checker,value) &&
        !(value->has_resolved_ast_type && type_has_view_arguments(checker, &value->resolved_ast_type)))) return;
    AstExpression escaped = {.kind = AST_EXPR_NAME,
        .resolved_symbol_id = checker->analyzer->current_function_symbol_id};
    BorrowRecord *boundary = checker->borrows;
    if (semantic_expression_is_future(value))
        capture_future_borrows(checker, &escaped, value, checker->current_scope_depth);
    else if (add_borrow_mode(checker, &escaped, value, checker->current_scope_depth, 1) == NULL)
        clone_aggregate_borrows(checker, &escaped, value, checker->current_scope_depth);
    const AstDeclarationNode *function = checker->analyzer->current_function;
    const char *required = function ? borrow_lifetime(checker->analyzer->program,
        &function->as.function.return_type) : NULL;
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next) {
        const char *field_required = function ? result_field_lifetime(checker,
            checker->analyzer->program, &function->as.function.return_type,
            loan->borrower_field_symbol) : NULL;
        const char *loan_required = required ? required : field_required;
        if (!loan_required && function && function->as.function.return_type.kind == AST_TYPE_NAMED &&
            loan->payload_known &&
            loan->payload_enum == resolve_named_symbol_id(checker->analyzer, checker->analyzer->program,
                function->as.function.return_type.name_token))
            loan_required = payload_lifetime(checker, checker->analyzer->program,
                &function->as.function.return_type, loan->payload_variant, loan->payload_index);
        if (loan->active &&
            (loan->borrower_symbol == escaped.resolved_symbol_id ||
             (value->kind == AST_EXPR_NAME && loan->borrower_symbol == value->resolved_symbol_id)) &&
            checker->analyzer->model->symbols[loan->owner_symbol].kind == SEMANTIC_SYMBOL_LOCAL)
            report_borrow_error(checker, value->first_token,
                               semantic_expression_is_future(value)
                                   ? "A returned Future cannot capture a borrow of a local value"
                                   : "A returned value cannot capture a borrow of a local value");
        if (loan_required && loan->active && loan->borrower_symbol == escaped.resolved_symbol_id) {
            const SemanticSymbol *owner = &checker->analyzer->model->symbols[loan->owner_symbol];
            const char *source_field_lifetime = loan->origin_lifetime ? loan->origin_lifetime :
                result_field_lifetime(checker, owner->source_program, &owner->declared_type, loan->field_symbol);
            if ((owner->kind == SEMANTIC_SYMBOL_PARAMETER || owner->kind == SEMANTIC_SYMBOL_FIELD) &&
                (source_field_lifetime ?
                    strcmp(source_field_lifetime, loan_required) && strcmp(source_field_lifetime, "'static") :
                    !type_mentions_lifetime(owner->source_program, &owner->declared_type, loan_required)))
                report_borrow_error(checker, value->first_token,
                    required ? "Returned borrow does not satisfy the declared result lifetime" :
                    loan->payload_known ?
                    "Returned aggregate does not satisfy its declared lifetimes: payload violates the declared result lifetime" :
                    "Returned aggregate does not satisfy its declared lifetimes: field violates the declared result lifetime");
            if (!strcmp(loan_required, "'static") &&
                owner->kind != SEMANTIC_SYMBOL_VARIABLE && owner->kind != SEMANTIC_SYMBOL_CONSTANT &&
                !(source_field_lifetime && !strcmp(source_field_lifetime, "'static")) &&
                !type_mentions_lifetime(owner->source_program, &owner->declared_type, loan_required))
                report_borrow_error(checker, value->first_token,
                                   "A static borrow requires permanently live storage");
        }
        if (!loan_required && function && loan->active && loan->borrower_symbol == escaped.resolved_symbol_id) {
            const SemanticSymbol *owner = &checker->analyzer->model->symbols[loan->owner_symbol];
            const AstType *result = &function->as.function.return_type;
            if (owner->kind == SEMANTIC_SYMBOL_PARAMETER || owner->kind == SEMANTIC_SYMBOL_FIELD) {
                int annotated = 0, compatible = 0;
                for (const AstLifetimeParameter *p = result->lifetime_arguments; p; p = p->next) {
                    const char *name = ast_program_lexeme(checker->analyzer->program, p->name_token);
                    annotated = 1;
                    compatible |= type_mentions_lifetime(owner->source_program, &owner->declared_type, name);
                }
                if (annotated && !compatible)
                    report_borrow_error(checker, value->first_token,
                                       "Returned aggregate does not satisfy its declared lifetimes");
            }
        }
    }
    while (checker->borrows != boundary) {
        BorrowRecord *next = checker->borrows->next;
        free(checker->borrows);
        checker->borrows = next;
    }
}

static BorrowRecord *add_borrow(BorrowChecker *checker,
                                const AstExpression *borrower,
                                const AstExpression *value,
                                size_t scope_depth) {
    return add_borrow_mode(checker, borrower, value, scope_depth, 0);
}

/* Explicit result lifetimes describe direct reference fields independently.
   More complex owner/aggregate payloads retain the conservative union below. */
static int clone_result_field_borrows(BorrowChecker *checker, const AstExpression *target,
                                      const AstExpression *call, size_t scope_depth) {
    if (call->resolved_symbol_id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION || !function->declaration ||
        function->declaration->as.function.is_async) return 0;
    const AstType *result = &function->declaration->as.function.return_type;
    if (result->kind != AST_TYPE_NAMED || result->borrow_kind != AST_BORROW_NONE ||
        result->pointer_depth || result->outer_pointer_depth) return 0;
    size_t id = resolve_named_symbol_id(checker->analyzer, function->source_program, result->name_token);
    if (id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[id];
    const AstDeclarationNode *aggregate = owner->declaration;
    if (!aggregate || aggregate->kind != AST_DECL_STRUCT) return 0;
    int references = 0;
    for (const AstField *field = aggregate->as.struct_decl.fields; field; field = field->next) {
        if (field->type.borrow_kind == AST_BORROW_NONE) {
            if (type_has_view_depth(checker, owner->source_program, &field->type, 0) ||
                (semantic_declared_type_properties(checker->analyzer, owner->source_program,
                    &field->type) & SEMANTIC_TYPE_MOVE_ONLY)) return 0;
            continue;
        }
        const char *lifetime = result_field_lifetime(checker, function->source_program,
            result, field->resolved_symbol_id);
        if (!lifetime) return 0;
        int found = 0;
        const AstExpression *argument = call->arguments;
        for (const AstParameter *parameter = function->declaration->as.function.parameters;
             parameter && argument; parameter = parameter->next, argument = argument->next)
            if (type_mentions_lifetime(function->source_program, &parameter->type, lifetime)) found = 1;
        if (!found) return 0;
        references++;
    }
    if (!references) return 0;
    BorrowPlace place;
    if (!expression_place(target, &place)) return 0;
    if (!checker->aggregate_capture_depth) deactivate_borrower(checker, place.owner, place.field);
    for (const AstField *field = aggregate->as.struct_decl.fields; field; field = field->next) {
        if (field->type.borrow_kind == AST_BORROW_NONE) continue;
        const char *lifetime = result_field_lifetime(checker, function->source_program,
            result, field->resolved_symbol_id);
        AstExpression destination = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)target,
            .resolved_symbol_id = field->resolved_symbol_id};
        const AstExpression *argument = call->arguments;
        for (const AstParameter *parameter = function->declaration->as.function.parameters;
             parameter && argument; parameter = parameter->next, argument = argument->next) {
            if (!type_mentions_lifetime(function->source_program, &parameter->type, lifetime)) continue;
            add_lifetime_parameter_borrows(checker, &destination, argument, function->source_program,
                &parameter->type, lifetime, field->type.borrow_kind, scope_depth);
        }
    }
    return 1;
}

static void clone_aggregate_borrows(BorrowChecker *checker,
                                    const AstExpression *target,
                                    const AstExpression *source,
                                    size_t scope_depth) {
    if (source && (source->lifetime_operation == LIFETIME_TAKE ||
                   source->lifetime_operation == LIFETIME_REPLACE)) {
        const AstExpression *ptr = source->arguments;
        const AstExpression *place = ptr && ptr->kind == AST_EXPR_UNARY &&
            ptr->operator_type == TOKEN_AMPERSAND ? ptr->right : NULL;
        if (source->lifetime_operation == LIFETIME_REPLACE && place) {
            /* Keep a conservative union of old/new loans on both values. An
               inline consuming use must not erase the replacement's loans. */
            AstExpression preserved = *place;
            preserved.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
            clone_aggregate_borrows(checker, target, &preserved, scope_depth);
        } else clone_aggregate_borrows(checker, target, place, scope_depth);
        return;
    }
    if (source && source->kind == AST_EXPR_PROPAGATE) {
        BorrowRecord *boundary = checker->borrows;
        clone_aggregate_borrows(checker, target, source->left, scope_depth);
        /* A by-value aggregate parameter has no caller loans in this function.
           Its view payloads nevertheless refer to caller storage, just as in
           a value-pattern binding. Do not invent this origin for local owners. */
        BorrowPlace parameter_place;
        if (checker->borrows == boundary && expression_place(source->left, &parameter_place) &&
            parameter_place.owner < checker->analyzer->model->symbol_count) {
            const SemanticSymbol *parameter = &checker->analyzer->model->symbols[parameter_place.owner];
            if (parameter->kind == SEMANTIC_SYMBOL_PARAMETER &&
                type_has_view_depth(checker, parameter->source_program, &parameter->declared_type, 0) &&
                (source->resolved_borrow_kind != AST_BORROW_NONE || source->resolved_is_slice ||
                 (source->has_resolved_ast_type && type_has_view_arguments(checker, &source->resolved_ast_type)))) {
                AstExpression referents = *source->left;
                referents.resolved_borrow_kind = source->resolved_borrow_kind == AST_BORROW_NONE
                    ? AST_BORROW_IMMUTABLE : source->resolved_borrow_kind;
                add_borrow_mode(checker, target, &referents, scope_depth, 1);
            }
        }
        /* branch is a normal DMM function and may reorder its input payloads.
           An extracted output cannot keep the operand enum's payload tags. */
        for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next) {
            loan->payload_known = 0;
            if (source->resolved_borrow_kind != AST_BORROW_NONE)
                loan->kind = source->resolved_borrow_kind;
        }
        return;
    }
    if (source != NULL && source->kind == AST_EXPR_STRUCT_LITERAL) {
        BorrowPlace place;
        if (!expression_place(target, &place)) return;
        if (!checker->aggregate_capture_depth)
            deactivate_borrower(checker, place.owner, place.field);
        checker->aggregate_capture_depth++;
        for (const AstExpression *value = source->arguments; value != NULL; value = value->next) {
            AstExpression field = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)target,
                .resolved_symbol_id = value->initializer_field_symbol_id};
            AstExpression view = *value;
            if (value->initializer_field_symbol_id < checker->analyzer->model->symbol_count &&
                checker->analyzer->model->symbols[value->initializer_field_symbol_id].declared_type.is_slice &&
                value->resolved_is_array) {
                view.resolved_is_slice = 1;
                view.resolved_is_array = 0;
            }
            if (semantic_expression_is_future(value))
                capture_future_borrows(checker, target, value, scope_depth);
            else if (add_borrow_mode(checker, &field, &view, scope_depth, 1) == NULL) {
                BorrowRecord *boundary = checker->borrows;
                clone_aggregate_borrows(checker, target, value, scope_depth);
                /* Until nested paths are represented, these origins belong to
                   an unknown inner field of the containing aggregate. */
                for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next) {
                    loan->borrower_field_symbol = AST_SYMBOL_NONE;
                    loan->payload_known = 0;
                }
            }
        }
        checker->aggregate_capture_depth--;
        return;
    }
    if (source != NULL && (source->kind == AST_EXPR_AWAIT || source->async_operation == ASYNC_BLOCK_ON) &&
        (semantic_symbol_type_properties(checker->analyzer->model,
                                         source->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME)) {
        capture_future_borrows(checker, target,
            source->kind == AST_EXPR_AWAIT ? source->right : source->arguments, scope_depth);
        return;
    }
    if (source != NULL && source->kind == AST_EXPR_ENUM_CONSTRUCT) {
        checker->aggregate_capture_depth++;
        size_t index = 0;
        for (const AstExpression *payload = source->arguments; payload; payload = payload->next) {
            BorrowRecord *boundary = checker->borrows;
            if (semantic_expression_is_future(payload))
                capture_future_borrows(checker, target, payload, scope_depth);
            else if (add_borrow_mode(checker, target, payload, scope_depth, 1) == NULL)
                clone_aggregate_borrows(checker, target, payload, scope_depth);
            for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next) {
                loan->payload_known = 1;
                loan->payload_enum = source->resolved_named_symbol_id;
                loan->payload_variant = source->resolved_symbol_id;
                loan->payload_index = index;
            }
            index++;
        }
        checker->aggregate_capture_depth--;
        return;
    }
    if (source != NULL && source->kind == AST_EXPR_CALL &&
        (semantic_expression_is_move_only(checker->analyzer, source) ||
         call_returns_view_aggregate(checker,source) ||
         (semantic_symbol_type_properties(checker->analyzer->model,
                                          source->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME))) {
        BorrowRecord *boundary = checker->borrows;
        if (clone_result_field_borrows(checker, target, source, scope_depth)) {
            for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next)
                loan->payload_known = 0;
            return;
        }
        if (source->left && source->left->kind == AST_EXPR_MEMBER && source->left->left) {
            AstExpression receiver = *source->left->left;
            receiver.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
            if (source->resolved_symbol_id < checker->analyzer->model->symbol_count) {
                const SemanticSymbol *function = &checker->analyzer->model->symbols[source->resolved_symbol_id];
                if (function->kind == SEMANTIC_SYMBOL_FUNCTION && function->declaration &&
                    type_has_mutable_view(checker, function->source_program,
                        &function->declaration->as.function.return_type, 0))
                    receiver.resolved_borrow_kind = AST_BORROW_MUTABLE;
            }
            clone_aggregate_borrows(checker, target, &receiver, scope_depth);
            if (call_returns_view_aggregate(checker,source))
                add_borrow_mode(checker, target, &receiver, scope_depth, 1);
        }
        /* A synchronous wrapper may transfer a pending Future inside its result.
           Preserve argument loans until the returned owner is consumed. */
        for (const AstExpression *argument = source->arguments; argument; argument = argument->next) {
            if (semantic_expression_is_future(argument)) {
                capture_future_borrows(checker, target, argument, scope_depth);
            } else if (argument->resolved_borrow_kind != AST_BORROW_NONE || argument->resolved_is_slice) {
                BorrowRecord *loan = add_borrow_mode(checker, target, argument, scope_depth, 1);
                if (loan != NULL) {
                    loan->captured_by_future = semantic_expression_is_future(source) ||
                        (semantic_symbol_type_properties(checker->analyzer->model,
                                                         source->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME) != 0;
                    if (loan->borrower_symbol < checker->analyzer->model->symbol_count) {
                        loan->scope_depth = checker->analyzer->model->symbols[loan->borrower_symbol].scope_depth;
                        const SemanticSymbol *origin = &checker->analyzer->model->symbols[loan->owner_symbol];
                        if (origin->kind == SEMANTIC_SYMBOL_LOCAL && origin->scope_depth > loan->scope_depth)
                            report_borrow_error(checker, argument->first_token,
                                "A Future cannot outlive the origin of a captured borrow");
                    }
                }
            } else {
                clone_aggregate_borrows(checker, target, argument, scope_depth);
            }
        }
        /* A callee may change variants or reorder payloads. Its conservative
           argument union cannot retain the caller's constructor positions. */
        for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next)
            loan->payload_known = 0;
        return;
    }
    if (source == NULL ||
        (!semantic_expression_is_future(source) && source->resolved_borrow_kind == AST_BORROW_NONE &&
         source->resolved_named_symbol_id == AST_SYMBOL_NONE && !source->resolved_is_array && !source->resolved_is_slice &&
         source->resolved_type != TYPE_STRING))
        return;
    BorrowPlace target_place, source_place;
    if (!expression_place(target, &target_place) ||
        !expression_place(source, &source_place))
        return;
    if (!checker->aggregate_capture_depth)
        deactivate_borrower(checker, target_place.owner, target_place.field);
    for (BorrowRecord *old = checker->borrows; old != NULL; old = old->next) {
        if (!old->active || old->borrower_symbol != source_place.owner ||
            (source_place.field != AST_SYMBOL_NONE && old->borrower_field_symbol != AST_SYMBOL_NONE &&
             old->borrower_field_symbol != source_place.field))
            continue;
        BorrowRecord *copy = calloc(1, sizeof(*copy));
        if (copy == NULL) {
            checker->analyzer->allocation_failed = 1;
            return;
        }
        *copy = *old;
        copy->package_storage = 0;
        copy->borrower_symbol = target_place.owner;
        if (source_place.field != AST_SYMBOL_NONE && old->borrower_field_symbol == AST_SYMBOL_NONE)
            copy->payload_known = 0;
        if (target_place.field != AST_SYMBOL_NONE) copy->borrower_field_symbol = target_place.field;
        else if (source_place.field != AST_SYMBOL_NONE) copy->borrower_field_symbol = AST_SYMBOL_NONE;
        copy->scope_depth = scope_depth;
        if (target_place.owner < checker->analyzer->model->symbol_count) {
            const SemanticSymbol *target_symbol = &checker->analyzer->model->symbols[target_place.owner];
            copy->captured_by_aggregate = target_symbol->resolved_borrow_kind == AST_BORROW_NONE &&
                !target_symbol->resolved_pointer_depth &&
                target_symbol->resolved_named_symbol_id < checker->analyzer->model->symbol_count && !copy->slice_view;
            if (target_symbol->kind == SEMANTIC_SYMBOL_LOCAL || target_symbol->kind == SEMANTIC_SYMBOL_VARIABLE ||
                target_symbol->kind == SEMANTIC_SYMBOL_PARAMETER) copy->scope_depth = target_symbol->scope_depth;
            if (!copy->slice_view && target_symbol->resolved_borrow_kind == AST_BORROW_NONE &&
                target_symbol->resolved_named_symbol_id < checker->analyzer->model->symbol_count &&
                (semantic_symbol_type_properties(checker->analyzer->model, target_symbol->resolved_named_symbol_id) &
                 SEMANTIC_TYPE_NEEDS_DROP)) copy->retained_until_drop = 1;
        }
        if (!copy->slice_view && target_place.owner < checker->analyzer->model->symbol_count) {
            const SemanticSymbol *target_symbol = &checker->analyzer->model->symbols[target_place.owner];
            const SemanticSymbol *origin = &checker->analyzer->model->symbols[copy->owner_symbol];
            if ((target_symbol->kind == SEMANTIC_SYMBOL_LOCAL || target_symbol->kind == SEMANTIC_SYMBOL_VARIABLE) &&
                origin->kind == SEMANTIC_SYMBOL_LOCAL && origin->scope_depth > target_symbol->scope_depth)
                report_borrow_error(checker, source->first_token, "A stored borrow cannot outlive its source");
        }
        if (copy->captured_by_future && target_place.owner < checker->analyzer->model->symbol_count) {
            copy->scope_depth = checker->analyzer->model->symbols[target_place.owner].scope_depth;
            const SemanticSymbol *origin = &checker->analyzer->model->symbols[copy->owner_symbol];
            if (origin->kind == SEMANTIC_SYMBOL_LOCAL && origin->scope_depth > copy->scope_depth)
                report_borrow_error(checker, source->first_token,
                                   "A Future cannot outlive the origin of a captured borrow");
        }
        copy->next = checker->borrows;
        checker->borrows = copy;
    }
    if (!checker->reserving_loans && semantic_expression_is_move_only(checker->analyzer, source))
        deactivate_borrower(checker, source_place.owner, source_place.field);
}

static void clone_pattern_borrows(BorrowChecker *checker, const AstExpression *target,
                                  const AstExpression *value, size_t variant,
                                  size_t index, size_t scope_depth) {
    if (value->kind == AST_EXPR_ENUM_CONSTRUCT && value->resolved_symbol_id == variant) {
        const AstExpression *payload = value->arguments;
        for (size_t i = 0; payload && i < index; i++) payload = payload->next;
        if (semantic_expression_is_future(payload))
            capture_future_borrows(checker, target, payload, scope_depth);
        else if (!add_borrow_mode(checker, target, payload, scope_depth, 1))
            clone_aggregate_borrows(checker, target, payload, scope_depth);
        return;
    }
    AstExpression source = *value;
    source.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
    if (value->kind != AST_EXPR_NAME) {
        clone_aggregate_borrows(checker, target, &source, scope_depth);
        return;
    }
    size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
    BorrowRecord *boundary = checker->borrows;
    for (const BorrowRecord *old = boundary; old; old = old->next) {
        if (!old->active || old->borrower_symbol != value->resolved_symbol_id) continue;
        if (old->payload_known && old->payload_enum == value->resolved_named_symbol_id &&
            (old->payload_variant != variant || old->payload_index != index)) continue;
        BorrowRecord *copy = malloc(sizeof(*copy));
        if (!copy) { checker->analyzer->allocation_failed = 1; break; }
        *copy = *old;
        copy->borrower_symbol = temporary;
        copy->package_storage = 0;
        copy->payload_known = 0;
        copy->next = checker->borrows;
        checker->borrows = copy;
    }
    source.resolved_symbol_id = temporary;
    clone_aggregate_borrows(checker, target, &source, scope_depth);
    release_argument_borrows(checker, temporary);
}

static void check_statement_list(BorrowChecker *checker,
                                 const AstStatement *statement,
                                 size_t scope_depth) {
    size_t saved_scope_depth = checker->current_scope_depth;
    checker->current_scope_depth = scope_depth;
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
            AstExpression view;
            const AstExpression *initial = statement->value;
            if (initial && initial->resolved_is_array &&
                statement->resolved_symbol_id < checker->analyzer->model->symbol_count &&
                checker->analyzer->model->symbols[statement->resolved_symbol_id].resolved_is_slice) {
                view = *initial;
                view.resolved_is_slice = 1;
                view.resolved_is_array = 0;
                initial = &view;
            }
            if (!semantic_expression_is_future(statement->value) &&
                add_borrow(checker, &borrower, initial,
                           scope_depth) == NULL)
                clone_aggregate_borrows(checker, &borrower,
                                        statement->value, scope_depth);
            capture_future_borrows(checker, &borrower, statement->value, scope_depth);
            if (statement->value != NULL && (statement->value->kind == AST_EXPR_AWAIT || statement->value->async_operation==ASYNC_BLOCK_ON) &&
                !semantic_expression_is_future(statement->value))
                release_future_value(checker, statement->value->kind==AST_EXPR_AWAIT ? statement->value->right:statement->value->arguments);
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
            AstExpression view;
            const AstExpression *assigned = statement->value;
            if (assigned && assigned->resolved_is_array && statement->expression &&
                statement->expression->resolved_is_slice) {
                view = *assigned;
                view.resolved_is_slice = 1;
                view.resolved_is_array = 0;
                assigned = &view;
            }
            if (!semantic_expression_is_future(statement->value) &&
                add_borrow(checker, statement->expression,
                           assigned, scope_depth) == NULL)
                clone_aggregate_borrows(checker, statement->expression,
                                        statement->value, scope_depth);
            capture_future_borrows(checker, statement->expression, statement->value, scope_depth);
            if (statement->value != NULL && (statement->value->kind == AST_EXPR_AWAIT || statement->value->async_operation==ASYNC_BLOCK_ON) &&
                !semantic_expression_is_future(statement->value))
                release_future_value(checker, statement->value->kind==AST_EXPR_AWAIT ? statement->value->right:statement->value->arguments);
            continue;
        }
        if (statement->kind == AST_STMT_DEFER) {
            const AstExpression *saved_deferred = checker->deferred_call;
            checker->deferred_call = statement->expression;
            check_expression(checker, statement->expression,
                             BORROW_ACCESS_READ);
            checker->deferred_call = saved_deferred;
            continue;
        }

        check_expression(checker, statement->expression, BORROW_ACCESS_READ);
        if (statement->kind == AST_STMT_RETURN)
            check_future_return(checker, statement->value);
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
        if (statement->kind == AST_STMT_IF) {
            size_t count = 0;
            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                count++;
            FutureLoanState *entry = count == 0 ? NULL : calloc(count, sizeof(*entry));
            if (count != 0 && entry == NULL) {
                checker->analyzer->allocation_failed = 1;
                break;
            }
            size_t index = 0;
            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                entry[index++] = (FutureLoanState) {.loan = loan, .active = loan->active};
            check_statement_list(checker, statement->body, scope_depth + 1);
            deactivate_scope(checker, scope_depth + 1);
            int then_reachable = statement_may_fall_through(statement->body);
            size_t then_count = 0;
            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                then_count++;
            FutureLoanState *then_state = then_count == 0 ? NULL : calloc(then_count, sizeof(*then_state));
            if (then_count != 0 && then_state == NULL) {
                free(entry);
                checker->analyzer->allocation_failed = 1;
                break;
            }
            index = 0;
            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                {
                    then_state[index++] = (FutureLoanState) {.loan = loan, .active = loan->active};
                    loan->active = 0;
                }
            for (index = 0; index < count; index++) entry[index].loan->active = entry[index].active;
            check_statement_list(checker, statement->else_body, scope_depth + 1);
            deactivate_scope(checker, scope_depth + 1);
            if (!statement_may_fall_through(statement->else_body))
                for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                    loan->active = 0;
            if (then_reachable)
                for (index = 0; index < then_count; index++)
                    then_state[index].loan->active |= then_state[index].active;
            free(entry);
            free(then_state);
            continue;
        }
        if (statement->kind == AST_STMT_MATCH) {
            FutureLoanBranch entry = future_loan_snapshot(checker);
            FutureLoanBranch *branches = NULL;
            for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                future_loan_restore(checker, &entry);
                for (const AstParameter *binding = arm->bindings; binding; binding = binding->next) {
                    const AstType *type = binding->resolved_symbol_id < checker->analyzer->model->symbol_count
                        ? &checker->analyzer->model->symbols[binding->resolved_symbol_id].declared_type : &binding->type;
                    AstExpression borrower = {.kind = AST_EXPR_NAME,
                        .resolved_symbol_id = binding->resolved_symbol_id};
                    if (statement->value && statement->value->resolved_borrow_kind != AST_BORROW_NONE) {
                        add_borrow_mode(checker, &borrower, statement->value, scope_depth + 1, 1);
                        continue;
                    }
                    if (statement->is_consuming_match || !type_has_view_arguments(checker, type)) continue;
                    clone_aggregate_borrows(checker, &borrower, statement->value, scope_depth + 1);
                    if (statement->value && statement->value->resolved_symbol_id < checker->analyzer->model->symbol_count &&
                        checker->analyzer->model->symbols[statement->value->resolved_symbol_id].kind == SEMANTIC_SYMBOL_PARAMETER) {
                        AstExpression source = *statement->value;
                        source.resolved_borrow_kind = type_has_mutable_view(checker, checker->analyzer->program, type, 0)
                            ? AST_BORROW_MUTABLE : AST_BORROW_IMMUTABLE;
                        add_borrow_mode(checker, &borrower, &source, scope_depth + 1, 1);
                    }
                }
                if (statement->is_consuming_match) {
                    size_t index = 0;
                    for (const AstParameter *binding = arm->bindings; binding; binding = binding->next, index++) {
                        if (!type_has_view_arguments(checker, &binding->type) &&
                            !(semantic_declared_type_properties(checker->analyzer,
                                checker->analyzer->program, &binding->type) & SEMANTIC_TYPE_MOVE_ONLY)) continue;
                        AstExpression borrower = {.kind = AST_EXPR_NAME,
                            .resolved_symbol_id = binding->resolved_symbol_id};
                        AstExpression source = *statement->value;
                        source.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
                        clone_pattern_borrows(checker, &borrower, statement->value,
                            arm->resolved_variant_symbol, index, scope_depth + 1);
                        if (statement->value->resolved_symbol_id < checker->analyzer->model->symbol_count &&
                            checker->analyzer->model->symbols[statement->value->resolved_symbol_id].kind == SEMANTIC_SYMBOL_PARAMETER &&
                            type_has_view_arguments(checker, &binding->type))
                            add_borrow_mode(checker, &borrower, &source, scope_depth + 1, 1);
                    }
                    if (statement->value && statement->value->kind == AST_EXPR_NAME)
                        deactivate_borrower(checker, statement->value->resolved_symbol_id, AST_SYMBOL_NONE);
                }
                /* Value patterns on an aggregate parameter copy/transfer the
                   payload referents, rather than borrowing the payload slots. */
                if (statement->value && statement->value->kind == AST_EXPR_NAME &&
                    statement->value->resolved_borrow_kind == AST_BORROW_NONE &&
                    statement->value->resolved_symbol_id < checker->analyzer->model->symbol_count) {
                    const SemanticSymbol *owner = &checker->analyzer->model->symbols[statement->value->resolved_symbol_id];
                    if (owner->kind == SEMANTIC_SYMBOL_PARAMETER) {
                        size_t index = 0;
                        for (const AstParameter *binding = arm->bindings; binding; binding = binding->next, index++) {
                            const char *lifetime = payload_lifetime(checker, owner->source_program,
                                &owner->declared_type, arm->resolved_variant_symbol, index);
                            if (!lifetime) continue;
                            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                                if (loan->active && loan->borrower_symbol == binding->resolved_symbol_id &&
                                    loan->owner_symbol == owner->id) loan->origin_lifetime = lifetime;
                        }
                    }
                }
                check_statement_list(checker, arm->body, scope_depth + 1);
                deactivate_scope(checker, scope_depth + 1);
                if (!statement_may_fall_through(arm->body)) continue;
                FutureLoanBranch *branch = calloc(1, sizeof(*branch));
                if (branch == NULL) { checker->analyzer->allocation_failed = 1; break; }
                *branch = future_loan_snapshot(checker);
                branch->next = branches;
                branches = branch;
            }
            future_loan_restore(checker, statement->match_exhaustive ? NULL : &entry);
            while (branches != NULL) {
                for (size_t i = 0; i < branches->count; i++)
                    branches->state[i].loan->active |= branches->state[i].active;
                FutureLoanBranch *next = branches->next;
                free(branches->state); free(branches); branches = next;
            }
            free(entry.state);
            continue;
        }
        if (statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR) {
            FutureLoanBranch entry = future_loan_snapshot(checker);
            check_statement_list(checker, statement->body, scope_depth + 1);
            deactivate_scope(checker, scope_depth + 1);
            /* The loop may execute zero times. A return in its body does not
               consume the loan on the fall-through path. */
            for (size_t i = 0; i < entry.count; i++) entry.state[i].loan->active |= entry.state[i].active;
            free(entry.state);
            continue;
        }
        if (statement->kind == AST_STMT_BLOCK ||
            statement->kind == AST_STMT_IF ||
            statement->kind == AST_STMT_WHILE ||
            statement->kind == AST_STMT_FOR) {
            check_statement_list(checker, statement->body, scope_depth + 1);
            check_expression(checker, statement->result, BORROW_ACCESS_READ);
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
    checker->current_scope_depth = saved_scope_depth;
}

/* Package loans are fixed for the lifetime of the program. Seed the same
   origins into every body, including bodies that never read the holder. */
static int package_borrow_holder(BorrowChecker *checker, const SemanticSymbol *symbol) {
    if (symbol->kind != SEMANTIC_SYMBOL_VARIABLE || !symbol->declaration) return 0;
    const AstType *type = &symbol->declared_type;
    const AstProgram *unit = symbol->source_program;
    const AstExpression *value = symbol->declaration->as.constant.value;
    if (type->kind == AST_TYPE_INFERRED && value && value->has_resolved_ast_type) {
        type = &value->resolved_ast_type;
        unit = value->resolved_type_program ? value->resolved_type_program : unit;
    }
    return type->borrow_kind != AST_BORROW_NONE ||
        (type->kind == AST_TYPE_NAMED && !type->pointer_depth && !type->outer_pointer_depth &&
         type_has_borrow_depth(checker, unit, type, 0, 0));
}

static void seed_package_symbol(BorrowChecker *, size_t, unsigned char *, int);

static int empty_package_variant(BorrowChecker *checker, const AstExpression *value, unsigned depth) {
    if (!value || depth > 64) return 0;
    if (value->kind == AST_EXPR_NAME && value->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *source = &checker->analyzer->model->symbols[value->resolved_symbol_id];
        return source->kind == SEMANTIC_SYMBOL_VARIABLE && source->scope_depth == 0 && source->declaration &&
            empty_package_variant(checker, source->declaration->as.constant.value, depth + 1);
    }
    const AstDeclarationNode *enumeration = value->resolved_named_symbol_id < checker->analyzer->model->symbol_count
        ? checker->analyzer->model->symbols[value->resolved_named_symbol_id].declaration : NULL;
    return value->kind == AST_EXPR_ENUM_CONSTRUCT && !value->arguments && enumeration &&
        enumeration->kind == AST_DECL_ENUM && !enumeration->as.enum_decl.fields;
}

static void seed_package_dependencies(BorrowChecker *checker, const AstExpression *value,
                                      unsigned char *states, int validate) {
    for (; value; value = value->next) {
        if (value->kind == AST_EXPR_NAME && value->resolved_symbol_id < checker->analyzer->model->symbol_count)
            seed_package_symbol(checker, value->resolved_symbol_id, states, validate);
        seed_package_dependencies(checker, value->left, states, validate);
        seed_package_dependencies(checker, value->right, states, validate);
        seed_package_dependencies(checker, value->arguments, states, validate);
    }
}

static void seed_package_symbol(BorrowChecker *checker, size_t id,
                                unsigned char *states, int validate) {
    if (states[id]) return;
    states[id] = 1;
    Analyzer *analyzer = checker->analyzer;
    const SemanticSymbol *symbol = &analyzer->model->symbols[id];
    if (!package_borrow_holder(checker, symbol)) { states[id] = 2; return; }
    const AstExpression *value = symbol->declaration->as.constant.value;
    seed_package_dependencies(checker, value, states, validate);
    AstProgram *saved = analyzer->program;
    analyzer->program = (AstProgram *)symbol->source_program;
    AstExpression target = {.kind = AST_EXPR_NAME, .resolved_symbol_id = id,
        .resolved_named_symbol_id = symbol->resolved_named_symbol_id};
    if (validate) check_expression(checker, value, BORROW_ACCESS_READ);
    if (!add_borrow(checker, &target, value, 0))
        clone_aggregate_borrows(checker, &target, value, 0);
    int found = 0;
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next) {
        if (!loan->active || loan->borrower_symbol != id) continue;
        found = 1;
        loan->package_storage = loan->retained_until_drop = 1;
        const SemanticSymbol *origin = loan->owner_symbol < analyzer->model->symbol_count
            ? &analyzer->model->symbols[loan->owner_symbol] : NULL;
        if (validate && (!origin || origin->scope_depth != 0 ||
            (origin->kind != SEMANTIC_SYMBOL_VARIABLE && origin->kind != SEMANTIC_SYMBOL_CONSTANT)))
            report_borrow_error(checker, symbol->name_token,
                "Package borrows require provably permanent package storage");
        if (validate && origin && origin->kind == SEMANTIC_SYMBOL_VARIABLE && origin->declaration &&
            !origin->declaration->as.constant.value &&
            semantic_requires_explicit_init(analyzer->model, origin->source_program, &origin->declared_type))
            report_borrow_error(checker, symbol->name_token,
                "Package borrows require initialized permanent storage");
        const AstType *borrowed_type = loan->borrower_field_symbol < analyzer->model->symbol_count
            ? &analyzer->model->symbols[loan->borrower_field_symbol].declared_type : &symbol->declared_type;
        if (validate && origin && origin->resolved_pointer_depth &&
            borrowed_type->pointer_depth < origin->resolved_pointer_depth)
            report_borrow_error(checker, symbol->name_token,
                "Raw pointer storage does not prove a permanently live referent");
        if (validate && loan->kind == AST_BORROW_MUTABLE)
            report_borrow_error(checker, symbol->name_token,
                "Mutable package borrows require interprocedural exclusive-access proof");
    }
    if (validate && !found && !empty_package_variant(checker, value, 0))
        report_borrow_error(checker, symbol->name_token,
            "Package borrow initializer has no provably permanent origin");
    analyzer->program = saved;
    states[id] = 2;
}

static void seed_package_borrows(BorrowChecker *checker, int validate) {
    size_t count = checker->analyzer->model->symbol_count;
    unsigned char *states = calloc(count, 1);
    if (!states) { checker->analyzer->allocation_failed = 1; return; }
    for (size_t id = 0; id < count; id++) seed_package_symbol(checker, id, states, validate);
    free(states);
}

static void free_borrow_checker(BorrowChecker *checker) {
    BorrowRecord *borrow = checker->borrows;
    while (borrow) {
        BorrowRecord *next = borrow->next;
        free(borrow);
        borrow = next;
    }
    free(checker->last_use);
}

void validate_package_borrows(Analyzer *analyzer) {
    BorrowChecker checker = {.analyzer = analyzer};
    checker.last_use = calloc(analyzer->model->symbol_count, sizeof(*checker.last_use));
    if (!checker.last_use) { analyzer->allocation_failed = 1; return; }
    seed_package_borrows(&checker, 1);
    free_borrow_checker(&checker);
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
        if (origin.ambiguous && !borrow_lifetime(analyzer->program, &function->as.function.return_type))
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
    seed_package_borrows(&checker, 0);
    check_statement_list(&checker, function->as.function.body,
                         analyzer->scope_depth);

    free_borrow_checker(&checker);
}
