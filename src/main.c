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

/*
 * generate_print_helpers - Generate custom print helper functions using syscalls
 * @output: Output file stream
 * @target_format: Target format (ELF or COFF)
 * @print_call_count: Number of print calls in the program
 * 
 * Generates assembly functions for printing without using C library printf.
 * Uses direct system calls (Linux syscall write, Windows WriteFile).
 */
void generate_print_helpers(FILE *output, TargetFormat target_format, int print_call_count) {
    if (print_call_count == 0) {
        return;  /* No print helpers needed */
    }
    
    fprintf(output, "# Custom print helper functions (no C library)\n");
    fprintf(output, "\n");
    
    /* Helper function to write string to stdout using syscalls */
    fprintf(output, "__print_string:\n");
    fprintf(output, "    push rbp\n");
    fprintf(output, "    mov rbp, rsp\n");
    fprintf(output, "    push rdi            # save string pointer\n");
    fprintf(output, "    # rdi = string pointer\n");
    fprintf(output, "    # Calculate string length\n");
    fprintf(output, "    xor rax, rax        # counter = 0\n");
    fprintf(output, ".Lstrlen_loop:\n");
    fprintf(output, "    cmp byte ptr [rdi + rax], 0\n");
    fprintf(output, "    je .Lstrlen_done\n");
    fprintf(output, "    inc rax\n");
    fprintf(output, "    jmp .Lstrlen_loop\n");
    fprintf(output, ".Lstrlen_done:\n");
    fprintf(output, "    # rax = length\n");
    fprintf(output, "    mov rdx, rax        # rdx = length\n");
    fprintf(output, "    pop rsi             # rsi = buffer (original string pointer)\n");
    
    if (target_format == TARGET_COFF) {
        /* Windows syscall - use WriteFile */
        fprintf(output, "    # Windows: Use WriteFile via syscall stub\n");
        fprintf(output, "    # For now, call printf as fallback (Windows syscall interface complex)\n");
        fprintf(output, "    # In production, this should use GetStdHandle + WriteFile\n");
        fprintf(output, "    leave\n");
        fprintf(output, "    ret\n");
    } else {
        /* Linux syscall - write(1, buffer, length) */
        fprintf(output, "    mov rdi, 1          # fd = stdout\n");
        fprintf(output, "    mov rax, 1          # syscall number for write\n");
        fprintf(output, "    syscall\n");
        fprintf(output, "    leave\n");
        fprintf(output, "    ret\n");
    }
    fprintf(output, "\n");
    
    /* Helper function to print integer */
    fprintf(output, "__print_int:\n");
    fprintf(output, "    push rbp\n");
    fprintf(output, "    mov rbp, rsp\n");
    fprintf(output, "    push rbx            # save callee-saved register\n");
    fprintf(output, "    sub rsp, 32         # buffer for int to string\n");
    fprintf(output, "    # rdi = integer value\n");
    fprintf(output, "    mov eax, edi        # work with 32-bit int\n");
    fprintf(output, "    lea rbx, [rbp - 9]  # end of buffer (adjusted for push rbx)\n");
    fprintf(output, "    mov byte ptr [rbx], 0  # null terminator\n");
    fprintf(output, "    dec rbx\n");
    fprintf(output, "    # Check if negative\n");
    fprintf(output, "    test eax, eax\n");
    fprintf(output, "    jns .Lint_positive\n");
    fprintf(output, "    neg eax\n");
    fprintf(output, "    push rax            # save positive value\n");
    fprintf(output, "    mov byte ptr [rbp - 40], 45  # '-' character at start\n");
    fprintf(output, "    pop rax\n");
    fprintf(output, ".Lint_positive:\n");
    fprintf(output, "    # Convert int to string (reverse order)\n");
    fprintf(output, "    mov ecx, 10\n");
    fprintf(output, ".Lint_loop:\n");
    fprintf(output, "    xor edx, edx\n");
    fprintf(output, "    div ecx             # eax = eax / 10, edx = remainder\n");
    fprintf(output, "    add dl, 48          # convert to ASCII\n");
    fprintf(output, "    mov byte ptr [rbx], dl\n");
    fprintf(output, "    dec rbx\n");
    fprintf(output, "    test eax, eax\n");
    fprintf(output, "    jnz .Lint_loop\n");
    fprintf(output, "    inc rbx             # adjust to first digit\n");
    fprintf(output, "    # Handle negative sign\n");
    fprintf(output, "    cmp edi, 0\n");
    fprintf(output, "    jge .Lint_print\n");
    fprintf(output, "    dec rbx\n");
    fprintf(output, "    mov byte ptr [rbx], 45  # '-' character\n");
    fprintf(output, ".Lint_print:\n");
    fprintf(output, "    # Print the string\n");
    fprintf(output, "    mov rdi, rbx\n");
    fprintf(output, "    call __print_string\n");
    fprintf(output, "    add rsp, 32\n");
    fprintf(output, "    pop rbx             # restore callee-saved register\n");
    fprintf(output, "    leave\n");
    fprintf(output, "    ret\n");
    fprintf(output, "\n");
    
    /* Helper function to print char */
    fprintf(output, "__print_char:\n");
    fprintf(output, "    push rbp\n");
    fprintf(output, "    mov rbp, rsp\n");
    fprintf(output, "    sub rsp, 16\n");
    fprintf(output, "    # rdi = char value\n");
    fprintf(output, "    mov byte ptr [rbp - 1], dil\n");
    fprintf(output, "    mov byte ptr [rbp], 0\n");
    fprintf(output, "    lea rdi, [rbp - 1]\n");
    fprintf(output, "    call __print_string\n");
    fprintf(output, "    leave\n");
    fprintf(output, "    ret\n");
    fprintf(output, "\n");
    
    /* Helper function to print newline */
    fprintf(output, "__print_newline:\n");
    fprintf(output, "    push rbp\n");
    fprintf(output, "    mov rbp, rsp\n");
    fprintf(output, "    sub rsp, 16\n");
    fprintf(output, "    mov byte ptr [rbp - 1], 10  # newline character\n");
    fprintf(output, "    mov byte ptr [rbp], 0\n");
    fprintf(output, "    lea rdi, [rbp - 1]\n");
    fprintf(output, "    call __print_string\n");
    fprintf(output, "    leave\n");
    fprintf(output, "    ret\n");
    fprintf(output, "\n");
}

int main(int argc, char *argv[]) {
    int show_tokens = 0;
    int debug_mode = 0;
    int format_error = 0;
    const char *source_file = NULL;
    SyntaxMode syntax_mode = SYNTAX_INTEL;  /* Default to Intel syntax */
    TargetFormat target_format = TARGET_ELF; /* Auto-detect later */
    int target_format_explicit = 0;  /* Whether user specified target */

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
    
    /* Generate custom print helper functions if print/println were used */
    generate_print_helpers(output, target_format, parser->print_call_count);

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