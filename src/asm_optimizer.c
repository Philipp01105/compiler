#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Helper function to trim leading whitespace from a string
 */
static char *trim_leading_whitespace(char *str) {
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    return str;
}

/*
 * Helper function to check if two instructions form a useless push/pop pair
 * Returns 1 if they match (useless), 0 otherwise
 */
static int is_useless_push_pop(const char *line1, const char *line2) {
    char *trimmed1 = trim_leading_whitespace((char *)line1);
    char *trimmed2 = trim_leading_whitespace((char *)line2);
    
    // Check for pattern: pushq %reg followed by popq %reg
    if (strncmp(trimmed1, "pushq ", 6) == 0 && strncmp(trimmed2, "popq ", 5) == 0) {
        // Extract the register from both instructions
        const char *reg1 = trimmed1 + 6;
        const char *reg2 = trimmed2 + 5;
        
        // Compare registers (they should be the same)
        return strcmp(reg1, reg2) == 0;
    }
    
    return 0;
}

/*
 * remove_useless_push_pop - Remove useless push/pop pairs from assembly
 * @filename: Path to the assembly file to clean up
 *
 * This function removes patterns like:
 *   pushq %rax
 *   popq %rax
 * which are no-ops and waste cycles.
 *
 * Returns: 0 on success, -1 on error
 */
static int remove_useless_push_pop(const char *filename) {
    FILE *input = fopen(filename, "r");
    if (!input) {
        return -1;
    }

    // Read entire file into memory
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);

    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(input);
        return -1;
    }

    size_t bytes_read = fread(content, 1, file_size, input);
    content[bytes_read] = '\0';
    fclose(input);

    // Store lines in an array for easier processing
    char **lines = malloc(sizeof(char*) * 10000);  // Assume max 10000 lines
    int line_count = 0;
    
    char *line_start = content;
    char *line_end;
    
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        *line_end = '\0';
        lines[line_count] = strdup(line_start);
        line_count++;
        line_start = line_end + 1;
    }
    
    // Handle last line if no newline at end
    if (*line_start != '\0') {
        lines[line_count] = strdup(line_start);
        line_count++;
    }

    free(content);

    // Create temporary file for cleaned output
    char temp_filename[520];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp2", filename);
    
    FILE *output = fopen(temp_filename, "w");
    if (!output) {
        for (int i = 0; i < line_count; i++) {
            free(lines[i]);
        }
        free(lines);
        return -1;
    }

    // Process lines, removing useless push/pop pairs
    int i = 0;
    while (i < line_count) {
        // Check if current and next line form a useless push/pop pair
        if (i + 1 < line_count && is_useless_push_pop(lines[i], lines[i + 1])) {
            // Skip both lines (the useless pair)
            i += 2;
        } else {
            // Write the line
            fprintf(output, "%s\n", lines[i]);
            i++;
        }
    }

    // Clean up
    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);
    fclose(output);

    // Replace original file
    if (remove(filename) != 0) {
        remove(temp_filename);
        return -1;
    }

    if (rename(temp_filename, filename) != 0) {
        return -1;
    }

    return 0;
}

/*
 * cleanup_assembly_file - Remove unreachable code from generated assembly
 * @filename: Path to the assembly file to clean up
 *
 * This function performs a post-processing pass on the generated assembly file
 * to remove dead/unreachable code that appears after ret instructions within
 * function bodies. This improves code quality and reduces binary size.
 *
 * Returns: 0 on success, -1 on error
 */
int cleanup_assembly_file(const char *filename) {
    FILE *input = fopen(filename, "r");
    if (!input) {
        fprintf(stderr, "Error: Could not open file for cleanup: %s\n", filename);
        return -1;
    }

    // Read the entire file into memory
    fseek(input, 0, SEEK_END);
    long file_size = ftell(input);
    fseek(input, 0, SEEK_SET);

    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(input);
        return -1;
    }

    size_t bytes_read = fread(content, 1, file_size, input);
    content[bytes_read] = '\0';
    fclose(input);

    // Create temporary file for cleaned output
    char temp_filename[520];
    snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);
    
    FILE *output = fopen(temp_filename, "w");
    if (!output) {
        free(content);
        return -1;
    }

    // Process line by line
    char *line_start = content;
    char *line_end;
    int in_function = 0;
    int found_ret = 0;
    int removed_lines = 0;

    while ((line_end = strchr(line_start, '\n')) != NULL) {
        *line_end = '\0';  // Temporarily terminate the line
        
        char *line = line_start;
        
        // Skip leading whitespace for analysis
        while (*line == ' ' || *line == '\t') {
            line++;
        }

        // Detect function start (labels that are global or local function labels)
        if (strstr(line_start, ".globl ") != NULL || 
            (line[0] != '.' && line[0] != '#' && line[0] != ' ' && line[0] != '\t' && 
             strchr(line, ':') != NULL && strstr(line, ".LC") == NULL)) {
            // Reset state for new function
            in_function = 1;
            found_ret = 0;
        }

        // Check for ret instruction
        if (in_function && strcmp(line, "ret") == 0) {
            if (!found_ret) {
                // First ret - write it
                found_ret = 1;
                fprintf(output, "%s\n", line_start);
            } else {
                // Duplicate ret - skip it (unreachable)
                removed_lines++;
            }
            line_start = line_end + 1;
            continue;
        }

        // If we found a ret and we're in unreachable code
        if (found_ret && in_function) {
            // Check if this is the start of a new function or section
            if (strstr(line_start, ".globl ") != NULL ||
                (line[0] != ' ' && line[0] != '\t' && line[0] != '#' && 
                 strchr(line, ':') != NULL && strstr(line, ".LC") == NULL) ||
                strstr(line, ".text") != NULL ||
                strstr(line, ".data") != NULL ||
                strstr(line, ".bss") != NULL ||
                strstr(line, ".section") != NULL ||
                line[0] == '\0') {  // Empty line often marks section boundary
                // End of unreachable code section
                found_ret = 0;
                in_function = 0;
                fprintf(output, "%s\n", line_start);
            } else {
                // This is unreachable code - skip it
                removed_lines++;
                // Skip writing this line
            }
        } else {
            // Normal code - write it
            fprintf(output, "%s\n", line_start);
        }

        line_start = line_end + 1;
    }

    // Write any remaining content (last line without newline)
    if (*line_start != '\0') {
        fprintf(output, "%s", line_start);
    }

    free(content);
    fclose(output);

    // Replace original file with cleaned version
    if (remove(filename) != 0) {
        fprintf(stderr, "Warning: Could not remove original file\n");
        remove(temp_filename);
        return -1;
    }

    if (rename(temp_filename, filename) != 0) {
        fprintf(stderr, "Warning: Could not rename temporary file\n");
        return -1;
    }

    if (removed_lines > 0) {
        // Only print if we're in debug mode or similar
        // For now, silent cleanup
    }

    // Second pass: Remove useless push/pop pairs
    if (remove_useless_push_pop(filename) != 0) {
        fprintf(stderr, "Warning: Failed to remove useless push/pop pairs\n");
    }

    return 0;
}
