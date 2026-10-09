#include "semantic_internal.h"
#include "errorHandler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int expression_is_constant_symbol(const Analyzer *analyzer,
                                  const AstExpression *expression) {
    return expression != NULL && expression->resolved_symbol_id < analyzer->model->symbol_count &&
           analyzer->model->symbols[expression->resolved_symbol_id].kind ==
           SEMANTIC_SYMBOL_CONSTANT;
}

int constant_expression_allowed(const Analyzer *analyzer,
                                const AstExpression *expression) {
    if (expression == NULL) return 0;
    if (expression->kind == AST_EXPR_LITERAL) return 1;
    if (expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF || expression->kind ==
        AST_EXPR_TYPE_PROPERTY)
        return expression->folded_constant.lexeme != NULL;
    if (expression->kind == AST_EXPR_NAME)
        return same_name(analyzer->program, expression->value_token, "true") ||
               same_name(analyzer->program, expression->value_token, "false") ||
               (expression->has_resolved_ast_type &&
                expression->resolved_ast_type.kind == AST_TYPE_FUNCTION &&
                expression->resolved_callable != NULL) ||
               expression_is_constant_symbol(analyzer, expression);
    if (expression->kind == AST_EXPR_CAST)
        return expression->arguments != NULL && expression->arguments->next == NULL &&
               constant_expression_allowed(analyzer, expression->arguments);
    if (expression->kind == AST_EXPR_UNARY)
        return (expression->operator_type == TOKEN_MINUS ||
                expression->operator_type == TOKEN_BANG) &&
               constant_expression_allowed(analyzer, expression->right);
    if (expression->kind == AST_EXPR_BINARY)
        return constant_expression_allowed(analyzer, expression->left) &&
               constant_expression_allowed(analyzer, expression->right);
    return 0;
}

const AstExpression *constant_initializer(const SemanticSymbol *symbol) {
    return symbol->declaration != NULL
               ? symbol->declaration->as.constant.value
               : (const AstExpression *) symbol->node;
}

static double folded_number(const AstExpression *expression) {
    const AstToken *value = &expression->folded_constant;
    if (value->type == TOKEN_CHAR_LITERAL) return (unsigned char) value->lexeme[0];
    if (strcmp(value->lexeme, "true") == 0) return 1;
    return strtod(value->lexeme, NULL);
}

static const char *folded_string(const AstExpression *expression, char buffer[64]) {
    if (expression->resolved_type == TYPE_STRING) return expression->folded_constant.lexeme;
    if (data_type_fixed_integer(expression->resolved_type)) {
        uint64_t bits = strtoull(expression->folded_constant.lexeme, NULL, 10);
        int64_t value;
        memcpy(&value, &bits, sizeof(value));
        if (data_type_unsigned(expression->resolved_type)) snprintf(buffer, 64, "%llu", (unsigned long long) bits);
        else snprintf(buffer, 64, "%lld", (long long) value);
        return buffer;
    }
    double number = folded_number(expression);
    if (expression->resolved_type == TYPE_CHAR)
        (void) snprintf(buffer, 64, "%c", (unsigned char) (long long) number);
    else if (expression->resolved_type == TYPE_FLOAT || expression->resolved_type == TYPE_DOUBLE)
        (void) snprintf(buffer, 64, "%f", number);
    else (void) snprintf(buffer, 64, "%lld", (long long) number);
    return buffer;
}

/* Keep fixed-width constants exact: binary64 cannot represent every u64/i64. */
int fold_constant(Analyzer *analyzer, AstExpression *expression, DataType target);

static int fold_integer_bits(Analyzer *analyzer, const AstExpression *expression, uint64_t *bits) {
    if (expression == NULL || !data_type_integral(expression->resolved_type)) return 0;
    if ((expression->kind == AST_EXPR_SIZEOF || expression->kind == AST_EXPR_ALIGNOF || expression->kind ==
         AST_EXPR_TYPE_PROPERTY) && expression->folded_constant.lexeme && integral_expression(expression)) {
        *bits = strtoull(expression->folded_constant.lexeme, NULL, 10);
    } else if (expression->kind == AST_EXPR_LITERAL) {
        const AstToken *token = ast_program_token(analyzer->program, expression->value_token);
        if (token->type == TOKEN_CHAR_LITERAL) *bits = (unsigned char) token->lexeme[0];
        else *bits = strtoull(token->lexeme, NULL, 10);
    } else if (expression->kind == AST_EXPR_NAME) {
        if (same_name(analyzer->program, expression->value_token, "true")) *bits = 1;
        else if (same_name(analyzer->program, expression->value_token, "false")) *bits = 0;
        else {
            if (!expression_is_constant_symbol(analyzer, expression)) return 0;
            const AstExpression *initializer = constant_initializer(
                &analyzer->model->symbols[expression->resolved_symbol_id]);
            if (!initializer || !initializer->folded_constant.lexeme) return 0;
            const AstToken *value = &initializer->folded_constant;
            *bits = value->type == TOKEN_CHAR_LITERAL
                        ? (unsigned char) value->lexeme[0]
                        : !strcmp(value->lexeme, "true")
                              ? 1
                              : strtoull(value->lexeme, NULL, 10);
        }
    } else if (expression->kind == AST_EXPR_CAST) {
        const AstExpression *source = expression->arguments;
        if (source && (source->resolved_type == TYPE_FLOAT || source->resolved_type == TYPE_DOUBLE)) {
            if (!source->folded_constant.lexeme &&
                !fold_constant(analyzer, (AstExpression *) source, source->resolved_type))
                return 0;
            const char *text = source->folded_constant.lexeme
                                   ? source->folded_constant.lexeme
                                   : ast_program_lexeme(analyzer->program, source->value_token);
            double value = strtod(text, NULL);
            if (data_type_unsigned(expression->resolved_type) && data_type_bytes(expression->resolved_type) == 8) {
                if (!(value >= 0 && value < 0x1p64)) return 0;
                *bits = (uint64_t) value;
            } else {
                if (!(value >= -0x1p63 && value < 0x1p63)) return 0;
                *bits = (uint64_t)(int64_t)
                value;
            }
        } else if (!fold_integer_bits(analyzer, source, bits)) return 0;
        if (expression->resolved_type == TYPE_BIT) *bits = (*bits != 0);
        else if (expression->resolved_type == TYPE_BYTE || expression->resolved_type == TYPE_CHAR) *bits &= 255;
    } else if (expression->kind == AST_EXPR_UNARY) {
        if (!fold_integer_bits(analyzer, expression->right, bits)) return 0;
        if (expression->operator_type == TOKEN_MINUS) *bits = 0 - *bits;
        else if (expression->operator_type == TOKEN_BANG) *bits = (*bits == 0);
        else return 0;
    } else if (expression->kind == AST_EXPR_BINARY) {
        uint64_t a, b;
        if (!fold_integer_bits(analyzer, expression->left, &a)) return 0;
        if (expression->operator_type == TOKEN_AMP_AMP && !a) {
            *bits = 0;
            return 1;
        }
        if (expression->operator_type == TOKEN_PIPE_PIPE && a) {
            *bits = 1;
            return 1;
        }
        if (!fold_integer_bits(analyzer, expression->right, &b)) return 0;
        DataType operation = data_type_promoted_integer(expression->left->resolved_type,
                                                        expression->right->resolved_type);
        if (data_type_fixed_integer(operation)) {
            a = data_type_normalize_integer(a, operation);
            b = data_type_normalize_integer(b, operation);
        }
        int64_t x, y;
        memcpy(&x, &a, sizeof(x));
        memcpy(&y, &b, sizeof(y));
        int unsign = data_type_fixed_integer(operation) && data_type_unsigned(operation);
        switch (expression->operator_type) {
            case TOKEN_PLUS: *bits = a + b;
                break;
            case TOKEN_MINUS: *bits = a - b;
                break;
            case TOKEN_STAR: *bits = a * b;
                break;
            case TOKEN_SLASH:
            case TOKEN_PERCENT:
                if (!b || (!unsign && x == INT64_MIN && y == -1)) {
                    semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                                   "Invalid division in constant expression");
                    return 0;
                }
                *bits = unsign
                            ? (expression->operator_type == TOKEN_SLASH ? a / b : a % b)
                            : (uint64_t)(expression->operator_type == TOKEN_SLASH ? x / y : x % y);
                break;
            case TOKEN_EQUAL_EQUAL: *bits = a == b;
                break;
            case TOKEN_BANG_EQUAL: *bits = a != b;
                break;
            case TOKEN_LESS: *bits = unsign ? a < b : x < y;
                break;
            case TOKEN_LESS_EQUAL: *bits = unsign ? a <= b : x <= y;
                break;
            case TOKEN_GREATER: *bits = unsign ? a > b : x > y;
                break;
            case TOKEN_GREATER_EQUAL: *bits = unsign ? a >= b : x >= y;
                break;
            case TOKEN_AMP_AMP: *bits = a && b;
                break;
            case TOKEN_PIPE_PIPE: *bits = a || b;
                break;
            default: return 0;
        }
    } else return 0;
    if (data_type_fixed_integer(expression->resolved_type))
        *bits = data_type_normalize_integer(*bits, expression->resolved_type);
    return 1;
}

int fold_constant(Analyzer *analyzer, AstExpression *expression, DataType target) {
    if (expression == NULL) return 0;
    if (expression->folded_constant.lexeme != NULL && target == expression->resolved_type) return 1;
    if (expression->kind == AST_EXPR_BINARY && expression->left && expression->right &&
        expression->left->resolved_callable && expression->right->resolved_callable &&
        (expression->operator_type == TOKEN_EQUAL_EQUAL ||
         expression->operator_type == TOKEN_BANG_EQUAL)) {
        int equal = expression->left->resolved_callable == expression->right->resolved_callable &&
                    expression->left->resolved_callable_program ==
                    expression->right->resolved_callable_program;
        if (expression->operator_type == TOKEN_BANG_EQUAL) equal = !equal;
        expression->folded_constant = (AstToken)
        {
            .type = TOKEN_IDENTIFIER,
            .span = expression->span,
            .lexeme = string_interner_intern(analyzer->program->strings,
                                             equal ? "true" : "false")
        };
        expression->resolved_type = TYPE_BIT;
        return expression->folded_constant.lexeme != NULL;
    }
    int fixed = data_type_fixed_integer(target) || data_type_fixed_integer(expression->resolved_type) ||
                (expression->left && data_type_fixed_integer(expression->left->resolved_type)) ||
                (expression->right && data_type_fixed_integer(expression->right->resolved_type)) ||
                (expression->arguments && data_type_fixed_integer(expression->arguments->resolved_type));
    uint64_t bits;
    if (fixed && data_type_integral(target) && fold_integer_bits(analyzer, expression, &bits)) {
        if (data_type_fixed_integer(target)) bits = data_type_normalize_integer(bits, target);
        char text[64];
        int64_t signed_value;
        memcpy(&signed_value, &bits, sizeof(bits));
        if (data_type_unsigned(target)) snprintf(text, sizeof(text), "%llu", (unsigned long long) bits);
        else snprintf(text, sizeof(text), "%lld", (long long) signed_value);
        expression->folded_constant = (AstToken)
        {
            .type = TOKEN_NUMBER,.span = expression->span,
            .lexeme = string_interner_intern(analyzer->program->strings, text)
        };
        expression->resolved_type = target;
        return expression->folded_constant.lexeme != NULL;
    }
    AstToken value = {.span = expression->span};
    if (expression->kind == AST_EXPR_LITERAL) {
        value = analyzer->program->tokens[expression->value_token];
    } else if (expression->kind == AST_EXPR_NAME) {
        if (same_name(analyzer->program, expression->value_token, "true") ||
            same_name(analyzer->program, expression->value_token, "false"))
            value = analyzer->program->tokens[expression->value_token];
        else if (expression_is_constant_symbol(analyzer, expression)) {
            const AstExpression *initializer = constant_initializer(
                &analyzer->model->symbols[expression->resolved_symbol_id]);
            if (initializer != NULL) value = initializer->folded_constant;
        }
        if (value.lexeme == NULL) {
            semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                           "Constant cycle or forward reference to an unevaluated constant");
            return 0;
        }
    } else {
        AstExpression *a = expression->kind == AST_EXPR_CAST
                               ? expression->arguments
                               : expression->kind == AST_EXPR_UNARY
                                     ? expression->right
                                     : expression->left;
        AstExpression *b = expression->kind == AST_EXPR_BINARY ? expression->right : NULL;
        if (a == NULL || !fold_constant(analyzer, a, a->resolved_type)) return 0;
        double x = folded_number(a);
        double result = x;
        if (expression->kind == AST_EXPR_BINARY) {
            int shorted = (expression->operator_type == TOKEN_AMP_AMP && x == 0) ||
                          (expression->operator_type == TOKEN_PIPE_PIPE && x != 0);
            if (!shorted && !fold_constant(analyzer, b, b->resolved_type)) return 0;
            if (a->resolved_type == TYPE_STRING || b->resolved_type == TYPE_STRING) {
                if (expression->operator_type == TOKEN_EQUAL_EQUAL || expression->operator_type == TOKEN_BANG_EQUAL) {
                    int equal = strcmp(a->folded_constant.lexeme, b->folded_constant.lexeme) == 0;
                    value.type = TOKEN_IDENTIFIER;
                    value.lexeme = (expression->operator_type == TOKEN_EQUAL_EQUAL ? equal : !equal) ? "true" : "false";
                    expression->folded_constant = value;
                    return 1;
                }
                if (expression->operator_type != TOKEN_PLUS) goto invalid;
                char first[64], second[64];
                const char *left = folded_string(a, first), *right = folded_string(b, second);
                size_t n = strlen(left), m = strlen(right);
                if (n > SIZE_MAX - m - 1) return 0;
                char *joined = ast_program_alloc(analyzer->program, n + m + 1);
                if (joined == NULL) return 0;
                memcpy(joined, left, n);
                memcpy(joined + n, right, m + 1);
                value.type = TOKEN_STRING_LITERAL;
                value.lexeme = joined;
                expression->folded_constant = value;
                return 1;
            }
            double y = shorted ? 0 : folded_number(b);
            int float_operation = expression->resolved_type == TYPE_FLOAT ||
                                  (expression->resolved_type == TYPE_BIT &&
                                   expression->operator_type != TOKEN_AMP_AMP &&
                                   expression->operator_type != TOKEN_PIPE_PIPE &&
                                   (a->resolved_type == TYPE_FLOAT || b->resolved_type == TYPE_FLOAT) &&
                                   a->resolved_type != TYPE_DOUBLE && b->resolved_type != TYPE_DOUBLE);
            if (float_operation) {
                x = (double) (float) x;
                y = (double) (float) y;
            }
            switch (expression->operator_type) {
                case TOKEN_PLUS: result = x + y;
                    break;
                case TOKEN_MINUS: result = x - y;
                    break;
                case TOKEN_STAR: result = x * y;
                    break;
                case TOKEN_SLASH:
                case TOKEN_PERCENT:
                    if (y == 0 && expression->resolved_type != TYPE_FLOAT && expression->resolved_type != TYPE_DOUBLE) {
                        semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE,
                                       ERR_TYPE_INVALID_OPERATION,
                                       "Division by zero in constant expression");
                        return 0;
                    }
                    result = expression->operator_type == TOKEN_PERCENT
                                 ? (double) ((long long) x % (long long) y)
                                 : x / y;
                    break;
                case TOKEN_EQUAL_EQUAL: result = x == y;
                    break;
                case TOKEN_BANG_EQUAL: result = x != y;
                    break;
                case TOKEN_LESS: result = x < y;
                    break;
                case TOKEN_LESS_EQUAL: result = x <= y;
                    break;
                case TOKEN_GREATER: result = x > y;
                    break;
                case TOKEN_GREATER_EQUAL: result = x >= y;
                    break;
                case TOKEN_AMP_AMP: result = x != 0 && y != 0;
                    break;
                case TOKEN_PIPE_PIPE: result = x != 0 || y != 0;
                    break;
                default: goto invalid;
            }
        } else if (expression->kind == AST_EXPR_UNARY) {
            result = expression->operator_type == TOKEN_MINUS ? -x : (double) (x == 0.0);
        } else if (expression->kind != AST_EXPR_CAST) goto invalid;
        char buffer[64];
        if (target == TYPE_FLOAT || target == TYPE_DOUBLE) {
            snprintf(buffer, sizeof(buffer), "%.17g", target == TYPE_FLOAT ? (double) (float) result : result);
            value.type = TOKEN_FLOAT_LITERAL;
        } else {
            if (target == TYPE_BIT) result = result != 0;
            if (!(result >= INT32_MIN && result <= INT32_MAX)) {
                semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                               "Integer overflow in constant expression");
                return 0;
            }
            long long integer = (long long) result;
            if (target == TYPE_BYTE || target == TYPE_CHAR) integer = (unsigned char) integer;
            snprintf(buffer, sizeof(buffer), "%lld", integer);
            value.type = TOKEN_NUMBER;
        }
        value.lexeme = string_interner_intern(analyzer->program->strings, buffer);
    }
    expression->folded_constant = value;
    /* Apply an explicit binding type to literal and referenced initializers too. */
    if (target != expression->resolved_type && target != TYPE_UNKNOWN) {
        AstExpression cast = {
            .kind = AST_EXPR_CAST, .arguments = expression,
            .resolved_type = target, .span = expression->span
        };
        if (!fold_constant(analyzer, &cast, target)) return 0;
        expression->folded_constant = cast.folded_constant;
    }
    if (target != TYPE_UNKNOWN) expression->resolved_type = target;
    return value.lexeme != NULL;
invalid:
    semantic_error(analyzer, expression->first_token, ERROR_CATEGORY_TYPE, ERR_TYPE_INVALID_OPERATION,
                   "Unsupported constant expression");
    return 0;
}

