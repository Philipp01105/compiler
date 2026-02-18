#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "lexer.h"
#include "parser.h"
#include "errorHandler.h"
#include "asm_optimizer.h"
#include "instruction_builder.h"

/*
 * print_usage - Display command line usage information
 * @program_name: Name of the program
 */
void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <source_file>\n", program_name);
    printf("\n");
    printf("Options:\n");
    printf("  --tokens       Show generated token stream\n");
    printf("  --debug        Enable debug output during parsing\n");
    printf("  --formatError  Output errors in JSON format\n");
    printf("  --syntax=MODE  Assembly syntax: att or intel (default: intel)\n");
    printf("  --target=FMT   Target format: elf or coff (default: auto-detect)\n");
    printf("  --help         Show this help message\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s program.txt\n", program_name);
    printf("  %s --tokens program.txt\n", program_name);
    printf("  %s --debug program.txt\n", program_name);
    printf("  %s --syntax=intel --target=coff program.txt\n", program_name);
    printf("  %s --syntax=att --target=elf program.txt\n", program_name);
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
        unsigned char c = (unsigned char) str[i];
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
    int format_error = 0;
    const char *source_file = NULL;
    SyntaxMode syntax_mode = SYNTAX_INTEL; /* Default to Intel syntax */
    TargetFormat target_format = TARGET_ELF; /* Auto-detect later */
    int target_format_explicit = 0; /* Whether user specified target */

    ErrorHandler *error_handler = error_handler_init();
    if (error_handler) {
        error_handler_set_global(error_handler);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--formatError") == 0) {
            format_error = 1;
            if (error_handler) {
                error_handler_set_json_output(error_handler, 1);
            }
        } else if (strncmp(argv[i], "--syntax=", 9) == 0) {
            const char *mode = argv[i] + 9;
            if (strcmp(mode, "intel") == 0) {
                syntax_mode = SYNTAX_INTEL;
            } else if (strcmp(mode, "att") == 0) {
                syntax_mode = SYNTAX_ATT;
            } else {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL,
                             "Invalid syntax mode: %s (use 'intel' or 'att')", mode);
                error_handler_flush(error_handler);
                print_usage(argv[0]);
                error_handler_free(error_handler);
                return 1;
            }
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            const char *fmt = argv[i] + 9;
            if (strcmp(fmt, "elf") == 0) {
                target_format = TARGET_ELF;
                target_format_explicit = 1;
            } else if (strcmp(fmt, "coff") == 0) {
                target_format = TARGET_COFF;
                target_format_explicit = 1;
            } else {
                error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                             ERR_COMP_INVALID_OPTION, NULL,
                             "Invalid target format: %s (use 'elf' or 'coff')", fmt);
                error_handler_flush(error_handler);
                print_usage(argv[0]);
                error_handler_free(error_handler);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            error_handler_free(error_handler);
            return 0;
        } else if (argv[i][0] != '-') {
            source_file = argv[i];
        } else {
            error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                         ERR_COMP_INVALID_OPTION, NULL,
                         "Unknown option: %s", argv[i]);
            error_handler_flush(error_handler);
            print_usage(argv[0]);
            error_handler_free(error_handler);
            return 1;
        }
    }

    /* Auto-detect target format if not explicitly set */
    if (!target_format_explicit) {
#if defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)
        target_format = TARGET_COFF;
#else
        target_format = TARGET_ELF;
#endif
    }

    if (!source_file) {
        error_report(error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_COMPILER,
                     ERR_COMP_NO_SOURCE_FILE, NULL,
                     "No source file specified");
        error_handler_flush(error_handler);
        print_usage(argv[0]);
        error_handler_free(error_handler);
        return 1;
    }

    if (debug_mode || show_tokens) {
        print_header(source_file);

        printf("================================================================\n");
        printf("           PHASE 1: LEXICAL ANALYSIS (LEXER)\n");
        printf("================================================================\n");
        printf("\n");
    }

    TokenStream *tokens = tokenize_file(source_file, debug_mode);
    if (!tokens) {
        fprintf(stderr, "[ERROR] Lexer failed!\n");
        return 1;
    }

    if (debug_mode || show_tokens) {
        printf("  [+] %d Tokens generated\n", tokens->count);
        printf("\n");
    }

    if (show_tokens) {
        print_tokens(tokens);
    }

    Parser *parser = create_parser(tokens);
    parser->debug_mode = debug_mode;
    parser->target_format = target_format;
    parser->syntax_mode = syntax_mode;

    /* Set instruction builder syntax mode */
    set_syntax_mode(syntax_mode);

    if (!parse_program(parser, source_file)) {
        error_handler_flush(error_handler);
        if (!format_error) {
            fprintf(stderr, "\n[ERROR] Compilation failed!\n\n");
        }
        free_parser(parser);
        free_token_stream(tokens);
        error_handler_free(error_handler);
        return 1;
    }

    if (debug_mode) {
        printf("\n");
        printf("================================================================\n");
        printf("           PHASE 3: CODE GENERATION\n");
        printf("================================================================\n");
        printf("\n");
    }

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

    /* Determine format name for header comment */
    const char *format_name = (target_format == TARGET_COFF) ? "COFF" : "ELF";
    const char *syntax_name = (syntax_mode == SYNTAX_INTEL) ? "Intel" : "AT&T";

    fprintf(output, "# Generated by Philipp01105's Compiler\n");
    fprintf(output, "# Source: %s\n", source_file);
    fprintf(output, "# Date: %s\n", datetime);
    fprintf(output, "# Target Format: %s\n", format_name);
    fprintf(output, "# Syntax: %s\n", syntax_name);

    /* Add Intel syntax directive if using Intel syntax */
    if (syntax_mode == SYNTAX_INTEL) {
        fprintf(output, "    .intel_syntax noprefix\n");
    }

    fprintf(output, "    .text\n");

    if (target_format == TARGET_COFF) {
        fprintf(output, "    .def    printf; .scl    2; .type   32; .endef\n");
        fprintf(output, "    .def    putchar; .scl    2; .type   32; .endef\n");
        fprintf(output, "    .section .rdata,\"dr\"\n");
    } else {
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
    fprintf(output, ".LC_pointer_format:\n");
    fprintf(output, "    .ascii \"%%p\\0\"\n");
    fprintf(output, ".LC_string_input_format:\n");
    fprintf(output, "    .ascii \"%%255[^\\n]\\0\"\n");
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
                unsigned long long *double_bits = (unsigned long long *) &dval;
                fprintf(output, "    .quad 0x%016llx    # double %s\n",
                        *double_bits, parser->float_literals[i].value);
            } else {
                float fval = strtof(parser->float_literals[i].value, NULL);
                unsigned int *float_bits = (unsigned int *) &fval;
                fprintf(output, "    .long 0x%08x    # float %s\n",
                        *float_bits, parser->float_literals[i].value);
            }
        }
        fprintf(output, "\n");
    }

    fprintf(output, "    .bss\n");
    fprintf(output, "    .align 8\n");
    fprintf(output, "_scanfString_buffer:\n");
    fprintf(output, "    .space 256\n");
    fprintf(output, "\n");

    if (parser->code_pos > 0) {
        fprintf(output, "%s", parser->code_buffer);
    }

    fprintf(output, "    .text\n");
    fprintf(output, "%s", parser->function_code_buffer);

    fclose(output);

    // Post-process: Clean up unreachable code from assembly
    if (cleanup_assembly_file(output_filename) != 0) {
        fprintf(stderr, "Warning: Assembly cleanup pass failed\n");
    }

    if (debug_mode) {
        printf("  [+] Assembly code generated: %s\n", output_filename);
        printf("      - Code size: %d Bytes\n", parser->function_code_pos);
        printf("      - String literals: %d\n", parser->string_literal_count);
        printf("      - Float literals: %d\n", parser->float_literal_count);
        printf("      - Assembly optimized (dead code removed)\n");
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
    }

    free_parser(parser);
    free_token_stream(tokens);

    if (error_handler) {
        error_handler_flush(error_handler);
        error_handler_free(error_handler);
    }

    return 0;
}
