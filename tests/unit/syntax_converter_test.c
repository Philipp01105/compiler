#include "syntax_converter.h"

#include <stdio.h>
#include <string.h>

static int expect(const char *input, const char *wanted) {
    char output[4096];
    return convert_att_to_intel(input, output, sizeof(output)) >= 0 && strcmp(output, wanted) == 0;
}

int main(void) {
    if (!expect("    movl -4(%rbp), %eax\n", "    mov  eax, [rbp-4]\n")) return 1;
    if (!expect("    leaq .LC0(%rip), %rdi\n", "    lea  rdi, [rip + .LC0]\n")) return 2;
    if (!expect("    movl 8(%rbp,%rax,4), %edx # indexed\n",
                "    mov  edx, [rbp+rax*4+8] # indexed\n")) return 3;
    char tiny[4];
    if (convert_att_to_intel("movq %rax, %rbx", tiny, sizeof(tiny)) >= 0) return 4;
    char long_label[3500];
    memset(long_label, 'a', sizeof(long_label));
    memcpy(long_label, "leaq ", 5);
    memcpy(long_label + sizeof(long_label) - 13, "(%rip), %rax", 13);
    long_label[sizeof(long_label) - 1] = '\0';
    char long_output[4096];
    if (convert_att_to_intel(long_label, long_output, sizeof(long_output)) < 0 ||
        strstr(long_output, "[rip + ") == NULL) return 5;
    return 0;
}
/* Category: unit/syntax converter. */
