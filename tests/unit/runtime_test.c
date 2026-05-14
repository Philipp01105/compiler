#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    char buffer[128];
    if (__dmm_rt_strlen(NULL) != 0 || __dmm_rt_strcmp(NULL, "") != 0) return 2;
    char *copy = __dmm_rt_strdup(NULL);
    if (copy == NULL || strcmp(copy, "") != 0) return 3;
    free(copy);
    copy = __dmm_rt_malloc(32);
    if (copy == NULL) return 4;
    __dmm_rt_strcpy(copy, "abc"); __dmm_rt_strcat(copy, "def");
    int correct = strcmp(copy, "abcdef") == 0;
    free(copy);
    if (!correct || __dmm_rt_string_to_int("-12345") != -12345 ||
        __dmm_rt_int_to_string(-12345, buffer, sizeof(buffer)) != 6 ||
        strcmp(buffer, "-12345") != 0) return 5;
    if (__dmm_rt_int_to_string(12345, buffer, 3) != 2 || strcmp(buffer, "12") != 0) return 6;
    int64_t descriptor = __dmm_rt_sys_open(argv[1], 577, 384);
    if (descriptor < 0 || __dmm_rt_sys_write(descriptor, "123", 3) != 3 ||
        __dmm_rt_sys_close(descriptor) != 0) return 7;
    descriptor = __dmm_rt_sys_open(argv[1], 1089, 384);
    if (descriptor < 0 || __dmm_rt_sys_write(descriptor, "45", 2) != 2 ||
        __dmm_rt_sys_close(descriptor) != 0) return 8;
    descriptor = __dmm_rt_sys_open(argv[1], 0, 0);
    memset(buffer, 0, sizeof(buffer));
    if (descriptor < 0 || __dmm_rt_sys_read(descriptor, buffer, sizeof(buffer) - 1) != 5 ||
        strcmp(buffer, "12345") != 0 || __dmm_rt_sys_close(descriptor) != 0) return 9;
    descriptor = __dmm_rt_sys_open(argv[1], 0, 0);
    if (descriptor < 0 || __dmm_rt_read_value(descriptor, "%i") != 12345 ||
        __dmm_rt_sys_close(descriptor) != 0) return 10;
    descriptor = __dmm_rt_sys_open(argv[1], 0, 0);
    if (descriptor < 0 || __dmm_rt_read_value(descriptor, "%c") != '1' ||
        __dmm_rt_sys_close(descriptor) != 0) return 11;
    if (__dmm_rt_sys_read(-1, buffer, 1) >= 0 || __dmm_rt_sys_write(-1, buffer, 1) >= 0 ||
        __dmm_rt_sys_close(-1) >= 0 || __dmm_rt_sys_read(0, buffer, -1) >= 0) return 12;
    FILE *file = fopen(argv[1], "wb");
    if (file == NULL || fputs("123 Z word", file) == EOF || fclose(file) != 0) return 13;
    if (freopen(argv[1], "rb", stdin) == NULL || __dmm_rt_scan_int() != 123 ||
        __dmm_rt_scan_char() != 'Z' || strcmp(__dmm_rt_scan_string(), "word") != 0 ||
        __dmm_rt_scan_int() != 0 || strcmp(__dmm_rt_scan_string(), "") != 0) return 14;
    fclose(stdin);
    remove(argv[1]);
    return 0;
}
