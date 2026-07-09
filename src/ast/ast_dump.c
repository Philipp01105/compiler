#include "ast.h"

#include <stdio.h>

static int indent(FILE *output, unsigned depth) {
    for (unsigned i = 0; i < depth; i++)
        if (fputs("  ", output) == EOF) return 0;
    return 1;
}

static int quoted(FILE *output, const char *text) {
    if (fputc('"', output) == EOF) return 0;
    for (const unsigned char *p = (const unsigned char *) (text == NULL ? "" : text); *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (fputc('\\', output) == EOF || fputc(*p, output) == EOF) return 0;
        } else if (*p == '\n') {
            if (fputs("\\n", output) == EOF) return 0;
        } else if (*p == '\r') {
            if (fputs("\\r", output) == EOF) return 0;
        } else if (*p == '\t') {
            if (fputs("\\t", output) == EOF) return 0;
        } else if (*p < 0x20) {
            if (fprintf(output, "\\x%02x", *p) < 0) return 0;
        } else if (fputc(*p, output) == EOF) return 0;
    }
    return fputc('"', output) != EOF;
}

static int span(FILE *output, AstSourceSpan value) {
    return fprintf(output, "%d:%d-%d:%d", value.begin.line, value.begin.column,
                   value.end.line, value.end.column) >= 0;
}

static const char *data_type_name(DataType type) {
    static const char *names[] = {DMM_TYPE_NAMES};
    return type >= TYPE_INT && type <= TYPE_UNKNOWN ? names[type] : "invalid";
}

static const char *expression_name(AstExpressionKind kind) {
    static const char *names[] = {
        "error", "literal", "name", "unary", "binary", "call",
        "index", "member", "slice-length", "reserve", "cast", "free", "enum-construct", "enum-access",
        "sizeof", "alignof", "slice", "slice-data", "type-info", "type-property"
    };
    return kind >= AST_EXPR_ERROR && kind <= AST_EXPR_TYPE_PROPERTY ? names[kind] : "invalid";
}

static const char *statement_name(AstStatementKind kind) {
    static const char *names[] = {
        "error", "block", "variable", "expression", "assignment",
        "if", "while", "for", "return", "break", "continue", "match", "defer"
    };
    return kind >= AST_STMT_ERROR && kind <= AST_STMT_DEFER ? names[kind] : "invalid";
}

static const char *operator_name(TokenType type) {
    switch (type) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_EQUAL: return "=";
        case TOKEN_EQUAL_EQUAL: return "==";
        case TOKEN_BANG_EQUAL: return "!=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_AMP_AMP: return "&&";
        case TOKEN_PIPE_PIPE: return "||";
        case TOKEN_BANG: return "!";
        case TOKEN_AMPERSAND: return "&";
        case TOKEN_PLUS_EQUAL: return "+=";
        case TOKEN_MINUS_EQUAL: return "-=";
        case TOKEN_STAR_EQUAL: return "*=";
        case TOKEN_SLASH_EQUAL: return "/=";
        case TOKEN_PLUS_PLUS: return "++";
        case TOKEN_MINUS_MINUS: return "--";
        default: return "-";
    }
}

static int token_field(FILE *output, const AstProgram *program, const char *key, size_t token) {
    if (token == AST_TOKEN_NONE) return fprintf(output, " %s=-", key) >= 0;
    if (fprintf(output, " %s=", key) < 0) return 0;
    return quoted(output, ast_program_lexeme(program, token));
}

static int symbol_field(FILE *output, size_t symbol) {
    return symbol == AST_SYMBOL_NONE
               ? fputs(" symbol=-", output) != EOF
               : fprintf(output, " symbol=%zu", symbol) >= 0;
}

static int dump_type(FILE *output, const AstProgram *program, const AstType *type) {
    if (type->kind == AST_TYPE_INFERRED) return fputs("inferred", output) != EOF;
    if (type->borrow_kind == AST_BORROW_IMMUTABLE &&
        fputc('&', output) == EOF)
        return 0;
    if (type->borrow_kind == AST_BORROW_MUTABLE &&
        fputs("&mut ", output) == EOF)
        return 0;
    for (unsigned i = 0; i < type->outer_pointer_depth; i++)
        if (fputc('*', output) == EOF) return 0;
    if (type->outer_pointer_depth != 0 && (type->is_array || type->is_slice) &&
        fputc('(', output) == EOF)
        return 0;
    for (unsigned i = 0; i < type->pointer_depth; i++)
        if (fputc('*', output) == EOF) return 0;
    if (fputs(ast_program_lexeme(program, type->name_token), output) == EOF) return 0;
    if (type->arguments) {
        if (fputc('<', output) == EOF) return 0;
        for (const AstTypeArgument *argument = type->arguments; argument; argument = argument->next) {
            if (!dump_type(output, program, &argument->type)) return 0;
            if (argument->next && fputc(',', output) == EOF) return 0;
        }
        if (fputc('>', output) == EOF) return 0;
    }
    if (type->is_array && fprintf(output, "[%s]",
                                  ast_program_lexeme(program, type->array_length_token)) < 0)
        return 0;
    if (type->is_slice && fputs("[]", output) == EOF) return 0;
    if (type->outer_pointer_depth != 0 && (type->is_array || type->is_slice) &&
        fputc(')', output) == EOF)
        return 0;
    return 1;
}

static int dump_expression(FILE *, const AstProgram *, const AstExpression *, unsigned,
                           const char *);

static int dump_expression_list(FILE *output, const AstProgram *program,
                                const AstExpression *expression, unsigned depth,
                                const char *role) {
    for (; expression != NULL; expression = expression->next)
        if (!dump_expression(output, program, expression, depth, role)) return 0;
    return 1;
}

static int dump_expression(FILE *output, const AstProgram *program,
                           const AstExpression *expression, unsigned depth,
                           const char *role) {
    if (expression == NULL) return 1;
    if (!indent(output, depth) || fprintf(output, "expression %s %s span=",
                                          role, expression_name(expression->kind)) < 0 ||
        !span(output, expression->span) ||
        fprintf(output, " type=%s pointers=%u outer-pointers=%u array=%d slice=%d",
                data_type_name(expression->resolved_type), expression->resolved_pointer_depth,
                expression->resolved_outer_pointer_depth, expression->resolved_is_array,
                expression->resolved_is_slice) < 0 ||
        !symbol_field(output, expression->resolved_symbol_id))
        return 0;
    if (expression->value_token != AST_TOKEN_NONE &&
        !token_field(output, program, "token", expression->value_token))
        return 0;
    if ((expression->kind == AST_EXPR_UNARY || expression->kind == AST_EXPR_BINARY) &&
        (fputs(" operator=", output) == EOF ||
         !quoted(output, operator_name(expression->operator_type))))
        return 0;
    if (expression->kind == AST_EXPR_UNARY &&
        expression->operator_type == TOKEN_AMPERSAND &&
        fprintf(output, " mutable=%d", expression->mutable_borrow) < 0)
        return 0;
    if (expression->folded_constant.lexeme != NULL &&
        (fputs(" folded=", output) == EOF || !quoted(output, expression->folded_constant.lexeme)))
        return 0;
    if (expression->kind == AST_EXPR_CAST || expression->kind == AST_EXPR_RESERVE || expression->kind ==
        AST_EXPR_TYPE_INFO) {
        if (fputs(" operand-type=", output) == EOF ||
            !dump_type(output, program, &expression->allocated_type))
            return 0;
    }
    if (fputc('\n', output) == EOF) return 0;
    return dump_expression(output, program, expression->left, depth + 1, "left") &&
           dump_expression(output, program, expression->right, depth + 1, "right") &&
           dump_expression_list(output, program, expression->arguments, depth + 1, "argument");
}

static int dump_statement(FILE *, const AstProgram *, const AstStatement *, unsigned,
                          const char *);

static int dump_statement_list(FILE *output, const AstProgram *program,
                               const AstStatement *statement, unsigned depth,
                               const char *role) {
    for (; statement != NULL; statement = statement->next)
        if (!dump_statement(output, program, statement, depth, role)) return 0;
    return 1;
}

static int dump_statement(FILE *output, const AstProgram *program,
                          const AstStatement *statement, unsigned depth,
                          const char *role) {
    if (statement == NULL) return 1;
    if (!indent(output, depth) || fprintf(output, "statement %s %s span=", role,
                                          statement_name(statement->kind)) < 0 ||
        !span(output, statement->span) || !symbol_field(output, statement->resolved_symbol_id))
        return 0;
    if (statement->name_token != AST_TOKEN_NONE &&
        !token_field(output, program, "name", statement->name_token))
        return 0;
    if (statement->kind == AST_STMT_VARIABLE) {
        if (fputs(" type=", output) == EOF || !dump_type(output, program, &statement->type)) return 0;
        if (fprintf(output, " const=%d", statement->is_const) < 0) return 0;
    }
    if (statement->kind == AST_STMT_ASSIGNMENT &&
        (fputs(" operator=", output) == EOF ||
         !quoted(output, operator_name(statement->assignment_operator))))
        return 0;
    if (fputc('\n', output) == EOF) return 0;
    for (const AstMatchArm *a = statement->match_arms; a; a = a->next) {
        if (!indent(output, depth + 1) || fputs("pattern=", output) == EOF ||
            !quoted(output, ast_program_lexeme(program, a->variant_token)))
            return 0;
        if (a->is_type_pattern && (fputs(" type=", output) == EOF || !dump_type(output, program, &a->type))) return 0;
        if (statement->selected_type_arm == a && fputs(" selected", output) == EOF) return 0;
        if (fputc('\n', output) == EOF) return 0;
        for (const AstParameter *binding = a->bindings; binding; binding = binding->next)
            if (!indent(output, depth + 2) || fputs("binding=", output) == EOF ||
                !quoted(output, ast_program_lexeme(program, binding->name_token)) ||
                fputs(" type=", output) == EOF || !dump_type(output, program, &binding->type) ||
                !symbol_field(output, binding->resolved_symbol_id) || fputs(" span=", output) == EOF ||
                !span(output, ast_program_token(program, binding->name_token)->span) || fputc('\n', output) == EOF)
                return 0;
        if (!dump_statement(output, program, a->body, depth + 2, "arm")) return 0;
    }
    return dump_statement(output, program, statement->initializer, depth + 1, "initializer") &&
           dump_expression(output, program, statement->expression, depth + 1, "expression") &&
           dump_expression(output, program, statement->value, depth + 1, "value") &&
           dump_expression(output, program, statement->condition, depth + 1, "condition") &&
           dump_expression(output, program, statement->update, depth + 1, "update") &&
           dump_statement_list(output, program, statement->body, depth + 1, "body") &&
           dump_statement_list(output, program, statement->else_body, depth + 1, "else");
}

static int dump_function(FILE *output, const AstProgram *program,
                         const AstDeclarationNode *declaration, unsigned depth) {
    for (const AstParameter *parameter = declaration->as.function.parameters;
         parameter != NULL; parameter = parameter->next) {
        if (!indent(output, depth) || fputs("parameter name=", output) == EOF ||
            !quoted(output, ast_program_lexeme(program, parameter->name_token)) ||
            fputs(" type=", output) == EOF || !dump_type(output, program, &parameter->type) ||
            !symbol_field(output, parameter->resolved_symbol_id) || fputc('\n', output) == EOF)
            return 0;
    }
    if (!indent(output, depth) || fputs("return type=", output) == EOF ||
        !dump_type(output, program, &declaration->as.function.return_type) || fputc('\n', output) == EOF)
        return 0;
    return dump_statement(output, program, declaration->as.function.body, depth, "body");
}

static int dump_declaration(FILE *output, const AstProgram *program,
                            const AstDeclarationNode *declaration, unsigned depth) {
    if (!indent(output, depth) || fprintf(output, "declaration %s span=",
                                          ast_declaration_kind_name(declaration->kind)) < 0 || !span(
            output, declaration->span) ||
        !symbol_field(output, declaration->resolved_symbol_id))
        return 0;
    if (declaration->name_token != AST_TOKEN_NONE &&
        !token_field(output, program, "name", declaration->name_token))
        return 0;
    if (declaration->kind == AST_DECL_FUNCTION &&
        fprintf(output, " static=%d", declaration->as.function.is_static) < 0)
        return 0;
    if (declaration->kind == AST_DECL_STRUCT &&
        fprintf(output, " resource=%d", declaration->as.struct_decl.is_resource) < 0)
        return 0;
    if (declaration->kind == AST_DECL_IMPORT) {
        if (fputs(" paths=[", output) == EOF) return 0;
        for (const AstImportPath *path = declaration->as.import_decl.paths; path != NULL; path = path->next) {
            if (path != declaration->as.import_decl.paths && fputs(",", output) == EOF) return 0;
            if (path->path_token != AST_TOKEN_NONE) {
                if (!quoted(output, ast_program_lexeme(program,
                                                       path->path_token)))
                    return 0;
            } else {
                if (fputc('"', output) == EOF) return 0;
                size_t first = path->path_first_token;
                for (size_t i = 0; i < path->path_token_count &&
                                   first + i < program->token_count; i++)
                    if (fputs(program->tokens[first + i].lexeme, output) == EOF) return 0;
                if (fputc('"', output) == EOF) return 0;
            }
        }
        if (fputs("]", output) == EOF) return 0;
    }
    if (declaration->generic_parameters) {
        if (fputs(" generics=[", output) == EOF) return 0;
        for (const AstGenericParameter *g = declaration->generic_parameters; g; g = g->next) {
            if (g != declaration->generic_parameters && fputc(',', output) == EOF) return 0;
            if (!quoted(output, ast_program_lexeme(program, g->name_token))) return 0;
            for (const AstInterfaceBound *b = g->bounds; b; b = b->next)
                if (fputc(':', output) == EOF || !quoted(output, ast_program_lexeme(program, b->name_token))) return 0;
        }
        if (fputc(']', output) == EOF) return 0;
    }
    if (declaration->specialization_identity &&
        (fputs(" specialization=", output) == EOF || !quoted(output, declaration->specialization_identity)))
        return 0;
    if (fprintf(output, " visibility=%s", declaration->is_public ? "public" : "private") < 0) return 0;
    if (fputc('\n', output) == EOF) return 0;
    if (declaration->generic_origin) {
        if (!indent(output, depth + 1) || fputs("generic-origin name=", output) == EOF ||
            !quoted(output, ast_program_lexeme(program, declaration->generic_origin->name_token)) || fputc('\n', output)
            == EOF)
            return 0;
        for (const AstTypeArgument *argument = declaration->specialization_arguments; argument;
             argument = argument->next)
            if (!indent(output, depth + 1) || fputs("type-argument type=", output) == EOF ||
                !dump_type(output, program, &argument->type) || fputc('\n', output) == EOF)
                return 0;
    }
    if (declaration->kind == AST_DECL_FUNCTION)
        return dump_function(output, program, declaration, depth + 1);
    if (declaration->kind == AST_DECL_CONSTANT || declaration->kind == AST_DECL_VARIABLE) {
        if (!indent(output, depth + 1) || fputs("type=", output) == EOF ||
            !dump_type(output, program, &declaration->as.constant.type) ||
            fputc('\n', output) == EOF)
            return 0;
        return dump_expression(output, program, declaration->as.constant.value,
                               depth + 1, "value");
    }
    if (declaration->kind == AST_DECL_INTERFACE) {
        const AstDeclarationNode *method = declaration->as.interface_decl.methods;
        for (; method; method = method->next)
            if (!dump_declaration(output, program, method, depth + 1)) return 0;
        return 1;
    }
    const AstField *fields = declaration->kind == AST_DECL_STRUCT
                                 ? declaration->as.struct_decl.fields
                                 : declaration->kind == AST_DECL_ENUM
                                       ? declaration->as.enum_decl.fields
                                       : NULL;
    for (const AstField *field = fields; field != NULL; field = field->next) {
        if (!indent(output, depth + 1) || fprintf(output, "member-visibility=%s\n",
                                                  field->is_public ? "public" : "private") < 0) return 0;
        if (!indent(output, depth + 1) || fputs("field name=", output) == EOF ||
            !quoted(output, ast_program_lexeme(program, field->name_token)) ||
            fputs(" type=", output) == EOF || !dump_type(output, program, &field->type) ||
            !symbol_field(output, field->resolved_symbol_id) || fputc('\n', output) == EOF)
            return 0;
    }
    if (declaration->kind == AST_DECL_STRUCT) {
        if (declaration->as.struct_decl.destructor != NULL &&
            !dump_statement(output, program, declaration->as.struct_decl.destructor,
                            depth + 1, "destructor"))
            return 0;
        for (const AstDeclarationNode *method = declaration->as.struct_decl.methods;
             method != NULL; method = method->next)
            if (!dump_declaration(output, program, method, depth + 1)) return 0;
    }
    if (declaration->kind == AST_DECL_ENUM)
        for (const AstEnumValue *value = declaration->as.enum_decl.values;
             value != NULL; value = value->next) {
            if (!indent(output, depth + 1) || fputs("variant name=", output) == EOF ||
                !quoted(output, ast_program_lexeme(program, value->name_token)) ||
                !symbol_field(output, value->resolved_symbol_id) || fputs(" span=", output) == EOF ||
                !span(output, value->span) || fputc('\n', output) == EOF ||
                !dump_expression_list(output, program, value->arguments, depth + 2, "argument"))
                return 0;
            for (const AstTypeArgument *p = value->payload_types; p; p = p->next)
                if (!indent(output, depth + 2) || fputs("payload=", output) == EOF ||
                    !dump_type(output, program, &p->type) || fputc('\n', output) == EOF)
                    return 0;
        }
    return 1;
}

static int dump_program(FILE *output, const AstProgram *program, size_t index,
                        const char *role) {
    if (fprintf(output, "program #%zu role=%s path=", index, role) < 0 ||
        !quoted(output, program->source_path) ||
        fprintf(output, " declarations=%zu tokens=%zu\n", program->structured_declaration_count,
                program->token_count) < 0)
        return 0;
    if (fputs("  package name=", output) == EOF || !quoted(output, program->package_name ? program->package_name : "")
        ||
        fputs(" identity=", output) == EOF || !quoted(output, program->module_identity ? program->module_identity : "")
        || fputc('\n', output) == EOF)
        return 0;
    for (const AstDeclarationNode *declaration = program->root;
         declaration != NULL; declaration = declaration->next)
        if (!dump_declaration(output, program, declaration, 1)) return 0;
    return 1;
}

int ast_dump(FILE *output, const AstProgram *program) {
    if (output == NULL || program == NULL || !ast_validate_program(program)) return 0;
    if (fprintf(output, "dmm-ast-v3\nmodule units=%zu\n", program->owned_import_count + 1) < 0 ||
        !dump_program(output, program, 0, "root"))
        return 0;
    for (size_t i = 0; i < program->owned_import_count; i++)
        if (!dump_program(output, program->owned_imports[i], i + 1, "import")) return 0;
    return !ferror(output);
}
