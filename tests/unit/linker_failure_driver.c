#include <stdio.h>

int main(void) {
    puts("private linker stdout");
    fputs("private linker stderr\n", stderr);
    return 42;
}
