#ifndef DMM_GENERICS_H
#define DMM_GENERICS_H
#include "ast.h"
#define DMM_MAX_TYPE_PARAMETERS 16
#define DMM_MAX_SPECIALIZATIONS 256

int ast_concrete_type_equal(const AstProgram *a, const AstType *left,
                            const AstProgram *b, const AstType *right);

int ast_polymorphic_callable_compatible(const AstProgram *target_program,
                                        const AstType *target,
                                        const AstProgram *source_program,
                                        const AstType *source);

AstDeclarationNode *ast_specialize_function(AstProgram *program,
                                            const AstDeclarationNode *origin, const AstType *arguments, size_t count,
                                            const AstProgram *argument_program);

AstDeclarationNode *ast_specialize_callable_consumer(AstProgram *program,
                                                     const AstDeclarationNode *origin,
                                                     const AstExpression *arguments);
#endif
