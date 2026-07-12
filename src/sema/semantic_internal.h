#ifndef DMM_SEMANTIC_INTERNAL_H
#define DMM_SEMANTIC_INTERNAL_H

#include "semantic.h"

struct CoreIntrinsic;

typedef struct LocalSymbol {
    size_t name_token;
    AstType type;
    size_t symbol_id;
    DataType resolved_type;
    AstBorrowKind resolved_borrow_kind;
    unsigned resolved_pointer_depth;
    unsigned resolved_outer_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    int resolved_is_slice;
    AstType resolved_ast_type;
    const AstProgram *resolved_type_program;
    int has_resolved_ast_type;
    int is_constant;
    int moved;
    int initialized;
    size_t scope_depth;
    struct LocalSymbol *next;
} LocalSymbol;

typedef struct {
    SemanticModel *model;
    AstProgram *program;
    LocalSymbol *locals;
    size_t current_function_token;
    size_t current_function_symbol_id;
    size_t current_owner_token;
    size_t scope_depth;
    unsigned loop_depth;
    const AstDeclarationNode *current_function;
    size_t local_storage;
    int storage_error_reported;
    int complexity_error_reported;
    int allocation_failed;
    int in_destructor;
    int in_defer_closure;
    const AstExpression *assignment_target;
} Analyzer;

typedef struct {
    char *text;
    size_t used;
    int failed;
} DiagnosticText;

void diagnostic_append(DiagnosticText *text, const char *format, ...);
void diagnostic_type(DiagnosticText *text, const Analyzer *analyzer,
                     DataType primitive, size_t nominal, unsigned pointers,
                     unsigned outer, int array, int slice, const char *length);
void operand_error(Analyzer *analyzer, const AstExpression *expression,
                   char category, int code, const char *reason);
void conversion_error(Analyzer *analyzer, const AstExpression *value,
                      const AstProgram *expected_program, const AstType *expected,
                      const AstExpression *expected_value, const char *reason);
void overload_error(Analyzer *analyzer, const AstExpression *call,
                    const char *name, size_t owner, int is_static, int ambiguous);
int viable_function(const Analyzer *analyzer, const SemanticSymbol *function,
                    const AstExpression *arguments);

void semantic_error(Analyzer *analyzer, size_t token, char category, int code,
                    const char *message);
int integral_expression(const AstExpression *expression);
int expression_is_constant_symbol(const Analyzer *analyzer, const AstExpression *expression);
int constant_expression_allowed(const Analyzer *analyzer, const AstExpression *expression);
const AstExpression *constant_initializer(const SemanticSymbol *symbol);
int fold_constant(Analyzer *analyzer, AstExpression *expression, DataType target);
void semantic_duplicate(Analyzer *analyzer, size_t token,
                        const AstProgram *previous_program, size_t previous_token,
                        const char *message);
DataType primitive_type(const AstProgram *program, const AstType *type);
size_t named_type_token(const AstProgram *program, const AstType *type);
size_t resolve_named_symbol_id(const Analyzer *analyzer, const AstProgram *program, size_t token);
void add_global(Analyzer *analyzer, AstDeclarationNode *declaration,
                SemanticSymbolKind kind, size_t owner_token);
void add_member(Analyzer *analyzer, size_t name_token, size_t owner_token,
                AstType type, SemanticSymbolKind kind, const void *node,
                size_t *resolved_symbol_id);
int expression_to_declared_type_allowed(const Analyzer *analyzer,
                                        const AstExpression *expression,
                                        const AstProgram *type_program, const AstType *type);
size_t parameter_count(const AstDeclarationNode *function);
int contains_type_parameter(const AstProgram *unit, const AstType *type,
                            const AstDeclarationNode *origin);
int function_dominates(const Analyzer *analyzer, const SemanticSymbol *left,
                       const SemanticSymbol *right, const AstExpression *arguments);
int known_declared_type(const Analyzer *analyzer, const AstType *type);
void validate_array_shape(Analyzer *analyzer, AstType *type);
const SemanticSymbol *explicit_generic_function(Analyzer *analyzer, const char *name,
                                                 AstExpression *call);
size_t concrete_token(Analyzer *analyzer, TokenType kind, const char *text);
AstType inferred_argument_type(Analyzer *analyzer, const AstExpression *value);
void normalize_generic_type(Analyzer *analyzer, AstType *type, unsigned depth);
void normalize_statement_types(Analyzer *analyzer, AstStatement *statement);
void normalize_function_types(Analyzer *analyzer, AstDeclarationNode *declaration);
void replace_self_type(Analyzer *analyzer, AstType *type, const AstType *self);
void replace_self_statement(Analyzer *analyzer, AstStatement *statement, const AstType *self);
void prepare_interfaces(Analyzer *analyzer, AstProgram *root);
int generic_bounds_satisfied(Analyzer *analyzer, const AstProgram *declaration_unit,
                             const AstDeclarationNode *declaration, const AstType *arguments);
AstType argument_type_copy(Analyzer *analyzer, const AstProgram *unit, AstType type);
void instantiate_generic_candidates(Analyzer *analyzer, const char *name,
                                    const AstExpression *arguments);
int aggregate_reaches(const Analyzer *analyzer, size_t current_symbol,
                      size_t target_symbol, size_t depth);
size_t semantic_type_slots(const Analyzer *analyzer, const AstProgram *program,
                           const AstType *type, const AstExpression *inferred);
int assignable_expression(const Analyzer *analyzer, const AstExpression *expression);
size_t layout_size(Analyzer *analyzer, AstType *type, size_t depth);
const LocalSymbol *find_local(const Analyzer *analyzer, size_t name_token);
LocalSymbol *find_local_by_symbol(Analyzer *analyzer, size_t symbol_id);
void analyze_constant_declaration(Analyzer *analyzer, AstDeclarationNode *declaration);
void analyze_expression(Analyzer *analyzer, AstExpression *expression);
void collect_declarations(Analyzer *analyzer, AstProgram *program);
LocalSymbol *push_local(Analyzer *analyzer, size_t name_token, AstType type,
                        SemanticSymbolKind kind, const AstExpression *inferred, int is_constant);
void pop_to(Analyzer *analyzer, LocalSymbol *saved);
const AstField *find_field(const Analyzer *analyzer, size_t type_token, size_t field_token);
const AstField *find_field_by_symbol(const Analyzer *analyzer, size_t type_symbol_id,
                                      size_t field_token);
const AstEnumValue *find_enum_value_by_symbol(const Analyzer *analyzer,
                                              size_t type_symbol_id, size_t value_token);
const SemanticSymbol *find_method(const Analyzer *analyzer, size_t owner_symbol_id,
                                  size_t method_token);
DataType promoted_numeric(DataType left, DataType right);
DataType builtin_result_type(const char *name);
int is_builtin_name(const char *name);
void validate_overload_sets(Analyzer *analyzer);
int expression_assignment_allowed(const Analyzer *analyzer, const AstExpression *source,
                                  const AstExpression *target);
int plain_numeric_expression(const AstExpression *expression);
int pointer_expression(const AstExpression *expression);
int semantic_type_is_move_only(const Analyzer *analyzer, size_t type_symbol_id);
int semantic_type_needs_drop(const Analyzer *analyzer, size_t type_symbol_id);
void derive_type_properties(Analyzer *analyzer);
int semantic_expression_is_move_only(const Analyzer *analyzer,
                                     const AstExpression *expression);
void validate_function_borrows(Analyzer *analyzer,
                               const AstDeclarationNode *function);
void validate_function_ownership(Analyzer *analyzer,
                                 const AstDeclarationNode *function);
int enum_constant_expression(const Analyzer *analyzer, const AstExpression *expression);
const SemanticSymbol *resolve_overload(const Analyzer *analyzer, const char *name,
                                       size_t owner_symbol_id, int is_static,
                                       const AstExpression *arguments, int *ambiguous);
void validate_expression(Analyzer *analyzer, AstExpression *expression, int is_callee);
int statement_may_fall_through(const AstStatement *statement);

int same_name(const AstProgram *program, size_t token, const char *name);
int same_package(const AstProgram *left, const AstProgram *right);
const char *symbol_name(const SemanticSymbol *symbol);
int symbol_matches_scope(const AstProgram *file, const SemanticSymbol *symbol, const char *name);
const DmmPackage *lookup_package(const AstProgram *file, const char **name);
const SemanticSymbol *scoped_find_global(const SemanticModel *model, const AstProgram *file,
                                         const char *name, SemanticSymbolKind kind);
int semantic_append_symbol(SemanticModel *model, SemanticSymbol symbol);

#endif
