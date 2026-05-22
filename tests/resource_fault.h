#ifndef DMM_RESOURCE_FAULT_H
#define DMM_RESOURCE_FAULT_H
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
void dmm_fault_reset(size_t allocation, size_t io);
size_t dmm_fault_allocations(void);
size_t dmm_fault_io_calls(void);
int dmm_fault_triggered(void);
void *dmm_fault_malloc(size_t size);
void *dmm_fault_calloc(size_t count, size_t size);
void *dmm_fault_realloc(void *pointer, size_t size);
FILE *dmm_fault_fopen(const char *path, const char *mode);
size_t dmm_fault_fwrite(const void *data, size_t size, size_t count, FILE *file);
int dmm_fault_fclose(FILE *file);
#ifndef DMM_RESOURCE_IMPLEMENTATION
#define malloc dmm_fault_malloc
#define calloc dmm_fault_calloc
#define realloc dmm_fault_realloc
#define fopen dmm_fault_fopen
#define fwrite dmm_fault_fwrite
#define fclose dmm_fault_fclose
#endif
#endif
