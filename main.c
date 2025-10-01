#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#define MAX_VARS 200
#define MAX_LINE 512
#define MAX_TOKEN 128
#define MAX_FUNCTIONS 100

// Globale Debug-Flag
int DEBUG_MODE = 0;

#define DEBUG_PRINT(...) if(DEBUG_MODE) fprintf(stderr, __VA_ARGS__)

typedef enum {
    TYPE_INT,
    TYPE_STRING,
    TYPE_VOID,
    TYPE_UNKNOWN
} DataType;

typedef struct {
    char name[MAX_TOKEN];
    int offset;
    int scope;
    DataType type;
} Variable;

typedef struct {
    int id;
    char text[MAX_LINE];
} StringLiteral;

typedef struct {
    char name[MAX_TOKEN];
    int param_count;
    char params[10][MAX_TOKEN];
    DataType param_types[10];
    DataType return_type;
} Function;

Variable vars[MAX_VARS];
int var_count = 0;
int stack_offset = 0;

StringLiteral string_literals[1000];
int string_literal_count = 0;
int string_count = 0;

Function functions[MAX_FUNCTIONS];
int function_count = 0;
int current_function_scope = 0;

char code_buffer[500000];
int code_pos = 0;

char function_code_buffer[500000];
int function_code_pos = 0;

int label_counter = 0;
int current_line_number = 0;
int main_function_found = 0;

void code_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    if (current_function_scope > 0) {
        function_code_pos += vsnprintf(function_code_buffer + function_code_pos,
                                       sizeof(function_code_buffer) - function_code_pos, format, args);
    } else {
        code_pos += vsnprintf(code_buffer + code_pos, sizeof(code_buffer) - code_pos, format, args);
    }

    va_end(args);
}

void trim(char *str) {
    char *start = str;
    char *end;

    while (isspace((unsigned char) *str)) str++;

    if (*str == 0) {
        *start = '\0';
        return;
    }

    end = str + strlen(str) - 1;
    while (end > str && (isspace((unsigned char) *end) || *end == '\r')) end--;
    end[1] = '\0';

    memmove(start, str, strlen(str) + 1);
}

DataType string_to_type(const char *str) {
    if (strcmp(str, "int") == 0) return TYPE_INT;
    if (strcmp(str, "string") == 0) return TYPE_STRING;
    if (strcmp(str, "void") == 0) return TYPE_VOID;
    return TYPE_UNKNOWN;
}

const char *type_to_string(DataType type) {
    switch (type) {
        case TYPE_INT: return "int";
        case TYPE_STRING: return "string";
        case TYPE_VOID: return "void";
        default: return "unknown";
    }
}

int get_var_offset(const char *name) {
    DEBUG_PRINT("DEBUG get_var_offset: Suche '%s', current_scope=%d, var_count=%d\n",
                name, current_function_scope, var_count);

    for (int i = var_count - 1; i >= 0; i--) {
        DEBUG_PRINT("  [%d] name='%s', scope=%d, offset=%d\n",
                    i, vars[i].name, vars[i].scope, vars[i].offset);
        if (strcmp(vars[i].name, name) == 0) {
            if (vars[i].scope <= current_function_scope) {
                DEBUG_PRINT("  -> GEFUNDEN!\n");
                return vars[i].offset;
            } else {
                DEBUG_PRINT("  -> Name passt, aber scope %d > current_scope %d\n",
                           vars[i].scope, current_function_scope);
            }
        }
    }

    fprintf(stderr, "Fehler: Variable '%s' nicht deklariert!\n", name);
    exit(1);
}

DataType get_var_type(const char *name) {
    DEBUG_PRINT("DEBUG get_var_type: Suche Typ von '%s'\n", name);

    for (int i = var_count - 1; i >= 0; i--) {
        if (strcmp(vars[i].name, name) == 0) {
            if (vars[i].scope <= current_function_scope) {
                DEBUG_PRINT("  -> Typ: %s\n", type_to_string(vars[i].type));
                return vars[i].type;
            }
        }
    }

    DEBUG_PRINT("  -> Typ UNKNOWN\n");
    return TYPE_UNKNOWN;
}

int create_variable(const char *name, DataType type) {
    DEBUG_PRINT("DEBUG create_variable: Erstelle '%s' mit Typ %s, Scope %d\n",
                name, type_to_string(type), current_function_scope);

    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0 && vars[i].scope == current_function_scope) {
            fprintf(stderr, "Fehler: Variable '%s' bereits deklariert!\n", name);
            exit(1);
        }
    }

    stack_offset += 4;
    strcpy(vars[var_count].name, name);
    vars[var_count].offset = stack_offset;
    vars[var_count].scope = current_function_scope;
    vars[var_count].type = type;

    DEBUG_PRINT("  -> Variable '%s' erstellt: offset=%d, var_count=%d->%d\n",
                name, stack_offset, var_count, var_count + 1);

    var_count++;
    return stack_offset;
}

void emit_push_var(const char *name) {
    int offset = get_var_offset(name);
    code_printf("    movl -%d(%%rbp), %%eax\n", offset);
    code_printf("    pushq %%rax\n");
}

DataType emit_expression(char *expr);
void emit_function_call(char *stmt);

DataType emit_term(char *term) {
    trim(term);

    int len = strlen(term);
    if (len > 0 && term[len - 1] == ';') {
        term[len - 1] = '\0';
        trim(term);
    }

    DEBUG_PRINT("DEBUG emit_term: '%s'\n", term);

    // Klammern behandeln
    if (term[0] == '(' && term[strlen(term) - 1] == ')') {
        term[strlen(term) - 1] = '\0';
        char *inner = term + 1;
        trim(inner);
        DEBUG_PRINT("  -> Klammern entfernt, evaluiere: '%s'\n", inner);
        return emit_expression(inner);
    }

    if (isdigit(term[0]) || (term[0] == '-' && isdigit(term[1]))) {
        code_printf("    pushq $%d\n", atoi(term));
        return TYPE_INT;
    }

    if (strchr(term, '(') && strchr(term, ')')) {
        DEBUG_PRINT("  -> Erkannt als Funktionsaufruf\n");
        emit_function_call(term);
        return TYPE_INT;
    }

    DataType var_type = get_var_type(term);
    emit_push_var(term);
    return var_type;
}

char *find_operator_outside_parens(char *expr, const char *op) {
    int depth = 0;
    int op_len = strlen(op);
    char *last_found = NULL;

    for (int i = 0; expr[i] != '\0'; i++) {
        if (expr[i] == '(') depth++;
        if (expr[i] == ')') depth--;

        if (depth == 0) {
            if (strncmp(&expr[i], op, op_len) == 0) {
                if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
                    return &expr[i];
                }

                if (strcmp(op, " + ") == 0 || strcmp(op, " - ") == 0) {
                    last_found = &expr[i];
                } else {
                    return &expr[i];
                }
            }
        }
    }

    return last_found;
}

DataType emit_expression(char *expr) {
    char temp[MAX_LINE];
    strcpy(temp, expr);
    trim(temp);

    DEBUG_PRINT("DEBUG emit_expression: '%s'\n", temp);

    int len = strlen(temp);
    if (len > 0 && temp[len - 1] == ';') {
        temp[len - 1] = '\0';
        trim(temp);
    }

    // Vergleichsoperatoren (niedrigste Priorität)
    char *le = find_operator_outside_parens(temp, " <= ");
    char *ge = find_operator_outside_parens(temp, " >= ");
    char *eq = find_operator_outside_parens(temp, " == ");
    char *ne = find_operator_outside_parens(temp, " != ");
    char *lt = find_operator_outside_parens(temp, " < ");
    char *gt = find_operator_outside_parens(temp, " > ");

    if (le) {
        *le = '\0';
        emit_expression(temp);
        emit_expression(le + 4);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    setle %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }
    if (ge) {
        *ge = '\0';
        emit_expression(temp);
        emit_expression(ge + 4);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    setge %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }
    if (eq) {
        *eq = '\0';
        emit_expression(temp);
        emit_expression(eq + 4);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    sete %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }
    if (ne) {
        *ne = '\0';
        emit_expression(temp);
        emit_expression(ne + 4);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    setne %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }
    if (lt) {
        *lt = '\0';
        emit_expression(temp);
        emit_expression(lt + 3);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    setl %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }
    if (gt) {
        *gt = '\0';
        emit_expression(temp);
        emit_expression(gt + 3);
        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cmpl %%ebx, %%eax\n");
        code_printf("    setg %%al\n");
        code_printf("    movzbl %%al, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }

    // Addition und Subtraktion
    char *plus = find_operator_outside_parens(temp, " + ");
    char *minus = find_operator_outside_parens(temp, " - ");

    if (plus) {
        *plus = '\0';
        DataType left_type = emit_expression(temp);
        DataType right_type = emit_expression(plus + 3);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            fprintf(stderr, "Fehler: Addition nur mit int möglich!\n");
            exit(1);
        }

        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    addl %%ebx, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    } else if (minus) {
        *minus = '\0';
        DataType left_type = emit_expression(temp);
        DataType right_type = emit_expression(minus + 3);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            fprintf(stderr, "Fehler: Subtraktion nur mit int möglich!\n");
            exit(1);
        }

        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    subl %%ebx, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    }

    // Multiplikation und Division
    char *mult = find_operator_outside_parens(temp, " * ");
    char *divd = find_operator_outside_parens(temp, " / ");

    if (mult) {
        *mult = '\0';
        DataType left_type = emit_expression(temp);
        DataType right_type = emit_expression(mult + 3);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            fprintf(stderr, "Fehler: Multiplikation nur mit int möglich!\n");
            exit(1);
        }

        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    imull %%ebx, %%eax\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    } else if (divd) {
        *divd = '\0';
        DataType left_type = emit_expression(temp);
        DataType right_type = emit_expression(divd + 3);

        if (left_type != TYPE_INT || right_type != TYPE_INT) {
            fprintf(stderr, "Fehler: Division nur mit int möglich!\n");
            exit(1);
        }

        code_printf("    popq %%rbx\n");
        code_printf("    popq %%rax\n");
        code_printf("    cltd\n");
        code_printf("    idivl %%ebx\n");
        code_printf("    pushq %%rax\n");
        return TYPE_INT;
    } else {
        return emit_term(temp);
    }
}

int add_string_literal(const char *text) {
    strcpy(string_literals[string_literal_count].text, text);
    string_literals[string_literal_count].id = string_count;
    string_literal_count++;
    return string_count++;
}

void emit_print(char *stmt) {
    char *open = strchr(stmt, '(');
    char *close = strrchr(stmt, ')');
    if (!open || !close) return;

    *close = '\0';
    char *content = open + 1;
    trim(content);

    DEBUG_PRINT("DEBUG emit_print: content='%s'\n", content);

    char parts[10][MAX_LINE];
    int part_count = 0;

    int in_string = 0;
    int paren_depth = 0;
    int start = 0;

    for (int i = 0; content[i] != '\0' && part_count < 10; i++) {
        if (content[i] == '"') {
            in_string = !in_string;
        }

        if (!in_string) {
            if (content[i] == '(') paren_depth++;
            if (content[i] == ')') paren_depth--;
        }

        if (content[i] == '+' && !in_string && paren_depth == 0) {
            int len = i - start;
            strncpy(parts[part_count], content + start, len);
            parts[part_count][len] = '\0';
            trim(parts[part_count]);

            DEBUG_PRINT("  Part %d: '%s'\n", part_count, parts[part_count]);
            part_count++;

            start = i + 1;
        }
    }

    if (start < strlen(content)) {
        strcpy(parts[part_count], content + start);
        trim(parts[part_count]);
        DEBUG_PRINT("  Part %d: '%s'\n", part_count, parts[part_count]);
        part_count++;
    }

    for (int i = 0; i < part_count; i++) {
        trim(parts[i]);

        if (parts[i][0] == '"') {
            char *end = strchr(parts[i] + 1, '"');
            if (end) {
                *end = '\0';
                int id = add_string_literal(parts[i] + 1);
                code_printf("    leaq .LC%d(%%rip), %%rcx\n", id);
                code_printf("    subq $32, %%rsp\n");
                code_printf("    call printf\n");
                code_printf("    addq $32, %%rsp\n");
            }
        } else {
            emit_expression(parts[i]);
            code_printf("    popq %%rax\n");
            code_printf("    movl %%eax, %%edx\n");
            int id = add_string_literal("%d");
            code_printf("    leaq .LC%d(%%rip), %%rcx\n", id);
            code_printf("    subq $32, %%rsp\n");
            code_printf("    call printf\n");
            code_printf("    addq $32, %%rsp\n");
        }
    }

    code_printf("    movl $10, %%ecx\n");
    code_printf("    subq $32, %%rsp\n");
    code_printf("    call putchar\n");
    code_printf("    addq $32, %%rsp\n");
}

void emit_function_call(char *stmt) {
    DEBUG_PRINT("DEBUG emit_function_call: '%s'\n", stmt);

    char *open = strchr(stmt, '(');
    char *close = strrchr(stmt, ')');
    if (!open || !close) return;

    *open = '\0';
    char func_name[MAX_TOKEN];
    strcpy(func_name, stmt);
    trim(func_name);

    *close = '\0';
    char *args = open + 1;
    trim(args);

    if (strlen(args) > 0) {
        // Klammer-bewusstes Argument-Parsing
        char arg_list[10][MAX_LINE];
        int arg_count = 0;
        int paren_depth = 0;
        int start = 0;

        for (int i = 0; args[i] != '\0' && arg_count < 10; i++) {
            if (args[i] == '(') paren_depth++;
            if (args[i] == ')') paren_depth--;

            if (args[i] == ',' && paren_depth == 0) {
                int len = i - start;
                strncpy(arg_list[arg_count], args + start, len);
                arg_list[arg_count][len] = '\0';
                trim(arg_list[arg_count]);

                DEBUG_PRINT("  Argument %d: '%s'\n", arg_count, arg_list[arg_count]);
                arg_count++;

                start = i + 1;
            }
        }

        if (start < strlen(args)) {
            strcpy(arg_list[arg_count], args + start);
            trim(arg_list[arg_count]);
            DEBUG_PRINT("  Argument %d: '%s'\n", arg_count, arg_list[arg_count]);
            arg_count++;
        }

        for (int i = arg_count - 1; i >= 0; i--) {
            emit_expression(arg_list[i]);
        }

        if (arg_count > 0) code_printf("    popq %%rcx\n");
        if (arg_count > 1) code_printf("    popq %%rdx\n");
        if (arg_count > 2) code_printf("    popq %%r8\n");
        if (arg_count > 3) code_printf("    popq %%r9\n");
    }

    code_printf("    subq $32, %%rsp\n");
    code_printf("    call %s\n", func_name);
    code_printf("    addq $32, %%rsp\n");
    code_printf("    pushq %%rax\n");
}

void compile_line(char *line);

void compile_for_loop(FILE *input, char *for_stmt) {
    DEBUG_PRINT("DEBUG compile_for_loop: '%s'\n", for_stmt);

    char *open = strchr(for_stmt, '(');
    char *close = strchr(for_stmt, ')');
    if (!open || !close) {
        fprintf(stderr, "Fehler: Ungültige for-Schleife\n");
        return;
    }

    *open = '\0';
    *close = '\0';
    char *header = open + 1;

    char init[MAX_LINE] = {0};
    char condition[MAX_LINE] = {0};
    char increment[MAX_LINE] = {0};

    char *semi1 = strchr(header, ';');
    if (semi1) {
        *semi1 = '\0';
        strcpy(init, header);

        char *semi2 = strchr(semi1 + 1, ';');
        if (semi2) {
            *semi2 = '\0';
            strcpy(condition, semi1 + 1);
            strcpy(increment, semi2 + 1);
        }
    }

    trim(init);
    trim(condition);
    trim(increment);

    DEBUG_PRINT("  Init: '%s'\n", init);
    DEBUG_PRINT("  Condition: '%s'\n", condition);
    DEBUG_PRINT("  Increment: '%s'\n", increment);

    current_function_scope++;
    int saved_var_count = var_count;
    DEBUG_PRINT("  Loop-Scope start: scope=%d, var_count=%d\n", current_function_scope, saved_var_count);

    int loop_id = label_counter++;

    if (strlen(init) > 0) {
        compile_line(init);
    }

    code_printf(".L_for_start_%d:\n", loop_id);

    if (strlen(condition) > 0) {
        emit_expression(condition);
        code_printf("    popq %%rax\n");
        code_printf("    testl %%eax, %%eax\n");
        code_printf("    je .L_for_end_%d\n", loop_id);
    }

    char line[MAX_LINE];
    int brace_count = 0;
    int found_brace = 0;

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        trim(line);
        if (strchr(line, '{')) {
            found_brace = 1;
            brace_count = 1;
            break;
        }
    }

    if (!found_brace) {
        fprintf(stderr, "Fehler: Keine öffnende Klammer für for-Schleife\n");
        current_function_scope--;
        var_count = saved_var_count;
        return;
    }

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        for (char *p = line; *p; p++) {
            if (*p == '{') brace_count++;
            if (*p == '}') brace_count--;
        }

        if (brace_count == 0) {
            break;
        }

        trim(line);
        if (strncmp(line, "for", 3) == 0 && strchr(line, '(')) {
            compile_for_loop(input, line);
        } else {
            compile_line(line);
        }
    }

    if (strlen(increment) > 0) {
        trim(increment);

        if (strstr(increment, "++")) {
            char var_name[MAX_TOKEN];
            char *pp = strstr(increment, "++");
            int name_len = pp - increment;
            strncpy(var_name, increment, name_len);
            var_name[name_len] = '\0';
            trim(var_name);

            int offset = get_var_offset(var_name);
            code_printf("    movl -%d(%%rbp), %%eax\n", offset);
            code_printf("    addl $1, %%eax\n");
            code_printf("    movl %%eax, -%d(%%rbp)\n", offset);
        } else if (strstr(increment, "--")) {
            char var_name[MAX_TOKEN];
            char *mm = strstr(increment, "--");
            int name_len = mm - increment;
            strncpy(var_name, increment, name_len);
            var_name[name_len] = '\0';
            trim(var_name);

            int offset = get_var_offset(var_name);
            code_printf("    movl -%d(%%rbp), %%eax\n", offset);
            code_printf("    subl $1, %%eax\n");
            code_printf("    movl %%eax, -%d(%%rbp)\n", offset);
        } else {
            compile_line(increment);
        }
    }

    code_printf("    jmp .L_for_start_%d\n", loop_id);
    code_printf(".L_for_end_%d:\n", loop_id);

    DEBUG_PRINT("  Loop-Scope end: var_count %d -> %d\n", var_count, saved_var_count);
    var_count = saved_var_count;
    current_function_scope--;
}

void compile_function(FILE *input, char *func_header) {
    DEBUG_PRINT("DEBUG compile_function: '%s'\n", func_header);
    DEBUG_PRINT("  var_count VOR Funktion=%d, current_scope=%d\n", var_count, current_function_scope);

    char *arrow = strstr(func_header, "->");
    if (!arrow) {
        fprintf(stderr, "Fehler: Funktion braucht Rückgabetyp (-> type)\n");
        exit(1);
    }

    *arrow = '\0';
    char *return_type_str = arrow + 2;
    trim(return_type_str);

    char *brace = strchr(return_type_str, '{');
    int has_opening_brace = 0;
    if (brace) {
        *brace = '\0';
        has_opening_brace = 1;
        fprintf(stderr, "⚠️  Warnung (Zeile %d): Öffnende Klammer '{' sollte auf neuer Zeile stehen.\n", current_line_number);
    }
    trim(return_type_str);

    DataType return_type = string_to_type(return_type_str);

    char *open = strchr(func_header, '(');
    char *close = strchr(func_header, ')');
    if (!open || !close) {
        fprintf(stderr, "Fehler: Ungültige Funktionssyntax\n");
        exit(1);
    }

    *open = '\0';
    *close = '\0';

    char *name_part = func_header + 5;
    trim(name_part);

    int is_main = (strcmp(name_part, "main") == 0);
    if (is_main) {
        main_function_found = 1;

        if (return_type != TYPE_VOID) {
            fprintf(stderr, "Fehler (Zeile %d): main() muss Rückgabetyp 'void' haben!\n", current_line_number);
            exit(1);
        }

        DEBUG_PRINT("  *** MAIN-FUNKTION gefunden ***\n");
    }

    char *params = open + 1;
    trim(params);

    strcpy(functions[function_count].name, name_part);
    functions[function_count].param_count = 0;
    functions[function_count].return_type = return_type;

    current_function_scope++;
    int saved_stack_offset = stack_offset;
    int saved_var_count = var_count;

    stack_offset = 0;

    DEBUG_PRINT("  Scope=%d, saved_var_count=%d\n", current_function_scope, saved_var_count);

    if (strlen(params) > 0) {
        char temp[MAX_LINE];
        strcpy(temp, params);
        char *token = strtok(temp, ",");
        while (token && functions[function_count].param_count < 10) {
            trim(token);

            char *colon = strchr(token, ':');
            if (!colon) {
                fprintf(stderr, "Fehler: Parameter braucht Typ (name:type)\n");
                exit(1);
            }

            *colon = '\0';
            char *param_name = token;
            char *param_type = colon + 1;
            trim(param_name);
            trim(param_type);

            DEBUG_PRINT("  Registriere Parameter '%s:%s'\n", param_name, param_type);

            strcpy(functions[function_count].params[functions[function_count].param_count], param_name);
            functions[function_count].param_types[functions[function_count].param_count] = string_to_type(param_type);
            functions[function_count].param_count++;

            token = strtok(NULL, ",");
        }
    }

    code_printf("\n.globl %s\n", name_part);
    code_printf("%s:\n", name_part);
    code_printf("    pushq %%rbp\n");
    code_printf("    movq %%rsp, %%rbp\n");
    code_printf("    subq $2048, %%rsp\n");

    for (int i = 0; i < functions[function_count].param_count; i++) {
        DEBUG_PRINT("  Erstelle Parameter-Variable '%s'\n", functions[function_count].params[i]);
        int offset = create_variable(functions[function_count].params[i], functions[function_count].param_types[i]);

        switch (i) {
            case 0: code_printf("    movl %%ecx, -%d(%%rbp)\n", offset); break;
            case 1: code_printf("    movl %%edx, -%d(%%rbp)\n", offset); break;
            case 2: code_printf("    movl %%r8d, -%d(%%rbp)\n", offset); break;
            case 3: code_printf("    movl %%r9d, -%d(%%rbp)\n", offset); break;
        }
    }

    DEBUG_PRINT("  var_count nach Parametern=%d\n", var_count);

    function_count++;

    char line[MAX_LINE];
    int brace_count = 0;

    if (has_opening_brace) {
        DEBUG_PRINT("  Öffnende Klammer war im Header\n");
        brace_count = 1;
    } else {
        int found_brace = 0;
        while (fgets(line, sizeof(line), input)) {
            current_line_number++;
            size_t len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
                line[--len] = '\0';
            }

            trim(line);
            DEBUG_PRINT("  Suche '{': '%s'\n", line);
            if (strchr(line, '{')) {
                found_brace = 1;
                brace_count = 1;
                break;
            }
        }

        if (!found_brace) {
            fprintf(stderr, "Fehler: Keine öffnende Klammer für Funktion\n");
            exit(1);
        }
    }

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        for (char *p = line; *p; p++) {
            if (*p == '{') brace_count++;
            if (*p == '}') brace_count--;
        }

        if (brace_count == 0) {
            DEBUG_PRINT("  Funktionsende erreicht bei: '%s'\n", line);
            break;
        }

        DEBUG_PRINT("  Body-Zeile: '%s'\n", line);
        trim(line);

        if (strncmp(line, "for", 3) == 0 && strchr(line, '(')) {
            compile_for_loop(input, line);
        } else {
            compile_line(line);
        }
    }

    if (return_type == TYPE_VOID) {
        code_printf("    movl $0, %%eax\n");
    }

    code_printf("    leave\n");
    code_printf("    ret\n\n");

    DEBUG_PRINT("  Ende Funktion: var_count %d -> %d\n", var_count, saved_var_count);

    var_count = saved_var_count;
    stack_offset = saved_stack_offset;
    current_function_scope--;

    DEBUG_PRINT("  Nach Reset: var_count=%d, scope=%d\n\n", var_count, current_function_scope);
}

void compile_line(char *line) {
    DEBUG_PRINT("DEBUG compile_line: '%s'\n", line);

    char *comment = strstr(line, "//");
    if (comment) *comment = '\0';

    trim(line);
    int len = strlen(line);
    if (len > 0 && line[len - 1] == ';') {
        line[len - 1] = '\0';
        trim(line);
    }

    if (strlen(line) == 0) return;

    if (strcmp(line, "{") == 0 || strcmp(line, "}") == 0) {
        return;
    }

    if (strncmp(line, "print", 5) == 0) {
        emit_print(line);
        return;
    }

    if (strncmp(line, "return", 6) == 0) {
        char *expr = line + 6;
        trim(expr);
        if (strlen(expr) > 0) {
            emit_expression(expr);
            code_printf("    popq %%rax\n");
        }
        code_printf("    leave\n");
        code_printf("    ret\n");
        return;
    }

    if (strchr(line, '(') && strchr(line, ')') && !strchr(line, '=') && strncmp(line, "var ", 4) != 0 && strncmp(line, "func ", 5) != 0) {
        emit_function_call(line);
        code_printf("    popq %%rax\n");
        return;
    }

    if (strncmp(line, "var ", 4) == 0) {
        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "Fehler: Variable muss initialisiert werden!\n");
            exit(1);
        }

        *eq = '\0';
        char *left = line + 4;
        char *right = eq + 1;

        trim(left);
        trim(right);

        DataType var_type = TYPE_INT;

        if (isdigit(right[0]) || (right[0] == '-' && isdigit(right[1]))) {
            var_type = TYPE_INT;
        } else if (right[0] == '"') {
            var_type = TYPE_STRING;
        } else {
            var_type = TYPE_INT;
        }

        int offset = create_variable(left, var_type);

        emit_expression(right);
        code_printf("    popq %%rax\n");
        code_printf("    movl %%eax, -%d(%%rbp)\n", offset);
        return;
    }

    char *eq = strchr(line, '=');
    if (eq) {
        char op = 0;
        if (eq > line && (eq[-1] == '+' || eq[-1] == '-' || eq[-1] == '*' || eq[-1] == '/')) {
            op = eq[-1];
            eq[-1] = '\0';
        }

        *eq = '\0';
        char *left = line;
        char *right = eq + 1;

        trim(left);
        trim(right);

        DataType var_type = get_var_type(left);
        if (var_type == TYPE_UNKNOWN) {
            fprintf(stderr, "Fehler: Variable '%s' nicht deklariert! Nutze 'var %s = ...'\n", left, left);
            exit(1);
        }

        int offset = get_var_offset(left);

        if (op) {
            emit_push_var(left);
            DataType expr_type = emit_expression(right);

            if (var_type != expr_type && expr_type != TYPE_UNKNOWN) {
                fprintf(stderr, "Fehler: Typ-Konflikt bei '%s' (erwartet %s, bekam %s)\n",
                        left, type_to_string(var_type), type_to_string(expr_type));
                exit(1);
            }

            code_printf("    popq %%rbx\n");
            code_printf("    popq %%rax\n");

            switch (op) {
                case '+': code_printf("    addl %%ebx, %%eax\n"); break;
                case '-': code_printf("    subl %%ebx, %%eax\n"); break;
                case '*': code_printf("    imull %%ebx, %%eax\n"); break;
                case '/':
                    code_printf("    cltd\n");
                    code_printf("    idivl %%ebx\n");
                    break;
            }
        } else {
            DataType expr_type = emit_expression(right);

            if (var_type != expr_type && expr_type != TYPE_UNKNOWN) {
                fprintf(stderr, "Fehler: Typ-Konflikt bei '%s' (erwartet %s, bekam %s)\n",
                        left, type_to_string(var_type), type_to_string(expr_type));
                exit(1);
            }

            code_printf("    popq %%rax\n");
        }

        code_printf("    movl %%eax, -%d(%%rbp)\n", offset);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [--debug] <source_file>\n", argv[0]);
        return 1;
    }

    char *source_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            DEBUG_MODE = 1;
            DEBUG_PRINT("DEBUG MODE ENABLED\n\n");
        } else {
            source_file = argv[i];
        }
    }

    if (!source_file) {
        printf("Fehler: Keine Quelldatei angegeben!\n");
        printf("Usage: %s [--debug] <source_file>\n", argv[0]);
        return 1;
    }

    FILE *input = fopen(source_file, "r");
    if (!input) {
        printf("Fehler: Datei '%s' konnte nicht geoeffnet werden\n", source_file);
        return 1;
    }

    char line[MAX_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), input)) {
        line_num++;
        current_line_number = line_num;

        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        trim(line);

        DEBUG_PRINT("\n=== Zeile %d: '%s' ===\n", line_num, line);

        if (strncmp(line, "func ", 5) == 0) {
            compile_function(input, line);
        } else if (strlen(line) > 0 && line[0] != '/' && strcmp(line, "{") != 0 && strcmp(line, "}") != 0) {
            fprintf(stderr, "⚠️  Warnung (Zeile %d): Code außerhalb von Funktionen wird ignoriert: '%s'\n",
                    line_num, line);
            fprintf(stderr, "   Alle Statements müssen innerhalb von Funktionen stehen!\n");
        }
    }

    fclose(input);

    if (!main_function_found) {
        fprintf(stderr, "\n❌ Fehler: Keine main() Funktion gefunden!\n");
        fprintf(stderr, "   Jedes Programm muss eine main() -> void Funktion enthalten.\n");
        fprintf(stderr, "\n   Beispiel:\n");
        fprintf(stderr, "   func main() -> void\n");
        fprintf(stderr, "   {\n");
        fprintf(stderr, "       print(\"Hello World\");\n");
        fprintf(stderr, "   }\n");
        return 1;
    }

    char output_name[256];
    snprintf(output_name, sizeof(output_name), "%s.s", source_file);
    FILE *output = fopen(output_name, "w");

    fprintf(output, "    .section .rdata,\"dr\"\n");
    for (int i = 0; i < string_literal_count; i++) {
        fprintf(output, ".LC%d:\n", string_literals[i].id);
        fprintf(output, "    .string \"%s\"\n", string_literals[i].text);
    }

    fprintf(output, "    .text\n");
    fprintf(output, "%s", function_code_buffer);

    fclose(output);

    printf("=================================\n");
    printf("✅ Kompilierung erfolgreich!\n");
    printf("=================================\n");
    printf("📄 Zeilen: %d\n", line_num);
    printf("🔤 Strings: %d\n", string_literal_count);
    printf("⚙️  Funktionen: %d\n", function_count);
    printf("\n📝 Assembly: %s\n", output_name);
    printf("\n🔨 Kompilieren: gcc %s -o program\n", output_name);
    printf("▶️  Ausführen:  ./program\n");
    printf("=================================\n");

    return 0;
}