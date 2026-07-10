#include "semantic_internal.h"
#include "generics.h"
#include "core_intrinsics.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void metadata_name(DiagnosticText *text, const Analyzer *analyzer, const AstProgram *unit, const AstType *type,
                          unsigned depth) {
    if (depth > 64) {
        text->failed = 1;
        return;
    }
    for (unsigned i = 0; i < type->outer_pointer_depth; i++) diagnostic_append(text, "*");
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, "(");
    for (unsigned i = 0; i < type->pointer_depth; i++) diagnostic_append(text, "*");
    size_t id = resolve_named_symbol_id(analyzer, unit, named_type_token(unit, type));
    if (id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[id];
        const AstDeclarationNode *decl = symbol->declaration;
        const AstProgram *owner = symbol->source_program;
        diagnostic_append(text, "%s.%s", owner->module_identity,
                          ast_program_lexeme(owner, decl && decl->generic_origin
                                                        ? decl->generic_origin->name_token
                                                        : symbol->name_token));
        if (decl && decl->specialization_arguments) {
            diagnostic_append(text, "<");
            for (const AstTypeArgument *a = decl->specialization_arguments; a; a = a->next) {
                metadata_name(text, analyzer, owner, &a->type, depth + 1);
                if (a->next) diagnostic_append(text, ",");
            }
            diagnostic_append(text, ">");
        }
    } else diagnostic_append(text, "%s", ast_program_lexeme(unit, type->name_token));
    if (type->is_slice) diagnostic_append(text, "[]");
    if (type->is_array) diagnostic_append(text, "[%zu]", type->resolved_array_length);
    if (type->outer_pointer_depth && (type->is_array || type->is_slice)) diagnostic_append(text, ")");
}

static int array_literal_element_allowed(const Analyzer *analyzer,
                                         const AstExpression *element,
                                         const AstType *element_type) {
    size_t target = resolve_named_symbol_id(analyzer, analyzer->program,
                                            named_type_token(analyzer->program, element_type));
    if (target < analyzer->model->symbol_count &&
        analyzer->model->symbols[target].kind == SEMANTIC_SYMBOL_INTERFACE &&
        element->resolved_pointer_depth == 0 &&
        element->resolved_outer_pointer_depth == 0 &&
        !element->resolved_is_array && !element->resolved_is_slice &&
        semantic_implements_interface(analyzer->model, target,
                                      element->resolved_named_symbol_id))
        return 1;
    return expression_to_declared_type_allowed(analyzer, element,
                                               analyzer->program, element_type);
}

static void contextualize_direct_call_literals(Analyzer *analyzer,
                                               AstExpression *call) {
    if (call == NULL || call->kind != AST_EXPR_CALL || call->left == NULL ||
        call->left->kind != AST_EXPR_NAME) return;
    const char *name = ast_program_lexeme(analyzer->program,
                                         call->left->value_token);
    const SemanticSymbol *function = scoped_find_global(
        analyzer->model, analyzer->program, name, SEMANTIC_SYMBOL_FUNCTION);
    if (function == NULL || function->declaration == NULL ||
        function->declaration->generic_parameters != NULL) return;
    AstExpression *argument = call->arguments;
    const AstParameter *parameter =
        function->declaration->as.function.parameters;
    for (; argument != NULL && parameter != NULL;
         argument = argument->next, parameter = parameter->next)
        if (argument->kind == AST_EXPR_ARRAY_LITERAL)
            argument->allocated_type = parameter->type;
}

void analyze_expression(Analyzer *analyzer, AstExpression *expression) {
    if (expression == NULL) return;
    if (expression->kind == AST_EXPR_TYPE_INFO && !expression->left &&
        !expression->allocated_type.pointer_depth && !expression->allocated_type.outer_pointer_depth &&
        !expression->allocated_type.is_array && !expression->allocated_type.is_slice && !expression->allocated_type.
        arguments) {
        const SemanticSymbol *global = scoped_find_global(analyzer->model, analyzer->program,
                                                          ast_program_lexeme(
                                                              analyzer->program, expression->value_token),
                                                          SEMANTIC_SYMBOL_VARIABLE);
        if (!global)
            global = scoped_find_global(analyzer->model, analyzer->program,
                                        ast_program_lexeme(analyzer->program, expression->value_token),
                                        SEMANTIC_SYMBOL_CONSTANT);
        if (global || find_local(analyzer, expression->value_token)) {
            expression->kind = AST_EXPR_NAME;
            expression->allocated_type = (AstType)
            {
                .kind = AST_TYPE_INFERRED,.name_token = AST_TOKEN_NONE
            };
        }
    }
    if (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind == AST_EXPR_NAME &&
        !find_local(analyzer, expression->left->value_token)) {
        const char *alias = ast_program_lexeme(analyzer->program, expression->left->value_token);
        for (AstDeclarationNode *d = analyzer->program->root; d; d = d->next)
            if (d->kind == AST_DECL_IMPORT)
                for (AstImportPath *p = d->as.import_decl.paths; p; p = p->next)
                    if (p->alias && !strcmp(alias, p->alias)) {
                        char qualified[2048];
                        snprintf(qualified, sizeof(qualified), "%s.%s", alias,
                                 ast_program_lexeme(analyzer->program, expression->value_token));
                        expression->value_token = concrete_token(analyzer, TOKEN_IDENTIFIER, qualified);
                        expression->kind = AST_EXPR_NAME;
                        expression->left = NULL;
                    }
    }
    analyze_expression(analyzer, expression->left);
    analyze_expression(analyzer, expression->right);
    contextualize_direct_call_literals(analyzer, expression);
    for (AstExpression *argument = expression->arguments; argument != NULL; argument = argument->next)
        analyze_expression(analyzer, argument);

    if (expression->kind == AST_EXPR_ENUM_ACCESS) return;

    if (expression->kind == AST_EXPR_ENUM_CONSTRUCT) {
        const AstExpression *left = expression->left;
        if (left && left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
            expression->resolved_symbol_id = left->resolved_symbol_id;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        } else if (left) {
            const AstEnumValue *variant = find_enum_value_by_symbol(analyzer, left->resolved_named_symbol_id,
                                                                    expression->value_token);
            expression->resolved_symbol_id = variant ? variant->resolved_symbol_id : AST_SYMBOL_NONE;
            expression->resolved_named_symbol_id = left->resolved_named_symbol_id;
            expression->resolved_named_type_token = left->resolved_named_type_token;
        }
        return;
    }
    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_borrow_kind = AST_BORROW_NONE;
    expression->resolved_pointer_depth = 0;
    expression->resolved_outer_pointer_depth = 0;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_is_array = 0;
    expression->resolved_is_slice = 0;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    expression->owns_slice_backing = 0;
    if (expression->kind == AST_EXPR_MEMBER && expression->left && same_name(
            analyzer->program, expression->value_token, "type")) {
        if (expression->left->kind == AST_EXPR_TYPE_INFO ||
            (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
             (analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_STRUCT ||
              analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM)))
            semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Types expose name, size and align directly; .type is for values");
        expression->kind = AST_EXPR_TYPE_INFO;
        expression->allocated_type = inferred_argument_type(analyzer, expression->left);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Value type metadata requires a known static type");
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_TYPE_INFO) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        if (!known_declared_type(analyzer, &expression->allocated_type))
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_UNKNOWN,
                           "Type metadata requires a known type");
        expression->resolved_type = TYPE_VOID; /* Compile-time entity, never an IR value. */
    } else if (expression->kind == AST_EXPR_TYPE_PROPERTY ||
               (expression->kind == AST_EXPR_MEMBER && expression->left && expression->left->kind ==
                AST_EXPR_TYPE_INFO)) {
        expression->kind = AST_EXPR_TYPE_PROPERTY;
        const char *property = ast_program_lexeme(analyzer->program, expression->value_token);
        AstType *type = &expression->left->allocated_type;
        if (!strcmp(property, "name")) {
            DiagnosticText text = {0};
            metadata_name(&text, analyzer, analyzer->program, type, 0);
            if (text.failed) analyzer->allocation_failed = 1;
            else expression->folded_constant = (AstToken)
            {
                .type = TOKEN_STRING_LITERAL,
                .lexeme = string_interner_intern(analyzer->program->strings, text.text),.span = expression->span
            };
            free(text.text);
            expression->resolved_type = TYPE_STRING;
        } else if (!strcmp(property, "size") || !strcmp(property, "align")) {
            DataType primitive = primitive_type(analyzer->program, type);
            int is_void = primitive == TYPE_VOID && !type->pointer_depth && !type->outer_pointer_depth && !type->
                          is_array && !type->is_slice;
            size_t size = is_void ? 0 : layout_size(analyzer, type, 0);
            if ((!size && !is_void) || size > INT64_MAX)
                semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Type metadata layout requires a complete, non-recursive sized type");
            size_t align = is_void
                               ? 1
                               : (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->
                                  is_slice && primitive != TYPE_UNKNOWN)
                                     ? data_type_bytes(primitive)
                                     : 8;
            char text[32];
            snprintf(text, sizeof(text), "%zu", !strcmp(property, "size") ? size : align);
            expression->folded_constant = (AstToken)
            {
                .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
                span = expression->span
            };
            expression->resolved_type = TYPE_USIZE;
        } else semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_SEMANTIC, ERR_SEM_FIELD_NOT_FOUND,
                              "Unknown type metadata property; expected name, size or align");
    } else if (expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF) {
        normalize_generic_type(analyzer, &expression->allocated_type, 0);
        size_t size = layout_size(analyzer, &expression->allocated_type, 0);
        expression->resolved_type = TYPE_USIZE;
        if (!size || size > (size_t) INT64_MAX) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Layout query requires a complete, non-recursive, sized non-void type");
            return;
        }
        const AstType *type = &expression->allocated_type;
        DataType primitive = primitive_type(analyzer->program, type);
        size_t alignment = (!type->pointer_depth && !type->outer_pointer_depth && !type->is_array && !type->is_slice &&
                            primitive != TYPE_UNKNOWN)
                               ? data_type_bytes(primitive)
                               : 8;
        char text[32];
        snprintf(text, sizeof(text), "%zu", expression->kind == AST_EXPR_SIZEOF ? size : alignment);
        expression->folded_constant = (AstToken)
        {
            .type = TOKEN_NUMBER,.lexeme = string_interner_intern(analyzer->program->strings, text),.
            span = expression->span
        };
    } else if (expression->kind == AST_EXPR_SLICE) {
        const AstExpression *data = expression->left;
        if (data) {
            expression->resolved_type = data->resolved_type;
            expression->resolved_named_type_token = data->resolved_named_type_token;
            expression->resolved_named_symbol_id = data->resolved_named_symbol_id;
            expression->resolved_pointer_depth = data->resolved_pointer_depth ? data->resolved_pointer_depth - 1 : 0;
            expression->resolved_is_slice = 1;
        }
    } else if (expression->kind == AST_EXPR_ARRAY_LITERAL) {
        AstType expected = expression->allocated_type;
        normalize_generic_type(analyzer, &expected, 0);
        validate_array_shape(analyzer, &expected);
        if (expected.kind == AST_TYPE_INFERRED ||
            (!expected.is_array && !expected.is_slice) ||
            expected.outer_pointer_depth != 0) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_UNKNOWN,
                           "Array literal requires an expected fixed-array or slice type");
            return;
        }

        size_t pattern_count = 0;
        for (const AstExpression *element = expression->arguments; element != NULL;
             element = element->next)
            pattern_count++;
        size_t element_count = pattern_count;
        if (expression->right != NULL) {
            if (!constant_expression_allowed(analyzer, expression->right) ||
                !fold_constant(analyzer, expression->right, TYPE_USIZE)) {
                semantic_error(analyzer, expression->right->first_token,
                               ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Array literal repetition count must be a positive compile-time integer constant");
                element_count = 0;
            } else {
                const char *text = expression->right->folded_constant.lexeme;
                char *end = NULL;
                unsigned long long value = strtoull(text ? text : "", &end, 10);
                if (text == NULL || end == text || *end != '\0' || value == 0 ||
                    value > SIZE_MAX) {
                    semantic_error(analyzer, expression->right->first_token,
                                   ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Array literal repetition count must be a positive compile-time integer constant");
                    element_count = 0;
                } else element_count = (size_t) value;
            }
        }
        if (expected.is_array && element_count != expected.resolved_array_length)
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                           ERR_TYPE_INCOMPATIBLE_TYPES,
                           "Array literal element count must match the fixed-array length");

        AstType element_type = expected;
        element_type.is_array = 0;
        element_type.is_slice = 0;
        element_type.array_length_token = AST_TOKEN_NONE;
        element_type.resolved_array_length = 0;
        for (AstExpression *element = expression->arguments; element != NULL;
             element = element->next)
            if (!array_literal_element_allowed(analyzer, element, &element_type))
                conversion_error(analyzer, element, analyzer->program, &element_type,
                                 NULL, "Cannot implicitly convert array literal element");

        expression->resolved_type = primitive_type(analyzer->program, &expected);
        expression->resolved_pointer_depth = expected.pointer_depth;
        expression->resolved_outer_pointer_depth = expected.outer_pointer_depth;
        expression->resolved_borrow_kind = expected.borrow_kind;
        expression->resolved_is_array = expected.is_array;
        expression->resolved_is_slice = expected.is_slice;
        expression->resolved_array_length = expected.is_array
                                                ? expected.resolved_array_length
                                                : 0;
        expression->literal_element_count = element_count;
        expression->owns_slice_backing = expected.is_slice;
        expression->resolved_named_type_token = named_type_token(analyzer->program, &expected);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(
            analyzer, analyzer->program, expression->resolved_named_type_token);
        expression->allocated_type = expected;
    } else if (expression->kind == AST_EXPR_LITERAL) {
        TokenType token = expression->value_token < analyzer->program->token_count
                              ? analyzer->program->tokens[expression->value_token].type
                              : TOKEN_ERROR;
        if (token == TOKEN_NUMBER) {
            unsigned long long value = strtoull(ast_program_lexeme(analyzer->program, expression->value_token), NULL,
                                                10);
            expression->resolved_type = value > INT64_MAX ? TYPE_U64 : value > INT32_MAX ? TYPE_I64 : TYPE_INT;
        } else if (token == TOKEN_FLOAT_LITERAL) expression->resolved_type = TYPE_DOUBLE;
        else if (token == TOKEN_CHAR_LITERAL) expression->resolved_type = TYPE_CHAR;
        else if (token == TOKEN_STRING_LITERAL) expression->resolved_type = TYPE_STRING;
    } else if (expression->kind == AST_EXPR_NAME) {
        const char *name = ast_program_lexeme(analyzer->program, expression->value_token);
        TokenType name_type = expression->value_token < analyzer->program->token_count
                                  ? analyzer->program->tokens[expression->value_token].type
                                  : TOKEN_ERROR;
        if (strcmp(name, "true") == 0 || strcmp(name, "false") == 0) {
            expression->resolved_type = TYPE_BIT;
        } else if (name_type >= TOKEN_TYPE_INT && name_type <= TOKEN_TYPE_VOID) {
            AstType type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            expression->resolved_type = primitive_type(analyzer->program, &type);
        } else if (is_builtin_name(name)) {
            expression->resolved_type = builtin_result_type(name);
            if (strcmp(name, "malloc") == 0) expression->resolved_pointer_depth = 1;
            const CoreIntrinsic *core = core_intrinsic_find(name);
            if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
        } else {
            const LocalSymbol *local = find_local(analyzer, expression->value_token);
            if (local != NULL) {
                expression->resolved_symbol_id = local->symbol_id;
                if (local->moved && analyzer->assignment_target != expression)
                    semantic_error(analyzer, expression->value_token,
                                   ERROR_CATEGORY_SEMANTIC, ERR_SEM_INVALID_DECLARATION,
                                   "Cannot read, borrow, copy, or move a value after ownership was moved");
                expression->resolved_type = local->resolved_type;
                expression->resolved_borrow_kind = local->resolved_borrow_kind;
                expression->resolved_pointer_depth = local->resolved_pointer_depth;
                expression->resolved_outer_pointer_depth = local->resolved_outer_pointer_depth;
                expression->resolved_named_type_token = local->resolved_named_type_token;
                expression->resolved_named_symbol_id = local->resolved_named_symbol_id;
                expression->resolved_is_array = local->resolved_is_array;
                expression->resolved_is_slice = local->resolved_is_slice;
            } else {
                const AstField *implicit_field = analyzer->current_owner_token == AST_TOKEN_NONE
                                                     ? NULL
                                                     : find_field(analyzer, analyzer->current_owner_token,
                                                                  expression->value_token);
                const SemanticSymbol *structure = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                     SEMANTIC_SYMBOL_STRUCT);
                const SemanticSymbol *enumeration = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                       SEMANTIC_SYMBOL_ENUM);
                const SemanticSymbol *function = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_FUNCTION);
                const SemanticSymbol *constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                                    SEMANTIC_SYMBOL_CONSTANT);
                if (!constant) constant = scoped_find_global(analyzer->model, analyzer->program, name,
                                                             SEMANTIC_SYMBOL_VARIABLE);
                if (implicit_field != NULL) {
                    expression->resolved_symbol_id = implicit_field->resolved_symbol_id;
                    expression->resolved_type = primitive_type(analyzer->program,
                                                               &implicit_field->type);
                    expression->resolved_borrow_kind = implicit_field->type.borrow_kind;
                    expression->resolved_pointer_depth = implicit_field->type.pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            implicit_field->type.outer_pointer_depth;
                    expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                             &implicit_field->type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        analyzer->program, expression->resolved_named_type_token);
                    expression->resolved_is_array = implicit_field->type.is_array;
                    expression->resolved_is_slice = implicit_field->type.is_slice;
                } else if (structure != NULL) {
                    expression->resolved_symbol_id = structure->id;
                    expression->resolved_named_type_token = structure->name_token;
                    expression->resolved_named_symbol_id = structure->id;
                } else if (enumeration != NULL) {
                    expression->resolved_symbol_id = enumeration->id;
                    expression->resolved_named_type_token = enumeration->name_token;
                    expression->resolved_named_symbol_id = enumeration->id;
                } else if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    expression->resolved_type = primitive_type(function->source_program,
                                                               &function->declared_type);
                } else if (constant != NULL) {
                    size_t constant_id = constant->id;
                    if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && !constant->declaration->
                        semantic_body_checked) {
                        AstProgram *saved = analyzer->program;
                        analyzer->program = (AstProgram *) constant->source_program;
                        analyze_constant_declaration(analyzer, (AstDeclarationNode *) constant->declaration);
                        analyzer->program = saved;
                        constant = &analyzer->model->symbols[constant_id];
                    } else if (constant->kind == SEMANTIC_SYMBOL_CONSTANT && constant->declaration && constant->
                               declaration->semantic_body_checked == 2) {
                        semantic_error(analyzer, expression->value_token, ERROR_CATEGORY_TYPE,
                                       ERR_TYPE_INVALID_OPERATION, "Constant initializer contains a dependency cycle");
                    }
                    expression->resolved_symbol_id = constant->id;
                    expression->resolved_type = constant->resolved_type;
                    expression->resolved_borrow_kind = constant->resolved_borrow_kind;
                    expression->resolved_pointer_depth = constant->resolved_pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            constant->resolved_outer_pointer_depth;
                    expression->resolved_named_type_token =
                            constant->resolved_named_type_token;
                    expression->resolved_named_symbol_id = constant->resolved_named_symbol_id;
                    expression->resolved_is_array = constant->resolved_is_array;
                    expression->resolved_is_slice = constant->resolved_is_slice;
                }
            }
        }
    } else if (expression->kind == AST_EXPR_UNARY) {
        if (expression->operator_type == TOKEN_BANG) expression->resolved_type = TYPE_BIT;
        else if (expression->right != NULL) {
            expression->resolved_type = expression->right->resolved_type;
            expression->resolved_borrow_kind = expression->right->resolved_borrow_kind;
            expression->resolved_pointer_depth = expression->right->resolved_pointer_depth;
            expression->resolved_outer_pointer_depth =
                    expression->right->resolved_outer_pointer_depth;
            expression->resolved_named_type_token = expression->right->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->right->resolved_named_symbol_id;
            expression->resolved_is_array = expression->right->resolved_is_array;
            expression->resolved_is_slice = expression->right->resolved_is_slice;
            if (expression->operator_type == TOKEN_AMPERSAND) {
                expression->resolved_borrow_kind = expression->mutable_borrow
                                                       ? AST_BORROW_MUTABLE
                                                       : AST_BORROW_IMMUTABLE;
                if (expression->resolved_is_array || expression->resolved_is_slice)
                    expression->resolved_outer_pointer_depth++;
                else expression->resolved_pointer_depth++;
            } else if (expression->operator_type == TOKEN_STAR) {
                expression->resolved_borrow_kind = AST_BORROW_NONE;
                if (expression->resolved_outer_pointer_depth > 0)
                    expression->resolved_outer_pointer_depth--;
                else if (expression->resolved_pointer_depth > 0)
                    expression->resolved_pointer_depth--;
            }
        }
    } else if (expression->kind == AST_EXPR_BINARY) {
        TokenType operation = expression->operator_type;
        if ((operation >= TOKEN_EQUAL_EQUAL && operation <= TOKEN_GREATER_EQUAL) ||
            operation == TOKEN_AMP_AMP || operation == TOKEN_PIPE_PIPE) {
            expression->resolved_type = TYPE_BIT;
        } else if (expression->left != NULL && expression->right != NULL) {
            if (operation == TOKEN_PLUS && (expression->left->resolved_type == TYPE_STRING ||
                                            expression->right->resolved_type == TYPE_STRING))
                expression->resolved_type = TYPE_STRING;
            else if (expression->left->resolved_type == TYPE_FLOAT &&
                     expression->right->resolved_type == TYPE_DOUBLE &&
                     expression->right->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else if (expression->right->resolved_type == TYPE_FLOAT &&
                     expression->left->resolved_type == TYPE_DOUBLE &&
                     expression->left->kind == AST_EXPR_LITERAL)
                expression->resolved_type = TYPE_FLOAT;
            else
                expression->resolved_type = promoted_numeric(expression->left->resolved_type,
                                                             expression->right->resolved_type);
        }
    } else if (expression->kind == AST_EXPR_CAST) {
        validate_array_shape(analyzer, &expression->allocated_type);
        const AstType *cast_type = &expression->allocated_type;
        expression->resolved_type = primitive_type(analyzer->program, cast_type);
        expression->resolved_pointer_depth = cast_type->pointer_depth;
        expression->resolved_outer_pointer_depth = cast_type->outer_pointer_depth;
        expression->resolved_named_type_token = named_type_token(analyzer->program, cast_type);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer, analyzer->program,
                                                                       expression->resolved_named_type_token);
        expression->resolved_is_array = cast_type->is_array;
        expression->resolved_is_slice = cast_type->is_slice;
        expression->resolved_array_length = cast_type->resolved_array_length;
    } else if (expression->kind == AST_EXPR_FREE) {
        expression->resolved_type = TYPE_VOID;
    } else if (expression->kind == AST_EXPR_CALL) {
        if (expression->left != NULL && expression->left->kind == AST_EXPR_NAME) {
            const char *name = ast_program_lexeme(analyzer->program, expression->left->value_token);
            TokenType callee_type = expression->left->value_token < analyzer->program->token_count
                                        ? analyzer->program->tokens[expression->left->value_token].type
                                        : TOKEN_ERROR;
            AstType cast_type = {
                .kind = AST_TYPE_NAMED,
                .name_token = expression->left->value_token,
                .array_length_token = AST_TOKEN_NONE
            };
            if (callee_type >= TOKEN_TYPE_INT && callee_type <= TOKEN_TYPE_VOID) {
                expression->resolved_type = primitive_type(analyzer->program, &cast_type);
            } else {
                int ambiguous = 0;
                const SemanticSymbol *function;
                if (expression->left->explicit_type_arguments) function = explicit_generic_function(
                                                                   analyzer, name, expression);
                else {
                    instantiate_generic_candidates(analyzer, name, expression->arguments);
                    function = resolve_overload(analyzer, name, AST_SYMBOL_NONE, 0, expression->arguments, &ambiguous);
                }
                (void) ambiguous;
                if (function != NULL) {
                    expression->resolved_symbol_id = function->id;
                    expression->resolved_type = primitive_type(function->source_program,
                                                               &function->declared_type);
                    expression->resolved_borrow_kind = function->declared_type.borrow_kind;
                    expression->resolved_pointer_depth = function->declared_type.pointer_depth;
                    expression->resolved_outer_pointer_depth =
                            function->declared_type.outer_pointer_depth;
                    expression->resolved_named_type_token = named_type_token(function->source_program,
                                                                             &function->declared_type);
                    expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                        function->source_program, expression->resolved_named_type_token);
                    expression->resolved_is_array = function->declared_type.is_array;
                    expression->resolved_is_slice = function->declared_type.is_slice;
                } else if (is_builtin_name(name)) {
                    expression->resolved_type = builtin_result_type(name);
                    if (strcmp(name, "malloc") == 0)
                        expression->resolved_pointer_depth = 1;
                    const CoreIntrinsic *core = core_intrinsic_find(name);
                    if (core != NULL && core->result == CORE_BYTES) expression->resolved_pointer_depth = 1;
                    if (strcmp(name, "read") == 0 && expression->arguments != NULL &&
                        expression->arguments->next != NULL) {
                        const AstExpression *format = expression->arguments->next;
                        const char *format_text = format->kind == AST_EXPR_LITERAL
                                                      ? ast_program_lexeme(analyzer->program, format->value_token)
                                                      : "";
                        expression->resolved_type = strcmp(format_text, "%c") == 0
                                                        ? TYPE_CHAR
                                                        : TYPE_INT;
                    }
                } else if (expression->left->resolved_named_type_token != AST_TOKEN_NONE) {
                    expression->resolved_named_type_token =
                            expression->left->resolved_named_type_token;
                    expression->resolved_named_symbol_id =
                            expression->left->resolved_named_symbol_id;
                }
            }
        } else if (expression->left != NULL && expression->left->kind == AST_EXPR_MEMBER &&
                   expression->left->left != NULL) {
            const AstExpression *receiver = expression->left->left;
            int enum_type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                     analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                     SEMANTIC_SYMBOL_ENUM;
            const AstEnumValue *access_variant = !enum_type_receiver &&
                                                 receiver->resolved_pointer_depth == 0 && !receiver->resolved_is_array
                                                 && !receiver->resolved_is_slice
                                                     ? find_enum_value_by_symbol(
                                                         analyzer, receiver->resolved_named_symbol_id,
                                                         expression->left->value_token)
                                                     : NULL;
            if (access_variant != NULL) {
                const SemanticSymbol *variant = &analyzer->model->symbols[access_variant->resolved_symbol_id];
                const AstTypeArgument *payload = access_variant->payload_types;
                expression->kind = AST_EXPR_ENUM_ACCESS;
                expression->resolved_symbol_id = variant->id;
                if (!payload || payload->next) {
                    semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Enum payload accessor requires a variant with exactly one payload");
                    return;
                }
                expression->resolved_type = primitive_type(variant->source_program, &payload->type);
                expression->resolved_borrow_kind = payload->type.borrow_kind;
                expression->resolved_pointer_depth = payload->type.pointer_depth;
                expression->resolved_outer_pointer_depth = payload->type.outer_pointer_depth;
                expression->resolved_named_type_token = named_type_token(variant->source_program, &payload->type);
                expression->resolved_named_symbol_id = resolve_named_symbol_id(
                    analyzer, variant->source_program, expression->resolved_named_type_token);
                expression->resolved_is_array = payload->type.is_array;
                expression->resolved_is_slice = payload->type.is_slice;
                return;
            }
            if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
                analyzer->model->symbols[expression->left->resolved_symbol_id].kind == SEMANTIC_SYMBOL_ENUM_VALUE) {
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
                expression->resolved_symbol_id = expression->left->resolved_symbol_id;
                expression->resolved_named_symbol_id = receiver->resolved_named_symbol_id;
                expression->resolved_named_type_token = receiver->resolved_named_type_token;
                return;
            }
            int type_receiver = receiver->resolved_symbol_id < analyzer->model->symbol_count &&
                                (analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_STRUCT ||
                                 analyzer->model->symbols[receiver->resolved_symbol_id].kind ==
                                 SEMANTIC_SYMBOL_ENUM);
            int ambiguous = 0;
            const SemanticSymbol *method = resolve_overload(analyzer,
                                                            ast_program_lexeme(
                                                                analyzer->program, expression->left->value_token),
                                                            receiver->resolved_named_symbol_id, type_receiver,
                                                            expression->arguments, &ambiguous);
            (void) ambiguous;
            if (method != NULL) {
                expression->resolved_symbol_id = method->id;
                expression->resolved_type = primitive_type(method->source_program,
                                                           &method->declared_type);
                expression->resolved_borrow_kind = method->declared_type.borrow_kind;
                expression->resolved_pointer_depth = method->declared_type.pointer_depth;
                expression->resolved_outer_pointer_depth =
                        method->declared_type.outer_pointer_depth;
                expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                         &method->declared_type);
                expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                               method->source_program,
                                                                               expression->resolved_named_type_token);
                expression->resolved_is_array = method->declared_type.is_array;
                expression->resolved_is_slice = method->declared_type.is_slice;
            }
        }
        if (expression->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *callee =
                &analyzer->model->symbols[expression->resolved_symbol_id];
            if (callee->kind == SEMANTIC_SYMBOL_FUNCTION &&
                callee->declaration != NULL &&
                callee->declaration->as.function.returns_owned_slice_backing)
                expression->owns_slice_backing = 1;
        }
    } else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL) {
        expression->resolved_type = expression->left->resolved_type;
        expression->resolved_borrow_kind = AST_BORROW_NONE;
        expression->resolved_pointer_depth = expression->left->resolved_pointer_depth;
        expression->resolved_outer_pointer_depth =
                expression->left->resolved_outer_pointer_depth;
        expression->resolved_named_type_token = expression->left->resolved_named_type_token;
        expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
        expression->resolved_is_array = expression->left->resolved_is_array;
        expression->resolved_is_slice = expression->left->resolved_is_slice;
        if (expression->resolved_outer_pointer_depth > 0)
            expression->resolved_outer_pointer_depth--;
        else if (expression->resolved_is_array || expression->resolved_is_slice) {
            expression->resolved_is_array = 0;
            expression->resolved_is_slice = 0;
        } else if (expression->resolved_pointer_depth > 0)
            expression->resolved_pointer_depth--;
    } else if (expression->kind == AST_EXPR_MEMBER && expression->left != NULL) {
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "length")) {
            expression->kind = AST_EXPR_SLICE_LENGTH;
            expression->resolved_type = TYPE_USIZE;
            return;
        }
        if (expression->left->resolved_is_slice && !expression->left->resolved_outer_pointer_depth &&
            same_name(analyzer->program, expression->value_token, "data")) {
            expression->kind = AST_EXPR_SLICE_DATA;
            expression->resolved_type = expression->left->resolved_type;
            expression->resolved_pointer_depth = expression->left->resolved_pointer_depth + 1;
            expression->resolved_named_type_token = expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
            return;
        }
        const AstEnumValue *value = NULL;
        if (expression->left->resolved_symbol_id < analyzer->model->symbol_count &&
            analyzer->model->symbols[expression->left->resolved_symbol_id].kind ==
            SEMANTIC_SYMBOL_ENUM)
            value = find_enum_value_by_symbol(analyzer,
                                              expression->left->resolved_named_symbol_id, expression->value_token);
        if (value != NULL) {
            expression->resolved_symbol_id = value->resolved_symbol_id;
            expression->resolved_named_type_token =
                    expression->left->resolved_named_type_token;
            expression->resolved_named_symbol_id = expression->left->resolved_named_symbol_id;
            const SemanticSymbol *enum_symbol = &analyzer->model->symbols[expression->resolved_named_symbol_id];
            if (enum_symbol->declaration->as.enum_decl.is_sum && value->payload_types == NULL)
                expression->kind = AST_EXPR_ENUM_CONSTRUCT;
            return;
        }
        const SemanticSymbol *method = find_method(analyzer,
                                                   expression->left->resolved_named_symbol_id, expression->value_token);
        if (method != NULL) {
            expression->resolved_symbol_id = method->id;
            expression->resolved_type = primitive_type(method->source_program,
                                                       &method->declared_type);
            expression->resolved_pointer_depth = method->declared_type.pointer_depth;
            expression->resolved_outer_pointer_depth =
                    method->declared_type.outer_pointer_depth;
            expression->resolved_named_type_token = named_type_token(method->source_program,
                                                                     &method->declared_type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                           method->source_program,
                                                                           expression->resolved_named_type_token);
            return;
        }
        const AstField *field = find_field_by_symbol(analyzer,
                                                     expression->left->resolved_named_symbol_id,
                                                     expression->value_token);
        if (field != NULL) {
            const SemanticSymbol *field_symbol = field->resolved_symbol_id <
                                                 analyzer->model->symbol_count
                                                     ? &analyzer->model->symbols[field->resolved_symbol_id]
                                                     : NULL;
            const AstProgram *field_program = field_symbol == NULL
                                                  ? analyzer->program
                                                  : field_symbol->source_program;
            expression->resolved_type = primitive_type(field_program, &field->type);
            expression->resolved_borrow_kind = field->type.borrow_kind;
            expression->resolved_pointer_depth = field->type.pointer_depth;
            expression->resolved_outer_pointer_depth = field->type.outer_pointer_depth;
            expression->resolved_named_type_token = named_type_token(field_program, &field->type);
            expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                           field_program,
                                                                           expression->resolved_named_type_token);
            expression->resolved_is_array = field->type.is_array;
            expression->resolved_is_slice = field->type.is_slice;
            expression->resolved_symbol_id = field->resolved_symbol_id;
        }
    } else if (expression->kind == AST_EXPR_RESERVE) {
        validate_array_shape(analyzer, &expression->allocated_type);
        const AstType *reserved_type = &expression->allocated_type;
        expression->resolved_type = primitive_type(analyzer->program, reserved_type);
        expression->resolved_named_type_token = named_type_token(analyzer->program,
                                                                 reserved_type);
        expression->resolved_named_symbol_id = resolve_named_symbol_id(analyzer,
                                                                       analyzer->program,
                                                                       expression->resolved_named_type_token);
        expression->resolved_pointer_depth = reserved_type->pointer_depth;
        expression->resolved_outer_pointer_depth = reserved_type->outer_pointer_depth;
        expression->resolved_is_array = reserved_type->is_array;
        expression->resolved_is_slice = reserved_type->is_slice;
        if (reserved_type->is_array || reserved_type->is_slice)
            expression->resolved_outer_pointer_depth++;
        else expression->resolved_pointer_depth++;
    }
    if (expression->kind == AST_EXPR_UNARY && expression->right != NULL)
        expression->resolved_array_length = expression->right->resolved_array_length;
    else if (expression->kind == AST_EXPR_INDEX && expression->left != NULL)
        expression->resolved_array_length = expression->resolved_is_array ? expression->left->resolved_array_length : 0;
    else if (expression->kind == AST_EXPR_CAST)
        expression->resolved_array_length = expression->allocated_type.resolved_array_length;
    else if (expression->kind == AST_EXPR_RESERVE)
        expression->resolved_array_length = expression->allocated_type.resolved_array_length;
    else if (expression->resolved_symbol_id < analyzer->model->symbol_count)
        expression->resolved_array_length = analyzer->model->symbols[expression->resolved_symbol_id].declared_type.
                resolved_array_length;
    if (expression->resolved_type == TYPE_UNKNOWN &&
        expression->resolved_named_type_token == AST_TOKEN_NONE &&
        expression->resolved_symbol_id == AST_SYMBOL_NONE &&
        expression->kind != AST_EXPR_RESERVE &&
        !(expression->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program, expression->value_token))) &&
        !(expression->kind == AST_EXPR_CALL && expression->left != NULL &&
          expression->left->kind == AST_EXPR_NAME &&
          is_builtin_name(ast_program_lexeme(analyzer->program,
                                             expression->left->value_token))))
        analyzer->model->unresolved_expression_count++;
}
