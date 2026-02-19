#include "asm_optimizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) return 3;
    for (int i = 0; i < 12050; ++i) fprintf(file, "label_%d:\n", i);
    fputs("    pushq %rax\n    popq %rax\n    push rbx\n    pop rbx\n    ret\n", file);
    if (fclose(file) != 0 || cleanup_assembly_file(argv[1]) != 0) return 4;

    file = fopen(argv[1], "rb");
    if (file == NULL) return 5;
    char line[128];
    int count = 0;
    int saw_push = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        ++count;
        if (strstr(line, "pushq") != NULL || strstr(line, "popq") != NULL) saw_push = 1;
    }
    fclose(file);
    remove(argv[1]);
    return count == 12051 && !saw_push ? 0 : 6;
}
