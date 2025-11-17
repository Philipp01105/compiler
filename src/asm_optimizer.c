#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    return 0;
}
