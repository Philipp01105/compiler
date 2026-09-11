#include "syntax_parser.h"

#include "errorHandler.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    AstProgram *program;
    size_t current;
    unsigned statement_depth;
    unsigned expression_depth;
    int failed;
    int reported;
} SyntaxParser;

#define AST_MAX_PARSE_DEPTH 512U

static void parser_failure(SyntaxParser *parser, int code, const char *message) {
    if (!parser->reported) {
        const AstToken *token = ast_program_token(parser->program, parser->current);
        error_report(global_error_handler, SEVERITY_ERROR,
                     token == NULL ? 0 : token->span.begin.line,
                     token == NULL ? 0 : token->span.begin.column,
                     ERROR_CATEGORY_PARSER, code, parser->program->source_path,
                     "%s", message);
        parser->reported = 1;
    }
    parser->failed = 1;
}

static const char *token_spelling(TokenType type) {
    switch (type) {
        case TOKEN_ARROW: return "->";
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACE: return "{";
        case TOKEN_RBRACE: return "}";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        case TOKEN_COLON: return ":";
        case TOKEN_SEMICOLON: return ";";
        default: return "token";
    }
}

static TokenType current_type(const SyntaxParser *parser) {
    if (parser->current >= parser->program->token_count) return TOKEN_EOF;
    return parser->program->tokens[parser->current].type;
}

static int check(const SyntaxParser *parser, TokenType type) {
    return current_type(parser) == type;
}

static int match(SyntaxParser *parser, TokenType type) {
    if (!check(parser, type)) return 0;
    parser->current++;
    return 1;
}

static size_t consume(SyntaxParser *parser, TokenType type) {
    if (!check(parser, type)) {
        char message[64];
        (void) snprintf(message, sizeof(message), "Expected '%s'", token_spelling(type));
        parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN, message);
        return AST_TOKEN_NONE;
    }
    return parser->current++;
}

static size_t consume_callable_name(SyntaxParser *parser) {
    TokenType type = current_type(parser);
    if (type != TOKEN_IDENTIFIER) {
        parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN, "Expected function name");
        return AST_TOKEN_NONE;
    }
    return parser->current++;
}

static void *allocate(SyntaxParser *parser, size_t size) {
    void *result = ast_program_alloc(parser->program, size);
    if (result == NULL) parser->failed = 1;
    return result;
}

static AstSourceSpan token_span(const SyntaxParser *parser, size_t token) {
    AstSourceSpan span = {{0, 0}, {0, 0}};
    if (token < parser->program->token_count) span = parser->program->tokens[token].span;
    return span;
}

static AstSourceSpan range_span(const SyntaxParser *parser, size_t first, size_t end) {
    AstSourceSpan span = token_span(parser, first);
    if (end > first) span.end = token_span(parser, end - 1).end;
    return span;
}

static AstType inferred_type(void) {
    AstType type = {0};
    type.kind = AST_TYPE_INFERRED;
    type.name_token = AST_TOKEN_NONE;
    type.array_length_token = AST_TOKEN_NONE;
    return type;
}

static int is_type_token(TokenType type) {
    return type == TOKEN_IDENTIFIER ||
           (type >= TOKEN_TYPE_INT && type <= TOKEN_TYPE_VOID);
}

static AstType parse_type(SyntaxParser *parser) {
    AstType type = inferred_type();
    size_t first = parser->current;
    unsigned leading_pointers = 0;
    while (match(parser, TOKEN_STAR)) leading_pointers++;
    if (match(parser, TOKEN_LPAREN)) {
        type = parse_type(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (type.is_array || type.is_slice) type.outer_pointer_depth += leading_pointers;
        else type.pointer_depth += leading_pointers;
    } else {
        if (!is_type_token(current_type(parser))) {
            parser_failure(parser, ERR_TYPE_UNKNOWN, "Unknown type");
            return type;
        }
        type.kind = AST_TYPE_NAMED;
        type.name_token = parser->current++;
        type.pointer_depth = leading_pointers;
    }
    if (match(parser, TOKEN_LBRACKET)) {
        if (match(parser, TOKEN_RBRACKET)) {
            type.is_slice = 1;
        } else {
            if (!check(parser, TOKEN_NUMBER) && !check(parser, TOKEN_IDENTIFIER))
                parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                               "Expected array length constant");
            type.is_array = 1;
            type.array_length_token = parser->current++;
            (void) consume(parser, TOKEN_RBRACKET);
        }
    }
    type.span = range_span(parser, first, parser->current);
    return type;
}

static AstExpression *parse_expression(SyntaxParser *parser);
static AstStatement *parse_statement(SyntaxParser *parser);
static AstDeclarationNode *parse_function(SyntaxParser *parser, int is_static,
                                          size_t owner_token);

static AstExpression *new_expression(SyntaxParser *parser, AstExpressionKind kind,
                                     size_t first) {
    AstExpression *expression = allocate(parser, sizeof(*expression));
    if (expression == NULL) return NULL;
    expression->kind = kind;
    expression->first_token = first;
    expression->value_token = AST_TOKEN_NONE;
    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    expression->allocated_type = inferred_type();
    return expression;
}

static void finish_expression(SyntaxParser *parser, AstExpression *expression) {
    if (expression == NULL) return;
    expression->token_count = parser->current - expression->first_token;
    expression->span = range_span(parser, expression->first_token, parser->current);
}

static AstExpression *parse_primary(SyntaxParser *parser) {
    size_t first = parser->current;
    TokenType type = current_type(parser);
    AstExpression *expression = NULL;
    if (type == TOKEN_NUMBER || type == TOKEN_FLOAT_LITERAL ||
        type == TOKEN_CHAR_LITERAL || type == TOKEN_STRING_LITERAL) {
        expression = new_expression(parser, AST_EXPR_LITERAL, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
    } else if (type == TOKEN_IDENTIFIER || type == TOKEN_KEYWORD_FREE || is_type_token(type)) {
        expression = new_expression(parser, AST_EXPR_NAME, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
    } else if (type == TOKEN_KEYWORD_RESERVE) {
        expression = new_expression(parser, AST_EXPR_RESERVE, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
        (void) consume(parser, TOKEN_LPAREN);
        AstType allocated_type = parse_type(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (expression != NULL) expression->allocated_type = allocated_type;
    } else if (match(parser, TOKEN_LPAREN)) {
        expression = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (expression != NULL) {
            expression->first_token = first;
            finish_expression(parser, expression);
        }
        return expression;
    } else {
        parser->failed = 1;
        expression = new_expression(parser, AST_EXPR_ERROR, first);
        if (!check(parser, TOKEN_EOF)) parser->current++;
    }

    /* A postfix expression keeps its operand as a child.  Finalize the
       primary before wrapping it so that the retained name/literal node has
       its own non-empty source range. */
    finish_expression(parser, expression);
    while (!parser->failed && expression != NULL) {
        if (match(parser, TOKEN_LPAREN)) {
            AstExpressionKind call_kind = AST_EXPR_CALL;
            if (expression->kind == AST_EXPR_NAME &&
                     expression->value_token < parser->program->token_count) {
                TokenType callee = parser->program->tokens[expression->value_token].type;
                if (callee >= TOKEN_TYPE_INT && callee <= TOKEN_TYPE_VOID)
                    call_kind = AST_EXPR_CAST;
                else if (callee == TOKEN_KEYWORD_FREE)
                    call_kind = AST_EXPR_FREE;
            }
            AstExpression *call = new_expression(parser, call_kind, first);
            AstExpression **tail = call == NULL ? NULL : &call->arguments;
            if (call != NULL) {
                call->left = call_kind == AST_EXPR_CALL ? expression : NULL;
                call->value_token = expression->value_token;
            }
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    AstExpression *argument = parse_expression(parser);
                    if (tail != NULL) {
                        *tail = argument;
                        if (argument != NULL) tail = &argument->next;
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            (void) consume(parser, TOKEN_RPAREN);
            expression = call;
        } else if (match(parser, TOKEN_LBRACKET)) {
            AstExpression *index = new_expression(parser, AST_EXPR_INDEX, first);
            if (index != NULL) {
                index->left = expression;
                index->right = parse_expression(parser);
            }
            (void) consume(parser, TOKEN_RBRACKET);
            expression = index;
        } else if (match(parser, TOKEN_DOT)) {
            AstExpression *member = new_expression(parser, AST_EXPR_MEMBER, first);
            size_t name = consume_callable_name(parser);
            if (member != NULL) {
                member->left = expression;
                member->value_token = name;
            }
            expression = member;
        } else {
            break;
        }
        finish_expression(parser, expression);
    }
    finish_expression(parser, expression);
    return expression;
}

static AstExpression *parse_unary(SyntaxParser *parser) {
    TokenType type = current_type(parser);
    if (type == TOKEN_BANG || type == TOKEN_MINUS || type == TOKEN_AMPERSAND ||
        type == TOKEN_STAR) {
        if (parser->expression_depth >= AST_MAX_PARSE_DEPTH) {
            parser_failure(parser, ERR_PARSE_TOO_MANY_ERRORS,
                           "Expression tree exceeds maximum depth");
            return NULL;
        }
        size_t first = parser->current++;
        AstExpression *expression = new_expression(parser, AST_EXPR_UNARY, first);
        parser->expression_depth++;
        if (expression != NULL) {
            expression->operator_type = type;
            expression->right = parse_unary(parser);
        }
        parser->expression_depth--;
        finish_expression(parser, expression);
        return expression;
    }
    return parse_primary(parser);
}

static int precedence(TokenType type) {
    if (type == TOKEN_PIPE_PIPE) return 1;
    if (type == TOKEN_AMP_AMP) return 2;
    if (type == TOKEN_EQUAL_EQUAL || type == TOKEN_BANG_EQUAL ||
        type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
        type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL) return 3;
    if (type == TOKEN_PLUS || type == TOKEN_MINUS) return 4;
    if (type == TOKEN_STAR || type == TOKEN_SLASH || type == TOKEN_PERCENT) return 5;
    return 0;
}

static AstExpression *parse_binary(SyntaxParser *parser, int minimum) {
    AstExpression *left = parse_unary(parser);
    while (!parser->failed) {
        TokenType operation = current_type(parser);
        int operation_precedence = precedence(operation);
        if (operation_precedence < minimum) break;
        size_t first = left == NULL ? parser->current : left->first_token;
        parser->current++;
        AstExpression *right = parse_binary(parser, operation_precedence + 1);
        AstExpression *binary = new_expression(parser, AST_EXPR_BINARY, first);
        if (binary != NULL) {
            binary->operator_type = operation;
            binary->left = left;
            binary->right = right;
        }
        finish_expression(parser, binary);
        left = binary;
    }
    return left;
}

static AstExpression *parse_expression(SyntaxParser *parser) {
    if (parser->expression_depth >= AST_MAX_PARSE_DEPTH) {
        parser_failure(parser, ERR_PARSE_TOO_MANY_ERRORS,
                       "Expression tree exceeds maximum depth");
        return NULL;
    }
    parser->expression_depth++;
    AstExpression *expression = parse_binary(parser, 1);
    parser->expression_depth--;
    return expression;
}

static AstStatement *new_statement(SyntaxParser *parser, AstStatementKind kind,
                                   size_t first) {
    AstStatement *statement = allocate(parser, sizeof(*statement));
    if (statement == NULL) return NULL;
    statement->kind = kind;
    statement->first_token = first;
    statement->name_token = AST_TOKEN_NONE;
    statement->resolved_symbol_id = AST_SYMBOL_NONE;
    statement->type = inferred_type();
    return statement;
}

static void finish_statement(SyntaxParser *parser, AstStatement *statement) {
    if (statement == NULL) return;
    statement->token_count = parser->current - statement->first_token;
    statement->span = range_span(parser, statement->first_token, parser->current);
}

static AstStatement *parse_block(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_LBRACE);
    AstStatement *block = new_statement(parser, AST_STMT_BLOCK, first);
    AstStatement **tail = block == NULL ? NULL : &block->body;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        AstStatement *child = parse_statement(parser);
        if (tail != NULL) {
            *tail = child;
            if (child != NULL) tail = &child->next;
        }
    }
    (void) consume(parser, TOKEN_RBRACE);
    finish_statement(parser, block);
    return block;
}

static AstStatement *parse_variable(SyntaxParser *parser, size_t first,
                                    int consume_semicolon) {
    int is_const = check(parser, TOKEN_KEYWORD_CONST);
    parser->current++;
    AstStatement *statement = new_statement(parser, AST_STMT_VARIABLE, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstType type = inferred_type();
    if (match(parser, TOKEN_COLON)) type = parse_type(parser);
    AstExpression *value = NULL;
    if (match(parser, TOKEN_EQUAL)) value = parse_expression(parser);
    if (consume_semicolon) (void) consume(parser, TOKEN_SEMICOLON);
    if (statement != NULL) {
        statement->name_token = name;
        statement->type = type;
        statement->value = value;
        statement->is_const = is_const;
    }
    finish_statement(parser, statement);
    return statement;
}

static int is_assignment(TokenType type) {
    return type == TOKEN_EQUAL || type == TOKEN_PLUS_EQUAL || type == TOKEN_MINUS_EQUAL ||
           type == TOKEN_STAR_EQUAL || type == TOKEN_SLASH_EQUAL ||
           type == TOKEN_PLUS_PLUS || type == TOKEN_MINUS_MINUS;
}

static AstStatement *parse_expression_statement(SyntaxParser *parser, int consume_semicolon) {
    size_t first = parser->current;
    AstExpression *left = parse_expression(parser);
    TokenType operation = current_type(parser);
    AstStatementKind kind = AST_STMT_EXPRESSION;
    AstExpression *value = NULL;
    if (is_assignment(operation)) {
        kind = AST_STMT_ASSIGNMENT;
        parser->current++;
        if (operation != TOKEN_PLUS_PLUS && operation != TOKEN_MINUS_MINUS)
            value = parse_expression(parser);
    }
    if (consume_semicolon) (void) consume(parser, TOKEN_SEMICOLON);
    AstStatement *statement = new_statement(parser, kind, first);
    if (statement != NULL) {
        statement->expression = left;
        statement->value = value;
        statement->assignment_operator = operation;
    }
    finish_statement(parser, statement);
    return statement;
}

static AstStatement *parse_statement_impl(SyntaxParser *parser) {
    size_t first = parser->current;
    if (check(parser, TOKEN_LBRACE)) return parse_block(parser);
    if (check(parser, TOKEN_KEYWORD_VAR) || check(parser, TOKEN_KEYWORD_CONST))
        return parse_variable(parser, first, 1);

    if (match(parser, TOKEN_KEYWORD_IF)) {
        AstStatement *statement = new_statement(parser, AST_STMT_IF, first);
        (void) consume(parser, TOKEN_LPAREN);
        AstExpression *condition = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        AstStatement *body = parse_statement(parser);
        AstStatement *else_body = NULL;
        if (match(parser, TOKEN_KEYWORD_ELSE)) else_body = parse_statement(parser);
        if (statement != NULL) {
            statement->condition = condition;
            statement->body = body;
            statement->else_body = else_body;
        }
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_WHILE)) {
        AstStatement *statement = new_statement(parser, AST_STMT_WHILE, first);
        (void) consume(parser, TOKEN_LPAREN);
        AstExpression *condition = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        AstStatement *body = parse_statement(parser);
        if (statement != NULL) { statement->condition = condition; statement->body = body; }
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_FOR)) {
        AstStatement *statement = new_statement(parser, AST_STMT_FOR, first);
        (void) consume(parser, TOKEN_LPAREN);
        AstStatement *initializer = NULL;
        if (!check(parser, TOKEN_SEMICOLON)) {
            initializer = (check(parser, TOKEN_KEYWORD_VAR) ||
                           check(parser, TOKEN_KEYWORD_CONST))
                ? parse_variable(parser, parser->current, 0)
                : parse_expression_statement(parser, 0);
        }
        (void) consume(parser, TOKEN_SEMICOLON);
        AstExpression *condition = NULL;
        if (!check(parser, TOKEN_SEMICOLON)) condition = parse_expression(parser);
        (void) consume(parser, TOKEN_SEMICOLON);
        AstStatement *update = NULL;
        if (!check(parser, TOKEN_RPAREN)) update = parse_expression_statement(parser, 0);
        (void) consume(parser, TOKEN_RPAREN);
        AstStatement *body = parse_statement(parser);
        if (statement != NULL) {
            statement->initializer = initializer;
            statement->condition = condition;
            statement->else_body = update;
            statement->body = body;
        }
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_RETURN)) {
        AstStatement *statement = new_statement(parser, AST_STMT_RETURN, first);
        AstExpression *value = NULL;
        if (!check(parser, TOKEN_SEMICOLON)) value = parse_expression(parser);
        (void) consume(parser, TOKEN_SEMICOLON);
        if (statement != NULL) statement->value = value;
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_BREAK) || match(parser, TOKEN_KEYWORD_CONTINUE)) {
        TokenType keyword = parser->program->tokens[first].type;
        AstStatement *statement = new_statement(parser,
            keyword == TOKEN_KEYWORD_BREAK ? AST_STMT_BREAK : AST_STMT_CONTINUE, first);
        (void) consume(parser, TOKEN_SEMICOLON);
        finish_statement(parser, statement);
        return statement;
    }
    return parse_expression_statement(parser, 1);
}

static AstStatement *parse_statement(SyntaxParser *parser) {
    if (parser->statement_depth >= AST_MAX_PARSE_DEPTH) {
        parser_failure(parser, ERR_PARSE_TOO_MANY_ERRORS,
                       "Statement nesting exceeds maximum depth");
        return NULL;
    }
    parser->statement_depth++;
    AstStatement *statement = parse_statement_impl(parser);
    parser->statement_depth--;
    return statement;
}

static AstParameter *parse_parameter(SyntaxParser *parser) {
    size_t first = parser->current;
    AstParameter *parameter = allocate(parser, sizeof(*parameter));
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    (void) consume(parser, TOKEN_COLON);
    AstType type = parse_type(parser);
    if (parameter != NULL) {
        parameter->name_token = name;
        parameter->type = type;
        parameter->resolved_symbol_id = AST_SYMBOL_NONE;
        parameter->span = range_span(parser, first, parser->current);
    }
    return parameter;
}

static AstDeclarationNode *new_declaration(SyntaxParser *parser,
                                           AstDeclarationKind kind, size_t first) {
    AstDeclarationNode *declaration = allocate(parser, sizeof(*declaration));
    if (declaration != NULL) {
        declaration->kind = kind;
        declaration->first_token = first;
        declaration->name_token = AST_TOKEN_NONE;
        declaration->resolved_symbol_id = AST_SYMBOL_NONE;
    }
    return declaration;
}

static void finish_declaration(SyntaxParser *parser, AstDeclarationNode *declaration) {
    if (declaration == NULL) return;
    declaration->token_count = parser->current - declaration->first_token;
    declaration->span = range_span(parser, declaration->first_token, parser->current);
}

static AstDeclarationNode *parse_function(SyntaxParser *parser, int is_static,
                                          size_t owner_token) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_FUNC);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_FUNCTION, first);
    size_t name = consume_callable_name(parser);
    (void) consume(parser, TOKEN_LPAREN);
    AstParameter *parameters = NULL;
    AstParameter **tail = &parameters;
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            AstParameter *parameter = parse_parameter(parser);
            *tail = parameter;
            if (parameter != NULL) tail = &parameter->next;
        } while (match(parser, TOKEN_COMMA));
    }
    (void) consume(parser, TOKEN_RPAREN);
    (void) consume(parser, TOKEN_ARROW);
    AstType return_type = parse_type(parser);
    AstStatement *body = parse_block(parser);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.function.parameters = parameters;
        declaration->as.function.return_type = return_type;
        declaration->as.function.body = body;
        declaration->as.function.is_static = is_static;
        declaration->as.function.owner_token = owner_token;
    }
    finish_declaration(parser, declaration);
    return declaration;
}

static AstField *parse_field(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_VAR);
    AstField *field = allocate(parser, sizeof(*field));
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    (void) consume(parser, TOKEN_COLON);
    AstType type = parse_type(parser);
    (void) consume(parser, TOKEN_SEMICOLON);
    if (field != NULL) {
        field->name_token = name;
        field->type = type;
        field->resolved_symbol_id = AST_SYMBOL_NONE;
        field->span = range_span(parser, first, parser->current);
    }
    return field;
}

static AstDeclarationNode *parse_struct(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_STRUCT);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_STRUCT, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    (void) consume(parser, TOKEN_LBRACE);
    AstField *fields = NULL;
    AstField **field_tail = &fields;
    AstDeclarationNode *methods = NULL;
    AstDeclarationNode **method_tail = &methods;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        int is_static = match(parser, TOKEN_KEYWORD_STATIC);
        if (check(parser, TOKEN_KEYWORD_FUNC)) {
            AstDeclarationNode *method = parse_function(parser, is_static, name);
            *method_tail = method;
            if (method != NULL) method_tail = &method->next;
        } else if (!is_static && check(parser, TOKEN_KEYWORD_VAR)) {
            AstField *field = parse_field(parser);
            *field_tail = field;
            if (field != NULL) field_tail = &field->next;
        } else {
            parser->failed = 1;
        }
    }
    (void) consume(parser, TOKEN_RBRACE);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.struct_decl.fields = fields;
        declaration->as.struct_decl.methods = methods;
    }
    finish_declaration(parser, declaration);
    return declaration;
}

static AstDeclarationNode *parse_enum(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_ENUM);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_ENUM, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstField *fields = NULL;
    AstField **field_tail = &fields;
    if (match(parser, TOKEN_LPAREN)) {
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                size_t field_first = parser->current;
                AstField *field = allocate(parser, sizeof(*field));
                size_t field_name = consume(parser, TOKEN_IDENTIFIER);
                (void) consume(parser, TOKEN_COLON);
                AstType field_type = parse_type(parser);
                if (field != NULL) {
                    field->name_token = field_name;
                    field->type = field_type;
                    field->resolved_symbol_id = AST_SYMBOL_NONE;
                    field->span = range_span(parser, field_first, parser->current);
                    *field_tail = field;
                    field_tail = &field->next;
                }
            } while (match(parser, TOKEN_COMMA));
        }
        (void) consume(parser, TOKEN_RPAREN);
    }
    (void) consume(parser, TOKEN_LBRACE);
    AstEnumValue *values = NULL;
    AstEnumValue **value_tail = &values;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        size_t value_first = parser->current;
        AstEnumValue *value = allocate(parser, sizeof(*value));
        size_t value_name = consume(parser, TOKEN_IDENTIFIER);
        AstExpression *arguments = NULL;
        AstExpression **argument_tail = &arguments;
        if (match(parser, TOKEN_LPAREN)) {
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    AstExpression *argument = parse_expression(parser);
                    *argument_tail = argument;
                    if (argument != NULL) argument_tail = &argument->next;
                } while (match(parser, TOKEN_COMMA));
            }
            (void) consume(parser, TOKEN_RPAREN);
        }
        if (value != NULL) {
            value->name_token = value_name;
            value->arguments = arguments;
            value->resolved_symbol_id = AST_SYMBOL_NONE;
            value->span = range_span(parser, value_first, parser->current);
            *value_tail = value;
            value_tail = &value->next;
        }
        if (!match(parser, TOKEN_COMMA)) break;
    }
    (void) consume(parser, TOKEN_RBRACE);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.enum_decl.fields = fields;
        declaration->as.enum_decl.values = values;
    }
    finish_declaration(parser, declaration);
    return declaration;
}

static AstDeclarationNode *parse_import(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_IMPORT);
    int grouped = match(parser, TOKEN_LPAREN);
    AstDeclarationNode *head = NULL;
    AstDeclarationNode **tail = &head;
    do {
        if (grouped && check(parser, TOKEN_RPAREN)) break;
        size_t path_start = parser->current;
        AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_IMPORT,
                                                          grouped ? path_start : first);
        size_t path_first = path_start;
        size_t path_token = AST_TOKEN_NONE;
        if (check(parser, TOKEN_STRING_LITERAL)) {
            path_token = parser->current++;
        } else if (match(parser, TOKEN_LESS)) {
            path_first = parser->current;
            while (!check(parser, TOKEN_GREATER) && !check(parser, TOKEN_EOF)) parser->current++;
            (void) consume(parser, TOKEN_GREATER);
        } else {
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN, "Expected import path");
        }
        if (declaration != NULL) {
            declaration->as.import_decl.path_token = path_token;
            declaration->as.import_decl.path_first_token = path_first;
            declaration->as.import_decl.path_token_count = parser->current - path_first;
            finish_declaration(parser, declaration);
            *tail = declaration;
            tail = &declaration->next;
        }
    } while (grouped && !parser->failed);
    if (grouped) (void) consume(parser, TOKEN_RPAREN);
    if (head == NULL && !parser->failed)
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                       "Import group must contain at least one path");
    return head;
}

static AstDeclarationNode *parse_constant(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_CONST);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_CONSTANT, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstType type = inferred_type();
    if (match(parser, TOKEN_COLON)) type = parse_type(parser);
    (void) consume(parser, TOKEN_EQUAL);
    AstExpression *value = parse_expression(parser);
    (void) consume(parser, TOKEN_SEMICOLON);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.constant.type = type;
        declaration->as.constant.value = value;
    }
    finish_declaration(parser, declaration);
    return declaration;
}

int frontend_build_structured_ast(AstProgram *program) {
    if (program == NULL) return 0;
    program->structured_error_token = AST_TOKEN_NONE;
    SyntaxParser parser = {.program = program};
    AstDeclarationNode **tail = &program->root;
    while (!parser.failed && !check(&parser, TOKEN_EOF)) {
        AstDeclarationNode *declaration = NULL;
        if (check(&parser, TOKEN_KEYWORD_IMPORT)) declaration = parse_import(&parser);
        else if (check(&parser, TOKEN_KEYWORD_CONST)) declaration = parse_constant(&parser);
        else if (check(&parser, TOKEN_KEYWORD_FUNC))
            declaration = parse_function(&parser, 0, AST_TOKEN_NONE);
        else if (check(&parser, TOKEN_KEYWORD_STRUCT)) declaration = parse_struct(&parser);
        else if (check(&parser, TOKEN_KEYWORD_ENUM)) declaration = parse_enum(&parser);
        else parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                            "Expected function declaration");
        while (declaration != NULL) {
            AstDeclarationNode *next = declaration->next;
            *tail = declaration;
            tail = &declaration->next;
            program->structured_declaration_count++;
            declaration = next;
        }
    }
    program->structured_ast_complete = !parser.failed && check(&parser, TOKEN_EOF);
    if (!program->structured_ast_complete) program->structured_error_token = parser.current;
    return program->structured_ast_complete;
}
