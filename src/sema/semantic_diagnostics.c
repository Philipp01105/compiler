#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void semantic_error_at(Analyzer *analyzer, size_t token, size_t last_token, char category, int code,
                              const char *message) {
    const AstToken *location = ast_program_token(analyzer->program, token);
    const AstToken *end = ast_program_token(analyzer->program, last_token);
    char named_message[1024];
    if (location != NULL && ((category == ERROR_CATEGORY_TYPE && code == ERR_TYPE_UNKNOWN) ||
                             (category == ERROR_CATEGORY_SEMANTIC &&
                              (code == ERR_SEM_UNDEFINED_VARIABLE || code == ERR_SEM_DUPLICATE_DEFINITION ||
                               code == ERR_SEM_FIELD_NOT_FOUND)))) {
        snprintf(named_message, sizeof(named_message), "%s '%s'", message, location->lexeme);
        message = named_message;
    }
    ErrorContext *context = error_context_create(SEVERITY_ERROR,
                                                 location == NULL ? 0 : location->span.begin.line,
                                                 location == NULL ? 0 : location->span.begin.column,
                                                 category, code, analyzer->program->source_path,
                                                 message);
    if (location != NULL) {
        error_context_set_span(context, end == NULL ? location->span.end.line : end->span.end.line,
                               end == NULL ? location->span.end.column : end->span.end.column);
        error_context_set_token(context, location->lexeme);
    }
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
    analyzer->model->error_count++;
}

void semantic_error(Analyzer *analyzer, size_t token, char category, int code,
                           const char *message) {
    semantic_error_at(analyzer, token, token, category, code, message);
}


void semantic_duplicate(Analyzer *analyzer, size_t token,
                               const AstProgram *previous_program, size_t previous_token,
                               const char *message) {
    const AstToken *location = ast_program_token(analyzer->program, token);
    const AstToken *previous = ast_program_token(previous_program, previous_token);
    char detail[1024];
    snprintf(detail, sizeof(detail), "%s '%s'", message, ast_program_lexeme(analyzer->program, token));
    ErrorContext *context = error_context_create(SEVERITY_ERROR,
                                                 location == NULL ? 0 : location->span.begin.line,
                                                 location == NULL ? 0 : location->span.begin.column,
                                                 ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                                 analyzer->program->source_path, detail);
    if (location != NULL) error_context_set_span(context, location->span.end.line, location->span.end.column);
    if (previous != NULL) {
        ErrorContext *note = error_context_create(SEVERITY_INFO, previous->span.begin.line, previous->span.begin.column,
                                                  ERROR_CATEGORY_SEMANTIC, ERR_SEM_DUPLICATE_DEFINITION,
                                                  previous_program->source_path,
                                                  "Previous declaration is here");
        error_context_set_span(note, previous->span.end.line, previous->span.end.column);
        error_context_add_child(context, note);
    }
    error_report_context(global_error_handler, context);
    if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
    analyzer->model->error_count++;
}

void diagnostic_append(DiagnosticText *text, const char *format, ...) {
    if (text->failed) return;
    va_list arguments, copy;
    va_start(arguments, format);
    va_copy(copy, arguments);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || (size_t) count > SIZE_MAX - text->used - 1) text->failed = 1;
    else {
        size_t needed = text->used + (size_t) count + 1;
        char *grown = realloc(text->text, needed);
        if (grown == NULL) text->failed = 1;
        else {
            text->text = grown;
            (void) vsnprintf(text->text + text->used, needed - text->used, format, arguments);
            text->used += (size_t) count;
        }
    }
    va_end(arguments);
}

void diagnostic_type(DiagnosticText *text, const Analyzer *analyzer,
                            DataType primitive, size_t nominal, unsigned pointers,
                            unsigned outer, int array, int slice, const char *length) {
    static const char *names[] = {DMM_TYPE_NAMES};
    for (unsigned i = 0; i < outer; i++) diagnostic_append(text, "*");
    if (outer != 0 && (array || slice)) diagnostic_append(text, "(");
    for (unsigned i = 0; i < pointers; i++) diagnostic_append(text, "*");
    if (nominal < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[nominal];
        const AstDeclarationNode *declaration = symbol->declaration;
        if (declaration && declaration->generic_origin) {
            diagnostic_append(text, "%s<",
                              ast_program_lexeme(symbol->source_program, declaration->generic_origin->name_token));
            for (const AstTypeArgument *argument = declaration->specialization_arguments; argument;
                 argument = argument->next) {
                const AstType *type = &argument->type;
                char size[32];
                snprintf(size, sizeof(size), "%zu", type->resolved_array_length);
                diagnostic_type(text, analyzer, primitive_type(symbol->source_program, type),
                                resolve_named_symbol_id(analyzer, symbol->source_program,
                                                        named_type_token(symbol->source_program, type)),
                                type->pointer_depth, type->outer_pointer_depth, type->is_array, type->is_slice, size);
                if (argument->next) diagnostic_append(text, ",");
            }
            diagnostic_append(text, ">");
        } else diagnostic_append(text, "%s", ast_program_lexeme(symbol->source_program, symbol->name_token));
    } else diagnostic_append(text, "%s", primitive <= TYPE_UNKNOWN ? names[primitive] : "unknown");
    if (slice) diagnostic_append(text, "[]");
    else if (array) diagnostic_append(text, "[%s]", length == NULL ? "?" : length);
    if (outer != 0 && (array || slice)) diagnostic_append(text, ")");
}

static void diagnostic_ast_type(DiagnosticText *text, const Analyzer *analyzer,
                                const AstProgram *program,
                                const AstType *type) {
    if (type->element_type != NULL) {
        for (unsigned i = 0;
             i < type->pointer_depth + type->outer_pointer_depth +
                     (type->borrow_kind != AST_BORROW_NONE);
             i++)
            diagnostic_append(text, "*");
        diagnostic_ast_type(text, analyzer, program, type->element_type);
        if (type->is_slice) diagnostic_append(text, "[]");
        else if (type->is_array)
            diagnostic_append(text, "[%zu]", type->resolved_array_length);
        return;
    }
    if (type->kind == AST_TYPE_FUTURE || type->kind == AST_TYPE_JOIN) {
        if (type->borrow_kind != AST_BORROW_NONE)
            diagnostic_append(text, type->borrow_kind == AST_BORROW_MUTABLE ? "&mut " : "&");
        for (unsigned i = 0; i < type->pointer_depth + type->outer_pointer_depth; i++)
            diagnostic_append(text, "*");
        diagnostic_append(text, type->kind == AST_TYPE_JOIN ? "JoinHandle<" : "Future<");
        if (type->arguments != NULL)
            diagnostic_ast_type(text, analyzer, program, &type->arguments->type);
        diagnostic_append(text, ">");
        if (type->is_slice) diagnostic_append(text, "[]");
        else if (type->is_array) diagnostic_append(text, "[%zu]", type->resolved_array_length);
        return;
    }
    if (type->kind == AST_TYPE_FUNCTION) {
        for (unsigned i = 0; i < type->pointer_depth + type->outer_pointer_depth; i++)
            diagnostic_append(text, "*");
        diagnostic_append(text, "func");
        if (type->function_generic_parameters) {
            diagnostic_append(text, "<");
            for (const AstGenericParameter *g = type->function_generic_parameters; g; g = g->next) {
                diagnostic_append(text, "%s", ast_program_lexeme(program, g->name_token));
                if (g->bounds) {
                    diagnostic_append(text, ": ");
                    for (const AstInterfaceBound *bound = g->bounds; bound; bound = bound->next) {
                        diagnostic_append(text, "%s", ast_program_lexeme(program, bound->name_token));
                        if (bound->next) diagnostic_append(text, " + ");
                    }
                }
                if (g->next) diagnostic_append(text, ", ");
            }
            diagnostic_append(text, ">");
        }
        diagnostic_append(text, "(");
        for (const AstTypeArgument *parameter = type->function_parameters; parameter; parameter = parameter->next) {
            diagnostic_ast_type(text, analyzer, program, &parameter->type);
            if (parameter->next) diagnostic_append(text, ", ");
        }
        diagnostic_append(text, ") -> ");
        if (type->function_return_type)
            diagnostic_ast_type(text, analyzer, program, type->function_return_type);
        else diagnostic_append(text, "unknown");
        return;
    }
    const char *length = type->is_array
                             ? ast_program_lexeme(program,
                                                  type->array_length_token)
                             : NULL;
    diagnostic_type(text, analyzer, primitive_type(program, type),
                    resolve_named_symbol_id(
                        analyzer, program, named_type_token(program, type)),
                    type->pointer_depth +
                        (type->borrow_kind != AST_BORROW_NONE),
                    type->outer_pointer_depth, type->is_array,
                    type->is_slice, length);
}

/* Preserve the rule text while showing every supplied operand's complete shape. */
void operand_error(Analyzer *analyzer, const AstExpression *expression,
                          char category, int code, const char *reason) {
    DiagnosticText message = {0};
    diagnostic_append(&message, "%s; supplied types:", reason);
    const AstExpression *operands[2] = {expression->left, expression->right};
    if (operands[0] == NULL && operands[1] == NULL) operands[0] = expression;
    if (expression->kind == AST_EXPR_CAST || expression->kind == AST_EXPR_FREE)
        operands[0] = expression->arguments;
    for (size_t i = 0; i < 2; i++) {
        const AstExpression *operand = operands[i];
        if (operand == NULL) continue;
        char length[32];
        snprintf(length, sizeof(length), "%zu", operand->resolved_array_length);
        diagnostic_append(&message, " '");
        diagnostic_type(&message, analyzer, operand->resolved_type,
                        operand->resolved_named_symbol_id, operand->resolved_pointer_depth,
                        operand->resolved_outer_pointer_depth, operand->resolved_is_array,
                        operand->resolved_is_slice, length);
        diagnostic_append(&message, "'");
    }
    semantic_error_at(analyzer, expression->first_token,
                      expression->token_count == 0
                          ? expression->first_token
                          : expression->first_token + expression->token_count - 1,
                      category, code, message.failed ? reason : message.text);
    free(message.text);
}

void conversion_error(Analyzer *analyzer, const AstExpression *value,
                             const AstProgram *expected_program, const AstType *expected,
                             const AstExpression *expected_value, const char *reason) {
    // An unresolved value already has a name/type diagnostic; avoid a follow-up conversion error.
    if (value->resolved_type == TYPE_UNKNOWN && value->resolved_named_symbol_id == AST_SYMBOL_NONE &&
          !(value->has_resolved_ast_type && (value->resolved_ast_type.kind == AST_TYPE_FUNCTION ||
                                           value->resolved_ast_type.kind == AST_TYPE_FUTURE))) return;
    DiagnosticText message = {0};
    diagnostic_append(&message, "%s; got '", reason);
    const char *length = NULL;
    if (value->resolved_symbol_id < analyzer->model->symbol_count) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[value->resolved_symbol_id];
        length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
    }
    if (value->has_resolved_ast_type &&
        (value->resolved_ast_type.element_type != NULL ||
           value->resolved_ast_type.kind == AST_TYPE_FUNCTION ||
           value->resolved_ast_type.kind == AST_TYPE_FUTURE))
        diagnostic_ast_type(
            &message, analyzer,
            value->resolved_type_program != NULL
                ? value->resolved_type_program : analyzer->program,
            &value->resolved_ast_type);
    else
        diagnostic_type(&message, analyzer, value->resolved_type,
                        value->resolved_named_symbol_id,
                        value->resolved_pointer_depth,
                        value->resolved_outer_pointer_depth,
                        value->resolved_is_array, value->resolved_is_slice,
                        length);
    diagnostic_append(&message, "', expected '");
    if (expected_value != NULL) {
        const char *expected_length = NULL;
        if (expected_value->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *symbol = &analyzer->model->symbols[expected_value->resolved_symbol_id];
            expected_length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
        }
        if (expected_value->resolved_type == TYPE_UNKNOWN && expected_value->resolved_named_symbol_id ==
            AST_SYMBOL_NONE && !semantic_expression_is_future(expected_value)) {
            free(message.text);
            return;
        }
        if (expected_value->has_resolved_ast_type &&
            (expected_value->resolved_ast_type.element_type != NULL ||
             expected_value->resolved_ast_type.kind == AST_TYPE_FUNCTION ||
             expected_value->resolved_ast_type.kind == AST_TYPE_FUTURE))
            diagnostic_ast_type(
                &message, analyzer,
                expected_value->resolved_type_program != NULL
                    ? expected_value->resolved_type_program
                    : analyzer->program,
                &expected_value->resolved_ast_type);
        else
            diagnostic_type(&message, analyzer,
                            expected_value->resolved_type,
                            expected_value->resolved_named_symbol_id,
                            expected_value->resolved_pointer_depth,
                            expected_value->resolved_outer_pointer_depth,
                            expected_value->resolved_is_array,
                            expected_value->resolved_is_slice,
                            expected_length);
    } else
        diagnostic_ast_type(&message, analyzer, expected_program, expected);
    diagnostic_append(&message, "'");
    semantic_error_at(analyzer, value->first_token,
                      value->token_count == 0 ? value->first_token : value->first_token + value->token_count - 1,
                      ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                      message.failed ? reason : message.text);
    free(message.text);
}

void overload_error(Analyzer *analyzer, const AstExpression *call,
                           const char *name, size_t owner, int is_static, int ambiguous) {
    DiagnosticText message = {0};
    diagnostic_append(&message, owner == AST_SYMBOL_NONE
                                    ? (ambiguous
                                           ? "Call to '%s' is ambiguous"
                                           : "No overload of '%s' matches the supplied arguments")
                                    : (ambiguous
                                           ? "Method call to '%s' is ambiguous"
                                           : "No method overload matches the supplied arguments for '%s'"), name);
    diagnostic_append(&message, "; supplied types: (");
    size_t index = 0;
    for (const AstExpression *argument = call->arguments; argument != NULL; argument = argument->next) {
        if (index++ != 0) diagnostic_append(&message, ", ");
        const char *length = NULL;
        if (argument->resolved_symbol_id < analyzer->model->symbol_count) {
            const SemanticSymbol *symbol = &analyzer->model->symbols[argument->resolved_symbol_id];
            if (symbol->declared_type.is_array)
                length = ast_program_lexeme(symbol->source_program, symbol->declared_type.array_length_token);
        }
        if (argument->has_resolved_ast_type &&
            argument->resolved_ast_type.element_type != NULL)
            diagnostic_ast_type(
                &message, analyzer,
                argument->resolved_type_program != NULL
                    ? argument->resolved_type_program : analyzer->program,
                &argument->resolved_ast_type);
        else
            diagnostic_type(&message, analyzer, argument->resolved_type,
                            argument->resolved_named_symbol_id,
                            argument->resolved_pointer_depth,
                            argument->resolved_outer_pointer_depth,
                            argument->resolved_is_array,
                            argument->resolved_is_slice, length);
    }
    diagnostic_append(&message, ambiguous ? "); viable signatures: " : "); available signatures: ");
    index = 0;
    for (size_t i = 0; i < analyzer->model->symbol_count; i++) {
        const SemanticSymbol *symbol = &analyzer->model->symbols[i];
        if (symbol->kind != SEMANTIC_SYMBOL_FUNCTION || symbol->owner_symbol_id != owner ||
            !same_name(symbol->source_program, symbol->name_token, name) || symbol->declaration == NULL ||
            (owner != AST_SYMBOL_NONE && symbol->declaration->as.function.is_static != is_static) ||
            (ambiguous && !viable_function(analyzer, symbol, call->arguments)))
            continue;
        if (index++ != 0) diagnostic_append(&message, "; ");
        if (owner < analyzer->model->symbol_count) {
            const SemanticSymbol *owner_symbol = &analyzer->model->symbols[owner];
            diagnostic_append(&message, "%s.",
                              ast_program_lexeme(owner_symbol->source_program, owner_symbol->name_token));
        }
        diagnostic_append(&message, "%s(", name);
        size_t parameter_index = 0;
        for (const AstParameter *parameter = symbol->declaration->as.function.parameters;
             parameter != NULL; parameter = parameter->next) {
            if (parameter_index++ != 0) diagnostic_append(&message, ", ");
            const AstType *type = &parameter->type;
            diagnostic_ast_type(&message, analyzer, symbol->source_program,
                                type);
        }
        diagnostic_append(&message, ")");
        for (const AstAutoCondition *c = symbol->declaration->where_conditions; c; c = c->next) {
            diagnostic_append(&message, c == symbol->declaration->where_conditions ? " where " : ", ");
            diagnostic_ast_type(&message, analyzer, symbol->source_program, &c->type);
            diagnostic_append(&message, ": ");
            for (const AstInterfaceBound *b = c->bounds; b; b = b->next) {
                diagnostic_append(&message, "%s", ast_program_lexeme(symbol->source_program, b->name_token));
                if (b->next) diagnostic_append(&message, " + ");
            }
            if (!semantic_method_constraints_satisfied(analyzer, symbol->source_program, symbol->declaration))
                diagnostic_append(&message, " (not satisfied)");
        }
    }
    if (index == 0) diagnostic_append(&message, "<none>");
    semantic_error(analyzer, call->left->value_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INCOMPATIBLE_TYPES,
                   message.failed ? "Could not format overload diagnostic" : message.text);
    free(message.text);
}
