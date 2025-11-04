#include <stdio.h>

void check_alignment() {
    unsigned long rsp;
    __asm__("mov %%rsp, %0" : "=r"(rsp));
    printf("RSP at function entry: %p (mod 16 = %lu)\n", (void*)rsp, rsp % 16);
}

int main() {
    check_alignment();
    return 0;
}
