#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "lexer.h"
#include "parser.h"

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <source_file>\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --tokens    Show generated token stream\n");
    printf("  --debug     Enable debug output during parsing\n");
    printf("  --help      Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s program.txt\n", program_name);
    printf("  %s --tokens program.txt\n", program_name);
    printf("  %s --debug program.txt\n", program_name);
    printf("  %s --tokens --debug program.txt\n", program_name);
    printf("\n");
}

void print_header(const char *source_file) {
    struct stat st;
    stat(source_file, &st);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char datetime[64];
    strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", t);

    printf("\n");
    printf("################################################################\n");
    printf("#                                                              #\n");
    printf("#                      DMM COMPILER                            #\n");
    printf("#                                                              #\n");
    printf("################################################################\n");
    printf("\n");
    printf("  File:    %s\n", source_file);
    printf("  Size:    %ld Bytes\n", st.st_size);
    printf("  Date:    %s\n", datetime);
    printf("\n");
}

void write_escaped_string(FILE *out, const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '\n':
                fprintf(out, "\\n");
                break;
            case '\t':
                fprintf(out, "\\t");
                break;
            case '\r':
                fprintf(out, "\\r");
                break;
            case '\\':
                fprintf(out, "\\\\");
                break;
            case '"':
                fprintf(out, "\\\"");
                break;
            case '\0':
                fprintf(out, "\\0");
                break;
            default:
                if (c >= 32 && c < 127) {
                    fprintf(out, "%c", c);
                } else {
                    fprintf(out, "\\%03o", c);
                }
        }
    }
}

int main(int argc, char *argv[]) {
    int show_tokens = 0;
    int debug_mode = 0;
    const char *source_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            source_file = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!source_file) {
        fprintf(stderr, "Error: No source file specified\n\n");
        print_usage(argv[0]);
        return 1;
    }

    print_header(source_file);

    printf("================================================================\n");
    printf("           PHASE 1: LEXICAL ANALYSIS (LEXER)\n");
    printf("================================================================\n");
    printf("\n");

    TokenStream *tokens = tokenize_file(source_file, debug_mode);
    if (!tokens) {
        fprintf(stderr, "[ERROR] Lexer failed!\n");
        return 1;
    }

    printf("  [+] %d Tokens generated\n", tokens->count);
    printf("\n");

    if (show_tokens) {
        print_tokens(tokens);
    }

    Parser *parser = create_parser(tokens);
    parser->debug_mode = debug_mode;

    if (!parse_program(parser, source_file)) {
        fprintf(stderr, "\n[ERROR] Compilation failed!\n\n");
        free_parser(parser);
        free_token_stream(tokens);
        return 1;
    }

    printf("\n");
    printf("================================================================\n");
    printf("           PHASE 3: CODE GENERATION\n");
    printf("================================================================\n");
    printf("\n");

    char output_filename[512];
    snprintf(output_filename, sizeof(output_filename), "%s.s", source_file);

    FILE *output = fopen(output_filename, "w");
    if (!output) {
        fprintf(stderr, "Error: Could not create output file '%s'\n", output_filename);
        free_parser(parser);
        free_token_stream(tokens);
        return 1;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char datetime[64];
    strftime(datetime, sizeof(datetime), "%Y-%m-%d %H:%M:%S", t);

    // Detect platform: Windows uses COFF format, Linux/Unix use ELF format
    int is_windows = 0;
#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
    is_windows = 1;
#endif

    fprintf(output, "# Generated by Philipp01105's Compiler\n");
    fprintf(output, "# Source: %s\n", source_file);
    fprintf(output, "# Date: %s\n", datetime);
    fprintf(output, "# Platform: %s\n", is_windows ? "Windows (COFF)" : "Linux/Unix (ELF)");
    fprintf(output, "    .text\n");
    
    // Windows-specific directives
    if (is_windows) {
        fprintf(output, "    .def    printf; .scl    2; .type   32; .endef\n");
        fprintf(output, "    .def    putchar; .scl    2; .type   32; .endef\n");
        fprintf(output, "    .section .rdata,\"dr\"\n");
    } else {
        // Linux/Unix ELF format
        fprintf(output, "    .section .rodata\n");
    }

    fprintf(output, ".LC_int_format:\n");
    fprintf(output, "    .ascii \"%%d\\0\"\n");
    fprintf(output, ".LC_float_format:\n");
    fprintf(output, "    .ascii \"%%f\\0\"\n");
    fprintf(output, ".LC_double_format:\n");
    fprintf(output, "    .ascii \"%%lf\\0\"\n");
    fprintf(output, ".LC_char_format:\n");
    fprintf(output, "    .ascii \"%%c\\0\"\n");
    fprintf(output, ".LC_char_input_format:\n");
    fprintf(output, "    .ascii \" %%c\\0\"\n");
    fprintf(output, ".LC_string_format:\n");
    fprintf(output, "    .ascii \"%%s\\0\"\n");
    fprintf(output, ".LC_newline:\n");
    fprintf(output, "    .ascii \"\\n\\0\"\n");
    fprintf(output, "\n");

    if (parser->string_literal_count > 0) {
        fprintf(output, "# String literals\n");
        for (int i = 0; i < parser->string_literal_count; i++) {
            fprintf(output, ".LC%d:\n", parser->string_literals[i].id);
            fprintf(output, "    .ascii \"");
            write_escaped_string(output, parser->string_literals[i].text);
            fprintf(output, "\\0\"\n");
        }
        fprintf(output, "\n");
    }

    if (parser->float_literal_count > 0) {
        fprintf(output, "# Float/Double constants\n");
        fprintf(output, "    .align 4\n");

        for (int i = 0; i < parser->float_literal_count; i++) {
            fprintf(output, ".LC_float_%d:\n", parser->float_literals[i].id);

            int is_double = (strchr(parser->float_literals[i].value, '.') != NULL &&
                           strlen(strchr(parser->float_literals[i].value, '.')) > 8);

            if (is_double) {
                double dval = strtod(parser->float_literals[i].value, NULL);
                unsigned long long *double_bits = (unsigned long long*)&dval;
                fprintf(output, "    .quad 0x%016llx    # double %s\n",
                        *double_bits, parser->float_literals[i].value);
            } else {
                float fval = strtof(parser->float_literals[i].value, NULL);
                unsigned int *float_bits = (unsigned int*)&fval;
                fprintf(output, "    .long 0x%08x    # float %s\n",
                        *float_bits, parser->float_literals[i].value);
            }
        }
        fprintf(output, "\n");
    }

    fprintf(output, "    .text\n");
    fprintf(output, "%s", parser->function_code_buffer);

    fclose(output);

    printf("  [+] Assembly code generated: %s\n", output_filename);
    printf("      - Code size: %d Bytes\n", parser->function_code_pos);
    printf("      - String literals: %d\n", parser->string_literal_count);
    printf("      - Float literals: %d\n", parser->float_literal_count);
    printf("\n");
    printf("################################################################\n");
    printf("#                                                              #\n");
    printf("#            [SUCCESS] COMPILATION COMPLETED!                 #\n");
    printf("#                                                              #\n");
    printf("################################################################\n");
    printf("\n");
    printf("Next steps:\n");
    printf("  [1] Assemble: gcc -no-pie %s -o program\n", output_filename);
    printf("  [2] Execute:  ./program\n");
    printf("\n");

    free_parser(parser);
    free_token_stream(tokens);

    return 0;
}