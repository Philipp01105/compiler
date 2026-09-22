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
    size_t type_expression_end;
    int interface_signature;
    int native_declaration;
    int pending_equal;
    int failed;
    int reported;
    int recover_syntax;
    int allocation_failed;
    int value_context;
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
                                                                  candidate->span.begin.line,
                                                                  candidate->span.begin.column,
                                                                  ERROR_CATEGORY_PARSER, code,
                                                                  parser->program->source_path,
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
        case TOKEN_KEYWORD_ASYNC: return "async";
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
    parser->pending_equal = 0;
    parser->current++;
    return 1;
}

static size_t consume(SyntaxParser *parser, TokenType type) {
    if (type == TOKEN_GREATER && !parser->pending_equal && check(parser, TOKEN_GREATER_EQUAL)) {
        parser->pending_equal = 1;
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
                                                             ERROR_CATEGORY_PARSER, ERR_PARSE_MISSING_SEMICOLON,
                                                             parser->program->source_path,
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
        parser_failure(parser, type == TOKEN_RBRACE
                                   ? ERR_PARSE_MISSING_BRACE
                                   : type == TOKEN_RPAREN
                                         ? ERR_PARSE_MISSING_PAREN
                                         : ERR_PARSE_EXPECTED_TOKEN, message);
        return AST_TOKEN_NONE;
    }
    parser->pending_equal = 0;
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
    type.lifetime_token = AST_TOKEN_NONE;
    return type;
}

static int is_type_token(TokenType type) {
    return type == TOKEN_IDENTIFIER ||
           type == TOKEN_KEYWORD_FUNC ||
           (type >= TOKEN_TYPE_INT && type <= TOKEN_TYPE_NEVER);
}

static AstGenericParameter *parse_generic_parameters(SyntaxParser *parser,
                                                      AstLifetimeParameter **lifetimes);

static void require_lifetime_edition(SyntaxParser *parser) {
    if (parser->program->module && strcmp(parser->program->module->edition, "2026-10-04-dev"))
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                       "Explicit lifetimes require DMM 2026-10-04-dev");
}

static AstType parse_type(SyntaxParser *parser) {
    AstType type = inferred_type();
    if (++parser->type_depth > AST_MAX_PARSE_DEPTH) {
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Type nesting exceeds parser limit");
        parser->type_depth--;
        return type;
    }
    size_t first = parser->current;
    AstBorrowKind borrow_kind = AST_BORROW_NONE;
    size_t lifetime_token = AST_TOKEN_NONE;
    if (match(parser, TOKEN_AMPERSAND)) {
        if (check(parser, TOKEN_LIFETIME)) {
            require_lifetime_edition(parser);
            lifetime_token = parser->current++;
        }
        borrow_kind = match(parser, TOKEN_KEYWORD_MUT) ? AST_BORROW_MUTABLE : AST_BORROW_IMMUTABLE;
    }
    unsigned leading_pointers = 0;
    while (match(parser, TOKEN_STAR)) leading_pointers++;
    unsigned callable_mode=0;
    if(check(parser,TOKEN_KEYWORD_MUT) && parser->current+1<parser->program->token_count &&
       parser->program->tokens[parser->current+1].type==TOKEN_KEYWORD_FUNC) {
        callable_mode=1; parser->current++;
    }
    else if(!strcmp(ast_program_lexeme(parser->program,parser->current),"once") &&
            parser->current+1<parser->program->token_count &&
            parser->program->tokens[parser->current+1].type==TOKEN_KEYWORD_FUNC) {
        callable_mode=2; parser->current++;
    }
    int native_function = match(parser, TOKEN_KEYWORD_EXTERN);
    if (native_function) {
        size_t abi = consume(parser, TOKEN_STRING_LITERAL);
        if (strcmp(ast_program_lexeme(parser->program, abi), "system"))
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Only extern system function ABI is supported");
        if (!check(parser, TOKEN_KEYWORD_FUNC))
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Expected native function-pointer type");
    }
    if (match(parser, TOKEN_LPAREN)) {
        type = parse_type(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (type.is_array || type.is_slice) type.outer_pointer_depth += leading_pointers;
        else type.pointer_depth += leading_pointers;
    } else if (match(parser, TOKEN_KEYWORD_FUNC)) {
        type.kind = AST_TYPE_FUNCTION;
        type.is_native_function = native_function;
        type.callable_mode=callable_mode;
        type.name_token = first;
        type.pointer_depth = leading_pointers;
        type.function_generic_parameters = parse_generic_parameters(parser, &type.function_lifetime_parameters);
        (void) consume(parser, TOKEN_LPAREN);
        AstTypeArgument **tail = &type.function_parameters;
        size_t count = 0;
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                if (++count > 16) {
                    parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                                   "Function types allow at most 16 parameters");
                    break;
                }
                AstTypeArgument *parameter = allocate(parser, sizeof(*parameter));
                AstType value = parse_type(parser);
                if (parameter != NULL) {
                    parameter->type = value;
                    *tail = parameter;
                    tail = &parameter->next;
                }
            } while (match(parser, TOKEN_COMMA));
        }
        (void) consume(parser, TOKEN_RPAREN);
        (void) consume(parser, TOKEN_ARROW);
        AstType *result = allocate(parser, sizeof(*result));
        AstType result_value = parse_type(parser);
        if (result != NULL) {
            *result = result_value;
            type.function_return_type = result;
        }
    } else {
        if (!is_type_token(current_type(parser))) {
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN, "Unknown type: expected a type name");
            parser->type_depth--;
            return type;
        }
        type.kind = AST_TYPE_NAMED;
        type.name_token = parser->current++;
        if ((!parser->type_expression_end || parser->current < parser->type_expression_end) &&
            match(parser, TOKEN_DOT)) {
            const char *qualifier = ast_program_lexeme(parser->program, type.name_token);
            size_t member = consume(parser, TOKEN_IDENTIFIER);
            char qualified[MAX_TOKEN];
            snprintf(qualified, sizeof(qualified), "%s.%s", qualifier, ast_program_lexeme(parser->program, member));
            const char *interned = string_interner_intern(parser->program->strings, qualified);
            if (!interned) parser->failed = 1;
            else parser->program->tokens[type.name_token].lexeme = interned;
        }
        type.pointer_depth = leading_pointers;
        if (match(parser, TOKEN_LESS)) {
            AstTypeArgument **tail = &type.arguments;
            AstLifetimeParameter **lifetime_tail = &type.lifetime_arguments;
            size_t count = 0;
            do {
                if (++count > 16) {
                    parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Generic types allow at most 16 arguments");
                    break;
                }
                if (check(parser, TOKEN_LIFETIME)) {
                    require_lifetime_edition(parser);
                    if (type.arguments)
                        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                                       "Lifetime arguments must precede type arguments");
                    AstLifetimeParameter *argument = allocate(parser, sizeof(*argument));
                    size_t token = parser->current++;
                    if (argument) {
                        argument->name_token = token;
                        *lifetime_tail = argument;
                        lifetime_tail = &argument->next;
                    }
                    continue;
                }
                AstTypeArgument *argument = allocate(parser, sizeof(*argument));
                AstType value = parse_type(parser);
                if (argument != NULL) {
                    argument->type = value;
                    *tail = argument;
                    tail = &argument->next;
                }
            } while (match(parser, TOKEN_COMMA));
            (void) consume(parser, TOKEN_GREATER);
        }
        const char *async_name = ast_program_lexeme(parser->program, type.name_token);
        if (!strcmp(async_name, "Future") || !strcmp(async_name, "JoinHandle")) {
            type.kind = !strcmp(async_name, "Future") ? AST_TYPE_FUTURE : AST_TYPE_JOIN;
            if (type.arguments == NULL || type.arguments->next != NULL)
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                               "Future requires exactly one result type");
        }
        if (!strcmp(async_name, "Executor")) {
            type.kind = AST_TYPE_EXECUTOR;
            if (type.arguments) parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                                               "Executor takes no type arguments");
        }
    }
    while (match(parser, TOKEN_LBRACKET)) {
        if (type.is_array || type.is_slice) {
            AstType *element = allocate(parser, sizeof(*element));
            if (element == NULL) break;
            *element = type;
            type.element_type = element;
            type.borrow_kind = AST_BORROW_NONE;
            type.pointer_depth = 0;
            type.outer_pointer_depth = 0;
            type.is_array = 0;
            type.is_slice = 0;
            type.array_length_token = AST_TOKEN_NONE;
            type.resolved_array_length = 0;
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
                type.resolved_array_length = (size_t) strtoull(
                    ast_program_lexeme(parser->program, type.array_length_token), NULL, 10);
            (void) consume(parser, TOKEN_RBRACKET);
        }
    }
    if (borrow_kind != AST_BORROW_NONE) {
        if (type.borrow_kind != AST_BORROW_NONE)
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Nested checked-reference types are not supported");
        else
            type.borrow_kind = borrow_kind;
        type.lifetime_token = lifetime_token;
    }
    type.span = range_span(parser, first, parser->current);
    if (parser->pending_equal) {
        type.span.end = token_span(parser, parser->current).begin;
        type.span.end.column++;
    }
    parser->type_depth--;
    return type;
}

static AstExpression *parse_expression(SyntaxParser *parser);

static AstStatement *parse_statement(SyntaxParser *parser);
static AstStatement *parse_statement_impl(SyntaxParser *parser);
static AstStatement *parse_value_block(SyntaxParser *parser);
static AstStatement *parse_expression_statement(SyntaxParser *parser, int consume_semicolon);

static AstGenericParameter *parse_generic_parameters(SyntaxParser *parser,
                                                      AstLifetimeParameter **lifetimes) {
    AstGenericParameter *head = NULL, **tail = &head;
    AstLifetimeParameter *lifetime_head = NULL, **lifetime_tail = &lifetime_head;
    if (!match(parser, TOKEN_LESS)) return NULL;
    size_t count = 0;
    do {
        if (++count > 16) {
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Generic declarations allow at most 16 type parameters");
            break;
        }
        if (check(parser, TOKEN_LIFETIME)) {
            require_lifetime_edition(parser);
            if (head)
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                               "Lifetime parameters must precede type parameters");
            AstLifetimeParameter *parameter = allocate(parser, sizeof(*parameter));
            size_t token = parser->current++;
            if (parameter) {
                parameter->name_token = token;
                *lifetime_tail = parameter;
                lifetime_tail = &parameter->next;
            }
            continue;
        }
        AstGenericParameter *parameter = allocate(parser, sizeof(*parameter));
        size_t name = consume(parser, TOKEN_IDENTIFIER);
        if (parameter != NULL) {
            parameter->name_token = name;
            if (match(parser, TOKEN_COLON)) {
                AstInterfaceBound **bound = &parameter->bounds;
                do {
                    AstInterfaceBound *b = allocate(parser, sizeof(*b));
                    AstType bound_type = parse_type(parser);
                    size_t token = bound_type.name_token;
                    if (b != NULL) {
                        b->name_token = token;
                        b->type = bound_type;
                        *bound = b;
                        bound = &b->next;
                    }
                } while (match(parser, TOKEN_PLUS));
            }
            *tail = parameter;
            tail = &parameter->next;
        }
    } while (match(parser, TOKEN_COMMA));
    (void) consume(parser, TOKEN_GREATER);
    if (lifetimes) *lifetimes = lifetime_head;
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
    expression->initializer_name_token = AST_TOKEN_NONE;
    expression->initializer_field_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_type = TYPE_UNKNOWN;
    expression->resolved_named_type_token = AST_TOKEN_NONE;
    expression->resolved_named_symbol_id = AST_SYMBOL_NONE;
    expression->resolved_symbol_id = AST_SYMBOL_NONE;
    expression->propagation_branch_symbol_id = AST_SYMBOL_NONE;
    expression->propagation_continue_symbol_id = AST_SYMBOL_NONE;
    expression->propagation_break_symbol_id = AST_SYMBOL_NONE;
    expression->propagation_from_residual_symbol_id = AST_SYMBOL_NONE;
    expression->propagation_return_variant_symbol_id = AST_SYMBOL_NONE;
    expression->allocated_type = inferred_type();
    return expression;
}

static void finish_expression(SyntaxParser *parser, AstExpression *expression) {
    if (expression == NULL) return;
    expression->token_count = parser->current - expression->first_token;
    expression->span = range_span(parser, expression->first_token, parser->current);
}

/* Recognize a type followed by .type without speculatively emitting diagnostics. */
static int look_type(const SyntaxParser *parser, size_t *index, unsigned depth) {
    if (depth > AST_MAX_PARSE_DEPTH || *index >= parser->program->token_count) return 0;
    const AstToken *tokens = parser->program->tokens;
    if (tokens[*index].type == TOKEN_AMPERSAND) {
        (*index)++;
        if (tokens[*index].type == TOKEN_LIFETIME) (*index)++;
        if (tokens[*index].type == TOKEN_KEYWORD_MUT) (*index)++;
    }
    while (tokens[*index].type == TOKEN_STAR) (*index)++;
    if (tokens[*index].type == TOKEN_LPAREN) {
        (*index)++;
        if (!look_type(parser, index, depth + 1) || tokens[*index].type != TOKEN_RPAREN) return 0;
        (*index)++;
    } else {
        if (!is_type_token(tokens[*index].type) && tokens[*index].type != TOKEN_IDENTIFIER) return 0;
        (*index)++;
        if (tokens[*index].type == TOKEN_DOT && *index + 1 < parser->program->token_count &&
            tokens[*index + 1].type == TOKEN_IDENTIFIER && strcmp(tokens[*index + 1].lexeme, "type") &&
            ((!strcmp(tokens[*index + 1].lexeme, "name") || !strcmp(tokens[*index + 1].lexeme, "size") || !strcmp(
                  tokens[*index + 1].lexeme, "align"))
                 ? (*index + 2 < parser->program->token_count && tokens[*index + 2].type == TOKEN_DOT)
                 : 1))
            *index += 2;
        if (tokens[*index].type == TOKEN_LESS) {
            (*index)++;
            do {
                if (tokens[*index].type == TOKEN_LIFETIME) (*index)++;
                else if (!look_type(parser, index, depth + 1)) return 0;
                if (tokens[*index].type != TOKEN_COMMA) break;
                (*index)++;
            } while (1);
            if (tokens[*index].type != TOKEN_GREATER) return 0;
            (*index)++;
        }
    }
    if (tokens[*index].type == TOKEN_LBRACKET) {
        (*index)++;
        if (tokens[*index].type == TOKEN_NUMBER || tokens[*index].type == TOKEN_IDENTIFIER) (*index)++;
        if (tokens[*index].type != TOKEN_RBRACKET) return 0;
        (*index)++;
    }
    return *index < parser->program->token_count;
}

static int type_metadata_ahead(const SyntaxParser *parser) {
    size_t index = parser->current;
    /* An enum variant followed by a field is a value expression. */
    if (index + 3 < parser->program->token_count &&
        parser->program->tokens[index].type == TOKEN_IDENTIFIER &&
        parser->program->tokens[index + 1].type == TOKEN_DOT &&
        parser->program->tokens[index + 3].type == TOKEN_DOT) {
        const char *name = parser->program->tokens[index].lexeme;
        for (const AstDeclarationNode *d = parser->program->root; d; d = d->next)
            if (d->kind == AST_DECL_ENUM &&
                !strcmp(name, ast_program_lexeme(parser->program, d->name_token)))
                return 0;
    }
    return look_type(parser, &index, 0) && index + 1 < parser->program->token_count &&
           parser->program->tokens[index].type == TOKEN_DOT &&
           parser->program->tokens[index + 1].type == TOKEN_IDENTIFIER &&
           (!strcmp(parser->program->tokens[index + 1].lexeme, "name") || !strcmp(
                parser->program->tokens[index + 1].lexeme, "size") || !strcmp(
                parser->program->tokens[index + 1].lexeme, "align"));
}

static AstParameter *parse_parameter(SyntaxParser *parser);
static AstStatement *parse_block(SyntaxParser *parser);
static AstDeclarationNode *new_declaration(SyntaxParser *parser, AstDeclarationKind kind, size_t first);

static AstExpression *parse_closure(SyntaxParser *parser) {
    size_t first=parser->current;
    (void)consume(parser,TOKEN_KEYWORD_FUNC);
    (void)consume(parser,TOKEN_LBRACKET);
    AstExpression *expression=new_expression(parser,AST_EXPR_STRUCT_LITERAL,first);
    AstDeclarationNode *environment=new_declaration(parser,AST_DECL_STRUCT,first);
    if(!expression || !environment) return expression;
    AstExpression **captures=&expression->arguments;
    AstField **fields=&environment->as.struct_decl.fields;
    while(!parser->failed && !check(parser,TOKEN_RBRACKET)) {
        size_t capture_first=parser->current;
        int borrowed=match(parser,TOKEN_AMPERSAND);
        int mutable=borrowed && match(parser,TOKEN_KEYWORD_MUT);
        if(!borrowed) {
            if(strcmp(ast_program_lexeme(parser->program,parser->current),"move"))
                parser_failure(parser,ERR_PARSE_INVALID_DECLARATION,"Captures require move, & or &mut");
            else parser->current++;
        }
        size_t name=consume(parser,TOKEN_IDENTIFIER);
        AstExpression *value=new_expression(parser,AST_EXPR_NAME,capture_first);
        AstField *field=allocate(parser,sizeof(*field));
        if(!value || !field) break;
        value->value_token=name;
        if(borrowed) {
            AstExpression *reference=new_expression(parser,AST_EXPR_UNARY,capture_first);
            if(!reference) break;
            reference->operator_type=TOKEN_AMPERSAND;
            reference->mutable_borrow=mutable;
            reference->right=value;
            value=reference;
        }
        value->initializer_name_token=name;
        finish_expression(parser,value);
        field->name_token=name;
        field->span=value->span;
        field->type=inferred_type();
        field->resolved_symbol_id=AST_SYMBOL_NONE;
        *fields=field; fields=&field->next;
        *captures=value; captures=&value->next;
        if(!match(parser,TOKEN_COMMA)) break;
    }
    (void)consume(parser,TOKEN_RBRACKET);
    AstDeclarationNode *invoke=new_declaration(parser,AST_DECL_FUNCTION,first);
    if(!invoke) return expression;
    (void)consume(parser,TOKEN_LPAREN);
    AstParameter **parameters=&invoke->as.function.parameters;
    if(!check(parser,TOKEN_RPAREN)) do {
        AstParameter *parameter=parse_parameter(parser);
        *parameters=parameter;
        if(parameter) parameters=&parameter->next;
    } while(match(parser,TOKEN_COMMA));
    (void)consume(parser,TOKEN_RPAREN);
    (void)consume(parser,TOKEN_ARROW);
    invoke->as.function.return_type=parse_type(parser);
    invoke->as.function.body=parse_block(parser);
    invoke->is_public=1;
    environment->is_closure_environment=1;
    environment->no_default=1;
    environment->as.struct_decl.methods=invoke;
    expression->closure_environment=environment;
    expression->allocated_type=inferred_type();
    finish_expression(parser,expression);
    return expression;
}

static AstExpression *parse_primary(SyntaxParser *parser) {
    size_t first = parser->current;
    TokenType type = current_type(parser);
    AstExpression *expression = NULL;
    size_t literal_type_end = parser->current;
    int struct_literal = type == TOKEN_IDENTIFIER &&
        look_type(parser, &literal_type_end, 0) &&
        literal_type_end + 1 < parser->program->token_count &&
        parser->program->tokens[literal_type_end].type == TOKEN_LBRACE &&
        (parser->program->tokens[literal_type_end + 1].type == TOKEN_RBRACE ||
         (literal_type_end + 2 < parser->program->token_count &&
          parser->program->tokens[literal_type_end + 1].type == TOKEN_IDENTIFIER &&
          parser->program->tokens[literal_type_end + 2].type == TOKEN_COLON));
    if(type==TOKEN_KEYWORD_FUNC) return parse_closure(parser);
    if (struct_literal) {
        expression = new_expression(parser, AST_EXPR_STRUCT_LITERAL, first);
        AstType literal_type = parse_type(parser);
        (void) consume(parser, TOKEN_LBRACE);
        AstExpression *fields = NULL, **tail = &fields;
        while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            size_t name = consume(parser, TOKEN_IDENTIFIER);
            (void) consume(parser, TOKEN_COLON);
            AstExpression *value = parse_expression(parser);
            if (value != NULL) {
                value->initializer_name_token = name;
                *tail = value;
                tail = &value->next;
            }
            if (!match(parser, TOKEN_COMMA)) break;
        }
        (void) consume(parser, TOKEN_RBRACE);
        if (expression != NULL) {
            expression->allocated_type = literal_type;
            expression->value_token = literal_type.name_token;
            expression->arguments = fields;
        }
    } else if (type_metadata_ahead(parser)) {
        expression = new_expression(parser, AST_EXPR_TYPE_INFO, first);
        size_t end = parser->current;
        (void) look_type(parser, &end, 0);
        size_t saved_end = parser->type_expression_end;
        parser->type_expression_end = end;
        AstType queried = parse_type(parser);
        parser->type_expression_end = saved_end;
        if (expression) {
            expression->allocated_type = queried;
            expression->value_token = queried.name_token;
        }
    } else if (type == TOKEN_NUMBER || type == TOKEN_FLOAT_LITERAL ||
               type == TOKEN_CHAR_LITERAL || type == TOKEN_STRING_LITERAL) {
        expression = new_expression(parser, AST_EXPR_LITERAL, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
    } else if (type == TOKEN_IDENTIFIER || type == TOKEN_KEYWORD_FREE || is_type_token(type)) {
        expression = new_expression(parser, AST_EXPR_NAME, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
    } else if (type == TOKEN_KEYWORD_RESERVE || type == TOKEN_KEYWORD_SIZEOF || type == TOKEN_KEYWORD_ALIGNOF || type ==
               TOKEN_KEYWORD_TYPEOF) {
        expression = new_expression(parser, type == TOKEN_KEYWORD_RESERVE
                                                ? AST_EXPR_RESERVE
                                                : type == TOKEN_KEYWORD_SIZEOF
                                                      ? AST_EXPR_SIZEOF
                                                      : AST_EXPR_ALIGNOF, first);
        if (expression != NULL) expression->value_token = parser->current;
        parser->current++;
        (void) consume(parser, TOKEN_LPAREN);
        AstType allocated_type = parse_type(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (expression != NULL) expression->allocated_type = allocated_type;
    } else if (type == TOKEN_KEYWORD_SLICE) {
        parser->current++;
        expression = new_expression(parser, AST_EXPR_SLICE, first);
        (void) consume(parser, TOKEN_LPAREN);
        AstExpression *data = parse_expression(parser);
        (void) consume(parser, TOKEN_COMMA);
        AstExpression *length = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        if (expression) {
            expression->left = data;
            expression->right = length;
        }
    } else if (match(parser, TOKEN_LBRACKET)) {
        expression = new_expression(parser, AST_EXPR_ARRAY_LITERAL, first);
        AstExpression *elements = NULL, **tail = &elements;
        if (check(parser, TOKEN_RBRACKET)) {
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                           "Array literal requires a non-empty element pattern");
        } else {
            do {
                AstExpression *element = parse_expression(parser);
                if (tail != NULL) {
                    *tail = element;
                    if (element != NULL) tail = &element->next;
                }
                if (!match(parser, TOKEN_COMMA)) break;
            } while (!parser->failed && !check(parser, TOKEN_RBRACKET) &&
                     !check(parser, TOKEN_SEMICOLON));
        }
        AstExpression *repeat = NULL;
        if (match(parser, TOKEN_SEMICOLON)) repeat = parse_expression(parser);
        (void) consume(parser, TOKEN_RBRACKET);
        if (expression != NULL) {
            expression->arguments = elements;
            expression->right = repeat;
        }
    } else if (type == TOKEN_LBRACE || type == TOKEN_KEYWORD_IF ||
               type == TOKEN_KEYWORD_MATCH) {
        expression = new_expression(parser, AST_EXPR_CONTROL, first);
        if (expression != NULL) {
            if (type == TOKEN_LBRACE) expression->control = parse_value_block(parser);
            else {
                int saved = parser->value_context;
                parser->value_context = 1;
                expression->control = parse_statement_impl(parser);
                parser->value_context = saved;
            }
        }
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

    if (expression != NULL && expression->kind == AST_EXPR_NAME && check(parser, TOKEN_LESS)) {
        size_t look = parser->current;
        unsigned depth = 0;
        do {
            TokenType t = parser->program->tokens[look++].type;
            if (t == TOKEN_LESS) depth++;
            if (t == TOKEN_GREATER) depth--;
            if (t == TOKEN_EOF || t == TOKEN_SEMICOLON) break;
        } while (look < parser->program->token_count && depth != 0);
        if (depth == 0 && look < parser->program->token_count &&
            (parser->program->tokens[look].type == TOKEN_DOT ||
             parser->program->tokens[look].type == TOKEN_LPAREN ||
             parser->program->tokens[look].type == TOKEN_SEMICOLON ||
             parser->program->tokens[look].type == TOKEN_COMMA ||
             parser->program->tokens[look].type == TOKEN_RPAREN ||
             parser->program->tokens[look].type == TOKEN_RBRACKET)) {
            size_t saved = parser->current;
            parser->current = expression->value_token;
            expression->allocated_type = parse_type(parser);
            expression->explicit_type_arguments =
                parser->program->tokens[look].type == TOKEN_LPAREN;
            expression->explicit_generic_reference =
                parser->program->tokens[look].type != TOKEN_LPAREN &&
                parser->program->tokens[look].type != TOKEN_DOT;
            if (parser->current <= saved) parser->failed = 1;
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
                if (callee >= TOKEN_TYPE_INT && callee <= TOKEN_TYPE_NEVER) {
                    parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                                   "Type-first casts were removed; use expression.(type)");
                    break;
                }
                if (callee == TOKEN_KEYWORD_FREE)
                    call_kind = AST_EXPR_FREE;
            }
            AstExpression *call = new_expression(parser, call_kind, first);
            AstExpression **tail = call == NULL ? NULL : &call->arguments;
            if (call != NULL) {
                expression->direct_call_target = 1;
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
            AstExpression *start = check(parser, TOKEN_COLON)
                                       ? NULL : parse_expression(parser);
            if (match(parser, TOKEN_COLON)) {
                AstExpression *subslice =
                    new_expression(parser, AST_EXPR_SUBSLICE, first);
                if (subslice != NULL) {
                    subslice->left = expression;
                    subslice->right = start;
                    if (!check(parser, TOKEN_RBRACKET))
                        subslice->arguments = parse_expression(parser);
                }
                (void) consume(parser, TOKEN_RBRACKET);
                expression = subslice;
                continue;
            }
            AstExpression *index = new_expression(parser, AST_EXPR_INDEX, first);
            if (index != NULL) {
                index->left = expression;
                index->right = start;
            }
            (void) consume(parser, TOKEN_RBRACKET);
            expression = index;
        } else if (match(parser, TOKEN_DOT)) {
            if (match(parser, TOKEN_KEYWORD_AWAIT)) {
                AstExpression *await = new_expression(parser, AST_EXPR_AWAIT, first);
                (void) consume(parser, TOKEN_LPAREN);
                if (!check(parser, TOKEN_RPAREN)) {
                    parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                                   "Future.await() takes no arguments");
                    break;
                }
                (void) consume(parser, TOKEN_RPAREN);
                if (await != NULL) {
                    await->operator_type = TOKEN_KEYWORD_AWAIT;
                    await->right = expression;
                }
                expression = await;
                finish_expression(parser, expression);
                continue;
            }
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
                if (expression->kind == AST_EXPR_NAME && check(parser, TOKEN_LESS)) {
                    size_t look = parser->current;
                    unsigned depth = 0;
                    do {
                        TokenType next = parser->program->tokens[look++].type;
                        if (next == TOKEN_LESS) depth++;
                        if (next == TOKEN_GREATER) depth--;
                        if (next == TOKEN_EOF || next == TOKEN_SEMICOLON) break;
                    } while (look < parser->program->token_count && depth);
                    if (!depth && look < parser->program->token_count &&
                        (parser->program->tokens[look].type == TOKEN_DOT || parser->program->tokens[look].type ==
                         TOKEN_LPAREN)) {
                        size_t saved = parser->current;
                        parser->current = expression->value_token;
                        member->allocated_type = parse_type(parser);
                        if (!parser->failed && parser->current <= saved)
                            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                                           "Expected generic arguments for qualified member");
                        member->explicit_type_arguments = parser->program->tokens[look].type == TOKEN_LPAREN;
                        member->kind = AST_EXPR_NAME;
                        member->left = NULL;
                        member->value_token = member->allocated_type.name_token;
                    }
                }
            }
            expression = member;
        } else if (match(parser, TOKEN_QUESTION)) {
            AstExpression *propagate = new_expression(parser, AST_EXPR_PROPAGATE, first);
            if (propagate != NULL) {
                propagate->left = expression;
                propagate->value_token = parser->current - 1;
            }
            expression = propagate;
        } else {
            break;
        }
        finish_expression(parser, expression);
    }
    finish_expression(parser, expression);
    return expression;
}

static AstExpression *parse_unary(SyntaxParser *parser) {
    if (check(parser, TOKEN_KEYWORD_AWAIT)) {
        parser_failure(parser, ERR_PARSE_UNEXPECTED_TOKEN,
                       "Prefix await was removed; use future.await()");
        return new_expression(parser, AST_EXPR_ERROR, parser->current);
    }
    if (type_metadata_ahead(parser)) return parse_primary(parser);
    AstExpression *operators[AST_MAX_PARSE_DEPTH];
    size_t count = 0;
    for (;;) {
        TokenType type = current_type(parser);
        if (type != TOKEN_BANG && type != TOKEN_MINUS &&
            type != TOKEN_AMPERSAND && type != TOKEN_STAR)
            break;
        if (parser->expression_depth + count >= AST_MAX_PARSE_DEPTH) {
            parser_failure(parser, ERR_PARSE_TOO_MANY_ERRORS,
                           "Expression tree exceeds maximum depth");
            return NULL;
        }
        size_t first = parser->current++;
        AstExpression *expression = new_expression(parser,
                                                   AST_EXPR_UNARY,
                                                   first);
        if (expression != NULL) {
            expression->operator_type = type;
            if (type == TOKEN_AMPERSAND && match(parser, TOKEN_KEYWORD_MUT)) expression->mutable_borrow = 1;
        }
        operators[count++] = expression;
    }
    parser->expression_depth += (unsigned) count;
    AstExpression *expression = parse_primary(parser);
    parser->expression_depth -= (unsigned) count;
    while (count != 0) {
        AstExpression *unary = operators[--count];
        if (unary != NULL) unary->right = expression;
        finish_expression(parser, unary);
        expression = unary;
    }
    return expression;
}

static int precedence(TokenType type) {
    if (type == TOKEN_PIPE_PIPE) return 1;
    if (type == TOKEN_AMP_AMP) return 2;
    if (type == TOKEN_EQUAL_EQUAL || type == TOKEN_BANG_EQUAL ||
        type == TOKEN_LESS || type == TOKEN_LESS_EQUAL ||
        type == TOKEN_GREATER || type == TOKEN_GREATER_EQUAL)
        return 3;
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
                                                  parser->program->tokens[parser->current - 1].type ==
                                                  TOKEN_SEMICOLON)) {
                TokenType token = current_type(parser);
                if (braces <= 0 && token == TOKEN_RBRACE) break;
                if (braces <= 0 && token == TOKEN_SEMICOLON) {
                    parser->current++;
                    break;
                }
                if (braces <= 0 && parser->current > child_first &&
                    (token == TOKEN_KEYWORD_VAR || token == TOKEN_KEYWORD_CONST ||
                     token == TOKEN_KEYWORD_RETURN || token == TOKEN_KEYWORD_IF ||
                     token == TOKEN_KEYWORD_FOR || token == TOKEN_KEYWORD_WHILE ||
                     token == TOKEN_KEYWORD_BREAK || token == TOKEN_KEYWORD_CONTINUE))
                    break;
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

static int control_value_ahead(const SyntaxParser *parser) {
    TokenType kind = current_type(parser);
    if (kind != TOKEN_KEYWORD_IF && kind != TOKEN_KEYWORD_MATCH)
        return 0;
    unsigned depth = 0;
    int saw_body = 0;
    int saw_else = 0;
    for (size_t i = parser->current; i < parser->program->token_count; i++) {
        TokenType token = parser->program->tokens[i].type;
        if (token == TOKEN_LBRACE) {
            depth++;
            saw_body = 1;
        } else if (token == TOKEN_RBRACE) {
            if (depth == 0) return 0;
            depth--;
            if (depth == 0 && saw_body) {
                TokenType next = i + 1 < parser->program->token_count
                                     ? parser->program->tokens[i + 1].type
                                     : TOKEN_EOF;
                if (kind == TOKEN_KEYWORD_IF && !saw_else) {
                    if (next != TOKEN_KEYWORD_ELSE) return 0;
                    saw_else = 1;
                    continue;
                }
                return next == TOKEN_RBRACE || next == TOKEN_SEMICOLON;
            }
        }
    }
    return 0;
}

static AstStatement *parse_value_block(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_LBRACE);
    AstStatement *block = new_statement(parser, AST_STMT_BLOCK, first);
    AstStatement **tail = block == NULL ? NULL : &block->body;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) &&
           !check(parser, TOKEN_EOF)) {
        TokenType type = current_type(parser);
        AstStatement *child;
        if (type == TOKEN_KEYWORD_VAR || type == TOKEN_KEYWORD_CONST ||
            type == TOKEN_KEYWORD_RETURN || type == TOKEN_KEYWORD_DEFER ||
            type == TOKEN_KEYWORD_FOR || type == TOKEN_KEYWORD_WHILE ||
            type == TOKEN_KEYWORD_BREAK || type == TOKEN_KEYWORD_CONTINUE ||
            ((type == TOKEN_KEYWORD_IF || type == TOKEN_KEYWORD_MATCH) &&
             !control_value_ahead(parser))) {
            int saved = parser->value_context;
            parser->value_context = 0;
            child = parse_statement(parser);
            parser->value_context = saved;
        } else {
            child = parse_expression_statement(parser, 0);
            if (check(parser, TOKEN_RBRACE) && child != NULL &&
                child->kind == AST_STMT_EXPRESSION) {
                if (block != NULL) block->result = child->expression;
                break;
            }
            (void) consume(parser, TOKEN_SEMICOLON);
            finish_statement(parser, child);
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

    if (match(parser, TOKEN_KEYWORD_MATCH)) {
        AstStatement *statement = new_statement(parser, AST_STMT_MATCH, first);
        (void) consume(parser, TOKEN_LPAREN);
        size_t type_end = parser->current;
        AstExpression *value;
        if (look_type(parser, &type_end, 0) && type_end + 2 < parser->program->token_count &&
            parser->program->tokens[type_end].type == TOKEN_RPAREN &&
            parser->program->tokens[type_end + 1].type == TOKEN_LBRACE &&
            parser->program->tokens[type_end + 2].type == TOKEN_KEYWORD_CASE) {
            value = new_expression(parser, AST_EXPR_TYPE_INFO, parser->current);
            size_t saved_end = parser->type_expression_end;
            parser->type_expression_end = type_end;
            AstType queried = parse_type(parser);
            parser->type_expression_end = saved_end;
            if (value) {
                value->allocated_type = queried;
                value->value_token = queried.name_token;
                finish_expression(parser, value);
            }
        } else value = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        (void) consume(parser, TOKEN_LBRACE);
        AstMatchArm *head = NULL, **tail = &head;
        while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            size_t arm_first = parser->current;
            AstMatchArm *arm = allocate(parser, sizeof(*arm));
            int type_pattern = match(parser, TOKEN_KEYWORD_CASE);
            AstType pattern = inferred_type();
            size_t variant;
            if (type_pattern) {
                pattern = parse_type(parser);
                variant = pattern.name_token;
            } else variant = consume(parser, TOKEN_IDENTIFIER);
            AstParameter *bindings = NULL, **binding_tail = &bindings;
            if (!type_pattern && match(parser, TOKEN_LPAREN)) {
                if (!check(parser, TOKEN_RPAREN))
                    do {
                        AstParameter *binding = allocate(parser, sizeof(*binding));
                        size_t token = consume(parser, TOKEN_IDENTIFIER);
                        if (binding) {
                            binding->name_token = token;
                            binding->type = inferred_type();
                            binding->resolved_symbol_id = AST_SYMBOL_NONE;
                            *binding_tail = binding;
                            binding_tail = &binding->next;
                        }
                    } while (match(parser, TOKEN_COMMA));
                (void) consume(parser, TOKEN_RPAREN);
            }
            if (type_pattern) (void) consume(parser, TOKEN_ARROW);
            else (void) consume(parser, TOKEN_FAT_ARROW);
            AstStatement *body = parser->value_context
                                     ? parse_value_block(parser)
                                     : parse_statement(parser);
            if (arm) {
                arm->variant_token = variant;
                arm->is_type_pattern = type_pattern;
                arm->type = pattern;
                arm->wildcard = !strcmp(ast_program_lexeme(parser->program, variant), "_");
                arm->bindings = bindings;
                arm->body = body;
                arm->span = range_span(parser, arm_first, parser->current);
                arm->resolved_variant_symbol = AST_SYMBOL_NONE;
                *tail = arm;
                tail = &arm->next;
            }
        }
        (void) consume(parser, TOKEN_RBRACE);
        if (statement) {
            statement->value = value;
            statement->match_arms = head;
        }
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_IF)) {
        AstStatement *statement = new_statement(parser, AST_STMT_IF, first);
        (void) consume(parser, TOKEN_LPAREN);
        AstExpression *condition = parse_expression(parser);
        (void) consume(parser, TOKEN_RPAREN);
        AstStatement *body = parser->value_context
                                 ? parse_value_block(parser)
                                 : parse_statement(parser);
        AstStatement *else_body = NULL;
        if (match(parser, TOKEN_KEYWORD_ELSE))
            else_body = parser->value_context
                            ? parse_value_block(parser)
                            : parse_statement(parser);
        else if (parser->value_context)
            parser_failure(parser, ERR_PARSE_EXPECTED_TOKEN,
                           "Value if requires an else branch");
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
        if (statement != NULL) {
            statement->condition = condition;
            statement->body = body;
        }
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
    if (match(parser, TOKEN_KEYWORD_DEFER)) {
        AstStatement *statement = new_statement(parser, AST_STMT_DEFER, first);
        if (match(parser, TOKEN_KEYWORD_FUNC)) {
            (void) consume(parser, TOKEN_LPAREN);
            (void) consume(parser, TOKEN_RPAREN);
            if (statement != NULL) statement->body = parse_block(parser);
            else (void) parse_block(parser);
        } else {
            AstExpression *operation = parse_expression(parser);
            (void) consume(parser, TOKEN_SEMICOLON);
            if (statement != NULL) statement->expression = operation;
        }
        finish_statement(parser, statement);
        return statement;
    }
    if (match(parser, TOKEN_KEYWORD_BREAK) || match(parser, TOKEN_KEYWORD_CONTINUE)) {
        TokenType keyword = parser->program->tokens[first].type;
        AstStatement *statement = new_statement(parser,
                                                keyword == TOKEN_KEYWORD_BREAK ? AST_STMT_BREAK : AST_STMT_CONTINUE,
                                                first);
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
        declaration->native_abi_token = AST_TOKEN_NONE;
        declaration->native_library_token = AST_TOKEN_NONE;
        declaration->native_name_token = AST_TOKEN_NONE;
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
    int is_async = match(parser, TOKEN_KEYWORD_ASYNC);
    (void) consume(parser, TOKEN_KEYWORD_FUNC);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_FUNCTION, first);
    size_t name = consume_callable_name(parser);
    AstLifetimeParameter *lifetimes = NULL;
    AstGenericParameter *generics = parse_generic_parameters(parser, &lifetimes);
    if (declaration != NULL) { declaration->generic_parameters = generics; declaration->lifetime_parameters = lifetimes; }
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
    AstAutoCondition *conditions = NULL, **condition_tail = &conditions;
    if (!strcmp(ast_program_lexeme(parser->program, parser->current), "where")) {
        if (parser->program->module && strcmp(parser->program->module->edition, "2026-10-04-dev"))
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Method where constraints require DMM 2026-10-04-dev");
        parser->current++;
        if (owner_token == AST_TOKEN_NONE || parser->native_declaration || parser->interface_signature)
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Method where constraints require an aggregate method");
        do {
            AstAutoCondition *condition = allocate(parser, sizeof(*condition));
            AstType subject = parse_type(parser);
            (void)consume(parser, TOKEN_COLON);
            AstInterfaceBound *bounds = NULL, **bound_tail = &bounds;
            do {
                AstInterfaceBound *bound = allocate(parser, sizeof(*bound));
                AstType type = parse_type(parser);
                if (bound) {
                    bound->type = type;
                    bound->name_token = type.name_token;
                    *bound_tail = bound;
                    bound_tail = &bound->next;
                }
            } while (match(parser, TOKEN_PLUS));
            if (condition) {
                condition->type = subject;
                condition->bounds = bounds;
                *condition_tail = condition;
                condition_tail = &condition->next;
            }
        } while (match(parser, TOKEN_COMMA));
    }
    AstStatement *body = NULL;
    size_t native_name = name;
    if (parser->native_declaration) {
        if (generics != NULL || is_async)
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Native functions must be synchronous and non-generic");
        if (match(parser, TOKEN_EQUAL)) native_name = consume(parser, TOKEN_STRING_LITERAL);
        if (check(parser, TOKEN_LBRACE))
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Native imports cannot have a function body");
        (void) consume(parser, TOKEN_SEMICOLON);
    } else if (parser->interface_signature) {
        body = new_statement(parser, AST_STMT_BLOCK, parser->current);
        (void) consume(parser, TOKEN_SEMICOLON);
        finish_statement(parser, body);
    } else body = parse_block(parser);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->where_conditions = conditions;
        declaration->native_name_token = native_name;
        declaration->as.function.parameters = parameters;
        declaration->as.function.return_type = return_type;
        declaration->as.function.body = body;
        declaration->as.function.is_static = is_static;
        declaration->as.function.is_async = is_async;
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
    int is_union = match(parser, TOKEN_KEYWORD_UNION);
    if (!is_union) (void) consume(parser, TOKEN_KEYWORD_STRUCT);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_STRUCT, first);
    if (declaration) declaration->is_native_union = is_union;
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstLifetimeParameter *lifetimes = NULL;
    AstGenericParameter *generics = parse_generic_parameters(parser, &lifetimes);
    if (declaration != NULL) { declaration->generic_parameters = generics; declaration->lifetime_parameters = lifetimes; }
    if (parser->native_declaration && generics != NULL)
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Native structs cannot be generic");
    if (parser->native_declaration && match(parser, TOKEN_SEMICOLON)) {
        if (declaration != NULL) {
            declaration->name_token = name;
            declaration->is_opaque = 1;
        }
        finish_declaration(parser, declaration);
        return declaration;
    }
    (void) consume(parser, TOKEN_LBRACE);
    AstField *fields = NULL;
    AstField **field_tail = &fields;
    AstDeclarationNode *methods = NULL;
    AstDeclarationNode **method_tail = &methods;
    AstStatement *destructor = NULL;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (parser->native_declaration &&
            (check(parser, TOKEN_KEYWORD_DESTRUCTOR) || check(parser, TOKEN_KEYWORD_FUNC) ||
             check(parser, TOKEN_KEYWORD_ASYNC) || check(parser, TOKEN_KEYWORD_STATIC))) {
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Native structs permit only var fields");
            break;
        }
        if (check(parser, TOKEN_KEYWORD_DESTRUCTOR)) {
            if (destructor != NULL) {
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                               "A struct may declare only one destructor");
                break;
            }
            parser->current++;
            destructor = parse_block(parser);
            continue;
        }
        int is_public = match(parser, TOKEN_KEYWORD_PUB);
        int is_static = match(parser, TOKEN_KEYWORD_STATIC);
        if (parser->native_declaration && (is_static ||
            check(parser, TOKEN_KEYWORD_FUNC) || check(parser, TOKEN_KEYWORD_ASYNC))) {
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "Native structs permit only var fields");
            break;
        }
        if (check(parser, TOKEN_KEYWORD_FUNC) || check(parser, TOKEN_KEYWORD_ASYNC)) {
            AstDeclarationNode *method = parse_function(parser, is_static, name);
            if (method) method->is_public = is_public;
            *method_tail = method;
            if (method != NULL) method_tail = &method->next;
        } else if (!is_static && check(parser, TOKEN_KEYWORD_VAR)) {
            AstField *field = parse_field(parser);
            if (field) field->is_public = is_public;
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
        declaration->as.struct_decl.destructor = destructor;
    }
    (void) match(parser, TOKEN_SEMICOLON);
    finish_declaration(parser, declaration);
    return declaration;
}

static AstDeclarationNode *parse_enum(SyntaxParser *parser) {
    size_t first = parser->current;
    (void) consume(parser, TOKEN_KEYWORD_ENUM);
    AstDeclarationNode *declaration = new_declaration(parser, AST_DECL_ENUM, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstLifetimeParameter *lifetimes = NULL;
    AstGenericParameter *generics = parse_generic_parameters(parser, &lifetimes);
    if (declaration) { declaration->generic_parameters = generics; declaration->lifetime_parameters = lifetimes; }
    AstField *fields = NULL;
    AstField **field_tail = &fields;
    if (match(parser, TOKEN_LPAREN)) {
        if (!check(parser, TOKEN_RPAREN)) {
            do {
                size_t field_first = parser->current;
                AstField *field = allocate(parser, sizeof(*field));
                int is_public = match(parser, TOKEN_KEYWORD_PUB);
                size_t field_name = consume(parser, TOKEN_IDENTIFIER);
                (void) consume(parser, TOKEN_COLON);
                AstType field_type = parse_type(parser);
                if (field != NULL) {
                    field->is_public = is_public;
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
    while (!parser->failed && !check(parser, TOKEN_RBRACE) &&
           !check(parser, TOKEN_SEMICOLON) && !check(parser, TOKEN_EOF)) {
        size_t value_first = parser->current;
        AstEnumValue *value = allocate(parser, sizeof(*value));
        int is_public = match(parser, TOKEN_KEYWORD_PUB);
        size_t value_name = consume(parser, TOKEN_IDENTIFIER);
        AstTypeArgument *payload = NULL, **payload_tail = &payload;
        AstExpression *arguments = NULL;
        AstExpression **argument_tail = &arguments;
        if (match(parser, TOKEN_LPAREN)) {
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (fields == NULL) {
                        AstTypeArgument *argument = allocate(parser, sizeof(*argument));
                        AstType t = parse_type(parser);
                        if (argument) {
                            argument->type = t;
                            *payload_tail = argument;
                            payload_tail = &argument->next;
                        }
                        if (declaration) declaration->as.enum_decl.is_sum = 1;
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
            value->is_public = is_public;
            value->name_token = value_name;
            value->arguments = arguments;
            value->payload_types = payload;
            value->resolved_symbol_id = AST_SYMBOL_NONE;
            value->span = range_span(parser, value_first, parser->current);
            *value_tail = value;
            value_tail = &value->next;
        }
        if (!match(parser, TOKEN_COMMA)) break;
    }
    AstDeclarationNode *methods = NULL, **method_tail = &methods;
    if (match(parser, TOKEN_SEMICOLON)) {
        while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
            int is_public = match(parser, TOKEN_KEYWORD_PUB);
            int is_static = match(parser, TOKEN_KEYWORD_STATIC);
            AstDeclarationNode *method = parse_function(parser, is_static, name);
            if (method != NULL) method->is_public = is_public;
            *method_tail = method;
            if (method != NULL) method_tail = &method->next;
        }
    }
    (void) consume(parser, TOKEN_RBRACE);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.enum_decl.fields = fields;
        declaration->as.enum_decl.values = values;
        declaration->as.enum_decl.methods = methods;
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
        size_t alias_token = AST_TOKEN_NONE;
        if (check(parser, TOKEN_IDENTIFIER)) alias_token = parser->current++;
        size_t path_first = path_start;
        size_t path_token = AST_TOKEN_NONE;
        if (check(parser, TOKEN_STRING_LITERAL)) {
            path_token = parser->current++;
        } else {
            parser_failure(parser, ERR_PACKAGE_ALIAS,
                           "Expected quoted package path and an optional identifier alias; dot imports and file imports are unsupported");
        }
        if (entry != NULL) {
            entry->path_token = path_token;
            entry->alias_token = alias_token;
            entry->alias = alias_token == AST_TOKEN_NONE ? NULL : ast_program_lexeme(parser->program, alias_token);
            entry->path_first_token = path_first;
            entry->path_token_count = parser->current - path_first;
            entry->span = range_span(parser, path_start, parser->current);
            entry->resolved_symbol_id = AST_SYMBOL_NONE;
            *tail = entry;
            tail = &entry->next;
        }
    } while (grouped && !parser->failed);
    if (grouped) (void) consume(parser, TOKEN_RPAREN);
    (void) consume(parser, TOKEN_SEMICOLON);
    if (head == NULL && !parser->failed)
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                       "Import group must contain at least one path");
    if (declaration != NULL) {
        declaration->as.import_decl.paths = head;
        finish_declaration(parser, declaration);
    }
    return declaration;
}

static AstDeclarationNode *parse_interface(SyntaxParser *parser) {
    size_t first = parser->current++;
    AstDeclarationNode *d = new_declaration(parser, AST_DECL_INTERFACE, first);
    size_t interface = consume(parser, TOKEN_IDENTIFIER);
    AstLifetimeParameter *lifetimes = NULL;
    AstGenericParameter *generics = parse_generic_parameters(parser, &lifetimes);
    if (d != NULL) d->generic_parameters = generics;
    (void) consume(parser, TOKEN_LBRACE);
    AstDeclarationNode *head = NULL, **tail = &head;
    int saved = parser->interface_signature;
    parser->interface_signature = 1;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        int is_public = match(parser, TOKEN_KEYWORD_PUB);
        int is_static = match(parser, TOKEN_KEYWORD_STATIC);
        AstDeclarationNode *method = parse_function(parser, is_static, interface);
        if (method) method->is_public = is_public;
        *tail = method;
        if (method) tail = &method->next;
    }
    parser->interface_signature = saved;
    (void) consume(parser, TOKEN_RBRACE);
    if (d) {
        d->name_token = interface;
        d->as.interface_decl.methods = head;
    }
    finish_declaration(parser, d);
    return d;
}

static AstDeclarationNode *parse_constant(SyntaxParser *parser) {
    size_t first = parser->current;
    int variable = match(parser, TOKEN_KEYWORD_VAR);
    if (!variable) (void) consume(parser, TOKEN_KEYWORD_CONST);
    AstDeclarationNode *declaration = new_declaration(parser, variable ? AST_DECL_VARIABLE : AST_DECL_CONSTANT, first);
    size_t name = consume(parser, TOKEN_IDENTIFIER);
    AstType type = inferred_type();
    if (match(parser, TOKEN_COLON)) type = parse_type(parser);
    AstExpression *value = NULL;
    if (!variable || check(parser, TOKEN_EQUAL)) {
        (void) consume(parser, TOKEN_EQUAL);
        value = parse_expression(parser);
    }
    (void) consume(parser, TOKEN_SEMICOLON);
    if (declaration != NULL) {
        declaration->name_token = name;
        declaration->as.constant.type = type;
        declaration->as.constant.value = value;
    }
    finish_declaration(parser, declaration);
    return declaration;
}

static AstDeclarationNode *parse_extern(SyntaxParser *parser) {
    (void) consume(parser, TOKEN_KEYWORD_EXTERN);
    size_t abi = consume(parser, TOKEN_STRING_LITERAL);
    if (strcmp(ast_program_lexeme(parser->program, abi), "system"))
        parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Only extern system ABI is supported");
    size_t library = AST_TOKEN_NONE;
    if (match(parser, TOKEN_KEYWORD_FROM)) {
        library = consume(parser, TOKEN_STRING_LITERAL);
        const unsigned char *id = (const unsigned char *)ast_program_lexeme(parser->program, library);
        int valid = (*id >= 'A' && *id <= 'Z') || (*id >= 'a' && *id <= 'z') || *id == '_';
        for (size_t i = 1; valid && id[i]; i++)
            valid = (id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= 'a' && id[i] <= 'z') ||
                    (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-';
        if (!valid)
            parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                           "from requires a logical library ID, not a filename, path or linker option");
    }
    (void) consume(parser, TOKEN_LBRACE);
    AstDeclarationNode *head = NULL;
    AstDeclarationNode **tail = &head;
    parser->native_declaration = 1;
    while (!parser->failed && !check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        int is_public = match(parser, TOKEN_KEYWORD_PUB);
        AstDeclarationNode *declaration = NULL;
        size_t pack = 0, alignment = 0;
        while (check(parser, TOKEN_IDENTIFIER) &&
               (!strcmp(ast_program_lexeme(parser->program, parser->current), "pack") ||
                !strcmp(ast_program_lexeme(parser->program, parser->current), "align"))) {
            int packing = !strcmp(ast_program_lexeme(parser->program, parser->current++), "pack");
            (void) consume(parser, TOKEN_LPAREN);
            size_t number = consume(parser, TOKEN_NUMBER);
            const char *text = ast_program_lexeme(parser->program, number);
            char *end = NULL;
            unsigned long long value = strtoull(text, &end, 10);
            if (!end || *end || !value || value > 16 || (value & (value - 1)))
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Native pack/align requires 1, 2, 4, 8 or 16");
            if ((packing && pack) || (!packing && alignment))
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Duplicate native layout modifier");
            if (packing) pack = (size_t)value; else alignment = (size_t)value;
            (void) consume(parser, TOKEN_RPAREN);
        }
        if (check(parser, TOKEN_KEYWORD_FUNC)) {
            if (pack || alignment) parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Layout modifiers require native struct or union");
            if (library == AST_TOKEN_NONE)
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Native imports require from library");
            else declaration = parse_function(parser, 0, AST_TOKEN_NONE);
        } else if (check(parser, TOKEN_KEYWORD_STRUCT) || check(parser, TOKEN_KEYWORD_UNION)) declaration = parse_struct(parser);
        else parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                            "Extern blocks permit only functions and native structs");
        if (declaration != NULL) {
            declaration->is_public = is_public;
            declaration->is_native = 1;
            declaration->native_pack = pack;
            declaration->native_alignment = alignment;
            if (declaration->is_opaque && (pack || alignment || declaration->is_native_union))
                parser_failure(parser, ERR_PARSE_INVALID_DECLARATION, "Opaque native types require an unmodified struct declaration");
            declaration->native_abi_token = abi;
            declaration->native_library_token = declaration->kind == AST_DECL_FUNCTION ? library : AST_TOKEN_NONE;
            *tail = declaration;
            tail = &declaration->next;
        }
    }
    parser->native_declaration = 0;
    (void) consume(parser, TOKEN_RBRACE);
    return head;
}

static int spelling(SyntaxParser *parser, const char *text) {
    return !strcmp(ast_program_lexeme(parser->program, parser->current), text);
}

static AstInterfaceBound *parse_auto_bounds(SyntaxParser *parser) {
    AstInterfaceBound *head = NULL, **tail = &head;
    do {
        AstInterfaceBound *bound = allocate(parser, sizeof(*bound));
        AstType type = parse_type(parser);
        if (bound) {
            bound->type = type;
            bound->name_token = type.name_token;
            *tail = bound;
            tail = &bound->next;
        }
    } while (match(parser, TOKEN_PLUS));
    return head;
}

static AstAutoRule *parse_attributes(SyntaxParser *parser, int *no_default) {
    AstAutoRule *head = NULL, **tail = &head;
    while (match(parser, TOKEN_AT)) {
        (void)consume(parser, TOKEN_LBRACKET);
        if (spelling(parser, "no_default")) {
            if (*no_default) parser_failure(parser, ERR_PARSE_INVALID_DECLARATION,
                                            "Duplicate no_default attribute");
            *no_default = 1;
            parser->current++;
        } else {
            AstAutoRule *rule = allocate(parser, sizeof(*rule));
            AstInterfaceBound *interfaces = parse_auto_bounds(parser);
            AstAutoCondition *conditions = NULL, **condition_tail = &conditions;
            if (spelling(parser, "where")) {
                parser->current++;
                do {
                    AstAutoCondition *condition = allocate(parser, sizeof(*condition));
                    AstType type = parse_type(parser);
                    (void)consume(parser, TOKEN_COLON);
                    AstInterfaceBound *bounds = parse_auto_bounds(parser);
                    if (condition) {
                        condition->type = type;
                        condition->bounds = bounds;
                        *condition_tail = condition;
                        condition_tail = &condition->next;
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            if (rule) {
                rule->interfaces = interfaces;
                rule->conditions = conditions;
                *tail = rule;
                tail = &rule->next;
            }
        }
        (void)consume(parser, TOKEN_RBRACKET);
    }
    return head;
}

int frontend_build_structured_ast_recover(AstProgram *program, int recover_syntax) {
    if (program == NULL) return 0;
    program->structured_error_token = AST_TOKEN_NONE;
    SyntaxParser parser = {.program = program, .recover_syntax = recover_syntax};
    program->package_token = AST_TOKEN_NONE;
    if (!check(&parser, TOKEN_KEYWORD_PACKAGE))
        parser_failure(&parser, ERR_PACKAGE_DECLARATION, "Every DMM source file must begin with 'package name;'");
    else {
        parser.current++;
        program->package_token = consume(&parser, TOKEN_IDENTIFIER);
        program->package_name = ast_program_lexeme(program, program->package_token);
        if (!strcmp(program->package_name, "_"))
            parser_failure(&parser, ERR_PACKAGE_NAME, "Package name cannot be '_'");
        (void) consume(&parser, TOKEN_SEMICOLON);
    }
    AstDeclarationNode **tail = &program->root;
    while (!parser.failed && !check(&parser, TOKEN_EOF)) {
        size_t first = parser.current;
        if (recover_syntax) parser.reported = 0;
        AstDeclarationNode *declaration = NULL;
        int no_default = 0;
        AstAutoRule *rules = parse_attributes(&parser, &no_default);
        int is_public = match(&parser, TOKEN_KEYWORD_PUB);
        if (check(&parser, TOKEN_KEYWORD_EXTERN)) {
            if (is_public) parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                                         "Place pub on declarations inside the extern block");
            declaration = parse_extern(&parser);
        }
        else if (check(&parser, TOKEN_KEYWORD_IMPORT)) declaration = parse_import(&parser);
        else if (check(&parser, TOKEN_KEYWORD_CONST)) declaration = parse_constant(&parser);
        else if (check(&parser, TOKEN_KEYWORD_VAR)) declaration = parse_constant(&parser);
        else if (check(&parser, TOKEN_KEYWORD_FUNC) || check(&parser, TOKEN_KEYWORD_ASYNC))
            declaration = parse_function(&parser, 0, AST_TOKEN_NONE);
        else if (match(&parser, TOKEN_KEYWORD_EXPORT)) {
            size_t abi = consume(&parser, TOKEN_STRING_LITERAL);
            if (strcmp(ast_program_lexeme(program, abi), "system"))
                parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION, "Only export system ABI is supported");
            declaration = parse_function(&parser, 0, AST_TOKEN_NONE);
            if (declaration) {
                declaration->is_native_export = 1;
                declaration->native_abi_token = abi;
            }
        }
        else if (check(&parser, TOKEN_KEYWORD_STRUCT)) declaration = parse_struct(&parser);
        else if (check(&parser, TOKEN_KEYWORD_ENUM)) declaration = parse_enum(&parser);
        else if (check(&parser, TOKEN_KEYWORD_INTERFACE)) declaration = parse_interface(&parser);
        else if (spelling(&parser, "auto")) {
            parser.current++;
            if (!check(&parser, TOKEN_KEYWORD_INTERFACE))
                parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION, "auto requires an interface declaration");
            else {
                declaration = parse_interface(&parser);
                if (declaration) {
                    declaration->is_auto_interface = 1;
                    if (declaration->generic_parameters || declaration->as.interface_decl.methods)
                        parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                                       "Auto interfaces cannot have members or type parameters");
                }
            }
        }
        else if (spelling(&parser, "type")) {
            parser.current++;
            declaration = new_declaration(&parser, AST_DECL_TYPE_RULE, first);
            AstType target = parse_type(&parser);
            (void)consume(&parser, TOKEN_SEMICOLON);
            if (declaration) {
                declaration->rule_target = target;
                declaration->name_token = target.name_token;
                if (!rules || no_default)
                    parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                                   "External type rules require auto attributes and a concrete target");
                finish_declaration(&parser, declaration);
            }
        }
        else if (check(&parser, TOKEN_KEYWORD_PACKAGE))
            parser_failure(&parser, ERR_PACKAGE_DECLARATION,
                           "Package declaration must appear exactly once, before all declarations");
        else if (check(&parser, TOKEN_HASH))
            parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                           "#import was removed; use import path");
        else
            parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                           "Expected function declaration");
        if (declaration && (rules || no_default)) {
            if (declaration->kind != AST_DECL_STRUCT && declaration->kind != AST_DECL_ENUM &&
                declaration->kind != AST_DECL_TYPE_RULE)
                parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                               "Type attributes require a struct, enum or external type rule");
            declaration->no_default = no_default;
            declaration->auto_rules = rules;
        }
        if (parser.failed && recover_syntax && !parser.allocation_failed) {
            int braces = 0;
            for (size_t i = first; i < parser.current && i < program->token_count; i++) {
                if (program->tokens[i].type == TOKEN_LBRACE) braces++;
                if (program->tokens[i].type == TOKEN_RBRACE) braces--;
            }
            while (!check(&parser, TOKEN_EOF)) {
                TokenType token = current_type(&parser);
                if (braces <= 0 && parser.current > first &&
                    (token == TOKEN_KEYWORD_EXTERN || token == TOKEN_KEYWORD_EXPORT || token == TOKEN_KEYWORD_FUNC || token == TOKEN_KEYWORD_ASYNC ||
                     token == TOKEN_KEYWORD_STRUCT ||
                     token == TOKEN_KEYWORD_ENUM || token == TOKEN_KEYWORD_INTERFACE ||
                     token == TOKEN_KEYWORD_IMPORT || token == TOKEN_KEYWORD_CONST))
                    break;
                if (token == TOKEN_LBRACE) braces++;
                if (token == TOKEN_RBRACE) braces--;
                parser.current++;
            }
            declaration = NULL;
            parser.failed = 0;
        }
        while (declaration != NULL) {
            if (is_public && declaration->kind == AST_DECL_IMPORT && program->module &&
                strcmp(program->module->edition, "2026-10-04-dev"))
                parser_failure(&parser, ERR_PARSE_INVALID_DECLARATION,
                               "Public package reexports require DMM 2026-10-04-dev");
            if (!declaration->is_native) declaration->is_public = is_public;
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
