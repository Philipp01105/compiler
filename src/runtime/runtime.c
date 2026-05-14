#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *nonnull(const char *text) { return text == NULL ? "" : text; }

int64_t __dmm_rt_strlen(const char *text) { return (int64_t) strlen(nonnull(text)); }
int64_t __dmm_rt_strcmp(const char *left, const char *right) { return strcmp(nonnull(left), nonnull(right)); }
char *__dmm_rt_strcpy(char *destination, const char *source) { return strcpy(destination, nonnull(source)); }
char *__dmm_rt_strcat(char *destination, const char *source) { return strcat(destination, nonnull(source)); }
char *__dmm_rt_strdup(const char *text) {
    text = nonnull(text);
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}
void *__dmm_rt_malloc(int64_t size) { return size < 0 ? NULL : malloc((size_t) size); }
int64_t __dmm_rt_scan_int(void) {
    int value = 0;
    return scanf("%d", &value) == 1 ? value : 0;
}
int64_t __dmm_rt_scan_char(void) {
    char value = 0;
    return scanf(" %c", &value) == 1 ? (unsigned char) value : 0;
}
char *__dmm_rt_scan_string(void) {
    static char buffer[256];
    if (scanf("%255s", buffer) != 1) buffer[0] = '\0';
    return buffer;
}
int64_t __dmm_rt_int_to_string(int64_t value, char *buffer, int64_t capacity) {
    if (buffer == NULL || capacity <= 0) return 0;
    int result = snprintf(buffer, (size_t) capacity, "%lld", (long long) value);
    return result < 0 ? 0 : (int64_t) strlen(buffer);
}
int64_t __dmm_rt_string_to_int(const char *text) { return strtoll(nonnull(text), NULL, 10); }
int64_t __dmm_rt_read_value(int64_t descriptor, const char *format) {
    char buffer[64];
    int64_t count = __dmm_rt_sys_read(descriptor, buffer, 63);
    if (count <= 0) return 0;
    buffer[(size_t) count] = '\0';
    return strcmp(nonnull(format), "%c") == 0 ? (unsigned char) buffer[0] : strtoll(buffer, NULL, 10);
}
