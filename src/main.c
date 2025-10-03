#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "compiler_types.h"
#include "lexer.h"
#include "parser.h"

// Print usage information
void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <source_file>\n", program_name);
    printf("\nOptions:\n");
    printf("  --tokens    Show generated token stream\n");
    printf("  --debug     Enable debug output during parsing\n");
    printf("  --help      Show this help message\n");
    printf("\nExample:\n");
    printf("  %s program.txt\n", program_name);
    printf("  %s --tokens --debug program.txt\n", program_name);
}

// Get current date/time as string
void get_current_datetime(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Main function
int main(int argc, char *argv[]) {
    // Parse command line arguments
    int show_tokens = 0;
    int debug_mode = 0;
    char *source_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) {
            show_tokens = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unbekannte Option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            source_file = argv[i];
        }
    }

    // Check if source file was provided
    if (!source_file) {
        fprintf(stderr, "Fehler: Keine Quelldatei angegeben\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Print header
    printf("\n");
    printf("================================================================\n");
    printf("         COMPILER MIT LEXER & PARSER - Philipp01105\n");
    printf("================================================================\n");
    printf("  Datei: %s\n", source_file);

    // Read source file - BINARY MODE
    FILE *input = fopen(source_file, "rb");
    if (!input) {
        fprintf(stderr, "Fehler: Datei '%s' konnte nicht geoeffnet werden\n", source_file);
        return 1;
    }

    // Get file size
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "Fehler: Datei ist leer oder konnte nicht gelesen werden\n");
        fclose(input);
        return 1;
    }

    printf("  Groesse: %ld Bytes\n", file_size);

    // Get current date/time
    char datetime[64];
    get_current_datetime(datetime, sizeof(datetime));
    printf("  Datum: %s\n", datetime);

    // Allocate buffer with extra space for null terminator
    char *source = (char *)malloc(file_size + 1);
    if (!source) {
        fprintf(stderr, "Fehler: Speicher konnte nicht allokiert werden (%ld Bytes)\n", file_size + 1);
        fclose(input);
        return 1;
    }

    // Read file into buffer
    size_t bytes_read = fread(source, 1, file_size, input);
    fclose(input);

    // Null-terminate the buffer
    source[bytes_read] = '\0';

    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "Warnung: Nur %zu von %ld Bytes gelesen\n", bytes_read, file_size);
    }

    // Phase 1: Lexical Analysis (Tokenization)
    printf("\n");
    printf("================================================================\n");
    printf("           PHASE 1: LEXIKALISCHE ANALYSE (LEXER)\n");
    printf("================================================================\n");
    printf("\n");

    TokenStream *tokens = tokenize_source(source);

    if (!tokens) {
        fprintf(stderr, "Fehler: Tokenisierung fehlgeschlagen\n");
        free(source);
        return 1;
    }

    printf("\xE2\x9C\x93 %d Tokens generiert\n", tokens->count);

    // Show tokens if requested
    if (show_tokens) {
        print_all_tokens(tokens);
    }

    // Phase 2: Syntax Analysis (Parsing)
    Parser *parser = create_parser(tokens);
    parser->debug_mode = debug_mode;

    int success = parse_program(parser);

    if (!success) {
        fprintf(stderr, "\n\xE2\x9D\x8C Kompilierung fehlgeschlagen!\n\n");
        free_parser(parser);
        free_token_stream(tokens);
        free(source);
        return 1;
    }

    // Phase 3: Code Generation (Output Assembly)
    printf("\n");
    printf("================================================================\n");
    printf("           PHASE 3: CODE-GENERIERUNG (ASSEMBLY)\n");
    printf("================================================================\n");
    printf("\n");

    // Generate output filename
    char output_file[512];
    snprintf(output_file, sizeof(output_file), "%s.s", source_file);

    FILE *output = fopen(output_file, "w");
    if (!output) {
        fprintf(stderr, "Fehler: Ausgabedatei '%s' konnte nicht erstellt werden\n", output_file);
        free_parser(parser);
        free_token_stream(tokens);
        free(source);
        return 1;
    }

    // Write assembly header
    fprintf(output, "# Generated by Philipp01105's Compiler\n");
    fprintf(output, "# Source: %s\n", source_file);
    fprintf(output, "# Date: %s\n", datetime);
    fprintf(output, "\n");
    fprintf(output, "    .text\n");
    fprintf(output, "    .def    printf; .scl    2; .type   32; .endef\n");
    fprintf(output, "    .def    putchar; .scl    2; .type   32; .endef\n");
    fprintf(output, "    .section .rdata,\"dr\"\n");

    // Write string literals
    for (int i = 0; i < parser->string_literal_count; i++) {
        fprintf(output, ".LC%d:\n", parser->string_literals[i].id);
        fprintf(output, "    .ascii \"%s\\0\"\n", parser->string_literals[i].text);
    }

    fprintf(output, "\n    .text\n");

    // Write main code (global scope)
    fprintf(output, "%s", parser->code_buffer);

    // Write function code
    fprintf(output, "%s", parser->function_code_buffer);

    fclose(output);

    printf("OK Assembly-Datei geschrieben: %s\n", output_file);
    printf("OK Code-Groesse: %d Bytes (Global), %d Bytes (Funktionen)\n",
           parser->code_pos, parser->function_code_pos);
    printf("OK String-Literale: %d\n", parser->string_literal_count);

    // Success message
    printf("\n");
    printf("================================================================\n");
    printf("                    KOMPILIERUNG ERFOLGREICH\n");
    printf("================================================================\n");
    printf("\n");
    printf("Naechste Schritte:\n");
    printf("  1. Assembly zu Binary: gcc %s -o %s.exe\n", output_file, source_file);
    printf("  2. Programm ausfuehren: ./%s.exe\n", source_file);
    printf("\n");

    // Cleanup
    free_parser(parser);
    free_token_stream(tokens);
    free(source);
    
    return 0;
}