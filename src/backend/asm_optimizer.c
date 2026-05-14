#include "asm_optimizer.h"

#include <ctype.h>
#include <errno.h>
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

#ifdef DMM_OPTIMIZER_FAULT_TEST
static int fail_flush, fail_close, fail_replace, close_count;
void assembly_cleanup_test_fail(int flush, int close, int replace) {
    fail_flush = flush; fail_close = close; fail_replace = replace; close_count = 0;
}
int assembly_cleanup_test_close_count(void) { return close_count; }
#endif

static int flush_output(FILE *output) {
    int result = fflush(output);
#ifdef DMM_OPTIMIZER_FAULT_TEST
    if (fail_flush) { errno = ENOSPC; return -1; }
#endif
    return result;
}

static int close_output(FILE *output) {
    int result = fclose(output);
#ifdef DMM_OPTIMIZER_FAULT_TEST
    close_count++;
    if (fail_close) { errno = EIO; return -1; }
#endif
    return result;
}

static void record_error(AssemblyCleanupError *error, const char *operation,
                         const char *path, int number, unsigned long windows_error) {
    if (error->operation != NULL) return;
    error->operation = operation;
    error->error_number = number == 0 ? EIO : number;
    error->windows_error = windows_error;
    (void) snprintf(error->reason, sizeof(error->reason), "%s", strerror(error->error_number));
#ifdef _WIN32
    if (windows_error != 0 &&
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, (DWORD) windows_error, 0, error->reason,
                       (DWORD) sizeof(error->reason), NULL) != 0)
        error->reason[strcspn(error->reason, "\r\n")] = '\0';
#endif
    (void) snprintf(error->path, sizeof(error->path), "%s", path == NULL ? "" : path);
}

static int replace_file(const char *temporary, const char *destination) {
#ifdef DMM_OPTIMIZER_FAULT_TEST
    if (fail_replace) { errno = EACCES; return -1; }
#endif
#ifdef _WIN32
    return MoveFileExA(temporary, destination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(temporary, destination);
#endif
}

int cleanup_assembly_file(const char *filename) {
    return cleanup_assembly_file_detailed(filename, NULL);
}

int cleanup_assembly_file_detailed(const char *filename, AssemblyCleanupError *error) {
    AssemblyCleanupError local_error = {0};
    if (error == NULL) error = &local_error;
    *error = (AssemblyCleanupError) {0};
    if (filename == NULL) {
        record_error(error, "validate input path", filename, EINVAL, 0);
        return -1;
    }

    size_t filename_length = strlen(filename);
    if (filename_length > SIZE_MAX - 64) {
        record_error(error, "allocate temporary path", filename, ENAMETOOLONG, 0);
        return -1;
    }
    char *temporary = malloc(filename_length + 64);
    if (temporary == NULL) {
        record_error(error, "allocate temporary path", filename, ENOMEM, 0);
        return -1;
    }

    int written = snprintf(temporary, filename_length + 64, "%s.tmp.%lu.%lu",
                           filename, process_id(), (unsigned long) clock());
    if (written < 0 || (size_t) written >= filename_length + 64) {
        record_error(error, "format temporary path", filename, ENAMETOOLONG, 0);
        free(temporary);
        return -1;
    }

    FILE *input = fopen(filename, "rb");
    if (input == NULL) {
        record_error(error, "open input", filename, errno, 0);
        free(temporary);
        return -1;
    }
    FILE *output = fopen(temporary, "wb");
    if (output == NULL) {
        record_error(error, "open temporary output", temporary, errno, 0);
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
                record_error(error, "write temporary output", temporary, errno, 0);
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
    if (result < 0) {
        record_error(error, "read input", filename, errno, 0);
        status = -1;
    }
    if (status == 0 && previous != NULL && fputs(previous, output) == EOF) {
        record_error(error, "write temporary output", temporary, errno, 0);
        status = -1;
    }

    free(previous);
    free(current);
    if (fclose(input) != 0) {
        record_error(error, "close input", filename, errno, 0);
        status = -1;
    }
    if (flush_output(output) != 0) {
        record_error(error, "flush temporary output", temporary, errno, 0);
        status = -1;
    }
    if (close_output(output) != 0) {
        record_error(error, "close temporary output", temporary, errno, 0);
        status = -1;
    }

    if (status == 0 && replace_file(temporary, filename) != 0) {
        unsigned long windows_error = 0;
#ifdef _WIN32
#ifdef DMM_OPTIMIZER_FAULT_TEST
        if (!fail_replace)
#endif
            windows_error = GetLastError();
#endif
        record_error(error, "replace assembly output", filename, errno, windows_error);
        status = -1;
    }
    if (status != 0) remove(temporary);
    free(temporary);
    if (status != 0) errno = error->error_number;
    return status;
}
