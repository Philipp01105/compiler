#include "syntax_parser.h"

#include "errorHandler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AstProgram *program;
    size_t current;
    unsigned statement_depth;
    unsigned expression_depth;
    unsigned type_depth;
    int trait_signature;
    int pending_equal;
    int failed;
    int reported;
    int recover_syntax;
    int allocation_failed;
} SyntaxParser;

#define AST_MAX_PARSE_DEPTH 512U

static void parser_failure(SyntaxParser *parser, int code, const char *message) {
    if (!parser->reported) {
        const AstToken *token = ast_program_token(parser->program, parser->current);
        char detail[1024];
        if (token == NULL || token->type == TOKEN_EOF)
            snprintf(detail, sizeof(detail), "%s; reached end of file", message);
        else snprintf(detail, sizeof(detail), "%s; found '%s'", message, token->lexeme);
        ErrorContext *context = error_context_create(SEVERITY_ERROR,
                     token == NULL ? 0 : token->span.begin.line,
                     token == NULL ? 0 : token->span.begin.column,
                     ERROR_CATEGORY_PARSER, code, parser->program->source_path,
                     detail);
        if (token != NULL) {
            error_context_set_span(context, token->span.end.line, token->span.end.column);
            error_context_set_token(context, token->lexeme);
        }
        if (code == ERR_PARSE_MISSING_BRACE || code == ERR_PARSE_MISSING_PAREN) {
            TokenType open = code == ERR_PARSE_MISSING_BRACE ? TOKEN_LBRACE : TOKEN_LPAREN;
            TokenType close = code == ERR_PARSE_MISSING_BRACE ? TOKEN_RBRACE : TOKEN_RPAREN;
            unsigned depth = 0;
            for (size_t i = parser->current; i > 0; i--) {
                const AstToken *candidate = ast_program_token(parser->program, i - 1);
                if (candidate->type == close) depth++;
                if (candidate->type == open) {
                    if (depth > 0) depth--;
                    else {
                        ErrorContext *note = error_context_create(SEVERITY_INFO,
                            candidate->span.begin.line, candidate->span.begin.column,
                            ERROR_CATEGORY_PARSER, code, parser->program->source_path,
                            "Opening delimiter is here");
                        error_context_set_span(note, candidate->span.end.line, candidate->span.end.column);
                        error_context_add_child(context, note);
                        break;
                    }
                }
            }
        }
        error_report_context(global_error_handler, context);
        if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
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
        case TOKEN_IDENTIFIER: return "identifier (name)";
        case TOKEN_EQUAL: return "=";
        case TOKEN_GREATER: return ">";
        case TOKEN_KEYWORD_FUNC: return "func";
        case TOKEN_KEYWORD_VAR: return "var";
        case TOKEN_KEYWORD_STRUCT: return "struct";
        case TOKEN_KEYWORD_ENUM: return "enum";
        case TOKEN_KEYWORD_IMPORT: return "import";
        case TOKEN_KEYWORD_CONST: return "const";
        default: return "token";
    }
}

static TokenType current_type(const SyntaxParser *parser) {
    if (parser->pending_equal) return TOKEN_EQUAL;
    if (parser->current >= parser->program->token_count) return TOKEN_EOF;
    return parser->program->tokens[parser->current].type;
}

static int check(const SyntaxParser *parser, TokenType type) {
    return current_type(parser) == type;
}

static int match(SyntaxParser *parser, TokenType type) {
    if (!check(parser, type)) return 0;
    parser->pending_equal=0;
    parser->current++;
    return 1;
}

static size_t consume(SyntaxParser *parser, TokenType type) {
    if (type == TOKEN_GREATER && !parser->pending_equal && check(parser,TOKEN_GREATER_EQUAL)) {
        parser->pending_equal=1;
        return parser->current;
    }
    if (!check(parser, type)) {
        if (parser->recover_syntax && !parser->failed && type == TOKEN_RBRACE && check(parser, TOKEN_EOF)) {
            parser_failure(parser, ERR_PARSE_MISSING_BRACE, "Expected '}' to close block before end of file");
            parser->failed = 0;
            return AST_TOKEN_NONE;
        }
        if (type == TOKEN_SEMICOLON && !parser->failed && parser->current > 0) {
            const AstToken *previous = ast_program_token(parser->program, parser->current - 1);
            if (previous != NULL) {
                ErrorContext *context = error_context_create(SEVERITY_ERROR,
                    previous->span.end.line, previous->span.end.column,
                    ERROR_CATEGORY_PARSER, ERR_PARSE_MISSING_SEMICOLON, parser->program->source_path,
                    "Expected ';' after statement");
                error_context_set_span(context, previous->span.end.line, previous->span.end.column);
                error_context_set_suggestion(context, "Insert ';' at the end of the preceding statement");
                error_context_set_fix(context, previous->span.end.line, previous->span.end.column,
                    previous->span.end.line, previous->span.end.column, ";");
                error_report_context(global_error_handler, context);
                if (global_error_handler == NULL || !global_error_handler->buffered) error_context_free(context);
                parser->reported = 1;
                parser->failed = !parser->recover_syntax;
                return AST_TOKEN_NONE;
            }
        }
        char message[64];
        (void) snprintf(message, sizeof(message), "Expected '%s'", token_spelling(type));
        parser_failure(parser, type == TOKEN_RBRACE ? ERR_PARSE_MISSING_BRACE :
                       type == TOKEN_RPAREN ? ERR_PARSE_MISSING_PAREN : ERR_PARSE_EXPECTED_TOKEN, message);
        return AST_TOKEN_NONE;
    }
    parser->pending_equal=0;
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
    if (result == NULL) {
        if (!parser->allocation_failed)
            error_report(global_error_handler, SEVERITY_FATAL, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INTERNAL_FAILURE, parser->program->source_path,
                         "Out of memory while building structured syntax tree");
        parser->failed = 1;
        parser->allocation_failed = 1;
        parser->reported = 1;
    }
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
    if (++parser->type_depth > AST_MAX_PARSE_DEPTH) {
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Type nesting exceeds parser limit");
        parser->type_depth--; return type;
    }
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
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN, "Unknown type: expected a type name");
            parser->type_depth--; return type;
        }
        type.kind = AST_TYPE_NAMED;
        type.name_token = parser->current++;
        type.pointer_depth = leading_pointers;
        if (match(parser, TOKEN_LESS)) {
            AstTypeArgument **tail=&type.arguments;
            size_t count=0;
            do {
                if (++count > 16) {
                    parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Generic types allow at most 16 arguments"); break;
                }
                AstTypeArgument *argument=allocate(parser,sizeof(*argument));
                AstType value=parse_type(parser);
                if (argument != NULL) { argument->type=value; *tail=argument; tail=&argument->next; }
            } while (match(parser,TOKEN_COMMA));
            (void)consume(parser,TOKEN_GREATER);
        }
    }
    if (match(parser, TOKEN_LBRACKET)) {
        if (type.is_array || type.is_slice) {
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                           "Nested array and slice types are not supported");
            parser->type_depth--; return type;
        }
        if (match(parser, TOKEN_RBRACKET)) {
            type.is_slice = 1;
        } else {
            if (!check(parser, TOKEN_NUMBER) && !check(parser, TOKEN_IDENTIFIER))
                parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                               "Expected array length constant");
            type.is_array = 1;
            type.array_length_token = parser->current++;
            if (type.array_length_token < parser->program->token_count &&
                parser->program->tokens[type.array_length_token].type == TOKEN_NUMBER)
                type.resolved_array_length=(size_t)strtoull(
                    ast_program_lexeme(parser->program,type.array_length_token),NULL,10);
            (void) consume(parser, TOKEN_RBRACKET);
        }
    }
    type.span = range_span(parser, first, parser->current);
    if (parser->pending_equal) {
        type.span.end=token_span(parser,parser->current).begin;
        type.span.end.column++;
    }
    parser->type_depth--; return type;
}

static AstExpression *parse_expression(SyntaxParser *parser);
static AstStatement *parse_statement(SyntaxParser *parser);
static AstGenericParameter *parse_generic_parameters(SyntaxParser *parser) {
    AstGenericParameter *head = NULL, **tail = &head;
    if (!match(parser, TOKEN_LESS)) return NULL;
    size_t count = 0;
    do {
        if (++count > 16) {
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Generic declarations allow at most 16 type parameters");
            break;
        }
        AstGenericParameter *parameter = allocate(parser, sizeof(*parameter));
        size_t name = consume(parser, TOKEN_IDENTIFIER);
        if (parameter != NULL) {
            parameter->name_token = name;
            if (match(parser,TOKEN_COLON)) {
                AstTraitBound **bound=&parameter->bounds;
                do {
                    AstTraitBound *b=allocate(parser,sizeof(*b));
                    size_t token=consume(parser,TOKEN_IDENTIFIER);
                    if (b != NULL) { b->name_token=token; *bound=b; bound=&b->next; }
                } while (match(parser,TOKEN_PLUS));
            }
            *tail = parameter; tail = &parameter->next;
        }
    } while (match(parser, TOKEN_COMMA));
    (void)consume(parser, TOKEN_GREATER);
    return head;
}

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
    } else {
        parser_failure(parser, ERR_PARSE_UNEXPECTED_TOKEN, "Expected expression");
        expression = new_expression(parser, AST_EXPR_ERROR, first);
        if (!check(parser, TOKEN_EOF) && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_SEMICOLON))
            parser->current++;
    }

    if (expression != NULL && expression->kind == AST_EXPR_NAME && check(parser,TOKEN_LESS)) {
        size_t look=parser->current; unsigned depth=0;
        do {
            TokenType t=parser->program->tokens[look++].type;
            if (t == TOKEN_LESS) depth++;
            if (t == TOKEN_GREATER) depth--;
            if (t == TOKEN_EOF || t == TOKEN_SEMICOLON) break;
        } while (look < parser->program->token_count && depth != 0);
        if (depth == 0 && look < parser->program->token_count &&
            parser->program->tokens[look].type == TOKEN_DOT) {
            size_t saved=parser->current; parser->current=expression->value_token;
            expression->allocated_type=parse_type(parser);
            if (parser->current <= saved) parser->failed=1;
        }
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
                if (callee >= TOKEN_TYPE_INT && callee <= TOKEN_TYPE_VOID) {
                    parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                                   "Type-first casts were removed; use expression.(type)");
                    break;
                }
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
            if (match(parser, TOKEN_LPAREN)) {
                AstExpression *cast = new_expression(parser, AST_EXPR_CAST, first);
                AstType target = parse_type(parser);
                (void) consume(parser, TOKEN_RPAREN);
                if (cast != NULL) {
                    cast->arguments = expression;
                    cast->allocated_type = target;
                    cast->value_token = target.name_token;
                }
                expression = cast;
                finish_expression(parser, expression);
                continue;
            }
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
        if (operation == TOKEN_STAR &&
            (check(parser, TOKEN_EQUAL) || check(parser, TOKEN_PLUS_EQUAL) ||
             check(parser, TOKEN_MINUS_EQUAL) || check(parser, TOKEN_STAR_EQUAL) ||
             check(parser, TOKEN_SLASH_EQUAL) || check(parser, TOKEN_SEMICOLON))) {
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                           "Postfix dereference is not supported; use *pointer");
            break;
        }
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
        size_t child_first = parser->current;
        if (parser->recover_syntax) parser->reported = 0;
        AstStatement *child = parse_statement(parser);
        if (parser->failed && parser->recover_syntax && !parser->allocation_failed) {
            // Throw away the malformed subtree; resume at a statement/block boundary.
            int braces = 0;
            for (size_t i = child_first; i < parser->current && i < parser->program->token_count; i++) {
                if (parser->program->tokens[i].type == TOKEN_LBRACE) braces++;
                if (parser->program->tokens[i].type == TOKEN_RBRACE) braces--;
            }
            while (!check(parser, TOKEN_EOF) && !(braces <= 0 && parser->current > child_first &&
                   parser->program->tokens[parser->current - 1].type == TOKEN_SEMICOLON)) {
                TokenType token = current_type(parser);
                if (braces <= 0 && token == TOKEN_RBRACE) break;
                if (braces <= 0 && token == TOKEN_SEMICOLON) { parser->current++; break; }
                if (braces <= 0 && parser->current > child_first &&
                    (token == TOKEN_KEYWORD_VAR || token == TOKEN_KEYWORD_CONST ||
                     token == TOKEN_KEYWORD_RETURN || token == TOKEN_KEYWORD_IF ||
                     token == TOKEN_KEYWORD_FOR || token == TOKEN_KEYWORD_WHILE ||
                     token == TOKEN_KEYWORD_BREAK || token == TOKEN_KEYWORD_CONTINUE)) break;
                if (token == TOKEN_LBRACE) braces++;
                if (token == TOKEN_RBRACE) braces--;
                parser->current++;
            }
            parser->failed = 0;
            child = NULL;
        }
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
    if (check(parser, TOKEN_LBRACKET))
        parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                       "Legacy array syntax was removed; use name:type[size]");
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
    if (check(parser, TOKEN_AT)) {
        parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                       "@gc was removed; use explicit free");
        return NULL;
    }
    if (check(parser, TOKEN_LBRACE)) return parse_block(parser);
    if (check(parser, TOKEN_KEYWORD_VAR) || check(parser, TOKEN_KEYWORD_CONST))
        return parse_variable(parser, first, 1);

    if (match(parser,TOKEN_KEYWORD_MATCH)) {
        AstStatement *statement=new_statement(parser,AST_STMT_MATCH,first);
        (void)consume(parser,TOKEN_LPAREN);
        AstExpression *value=parse_expression(parser);
        (void)consume(parser,TOKEN_RPAREN); (void)consume(parser,TOKEN_LBRACE);
        AstMatchArm *head=NULL, **tail=&head;
        while (!parser->failed && !check(parser,TOKEN_RBRACE) && !check(parser,TOKEN_EOF)) {
            size_t arm_first=parser->current;
            AstMatchArm *arm=allocate(parser,sizeof(*arm));
            size_t variant=consume(parser,TOKEN_IDENTIFIER);
            AstParameter *bindings=NULL, **binding_tail=&bindings;
            if (match(parser,TOKEN_LPAREN)) {
                if (!check(parser,TOKEN_RPAREN)) do {
                    AstParameter *binding=allocate(parser,sizeof(*binding));
                    size_t token=consume(parser,TOKEN_IDENTIFIER);
                    if (binding) { binding->name_token=token; binding->type=inferred_type();
                        binding->resolved_symbol_id=AST_SYMBOL_NONE; *binding_tail=binding; binding_tail=&binding->next; }
                } while (match(parser,TOKEN_COMMA));
                (void)consume(parser,TOKEN_RPAREN);
            }
            (void)consume(parser,TOKEN_FAT_ARROW);
            AstStatement *body=parse_statement(parser);
            if (arm) { arm->variant_token=variant; arm->wildcard=!strcmp(ast_program_lexeme(parser->program,variant),"_");
                arm->bindings=bindings; arm->body=body; arm->span=range_span(parser,arm_first,parser->current);
                arm->resolved_variant_symbol=AST_SYMBOL_NONE; *tail=arm; tail=&arm->next; }
        }
        (void)consume(parser,TOKEN_RBRACE);
        if (statement) { statement->value=value; statement->match_arms=head; }
        finish_statement(parser,statement); return statement;
    }
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
        if (!check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF))
            value = parse_expression(parser);
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
    AstGenericParameter *generics = parse_generic_parameters(parser);
    if (declaration != NULL) declaration->generic_parameters = generics;
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
    AstStatement *body;
    if (parser->trait_signature) {
        body=new_statement(parser,AST_STMT_BLOCK,parser->current);
        (void)consume(parser,TOKEN_SEMICOLON);
        finish_statement(parser,body);
    } else body = parse_block(parser);
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
    AstGenericParameter *generics=parse_generic_parameters(parser);
    if (declaration != NULL) declaration->generic_parameters=generics;
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
    AstGenericParameter *generics=parse_generic_parameters(parser);
    if (declaration) declaration->generic_parameters=generics;
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
        AstTypeArgument *payload=NULL, **payload_tail=&payload;
        AstExpression *arguments = NULL;
        AstExpression **argument_tail = &arguments;
        if (match(parser, TOKEN_LPAREN)) {
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (fields == NULL) {
                        AstTypeArgument *argument=allocate(parser,sizeof(*argument));
                        AstType t=parse_type(parser);
                        if (argument) { argument->type=t; *payload_tail=argument; payload_tail=&argument->next; }
                        if (declaration) declaration->as.enum_decl.is_sum=1;
                    } else {
                        AstExpression *argument = parse_expression(parser);
                        *argument_tail = argument;
                        if (argument != NULL) argument_tail = &argument->next;
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            (void) consume(parser, TOKEN_RPAREN);
        }
        if (value != NULL) {
            value->name_token = value_name;
            value->arguments = arguments;
            value->payload_types=payload;
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
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_IMPORT, first);
    AstImportPath *head = NULL;
    AstImportPath **tail = &head;
    do {
        if (grouped && check(parser, TOKEN_RPAREN)) break;
        size_t path_start = parser->current;
        AstImportPath *entry = allocate(parser, sizeof(*entry));
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
        if (entry != NULL) {
            entry->path_token = path_token;
            entry->path_first_token = path_first;
            entry->path_token_count = parser->current - path_first;
            entry->span = range_span(parser, path_start, parser->current);
            entry->resolved_symbol_id = AST_SYMBOL_NONE;
            *tail = entry;
            tail = &entry->next;
        }
    } while (grouped && !parser->failed);
    if (grouped) (void) consume(parser, TOKEN_RPAREN);
    if (head == NULL && !parser->failed)
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                       "Import group must contain at least one path");
    if (declaration != NULL) {
        declaration->as.import_decl.paths = head;
        finish_declaration(parser, declaration);
    }
    return declaration;
}

static AstDeclarationNode *parse_trait(SyntaxParser *parser,int implementation) {
    size_t first=parser->current++;
    AstDeclarationNode *d=new_declaration(parser,implementation ? AST_DECL_IMPL : AST_DECL_TRAIT,first);
    size_t trait=consume(parser,TOKEN_IDENTIFIER);
    AstType for_type=inferred_type();
    if (implementation) { (void)consume(parser,TOKEN_KEYWORD_FOR); for_type=parse_type(parser); }
    (void)consume(parser,TOKEN_LBRACE);
    AstDeclarationNode *head=NULL, **tail=&head;
    int saved=parser->trait_signature; parser->trait_signature=!implementation;
    while (!parser->failed && !check(parser,TOKEN_RBRACE) && !check(parser,TOKEN_EOF)) {
        AstDeclarationNode *method=parse_function(parser,0,implementation ? for_type.name_token : trait);
        *tail=method; if (method) tail=&method->next;
    }
    parser->trait_signature=saved;
    (void)consume(parser,TOKEN_RBRACE);
    if (d) {
        d->name_token=trait;
        if (implementation) { d->as.impl_decl.trait_token=trait; d->as.impl_decl.for_type=for_type; d->as.impl_decl.methods=head; }
        else d->as.trait_decl.methods=head;
    }
    finish_declaration(parser,d); return d;
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

int frontend_build_structured_ast_recover(AstProgram *program, int recover_syntax) {
    if (program == NULL) return 0;
    program->structured_error_token = AST_TOKEN_NONE;
    SyntaxParser parser = {.program = program, .recover_syntax = recover_syntax};
    AstDeclarationNode **tail = &program->root;
    while (!parser.failed && !check(&parser, TOKEN_EOF)) {
        size_t first = parser.current;
        if (recover_syntax) parser.reported = 0;
        AstDeclarationNode *declaration = NULL;
        if (check(&parser, TOKEN_KEYWORD_IMPORT)) declaration = parse_import(&parser);
        else if (check(&parser, TOKEN_KEYWORD_CONST)) declaration = parse_constant(&parser);
        else if (check(&parser, TOKEN_KEYWORD_FUNC))
            declaration = parse_function(&parser, 0, AST_TOKEN_NONE);
        else if (check(&parser, TOKEN_KEYWORD_STRUCT)) declaration = parse_struct(&parser);
        else if (check(&parser, TOKEN_KEYWORD_ENUM)) declaration = parse_enum(&parser);
        else if (check(&parser,TOKEN_KEYWORD_TRAIT)) declaration=parse_trait(&parser,0);
        else if (check(&parser,TOKEN_KEYWORD_IMPL)) declaration=parse_trait(&parser,1);
        else if (check(&parser, TOKEN_HASH))
            parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                           "#import was removed; use import path");
        else parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                            "Expected function declaration");
        if (parser.failed && recover_syntax && !parser.allocation_failed) {
            int braces = 0;
            for (size_t i = first; i < parser.current && i < program->token_count; i++) {
                if (program->tokens[i].type == TOKEN_LBRACE) braces++;
                if (program->tokens[i].type == TOKEN_RBRACE) braces--;
            }
            while (!check(&parser, TOKEN_EOF)) {
                TokenType token = current_type(&parser);
                if (braces <= 0 && parser.current > first &&
                    (token == TOKEN_KEYWORD_FUNC || token == TOKEN_KEYWORD_STRUCT ||
                     token == TOKEN_KEYWORD_ENUM || token == TOKEN_KEYWORD_IMPORT || token == TOKEN_KEYWORD_CONST)) break;
                if (token == TOKEN_LBRACE) braces++;
                if (token == TOKEN_RBRACE) braces--;
                parser.current++;
            }
            declaration = NULL;
            parser.failed = 0;
        }
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

int frontend_build_structured_ast(AstProgram *program) {
    return frontend_build_structured_ast_recover(program, 0);
}
