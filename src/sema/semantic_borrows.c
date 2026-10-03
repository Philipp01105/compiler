#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BorrowPath {
    size_t id;
    size_t parent;
    size_t field;
    int payload;
    size_t enum_symbol;
    size_t variant;
    size_t index;
    struct BorrowPath *next;
} BorrowPath;

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
    size_t iteration_scope_depth;
    int active;
    int captured_by_future;
    int future_frame_only;
    int captured_by_aggregate;
    int retained_until_drop;
    int package_storage;
    int payload_known;
    size_t payload_enum;
    size_t payload_variant;
    size_t payload_index;
    const char *origin_lifetime;
    const struct BorrowRecord *parent;
    int summary_foreign;
    int summary_effect;
    int summary_exit_active;
    int reference_slot;
    int heap_storage;
    int heap_ring;
    int heap_bounded;
    int heap_tagged;
    const AstStatement *heap_relocation_head_update;
    const AstStatement *heap_relocation_mod_update;
    size_t heap_extent_owner;
    size_t heap_extent_field;
    size_t heap_tag_field;
    int64_t heap_tag_value;
    size_t heap_head_owner;
    size_t heap_head_field;
    size_t heap_mod_owner;
    size_t heap_mod_field;
    size_t heap_anchor_owner;
    size_t heap_anchor_field;
    int64_t heap_min;
    int64_t heap_max;
    int64_t heap_exit_min;
    int64_t heap_exit_max;
    struct BorrowRecord *next;
} BorrowRecord;

typedef struct BorrowCallSummary {
    const AstExpression *call;
    size_t result_symbol;
    size_t epoch;
    int heap_effect;
    int unmodeled_storage;
    struct BorrowCallSummary *next;
} BorrowCallSummary;

typedef struct BorrowHeapGuard {
    size_t owner, root, tag;
    size_t index_owner, index_field;
    int fixed, valid;
    int64_t index, value;
    struct BorrowHeapGuard *next;
} BorrowHeapGuard;

typedef struct BorrowStorage {
    size_t symbol;
    size_t owner;
    size_t field;
    int heap_index;
    int heap_ring;
    int heap_bounded;
    size_t extent_owner;
    size_t extent_field;
    BorrowHeapGuard *guard;
    size_t head_owner;
    size_t head_field;
    size_t mod_owner;
    size_t mod_field;
    size_t index_owner;
    size_t index_field;
    int64_t index_offset;
    struct BorrowStorage *next;
} BorrowStorage;

typedef struct BorrowWrite {
    size_t owner;
    size_t field;
    struct BorrowWrite *next;
} BorrowWrite;

typedef struct BorrowContinuation {
    const AstStatement *statements;
    struct BorrowContinuation *next;
} BorrowContinuation;

typedef struct BorrowIndexConstant {
    size_t symbol;
    int64_t value;
    size_t anchor_owner;
    size_t anchor_field;
    int relative;
    int valid;
    struct BorrowIndexConstant *next;
} BorrowIndexConstant;

typedef struct BorrowIndexBound {
    size_t owner;
    size_t field;
    int64_t minimum;
    int exact;
    int valid;
    struct BorrowIndexBound *next;
} BorrowIndexBound;

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
    BorrowPath *paths;
    size_t path_count;
    unsigned summary_depth;
    size_t summary_functions[16];
    int summarizing;
    size_t summary_result;
    BorrowCallSummary *summaries;
    size_t evaluation_epoch;
    BorrowStorage *storage;
    BorrowWrite *writes;
    Analyzer *caller_analyzer;
    size_t call_token;
    int heap_effect;
    int unmodeled_storage;
    const AstStatement *current_statement;
    BorrowContinuation *continuations;
    BorrowIndexConstant *index_constants;
    BorrowIndexBound *index_bounds;
    BorrowHeapGuard *heap_guards;
} BorrowChecker;

typedef struct {
    BorrowRecord *loan;
    int active;
    int64_t heap_min;
    int64_t heap_max;
    int heap_tagged;
    int heap_bounded;
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
        branch.state[index++] = (FutureLoanState) {.loan = loan, .active = loan->active,
            .heap_min=loan->heap_min,.heap_max=loan->heap_max,
            .heap_tagged=loan->heap_tagged,.heap_bounded=loan->heap_bounded};
    return branch;
}

static void future_loan_restore(BorrowChecker *checker, const FutureLoanBranch *branch) {
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        loan->active = 0;
    if (branch != NULL)
        for (size_t i = 0; i < branch->count; i++) {
            branch->state[i].loan->active = branch->state[i].active;
            branch->state[i].loan->heap_min=branch->state[i].heap_min;
            branch->state[i].loan->heap_max=branch->state[i].heap_max;
        }
}

static void join_loan_state(const FutureLoanState *state) {
    BorrowRecord *loan=state->loan;
    if(!state->active) return;
    if(loan->active) {
        if(state->heap_min<loan->heap_min) loan->heap_min=state->heap_min;
        if(state->heap_max>loan->heap_max) loan->heap_max=state->heap_max;
    } else { loan->heap_min=state->heap_min; loan->heap_max=state->heap_max; }
    loan->active=1;
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

static const BorrowPath *borrow_path_node(const BorrowChecker *checker, size_t id) {
    for (const BorrowPath *path = checker->paths; path; path = path->next)
        if (path->id == id) return path;
    return NULL;
}

static size_t borrow_path_append(BorrowChecker *checker, size_t parent, size_t field) {
    if (field == AST_SYMBOL_NONE) return parent;
    if (parent == AST_SYMBOL_NONE) return field;
    const BorrowPath *suffix = borrow_path_node(checker, field);
    if (suffix && suffix->parent != AST_SYMBOL_NONE) return borrow_path_append(checker,
        borrow_path_append(checker, parent, suffix->parent), suffix->field);
    for (const BorrowPath *path = checker->paths; path; path = path->next)
        if (path->parent == parent && path->field == field) return path->id;
    BorrowPath *path = malloc(sizeof(*path));
    if (!path) { checker->analyzer->allocation_failed = 1; return AST_SYMBOL_NONE; }
    *path = (BorrowPath){.id = AST_SYMBOL_NONE / 2 - checker->path_count++,
        .parent = parent, .field = field, .next = checker->paths};
    checker->paths = path;
    return path->id;
}

static size_t borrow_payload_path(BorrowChecker *checker, size_t enum_symbol,
                                  size_t variant, size_t index) {
    for (const BorrowPath *path = checker->paths; path; path = path->next)
        if (path->payload && path->enum_symbol == enum_symbol &&
            path->variant == variant && path->index == index) return path->id;
    BorrowPath *path = calloc(1, sizeof(*path));
    if (!path) { checker->analyzer->allocation_failed = 1; return AST_SYMBOL_NONE; }
    path->id = AST_SYMBOL_NONE / 2 - checker->path_count++;
    path->parent = AST_SYMBOL_NONE;
    path->field = path->id;
    path->payload = 1;
    path->enum_symbol = enum_symbol;
    path->variant = variant;
    path->index = index;
    path->next = checker->paths;
    checker->paths = path;
    return path->id;
}

static int borrow_path_prefix(const BorrowChecker *checker, size_t prefix, size_t path) {
    if (prefix == AST_SYMBOL_NONE) return 1;
    for (;;) {
        if (prefix == path) return 1;
        const BorrowPath *node = borrow_path_node(checker, path);
        if (!node) return 0;
        path = node->parent;
    }
}

static int borrow_paths_overlap(const BorrowChecker *checker, size_t left, size_t right) {
    return borrow_path_prefix(checker, left, right) || borrow_path_prefix(checker, right, left);
}

static size_t borrow_path_relative(BorrowChecker *checker, size_t path, size_t prefix) {
    if (path == prefix || path == AST_SYMBOL_NONE) return AST_SYMBOL_NONE;
    const BorrowPath *node = borrow_path_node(checker, path);
    if (!node) return prefix == AST_SYMBOL_NONE ? path : AST_SYMBOL_NONE;
    return borrow_path_append(checker, borrow_path_relative(checker, node->parent, prefix), node->field);
}

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
    BorrowRecord *nearest = NULL;
    for (BorrowRecord *borrow = checker->borrows; borrow; borrow = borrow->next)
        if (borrow->active && borrow->borrower_symbol == symbol && !borrow->captured_by_future &&
            borrow_path_prefix(checker, borrow->borrower_field_symbol, field) &&
            (!nearest || borrow_path_prefix(checker, nearest->borrower_field_symbol,
                                           borrow->borrower_field_symbol))) nearest = borrow;
    return nearest;
}

static int expression_place(BorrowChecker *checker, const AstExpression *expression,
                            BorrowPlace *place) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_NAME) {
        place->owner = expression->resolved_symbol_id;
        place->field = AST_SYMBOL_NONE;
        const AstDeclarationNode *function = checker->analyzer->current_function;
        if (function && function->as.function.uses_receiver_reference && function->as.function.receiver_parameter &&
            expression->resolved_symbol_id < checker->analyzer->model->symbol_count &&
            checker->analyzer->model->symbols[expression->resolved_symbol_id].kind == SEMANTIC_SYMBOL_FIELD &&
            checker->analyzer->model->symbols[expression->resolved_symbol_id].owner_symbol_id ==
                checker->analyzer->model->symbols[function->resolved_symbol_id].owner_symbol_id) {
            place->owner = function->as.function.receiver_parameter->resolved_symbol_id;
            place->field = borrow_path_append(checker, AST_SYMBOL_NONE, expression->resolved_symbol_id);
        }
        place->region_known = 0;
        place->region_start = 0;
        place->region_end = 0;
        place->through = NULL;
        return place->owner != AST_SYMBOL_NONE;
    }
    if (expression->kind == AST_EXPR_MEMBER) {
        if (!expression_place(checker, expression->left, place)) return 0;
        place->field = borrow_path_append(checker, place->field, expression->resolved_symbol_id);
        return 1;
    }
    if (expression->kind == AST_EXPR_INDEX ||
        expression->kind == AST_EXPR_SUBSLICE)
        return expression_place(checker, expression->left, place);
    if (expression->kind == AST_EXPR_SLICE_DATA)
        return expression_place(checker, expression->left, place);
    if (expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_STAR)
        return expression_place(checker, expression->right, place);
    return 0;
}

static int canonical_place(BorrowChecker *checker,
                           const AstExpression *expression,
                           BorrowPlace *place) {
    if (!expression_place(checker, expression, place)) return 0;
    const AstExpression *leaf = expression;
    while (leaf && ((leaf->kind == AST_EXPR_UNARY && leaf->operator_type == TOKEN_STAR) ||
                   leaf->kind == AST_EXPR_INDEX || leaf->kind == AST_EXPR_SUBSLICE))
        leaf = leaf->kind == AST_EXPR_UNARY ? leaf->right : leaf->left;
    size_t terminal_field = leaf && leaf->kind == AST_EXPR_MEMBER
        ? leaf->resolved_symbol_id : place->field;
    const BorrowPath *terminal_path = borrow_path_node(checker, terminal_field);
    if (terminal_path) terminal_field = terminal_path->field;
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
        size_t source_field = source->borrower_field_symbol;
        const BorrowPath *source_path = borrow_path_node(checker, source_field);
        if (source_path) source_field = source_path->field;
        int reference_field = source_field < checker->analyzer->model->symbol_count &&
            checker->analyzer->model->symbols[source_field].declared_type.borrow_kind != AST_BORROW_NONE;
        if (source->captured_by_aggregate &&
            (place->field == AST_SYMBOL_NONE || (!reference_field && (terminal_field == AST_SYMBOL_NONE ||
             terminal_field >= checker->analyzer->model->symbol_count ||
             checker->analyzer->model->symbols[terminal_field].declared_type.borrow_kind == AST_BORROW_NONE)))) break;
        if (!place->through) place->through = source;
        if (place->owner == source->owner_symbol && place->field == source->field_symbol) break;
        place->owner = source->owner_symbol;
        size_t suffix = source->captured_by_aggregate && source->borrower_field_symbol == AST_SYMBOL_NONE
            ? AST_SYMBOL_NONE : borrow_path_relative(checker, place->field, source->borrower_field_symbol);
        place->field = borrow_path_append(checker, source->field_symbol, suffix);
        if (source->region_known) {
            place->region_known = 1;
            place->region_start = source->region_start;
            place->region_end = source->region_end;
        } else if (source->slice_view) {
            place->region_known = 0;
        }
        if (source->reference_slot && expression->has_resolved_ast_type &&
            expression->resolved_ast_type.reference_type) break;
    }
    return place->owner < checker->analyzer->model->symbol_count;
}

static int reference_storage_place(BorrowChecker *checker, const AstExpression *expression,
                                    BorrowPlace *place) {
    if (expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR)
        return canonical_place(checker, expression->right, place);
    if (!expression_place(checker, expression, place)) return 0;
    if (expression->kind == AST_EXPR_MEMBER && expression->left &&
        expression->left->resolved_borrow_kind != AST_BORROW_NONE) {
        BorrowPlace owner;
        if (!canonical_place(checker, expression->left, &owner)) return 0;
        place->owner = owner.owner;
        place->field = borrow_path_append(checker, owner.field, expression->resolved_symbol_id);
        place->through = owner.through;
    }
    return 1;
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
    if (expression_place(checker, expression, place)) return 1;
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
                AstExpression field = {.kind = AST_EXPR_MEMBER, .left = call->left->left,
                    .resolved_symbol_id = borrow_path_append(checker, origin.symbol, origin.field)};
                return canonical_place(checker, &field, place);
            }
            if (symbol != NULL && symbol->kind == SEMANTIC_SYMBOL_PARAMETER) {
                const AstParameter *receiver = function->declaration->as.function.receiver_parameter;
                if (receiver != NULL && receiver->resolved_symbol_id == origin.symbol &&
                    call->left != NULL && call->left->kind == AST_EXPR_MEMBER &&
                    call->left->left != NULL) {
                    if (origin.field != AST_SYMBOL_NONE) {
                        AstExpression field = {.kind = AST_EXPR_MEMBER,
                            .left = call->left->left, .resolved_symbol_id = origin.field};
                        return canonical_place(checker, &field, place);
                    }
                    return canonical_place(checker, call->left->left, place);
                }
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

static int places_overlap(const BorrowChecker *checker, const BorrowPlace *left,
                          const BorrowRecord *right) {
    if (left->owner != right->owner_symbol) return 0;
    if (!borrow_paths_overlap(checker, left->field, right->field_symbol)) return 0;
    if (left->region_known && right->region_known)
        return left->region_start < right->region_end &&
               right->region_start < left->region_end;
    return 1;
}

enum { BINDING_CONTINUES, BINDING_DEAD, BINDING_READ };

static int binding_flow(const AstStatement *, size_t, unsigned);

static int expression_reads_binding(const AstExpression *expression, size_t symbol, unsigned depth) {
    if (depth > 128) return 1;
    for (; expression; expression=expression->next) {
        if ((expression->kind==AST_EXPR_NAME && expression->resolved_symbol_id==symbol) ||
            expression_reads_binding(expression->left,symbol,depth+1) ||
            expression_reads_binding(expression->right,symbol,depth+1) ||
            expression_reads_binding(expression->arguments,symbol,depth+1) ||
            (expression->control && binding_flow(expression->control,symbol,depth+1)==BINDING_READ)) return 1;
    }
    return 0;
}

/* Follow the remaining control-flow path until this binding is read, replaced,
   or the path terminates. A later use of a replacement does not keep an old
   slice/reference loan alive. Loop continuations conservatively revisit the
   loop, while return paths do not inherit uses from subsequent iterations. */
static int binding_flow(const AstStatement *statement, size_t symbol, unsigned depth) {
    if (depth > 128) return BINDING_READ;
    for (; statement; statement=statement->next) {
        int replaces = statement->kind==AST_STMT_ASSIGNMENT &&
            statement->assignment_operator==TOKEN_EQUAL && statement->expression &&
            statement->expression->kind==AST_EXPR_NAME && statement->expression->resolved_symbol_id==symbol;
        if ((!replaces && expression_reads_binding(statement->expression,symbol,depth+1)) ||
            expression_reads_binding(statement->value,symbol,depth+1) ||
            expression_reads_binding(statement->condition,symbol,depth+1) ||
            expression_reads_binding(statement->update,symbol,depth+1) ||
            expression_reads_binding(statement->result,symbol,depth+1)) return BINDING_READ;
        if (replaces || statement->kind==AST_STMT_RETURN ||
            (statement->kind==AST_STMT_VARIABLE && statement->resolved_symbol_id==symbol)) return BINDING_DEAD;
        if (statement->kind==AST_STMT_IF) {
            int left=binding_flow(statement->body,symbol,depth+1);
            int right=binding_flow(statement->else_body,symbol,depth+1);
            if (left==BINDING_READ || right==BINDING_READ) return BINDING_READ;
            if (left==BINDING_DEAD && right==BINDING_DEAD) return BINDING_DEAD;
        } else if (statement->kind==AST_STMT_MATCH) {
            int all_dead=statement->match_exhaustive;
            for (const AstMatchArm *arm=statement->match_arms;arm;arm=arm->next) {
                int flow=binding_flow(arm->body,symbol,depth+1);
                if (flow==BINDING_READ) return BINDING_READ;
                if (flow!=BINDING_DEAD) all_dead=0;
            }
            if (all_dead) return BINDING_DEAD;
        } else if (statement->kind==AST_STMT_WHILE || statement->kind==AST_STMT_FOR) {
            if (binding_flow(statement->initializer,symbol,depth+1)==BINDING_READ ||
                binding_flow(statement->body,symbol,depth+1)==BINDING_READ) return BINDING_READ;
        } else if (statement->kind==AST_STMT_BLOCK) {
            int flow=binding_flow(statement->body,symbol,depth+1);
            if (flow!=BINDING_CONTINUES) return flow;
        } else if (statement->kind==AST_STMT_DEFER) {
            if (binding_flow(statement->body,symbol,depth+1)==BINDING_READ) return BINDING_READ;
        }
    }
    return BINDING_CONTINUES;
}

static int binding_still_used(const BorrowChecker *checker, size_t symbol, size_t token) {
    if (checker->summarizing || !checker->current_statement || checker->last_use[symbol]==SIZE_MAX)
        return checker->last_use[symbol]>=token;
    int flow=binding_flow(checker->current_statement,symbol,0);
    for (const BorrowContinuation *tail=checker->continuations;
         flow==BINDING_CONTINUES && tail;tail=tail->next)
        flow=binding_flow(tail->statements,symbol,0);
    return flow==BINDING_READ;
}

static int borrower_is_lent(const BorrowChecker *checker, const BorrowRecord *loan, size_t token) {
    for (const BorrowRecord *child = checker->borrows; child; child = child->next)
        if (child != loan && child->active && child->owner_symbol == loan->borrower_symbol &&
            child->borrower_symbol < checker->analyzer->model->symbol_count &&
            (child->retained_until_drop || child->captured_by_future ||
             binding_still_used(checker,child->borrower_symbol,token))) return 1;
    return 0;
}

static void expire_borrows(BorrowChecker *checker, size_t token) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && !borrow->summary_foreign && !borrow->summary_effect && borrow->borrower_symbol != AST_SYMBOL_NONE &&
            borrow->borrower_symbol < checker->analyzer->model->symbol_count &&
            !borrow->package_storage && !borrow->captured_by_future && !borrow->retained_until_drop &&
            checker->last_use[borrow->borrower_symbol] != SIZE_MAX &&
            !binding_still_used(checker,borrow->borrower_symbol,token) &&
            !borrower_is_lent(checker, borrow, token))
            borrow->active = 0;
}

static void deactivate_scope(BorrowChecker *checker, size_t scope_depth) {
    for (BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next)
        if (borrow->active && !borrow->summary_foreign && borrow->scope_depth >= scope_depth)
            borrow->active = 0;
}

static void report_borrow_error(BorrowChecker *checker, size_t token,
                                const char *message) {
    if (checker->summarizing) return;
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
    if (expression_place(checker, expression, &storage)) {
        if (access == BORROW_ACCESS_WRITE && storage.owner < checker->analyzer->model->symbol_count &&
            package_borrow_holder(checker, &checker->analyzer->model->symbols[storage.owner]) &&
            !(expression->kind == AST_EXPR_UNARY && expression->operator_type == TOKEN_STAR)) {
            report_borrow_error(checker, expression->first_token,
                "Package storage holding checked borrows cannot be rebound, moved, or modified");
            return;
        }
        for (const BorrowRecord *child = checker->borrows; child; child = child->next) {
            if (!child->active || !child->parent || child->borrower_symbol == storage.owner) continue;
            for (const BorrowRecord *parent = child->parent; parent; parent = parent->parent) {
                if (parent->borrower_symbol == storage.owner &&
                    borrow_paths_overlap(checker, storage.field, parent->borrower_field_symbol) &&
                    (access == BORROW_ACCESS_WRITE || child->kind == AST_BORROW_MUTABLE)) {
                    if (checker->summarizing && child->summary_foreign && !child->package_storage &&
                        storage.owner < checker->analyzer->model->symbol_count &&
                        checker->analyzer->model->symbols[storage.owner].scope_depth == 0 && checker->caller_analyzer)
                        semantic_error(checker->caller_analyzer, checker->call_token, ERROR_CATEGORY_SEMANTIC,
                            ERR_SEM_INVALID_DECLARATION, "Function accesses package storage while a conflicting borrow is live");
                    report_borrow_error(checker, expression->first_token,
                                       "Cannot access a value while a conflicting reborrow is live");
                    return;
                }
            }
        }
    }
    BorrowPlace place;
    int slot_write = access == BORROW_ACCESS_WRITE && expression->resolved_borrow_kind != AST_BORROW_NONE;
    if (!(slot_write ? reference_storage_place(checker, expression, &place) : canonical_place(checker, expression, &place)))
        return;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active || (borrow->owner_symbol == borrow->borrower_symbol &&
            borrow->field_symbol == borrow->borrower_field_symbol) || loan_ancestor(borrow, place.through) ||
            !places_overlap(checker, &place, borrow))
            continue;
        if (access == BORROW_ACCESS_WRITE) {
            if (borrow->slice_view && expression->kind == AST_EXPR_INDEX)
                continue;
            if (checker->summarizing && borrow->summary_foreign && !borrow->package_storage &&
                place.owner < checker->analyzer->model->symbol_count &&
                checker->analyzer->model->symbols[place.owner].scope_depth == 0 && checker->caller_analyzer)
                semantic_error(checker->caller_analyzer, checker->call_token, ERROR_CATEGORY_SEMANTIC,
                    ERR_SEM_INVALID_DECLARATION, "Function accesses package storage while a conflicting borrow is live");
            report_borrow_error(checker, expression->first_token,
                                "Cannot modify or move a value while it is borrowed");
            return;
        }
        if (borrow->kind == AST_BORROW_MUTABLE) {
            if (checker->summarizing && borrow->summary_foreign && !borrow->package_storage &&
                place.owner < checker->analyzer->model->symbol_count &&
                checker->analyzer->model->symbols[place.owner].scope_depth == 0 && checker->caller_analyzer)
                semantic_error(checker->caller_analyzer, checker->call_token, ERROR_CATEGORY_SEMANTIC,
                    ERR_SEM_INVALID_DECLARATION, "Function accesses package storage while a conflicting borrow is live");
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
    if (!(borrow_expression->right->resolved_borrow_kind != AST_BORROW_NONE
        ? reference_storage_place(checker, borrow_expression->right, &place)
        : canonical_place(checker, borrow_expression->right, &place))) return;
    for (const BorrowRecord *borrow = checker->borrows; borrow != NULL;
         borrow = borrow->next) {
        if (!borrow->active || (borrow->owner_symbol == borrow->borrower_symbol &&
            borrow->field_symbol == borrow->borrower_field_symbol) || loan_ancestor(borrow, place.through) ||
            !places_overlap(checker, &place, borrow))
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
/* Synthetic calls live in the AST arena because summaries retain call identity. */
static void clone_aggregate_borrows(BorrowChecker *, const AstExpression *, const AstExpression *, size_t);
static void clone_pattern_borrows(BorrowChecker *, const AstExpression *, const AstExpression *, size_t, size_t, size_t);
static void release_argument_borrows(BorrowChecker *, size_t);
static int call_returns_view_aggregate(const BorrowChecker *, const AstExpression *);
static int function_package_borrows(BorrowChecker *, size_t, unsigned char *);
static AstExpression *borrow_summary_call(BorrowChecker *checker, size_t symbol,
                                         const AstExpression *argument, size_t token) {
    if (symbol >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[symbol];
    if (!function->declaration || function->declaration->kind != AST_DECL_FUNCTION) return NULL;
    AstExpression *call = ast_program_alloc(checker->analyzer->program, sizeof(*call));
    AstExpression *input = ast_program_alloc(checker->analyzer->program, sizeof(*input));
    if (!call || !input) { checker->analyzer->allocation_failed = 1; return NULL; }
    *input = *argument;
    input->next = NULL;
    *call = (AstExpression){.kind = AST_EXPR_CALL, .resolved_symbol_id = symbol,
        .arguments = input, .first_token = token};
    set_expression_declared_type(checker->analyzer, call, function->source_program,
        &function->declaration->as.function.return_type);
    return call;
}

static void clone_propagation_borrows(BorrowChecker *checker, const AstExpression *target,
    const AstExpression *source, size_t scope_depth, int residual) {
    AstExpression *branch = borrow_summary_call(checker, source->propagation_branch_symbol_id,
                                               source->left, source->first_token);
    if (!branch) return;
    AstExpression branched = {.kind = AST_EXPR_NAME,
        .resolved_symbol_id = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++,
        .resolved_named_symbol_id = branch->resolved_named_symbol_id};
    clone_aggregate_borrows(checker, &branched, branch, scope_depth);
    if (!residual) {
        clone_pattern_borrows(checker, target, &branched,
            source->propagation_continue_symbol_id, 0, scope_depth);
    } else {
        AstExpression input = {.kind = AST_EXPR_NAME,
            .resolved_symbol_id = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++};
        set_expression_declared_type(checker->analyzer, &input, source->propagation_contract_program,
            &source->propagation_residual_type);
        clone_pattern_borrows(checker, &input, &branched,
            source->propagation_break_symbol_id, 0, scope_depth);
        AstExpression *conversion = borrow_summary_call(checker, source->propagation_from_residual_symbol_id,
                                                        &input, source->first_token);
        if (conversion) {
            if (source->propagation_return_variant_symbol_id == AST_SYMBOL_NONE)
                clone_aggregate_borrows(checker, target, conversion, scope_depth);
            else {
                const SemanticSymbol *variant = &checker->analyzer->model->symbols[source->propagation_return_variant_symbol_id];
                AstExpression wrapped = {.kind = AST_EXPR_ENUM_CONSTRUCT,
                    .resolved_named_symbol_id = variant->owner_symbol_id,
                    .resolved_symbol_id = variant->id, .arguments = conversion};
                clone_aggregate_borrows(checker, target, &wrapped, scope_depth);
            }
        }
        release_argument_borrows(checker, input.resolved_symbol_id);
    }
    release_argument_borrows(checker, branched.resolved_symbol_id);
}

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
    if (expression->is_receiver_reference) return 1;
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
static void heap_initialize(BorrowChecker *,const AstExpression *,const AstExpression *,size_t);
static void heap_shift(BorrowChecker *,const AstStatement *);
static int heap_take(BorrowChecker *,const AstExpression *,const AstExpression *,size_t);
typedef struct HeapBorrowPlace HeapBorrowPlace;
static int heap_drain(BorrowChecker *,const AstStatement *,const FutureLoanBranch *,const HeapBorrowPlace *);
static int heap_forwarded_operation(BorrowChecker *, const AstExpression *, AstLifetimeOperation);
static void index_invalidate_call(BorrowChecker *, const AstExpression *, const SemanticSymbol *, int);
static BorrowRecord *add_borrow_mode(BorrowChecker *, const AstExpression *, const AstExpression *, size_t, int);
static BorrowRecord *add_lifetime_parameter_borrows(BorrowChecker *, const AstExpression *,
    const AstExpression *, const AstProgram *, const AstType *, const char *, AstBorrowKind, size_t);
static int type_has_view_arguments(const BorrowChecker *checker, const AstType *type);
static int type_has_view_depth(const BorrowChecker *, const AstProgram *, const AstType *, size_t);
static int type_has_mutable_view(const BorrowChecker *, const AstProgram *, const AstType *, unsigned);
static const char *payload_lifetime(BorrowChecker *, const AstProgram *, const AstType *, size_t, size_t);
static void check_future_return(BorrowChecker *, const AstExpression *);
static BorrowCallSummary *summarize_call_borrows(BorrowChecker *, const AstExpression *);
static void clone_propagation_borrows(BorrowChecker *, const AstExpression *,
                                     const AstExpression *, size_t, int);
static BorrowRecord *clone_call_summary(BorrowChecker *, const AstExpression *,
                                        BorrowCallSummary *, size_t);
static BorrowCallSummary *replacement_borrows(BorrowChecker *, const AstExpression *, size_t);
static int clone_result_field_borrows(BorrowChecker *, const AstExpression *,
                                      const AstExpression *, size_t);

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
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        if (loan->borrower_symbol == temporary) loan->active = 0;
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
            if (places_overlap(checker, &left_place, &loan)) {
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
            int checked_pointer = ptr && (ptr->resolved_borrow_kind != AST_BORROW_NONE ||
                (ptr->kind == AST_EXPR_NAME && ptr->resolved_symbol_id < checker->analyzer->model->symbol_count &&
                 checker->analyzer->model->symbols[ptr->resolved_symbol_id].resolved_borrow_kind != AST_BORROW_NONE));
            if (checked_pointer &&
                canonical_place(checker, ptr, &origin) && origin.through)
                report_borrow_error(checker, expression->first_token,
                                   "Cannot change a value's lifetime while it is borrowed");
            check_expression(checker, ptr, BORROW_ACCESS_READ);
        }
        if (expression->lifetime_operation == LIFETIME_INITIALIZE) {
            check_expression(checker, ptr ? ptr->next : NULL, BORROW_ACCESS_WRITE);
            heap_initialize(checker,ptr,ptr ? ptr->next:NULL,checker->current_scope_depth);
        }
        if (expression->lifetime_operation == LIFETIME_DESTROY) {
            AstExpression discarded={.kind=AST_EXPR_NAME,
                .resolved_symbol_id=AST_SYMBOL_NONE-1-checker->temporary_borrowers++};
            if (heap_take(checker,&discarded,ptr,checker->current_scope_depth))
                deactivate_borrower(checker,discarded.resolved_symbol_id,AST_SYMBOL_NONE);
            else if (place && place->kind==AST_EXPR_NAME)
                deactivate_borrower(checker,place->resolved_symbol_id,AST_SYMBOL_NONE);
        }
        if (expression->lifetime_operation == LIFETIME_REPLACE)
            replacement_borrows(checker,expression,checker->current_scope_depth);
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
                checker, function, function->declaration->as.function.body) || function->declaration->as.function.receiver_mode != 0 ||
            expression->receiver_contract_mode != 0 ||
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
                if (!loan->active || loan == origin.through || !places_overlap(checker, &origin, loan)) continue;
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
    BorrowCallSummary *call_summary=summarize_call_borrows(checker, expression);
    index_invalidate_call(checker, expression, function, call_summary != NULL);
    if (function && function->declaration && (function->declaration->as.function.receiver_mode == 2 || expression->receiver_contract_mode==2) &&
        expression->left && expression->left->kind == AST_EXPR_MEMBER) {
        BorrowPlace consumed;
        if (expression_place(checker, expression->left->left, &consumed))
            deactivate_borrower(checker, consumed.owner, consumed.field);
    }
    if ((!call_summary || !call_summary->heap_effect) && function && function->declaration && !function->declaration->as.function.is_static &&
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
    if (expression->async_operation == ASYNC_CANCEL_COMPLETE) {
        check_call(checker,expression);
        release_future_value(checker,expression->arguments);
        return;
    }
    if ((expression->async_operation == ASYNC_BLOCK_ON || expression->async_operation == ASYNC_COMPLETE)) {
        check_call(checker,expression);
        if (!semantic_expression_is_future(expression) && expression->resolved_borrow_kind==AST_BORROW_NONE && !expression->resolved_is_slice &&
            !(expression->has_resolved_ast_type && type_has_view_depth(checker,
                expression->resolved_type_program ? expression->resolved_type_program : checker->analyzer->program,
                &expression->resolved_ast_type, 0)) &&
            !(semantic_symbol_type_properties(checker->analyzer->model, expression->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME))
            release_future_value(checker,expression->arguments);
        return;
    }
    if (expression->kind == AST_EXPR_AWAIT) {
        check_expression(checker, expression->right, BORROW_ACCESS_WRITE);
        if (!semantic_expression_is_future(expression) &&
            expression->resolved_borrow_kind == AST_BORROW_NONE && !expression->resolved_is_slice &&
            !(expression->has_resolved_ast_type && type_has_view_depth(checker,
                expression->resolved_type_program ? expression->resolved_type_program : checker->analyzer->program,
                &expression->resolved_ast_type, 0)) &&
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
            AstExpression residual = {.kind = AST_EXPR_NAME,
                .resolved_symbol_id = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++,
                .first_token = expression->first_token};
            set_expression_declared_type(checker->analyzer, &residual,
                checker->analyzer->program, &checker->analyzer->current_function->as.function.return_type);
            int saved_reserving = checker->reserving_loans;
            checker->reserving_loans = 1;
            checker->aggregate_capture_depth++;
            clone_propagation_borrows(checker, &residual, expression, checker->current_scope_depth, 1);
            check_future_return(checker, &residual);
            release_argument_borrows(checker, residual.resolved_symbol_id);
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
             borrow_path_prefix(checker, borrower_field, old->borrower_field_symbol)))
            old->active = 0;
}

static BorrowRecord *add_borrow_mode(BorrowChecker *checker,
                                     const AstExpression *borrower,
                                     const AstExpression *value,
                                     size_t scope_depth,
                                     int preserve_existing) {
    if (value == NULL || borrower == NULL)
        return NULL;
    if (value->lifetime_operation == LIFETIME_TAKE || value->lifetime_operation == LIFETIME_REPLACE) {
        BorrowRecord *boundary=checker->borrows;
        clone_aggregate_borrows(checker,borrower,value,scope_depth);
        return checker->borrows!=boundary ? checker->borrows:NULL;
    }
    if (value->kind == AST_EXPR_PROPAGATE) {
        BorrowRecord *boundary = checker->borrows;
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return checker->borrows != boundary ? checker->borrows : NULL;
    }
    /* An explicit result lifetime can be shared by multiple input origins.
       Retain all of them; runtime control flow chooses the actual referent. */
    if (value->kind == AST_EXPR_CALL && value->resolved_borrow_kind != AST_BORROW_NONE &&
        value->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        BorrowCallSummary *summary = summarize_call_borrows(checker, value);
        if (summary) return clone_call_summary(checker, borrower, summary, scope_depth);
        const SemanticSymbol *function = &checker->analyzer->model->symbols[value->resolved_symbol_id];
        const AstDeclarationNode *declaration = function->kind == SEMANTIC_SYMBOL_FUNCTION ? function->declaration : NULL;
        const char *lifetime = declaration ? borrow_lifetime(function->source_program,
            &declaration->as.function.return_type) : NULL;
        if (lifetime) {
            BorrowRecord *first = NULL;
            BorrowPlace target;
            if (!expression_place(checker, borrower, &target)) return NULL;
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
        if (!expression_place(checker, borrower, &borrower_place)) return NULL;
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
    if (follows_referent && referent && expression_place(checker, referent, &storage)) {
        size_t count = 0;
        for (const BorrowRecord *old = checker->borrows; old; old = old->next)
            if (old->active && !old->captured_by_future && old->borrower_symbol == storage.owner &&
                borrow_paths_overlap(checker, storage.field, old->borrower_field_symbol)) count++;
        if (count > 1) {
            size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
            BorrowRecord *boundary = checker->borrows;
            for (const BorrowRecord *old = boundary; old; old = old->next) {
                if (!old->active || old->captured_by_future || old->borrower_symbol != storage.owner ||
                    !borrow_paths_overlap(checker, storage.field, old->borrower_field_symbol)) continue;
                BorrowRecord *copy = malloc(sizeof(*copy));
                if (!copy) { checker->analyzer->allocation_failed = 1; break; }
                *copy = *old;
                copy->summary_foreign = 0;
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
                                   (value->async_operation==ASYNC_BLOCK_ON || value->async_operation==ASYNC_COMPLETE) ? value->arguments:NULL;
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
    int slice_view = (value->resolved_is_slice || value->is_array_view) &&
                     value->resolved_borrow_kind == AST_BORROW_NONE;
    int string_view = value->resolved_type == TYPE_STRING &&
                      has_returned_place;
    if (!slice_view && !string_view &&
        value->resolved_borrow_kind == AST_BORROW_NONE)
        return NULL;
    BorrowPlace place, borrower_place;
    if (!expression_place(checker, borrower, &borrower_place))
        return NULL;
    const AstExpression *origin = value;
    int reference_slot = value->kind == AST_EXPR_UNARY && value->operator_type == TOKEN_AMPERSAND &&
        value->right && value->right->resolved_borrow_kind != AST_BORROW_NONE;
    if (origin->kind == AST_EXPR_UNARY &&
        origin->operator_type == TOKEN_AMPERSAND)
        origin = origin->right;
    if (!(reference_slot ? reference_storage_place(checker, origin, &place) : canonical_place(checker, origin, &place))) {
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
    borrow->reference_slot = reference_slot || (place.through && place.through->reference_slot &&
        value->has_resolved_ast_type && value->resolved_ast_type.reference_type);
    borrow->parent = place.through;
    borrow->iteration_scope_depth = place.through ? place.through->iteration_scope_depth : 0;
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
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, type));
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
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, type));
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
    if (value->kind == AST_EXPR_CALL) {
        BorrowCallSummary *summary = summarize_call_borrows(checker, value);
        if (summary) {
            clone_call_summary(checker, borrower, summary, scope_depth);
            return;
        }
    }
    if (value->lifetime_operation == LIFETIME_TAKE || value->lifetime_operation == LIFETIME_REPLACE) {
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return;
    }
    if (value->kind == AST_EXPR_NAME) {
        clone_aggregate_borrows(checker, borrower, value, scope_depth);
        return;
    }
    if (value->kind == AST_EXPR_AWAIT || (value->async_operation==ASYNC_BLOCK_ON || value->async_operation==ASYNC_COMPLETE)) {
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
    BorrowRecord *frame_boundary = checker->borrows;
    unsigned char *package_uses = calloc(checker->analyzer->model->symbol_count, 1);
    if (!package_uses) { checker->analyzer->allocation_failed = 1; return; }
    if (function_package_borrows(checker, value->resolved_symbol_id, package_uses)) {
        BorrowRecord *boundary = checker->borrows;
        for (BorrowRecord *global = boundary; global; global = global->next) {
            if (!global->package_storage || !global->active || global->kind != AST_BORROW_MUTABLE ||
                global->borrower_symbol >= checker->analyzer->model->symbol_count ||
                !package_uses[global->borrower_symbol]) continue;
            AstExpression input = {.kind = AST_EXPR_NAME, .resolved_symbol_id = global->borrower_symbol,
                .resolved_borrow_kind = AST_BORROW_MUTABLE, .first_token = value->first_token};
            AstExpression referent = {.kind = AST_EXPR_UNARY, .operator_type = TOKEN_STAR, .right = &input};
            AstExpression reborrow = {.kind = AST_EXPR_UNARY, .operator_type = TOKEN_AMPERSAND,
                .right = &referent, .mutable_borrow = 1, .first_token = value->first_token};
            check_new_borrow(checker, &reborrow);
            BorrowRecord *loan = add_borrow_mode(checker, borrower, &input, scope_depth, 1);
            if (loan) loan->captured_by_future = 1;
        }
    }
    free(package_uses);
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
                copy->summary_foreign = 0;
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
    BorrowRecord *result_boundary = checker->borrows;
    checker->aggregate_capture_depth++;
    int mapped_result = clone_result_field_borrows(checker, borrower, value, scope_depth);
    checker->aggregate_capture_depth--;
    if (mapped_result) {
        for (BorrowRecord *loan = result_boundary; loan != frame_boundary; loan = loan->next)
            loan->future_frame_only = 1;
        for (BorrowRecord *loan = checker->borrows; loan != result_boundary; loan = loan->next) {
            loan->captured_by_future = 1;
            loan->future_frame_only = 0;
        }
    }
}

static const char *aggregate_lifetime(BorrowChecker *checker, const AstProgram *unit,
                                     const AstType *type, const char *declared) {
    if (!declared || !type || type->kind != AST_TYPE_NAMED) return NULL;
    if (!strcmp(declared, "'static")) return declared;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, type));
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
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, type));
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
    if (!result || result->kind != AST_TYPE_NAMED || field_id == AST_SYMBOL_NONE) return NULL;
    size_t root = field_id;
    const BorrowPath *path;
    while ((path = borrow_path_node(checker, root)) && path->parent != AST_SYMBOL_NONE)
        root = path->parent;
    size_t owner = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, result));
    if (owner >= checker->analyzer->model->symbol_count) return NULL;
    const SemanticSymbol *aggregate = &checker->analyzer->model->symbols[owner];
    const AstType *child = NULL;
    const AstProgram *child_unit = aggregate->source_program;
    if (path && path->payload) {
        if (path->enum_symbol != owner || !aggregate->declaration ||
            aggregate->declaration->kind != AST_DECL_ENUM) return NULL;
        for (const AstEnumValue *value = aggregate->declaration->as.enum_decl.values; value; value = value->next) {
            if (value->resolved_symbol_id != path->variant) continue;
            const AstTypeArgument *payload = value->payload_types;
            for (size_t i = 0; payload && i < path->index; i++) payload = payload->next;
            if (payload) child = &payload->type;
            break;
        }
    } else if (root < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *field = &checker->analyzer->model->symbols[root];
        if (field->kind != SEMANTIC_SYMBOL_FIELD || field->owner_symbol_id != owner) return NULL;
        child = &field->declared_type;
        child_unit = field->source_program;
    }
    if (!child) return NULL;
    if (field_id != root) {
        const char *inner = result_field_lifetime(checker, child_unit, child,
            borrow_path_relative(checker, field_id, root));
        return aggregate_lifetime(checker, unit, result, inner);
    }
    const char *declared = borrow_lifetime(child_unit, child);
    if (!declared && child->kind == AST_TYPE_NAMED &&
        !child->pointer_depth && !child->outer_pointer_depth &&
        child->lifetime_arguments && !child->lifetime_arguments->next)
        declared = ast_program_lexeme(child_unit, child->lifetime_arguments->name_token);
    return aggregate_lifetime(checker, unit, result, declared);
}

static void summary_storage_map(BorrowChecker *checker, size_t symbol,
                                size_t owner, size_t field) {
    BorrowStorage *storage = malloc(sizeof(*storage));
    if (!storage) { checker->analyzer->allocation_failed = 1; return; }
    *storage = (BorrowStorage){.symbol = symbol, .owner = owner, .field = field,
        .next = checker->storage};
    checker->storage = storage;
}

static int summary_write_place(BorrowChecker *checker, const AstExpression *target,
                               BorrowPlace *place) {
    if (!checker->summarizing || !expression_place(checker, target, place)) return 0;
    for (const BorrowStorage *storage = checker->storage; storage; storage = storage->next) {
        if (storage->symbol != place->owner) continue;
        place->owner = storage->owner;
        place->field = borrow_path_append(checker, storage->field, place->field);
        BorrowWrite *write = malloc(sizeof(*write));
        if (!write) { checker->analyzer->allocation_failed = 1; return 0; }
        *write = (BorrowWrite){.owner = place->owner, .field = place->field, .next = checker->writes};
        checker->writes = write;
        return 1;
    }
    return 0;
}

static int summary_written(const BorrowChecker *checker, const BorrowRecord *loan) {
    for (const BorrowWrite *write = checker->writes; write; write = write->next)
        if (write->owner == loan->borrower_symbol &&
            borrow_path_prefix(checker, write->field, loan->borrower_field_symbol)) return 1;
    return 0;
}

static void summary_capture_exit(BorrowChecker *checker) {
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next) {
        if(loan->active) {
            if(!loan->summary_exit_active) { loan->heap_exit_min=loan->heap_min; loan->heap_exit_max=loan->heap_max; }
            else {
                if(loan->heap_min<loan->heap_exit_min) loan->heap_exit_min=loan->heap_min;
                if(loan->heap_max>loan->heap_exit_max) loan->heap_exit_max=loan->heap_max;
            }
        }
        loan->summary_exit_active |= loan->active;
    }
}

struct HeapBorrowPlace {
    BorrowPlace storage;
    BorrowPlace anchor;
    int anchored;
    int64_t offset;
    int64_t minimum;
    int exact;
    int frozen;
    int ring;
    BorrowPlace head;
    BorrowPlace modulus;
    int bounded;
    BorrowPlace extent;
    BorrowHeapGuard *guard;
};

static void index_bound_value(const BorrowChecker *checker, const BorrowPlace *place,
                              int64_t *minimum, int *exact) {
    *minimum = 0; *exact = 0;
    for (const BorrowIndexBound *p = checker->index_bounds; p; p = p->next)
        if (p->valid && p->owner == place->owner && p->field == place->field) {
            if (p->exact) { *minimum = p->minimum; *exact = 1; return; }
            if (p->minimum > *minimum) *minimum = p->minimum;
        }
}

/* Values of unchanged by-value index parameters survive summary boundaries.
   This is deliberately not a mutable scalar constant-propagation domain. */
static int heap_index_constant(const BorrowChecker *checker, const AstExpression *value,
                               int64_t *result, unsigned depth) {
    if (!value || depth > 32) return 0;
    if (value->kind == AST_EXPR_NAME) {
        for (const BorrowIndexConstant *p = checker->index_constants; p; p = p->next)
            if (p->symbol == value->resolved_symbol_id && p->valid && !p->relative)
                { *result = p->value; return 1; }
        return 0;
    }
    if (value->kind == AST_EXPR_LITERAL) {
        const AstToken *token = ast_program_token(checker->analyzer->program, value->value_token);
        if (!token || token->type != TOKEN_NUMBER) return 0;
        const char *p = token->lexeme;
        int64_t n = 0;
        if (!*p) return 0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || n > (INT64_MAX - (*p - '0')) / 10) return 0;
            n = n * 10 + (*p - '0');
        }
        *result = n;
        return 1;
    }
    int64_t left, right;
    if (value->kind != AST_EXPR_BINARY ||
        (value->resolved_type != TYPE_USIZE && value->resolved_type != TYPE_U64 &&
         value->resolved_type != TYPE_I64) ||
        !heap_index_constant(checker, value->left, &left, depth + 1) ||
        !heap_index_constant(checker, value->right, &right, depth + 1)) return 0;
    if (value->operator_type == TOKEN_PLUS && left <= INT64_MAX - right) *result = left + right;
    else if (value->operator_type == TOKEN_MINUS && left >= right) *result = left - right;
    else return 0;
    return 1;
}

static int index_statement_writes(BorrowChecker *, const AstStatement *, size_t, unsigned);
static int index_expression_writes(BorrowChecker *checker, const AstExpression *e,
                                   size_t symbol, unsigned depth) {
    if (depth > 64) return 1;
    for (; e; e = e->next) {
        BorrowPlace place;
        if (e->kind == AST_EXPR_UNARY && e->operator_type == TOKEN_AMPERSAND &&
            e->mutable_borrow && expression_place(checker, e->right, &place) &&
            place.owner == symbol) return 1;
        if (index_expression_writes(checker, e->left, symbol, depth + 1) ||
            index_expression_writes(checker, e->right, symbol, depth + 1) ||
            index_expression_writes(checker, e->arguments, symbol, depth + 1) ||
            index_statement_writes(checker, e->control, symbol, depth + 1)) return 1;
    }
    return 0;
}
static int index_statement_writes(BorrowChecker *checker, const AstStatement *s,
                                  size_t symbol, unsigned depth) {
    if (depth > 64) return 1;
    for (; s; s = s->next) {
        BorrowPlace place;
        if (s->kind == AST_STMT_ASSIGNMENT && expression_place(checker, s->expression, &place) &&
            place.owner == symbol) return 1;
        if (index_expression_writes(checker, s->expression, symbol, depth + 1) ||
            index_expression_writes(checker, s->value, symbol, depth + 1) ||
            index_expression_writes(checker, s->condition, symbol, depth + 1) ||
            index_expression_writes(checker, s->update, symbol, depth + 1) ||
            index_expression_writes(checker, s->result, symbol, depth + 1) ||
            index_statement_writes(checker, s->initializer, symbol, depth + 1) ||
            index_statement_writes(checker, s->body, symbol, depth + 1) ||
            index_statement_writes(checker, s->else_body, symbol, depth + 1)) return 1;
        for (const AstMatchArm *arm = s->match_arms; arm; arm = arm->next)
            if (index_statement_writes(checker, arm->body, symbol, depth + 1)) return 1;
    }
    return 0;
}

static const BorrowStorage *heap_map_place(BorrowChecker *, BorrowPlace *);
static int heap_index_place(BorrowChecker *checker, const AstExpression *index,
                            BorrowPlace *anchor, int64_t *offset, unsigned depth) {
    if (!index || depth > 32) return 0;
    if (index->kind == AST_EXPR_NAME)
        for (const BorrowIndexConstant *p = checker->index_constants; p; p = p->next)
            if (p->symbol == index->resolved_symbol_id) {
                if (!p->valid) return 0;
                *anchor = (BorrowPlace){.owner=p->relative ? p->anchor_owner : AST_SYMBOL_NONE,
                    .field=p->relative ? p->anchor_field : AST_SYMBOL_NONE};
                *offset = p->value;
                return 1;
            }
    if (heap_index_constant(checker, index, offset, 0)) {
        *anchor = (BorrowPlace){.owner=AST_SYMBOL_NONE,.field=AST_SYMBOL_NONE};
        return 1;
    }
    if (expression_place(checker, index, anchor)) {
        heap_map_place(checker, anchor);
        *offset = 0;
        return 1;
    }
    /* A parameterless getter returning exactly one scalar field is a place
       projection. Its spelling and package do not matter. */
    if (index->kind == AST_EXPR_CALL && !index->arguments && index->left &&
        index->left->kind == AST_EXPR_MEMBER &&
        index->resolved_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *function = &checker->analyzer->model->symbols[index->resolved_symbol_id];
        const AstStatement *body = function->kind == SEMANTIC_SYMBOL_FUNCTION &&
            function->declaration && !function->declaration->as.function.is_async
            ? function->declaration->as.function.body : NULL;
        if (body && body->kind == AST_STMT_BLOCK && !body->next) body = body->body;
        if (body && !body->next && body->kind == AST_STMT_RETURN && body->value &&
            body->value->kind == AST_EXPR_NAME &&
            body->value->resolved_symbol_id < checker->analyzer->model->symbol_count &&
            canonical_place(checker, index->left->left, anchor)) {
            const SemanticSymbol *field = &checker->analyzer->model->symbols[body->value->resolved_symbol_id];
            if (field->kind == SEMANTIC_SYMBOL_FIELD && field->owner_symbol_id == function->owner_symbol_id &&
                data_type_integral(field->resolved_type)) {
                anchor->field = borrow_path_append(checker, anchor->field, field->id);
                heap_map_place(checker, anchor);
                *offset = 0;
                return 1;
            }
        }
    }
    if (index->kind != AST_EXPR_BINARY ||
        (index->resolved_type != TYPE_USIZE && index->resolved_type != TYPE_U64 &&
         index->resolved_type != TYPE_I64)) return 0;
    int64_t adjustment, base;
    if (!heap_index_constant(checker, index->right, &adjustment, 0) ||
        !heap_index_place(checker, index->left, anchor, &base, depth + 1)) return 0;
    if (index->operator_type == TOKEN_PLUS && base <= INT64_MAX - adjustment)
        *offset = base + adjustment;
    else if (index->operator_type == TOKEN_MINUS && base >= INT64_MIN + adjustment)
        *offset = base - adjustment;
    else return 0;
    return *offset != INT64_MIN && *offset != INT64_MAX;
}

static void index_invalidate_place(BorrowChecker *checker, const BorrowPlace *place) {
    for(BorrowStorage *storage=checker->storage;storage;storage=storage->next)
        if(storage->heap_bounded && storage->extent_owner==place->owner &&
            borrow_paths_overlap(checker,storage->extent_field,place->field)) storage->heap_bounded=0;
    for(BorrowHeapGuard *guard=checker->heap_guards;guard;guard=guard->next)
        if((guard->owner==place->owner &&
            (borrow_paths_overlap(checker,guard->tag,place->field) ||
             borrow_path_prefix(checker,place->field,guard->root))) ||
            (!guard->fixed && guard->index_owner==place->owner &&
             borrow_paths_overlap(checker,guard->index_field,place->field))) guard->valid=0;
    for (BorrowIndexConstant *p = checker->index_constants; p; p = p->next)
        if ((p->symbol == place->owner && place->field == AST_SYMBOL_NONE) ||
            (p->relative && p->anchor_owner == place->owner &&
             borrow_paths_overlap(checker, p->anchor_field, place->field))) p->valid = 0;
    for (BorrowIndexBound *p = checker->index_bounds; p; p = p->next)
        if (p->owner == place->owner && borrow_paths_overlap(checker, p->field, place->field)) p->valid = 0;
}

/* A guard fact is useful only while its place stays unchanged. Invalidation
   is monotone across unknown branches; no fact learned on just one arm leaks. */
static BorrowIndexBound *index_guard_probe(BorrowChecker *checker, const AstExpression *condition) {
    if (!condition || condition->kind != AST_EXPR_BINARY ||
        !condition->left || (condition->left->resolved_type != TYPE_USIZE && condition->left->resolved_type != TYPE_U64)) return NULL;
    BorrowPlace anchor;
    int64_t offset, value;
    if (!heap_index_place(checker, condition->left, &anchor, &offset, 0) || offset != 0 ||
        anchor.owner == AST_SYMBOL_NONE || !heap_index_constant(checker, condition->right, &value, 0)) return NULL;
    BorrowIndexBound *probe = malloc(sizeof(*probe));
    if (!probe) { checker->analyzer->allocation_failed=1; return NULL; }
    *probe = (BorrowIndexBound){.owner=anchor.owner,.field=anchor.field,.valid=1,.next=checker->index_bounds};
    checker->index_bounds=probe;
    return probe;
}

static void index_guard_finish(BorrowChecker *checker, const AstStatement *statement, BorrowIndexBound *probe) {
    if (!probe || !probe->valid) return;
    int yes = statement_may_fall_through(statement->body);
    int no = statement_may_fall_through(statement->else_body);
    int64_t value;
    if (yes == no || !heap_index_constant(checker, statement->condition->right, &value, 0)) return;
    TokenType op = statement->condition->operator_type;
    if ((yes && op == TOKEN_EQUAL_EQUAL) || (no && op == TOKEN_BANG_EQUAL))
        { probe->minimum=value; probe->exact=1; }
    else if ((yes && op == TOKEN_GREATER_EQUAL) || (no && op == TOKEN_LESS)) probe->minimum=value;
    else if (((yes && op == TOKEN_GREATER) || (no && op == TOKEN_LESS_EQUAL)) && value < INT64_MAX)
        probe->minimum=value+1;
}

static void index_discard_since(BorrowChecker *checker, const BorrowIndexBound *boundary) {
    for (BorrowIndexBound *bound=checker->index_bounds;bound!=boundary;bound=bound->next) bound->valid=0;
}

static void heap_invalidate_opaque_write(BorrowChecker *checker,const BorrowPlace *place) {
    for(BorrowRecord *loan=checker->borrows;loan;loan=loan->next) {
        if(!loan->active || !loan->heap_storage) continue;
        if(loan->borrower_symbol==place->owner &&
            borrow_paths_overlap(checker,place->field,loan->borrower_field_symbol)) {
            loan->heap_tagged=0;
            loan->heap_bounded=0;
        }
        if(loan->heap_extent_owner==place->owner &&
            borrow_paths_overlap(checker,place->field,loan->heap_extent_field)) loan->heap_bounded=0;
        if(loan->borrower_symbol==place->owner &&
            borrow_paths_overlap(checker,place->field,loan->heap_tag_field)) loan->heap_tagged=0;
    }
}

static void index_invalidate_call(BorrowChecker *checker, const AstExpression *call,
                                   const SemanticSymbol *function,int summarized) {
    const AstParameter *parameter = function && function->declaration
        ? function->declaration->as.function.parameters : NULL;
    for (const AstExpression *arg = call->arguments; arg; arg = arg->next) {
        if (arg->resolved_borrow_kind == AST_BORROW_MUTABLE ||
            (parameter && parameter->type.borrow_kind == AST_BORROW_MUTABLE)) {
            const AstExpression *base = arg;
            if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND) base = base->right;
            BorrowPlace place;
            if (canonical_place(checker, base, &place)) {
                heap_map_place(checker, &place);
                index_invalidate_place(checker, &place);
                if(!summarized) heap_invalidate_opaque_write(checker,&place);
            }
        }
        if (parameter) parameter = parameter->next;
    }
    if (function && function->declaration && call->left && call->left->kind == AST_EXPR_MEMBER &&
        !function->declaration->as.function.is_static &&
        (function->declaration->as.function.receiver_mode != 0 ||
         statements_mutate_receiver(checker, function, function->declaration->as.function.body))) {
        BorrowPlace place;
        if (canonical_place(checker, call->left->left, &place)) {
            heap_map_place(checker, &place);
            index_invalidate_place(checker, &place);
            if(!summarized) heap_invalidate_opaque_write(checker,&place);
        }
    }
}

static const BorrowStorage *heap_map_place(BorrowChecker *checker,BorrowPlace *place) {
    const BorrowStorage *first=NULL;
    for(unsigned depth=0;depth<32;depth++) {
        const BorrowStorage *match=NULL;
        for(const BorrowStorage *storage=checker->storage;storage;storage=storage->next)
            if(storage->symbol==place->owner) { match=storage; break; }
        if(!match || (match->owner==place->owner && match->field==AST_SYMBOL_NONE)) break;
        if(!first) first=match;
        place->field=borrow_path_append(checker,match->field,place->field);
        place->owner=match->owner;
    }
    return first;
}

static const AstStatement *heap_index_declaration(const AstStatement *statement,size_t symbol,unsigned depth) {
    if(depth>64) return NULL;
    for(;statement;statement=statement->next) {
        if(statement->kind==AST_STMT_VARIABLE && statement->resolved_symbol_id==symbol) return statement;
        const AstStatement *found=heap_index_declaration(statement->initializer,symbol,depth+1);
        if(!found) found=heap_index_declaration(statement->body,symbol,depth+1);
        if(!found) found=heap_index_declaration(statement->else_body,symbol,depth+1);
        if(found) return found;
        for(const AstMatchArm *arm=statement->match_arms;arm;arm=arm->next) {
            found=heap_index_declaration(arm->body,symbol,depth+1);
            if(found) return found;
        }
    }
    return NULL;
}

static int heap_modulus_place(BorrowChecker *checker,const AstExpression *value,BorrowPlace *extent) {
    if(!value || value->kind!=AST_EXPR_BINARY || value->operator_type!=TOKEN_PERCENT ||
        value->resolved_type!=TYPE_USIZE || !expression_place(checker,value->right,extent)) return 0;
    heap_map_place(checker,extent);
    return 1;
}

static int heap_extent_expression_safe(BorrowChecker *checker,const AstExpression *value,
                                       const BorrowPlace *extent,unsigned depth) {
    if(depth>64) return 0;
    for(;value;value=value->next) {
        if(value->resolved_borrow_kind==AST_BORROW_MUTABLE) {
            BorrowPlace place;
            if(!canonical_place(checker,value,&place)) return 0;
            heap_map_place(checker,&place);
            if(place.owner==extent->owner && borrow_paths_overlap(checker,place.field,extent->field)) return 0;
        }
        if(value->kind==AST_EXPR_CALL) {
            const SemanticSymbol *function=value->resolved_symbol_id<checker->analyzer->model->symbol_count ?
                &checker->analyzer->model->symbols[value->resolved_symbol_id]:NULL;
            if(value->left && value->left->kind==AST_EXPR_MEMBER &&
                (!function || !function->declaration ||
                 statements_mutate_receiver(checker,function,function->declaration->as.function.body))) {
                BorrowPlace receiver;
                if(canonical_place(checker,value->left->left,&receiver)) {
                    heap_map_place(checker,&receiver);
                    if(receiver.owner==extent->owner && borrow_paths_overlap(checker,receiver.field,extent->field)) return 0;
                }
            }
        }
        if(value->kind==AST_EXPR_UNARY && value->operator_type==TOKEN_AMPERSAND && value->mutable_borrow) {
            BorrowPlace place;
            if(canonical_place(checker,value->right,&place)) {
                heap_map_place(checker,&place);
                if(place.owner==extent->owner && borrow_paths_overlap(checker,place.field,extent->field)) return 0;
            }
        }
        if(!heap_extent_expression_safe(checker,value->left,extent,depth+1) ||
           !heap_extent_expression_safe(checker,value->right,extent,depth+1) ||
           !heap_extent_expression_safe(checker,value->arguments,extent,depth+1) || value->control) return 0;
    }
    return 1;
}

static int heap_extent_statements_safe(BorrowChecker *checker,const AstStatement *statement,size_t symbol,
                                       const BorrowPlace *extent,unsigned depth) {
    if(depth>64) return 0;
    for(;statement;statement=statement->next) {
        BorrowPlace place;
        if(statement->kind==AST_STMT_ASSIGNMENT && canonical_place(checker,statement->expression,&place)) {
            heap_map_place(checker,&place);
            if(place.owner==symbol) {
                BorrowPlace modulus;
                if(statement->assignment_operator!=TOKEN_EQUAL || !heap_modulus_place(checker,statement->value,&modulus) ||
                    modulus.owner!=extent->owner || modulus.field!=extent->field) return 0;
            } else if(place.owner==extent->owner && borrow_paths_overlap(checker,place.field,extent->field)) return 0;
        }
        if(index_expression_writes(checker,statement->expression,symbol,0) ||
           index_expression_writes(checker,statement->value,symbol,0) ||
           index_expression_writes(checker,statement->condition,symbol,0) ||
           index_expression_writes(checker,statement->update,symbol,0) ||
           index_expression_writes(checker,statement->result,symbol,0) ||
           !heap_extent_expression_safe(checker,statement->expression,extent,0) ||
           !heap_extent_expression_safe(checker,statement->value,extent,0) ||
           !heap_extent_expression_safe(checker,statement->condition,extent,0) ||
           !heap_extent_expression_safe(checker,statement->update,extent,0) ||
           !heap_extent_expression_safe(checker,statement->result,extent,0) ||
           !heap_extent_statements_safe(checker,statement->initializer,symbol,extent,depth+1) ||
           !heap_extent_statements_safe(checker,statement->body,symbol,extent,depth+1) ||
           !heap_extent_statements_safe(checker,statement->else_body,symbol,extent,depth+1)) return 0;
        for(const AstMatchArm *arm=statement->match_arms;arm;arm=arm->next)
            if(!heap_extent_statements_safe(checker,arm->body,symbol,extent,depth+1)) return 0;
    }
    return 1;
}

static int heap_index_extent(BorrowChecker *checker,const AstExpression *index,BorrowPlace *extent) {
    if(heap_modulus_place(checker,index,extent)) return 1;
    if(!index || index->kind!=AST_EXPR_NAME || !checker->analyzer->current_function) return 0;
    const AstStatement *declaration=heap_index_declaration(
        checker->analyzer->current_function->as.function.body,index->resolved_symbol_id,0);
    return declaration && heap_modulus_place(checker,declaration->value,extent) &&
        heap_extent_statements_safe(checker,declaration->next,index->resolved_symbol_id,extent,0);
}

static void index_seed_struct(BorrowChecker *checker,const AstExpression *target,const AstExpression *value) {
    BorrowPlace place;
    if(!value || value->kind!=AST_EXPR_STRUCT_LITERAL || !expression_place(checker,target,&place)) return;
    for(const AstExpression *field=value->arguments;field;field=field->next) {
        int64_t number;
        if(field->initializer_field_symbol_id>=checker->analyzer->model->symbol_count ||
            !data_type_integral(field->resolved_type) || !heap_index_constant(checker,field,&number,0)) continue;
        BorrowIndexBound *bound=malloc(sizeof(*bound));
        if(!bound) { checker->analyzer->allocation_failed=1; return; }
        *bound=(BorrowIndexBound){.owner=place.owner,
            .field=borrow_path_append(checker,place.field,field->initializer_field_symbol_id),
            .minimum=number,.exact=1,.valid=1,.next=checker->index_bounds};
        checker->index_bounds=bound;
    }
}

static int heap_borrow_place(BorrowChecker *checker,const AstExpression *pointer,HeapBorrowPlace *place) {
    if(!pointer) return 0;
    *place=(HeapBorrowPlace){0};
    const AstExpression *value=pointer;
    if(value->kind==AST_EXPR_UNARY && value->operator_type==TOKEN_AMPERSAND) value=value->right;
    if(!expression_place(checker,value,&place->storage)) return 0;
    const BorrowStorage *mapped=heap_map_place(checker,&place->storage);
    place->anchored=0;
    place->ring=0;
    place->offset=0;
    const AstExpression *indexed=value;
    while (indexed && indexed->kind==AST_EXPR_MEMBER) indexed=indexed->left;
    if(indexed && indexed->kind==AST_EXPR_INDEX) {
        const AstExpression *index=indexed->right;
        place->bounded=heap_index_extent(checker,index,&place->extent);
        if (index && index->kind==AST_EXPR_BINARY && index->operator_type==TOKEN_PERCENT &&
            index->resolved_type==TYPE_USIZE && index->left && index->left->kind==AST_EXPR_BINARY &&
            index->left->operator_type==TOKEN_PLUS &&
            expression_place(checker,index->left->left,&place->head) &&
            expression_place(checker,index->right,&place->modulus) &&
            heap_index_place(checker,index->left->right,&place->anchor,&place->offset,0)) {
            heap_map_place(checker,&place->head);
            heap_map_place(checker,&place->modulus);
            place->anchored=1;
            place->ring=1;
        }
        else if(heap_index_place(checker,index,&place->anchor,&place->offset,0)) {
            place->anchored=1;
        }
    } else if(mapped && mapped->heap_index) {
        place->anchor=(BorrowPlace){.owner=mapped->index_owner,.field=mapped->index_field};
        heap_map_place(checker,&place->anchor);
        place->anchored=1;
        place->offset=mapped->index_offset;
        place->ring=mapped->heap_ring;
        place->head=(BorrowPlace){.owner=mapped->head_owner,.field=mapped->head_field};
        place->modulus=(BorrowPlace){.owner=mapped->mod_owner,.field=mapped->mod_field};
        place->bounded=mapped->heap_bounded;
        place->extent=(BorrowPlace){.owner=mapped->extent_owner,.field=mapped->extent_field};
        place->guard=mapped->guard;
    } else return 0;
    /* A constant logical ring index can use an exactly known scalar as its
       affine basis. The following increment establishes the live prefix. */
    const AstStatement *next=checker->current_statement ? checker->current_statement->next : NULL;
    if (place->ring && place->anchor.owner==AST_SYMBOL_NONE && next &&
        next->kind==AST_STMT_ASSIGNMENT && next->assignment_operator==TOKEN_PLUS_EQUAL &&
        next->expression && next->expression->resolved_type==TYPE_USIZE) {
        BorrowPlace basis;
        int64_t one, count; int exact;
        if (heap_index_constant(checker,next->value,&one,0) && one==1 &&
            expression_place(checker,next->expression,&basis)) {
            heap_map_place(checker,&basis);
            index_bound_value(checker,&basis,&count,&exact);
            if (exact && count<INT64_MAX && place->offset>=0 && place->offset<=count &&
                !(basis.owner==place->head.owner && basis.field==place->head.field) &&
                !(basis.owner==place->modulus.owner && basis.field==place->modulus.field)) {
                place->anchor=basis;
                place->offset-=count;
            }
        }
    }
    if(place->storage.owner>=checker->analyzer->model->symbol_count) return 0;
    if(indexed && indexed->kind==AST_EXPR_INDEX)
        for(BorrowHeapGuard *guard=checker->heap_guards;guard;guard=guard->next) {
            if(!guard->valid || guard->owner!=place->storage.owner ||
                !borrow_path_prefix(checker,guard->root,place->storage.field)) continue;
            BorrowPlace index;
            int64_t number;
            if(guard->fixed ? (indexed->right && indexed->right->kind==AST_EXPR_LITERAL &&
                heap_index_constant(checker,indexed->right,&number,0) && number==guard->index) :
                (indexed->right && indexed->right->kind==AST_EXPR_NAME &&
                 expression_place(checker,indexed->right,&index) &&
                 index.owner==guard->index_owner && index.field==guard->index_field)) {
                place->guard=guard; break;
            }
        }
    const SemanticSymbol *owner=&checker->analyzer->model->symbols[place->storage.owner];
    return !owner->resolved_pointer_depth && !owner->resolved_outer_pointer_depth &&
        owner->resolved_named_symbol_id<checker->analyzer->model->symbol_count &&
        (semantic_symbol_type_properties(checker->analyzer->model,owner->resolved_named_symbol_id)&SEMANTIC_TYPE_NEEDS_DROP);
}

static int heap_condition_passive(const AstExpression *expression,unsigned depth) {
    if(depth>32) return 0;
    for(;expression;expression=expression->next)
        if(expression->kind==AST_EXPR_CALL || expression->kind==AST_EXPR_AWAIT || expression->control ||
            !heap_condition_passive(expression->left,depth+1) ||
            !heap_condition_passive(expression->right,depth+1) ||
            !heap_condition_passive(expression->arguments,depth+1)) return 0;
    return 1;
}

static void heap_guard_push(BorrowChecker *checker,const AstExpression *condition,int truth,unsigned depth) {
    if(!condition || depth>32 || condition->kind!=AST_EXPR_BINARY) return;
    if((truth && condition->operator_type==TOKEN_AMP_AMP) ||
       (!truth && condition->operator_type==TOKEN_PIPE_PIPE)) {
        if(heap_condition_passive(condition->right,0))
            heap_guard_push(checker,condition->left,truth,depth+1);
        heap_guard_push(checker,condition->right,truth,depth+1);
        return;
    }
    if(!((truth && condition->operator_type==TOKEN_EQUAL_EQUAL) ||
         (!truth && condition->operator_type==TOKEN_BANG_EQUAL)) ||
        !condition->left || condition->left->kind!=AST_EXPR_MEMBER ||
        !condition->left->left || condition->left->left->kind!=AST_EXPR_INDEX) return;
    HeapBorrowPlace root,tag;
    int64_t value;
    if(!heap_borrow_place(checker,condition->left->left,&root) ||
        !heap_borrow_place(checker,condition->left,&tag) ||
        !heap_index_constant(checker,condition->right,&value,0)) return;
    const AstExpression *index=condition->left->left->right;
    BorrowHeapGuard proof={.owner=root.storage.owner,.root=root.storage.field,.tag=tag.storage.field,
        .value=value,.valid=1,.next=checker->heap_guards};
    BorrowPlace place;
    if(index && index->kind==AST_EXPR_LITERAL && heap_index_constant(checker,index,&proof.index,0)) proof.fixed=1;
    else if(index && index->kind==AST_EXPR_NAME && expression_place(checker,index,&place))
        { proof.index_owner=place.owner; proof.index_field=place.field; }
    else return;
    BorrowHeapGuard *guard=malloc(sizeof(*guard));
    if(!guard) { checker->analyzer->allocation_failed=1; return; }
    *guard=proof; checker->heap_guards=guard;
}

static void heap_guard_pop(BorrowChecker *checker,const BorrowHeapGuard *boundary) {
    while(checker->heap_guards!=boundary) {
        BorrowHeapGuard *guard=checker->heap_guards;
        checker->heap_guards=guard->next; free(guard);
    }
}

static void heap_write(BorrowChecker *checker,const BorrowPlace *place) {
    BorrowWrite *write=malloc(sizeof(*write));
    if(!write) { checker->analyzer->allocation_failed=1; return; }
    *write=(BorrowWrite){.owner=place->owner,.field=place->field,.next=checker->writes};
    checker->writes=write;
    checker->heap_effect=1;
}

static int heap_value_has_views(BorrowChecker *checker,const AstExpression *value,unsigned depth) {
    if (!value || depth>64) return depth>64;
    if (value->resolved_borrow_kind!=AST_BORROW_NONE || value->resolved_is_slice ||
        semantic_expression_is_future(value) || (value->has_resolved_ast_type &&
        type_has_view_depth(checker,value->resolved_type_program ? value->resolved_type_program : checker->analyzer->program,
            &value->resolved_ast_type,0))) return 1;
    if (value->kind==AST_EXPR_STRUCT_LITERAL || value->kind==AST_EXPR_ENUM_CONSTRUCT)
        for (const AstExpression *field=value->arguments;field;field=field->next)
            if (heap_value_has_views(checker,field,depth+1)) return 1;
    return 0;
}

static int heap_value_tag(BorrowChecker *checker,const AstExpression *value,size_t *field,int64_t *number) {
    if(value && value->kind==AST_EXPR_STRUCT_LITERAL) {
        for(const AstExpression *member=value->arguments;member;member=member->next)
            if(member->initializer_field_symbol_id<checker->analyzer->model->symbol_count &&
                data_type_integral(member->resolved_type) && heap_index_constant(checker,member,number,0)) {
                *field=member->initializer_field_symbol_id; return 1;
            }
    }
    BorrowPlace place;
    if(!expression_place(checker,value,&place)) return 0;
    for(const BorrowIndexBound *bound=checker->index_bounds;bound;bound=bound->next)
        if(bound->valid && bound->exact && bound->owner==place.owner &&
            borrow_path_prefix(checker,place.field,bound->field)) {
            size_t leaf=borrow_path_relative(checker,bound->field,place.field);
            if(leaf<checker->analyzer->model->symbol_count &&
                checker->analyzer->model->symbols[leaf].kind==SEMANTIC_SYMBOL_FIELD) {
                *field=leaf; *number=bound->minimum; return 1;
            }
        }
    return 0;
}

static void heap_initialize(BorrowChecker *checker,const AstExpression *pointer,
                            const AstExpression *value,size_t scope_depth) {
    HeapBorrowPlace storage;
    if(!value) return;
    if (!heap_value_has_views(checker,value,0)) return;
    if (!heap_borrow_place(checker,pointer,&storage)) {
        checker->unmodeled_storage=1;
        return;
    }
    AstExpression owner={.kind=AST_EXPR_NAME,.resolved_symbol_id=storage.storage.owner};
    AstExpression field={.kind=AST_EXPR_MEMBER,.left=&owner,.resolved_symbol_id=storage.storage.field};
    BorrowRecord *boundary=checker->borrows;
    checker->aggregate_capture_depth++;
    if(!add_borrow_mode(checker,&field,value,scope_depth,1))
        clone_aggregate_borrows(checker,&field,value,scope_depth);
    checker->aggregate_capture_depth--;
    size_t tag_field=AST_SYMBOL_NONE; int64_t tag_value=0;
    int tagged=heap_value_tag(checker,value,&tag_field,&tag_value);
    if(!tagged && storage.guard && storage.guard->valid) {
        tagged=1; tag_value=storage.guard->value;
    }
    for(BorrowRecord *loan=checker->borrows;loan!=boundary;loan=loan->next) {
        loan->heap_storage=1;
        loan->heap_ring=storage.ring;
        loan->heap_head_owner=storage.head.owner;
        loan->heap_head_field=storage.head.field;
        loan->heap_mod_owner=storage.modulus.owner;
        loan->heap_mod_field=storage.modulus.field;
        loan->heap_bounded=storage.bounded;
        loan->heap_extent_owner=storage.extent.owner;
        loan->heap_extent_field=storage.extent.field;
        loan->heap_tagged=tagged;
        loan->heap_tag_field=tag_field==AST_SYMBOL_NONE && storage.guard ? storage.guard->tag :
            borrow_path_append(checker,storage.storage.field,tag_field);
        loan->heap_tag_value=tag_value;
        loan->summary_exit_active=0;
        loan->heap_anchor_owner=storage.anchored ? storage.anchor.owner:AST_SYMBOL_NONE;
        loan->heap_anchor_field=storage.anchored ? storage.anchor.field:AST_SYMBOL_NONE;
        loan->heap_min=storage.anchored ? storage.offset:INT64_MIN;
        loan->heap_max=storage.anchored ? storage.offset:INT64_MAX;
        loan->retained_until_drop=1;
        loan->captured_by_aggregate=1;
        loan->summary_effect=checker->summarizing;
        loan->scope_depth=checker->summarizing ? 0:checker->analyzer->model->symbols[storage.storage.owner].scope_depth;
    }
    heap_write(checker,&storage.storage);
}

static void heap_shift(BorrowChecker *checker,const AstStatement *statement) {
    BorrowPlace anchor;
    if(!expression_place(checker,statement->expression,&anchor)) return;
    heap_map_place(checker,&anchor);
    index_invalidate_place(checker, &anchor);
    int unit_shift = (statement->assignment_operator == TOKEN_PLUS_EQUAL ||
        statement->assignment_operator == TOKEN_MINUS_EQUAL) && statement->value &&
        statement->value->kind == AST_EXPR_LITERAL &&
        !strcmp(ast_program_lexeme(checker->analyzer->program, statement->value->value_token), "1");
    int64_t delta=statement->assignment_operator==TOKEN_PLUS_EQUAL ? -1:1;
    for(BorrowRecord *loan=checker->borrows;loan;loan=loan->next) {
        if(!loan->active || !loan->heap_storage) continue;
        int relocation_extent_update=statement==loan->heap_relocation_mod_update;
        if(loan->borrower_symbol==anchor.owner &&
            borrow_paths_overlap(checker,anchor.field,loan->heap_tag_field)) loan->heap_tagged=0;
        if((loan->heap_extent_owner==anchor.owner && !relocation_extent_update &&
            borrow_paths_overlap(checker,anchor.field,loan->heap_extent_field)) ||
            (loan->borrower_symbol==anchor.owner &&
             borrow_path_prefix(checker,anchor.field,loan->borrower_field_symbol))) loan->heap_bounded=0;
        if (relocation_extent_update && !loan->heap_ring) loan->heap_relocation_mod_update=NULL;
        int ring_change=loan->heap_ring &&
            ((loan->heap_head_owner==anchor.owner && loan->heap_head_field==anchor.field) ||
             (loan->heap_mod_owner==anchor.owner && loan->heap_mod_field==anchor.field));
        if (ring_change) {
            if (statement==loan->heap_relocation_head_update || statement==loan->heap_relocation_mod_update) {
                if (statement==loan->heap_relocation_head_update) loan->heap_relocation_head_update=NULL;
                if (statement==loan->heap_relocation_mod_update) loan->heap_relocation_mod_update=NULL;
                continue;
            }
            const AstExpression *value=statement->value;
            BorrowPlace head, modulus;
            int64_t one;
            int advance=statement->assignment_operator==TOKEN_EQUAL && value && value->kind==AST_EXPR_BINARY &&
                value->operator_type==TOKEN_PERCENT && value->left && value->left->kind==AST_EXPR_BINARY &&
                value->left->operator_type==TOKEN_PLUS &&
                heap_index_constant(checker,value->left->right,&one,0) && one==1 &&
                expression_place(checker,value->left->left,&head) && expression_place(checker,value->right,&modulus);
            int retreat=0;
            if (!advance && statement->assignment_operator==TOKEN_EQUAL && value &&
                value->kind==AST_EXPR_BINARY && value->operator_type==TOKEN_PERCENT &&
                value->left && value->left->kind==AST_EXPR_BINARY && value->left->operator_type==TOKEN_MINUS &&
                heap_index_constant(checker,value->left->right,&one,0) && one==1 &&
                value->left->left && value->left->left->kind==AST_EXPR_BINARY &&
                value->left->left->operator_type==TOKEN_PLUS &&
                expression_place(checker,value->left->left->left,&head) &&
                expression_place(checker,value->right,&modulus)) {
                BorrowPlace added;
                if (expression_place(checker,value->left->left->right,&added)) {
                    heap_map_place(checker,&added); heap_map_place(checker,&modulus);
                    retreat=added.owner==modulus.owner && added.field==modulus.field;
                    advance=retreat;
                }
            }
            if (advance) {
                heap_map_place(checker,&head); heap_map_place(checker,&modulus);
                advance=head.owner==loan->heap_head_owner && head.field==loan->heap_head_field &&
                    modulus.owner==loan->heap_mod_owner && modulus.field==loan->heap_mod_field &&
                    anchor.owner==head.owner && anchor.field==head.field;
            }
            if (!advance) { loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX; }
            else {
                int64_t change=retreat ? 1 : -1;
                if(loan->heap_min!=INT64_MIN && loan->heap_min!=INT64_MAX) loan->heap_min+=change;
                if(loan->heap_max!=INT64_MAX && loan->heap_max!=INT64_MIN) loan->heap_max+=change;
            }
            BorrowPlace storage={.owner=loan->borrower_symbol,.field=loan->borrower_field_symbol};
            heap_write(checker,&storage);
        }
        if (loan->heap_anchor_owner!=anchor.owner || loan->heap_anchor_field!=anchor.field) continue;
        if (!unit_shift) { loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX; }
        else {
            if(loan->heap_min!=INT64_MIN && loan->heap_min!=INT64_MAX) loan->heap_min+=delta;
            if(loan->heap_max!=INT64_MIN && loan->heap_max!=INT64_MAX) loan->heap_max+=delta;
        }
        BorrowPlace storage={.owner=loan->borrower_symbol,.field=loan->borrower_field_symbol};
        heap_write(checker,&storage);
    }
}

static int heap_ring_prefix(const BorrowChecker *checker, const BorrowRecord *loan) {
    if (!loan->heap_ring) return 1;
    if (loan->heap_min==INT64_MIN || loan->heap_min>=0 || loan->heap_max>=0) return 0;
    BorrowPlace count={.owner=loan->heap_anchor_owner,.field=loan->heap_anchor_field};
    int64_t minimum; int exact;
    index_bound_value(checker,&count,&minimum,&exact);
    return minimum>=-loan->heap_min;
}

static int heap_take(BorrowChecker *checker,const AstExpression *target,
                     const AstExpression *pointer,size_t scope_depth) {
    HeapBorrowPlace source;
    BorrowPlace destination;
    if(!heap_borrow_place(checker,pointer,&source) || !expression_place(checker,target,&destination)) return 0;
    int found=0;
    BorrowRecord *boundary=checker->borrows;
    for(BorrowRecord *loan=boundary;loan;loan=loan->next) {
        if(!loan->active || !loan->heap_storage || loan->borrower_symbol!=source.storage.owner ||
           !borrow_path_prefix(checker,source.storage.field,loan->borrower_field_symbol)) continue;
        int64_t selected = source.offset;
        int same=source.anchored && loan->heap_anchor_owner==source.anchor.owner &&
            loan->heap_anchor_field==source.anchor.field;
        if (source.anchored && !same) {
            BorrowPlace loan_anchor = {.owner=loan->heap_anchor_owner,.field=loan->heap_anchor_field};
            int64_t source_count, loan_count; int source_exact, loan_exact;
            index_bound_value(checker, &source.anchor, &source_count, &source_exact);
            index_bound_value(checker, &loan_anchor, &loan_count, &loan_exact);
            if (source.anchor.owner == AST_SYMBOL_NONE) { source_count=0; source_exact=1; }
            if (loan_anchor.owner == AST_SYMBOL_NONE) { loan_count=0; loan_exact=1; }
            if (source_exact && loan_exact && selected >= -source_count && selected <= INT64_MAX-source_count) {
                selected += source_count;
                selected -= loan_count;
                same=1;
            }
        }
        int coordinates=source.ring==loan->heap_ring;
        if (source.ring && loan->heap_ring)
            coordinates=source.head.owner==loan->heap_head_owner && source.head.field==loan->heap_head_field &&
                source.modulus.owner==loan->heap_mod_owner && source.modulus.field==loan->heap_mod_field;
        /* A direct head slot is logical index zero in a normalized ring. */
        if (!source.ring && loan->heap_ring && source.anchored && source.offset==0 &&
            source.anchor.owner==loan->heap_head_owner && source.anchor.field==loan->heap_head_field) {
            BorrowPlace count={.owner=loan->heap_anchor_owner,.field=loan->heap_anchor_field};
            int64_t minimum; int exact;
            index_bound_value(checker,&count,&minimum,&exact);
            if (exact) { selected=-minimum; same=1; coordinates=1; }
        }
        same &= coordinates;
        if(same && (loan->heap_min>selected || loan->heap_max<selected)) {
            if (!loan->heap_ring) continue;
            BorrowPlace modulus={.owner=loan->heap_mod_owner,.field=loan->heap_mod_field};
            int64_t minimum; int exact;
            index_bound_value(checker,&modulus,&minimum,&exact);
            if (heap_ring_prefix(checker,loan) && loan->heap_min!=INT64_MIN && loan->heap_max!=INT64_MAX && minimum>0 &&
                selected>INT64_MIN+minimum && selected<INT64_MAX-minimum &&
                loan->heap_min>selected-minimum && loan->heap_max<selected+minimum) continue;
            same=0;
        }
        BorrowRecord *copy=malloc(sizeof(*copy));
        if(!copy) { checker->analyzer->allocation_failed=1; return 1; }
        *copy=*loan;
        copy->borrower_symbol=destination.owner;
        copy->borrower_field_symbol=borrow_path_append(checker,destination.field,
            borrow_path_relative(checker,loan->borrower_field_symbol,source.storage.field));
        copy->heap_storage=0;
        copy->summary_foreign=0;
        copy->summary_effect=0;
        copy->summary_exit_active=0;
        copy->scope_depth=scope_depth;
        copy->next=checker->borrows;
        checker->borrows=copy;
        if (same && !checker->reserving_loans) {
            if (loan->heap_min==selected && loan->heap_max==selected) loan->active=0;
            else if (loan->heap_max==selected && selected>INT64_MIN) loan->heap_max=selected-1;
            else if (loan->heap_min==selected && selected<INT64_MAX) loan->heap_min=selected+1;
        }
        found=1;
    }
    if(found && !checker->reserving_loans) heap_write(checker,&source.storage);
    return found;
}

static int heap_index_frozen(BorrowChecker *checker, const AstExpression *value, const BorrowPlace *counter,
                             unsigned depth) {
    if (!value || depth>32) return 0;
    if (value->kind == AST_EXPR_BINARY)
        return heap_index_frozen(checker,value->left,counter,depth+1) &&
               heap_index_frozen(checker,value->right,counter,depth+1);
    BorrowPlace place;
    if (expression_place(checker,value,&place)) {
        heap_map_place(checker,&place);
        if (place.owner == counter->owner && borrow_paths_overlap(checker,place.field,counter->field)) return 0;
    }
    if (value->kind == AST_EXPR_NAME)
        for (const BorrowIndexConstant *binding=checker->index_constants; binding; binding=binding->next)
            if (binding->symbol == value->resolved_symbol_id) return binding->valid;
    int64_t constant;
    if (heap_index_constant(checker,value,&constant,0)) return 1;
    if (!expression_place(checker,value,&place)) return 0;
    heap_map_place(checker,&place);
    return place.owner != counter->owner || !borrow_paths_overlap(checker,place.field,counter->field);
}

/* Prove a complete countdown traversal: each iteration decrements the same
   bound and destroys that indexed slot unconditionally. Type/package/method
   names do not participate in this proof. */
static int heap_drain(BorrowChecker *checker,const AstStatement *loop,const FutureLoanBranch *entry,
                       const HeapBorrowPlace *limit) {
    if(loop->kind!=AST_STMT_WHILE || !loop->condition || loop->condition->kind!=AST_EXPR_BINARY ||
       loop->condition->operator_type!=TOKEN_GREATER ||
       !limit->anchored) return 0;
    if (loop->condition->left->resolved_type != TYPE_USIZE &&
        loop->condition->left->resolved_type != TYPE_U64) return 0;
    BorrowPlace anchor;
    if(!expression_place(checker,loop->condition->left,&anchor)) return 0;
    heap_map_place(checker,&anchor);
    if (!limit->frozen) return 0;
    int complete = limit->anchor.owner == AST_SYMBOL_NONE && limit->offset == 0;
    int64_t amount = 0;
    if (limit->anchor.owner == anchor.owner && limit->anchor.field == anchor.field && limit->offset < 0 &&
        limit->offset != INT64_MIN && (limit->offset == -1 || limit->minimum >= -limit->offset)) amount=-limit->offset;
    else if (limit->anchor.owner == AST_SYMBOL_NONE && limit->exact &&
        limit->minimum > limit->offset && limit->offset >= 0) amount=limit->minimum-limit->offset;
    int tail = amount > 0;
    int bounded = limit->anchor.owner == AST_SYMBOL_NONE && limit->offset >= 0 && limit->minimum > limit->offset;
    if (!complete && !tail && !bounded) return 0;
    const AstStatement *body=loop->body;
    if(body && body->kind==AST_STMT_BLOCK) {
        if (body->next || body->result) return 0;
        body=body->body;
    }
    if(!body || body->kind!=AST_STMT_ASSIGNMENT || body->assignment_operator!=TOKEN_MINUS_EQUAL ||
       !body->value || body->value->kind!=AST_EXPR_LITERAL ||
       strcmp(ast_program_lexeme(checker->analyzer->program,body->value->value_token),"1")) return 0;
    BorrowPlace update;
    if(!expression_place(checker,body->expression,&update)) return 0;
    heap_map_place(checker,&update);
    if(update.owner!=anchor.owner || update.field!=anchor.field) return 0;
    const AstStatement *drop=body->next;
    if(!drop || drop->next || drop->kind!=AST_STMT_EXPRESSION ||
       !drop->expression || drop->expression->kind!=AST_EXPR_CALL) return 0;
    const AstExpression *call=drop->expression;
    if(!heap_forwarded_operation(checker,call,LIFETIME_DESTROY)) return 0;
    HeapBorrowPlace storage;
    if(!heap_borrow_place(checker,call->arguments,&storage) || storage.ring || !storage.anchored || storage.offset!=0 ||
       storage.anchor.owner!=anchor.owner || storage.anchor.field!=anchor.field) return 0;
    for (size_t i=0; i<entry->count; i++) {
        BorrowRecord *loan=entry->state[i].loan;
        if (entry->state[i].active && loan->active && loan->heap_storage &&
            loan->borrower_symbol==storage.storage.owner &&
            borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol) &&
            (loan->heap_ring || loan->heap_anchor_owner!=anchor.owner ||
             loan->heap_anchor_field!=anchor.field || entry->state[i].heap_max>=0)) {
            loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX;
        }
        if(loan->heap_storage && !loan->heap_ring && loan->borrower_symbol==storage.storage.owner &&
           borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol) &&
           loan->heap_anchor_owner==anchor.owner && loan->heap_anchor_field==anchor.field &&
           entry->state[i].active && entry->state[i].heap_max<0) {
            if (complete || (tail && entry->state[i].heap_min >= -amount) ||
                (bounded && entry->state[i].heap_min >= limit->offset-limit->minimum)) loan->active=0;
            else if (tail) {
                loan->heap_min = entry->state[i].heap_min == INT64_MIN ? INT64_MIN : entry->state[i].heap_min + amount;
                loan->heap_max = entry->state[i].heap_max >= -amount ? -1 : entry->state[i].heap_max + amount;
                loan->active = 1;
            } else {
                loan->heap_min=INT64_MIN; loan->heap_max=-1;
                loan->active=1;
            }
        }
    }
    heap_write(checker,&storage.storage);
    return 1;
}

/* Recognize only a direct intrinsic or a one-statement parameter forwarding
   wrapper. Arbitrary function bodies never count as a relocation primitive. */
static int heap_forwarded_operation(BorrowChecker *checker, const AstExpression *call, AstLifetimeOperation operation) {
    if (!call || call->kind != AST_EXPR_CALL) return 0;
    if (call->lifetime_operation == operation) return 1;
    if (call->resolved_symbol_id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION || !function->declaration ||
        function->declaration->as.function.is_async) return 0;
    const AstStatement *body = function->declaration->as.function.body;
    if (body && body->kind == AST_STMT_BLOCK && !body->next) body=body->body;
    if (!body || body->next || body->kind != AST_STMT_EXPRESSION || !body->expression ||
        body->expression->lifetime_operation != operation) return 0;
    const AstParameter *parameter = function->declaration->as.function.parameters;
    const AstExpression *actual = call->arguments;
    const AstExpression *forwarded = body->expression->arguments;
    for (; parameter && actual && forwarded; parameter=parameter->next, actual=actual->next, forwarded=forwarded->next)
        if (forwarded->kind != AST_EXPR_NAME || forwarded->resolved_symbol_id != parameter->resolved_symbol_id) return 0;
    return !parameter && !actual && !forwarded;
}

static int heap_indexed_by(const AstExpression *expression,size_t symbol) {
    if(expression && expression->kind==AST_EXPR_UNARY && expression->operator_type==TOKEN_AMPERSAND)
        expression=expression->right;
    while(expression && expression->kind==AST_EXPR_MEMBER) expression=expression->left;
    return expression && expression->kind==AST_EXPR_INDEX && expression->right &&
        expression->right->kind==AST_EXPR_NAME && expression->right->resolved_symbol_id==symbol;
}

/* A tagged heap value is live only under its preserved discriminant. Visit
   the entire proved extent and destroy the whole value under that exact tag. */
static int heap_tagged_drain(BorrowChecker *checker,const AstStatement *loop,const FutureLoanBranch *entry) {
    if(loop->kind!=AST_STMT_FOR || !loop->condition || loop->condition->kind!=AST_EXPR_BINARY ||
        loop->condition->operator_type!=TOKEN_LESS || !loop->condition->left ||
        loop->condition->left->kind!=AST_EXPR_NAME || loop->condition->left->resolved_type!=TYPE_USIZE) return 0;
    size_t cursor=loop->condition->left->resolved_symbol_id;
    const AstStatement *initial=loop->initializer,*update=loop->else_body;
    int64_t number;
    BorrowPlace extent;
    if(!initial || initial->next || initial->kind!=AST_STMT_VARIABLE || initial->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,initial->value,&number,0) || number!=0 ||
        !update || update->next || update->kind!=AST_STMT_ASSIGNMENT || update->assignment_operator!=TOKEN_PLUS_EQUAL ||
        !update->expression || update->expression->kind!=AST_EXPR_NAME || update->expression->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,update->value,&number,0) || number!=1 ||
        !expression_place(checker,loop->condition->right,&extent)) return 0;
    heap_map_place(checker,&extent);
    const AstStatement *guard=loop->body;
    if(guard && guard->kind==AST_STMT_BLOCK) {
        if(guard->next || guard->result) return 0;
        guard=guard->body;
    }
    if(!guard || guard->kind!=AST_STMT_IF || guard->else_body || !guard->condition ||
        guard->condition->kind!=AST_EXPR_BINARY || guard->condition->operator_type!=TOKEN_EQUAL_EQUAL ||
        !guard->condition->left || guard->condition->left->kind!=AST_EXPR_MEMBER ||
        !heap_indexed_by(guard->condition->left,cursor) ||
        !heap_index_constant(checker,guard->condition->right,&number,0)) return 0;
    const AstStatement *drop=guard->body;
    if(drop && drop->kind==AST_STMT_BLOCK) {
        if(drop->next || drop->result) return 0;
        drop=drop->body;
    }
    if(!drop || drop->next || drop->kind!=AST_STMT_EXPRESSION ||
        !heap_forwarded_operation(checker,drop->expression,LIFETIME_DESTROY) ||
        !heap_indexed_by(drop->expression->arguments,cursor)) return 0;
    HeapBorrowPlace storage,tag;
    if(!heap_borrow_place(checker,drop->expression->arguments,&storage) ||
        !heap_borrow_place(checker,guard->condition->left,&tag) ||
        tag.storage.owner!=storage.storage.owner ||
        tag.storage.field!=borrow_path_append(checker,storage.storage.field,guard->condition->left->resolved_symbol_id)) return 0;
    const AstStatement *reset=guard->next;
    if(reset) {
        HeapBorrowPlace target;
        if(reset->next || reset->kind!=AST_STMT_ASSIGNMENT || reset->assignment_operator!=TOKEN_EQUAL ||
            !reset->expression || reset->expression->kind!=AST_EXPR_MEMBER ||
            !heap_indexed_by(reset->expression,cursor) ||
            !heap_borrow_place(checker,reset->expression,&target) ||
            target.storage.owner!=tag.storage.owner || target.storage.field!=tag.storage.field ||
            !heap_index_constant(checker,reset->value,&number,0)) return 0;
    }
    if(!heap_index_constant(checker,guard->condition->right,&number,0)) return 0;
    int found=0;
    for(size_t i=0;i<entry->count;i++) {
        BorrowRecord *loan=entry->state[i].loan;
        if(entry->state[i].active && entry->state[i].heap_bounded && entry->state[i].heap_tagged &&
            loan->heap_storage && loan->borrower_symbol==storage.storage.owner &&
            borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol) &&
            loan->heap_extent_owner==extent.owner && loan->heap_extent_field==extent.field &&
            loan->heap_tag_field==tag.storage.field && loan->heap_tag_value==number) { loan->active=0; found=1; }
    }
    if(found) {
        heap_write(checker,&storage.storage);
        for(BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
            if(loan->active && loan->heap_storage && loan->borrower_symbol==storage.storage.owner &&
                borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol)) {
                loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX;
            }
    }
    return found;
}

static int heap_ring_drain(BorrowChecker *checker, const AstStatement *loop, const FutureLoanBranch *entry,
                           const HeapBorrowPlace *limit) {
    if (loop->kind!=AST_STMT_WHILE || !loop->condition || loop->condition->kind!=AST_EXPR_BINARY ||
        loop->condition->operator_type!=TOKEN_GREATER || !loop->condition->left ||
        loop->condition->left->resolved_type!=TYPE_USIZE) return 0;
    int64_t zero;
    BorrowPlace count;
    if (!heap_index_constant(checker,loop->condition->right,&zero,0) || zero!=0 ||
        !expression_place(checker,loop->condition->left,&count)) return 0;
    heap_map_place(checker,&count);
    const AstStatement *drop=loop->body;
    if (drop && drop->kind==AST_STMT_BLOCK) {
        if (drop->next || drop->result) return 0;
        drop=drop->body;
    }
    if (!drop || drop->kind!=AST_STMT_EXPRESSION ||
        !heap_forwarded_operation(checker,drop->expression,LIFETIME_DESTROY)) return 0;
    HeapBorrowPlace storage;
    if (!heap_borrow_place(checker,drop->expression->arguments,&storage) || storage.ring ||
        !storage.anchored || storage.offset!=0) return 0;
    const AstStatement *advance=drop->next;
    const AstStatement *decrement=advance ? advance->next : NULL;
    if (!advance || advance->kind!=AST_STMT_ASSIGNMENT || advance->assignment_operator!=TOKEN_EQUAL ||
        !decrement || decrement->next || decrement->kind!=AST_STMT_ASSIGNMENT ||
        decrement->assignment_operator!=TOKEN_MINUS_EQUAL) return 0;
    const AstExpression *value=advance->value;
    BorrowPlace head, previous, modulus, bound;
    int64_t one;
    if (!value || value->kind!=AST_EXPR_BINARY || value->operator_type!=TOKEN_PERCENT ||
        value->resolved_type!=TYPE_USIZE || !value->left || value->left->kind!=AST_EXPR_BINARY ||
        value->left->operator_type!=TOKEN_PLUS ||
        !heap_index_constant(checker,value->left->right,&one,0) || one!=1 ||
        !heap_index_constant(checker,decrement->value,&one,0) || one!=1 ||
        !expression_place(checker,advance->expression,&head) ||
        !expression_place(checker,value->left->left,&previous) ||
        !expression_place(checker,value->right,&modulus) ||
        !expression_place(checker,decrement->expression,&bound)) return 0;
    heap_map_place(checker,&head); heap_map_place(checker,&previous);
    heap_map_place(checker,&modulus); heap_map_place(checker,&bound);
    if (head.owner!=previous.owner || head.field!=previous.field ||
        head.owner!=storage.anchor.owner || head.field!=storage.anchor.field ||
        bound.owner!=count.owner || bound.field!=count.field ||
        (head.owner==count.owner && borrow_paths_overlap(checker,head.field,count.field)) ||
        (modulus.owner==head.owner && borrow_paths_overlap(checker,modulus.field,head.field)) ||
        (modulus.owner==count.owner && borrow_paths_overlap(checker,modulus.field,count.field))) return 0;
    int found=0;
    for (size_t i=0; i<entry->count; i++) {
        BorrowRecord *loan=entry->state[i].loan;
        if (entry->state[i].active && loan->heap_storage && loan->heap_ring &&
            loan->borrower_symbol==storage.storage.owner &&
            borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol) &&
            loan->heap_anchor_owner==count.owner && loan->heap_anchor_field==count.field &&
            loan->heap_head_owner==head.owner && loan->heap_head_field==head.field &&
            loan->heap_mod_owner==modulus.owner && loan->heap_mod_field==modulus.field &&
            entry->state[i].heap_min!=INT64_MIN && entry->state[i].heap_max<0 &&
            limit->minimum>=-entry->state[i].heap_min) { loan->active=0; found=1; }
    }
    if (found) heap_write(checker,&storage.storage);
    if (found)
        for (BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
            if (loan->active && loan->heap_storage && loan->borrower_symbol==storage.storage.owner &&
                borrow_path_prefix(checker,storage.storage.field,loan->borrower_field_symbol)) {
                loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX;
            }
    return found;
}

/* Transport a proved occupied prefix to another allocation without merging
   the referents of individual slots. Raw allocation validity remains the
   owner's responsibility; the proof concerns initialize/take and the range. */
static int heap_relocate_storage(BorrowChecker *checker, const AstStatement *loop) {
    if (!checker->summarizing || loop->kind!=AST_STMT_FOR || !loop->condition ||
        loop->condition->kind!=AST_EXPR_BINARY || loop->condition->operator_type!=TOKEN_LESS ||
        !loop->condition->left || loop->condition->left->kind!=AST_EXPR_NAME ||
        loop->condition->left->resolved_type!=TYPE_USIZE) return 0;
    size_t cursor=loop->condition->left->resolved_symbol_id;
    const AstStatement *initial=loop->initializer, *update=loop->else_body;
    int64_t number, offset;
    BorrowPlace count;
    if (!initial || initial->next || initial->kind!=AST_STMT_VARIABLE || initial->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,initial->value,&number,0) || number!=0 ||
        !update || update->next || update->kind!=AST_STMT_ASSIGNMENT || update->assignment_operator!=TOKEN_PLUS_EQUAL ||
        !update->expression || update->expression->kind!=AST_EXPR_NAME || update->expression->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,update->value,&number,0) || number!=1 ||
        !heap_index_place(checker,loop->condition->right,&count,&offset,0) ||
        (count.owner!=AST_SYMBOL_NONE && offset!=0)) return 0;
    const AstStatement *body=loop->body;
    if (body && body->kind==AST_STMT_BLOCK) {
        if (body->next || body->result) return 0;
        body=body->body;
    }
    if (!body || body->next || body->kind!=AST_STMT_EXPRESSION ||
        !heap_forwarded_operation(checker,body->expression,LIFETIME_INITIALIZE)) return 0;
    const AstExpression *pointer=body->expression->arguments, *value=pointer ? pointer->next : NULL;
    if (!value || value->next || value->lifetime_operation!=LIFETIME_TAKE ||
        !heap_indexed_by(pointer,cursor)) return 0;
    /* Whole slots only: member moves require a separate coverage proof. */
    const AstExpression *dst=pointer, *src=value->arguments;
    if (dst->kind==AST_EXPR_UNARY && dst->operator_type==TOKEN_AMPERSAND) dst=dst->right;
    if (src->kind==AST_EXPR_UNARY && src->operator_type==TOKEN_AMPERSAND) src=src->right;
    HeapBorrowPlace source;
    BorrowPlace destination;
    if (src->kind!=AST_EXPR_INDEX || dst->kind!=AST_EXPR_INDEX ||
        !heap_borrow_place(checker,value->arguments,&source) ||
        !expression_place(checker,dst->left,&destination)) return 0;
    const AstStatement *head_update=NULL, *mod_update=NULL;
    if (source.ring) {
        const AstExpression *index=src->right;
        if (!index || index->kind!=AST_EXPR_BINARY || index->operator_type!=TOKEN_PERCENT ||
            !index->left || index->left->kind!=AST_EXPR_BINARY || index->left->operator_type!=TOKEN_PLUS ||
            !index->left->right || index->left->right->kind!=AST_EXPR_NAME ||
            index->left->right->resolved_symbol_id!=cursor) return 0;
        /* Normalize the transported logical prefix only for the exact
           subsequent pointer/capacity/head commit sequence. */
        const AstStatement *commit=loop->next;
        if (commit && commit->kind==AST_STMT_EXPRESSION) commit=commit->next;
        mod_update=commit ? commit->next : NULL;
        head_update=mod_update ? mod_update->next : NULL;
        BorrowPlace place, fresh, modulus, head;
        if (!commit || commit->kind!=AST_STMT_ASSIGNMENT || commit->assignment_operator!=TOKEN_EQUAL ||
            !expression_place(checker,commit->expression,&place) ||
            !expression_place(checker,commit->value,&fresh) ||
            !mod_update || mod_update->kind!=AST_STMT_ASSIGNMENT || mod_update->assignment_operator!=TOKEN_EQUAL ||
            !expression_place(checker,mod_update->expression,&modulus) ||
            !head_update || head_update->kind!=AST_STMT_ASSIGNMENT || head_update->assignment_operator!=TOKEN_EQUAL ||
            !expression_place(checker,head_update->expression,&head) ||
            !heap_index_constant(checker,head_update->value,&number,0) || number!=0) return 0;
        heap_map_place(checker,&place); heap_map_place(checker,&fresh);
        heap_map_place(checker,&modulus); heap_map_place(checker,&head);
        if (place.owner!=source.storage.owner || place.field!=source.storage.field ||
            fresh.owner!=destination.owner || fresh.field!=destination.field ||
            modulus.owner!=source.modulus.owner || modulus.field!=source.modulus.field ||
            head.owner!=source.head.owner || head.field!=source.head.field) return 0;
    } else if (!heap_indexed_by(value->arguments,cursor)) return 0;
    heap_map_place(checker,&destination);
    /* Scratch storage belongs to this invocation. Do not export a write to
       its declaration as a caller-visible effect or reuse it on a later call. */
    if (destination.owner>=checker->analyzer->model->symbol_count || destination.field!=AST_SYMBOL_NONE ||
        checker->analyzer->model->symbols[destination.owner].kind!=SEMANTIC_SYMBOL_LOCAL) return 0;
    if (source.storage.owner==destination.owner &&
        borrow_paths_overlap(checker,source.storage.field,destination.field)) return 0;
    int64_t minimum=0; int exact=0;
    if (count.owner==AST_SYMBOL_NONE) {
        minimum=offset;
    } else index_bound_value(checker,&count,&minimum,&exact);
    int found=0;
    BorrowRecord *boundary=checker->borrows;
    for (BorrowRecord *loan=boundary;loan;loan=loan->next) {
        if (!loan->active || !loan->heap_storage || loan->borrower_symbol!=source.storage.owner ||
            !borrow_path_prefix(checker,source.storage.field,loan->borrower_field_symbol)) continue;
        if (loan->heap_ring!=source.ring || loan->heap_min==INT64_MIN || loan->heap_max==INT64_MAX) return 0;
        if (source.ring && (loan->heap_head_owner!=source.head.owner || loan->heap_head_field!=source.head.field ||
            loan->heap_mod_owner!=source.modulus.owner || loan->heap_mod_field!=source.modulus.field)) return 0;
        if (loan->heap_anchor_owner==AST_SYMBOL_NONE) {
            if (loan->heap_min<0 || loan->heap_max>=minimum) return 0;
        } else if (loan->heap_anchor_owner!=count.owner || loan->heap_anchor_field!=count.field ||
                   loan->heap_max>=0 || minimum < -loan->heap_min) return 0;
        found=1;
    }
    if (!found) return 0;
    for (BorrowRecord *loan=boundary;loan;loan=loan->next) {
        if (!loan->active || !loan->heap_storage || loan->borrower_symbol!=source.storage.owner ||
            !borrow_path_prefix(checker,source.storage.field,loan->borrower_field_symbol)) continue;
        BorrowRecord *copy=malloc(sizeof(*copy));
        if (!copy) { checker->analyzer->allocation_failed=1; return 1; }
        *copy=*loan;
        copy->borrower_symbol=destination.owner;
        copy->borrower_field_symbol=borrow_path_append(checker,destination.field,
            borrow_path_relative(checker,loan->borrower_field_symbol,source.storage.field));
        copy->heap_bounded=0; copy->heap_tagged=0;
        copy->heap_relocation_head_update=head_update;
        copy->heap_relocation_mod_update=mod_update;
        copy->summary_foreign=0; copy->summary_effect=1; copy->summary_exit_active=0;
        copy->scope_depth=checker->current_scope_depth;
        copy->next=checker->borrows; checker->borrows=copy;
        loan->active=0;
    }
    heap_write(checker,&source.storage);
    BorrowPlace induction={.owner=cursor,.field=AST_SYMBOL_NONE};
    index_invalidate_place(checker,&induction);
    return 1;
}

/* Scan every old tagged slot and move it to a bounded probing coordinate in
   scratch storage. The key/hash callback is checked before trusting the tag. */
static int heap_rehash_storage(BorrowChecker *checker, const AstStatement *loop) {
    if (!checker->summarizing || loop->kind!=AST_STMT_FOR || !loop->condition ||
        loop->condition->kind!=AST_EXPR_BINARY || loop->condition->operator_type!=TOKEN_LESS ||
        !loop->condition->left || loop->condition->left->kind!=AST_EXPR_NAME ||
        loop->condition->left->resolved_type!=TYPE_USIZE) return 0;
    size_t cursor=loop->condition->left->resolved_symbol_id;
    const AstStatement *initial=loop->initializer, *update=loop->else_body;
    int64_t number, tag_value;
    BorrowPlace extent;
    if (!initial || initial->next || initial->kind!=AST_STMT_VARIABLE || initial->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,initial->value,&number,0) || number!=0 ||
        !update || update->next || update->kind!=AST_STMT_ASSIGNMENT || update->assignment_operator!=TOKEN_PLUS_EQUAL ||
        !update->expression || update->expression->kind!=AST_EXPR_NAME || update->expression->resolved_symbol_id!=cursor ||
        !heap_index_constant(checker,update->value,&number,0) || number!=1 ||
        !expression_place(checker,loop->condition->right,&extent)) return 0;
    heap_map_place(checker,&extent);
    const AstStatement *guard=loop->body;
    if (guard && guard->kind==AST_STMT_BLOCK) {
        if (guard->next || guard->result) return 0;
        guard=guard->body;
    }
    if (!guard || guard->next || guard->kind!=AST_STMT_IF || guard->else_body || !guard->condition ||
        guard->condition->kind!=AST_EXPR_BINARY || guard->condition->operator_type!=TOKEN_EQUAL_EQUAL ||
        !guard->condition->left || guard->condition->left->kind!=AST_EXPR_MEMBER ||
        !heap_indexed_by(guard->condition->left,cursor) ||
        !heap_index_constant(checker,guard->condition->right,&tag_value,0)) return 0;
    const AstStatement *index=guard->body;
    if (index && index->kind==AST_STMT_BLOCK) {
        if (index->next || index->result) return 0;
        index=index->body;
    }
    const AstStatement *probe=index ? index->next : NULL, *move=probe ? probe->next : NULL;
    if (!index || index->kind!=AST_STMT_VARIABLE || !index->value || index->value->kind!=AST_EXPR_BINARY ||
        index->value->operator_type!=TOKEN_PERCENT || index->value->resolved_type!=TYPE_USIZE ||
        !probe || probe->kind!=AST_STMT_WHILE || !probe->condition ||
        probe->condition->kind!=AST_EXPR_BINARY || probe->condition->operator_type!=TOKEN_EQUAL_EQUAL ||
        !probe->condition->left || probe->condition->left->kind!=AST_EXPR_MEMBER ||
        !heap_indexed_by(probe->condition->left,index->resolved_symbol_id) ||
        !heap_index_constant(checker,probe->condition->right,&number,0) || number!=tag_value ||
        !move || move->next || move->kind!=AST_STMT_EXPRESSION ||
        !heap_forwarded_operation(checker,move->expression,LIFETIME_INITIALIZE)) return 0;
    const AstStatement *step=probe->body;
    if (step && step->kind==AST_STMT_BLOCK) {
        if (step->next || step->result) return 0;
        step=step->body;
    }
    const AstExpression *advance=step ? step->value : NULL;
    BorrowPlace modulus, stepped_modulus;
    if (!step || step->next || step->kind!=AST_STMT_ASSIGNMENT || step->assignment_operator!=TOKEN_EQUAL ||
        !step->expression || step->expression->kind!=AST_EXPR_NAME ||
        step->expression->resolved_symbol_id!=index->resolved_symbol_id ||
        !advance || advance->kind!=AST_EXPR_BINARY || advance->operator_type!=TOKEN_PERCENT ||
        !advance->left || advance->left->kind!=AST_EXPR_BINARY || advance->left->operator_type!=TOKEN_PLUS ||
        !advance->left->left || advance->left->left->kind!=AST_EXPR_NAME ||
        advance->left->left->resolved_symbol_id!=index->resolved_symbol_id ||
        !heap_index_constant(checker,advance->left->right,&number,0) || number!=1 ||
        !expression_place(checker,index->value->right,&modulus) ||
        !expression_place(checker,advance->right,&stepped_modulus)) return 0;
    heap_map_place(checker,&modulus); heap_map_place(checker,&stepped_modulus);
    if (modulus.owner!=stepped_modulus.owner || modulus.field!=stepped_modulus.field) return 0;
    const AstExpression *pointer=move->expression->arguments, *value=pointer ? pointer->next : NULL;
    if (!value || value->next || value->lifetime_operation!=LIFETIME_TAKE ||
        !heap_indexed_by(value->arguments,cursor) || !heap_indexed_by(pointer,index->resolved_symbol_id)) return 0;
    const AstExpression *dst=pointer, *src=value->arguments;
    if (dst->kind==AST_EXPR_UNARY && dst->operator_type==TOKEN_AMPERSAND) dst=dst->right;
    if (src->kind==AST_EXPR_UNARY && src->operator_type==TOKEN_AMPERSAND) src=src->right;
    HeapBorrowPlace source, tag;
    BorrowPlace destination, probed;
    if (src->kind!=AST_EXPR_INDEX || dst->kind!=AST_EXPR_INDEX ||
        !heap_borrow_place(checker,src,&source) || !heap_borrow_place(checker,guard->condition->left,&tag) ||
        !expression_place(checker,dst->left,&destination) ||
        !expression_place(checker,probe->condition->left->left->left,&probed)) return 0;
    heap_map_place(checker,&destination); heap_map_place(checker,&probed);
    if (destination.owner!=probed.owner || destination.field!=probed.field ||
        destination.owner>=checker->analyzer->model->symbol_count || destination.field!=AST_SYMBOL_NONE ||
        checker->analyzer->model->symbols[destination.owner].kind!=SEMANTIC_SYMBOL_LOCAL ||
        tag.storage.owner!=source.storage.owner ||
        tag.storage.field!=borrow_path_append(checker,source.storage.field,guard->condition->left->resolved_symbol_id) ||
        probe->condition->left->resolved_symbol_id!=guard->condition->left->resolved_symbol_id) return 0;
    const AstStatement *commit=loop->next;
    if (commit && commit->kind==AST_STMT_EXPRESSION) commit=commit->next;
    const AstStatement *resize=commit ? commit->next : NULL;
    BorrowPlace target, fresh, resized, size;
    if (!commit || commit->kind!=AST_STMT_ASSIGNMENT || commit->assignment_operator!=TOKEN_EQUAL ||
        !expression_place(checker,commit->expression,&target) || !expression_place(checker,commit->value,&fresh) ||
        !resize || resize->kind!=AST_STMT_ASSIGNMENT || resize->assignment_operator!=TOKEN_EQUAL ||
        !expression_place(checker,resize->expression,&resized) || !expression_place(checker,resize->value,&size)) return 0;
    heap_map_place(checker,&target); heap_map_place(checker,&fresh);
    heap_map_place(checker,&resized); heap_map_place(checker,&size);
    if (target.owner!=source.storage.owner || target.field!=source.storage.field ||
        fresh.owner!=destination.owner || fresh.field!=destination.field ||
        resized.owner!=extent.owner || resized.field!=extent.field ||
        size.owner!=modulus.owner || size.field!=modulus.field) return 0;
    check_expression(checker,index->value,BORROW_ACCESS_READ);
    int found=0;
    BorrowRecord *boundary=checker->borrows;
    for (BorrowRecord *loan=boundary;loan;loan=loan->next) {
        if (!loan->active || !loan->heap_storage || loan->borrower_symbol!=source.storage.owner ||
            !borrow_path_prefix(checker,source.storage.field,loan->borrower_field_symbol)) continue;
        if (!loan->heap_bounded || !loan->heap_tagged || loan->heap_extent_owner!=extent.owner ||
            loan->heap_extent_field!=extent.field || loan->heap_tag_field!=tag.storage.field ||
            loan->heap_tag_value!=tag_value) return 0;
        found=1;
    }
    if (!found) return 0;
    for (BorrowRecord *loan=boundary;loan;loan=loan->next) {
        if (!loan->active || !loan->heap_storage || loan->borrower_symbol!=source.storage.owner ||
            !borrow_path_prefix(checker,source.storage.field,loan->borrower_field_symbol)) continue;
        BorrowRecord *copy=malloc(sizeof(*copy));
        if (!copy) { checker->analyzer->allocation_failed=1; return 1; }
        *copy=*loan;
        copy->borrower_symbol=destination.owner;
        copy->borrower_field_symbol=borrow_path_append(checker,destination.field,
            borrow_path_relative(checker,loan->borrower_field_symbol,source.storage.field));
        copy->heap_tag_field=borrow_path_append(checker,destination.field,guard->condition->left->resolved_symbol_id);
        copy->heap_ring=0; copy->heap_anchor_owner=AST_SYMBOL_NONE; copy->heap_anchor_field=AST_SYMBOL_NONE;
        copy->heap_min=INT64_MIN; copy->heap_max=INT64_MAX;
        copy->heap_relocation_mod_update=resize;
        copy->summary_foreign=0; copy->summary_effect=1; copy->summary_exit_active=0;
        copy->scope_depth=checker->current_scope_depth;
        copy->next=checker->borrows; checker->borrows=copy;
        loan->active=0;
    }
    heap_write(checker,&source.storage);
    BorrowPlace induction={.owner=cursor,.field=AST_SYMBOL_NONE};
    index_invalidate_place(checker,&induction);
    return 1;
}

/* A contiguous left relocation moves each live slot once. Its affine range
   follows the induction variable, independently of container/type names. */
static int heap_relocate_left(BorrowChecker *checker, const AstStatement *loop) {
    if (!checker->summarizing || loop->kind != AST_STMT_FOR || !loop->condition ||
        loop->condition->kind != AST_EXPR_BINARY || loop->condition->operator_type != TOKEN_LESS ||
        !loop->condition->left || loop->condition->left->kind != AST_EXPR_NAME ||
        (loop->condition->left->resolved_type != TYPE_USIZE && loop->condition->left->resolved_type != TYPE_U64)) return 0;
    const AstStatement *update = loop->else_body;
    int64_t step;
    if (!update || update->next || update->kind != AST_STMT_ASSIGNMENT ||
        update->assignment_operator != TOKEN_PLUS_EQUAL || !update->expression ||
        update->expression->kind != AST_EXPR_NAME ||
        update->expression->resolved_symbol_id != loop->condition->left->resolved_symbol_id ||
        !heap_index_constant(checker, update->value, &step, 0) || step != 1) return 0;
    const AstStatement *body=loop->body;
    if (body && body->kind == AST_STMT_BLOCK && !body->next) body=body->body;
    if (!body || body->next || body->kind != AST_STMT_EXPRESSION ||
        !heap_forwarded_operation(checker, body->expression, LIFETIME_INITIALIZE)) return 0;
    const AstExpression *pointer = body->expression->arguments;
    const AstExpression *value = pointer ? pointer->next : NULL;
    if (!value || value->next || value->lifetime_operation != LIFETIME_TAKE) return 0;
    const AstExpression *source_index = value->arguments;
    const AstExpression *destination_index = pointer;
    if (source_index && source_index->kind == AST_EXPR_UNARY && source_index->operator_type == TOKEN_AMPERSAND)
        source_index=source_index->right;
    if (destination_index && destination_index->kind == AST_EXPR_UNARY && destination_index->operator_type == TOKEN_AMPERSAND)
        destination_index=destination_index->right;
    if (!source_index || source_index->kind != AST_EXPR_INDEX || !source_index->right ||
        source_index->right->kind != AST_EXPR_NAME ||
        source_index->right->resolved_symbol_id != loop->condition->left->resolved_symbol_id ||
        !destination_index || destination_index->kind != AST_EXPR_INDEX || !destination_index->right ||
        destination_index->right->kind != AST_EXPR_BINARY || destination_index->right->operator_type != TOKEN_MINUS ||
        !destination_index->right->left || destination_index->right->left->kind != AST_EXPR_NAME ||
        destination_index->right->left->resolved_symbol_id != loop->condition->left->resolved_symbol_id ||
        !heap_index_constant(checker, destination_index->right->right, &step, 0) || step != 1) return 0;
    HeapBorrowPlace source, destination;
    BorrowPlace end; int64_t start, finish;
    if (!heap_borrow_place(checker, value->arguments, &source) || !source.anchored ||
        !heap_borrow_place(checker, pointer, &destination) || !destination.anchored ||
        source.ring || destination.ring ||
        destination.storage.owner != source.storage.owner || destination.storage.field != source.storage.field ||
        destination.anchor.owner != source.anchor.owner || destination.anchor.field != source.anchor.field ||
        source.offset == INT64_MIN || destination.offset != source.offset-1 ||
        !heap_index_place(checker, loop->condition->left, &end, &start, 0) ||
        end.owner != source.anchor.owner || end.field != source.anchor.field || start != source.offset ||
        !heap_index_place(checker, loop->condition->right, &end, &finish, 0) ||
        end.owner == AST_SYMBOL_NONE || finish != 0) return 0;
    int64_t count; int exact;
    index_bound_value(checker, &end, &count, &exact);
    if (source.anchor.owner == AST_SYMBOL_NONE) {
        if (!exact || start < 1 || start > count) return 0;
        start-=count;
    } else if (source.anchor.owner != end.owner || source.anchor.field != end.field ||
               start > 0 || start == INT64_MIN || count < 1-start) return 0;
    for (BorrowRecord *loan=checker->borrows; loan; loan=loan->next) {
        if (!loan->active || !loan->heap_storage || loan->borrower_symbol != source.storage.owner ||
            !borrow_path_prefix(checker, source.storage.field, loan->borrower_field_symbol)) continue;
        if (loan->heap_anchor_owner == AST_SYMBOL_NONE && exact && loan->heap_min>=0 && loan->heap_max<count) {
            if (loan->heap_min-count>=start) loan->heap_min--;
            if (loan->heap_max-count>=start) loan->heap_max--;
        } else if (loan->heap_anchor_owner != end.owner || loan->heap_anchor_field != end.field ||
            loan->heap_min == INT64_MIN || loan->heap_max >= 0) {
            loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX;
        } else {
            if (loan->heap_min>=start) loan->heap_min--;
            if (loan->heap_max>=start) loan->heap_max--;
        }
    }
    heap_write(checker, &source.storage);
    BorrowPlace induction = {.owner=loop->condition->left->resolved_symbol_id,.field=AST_SYMBOL_NONE};
    index_invalidate_place(checker, &induction);
    return 1;
}

/* An initialized slot strictly before an unsigned countdown bound proves that
   the bound is nonzero. Only call summaries use this fact; ordinary checking
   still validates both branches of the function body. */
static int heap_condition_value(BorrowChecker *checker, const AstExpression *condition) {
    if (checker->summarizing && condition && condition->kind == AST_EXPR_BINARY) {
        BorrowPlace left, right;
        int64_t l, r;
        if (heap_index_place(checker,condition->left,&left,&l,0) &&
            heap_index_place(checker,condition->right,&right,&r,0)) {
            BorrowPlace *places[] = {&left, &right};
            int64_t *offsets[] = {&l, &r};
            for (size_t i = 0; i < 2; i++) {
                int64_t minimum; int exact;
                index_bound_value(checker, places[i], &minimum, &exact);
                if (places[i]->owner != AST_SYMBOL_NONE && exact &&
                    *offsets[i] >= -minimum && *offsets[i] <= INT64_MAX - minimum) {
                    *offsets[i] += minimum;
                    places[i]->owner = AST_SYMBOL_NONE;
                    places[i]->field = AST_SYMBOL_NONE;
                }
            }
            if (left.owner != right.owner || left.field != right.field) goto unknown_condition;
            int ordered = left.owner == AST_SYMBOL_NONE || l == r;
            if (!ordered && ((l == -1 && r == 0) || (r == -1 && l == 0)))
                for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                    if (loan->active && loan->heap_storage && heap_ring_prefix(checker,loan) && loan->heap_anchor_owner == left.owner &&
                        loan->heap_anchor_field == left.field && loan->heap_min != INT64_MIN && loan->heap_max < 0)
                        { ordered = 1; break; }
            switch (condition->operator_type) {
                case TOKEN_EQUAL_EQUAL: return l == r;
                case TOKEN_BANG_EQUAL: return l != r;
                case TOKEN_LESS: if (ordered) return l < r; break;
                case TOKEN_LESS_EQUAL: if (ordered) return l <= r; break;
                case TOKEN_GREATER: if (ordered) return l > r; break;
                case TOKEN_GREATER_EQUAL: if (ordered) return l >= r; break;
                default: break;
            }
        }
    }
unknown_condition:
    if (!checker->summarizing || !condition || condition->kind!=AST_EXPR_BINARY ||
        (condition->operator_type!=TOKEN_EQUAL_EQUAL && condition->operator_type!=TOKEN_BANG_EQUAL) ||
        !condition->right || condition->right->kind!=AST_EXPR_LITERAL ||
        strcmp(ast_program_lexeme(checker->analyzer->program,condition->right->value_token),"0")) return -1;
    BorrowPlace anchor;
    if (!expression_place(checker,condition->left,&anchor)) return -1;
    heap_map_place(checker,&anchor);
    DataType type=condition->left->resolved_type;
    /* Narrow counters can wrap while their storage still contains loans. */
    if (type!=TYPE_USIZE && type!=TYPE_U64) return -1;
    for (const BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
        if (loan->active && loan->heap_storage && heap_ring_prefix(checker,loan) && loan->heap_anchor_owner==anchor.owner &&
            loan->heap_anchor_field==anchor.field && loan->heap_min!=INT64_MIN && loan->heap_max<0)
            return condition->operator_type==TOKEN_BANG_EQUAL;
    return -1;
}

/* Walk declared aggregate leaves using their full struct/enum payload paths. */
typedef int (*BorrowFieldVisitor)(BorrowChecker *, size_t, const AstType *, void *);

static int visit_borrow_fields(BorrowChecker *checker, const AstProgram *unit,
    const AstType *type, size_t path, BorrowFieldVisitor visitor, void *context, size_t depth) {
    if (type->borrow_kind != AST_BORROW_NONE || type->is_slice || type->kind == AST_TYPE_FUTURE)
        return visitor ? visitor(checker, path, type, context) : 1;
    if (depth > 32 || type->kind != AST_TYPE_NAMED || type->borrow_kind != AST_BORROW_NONE ||
        type->pointer_depth || type->outer_pointer_depth || type->is_slice || type->is_array) return 0;
    size_t id = resolve_named_symbol_id(checker->analyzer, unit, named_type_token(unit, type));
    if (id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *owner = &checker->analyzer->model->symbols[id];
    const AstDeclarationNode *aggregate = owner->declaration;
    if (!aggregate) return 0;
    if (aggregate->kind == AST_DECL_ENUM) {
        for (const AstEnumValue *value = aggregate->as.enum_decl.values; value; value = value->next) {
            size_t index = 0;
            for (const AstTypeArgument *payload = value->payload_types; payload; payload = payload->next, index++) {
                if (!type_has_view_depth(checker, owner->source_program, &payload->type, 0)) continue;
                size_t child = borrow_path_append(checker, path,
                    borrow_payload_path(checker, id, value->resolved_symbol_id, index));
                if (payload->type.borrow_kind != AST_BORROW_NONE) {
                    if (visitor && !visitor(checker, child, &payload->type, context)) return 0;
                } else if (!visit_borrow_fields(checker, owner->source_program, &payload->type,
                                               child, visitor, context, depth + 1)) return 0;
            }
        }
        return 1;
    }
    if (aggregate->kind != AST_DECL_STRUCT) return 0;
    for (const AstField *field = aggregate->as.struct_decl.fields; field; field = field->next) {
        if (!type_has_view_depth(checker, owner->source_program, &field->type, 0)) {
            if (semantic_declared_type_properties(checker->analyzer, owner->source_program,
                &field->type) & SEMANTIC_TYPE_MOVE_ONLY) return 0;
            continue;
        }
        size_t child = borrow_path_append(checker, path, field->resolved_symbol_id);
        if (field->type.borrow_kind != AST_BORROW_NONE) {
            if (visitor && !visitor(checker, child, &field->type, context)) return 0;
        } else if (!visit_borrow_fields(checker, owner->source_program, &field->type,
                                       child, visitor, context, depth + 1)) return 0;
    }
    return 1;
}

typedef struct {
    const AstExpression *target;
    const AstExpression *base;
    const AstProgram *unit;
    const AstType *type;
    const char *lifetime;
    AstBorrowKind kind;
    size_t scope_depth;
    BorrowRecord *first;
} ParameterFieldBorrows;

static int add_parameter_borrow_field(BorrowChecker *checker, size_t path,
                                      const AstType *type, void *context) {
    (void)type;
    ParameterFieldBorrows *fields = context;
    const char *lifetime = result_field_lifetime(checker, fields->unit, fields->type, path);
    if (!lifetime || strcmp(lifetime, fields->lifetime)) return 1;
    AstExpression input = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)fields->base,
        .resolved_symbol_id = path, .resolved_borrow_kind = fields->kind,
        .first_token = fields->base->first_token};
    BorrowRecord *loan = add_borrow_mode(checker, fields->target, &input, fields->scope_depth, 1);
    if (!fields->first) fields->first = loan;
    return 1;
}

/* A reference returned from an aggregate parameter follows the reference fields
   with the declared lifetime, rather than borrowing the aggregate's storage. */
static BorrowRecord *add_parameter_field_borrows(BorrowChecker *checker,
    const AstExpression *target, const AstExpression *argument, const AstProgram *unit,
    const AstType *parameter, const char *lifetime, AstBorrowKind kind, size_t scope_depth) {
    if (parameter->kind != AST_TYPE_NAMED || parameter->pointer_depth ||
        parameter->outer_pointer_depth) return NULL;
    AstType referent = *parameter;
    referent.borrow_kind = AST_BORROW_NONE;
    if (!visit_borrow_fields(checker, unit, &referent, AST_SYMBOL_NONE, NULL, NULL, 0)) return NULL;
    const AstExpression *base = argument;
    if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND)
        base = base->right;
    BorrowPlace storage;
    AstExpression temporary = {.kind = AST_EXPR_NAME,
        .resolved_symbol_id = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++,
        .first_token = argument->first_token};
    int materialized = !expression_place(checker, base, &storage);
    if (materialized) {
        checker->aggregate_capture_depth++;
        clone_aggregate_borrows(checker, &temporary, base, scope_depth);
        checker->aggregate_capture_depth--;
        base = &temporary;
    }
    ParameterFieldBorrows fields = {.target = target, .base = base, .unit = unit,
        .type = parameter, .lifetime = lifetime, .kind = kind, .scope_depth = scope_depth};
    visit_borrow_fields(checker, unit, &referent, AST_SYMBOL_NONE, add_parameter_borrow_field, &fields, 0);
    if (materialized) deactivate_borrower(checker, temporary.resolved_symbol_id, AST_SYMBOL_NONE);
    return fields.first;
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

/* Raw slice fields retain their existing explicit storage contract. A proved
   view of a tracked fixed array still cannot escape its array owner, even when
   the surrounding aggregate does not have checked lifetime parameters. */
static int has_array_view_origin(BorrowChecker *checker, const AstExpression *value, unsigned depth) {
    if (!value || depth>64) return 0;
    if (value->is_array_view) return 1;
    if (value->kind==AST_EXPR_STRUCT_LITERAL || value->kind==AST_EXPR_ENUM_CONSTRUCT)
        for (const AstExpression *argument=value->arguments;argument;argument=argument->next)
            if (has_array_view_origin(checker,argument,depth+1)) return 1;
    BorrowPlace place;
    if (expression_place(checker,value,&place))
        for (const BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
            if (loan->active && loan->borrower_symbol==place.owner &&
                borrow_paths_overlap(checker,place.field,loan->borrower_field_symbol) &&
                loan->owner_symbol<checker->analyzer->model->symbol_count) {
                const AstType *origin=&checker->analyzer->model->symbols[loan->owner_symbol].declared_type;
                if (origin->is_array && !origin->outer_pointer_depth && origin->borrow_kind==AST_BORROW_NONE) return 1;
            }
    return 0;
}

static void check_future_return(BorrowChecker *checker, const AstExpression *value) {
    if (checker->summarizing) {
        AstExpression output = {.kind = AST_EXPR_NAME, .resolved_symbol_id = checker->summary_result};
        checker->aggregate_capture_depth++;
        if (semantic_expression_is_future(value)) capture_future_borrows(checker, &output, value, 0);
        else if (!add_borrow_mode(checker, &output, value, 0, 1))
            clone_aggregate_borrows(checker, &output, value, 0);
        checker->aggregate_capture_depth--;
        return;
    }
    if (value == NULL || (!semantic_expression_is_future(value) &&
        !semantic_expression_is_move_only(checker->analyzer, value) &&
        !value->resolved_is_slice && !value->is_array_view && value->resolved_type != TYPE_STRING &&
        value->resolved_borrow_kind == AST_BORROW_NONE &&
        value->kind != AST_EXPR_ENUM_CONSTRUCT &&
        !call_returns_view_aggregate(checker,value) && !has_array_view_origin(checker,value,0) &&
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
        if (loan->active && loan->iteration_scope_depth &&
            loan->borrower_symbol == escaped.resolved_symbol_id)
            report_borrow_error(checker, value->first_token,
                "A mutable iterator item cannot escape its iteration");
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
    /* Summaries retain inactive ancestors and cached result markers. Keep their
       addresses valid until this checker is destroyed. */
    for (BorrowRecord *loan = checker->borrows; loan != boundary; loan = loan->next)
        loan->active = 0;
}

static BorrowRecord *add_borrow(BorrowChecker *checker,
                                const AstExpression *borrower,
                                const AstExpression *value,
                                size_t scope_depth) {
    return add_borrow_mode(checker, borrower, value, scope_depth, 0);
}

typedef struct {
    const SemanticSymbol *function;
    const AstExpression *target;
    const AstExpression *call;
    size_t scope_depth;
    size_t count;
    int emit;
} ResultFieldBorrows;

static int clone_result_borrow_field(BorrowChecker *checker, size_t path,
                                    const AstType *type, void *context) {
    ResultFieldBorrows *fields = context;
    const SemanticSymbol *function = fields->function;
    const char *lifetime = result_field_lifetime(checker, function->source_program,
        &function->declaration->as.function.return_type, path);
    if (!lifetime) return 0;
    AstExpression destination = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)fields->target,
        .resolved_symbol_id = path};
    int found = 0;
    const AstExpression *argument = fields->call->arguments;
    for (const AstParameter *parameter = function->declaration->as.function.parameters;
         parameter && argument; parameter = parameter->next, argument = argument->next) {
        if (!type_mentions_lifetime(function->source_program, &parameter->type, lifetime)) continue;
        found = 1;
        if (fields->emit) add_lifetime_parameter_borrows(checker, &destination, argument,
            function->source_program, &parameter->type, lifetime, type->borrow_kind, fields->scope_depth);
    }
    fields->count++;
    return found;
}

/* Each returned reference leaf follows its declared lifetime, independently
   of how many struct levels or sibling lifetimes surround it. */
static int clone_result_field_borrows(BorrowChecker *checker, const AstExpression *target,
                                      const AstExpression *call, size_t scope_depth) {
    if (call->resolved_symbol_id >= checker->analyzer->model->symbol_count) return 0;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION || !function->declaration) return 0;
    const AstType *result = &function->declaration->as.function.return_type;
    if (result->kind != AST_TYPE_NAMED || result->borrow_kind != AST_BORROW_NONE ||
        result->pointer_depth || result->outer_pointer_depth) return 0;
    ResultFieldBorrows fields = {.function = function, .target = target, .call = call,
        .scope_depth = scope_depth};
    if (!visit_borrow_fields(checker, function->source_program, result, AST_SYMBOL_NONE,
        clone_result_borrow_field, &fields, 0) || !fields.count) return 0;
    BorrowPlace place;
    if (!expression_place(checker, target, &place)) return 0;
    if (!checker->aggregate_capture_depth) deactivate_borrower(checker, place.owner, place.field);
    fields.emit = 1;
    visit_borrow_fields(checker, function->source_program, result, AST_SYMBOL_NONE,
        clone_result_borrow_field, &fields, 0);
    return 1;
}

static void heap_rebase_ring(BorrowChecker *checker, BorrowRecord *loan,
                             const BorrowPlace *source, const BorrowPlace *target) {
    if (!loan->heap_storage) return;
    if(loan->heap_bounded && loan->heap_extent_owner==source->owner &&
        borrow_path_prefix(checker,source->field,loan->heap_extent_field)) {
        loan->heap_extent_owner=target->owner;
        loan->heap_extent_field=borrow_path_append(checker,target->field,
            borrow_path_relative(checker,loan->heap_extent_field,source->field));
    }
    if(loan->heap_tagged && borrow_path_prefix(checker,source->field,loan->heap_tag_field))
        loan->heap_tag_field=borrow_path_append(checker,target->field,
            borrow_path_relative(checker,loan->heap_tag_field,source->field));
    if(!loan->heap_ring) return;
    size_t *owners[]={&loan->heap_head_owner,&loan->heap_mod_owner};
    size_t *fields[]={&loan->heap_head_field,&loan->heap_mod_field};
    for (size_t i=0;i<2;i++)
        if (*owners[i]==source->owner && borrow_path_prefix(checker,source->field,*fields[i])) {
            *owners[i]=target->owner;
            *fields[i]=borrow_path_append(checker,target->field,
                borrow_path_relative(checker,*fields[i],source->field));
        }
}

static int clone_heap_storage_borrows(BorrowChecker *checker, const AstExpression *target,
                                     const AstExpression *source, size_t scope_depth) {
    /* Raw pointer copies carry the loans of storage already tracked behind
       that pointer, even though the pointer type itself has no checked view. */
    if (source && source->resolved_pointer_depth && source->resolved_borrow_kind==AST_BORROW_NONE) {
        BorrowPlace from, to;
        if (expression_place(checker,source,&from) && expression_place(checker,target,&to)) {
            heap_map_place(checker,&from); heap_map_place(checker,&to);
            if (from.owner==to.owner && from.field==to.field) return 1;
            BorrowRecord *boundary=checker->borrows;
            int copied=0;
            for (BorrowRecord *loan=boundary;loan;loan=loan->next) {
                if (!loan->active || !loan->heap_storage || loan->borrower_symbol!=from.owner ||
                    !borrow_path_prefix(checker,from.field,loan->borrower_field_symbol)) continue;
                BorrowRecord *copy=malloc(sizeof(*copy));
                if (!copy) { checker->analyzer->allocation_failed=1; return 1; }
                *copy=*loan;
                copy->borrower_symbol=to.owner;
                copy->borrower_field_symbol=borrow_path_append(checker,to.field,
                    borrow_path_relative(checker,loan->borrower_field_symbol,from.field));
                heap_rebase_ring(checker,copy,&from,&to);
                copy->summary_foreign=0; copy->summary_exit_active=0;
                copy->summary_effect=checker->summarizing;
                copy->scope_depth=checker->summarizing ? 0:scope_depth;
                copy->next=checker->borrows; checker->borrows=copy;
                copied=1;
            }
            if (copied) return 1;
        }
    }
    return 0;
}

static void clone_aggregate_borrows(BorrowChecker *checker,
                                    const AstExpression *target,
                                    const AstExpression *source,
                                    size_t scope_depth) {
    const AstExpression *indexed=source;
    while(indexed && indexed->kind==AST_EXPR_MEMBER) indexed=indexed->left;
    if (indexed && indexed->kind == AST_EXPR_INDEX && source->resolved_borrow_kind == AST_BORROW_NONE) {
        int saved = checker->reserving_loans;
        checker->reserving_loans = 1;
        int copied = heap_take(checker, target, source, scope_depth);
        checker->reserving_loans = saved;
        if (copied) return;
    }
    if (source && (source->lifetime_operation == LIFETIME_TAKE ||
                   source->lifetime_operation == LIFETIME_REPLACE)) {
        if (source->lifetime_operation == LIFETIME_REPLACE)
            for (BorrowCallSummary *entry=checker->summaries;entry;entry=entry->next)
                if (entry->call==source) {
                    clone_call_summary(checker,target,entry,scope_depth);
                    return;
                }
        if(source->lifetime_operation==LIFETIME_TAKE && heap_take(checker,target,source->arguments,scope_depth)) return;
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
        clone_propagation_borrows(checker, target, source, scope_depth, 0);
        return;
    }
    if (source != NULL && source->kind == AST_EXPR_STRUCT_LITERAL) {
        BorrowPlace place;
        if (!expression_place(checker, target, &place)) return;
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
                clone_aggregate_borrows(checker, &field, value, scope_depth);
            }
        }
        checker->aggregate_capture_depth--;
        return;
    }
    if (source != NULL && (source->kind == AST_EXPR_AWAIT || (source->async_operation == ASYNC_BLOCK_ON || source->async_operation == ASYNC_COMPLETE)) &&
        ((semantic_symbol_type_properties(checker->analyzer->model,
                                         source->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME) ||
         (source->has_resolved_ast_type && type_has_view_depth(checker,
             source->resolved_type_program ? source->resolved_type_program : checker->analyzer->program,
             &source->resolved_ast_type,0)))) {
        capture_future_borrows(checker, target,
            source->kind == AST_EXPR_AWAIT ? source->right : source->arguments, scope_depth);
        BorrowPlace destination;
        if (expression_place(checker, target, &destination))
            for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                if (loan->active && loan->borrower_symbol == destination.owner &&
                    borrow_path_prefix(checker, destination.field, loan->borrower_field_symbol) &&
                    loan->future_frame_only) loan->active = 0;
        if(expression_place(checker,target,&destination) &&
           !(semantic_symbol_type_properties(checker->analyzer->model,source->resolved_named_symbol_id) & SEMANTIC_TYPE_MUST_CONSUME))
            for(BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
                if(loan->active && loan->borrower_symbol==destination.owner &&
                   borrow_path_prefix(checker,destination.field,loan->borrower_field_symbol)) loan->captured_by_future=0;
        return;
    }
    if (source != NULL && source->kind == AST_EXPR_ENUM_CONSTRUCT) {
        BorrowPlace place;
        if (!expression_place(checker, target, &place)) return;
        if (!checker->aggregate_capture_depth) deactivate_borrower(checker, place.owner, place.field);
        checker->aggregate_capture_depth++;
        size_t index = 0;
        for (const AstExpression *payload = source->arguments; payload; payload = payload->next) {
            BorrowRecord *boundary = checker->borrows;
            AstExpression destination = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)target,
                .resolved_symbol_id = borrow_payload_path(checker, source->resolved_named_symbol_id,
                    source->resolved_symbol_id, index)};
            AstExpression view = *payload;
            if (source->resolved_symbol_id < checker->analyzer->model->symbol_count) {
                const SemanticSymbol *variant = &checker->analyzer->model->symbols[source->resolved_symbol_id];
                const AstTypeArgument *type = variant->node ? ((const AstEnumValue *)variant->node)->payload_types : NULL;
                for (size_t i = 0; type && i < index; i++) type = type->next;
                if (type && type->type.is_slice && payload->resolved_is_array) {
                    view.resolved_is_slice = 1;
                    view.resolved_is_array = 0;
                }
            }
            if (semantic_expression_is_future(payload))
                capture_future_borrows(checker, &destination, payload, scope_depth);
            else if (add_borrow_mode(checker, &destination, &view, scope_depth, 1) == NULL)
                clone_aggregate_borrows(checker, &destination, payload, scope_depth);
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
        BorrowCallSummary *summary = summarize_call_borrows(checker, source);
        if (summary && !summary->unmodeled_storage) {
            clone_call_summary(checker, target, summary, scope_depth);
            return;
        }
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
         source->resolved_type != TYPE_STRING &&
         (!source->has_resolved_ast_type || !type_has_view_depth(checker,
             source->resolved_type_program ? source->resolved_type_program : checker->analyzer->program,
             &source->resolved_ast_type, 0))))
        return;
    BorrowPlace target_place, source_place;
    if (!expression_place(checker, target, &target_place) ||
        !expression_place(checker, source, &source_place))
        return;
    if (!checker->aggregate_capture_depth)
        deactivate_borrower(checker, target_place.owner, target_place.field);
    for (BorrowRecord *old = checker->borrows; old != NULL; old = old->next) {
        if (!old->active || old->borrower_symbol != source_place.owner ||
            !borrow_paths_overlap(checker, source_place.field, old->borrower_field_symbol))
            continue;
        BorrowRecord *copy = calloc(1, sizeof(*copy));
        if (copy == NULL) {
            checker->analyzer->allocation_failed = 1;
            return;
        }
        *copy = *old;
                copy->summary_foreign = 0;
        copy->summary_exit_active = 0;
        if (source->kind == AST_EXPR_INDEX) copy->heap_storage = 0;
        copy->package_storage = 0;
        copy->borrower_symbol = target_place.owner;
        if (source_place.field != AST_SYMBOL_NONE && old->borrower_field_symbol == AST_SYMBOL_NONE)
            copy->payload_known = 0;
        copy->borrower_field_symbol = borrow_path_append(checker, target_place.field,
            borrow_path_relative(checker, old->borrower_field_symbol, source_place.field));
        heap_rebase_ring(checker,copy,&source_place,&target_place);
        if (copy->heap_storage && old->heap_anchor_owner == source_place.owner &&
            borrow_path_prefix(checker, source_place.field, old->heap_anchor_field)) {
            copy->heap_anchor_owner = target_place.owner;
            copy->heap_anchor_field = borrow_path_append(checker, target_place.field,
                borrow_path_relative(checker, old->heap_anchor_field, source_place.field));
        }
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
        size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
        AstExpression materialized = {.kind = AST_EXPR_NAME, .resolved_symbol_id = temporary,
            .resolved_named_symbol_id = value->resolved_named_symbol_id};
        clone_aggregate_borrows(checker, &materialized, &source, scope_depth);
        clone_pattern_borrows(checker, target, &materialized, variant, index, scope_depth);
        release_argument_borrows(checker, temporary);
        return;
    }
    size_t temporary = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
    BorrowRecord *boundary = checker->borrows;
    size_t selected = borrow_payload_path(checker, value->resolved_named_symbol_id, variant, index);
    for (const BorrowRecord *old = boundary; old; old = old->next) {
        if (!old->active || old->borrower_symbol != value->resolved_symbol_id) continue;
        if (!borrow_paths_overlap(checker, selected, old->borrower_field_symbol)) continue;
        if (old->payload_known && old->payload_enum == value->resolved_named_symbol_id &&
            (old->payload_variant != variant || old->payload_index != index)) continue;
        BorrowRecord *copy = malloc(sizeof(*copy));
        if (!copy) { checker->analyzer->allocation_failed = 1; break; }
        *copy = *old;
                copy->summary_foreign = 0;
        copy->borrower_symbol = temporary;
        copy->borrower_field_symbol = borrow_path_relative(checker, old->borrower_field_symbol, selected);
        BorrowPlace ring_source={.owner=value->resolved_symbol_id,.field=selected};
        BorrowPlace ring_target={.owner=temporary,.field=AST_SYMBOL_NONE};
        heap_rebase_ring(checker,copy,&ring_source,&ring_target);
        if (copy->heap_storage && old->heap_anchor_owner == value->resolved_symbol_id &&
            borrow_path_prefix(checker, selected, old->heap_anchor_field)) {
            copy->heap_anchor_owner = temporary;
            copy->heap_anchor_field = borrow_path_relative(checker, old->heap_anchor_field, selected);
        }
        copy->package_storage = 0;
        copy->payload_known = 0;
        copy->next = checker->borrows;
        checker->borrows = copy;
    }
    source.resolved_symbol_id = temporary;
    clone_aggregate_borrows(checker, target, &source, scope_depth);
    release_argument_borrows(checker, temporary);
}

static void check_iteration_escapes(BorrowChecker *checker, size_t token) {
    if (checker->summarizing) return;
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next) {
        if (!loan->active || !loan->iteration_scope_depth ||
            loan->borrower_symbol >= checker->analyzer->model->symbol_count) continue;
        const SemanticSymbol *holder = &checker->analyzer->model->symbols[loan->borrower_symbol];
        if (holder->scope_depth < loan->iteration_scope_depth) {
            report_borrow_error(checker, token,
                "A mutable iterator item cannot escape its iteration");
            loan->iteration_scope_depth = 0; /* Avoid repeating the same diagnostic. */
        }
    }
}

static void check_statement_list(BorrowChecker *checker,
                                 const AstStatement *statement,
                                 size_t scope_depth) {
    size_t saved_scope_depth = checker->current_scope_depth;
    const AstStatement *saved_statement=checker->current_statement;
    BorrowContinuation *saved_continuations=checker->continuations;
    BorrowContinuation continuation={.statements=saved_statement ?
        ((saved_statement->kind==AST_STMT_WHILE || saved_statement->kind==AST_STMT_FOR) ?
            saved_statement : saved_statement->next) : NULL, .next=saved_continuations};
    if (saved_statement) checker->continuations=&continuation;
    checker->current_scope_depth = scope_depth;
    for (; statement != NULL; statement = statement->next) {
        check_iteration_escapes(checker, statement->first_token);
        checker->current_statement=statement;
        checker->evaluation_epoch++;
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
            index_seed_struct(checker,&borrower,statement->value);
            BorrowPlace index_anchor;
            int64_t index_offset;
            if (statement->value && data_type_integral(statement->value->resolved_type) &&
                heap_index_place(checker, statement->value, &index_anchor, &index_offset, 0) &&
                (index_anchor.owner != AST_SYMBOL_NONE || index_offset >= 0)) {
                BorrowIndexConstant *binding = malloc(sizeof(*binding));
                if (!binding) checker->analyzer->allocation_failed = 1;
                else {
                    *binding = (BorrowIndexConstant){.symbol=statement->resolved_symbol_id,
                        .value=index_offset,.anchor_owner=index_anchor.owner,.anchor_field=index_anchor.field,
                        .relative=index_anchor.owner != AST_SYMBOL_NONE,.valid=1,.next=checker->index_constants};
                    checker->index_constants=binding;
                }
            }
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
            if (statement->value != NULL && (statement->value->kind == AST_EXPR_AWAIT || (statement->value->async_operation==ASYNC_BLOCK_ON || statement->value->async_operation==ASYNC_COMPLETE)) &&
                !semantic_expression_is_future(statement->value))
                release_future_value(checker, statement->value->kind==AST_EXPR_AWAIT ? statement->value->right:statement->value->arguments);
            continue;
        }
        if (statement->kind == AST_STMT_ASSIGNMENT) {
            heap_shift(checker,statement);
            check_expression(checker, statement->expression,
                             BORROW_ACCESS_WRITE);
            BorrowAccess value_access =
                semantic_expression_is_move_only(checker->analyzer,
                                                 statement->value)
                    ? BORROW_ACCESS_WRITE
                    : BORROW_ACCESS_READ;
            check_expression(checker, statement->value, value_access);
            /* A checked storage parameter belongs to the caller. A value
               assigned into it must satisfy the same escape/lifetime proof as
               an explicitly returned value, even when both lexical scopes match. */
            BorrowPlace checked_storage;
            if(!checker->summarizing && statement->expression && statement->expression->has_resolved_ast_type &&
               reference_storage_place(checker,statement->expression,&checked_storage) &&
               checked_storage.owner<checker->analyzer->model->symbol_count) {
                const SemanticSymbol *holder=&checker->analyzer->model->symbols[checked_storage.owner];
                if((holder->kind==SEMANTIC_SYMBOL_FIELD ||
                    (holder->kind==SEMANTIC_SYMBOL_PARAMETER && holder->resolved_borrow_kind!=AST_BORROW_NONE)) &&
                   type_has_borrow_depth(checker,checker->analyzer->program,
                       &statement->expression->resolved_ast_type,0,0)) {
                    const AstDeclarationNode *saved=checker->analyzer->current_function;
                    AstDeclarationNode contract={.kind=AST_DECL_FUNCTION};
                    contract.as.function.return_type=statement->expression->resolved_ast_type;
                    checker->analyzer->current_function=&contract;
                    int saved_reserving=checker->reserving_loans;
                    checker->reserving_loans=1;
                    check_future_return(checker,statement->value);
                    checker->reserving_loans=saved_reserving;
                    checker->analyzer->current_function=saved;
                }
            }
            BorrowPlace storage;
            int external = summary_write_place(checker, statement->expression, &storage);
            int slot_assignment = !external && statement->expression &&
                statement->expression->kind == AST_EXPR_UNARY && statement->expression->operator_type == TOKEN_STAR &&
                statement->expression->resolved_borrow_kind != AST_BORROW_NONE &&
                reference_storage_place(checker, statement->expression, &storage);
            AstExpression owner, field;
            const AstExpression *destination = statement->expression;
            if (external || slot_assignment) {
                owner = (AstExpression){.kind = AST_EXPR_NAME, .resolved_symbol_id = storage.owner};
                field = (AstExpression){.kind = AST_EXPR_MEMBER, .left = &owner, .resolved_symbol_id = storage.field};
                destination = storage.field == AST_SYMBOL_NONE ? &owner : &field;
                /* Replacing a raw storage pointer cannot prove that omitted
                   elements were transferred or destroyed. A complete
                   relocation has already consumed these source loans. */
                if (statement->value && statement->value->resolved_pointer_depth &&
                    statement->value->resolved_borrow_kind==AST_BORROW_NONE) {
                    for (BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
                        if (loan->active && loan->borrower_symbol==storage.owner &&
                            borrow_path_prefix(checker,storage.field,loan->borrower_field_symbol)) {
                            if (loan->heap_storage) {
                                loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX;
                                loan->heap_bounded=0; loan->heap_tagged=0;
                            } else loan->active=0;
                        }
                } else deactivate_borrower(checker, storage.owner, storage.field);
            }
            AstExpression view;
            const AstExpression *assigned = statement->value;
            if (assigned && assigned->resolved_is_array && statement->expression &&
                statement->expression->resolved_is_slice) {
                view = *assigned;
                view.resolved_is_slice = 1;
                view.resolved_is_array = 0;
                assigned = &view;
            }
            int raw_storage_assignment=(external || slot_assignment) && assigned &&
                assigned->resolved_pointer_depth && assigned->resolved_borrow_kind==AST_BORROW_NONE;
            if (raw_storage_assignment) checker->aggregate_capture_depth++;
            if (!(raw_storage_assignment && clone_heap_storage_borrows(checker,destination,assigned,scope_depth)) &&
                !semantic_expression_is_future(statement->value) &&
                add_borrow(checker, destination,
                           assigned, scope_depth) == NULL)
                clone_aggregate_borrows(checker, destination,
                                        statement->value, scope_depth);
            if (raw_storage_assignment) checker->aggregate_capture_depth--;
            capture_future_borrows(checker, destination, statement->value, scope_depth);
            if (external) {
                for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                    if (loan->active && loan->borrower_symbol == storage.owner &&
                        borrow_path_prefix(checker, storage.field, loan->borrower_field_symbol)) {
                        loan->scope_depth = 0;
                        loan->summary_effect = 1;
                    }
                /* Subsequent reads through the parameter must see the updated slots. */
                AstExpression updated = *destination;
                updated.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
                clone_aggregate_borrows(checker, statement->expression, &updated, scope_depth);
            }
            if (statement->value != NULL && (statement->value->kind == AST_EXPR_AWAIT || (statement->value->async_operation==ASYNC_BLOCK_ON || statement->value->async_operation==ASYNC_COMPLETE)) &&
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
        check_expression(checker, statement->condition, BORROW_ACCESS_READ);
        check_expression(checker, statement->update, BORROW_ACCESS_READ);
        check_expression(checker, statement->value,
                         statement->kind == AST_STMT_RETURN &&
                         semantic_expression_is_move_only(checker->analyzer,
                                                          statement->value)
                             ? BORROW_ACCESS_WRITE
                             : BORROW_ACCESS_READ);
        if (statement->kind == AST_STMT_RETURN) {
            check_future_return(checker, statement->value);
            if (checker->summarizing) summary_capture_exit(checker);
        }

        check_statement_list(checker, statement->initializer,
                             scope_depth + 1);
        if (statement->kind == AST_STMT_IF) {
            const BorrowHeapGuard *guard_boundary=checker->heap_guards;
            BorrowIndexBound *guard = index_guard_probe(checker, statement->condition);
            int condition=heap_condition_value(checker,statement->condition);
            if (condition>=0) {
                heap_guard_push(checker,statement->condition,condition,0);
                check_statement_list(checker,condition ? statement->body : statement->else_body,scope_depth+1);
                heap_guard_pop(checker,guard_boundary);
                deactivate_scope(checker,scope_depth+1);
                index_guard_finish(checker, statement, guard);
                continue;
            }
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
                entry[index++] = (FutureLoanState) {.loan = loan, .active = loan->active,
                    .heap_min=loan->heap_min,.heap_max=loan->heap_max};
            const BorrowIndexBound *index_boundary=checker->index_bounds;
            heap_guard_push(checker,statement->condition,1,0);
            check_statement_list(checker, statement->body, scope_depth + 1);
            heap_guard_pop(checker,guard_boundary);
            index_discard_since(checker,index_boundary);
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
                    then_state[index++] = (FutureLoanState) {.loan = loan, .active = loan->active,
                        .heap_min=loan->heap_min,.heap_max=loan->heap_max};
                    loan->active = 0;
                }
            for (index = 0; index < count; index++) {
                entry[index].loan->active = entry[index].active;
                entry[index].loan->heap_min=entry[index].heap_min;
                entry[index].loan->heap_max=entry[index].heap_max;
            }
            heap_guard_push(checker,statement->condition,0,0);
            check_statement_list(checker, statement->else_body, scope_depth + 1);
            heap_guard_pop(checker,guard_boundary);
            deactivate_scope(checker, scope_depth + 1);
            if (!statement_may_fall_through(statement->else_body))
                for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                    loan->active = 0;
            if (then_reachable)
                for (index = 0; index < then_count; index++)
                    join_loan_state(&then_state[index]);
            free(entry);
            free(then_state);
            index_discard_since(checker,index_boundary);
            index_guard_finish(checker, statement, guard);
            continue;
        }
        if (statement->kind == AST_STMT_MATCH) {
            FutureLoanBranch entry = future_loan_snapshot(checker);
            FutureLoanBranch *branches = NULL;
            for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next) {
                const BorrowIndexBound *index_boundary=checker->index_bounds;
                future_loan_restore(checker, &entry);
                size_t binding_index = 0;
                for (const AstParameter *binding = arm->bindings; binding; binding = binding->next, binding_index++) {
                    const AstType *type = binding->resolved_symbol_id < checker->analyzer->model->symbol_count
                        ? &checker->analyzer->model->symbols[binding->resolved_symbol_id].declared_type : &binding->type;
                    AstExpression borrower = {.kind = AST_EXPR_NAME,
                        .resolved_symbol_id = binding->resolved_symbol_id};
                    if (statement->value && statement->value->resolved_borrow_kind != AST_BORROW_NONE) {
                        add_borrow_mode(checker, &borrower, statement->value, scope_depth + 1, 1);
                        continue;
                    }
                    if (statement->is_consuming_match || !type_has_view_arguments(checker, type)) continue;
                    BorrowRecord *boundary = checker->borrows;
                    clone_pattern_borrows(checker, &borrower, statement->value,
                        arm->resolved_variant_symbol, binding_index, scope_depth + 1);
                    if (checker->borrows == boundary && statement->value && statement->value->resolved_symbol_id < checker->analyzer->model->symbol_count &&
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
                        BorrowRecord *boundary = checker->borrows;
                        clone_pattern_borrows(checker, &borrower, statement->value,
                            arm->resolved_variant_symbol, index, scope_depth + 1);
                        if (checker->borrows == boundary && statement->value->resolved_symbol_id < checker->analyzer->model->symbol_count &&
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
                for (const AstParameter *binding = arm->bindings; binding; binding = binding->next)
                    if (binding->iteration_borrow)
                        for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                            if (loan->active && loan->kind == AST_BORROW_MUTABLE &&
                                loan->borrower_symbol == binding->resolved_symbol_id)
                                loan->iteration_scope_depth = checker->analyzer->model->symbols[binding->resolved_symbol_id].scope_depth;
                check_statement_list(checker, arm->body, scope_depth + 1);
                index_discard_since(checker,index_boundary);
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
                    join_loan_state(&branches->state[i]);
                FutureLoanBranch *next = branches->next;
                free(branches->state); free(branches); branches = next;
            }
            free(entry.state);
            continue;
        }
        if (statement->kind == AST_STMT_WHILE || statement->kind == AST_STMT_FOR) {
            if (heap_condition_value(checker, statement->condition) == 0) continue;
            if (heap_relocate_storage(checker, statement) || heap_rehash_storage(checker, statement) ||
                heap_relocate_left(checker, statement)) continue;
            HeapBorrowPlace limit = {0};
            if (statement->condition)
                limit.anchored = heap_index_place(checker, statement->condition->right,
                    &limit.anchor, &limit.offset, 0);
            if (limit.anchored) {
                BorrowPlace count = limit.anchor;
                int64_t offset;
                if (count.owner == AST_SYMBOL_NONE && statement->condition)
                    heap_index_place(checker, statement->condition->left, &count, &offset, 0);
                index_bound_value(checker, &count, &limit.minimum, &limit.exact);
                BorrowPlace counter;
                if (expression_place(checker, statement->condition->left, &counter)) {
                    heap_map_place(checker, &counter);
                    limit.frozen=heap_index_frozen(checker, statement->condition->right, &counter, 0);
                }
            }
            FutureLoanBranch entry = future_loan_snapshot(checker);
            const BorrowWrite *write_boundary = checker->writes;
            const BorrowIndexBound *index_boundary=checker->index_bounds;
            check_statement_list(checker, statement->body, scope_depth + 1);
            check_statement_list(checker, statement->else_body, scope_depth + 1);
            index_discard_since(checker,index_boundary);
            deactivate_scope(checker, scope_depth + 1);
            /* The loop may execute zero times. A return in its body does not
               consume the loan on the fall-through path. */
            for (size_t i = 0; i < entry.count; i++) join_loan_state(&entry.state[i]);
            if(!heap_drain(checker,statement,&entry,&limit) && !heap_ring_drain(checker,statement,&entry,&limit) &&
               !heap_tagged_drain(checker,statement,&entry))
                for(BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
                    if(loan->active && loan->heap_storage)
                        for (const BorrowWrite *write = checker->writes; write != write_boundary; write = write->next)
                            if (write->owner == loan->borrower_symbol &&
                                borrow_path_prefix(checker, write->field, loan->borrower_field_symbol)) {
                                loan->heap_min=INT64_MIN; loan->heap_max=INT64_MAX; break;
                            }
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
    check_iteration_escapes(checker, saved_statement ? saved_statement->first_token : AST_TOKEN_NONE);
    checker->current_scope_depth = saved_scope_depth;
    checker->current_statement=saved_statement;
    checker->continuations=saved_continuations;
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

static void collect_package_statement_uses(BorrowChecker *, const AstStatement *, unsigned char *, unsigned char *);
static void collect_package_function_uses(BorrowChecker *checker, size_t symbol,
                                          unsigned char *uses, unsigned char *visited) {
    if (symbol >= checker->analyzer->model->symbol_count || visited[symbol]) return;
    visited[symbol] = 1;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[symbol];
    if (function->kind == SEMANTIC_SYMBOL_FUNCTION && function->declaration)
        collect_package_statement_uses(checker, function->declaration->as.function.body, uses, visited);
}

static void collect_package_expression_uses(BorrowChecker *checker, const AstExpression *expression,
                                            unsigned char *uses, unsigned char *visited) {
    for (; expression; expression = expression->next) {
        if (expression->kind == AST_EXPR_NAME && expression->resolved_symbol_id < checker->analyzer->model->symbol_count) {
            const SemanticSymbol *symbol = &checker->analyzer->model->symbols[expression->resolved_symbol_id];
            if (!symbol->scope_depth && (symbol->kind == SEMANTIC_SYMBOL_VARIABLE || symbol->kind == SEMANTIC_SYMBOL_CONSTANT))
                uses[symbol->id] = 1;
        }
        if (expression->kind == AST_EXPR_CALL)
            collect_package_function_uses(checker, expression->resolved_symbol_id, uses, visited);
        collect_package_expression_uses(checker, expression->left, uses, visited);
        collect_package_expression_uses(checker, expression->right, uses, visited);
        collect_package_expression_uses(checker, expression->arguments, uses, visited);
        collect_package_statement_uses(checker, expression->control, uses, visited);
    }
}

static void collect_package_statement_uses(BorrowChecker *checker, const AstStatement *statement,
                                           unsigned char *uses, unsigned char *visited) {
    for (; statement; statement = statement->next) {
        collect_package_expression_uses(checker, statement->expression, uses, visited);
        collect_package_expression_uses(checker, statement->value, uses, visited);
        collect_package_expression_uses(checker, statement->condition, uses, visited);
        collect_package_expression_uses(checker, statement->update, uses, visited);
        collect_package_expression_uses(checker, statement->result, uses, visited);
        collect_package_statement_uses(checker, statement->initializer, uses, visited);
        collect_package_statement_uses(checker, statement->body, uses, visited);
        collect_package_statement_uses(checker, statement->else_body, uses, visited);
        for (const AstMatchArm *arm = statement->match_arms; arm; arm = arm->next)
            collect_package_statement_uses(checker, arm->body, uses, visited);
    }
}

static int function_package_borrows(BorrowChecker *checker, size_t symbol, unsigned char *uses) {
    int present = 0;
    for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        if (loan->active && loan->package_storage && loan->kind == AST_BORROW_MUTABLE) { present = 1; break; }
    if (!present) return 0;
    size_t count = checker->analyzer->model->symbol_count;
    unsigned char *visited = calloc(count, 1);
    unsigned char *owned_uses = uses ? NULL : calloc(count, 1);
    if (!visited || (!uses && !owned_uses)) {
        free(visited); free(owned_uses); checker->analyzer->allocation_failed = 1; return 0;
    }
    if (!uses) uses = owned_uses;
    collect_package_function_uses(checker, symbol, uses, visited);
    int found = 0;
    for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        if (loan->active && loan->package_storage && loan->kind == AST_BORROW_MUTABLE &&
            loan->borrower_symbol < count && uses[loan->borrower_symbol]) { found = 1; break; }
    free(visited); free(owned_uses);
    return found;
}

static BorrowRecord *clone_call_summary(BorrowChecker *checker, const AstExpression *target,
                                        BorrowCallSummary *summary, size_t scope_depth) {
    AstExpression source = {.kind = AST_EXPR_NAME, .resolved_symbol_id = summary->result_symbol,
        .resolved_borrow_kind = AST_BORROW_IMMUTABLE};
    for (BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        if (loan->borrower_symbol == summary->result_symbol) loan->active = 1;
    BorrowRecord *boundary = checker->borrows;
    clone_aggregate_borrows(checker, target, &source, scope_depth);
    for (BorrowRecord *loan = boundary; loan; loan = loan->next)
        if (loan->borrower_symbol == summary->result_symbol) loan->active = 0;
    return checker->borrows != boundary ? checker->borrows : NULL;
}

/* Snapshot the old value after evaluating the replacement. Returned-value
   loans and new-slot loans have independent origins. */
static BorrowCallSummary *replacement_borrows(BorrowChecker *checker,
                                             const AstExpression *call,size_t scope_depth) {
    for (BorrowCallSummary *entry=checker->summaries;entry;entry=entry->next)
        if (entry->call==call && entry->epoch==checker->evaluation_epoch) return entry;
    BorrowCallSummary *entry=calloc(1,sizeof(*entry));
    if (!entry) { checker->analyzer->allocation_failed=1; return NULL; }
    entry->call=call;
    entry->epoch=checker->evaluation_epoch;
    entry->result_symbol=AST_SYMBOL_NONE-1-checker->temporary_borrowers++;
    AstExpression result={.kind=AST_EXPR_NAME,.resolved_symbol_id=entry->result_symbol};
    const AstExpression *pointer=call->arguments;
    const AstExpression *place=pointer && pointer->kind==AST_EXPR_UNARY &&
        pointer->operator_type==TOKEN_AMPERSAND ? pointer->right:NULL;
    int heap=heap_take(checker,&result,pointer,scope_depth);
    if (!heap && place) {
        if (!add_borrow_mode(checker,&result,place,scope_depth,1))
            clone_aggregate_borrows(checker,&result,place,scope_depth);
    }
    for (BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
        if (loan->borrower_symbol==entry->result_symbol) loan->active=0;
    if (heap) heap_initialize(checker,pointer,pointer->next,scope_depth);
    else if (place && place->kind==AST_EXPR_NAME) {
        deactivate_borrower(checker,place->resolved_symbol_id,AST_SYMBOL_NONE);
        if (!add_borrow_mode(checker,place,pointer->next,scope_depth,1))
            clone_aggregate_borrows(checker,place,pointer->next,scope_depth);
    }
    entry->next=checker->summaries;
    checker->summaries=entry;
    return entry;
}

/* Evaluate the resolved borrow flow of a synchronous callee with its parameters
   bound to caller origins. Runtime values remain abstract; branch/loop joins
   are the same conservative joins used in ordinary function checking. */
typedef struct {
    const AstExpression *target;
    const AstExpression *source;
    const AstType *type;
    const AstProgram *unit;
} AbstractParameterBorrows;

static int seed_abstract_parameter_field(BorrowChecker *checker, size_t path,
                                        const AstType *type, void *context) {
    AbstractParameterBorrows *fields = context;
    AstExpression target = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)fields->target,
        .resolved_symbol_id = path};
    AstExpression source = {.kind = AST_EXPR_MEMBER, .left = (AstExpression *)fields->source,
        .resolved_symbol_id = path, .resolved_named_symbol_id = AST_SYMBOL_NONE,
        .resolved_borrow_kind = type->borrow_kind == AST_BORROW_NONE ? AST_BORROW_IMMUTABLE : type->borrow_kind};
    BorrowRecord *loan = add_borrow_mode(checker, &target, &source, 1, 1);
    if (loan) loan->origin_lifetime = result_field_lifetime(checker, fields->unit, fields->type, path);
    return 1;
}

static BorrowCallSummary *summarize_call_borrows(BorrowChecker *checker, const AstExpression *call) {
    if (!call || call->kind != AST_EXPR_CALL || checker->summary_depth >= 16 ||
        call->resolved_symbol_id >= checker->analyzer->model->symbol_count) return NULL;
    for (unsigned i = 0; i < checker->summary_depth; i++)
        if (checker->summary_functions[i] == call->resolved_symbol_id) return NULL;
    for (BorrowCallSummary *entry = checker->summaries; entry; entry = entry->next)
        if (entry->call == call && entry->epoch == checker->evaluation_epoch) return entry;
    const SemanticSymbol *function = &checker->analyzer->model->symbols[call->resolved_symbol_id];
    if (function->kind != SEMANTIC_SYMBOL_FUNCTION || !function->declaration ||
        !function->declaration->as.function.body || function->declaration->as.function.is_async) return NULL;
    int needed = call->resolved_borrow_kind != AST_BORROW_NONE || call->resolved_is_slice ||
        semantic_expression_is_future(call) ||
        type_has_view_depth(checker, function->source_program,
            &function->declaration->as.function.return_type, 0);
    for (const AstExpression *arg = call->arguments; arg && !needed; arg = arg->next)
        if (arg->has_resolved_ast_type && !arg->resolved_is_slice &&
            arg->resolved_borrow_kind == AST_BORROW_NONE &&
            type_has_view_depth(checker,
                arg->resolved_type_program ? arg->resolved_type_program : checker->analyzer->program,
                &arg->resolved_ast_type, 0)) needed = 1;
    for (const AstExpression *arg = call->arguments; arg && !needed; arg = arg->next) {
        BorrowPlace storage;
        const AstExpression *base = arg;
        if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND) base = base->right;
        if (!canonical_place(checker, base, &storage)) continue;
        for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
            if (loan->active && loan->heap_storage && loan->borrower_symbol == storage.owner &&
                borrow_path_prefix(checker, storage.field, loan->borrower_field_symbol)) {
                needed = 1; break;
            }
    }
    needed |= function_package_borrows(checker, function->id, NULL);
    const AstParameter *mutable_parameter = function->declaration->as.function.parameters;
    for (const AstExpression *arg = call->arguments; arg && mutable_parameter;
         arg = arg->next, mutable_parameter = mutable_parameter->next)
        if (mutable_parameter->type.borrow_kind == AST_BORROW_MUTABLE) {
            AstType referent = mutable_parameter->type;
            referent.borrow_kind = AST_BORROW_NONE;
            if (type_has_view_depth(checker, function->source_program, &referent, 0)) needed = 1;
        }
    if (!function->declaration->as.function.is_static && call->left &&
        call->left->kind == AST_EXPR_MEMBER && call->left->left &&
        function->owner_symbol_id < checker->analyzer->model->symbol_count) {
        const SemanticSymbol *owner = &checker->analyzer->model->symbols[function->owner_symbol_id];
        if (type_has_view_depth(checker, owner->source_program, &owner->declared_type, 0) &&
            statements_mutate_receiver(checker, function, function->declaration->as.function.body)) needed = 1;
        BorrowPlace receiver;
        if(canonical_place(checker,call->left->left,&receiver))
            for(const BorrowRecord *loan=checker->borrows;loan;loan=loan->next)
                if(loan->active && loan->heap_storage && loan->borrower_symbol==receiver.owner) { needed=1; break; }
    }
    if (!needed && semantic_expression_is_move_only(checker->analyzer, call)) {
        for (const AstExpression *arg = call->arguments; arg && !needed; arg = arg->next) {
            if (semantic_expression_is_move_only(checker->analyzer, arg)) { needed = 1; break; }
            BorrowPlace storage;
            if (!expression_place(checker, arg, &storage)) continue;
            for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                if (loan->active && loan->borrower_symbol == storage.owner) { needed = 1; break; }
        }
    }
    if (!needed) return NULL;
    size_t count = 0;
    for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
        if (loan->active) count++;
    BorrowRecord **original = count ? calloc(count, sizeof(*original)) : NULL;
    BorrowRecord **foreign = count ? calloc(count, sizeof(*foreign)) : NULL;
    unsigned char *consumed = count ? calloc(count, 1) : NULL;
    BorrowCallSummary *entry = calloc(1, sizeof(*entry));
    if (!entry || (count && (!original || !foreign || !consumed))) {
        free(original); free(foreign); free(consumed); free(entry);
        checker->analyzer->allocation_failed = 1;
        return NULL;
    }
    entry->call = call;
    entry->epoch = checker->evaluation_epoch;
    entry->result_symbol = AST_SYMBOL_NONE - 1 - checker->temporary_borrowers++;
    Analyzer analyzer = *checker->analyzer;
    analyzer.program = (AstProgram *)function->source_program;
    analyzer.current_function = function->declaration;
    analyzer.current_function_symbol_id = function->id;
    BorrowChecker nested = {.analyzer = &analyzer, .summarizing = 1,
        .summary_depth = checker->summary_depth + 1, .summary_result = entry->result_symbol,
        .paths = checker->paths, .path_count = checker->path_count,
        .temporary_borrowers = checker->temporary_borrowers};
    nested.caller_analyzer = checker->caller_analyzer ? checker->caller_analyzer : checker->analyzer;
    nested.heap_guards=checker->heap_guards;
    nested.call_token = checker->caller_analyzer ? checker->call_token : call->first_token;
    memcpy(nested.summary_functions, checker->summary_functions, sizeof(nested.summary_functions));
    nested.summary_functions[checker->summary_depth] = function->id;
    nested.last_use = calloc(analyzer.model->symbol_count, sizeof(*nested.last_use));
    if (!nested.last_use) analyzer.allocation_failed = 1;
    for (const BorrowIndexBound *bound = checker->index_bounds; bound; bound = bound->next) {
        if (!bound->valid || (!bound->exact && !bound->minimum)) continue;
        BorrowIndexBound *copy = malloc(sizeof(*copy));
        if (!copy) { analyzer.allocation_failed = 1; break; }
        *copy = *bound;
        copy->next = nested.index_bounds;
        nested.index_bounds = copy;
    }
    size_t index = 0;
    for (BorrowRecord *loan = checker->borrows; loan && !analyzer.allocation_failed; loan = loan->next) {
        if (!loan->active) continue;
        BorrowRecord *copy = malloc(sizeof(*copy));
        if (!copy) { analyzer.allocation_failed = 1; break; }
        *copy = *loan;
        copy->summary_exit_active = 0;
        copy->summary_foreign = 1;
        copy->scope_depth = 0;
        copy->next = nested.borrows;
        nested.borrows = copy;
        original[index] = loan;
        foreign[index++] = copy;
    }
    for (size_t i = 0; i < index; i++)
        for (size_t j = 0; j < index; j++)
            if (foreign[i]->parent == original[j]) { foreign[i]->parent = foreign[j]; break; }
    if (!analyzer.allocation_failed) {
        collect_statement_uses(&nested, function->declaration->as.function.body, 0);
        /* Input expressions still use caller token IDs (notably constant
           slice/index regions). Switch units only when entering the body. */
        analyzer.program = checker->analyzer->program;
        const AstExpression *argument = call->arguments;
        for (const AstParameter *parameter = function->declaration->as.function.parameters;
             parameter && argument; parameter = parameter->next, argument = argument->next) {
            BorrowPlace index_anchor;
            int64_t constant;
            if (parameter->type.borrow_kind == AST_BORROW_NONE && !parameter->type.pointer_depth &&
                !parameter->type.outer_pointer_depth && data_type_integral(argument->resolved_type) &&
                heap_index_place(checker, argument, &index_anchor, &constant, 0) &&
                !index_statement_writes(&nested, function->declaration->as.function.body,
                                        parameter->resolved_symbol_id, 0)) {
                BorrowIndexConstant *binding = malloc(sizeof(*binding));
                if (!binding) analyzer.allocation_failed = 1;
                else {
                    *binding = (BorrowIndexConstant){.symbol=parameter->resolved_symbol_id,
                        .value=constant,.anchor_owner=index_anchor.owner,.anchor_field=index_anchor.field,
                        .relative=index_anchor.owner != AST_SYMBOL_NONE,.valid=1,.next=nested.index_constants};
                    nested.index_constants=binding;
                }
            }
            AstExpression target = {.kind = AST_EXPR_NAME, .resolved_symbol_id = parameter->resolved_symbol_id};
            index_seed_struct(&nested,&target,argument);
            nested.aggregate_capture_depth++;
            nested.reserving_loans = 1;
            BorrowRecord *boundary = nested.borrows;
            if (semantic_expression_is_future(argument)) capture_future_borrows(&nested, &target, argument, 1);
            else if (!add_borrow_mode(&nested, &target, argument, 1, 1))
                clone_aggregate_borrows(&nested, &target, argument, 1);
            if (nested.borrows == boundary && type_has_view_depth(&nested, function->source_program, &parameter->type, 0)) {
                BorrowPlace place;
                if (expression_place(&nested, argument, &place) && place.owner < analyzer.model->symbol_count &&
                    analyzer.model->symbols[place.owner].kind == SEMANTIC_SYMBOL_PARAMETER) {
                    const SemanticSymbol *origin = &analyzer.model->symbols[place.owner];
                    AbstractParameterBorrows fields = {.target = &target, .source = argument,
                        .type = &origin->declared_type, .unit = origin->source_program};
                    visit_borrow_fields(&nested, origin->source_program, &origin->declared_type,
                        AST_SYMBOL_NONE, seed_abstract_parameter_field, &fields, 0);
                }
            }
            nested.reserving_loans = 0;
            nested.aggregate_capture_depth--;
            if (parameter->type.borrow_kind == AST_BORROW_MUTABLE) {
                const AstExpression *base = argument;
                if (base->kind == AST_EXPR_UNARY && base->operator_type == TOKEN_AMPERSAND) base = base->right;
                BorrowPlace place;
                if (canonical_place(&nested, base, &place))
                    summary_storage_map(&nested, parameter->resolved_symbol_id, place.owner, place.field);
            }
            if(parameter->type.pointer_depth && parameter->type.borrow_kind==AST_BORROW_NONE) {
                HeapBorrowPlace heap = {0};
                if(heap_borrow_place(checker,argument,&heap)) {
                    summary_storage_map(&nested,parameter->resolved_symbol_id,heap.storage.owner,heap.storage.field);
                    nested.storage->heap_index=heap.anchored;
                    nested.storage->index_owner=heap.anchor.owner;
                    nested.storage->index_field=heap.anchor.field;
                    nested.storage->index_offset=heap.offset;
                    nested.storage->heap_ring=heap.ring;
                    nested.storage->head_owner=heap.head.owner;
                    nested.storage->head_field=heap.head.field;
                    nested.storage->mod_owner=heap.modulus.owner;
                    nested.storage->mod_field=heap.modulus.field;
                    nested.storage->heap_bounded=heap.bounded;
                    nested.storage->extent_owner=heap.extent.owner;
                    nested.storage->extent_field=heap.extent.field;
                    nested.storage->guard=heap.guard;
                }
            }
            if (parameter->type.borrow_kind == AST_BORROW_NONE &&
                semantic_expression_is_move_only(checker->analyzer, argument)) {
                BorrowPlace place;
                if (expression_place(&nested, argument, &place)) {
                    for (size_t i = 0; i < index; i++)
                        if (foreign[i]->borrower_symbol == place.owner &&
                            borrow_path_prefix(&nested, place.field, foreign[i]->borrower_field_symbol)) consumed[i] = 1;
                    deactivate_borrower(&nested, place.owner, place.field);
                }
            }
        }
        if (!function->declaration->as.function.is_static && call->left &&
            call->left->kind == AST_EXPR_MEMBER && call->left->left) {
            if (function->declaration->as.function.uses_receiver_reference && function->declaration->as.function.receiver_parameter) {
                AstExpression target = {.kind=AST_EXPR_NAME,
                    .resolved_symbol_id=function->declaration->as.function.receiver_parameter->resolved_symbol_id};
                AstExpression source = *call->left->left;
                source.resolved_borrow_kind = statements_mutate_receiver(checker,function,function->declaration->as.function.body) ||
                    type_has_mutable_view(checker,function->source_program,&function->declaration->as.function.return_type,0)
                    ? AST_BORROW_MUTABLE : AST_BORROW_IMMUTABLE;
                add_borrow_mode(&nested,&target,&source,1,1);
                BorrowPlace receiver;
                if (canonical_place(&nested,call->left->left,&receiver))
                    summary_storage_map(&nested,target.resolved_symbol_id,receiver.owner,receiver.field);
            }
            BorrowPlace receiver_storage;
            int receiver_heap = 0;
            if (expression_place(checker, call->left->left, &receiver_storage))
                for (const BorrowRecord *loan = checker->borrows; loan; loan = loan->next)
                    if (loan->active && loan->heap_storage && loan->borrower_symbol == receiver_storage.owner &&
                        borrow_path_prefix(checker, receiver_storage.field, loan->borrower_field_symbol)) {
                        receiver_heap = 1; break;
                    }
            for (size_t i = 0; i < analyzer.model->symbol_count; i++) {
                const SemanticSymbol *field = &analyzer.model->symbols[i];
                if (field->kind != SEMANTIC_SYMBOL_FIELD || field->owner_symbol_id != function->owner_symbol_id) continue;
                AstExpression target = {.kind = AST_EXPR_NAME, .resolved_symbol_id = i};
                AstExpression source = {.kind = AST_EXPR_MEMBER, .left = call->left->left,
                    .resolved_symbol_id = i, .resolved_borrow_kind = field->resolved_borrow_kind,
                    .resolved_type = field->resolved_type, .resolved_named_symbol_id = field->resolved_named_symbol_id,
                    .resolved_pointer_depth = field->resolved_pointer_depth,
                    .resolved_ast_type = field->declared_type, .has_resolved_ast_type = 1,
                    .resolved_type_program = field->source_program};
                /* A checked getter into raw storage owned by a receiver still
                   borrows that receiver. Its allocation must outlive the view. */
                if (source.resolved_borrow_kind == AST_BORROW_NONE &&
                    (field->resolved_pointer_depth || field->resolved_outer_pointer_depth) &&
                    (call->resolved_borrow_kind != AST_BORROW_NONE ||
                     (!receiver_heap && call_returns_view_aggregate(checker, call))))
                    source.resolved_borrow_kind = AST_BORROW_IMMUTABLE;
                nested.aggregate_capture_depth++;
                if (!add_borrow_mode(&nested, &target, &source, 1, 1))
                    clone_aggregate_borrows(&nested, &target, &source, 1);
                nested.aggregate_capture_depth--;
                BorrowPlace receiver;
                if (canonical_place(&nested, call->left->left, &receiver))
                    summary_storage_map(&nested, i, receiver.owner,
                        borrow_path_append(&nested, receiver.field, i));
            }
        }
        analyzer.program = (AstProgram *)function->source_program;
        check_statement_list(&nested, function->declaration->as.function.body, 1);
        if (statement_may_fall_through(function->declaration->as.function.body)) summary_capture_exit(&nested);
    }
    for (BorrowRecord *loan = nested.borrows; loan; loan = loan->next) {
        if (!loan->summary_foreign)
            for (size_t i = 0; i < index; i++)
                if (loan->parent == foreign[i]) { loan->parent = original[i]; break; }
        if (loan->summary_foreign) {
            for (size_t i = 0; i < index; i++)
                if (loan == foreign[i]) {
                    original[i]->heap_tagged &= loan->heap_tagged;
                    original[i]->heap_bounded &= loan->heap_bounded;
                    if (consumed[i]) original[i]->active = 0;
                    else if (summary_written(&nested, loan)) {
                        original[i]->active = loan->summary_exit_active;
                        original[i]->heap_min=loan->heap_exit_min;
                        original[i]->heap_max=loan->heap_exit_max;
                    }
                }
        }
        int effect = !loan->summary_foreign && summary_written(&nested, loan) && loan->summary_exit_active;
        if(effect) { loan->heap_min=loan->heap_exit_min; loan->heap_max=loan->heap_exit_max; }
        loan->active = effect;
        if (effect && loan->borrower_symbol < analyzer.model->symbol_count)
            loan->scope_depth = analyzer.model->symbols[loan->borrower_symbol].scope_depth;
        loan->summary_foreign = 0;
        loan->summary_effect = checker->summarizing && effect;
        if(loan->summary_effect) loan->scope_depth=0;
        loan->summary_exit_active = 0;
    }
    if (nested.borrows) {
        BorrowRecord *tail = nested.borrows;
        while (tail->next) tail = tail->next;
        tail->next = checker->borrows;
        checker->borrows = nested.borrows;
    }
    checker->paths = nested.paths;
    checker->path_count = nested.path_count;
    checker->temporary_borrowers = nested.temporary_borrowers;
    entry->heap_effect=nested.heap_effect;
    entry->unmodeled_storage=nested.unmodeled_storage;
    checker->unmodeled_storage|=nested.unmodeled_storage;
    checker->heap_effect|=nested.heap_effect;
    if (analyzer.allocation_failed) checker->analyzer->allocation_failed = 1;
    BorrowCallSummary *child = nested.summaries;
    while (child) {
        BorrowCallSummary *next = child->next;
        free(child);
        child = next;
    }
    free(nested.last_use);
    while (nested.index_bounds) {
        BorrowIndexBound *next = nested.index_bounds->next;
        free(nested.index_bounds); nested.index_bounds = next;
    }
    while (nested.index_constants) {
        BorrowIndexConstant *next = nested.index_constants->next;
        free(nested.index_constants); nested.index_constants = next;
    }
    while (nested.storage) {
        BorrowStorage *next = nested.storage->next;
        free(nested.storage); nested.storage = next;
    }
    while (nested.writes) {
        BorrowWrite *next = nested.writes->next;
        nested.writes->next=checker->writes;
        checker->writes=nested.writes;
        nested.writes = next;
    }
    free(original); free(foreign); free(consumed);
    entry->next = checker->summaries;
    checker->summaries = entry;
    return entry;
}

static void free_borrow_checker(BorrowChecker *checker) {
    while (checker->index_bounds) {
        BorrowIndexBound *next = checker->index_bounds->next;
        free(checker->index_bounds); checker->index_bounds = next;
    }
    while (checker->index_constants) {
        BorrowIndexConstant *next = checker->index_constants->next;
        free(checker->index_constants); checker->index_constants = next;
    }
    while(checker->storage) {
        BorrowStorage *next=checker->storage->next;
        free(checker->storage); checker->storage=next;
    }
    while(checker->writes) {
        BorrowWrite *next=checker->writes->next;
        free(checker->writes); checker->writes=next;
    }
    BorrowRecord *borrow = checker->borrows;
    while (borrow) {
        BorrowRecord *next = borrow->next;
        free(borrow);
        borrow = next;
    }
    free(checker->last_use);
    BorrowPath *path = checker->paths;
    while (path) {
        BorrowPath *next = path->next;
        free(path);
        path = next;
    }
    BorrowCallSummary *summary = checker->summaries;
    while (summary) {
        BorrowCallSummary *next = summary->next;
        free(summary);
        summary = next;
    }
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
    for (const AstParameter *parameter = function->as.function.parameters; parameter; parameter = parameter->next) {
        if (parameter->type.borrow_kind != AST_BORROW_NONE) continue;
        AstExpression target = {.kind = AST_EXPR_NAME, .resolved_symbol_id = parameter->resolved_symbol_id};
        AbstractParameterBorrows fields = {.target = &target, .source = &target,
            .type = &parameter->type, .unit = analyzer->program};
        checker.summarizing = 1;
        visit_borrow_fields(&checker, analyzer->program, &parameter->type,
            AST_SYMBOL_NONE, seed_abstract_parameter_field, &fields, 0);
        checker.summarizing = 0;
    }
    check_statement_list(&checker, function->as.function.body,
                         analyzer->scope_depth);

    free_borrow_checker(&checker);
}
