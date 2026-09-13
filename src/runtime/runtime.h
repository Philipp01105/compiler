#ifndef DMM_RUNTIME_H
#define DMM_RUNTIME_H
#include <stdint.h>
#include <stddef.h>

/* Runtime ABI v1: integral arguments/results occupy signed 64-bit ABI slots.
   The compiler normalizes source primitives at the typed IR boundary. */
int64_t __dmm_rt_strlen(const char *text);
int64_t __dmm_rt_strcmp(const char *left, const char *right);
char *__dmm_rt_strcpy(char *destination, const char *source);
char *__dmm_rt_strcat(char *destination, const char *source);
char *__dmm_rt_strdup(const char *text);
void *__dmm_rt_malloc(int64_t size);
int64_t __dmm_rt_scan_int(void);
int64_t __dmm_rt_scan_char(void);
char *__dmm_rt_scan_string(void);
int64_t __dmm_rt_int_to_string(int64_t value, char *buffer, int64_t capacity);
int64_t __dmm_rt_string_to_int(const char *text);
int64_t __dmm_rt_read_value(int64_t descriptor, const char *format);
int64_t __dmm_rt_sys_read(int64_t descriptor, void *buffer, int64_t count);
int64_t __dmm_rt_sys_write(int64_t descriptor, const void *buffer, int64_t count);
int64_t __dmm_rt_sys_open(const char *path, int64_t flags, int64_t mode);
int64_t __dmm_rt_sys_close(int64_t descriptor);

#endif
