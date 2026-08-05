#ifndef DMM_NATIVE_OBJECT_H
#define DMM_NATIVE_OBJECT_H
#include <stddef.h>
#include <stdint.h>
#include "language_types.h"

typedef struct {
    unsigned char *data;
    size_t size, capacity;
} NativeBuffer;

typedef enum { NATIVE_TEXT, NATIVE_RODATA, NATIVE_DATA, NATIVE_PDATA, NATIVE_XDATA, NATIVE_SECTION_COUNT } NativeSection;

typedef enum { NATIVE_REL32, NATIVE_CALL32, NATIVE_ADDR64, NATIVE_ADDR32NB } NativeRelocKind;

typedef struct {
    char *name;
    size_t offset, size;
    int section, global, function, defined;
} NativeSymbol;

typedef struct {
    NativeSection section;
    size_t offset, symbol;
    NativeRelocKind kind;
    int64_t addend;
} NativeRelocation;

typedef struct {
    NativeBuffer sections[NATIVE_SECTION_COUNT];
    NativeSymbol *symbols;
    size_t symbol_count, symbol_capacity;
    NativeRelocation *relocations;
    size_t relocation_count, relocation_capacity;
    NativeSection section;
    int failed;
    char error[256];
} NativeObject;

int native_buffer_bytes(NativeBuffer *buffer, const void *data, size_t size);

int native_buffer_uint(NativeBuffer *buffer, uint64_t value, size_t size);

int native_buffer_align(NativeBuffer *buffer, size_t alignment);

void native_buffer_patch(NativeBuffer *buffer, size_t offset, uint64_t value, size_t size);

void native_error(NativeObject *object, const char *message);

size_t native_symbol(NativeObject *object, const char *name);

int native_define(NativeObject *object, const char *name, int global, int function);

int native_bytes(NativeObject *object, const void *bytes, size_t size);

int native_uint(NativeObject *object, uint64_t value, size_t size);

int native_reference(NativeObject *object, const char *name, NativeRelocKind kind,
                     size_t offset, int64_t addend);

int native_write_object(NativeObject *object, TargetFormat target, NativeBuffer *output);

int native_validate(NativeObject *object);

void native_object_free(NativeObject *object);
#endif
