#include "asm_optimizer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int read_line(FILE *input, char **line, size_t *capacity) {
    size_t length = 0;
    int ch = EOF;

    if (*line == NULL) {
        *capacity = 256;
        *line = malloc(*capacity);
        if (*line == NULL) return -1;
    }

    while ((ch = fgetc(input)) != EOF) {
        if (length + 1 >= *capacity) {
            if (*capacity > SIZE_MAX / 2) return -1;
            size_t new_capacity = *capacity * 2;
            char *grown = realloc(*line, new_capacity);
            if (grown == NULL) return -1;
            *line = grown;
            *capacity = new_capacity;
        }
        (*line)[length++] = (char) ch;
        if (ch == '\n') break;
    }

    if (ferror(input)) return -1;
    if (length == 0 && ch == EOF) return 0;
    (*line)[length] = '\0';
    return 1;
}

static const char *skip_space(const char *text) {
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

static size_t operand_length(const char *operand) {
    size_t length = strcspn(operand, "\r\n#;");
    while (length > 0 && isspace((unsigned char) operand[length - 1])) --length;
    return length;
}

static int is_useless_push_pop(const char *first, const char *second) {
    first = skip_space(first);
    second = skip_space(second);
    if (strncmp(first, "pushq ", 6) == 0 && strncmp(second, "popq ", 5) == 0) {
        first += 6;
        second += 5;
    } else if (strncmp(first, "push ", 5) == 0 && strncmp(second, "pop ", 4) == 0) {
        first += 5;
        second += 4;
    } else {
        return 0;
    }
    first = skip_space(first);
    second = skip_space(second);
    size_t first_length = operand_length(first);
    size_t second_length = operand_length(second);
    return first_length != 0 && first_length == second_length &&
           memcmp(first, second, first_length) == 0;
}

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long) GetCurrentProcessId();
#else
    return (unsigned long) getpid();
#endif
}

static int replace_file(const char *temporary, const char *destination) {
#ifdef _WIN32
    return MoveFileExA(temporary, destination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(temporary, destination);
#endif
}

int cleanup_assembly_file(const char *filename) {
    if (filename == NULL) return -1;

    size_t filename_length = strlen(filename);
    if (filename_length > SIZE_MAX - 64) return -1;
    char *temporary = malloc(filename_length + 64);
    if (temporary == NULL) return -1;

    int written = snprintf(temporary, filename_length + 64, "%s.tmp.%lu.%lu",
                           filename, process_id(), (unsigned long) clock());
    if (written < 0 || (size_t) written >= filename_length + 64) {
        free(temporary);
        return -1;
    }

    FILE *input = fopen(filename, "rb");
    if (input == NULL) {
        free(temporary);
        return -1;
    }
    FILE *output = fopen(temporary, "wb");
    if (output == NULL) {
        fclose(input);
        free(temporary);
        return -1;
    }

    char *previous = NULL;
    char *current = NULL;
    size_t previous_capacity = 0;
    size_t current_capacity = 0;
    int status = 0;
    int result;

    while ((result = read_line(input, &current, &current_capacity)) > 0) {
        if (previous != NULL && is_useless_push_pop(previous, current)) {
            free(previous);
            previous = NULL;
            previous_capacity = 0;
        } else {
            if (previous != NULL && fputs(previous, output) == EOF) {
                status = -1;
                break;
            }
            char *swap_line = previous;
            previous = current;
            current = swap_line;
            size_t swap_capacity = previous_capacity;
            previous_capacity = current_capacity;
            current_capacity = swap_capacity;
        }
    }
    if (result < 0) status = -1;
    if (status == 0 && previous != NULL && fputs(previous, output) == EOF) status = -1;

    free(previous);
    free(current);
    if (fclose(input) != 0) status = -1;
    if (fflush(output) != 0 || fclose(output) != 0) status = -1;

    if (status == 0 && replace_file(temporary, filename) != 0) status = -1;
    if (status != 0) remove(temporary);
    free(temporary);
    return status;
}
