#ifndef DMM_STRING_INTERNER_H
#define DMM_STRING_INTERNER_H

#include <stddef.h>

typedef struct StringInterner StringInterner;

StringInterner *string_interner_create(void);
void string_interner_free(StringInterner *interner);
const char *string_interner_intern(StringInterner *interner, const char *text);
const char *string_interner_intern_n(StringInterner *interner, const char *text,
                                     size_t length);
size_t string_interner_count(const StringInterner *interner);
size_t string_interner_bytes(const StringInterner *interner);

#endif
