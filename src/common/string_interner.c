#include "string_interner.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;
    size_t length;
    uint64_t hash;
} InternedString;

struct StringInterner {
    InternedString *entries;
    size_t capacity;
    size_t count;
    size_t bytes;
};

static uint64_t hash_bytes(const char *text, size_t length) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) text[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int grow(StringInterner *interner) {
    size_t capacity = interner->capacity == 0 ? 64U : interner->capacity * 2U;
    if (capacity < interner->capacity || capacity > SIZE_MAX / sizeof(*interner->entries))
        return 0;
    InternedString *entries = calloc(capacity, sizeof(*entries));
    if (entries == NULL) return 0;
    for (size_t i = 0; i < interner->capacity; i++) {
        InternedString entry = interner->entries[i];
        if (entry.text == NULL) continue;
        size_t slot = (size_t) entry.hash & (capacity - 1U);
        while (entries[slot].text != NULL) slot = (slot + 1U) & (capacity - 1U);
        entries[slot] = entry;
    }
    free(interner->entries);
    interner->entries = entries;
    interner->capacity = capacity;
    return 1;
}

StringInterner *string_interner_create(void) {
    return calloc(1, sizeof(StringInterner));
}

void string_interner_free(StringInterner *interner) {
    if (interner == NULL) return;
    for (size_t i = 0; i < interner->capacity; i++) free(interner->entries[i].text);
    free(interner->entries);
    free(interner);
}

const char *string_interner_intern_n(StringInterner *interner, const char *text,
                                     size_t length) {
    if (interner == NULL || text == NULL || length == SIZE_MAX) return NULL;
    if (interner->capacity == 0 ||
        interner->count + 1U >= interner->capacity - interner->capacity / 4U)
        if (!grow(interner)) return NULL;
    uint64_t hash = hash_bytes(text, length);
    size_t slot = (size_t) hash & (interner->capacity - 1U);
    while (interner->entries[slot].text != NULL) {
        InternedString *entry = &interner->entries[slot];
        if (entry->hash == hash && entry->length == length &&
            memcmp(entry->text, text, length) == 0)
            return entry->text;
        slot = (slot + 1U) & (interner->capacity - 1U);
    }
    if (length + 1U > SIZE_MAX - interner->bytes) return NULL;
    char *copy = malloc(length + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    interner->entries[slot] = (InternedString){copy, length, hash};
    interner->count++;
    interner->bytes += length + 1U;
    return copy;
}

const char *string_interner_intern(StringInterner *interner, const char *text) {
    return text == NULL ? NULL : string_interner_intern_n(interner, text, strlen(text));
}

size_t string_interner_count(const StringInterner *interner) {
    return interner == NULL ? 0 : interner->count;
}

size_t string_interner_bytes(const StringInterner *interner) {
    return interner == NULL ? 0 : interner->bytes;
}
