#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_VARS 200
#define MAX_LINE 512
#define MAX_TOKEN 128
#define MAX_FUNCTIONS 100

// Kopiere deine Original-Strukturen
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
    char name[MAX_TOKEN];
    int param_count;
    char params[10][MAX_TOKEN];
    DataType param_types[10];
    DataType return_type;
    int local_var_count;  // Anzahl lokaler Variablen
    int total_var_count;  // Total (Parameter + Lokale)
} Function;

Variable vars[MAX_VARS];
int var_count = 0;
int stack_offset = 0;

// Spezieller Speicher für main() Variablen
Variable main_vars[MAX_VARS];
int main_var_count = 0;

Function functions[MAX_FUNCTIONS];
int function_count = 0;
int current_function_scope = 0;
int current_line_number = 0;
int in_main_function = 0;

// Für Analyse-Ausgabe
int total_expressions = 0;
int total_function_calls = 0;
int total_for_loops = 0;

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

int create_variable(const char *name, DataType type) {
    printf("      [*] Variable erstellt: '%s' (Typ: %s, Scope: %d, Offset: %d)\n",
           name, type_to_string(type), current_function_scope, stack_offset + 4);

    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0 && vars[i].scope == current_function_scope) {
            fprintf(stderr, "      [!] Variable '%s' bereits deklariert!\n", name);
            exit(1);
        }
    }

    stack_offset += 4;
    strcpy(vars[var_count].name, name);
    vars[var_count].offset = stack_offset;
    vars[var_count].scope = current_function_scope;
    vars[var_count].type = type;

    // Speichere main() Variablen separat
    if (in_main_function) {
        strcpy(main_vars[main_var_count].name, name);
        main_vars[main_var_count].offset = stack_offset;
        main_vars[main_var_count].scope = current_function_scope;
        main_vars[main_var_count].type = type;
        main_var_count++;
    }

    var_count++;
    return stack_offset;
}

void analyze_expression(char *expr) {
    trim(expr);
    printf("      [~] Expression analysiert: '%s'\n", expr);
    total_expressions++;

    // Zeige erkannte Operatoren
    if (strstr(expr, " + ")) printf("         -> Operator: Addition (+)\n");
    if (strstr(expr, " - ")) printf("         -> Operator: Subtraktion (-)\n");
    if (strstr(expr, " * ")) printf("         -> Operator: Multiplikation (*)\n");
    if (strstr(expr, " / ")) printf("         -> Operator: Division (/)\n");
    if (strstr(expr, " == ")) printf("         -> Operator: Gleichheit (==)\n");
    if (strstr(expr, " != ")) printf("         -> Operator: Ungleichheit (!=)\n");
    if (strstr(expr, " < ")) printf("         -> Operator: Kleiner (<)\n");
    if (strstr(expr, " > ")) printf("         -> Operator: Groesser (>)\n");
    if (strstr(expr, " <= ")) printf("         -> Operator: Kleiner-Gleich (<=)\n");
    if (strstr(expr, " >= ")) printf("         -> Operator: Groesser-Gleich (>=)\n");

    // Zeige Klammern
    if (strchr(expr, '(') && strchr(expr, ')')) {
        printf("         -> Klammern erkannt\n");
    }

    // Zeige Zahlen
    for (char *p = expr; *p; p++) {
        if (isdigit(*p)) {
            printf("         -> Numerisches Literal gefunden\n");
            break;
        }
    }
}

void analyze_print(char *stmt) {
    printf("      [P] Print-Statement: '%s'\n", stmt);

    char *open = strchr(stmt, '(');
    char *close = strrchr(stmt, ')');
    if (!open || !close) return;

    *close = '\0';
    char *content = open + 1;
    trim(content);

    printf("         -> Inhalt: '%s'\n", content);

    // Zeige String-Literale
    if (content[0] == '"') {
        printf("         -> String-Literal erkannt\n");
    }

    // Zeige Konkatenation
    if (strchr(content, '+')) {
        printf("         -> String-Konkatenation mit '+'\n");
    }
}

void analyze_function_call(char *stmt) {
    printf("      [F] Funktionsaufruf: '%s'\n", stmt);
    total_function_calls++;

    char *open = strchr(stmt, '(');
    char *close = strrchr(stmt, ')');
    if (!open || !close) return;

    *open = '\0';
    char func_name[MAX_TOKEN];
    strcpy(func_name, stmt);
    trim(func_name);

    printf("         -> Funktion: '%s'\n", func_name);

    *close = '\0';
    char *args = open + 1;
    trim(args);

    if (strlen(args) > 0) {
        int arg_count = 1;
        for (char *p = args; *p; p++) {
            if (*p == ',') arg_count++;
        }
        printf("         -> Argumente: %d\n", arg_count);
    } else {
        printf("         -> Keine Argumente\n");
    }
}

void compile_line(char *line, FILE *input);

void compile_for_loop(FILE *input, char *for_stmt) {
    printf("      [@] For-Schleife erkannt: '%s'\n", for_stmt);
    total_for_loops++;

    char *open = strchr(for_stmt, '(');
    char *close = strchr(for_stmt, ')');
    if (!open || !close) return;

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

    printf("         +- Init: '%s'\n", init);
    printf("         +- Condition: '%s'\n", condition);
    printf("         +- Increment: '%s'\n", increment);

    current_function_scope++;
    int saved_var_count = var_count;

    printf("         [S] Scope erhoeht: %d -> %d\n", current_function_scope - 1, current_function_scope);

    if (strlen(init) > 0) {
        compile_line(init, NULL);
    }

    // Lese Loop-Body (vereinfacht)
    char line[MAX_LINE];
    int brace_count = 0;

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;
        trim(line);

        if (strchr(line, '{')) {
            brace_count = 1;
            break;
        }
    }

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;

        for (char *p = line; *p; p++) {
            if (*p == '{') brace_count++;
            if (*p == '}') brace_count--;
        }

        if (brace_count == 0) break;

        trim(line);
        compile_line(line, input);
    }

    printf("         [S] Scope reduziert: %d -> %d (Variablen cleanup: %d -> %d)\n",
           current_function_scope, current_function_scope - 1, var_count, saved_var_count);

    var_count = saved_var_count;
    current_function_scope--;
}

void compile_function(FILE *input, char *func_header) {
    printf("\n   [FUNC] FUNKTION WIRD VERARBEITET\n");
    printf("   ===============================================================\n");
    printf("      Header: '%s'\n", func_header);

    char *arrow = strstr(func_header, "->");
    if (!arrow) {
        fprintf(stderr, "      [X] Fehler: Kein Rueckgabetyp gefunden\n");
        exit(1);
    }

    *arrow = '\0';
    char *return_type_str = arrow + 2;
    trim(return_type_str);

    DataType return_type = string_to_type(return_type_str);
    printf("      -> Rueckgabetyp: %s\n", type_to_string(return_type));

    char *open = strchr(func_header, '(');
    char *close = strchr(func_header, ')');
    if (!open || !close) return;

    *open = '\0';
    *close = '\0';

    char *name_part = func_header + 5;
    trim(name_part);

    printf("      -> Funktionsname: '%s'\n", name_part);

    if (strcmp(name_part, "main") == 0) {
        printf("      [*] MAIN-FUNKTION erkannt!\n");
        in_main_function = 1;
    } else {
        in_main_function = 0;
    }

    char *params = open + 1;
    trim(params);

    strcpy(functions[function_count].name, name_part);
    functions[function_count].param_count = 0;
    functions[function_count].return_type = return_type;
    functions[function_count].local_var_count = 0;
    functions[function_count].total_var_count = 0;

    current_function_scope++;
    int saved_var_count = var_count;
    int var_count_before_body = var_count;
    stack_offset = 0;

    if (strlen(params) > 0) {
        printf("      -> Parameter gefunden:\n");
        char temp[MAX_LINE];
        strcpy(temp, params);
        char *token = strtok(temp, ",");
        while (token && functions[function_count].param_count < 10) {
            trim(token);

            char *colon = strchr(token, ':');
            if (colon) {
                *colon = '\0';
                char *param_name = token;
                char *param_type = colon + 1;
                trim(param_name);
                trim(param_type);

                printf("         * %s: %s\n", param_name, param_type);

                strcpy(functions[function_count].params[functions[function_count].param_count], param_name);
                functions[function_count].param_types[functions[function_count].param_count] = string_to_type(param_type);
                functions[function_count].param_count++;

                create_variable(param_name, string_to_type(param_type));
            }

            token = strtok(NULL, ",");
        }
        var_count_before_body = var_count;
    } else {
        printf("      -> Keine Parameter\n");
    }

    function_count++;

    printf("      -> Function-Body wird verarbeitet...\n\n");

    char line[MAX_LINE];
    int brace_count = 0;

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;
        trim(line);

        if (strchr(line, '{')) {
            brace_count = 1;
            break;
        }
    }

    while (fgets(line, sizeof(line), input)) {
        current_line_number++;

        for (char *p = line; *p; p++) {
            if (*p == '{') brace_count++;
            if (*p == '}') brace_count--;
        }

        if (brace_count == 0) {
            printf("      [OK] Funktionsende erreicht\n");
            break;
        }

        trim(line);

        if (strncmp(line, "for", 3) == 0 && strchr(line, '(')) {
            compile_for_loop(input, line);
        } else {
            compile_line(line, input);
        }
    }

    // Berechne Variablen-Anzahl für diese Funktion
    int local_vars = var_count - var_count_before_body;
    functions[function_count - 1].local_var_count = local_vars;
    functions[function_count - 1].total_var_count = var_count - saved_var_count;

    printf("      -> Statistik: %d Parameter, %d lokale Variablen, %d total\n",
           functions[function_count - 1].param_count,
           local_vars,
           functions[function_count - 1].total_var_count);
    printf("      -> Scope cleanup: Variablen %d -> %d\n\n", var_count, saved_var_count);

    var_count = saved_var_count;
    stack_offset = 0;
    current_function_scope--;
    in_main_function = 0;
}

void compile_line(char *line, FILE *input) {
    char *comment = strstr(line, "//");
    if (comment) {
        printf("      [#] Kommentar: '%s'\n", comment);
        *comment = '\0';
    }

    trim(line);
    int len = strlen(line);
    if (len > 0 && line[len - 1] == ';') {
        line[len - 1] = '\0';
        trim(line);
    }

    if (strlen(line) == 0) return;
    if (strcmp(line, "{") == 0 || strcmp(line, "}") == 0) return;

    printf("      [L] Zeile %d: '%s'\n", current_line_number, line);

    if (strncmp(line, "print", 5) == 0) {
        analyze_print(line);
        return;
    }

    if (strncmp(line, "return", 6) == 0) {
        char *expr = line + 6;
        trim(expr);
        printf("      [R] Return-Statement");
        if (strlen(expr) > 0) {
            printf(": '%s'\n", expr);
            analyze_expression(expr);
        } else {
            printf(" (void)\n");
        }
        return;
    }

    if (strchr(line, '(') && strchr(line, ')') && !strchr(line, '=') && strncmp(line, "var ", 4) != 0) {
        analyze_function_call(line);
        return;
    }

    if (strncmp(line, "var ", 4) == 0) {
        printf("      [V] Variablendeklaration: '%s'\n", line);
        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "      [X] Variable muss initialisiert werden!\n");
            exit(1);
        }

        *eq = '\0';
        char *left = line + 4;
        char *right = eq + 1;

        trim(left);
        trim(right);

        DataType var_type = TYPE_INT;
        create_variable(left, var_type);

        printf("         -> Initial-Wert: '%s'\n", right);
        analyze_expression(right);
        return;
    }

    char *eq = strchr(line, '=');
    if (eq) {
        printf("      [=] Assignment: '%s'\n", line);

        char op = 0;
        if (eq > line && (eq[-1] == '+' || eq[-1] == '-' || eq[-1] == '*' || eq[-1] == '/')) {
            op = eq[-1];
            printf("         -> Compound Assignment: '%c='\n", op);
        }

        *eq = '\0';
        char *left = line;
        char *right = eq + 1;

        trim(left);
        trim(right);

        printf("         -> Links: '%s'\n", left);
        printf("         -> Rechts: '%s'\n", right);

        analyze_expression(right);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <source_file>\n", argv[0]);
        return 1;
    }

    FILE *input = fopen(argv[1], "r");
    if (!input) {
        printf("Fehler: Datei '%s' konnte nicht geoeffnet werden\n", argv[1]);
        return 1;
    }

    printf("\n");
    printf("================================================================\n");
    printf("        COMPILER ANALYZER - ORIGINAL LOGIC TRACE\n");
    printf("================================================================\n");
    printf("  Datei: %s\n", argv[1]);
    printf("  Dieser Analyzer zeigt EXAKT, wie dein Compiler arbeitet!\n\n");

    printf("================================================================\n");
    printf("                  PARSING & ANALYSE START\n");
    printf("================================================================\n");

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

        if (strncmp(line, "func ", 5) == 0) {
            compile_function(input, line);
        } else if (strlen(line) > 0 && line[0] != '/' && strcmp(line, "{") != 0 && strcmp(line, "}") != 0) {
            printf("\n   [!] Code ausserhalb von Funktionen (Zeile %d): '%s'\n", line_num, line);
        }
    }

    fclose(input);

    printf("\n");
    printf("================================================================\n");
    printf("                  DETAILLIERTE FUNKTIONS-TABELLE\n");
    printf("================================================================\n\n");

    printf("%-20s %-8s %-8s %-10s %-15s\n",
           "Funktion", "Params", "Lokale", "Total", "Return-Typ");
    printf("---------------------------------------------------------------------\n");

    for (int i = 0; i < function_count; i++) {
        printf("%-20s %-8d %-8d %-10d %-15s\n",
               functions[i].name,
               functions[i].param_count,
               functions[i].local_var_count,
               functions[i].total_var_count,
               type_to_string(functions[i].return_type));
    }

    printf("\n");
    printf("================================================================\n");
    printf("                  VARIABLEN IN main() FUNKTION\n");
    printf("================================================================\n\n");

    if (main_var_count > 0) {
        printf("   %-20s %-10s %-10s %-10s\n", "Name", "Typ", "Scope", "Offset");
        printf("   -------------------------------------------------------------\n");
        for (int i = 0; i < main_var_count; i++) {
            printf("   %-20s %-10s %-10d %-10d\n",
                   main_vars[i].name,
                   type_to_string(main_vars[i].type),
                   main_vars[i].scope,
                   main_vars[i].offset);
        }
    } else {
        printf("   (Keine Variablen in main() gefunden)\n");
    }

    printf("\n");
    printf("================================================================\n");
    printf("                      STATISTIKEN\n");
    printf("================================================================\n\n");
    printf("   Code-Metriken:\n");
    printf("      * Zeilen:            %d\n", line_num);
    printf("      * Funktionen:        %d\n", function_count);
    printf("      * Variablen (main):  %d\n", main_var_count);
    printf("      * Expressions:       %d\n", total_expressions);
    printf("      * Funktionsaufrufe:  %d\n", total_function_calls);
    printf("      * For-Schleifen:     %d\n", total_for_loops);

    printf("\n   Funktions-Komplexitaet:\n");
    int total_params = 0;
    int total_locals = 0;
    int max_vars = 0;
    char max_func[MAX_TOKEN] = "";

    for (int i = 0; i < function_count; i++) {
        total_params += functions[i].param_count;
        total_locals += functions[i].local_var_count;
        if (functions[i].total_var_count > max_vars) {
            max_vars = functions[i].total_var_count;
            strcpy(max_func, functions[i].name);
        }
    }

    printf("      * Total Parameter:      %d\n", total_params);
    printf("      * Total lokale Vars:    %d\n", total_locals);
    printf("      * Groesste Funktion:    %s (%d Variablen)\n", max_func, max_vars);
    printf("      * Durchschn. Vars/Func: %.1f\n",
           function_count > 0 ? (float)(total_params + total_locals) / function_count : 0.0);

    printf("\n");
    printf("================================================================\n");
    printf("                  [OK] ANALYSE ABGESCHLOSSEN\n");
    printf("================================================================\n\n");

    return 0;
}