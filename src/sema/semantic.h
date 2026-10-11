#ifndef DMM_SEMANTIC_H
#define DMM_SEMANTIC_H

#include <stddef.h>
#include <stdio.h>

#include "ast.h"

typedef enum {
    SEMANTIC_SYMBOL_FUNCTION,
    SEMANTIC_SYMBOL_STRUCT,
    SEMANTIC_SYMBOL_ENUM,
    SEMANTIC_SYMBOL_INTERFACE,
    SEMANTIC_SYMBOL_IMPORT,
    SEMANTIC_SYMBOL_FIELD,
    SEMANTIC_SYMBOL_ENUM_VALUE,
    SEMANTIC_SYMBOL_PARAMETER,
    SEMANTIC_SYMBOL_LOCAL,
    SEMANTIC_SYMBOL_CONSTANT, SEMANTIC_SYMBOL_VARIABLE,
    SEMANTIC_SYMBOL_TYPE_PARAMETER, SEMANTIC_SYMBOL_LIFETIME_PARAMETER
} SemanticSymbolKind;

/*
 * Ownership and destruction are properties of a concrete semantic type.
 * COPYABLE and MOVE_ONLY are mutually exclusive. NEEDS_DROP is orthogonal.
 */
typedef enum {
    SEMANTIC_TYPE_COPYABLE = 1u << 0,
    SEMANTIC_TYPE_MOVE_ONLY = 1u << 1,
    SEMANTIC_TYPE_NEEDS_DROP = 1u << 2,
    SEMANTIC_TYPE_SEND = 1u << 3,
    SEMANTIC_TYPE_SYNC = 1u << 4,
    /* A live value must be transferred or awaited, including inside aggregates. */
    SEMANTIC_TYPE_MUST_CONSUME = 1u << 5
} SemanticTypeProperties;

typedef struct {
    size_t id;
    const AstProgram *source_program;
    SemanticSymbolKind kind;
    size_t name_token;
    size_t owner_token;
    size_t owner_symbol_id;
    AstType declared_type;
    DataType resolved_type;
    AstBorrowKind resolved_borrow_kind;
    unsigned resolved_pointer_depth;
    unsigned resolved_outer_pointer_depth;
    size_t resolved_named_type_token;
    size_t resolved_named_symbol_id;
    int resolved_is_array;
    int resolved_is_slice;
    const AstDeclarationNode *declaration;
    const void *node;
    size_t scope_depth;
    unsigned type_properties;
    /* Per-constructor property: Future<T> alone does not describe captures. */
    unsigned async_frame_properties;
    /* Conditional transferability when caller proves all loans frame-internal.
       This is not the standalone Send property of a borrowed child Future. */
    unsigned async_graph_properties;
    int source_only;
    const AstExpression *source_capture;
    size_t source_scope;
    AstSourceLocation source_visible_from;
} SemanticSymbol;

typedef struct {
    const AstProgram *program;
    const void *node;
    AstSourceSpan span;
    size_t parent;
    size_t owner;
    unsigned binder_barriers; /* Nested callable binders replace the corresponding outer binder set. */
    const AstField *closure_captures;
} SemanticSourceScope;

typedef struct {
    const char *source_path;
    AstSourceLocation position;
    const char *marker;
} SemanticCompletionQuery;

/* Concrete interface arguments observed while checking generic bounds. The
 * linked argument cells are owned here; their nested types belong to the AST. */
typedef struct {
    size_t interface_id;
    const AstProgram *argument_program;
    AstTypeArgument *arguments;
    unsigned wildcard_parameters;
    size_t source_owner;
} SemanticInterfaceContext;

typedef struct {
    const AstProgram *program;
    TargetFormat target_format;
    SemanticSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    size_t *symbol_index;
    size_t symbol_index_capacity;
    size_t unresolved_expression_count;
    size_t duplicate_symbol_count;
    size_t error_count;
    const SemanticCompletionQuery *completion_query;
    size_t *completion_symbols;
    size_t completion_count;
    int completion_found;
    SemanticSourceScope *source_scopes;
    size_t source_scope_count;
    int source_registry_complete;
    SemanticInterfaceContext *interface_contexts;
    size_t interface_context_count;
    int interface_contexts_incomplete;
    unsigned interface_check_depth;
    unsigned char *interface_discovery_coverage;
    size_t interface_discovery_count;
} SemanticModel;

SemanticModel *semantic_analyze(AstProgram * program);

SemanticModel *semantic_analyze_target(AstProgram *program, TargetFormat target);
SemanticModel *semantic_analyze_target_ide(AstProgram *program, TargetFormat target,const SemanticCompletionQuery *query);

typedef struct {
    size_t size;
    size_t alignment;
} NativeTypeLayout;

/* Shared x86-64 glibc/UCRT layout. Arrays here have native element strides;
   ordinary DMM arrays continue to use eight-byte slots. */
int semantic_native_layout(const SemanticModel *model, const AstProgram *program,
                           const AstType *type, NativeTypeLayout *layout);

int semantic_native_field_offset(const SemanticModel *model, size_t symbol_id,
                                 size_t field_index, size_t *offset);

int semantic_implements_interface(const SemanticModel *model, size_t interface_id,
                                  size_t struct_id);

int semantic_satisfies(const SemanticModel *model, const AstProgram *unit,
                       const AstType *type, size_t interface_id);

int semantic_requires_explicit_init(const SemanticModel *model, const AstProgram *unit,
                                    const AstType *type);

int semantic_implements_specialized_interface(
    const SemanticModel *model, size_t interface_id, size_t owner_id,
    const AstProgram *argument_unit, const AstType *interface_type);

size_t semantic_interface_method(const SemanticModel *model, size_t interface_method_id,
                                 size_t struct_id);
size_t semantic_specialized_interface_method(const SemanticModel *model, size_t interface_method_id,
                                            size_t struct_id, const AstProgram *argument_program,
                                            const AstType *interface_type);
void semantic_record_interface_context(SemanticModel *model, size_t interface_id,
                                       const AstProgram *argument_program, const AstType *interface_type);
void semantic_discover_interface_contexts(SemanticModel *model);
int semantic_implements_interface_context(const SemanticModel *model, const SemanticInterfaceContext *context,
                                           size_t owner_id);
int semantic_receiver_contract(const SemanticModel *model, size_t function_id, unsigned *mode);

void semantic_model_free(SemanticModel *model);

int semantic_dump(FILE *output, const SemanticModel *model);
/* Uses the same package/reexport lookup as semantic analysis; never guesses by spelling. */
size_t semantic_resolve_type_name(const SemanticModel *model, const AstProgram *program, size_t token);
char *semantic_display_type(const SemanticModel *model, const AstProgram *program, const AstType *type);
char *semantic_display_symbol_type(const SemanticModel *model, const SemanticSymbol *symbol);
/* IDE-only registry, appended after all runtime analysis/refactoring validation has finished. */
int semantic_register_source_graph(SemanticModel *model);
size_t semantic_source_scope(const SemanticModel *model, const void *node);
size_t semantic_source_scope_at(const SemanticModel *model, const AstProgram *program, size_t owner,
                                 AstSourceLocation position);
size_t semantic_source_global_binding(const SemanticModel *model, const AstProgram *program, const char *name);
int semantic_source_intrinsic(const SemanticModel *model, const AstProgram *program, size_t scope,
                              size_t token, AstSourceLocation position);
size_t semantic_source_members(const SemanticModel *model, const AstProgram *program, size_t scope,
                                const AstExpression *receiver, size_t member_token, int call_arity,
                                size_t *candidates, size_t capacity, int *complete);
size_t semantic_source_binding(const SemanticModel *model, const AstProgram *program, size_t scope,
                               size_t token, SemanticSymbolKind kind, AstSourceLocation position);

const SemanticSymbol *semantic_find_global(const SemanticModel *model,
                                           const char *name,
                                           SemanticSymbolKind kind);

const SemanticSymbol *semantic_find_in_package(const SemanticModel *model, const AstProgram *file,
                                               const char *name, SemanticSymbolKind kind);

unsigned semantic_symbol_type_properties(const SemanticModel *model, size_t type_symbol_id);

#endif
