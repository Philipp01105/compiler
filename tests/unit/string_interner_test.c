#include "string_interner.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    StringInterner *interner = string_interner_create();
    if (interner == NULL) return 1;

    const char *alpha = string_interner_intern(interner, "alpha");
    const char *alpha_slice = string_interner_intern_n(interner, "alphabet", 5);
    const char *empty = string_interner_intern(interner, "");
    if (alpha == NULL || alpha != alpha_slice || empty == NULL || empty[0] != '\0') return 2;

    for (size_t i = 0; i < 10000; i++) {
        char text[32];
        (void) snprintf(text, sizeof(text), "identifier_%zu", i);
        const char *first = string_interner_intern(interner, text);
        const char *second = string_interner_intern(interner, text);
        if (first == NULL || first != second || strcmp(first, text) != 0) return 3;
    }
    if (string_interner_count(interner) != 10002U ||
        string_interner_bytes(interner) <= string_interner_count(interner))
        return 4;

    string_interner_free(interner);
    return 0;
}
