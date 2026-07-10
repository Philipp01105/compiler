#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "native_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#define TokenType WindowsTokenType
#include <windows.h>
#undef TokenType
#else
#include <sys/mman.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "runtime check failed at %d\n", __LINE__); return 1; } } while (0)

typedef struct {
    NativeObject object;
    unsigned char *memory;
    size_t size, offsets[3];
} Image;

static int initialize(Image *image) {
#ifdef _WIN32
    TargetFormat target = TARGET_COFF;
#else
    TargetFormat target = TARGET_ELF;
#endif
    NativeObject *o = &image->object;
    native_define(o, "main", 1, 1);
    native_uint(o, 0xc3, 1);
    native_define(o, "__dmm_package_cleanup", 0, 1);
    native_uint(o, 0xc3, 1);
    if (!native_runtime_emit(o, target)) return 0;
    for (size_t i = 0; i < o->symbol_count; ++i) {
        NativeSymbol *s = &o->symbols[i];
        if (s->defined) continue;
#ifdef _WIN32
        const char *name = native_runtime_import(s->name, target);
        if (!name) return 0;
        FARPROC proc = GetProcAddress(GetModuleHandleA("kernel32.dll"), name);
        uintptr_t address = 0;
        if (!proc) return 0;
        memcpy(&address, &proc, sizeof(address));
        s->defined = 1; s->section = NATIVE_TEXT; s->offset = o->sections[NATIVE_TEXT].size;
        o->section = NATIVE_TEXT;
        native_uint(o, 0xb848, 2); native_uint(o, address, 8); native_uint(o, 0xe0ff, 2);
#else
        return 0;
#endif
    }
    for (size_t i = 0; i < 3; ++i) {
        image->size = (image->size + 15) & ~(size_t) 15;
        image->offsets[i] = image->size;
        image->size += o->sections[i].size;
    }
#ifdef _WIN32
    image->memory = VirtualAlloc(NULL, image->size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    image->memory = mmap(NULL, image->size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image->memory == MAP_FAILED) image->memory = NULL;
#endif
    if (!image->memory) return 0;
    for (size_t i = 0; i < 3; ++i)
        memcpy(image->memory + image->offsets[i], o->sections[i].data, o->sections[i].size);
    for (size_t i = 0; i < o->relocation_count; ++i) {
        NativeRelocation *r = &o->relocations[i];
        NativeSymbol *s = &o->symbols[r->symbol];
        unsigned char *patch = image->memory + image->offsets[r->section] + r->offset;
        uint64_t value = (uintptr_t)(image->memory + image->offsets[s->section] + s->offset) + (uint64_t) r->addend;
        size_t width = r->kind == NATIVE_ADDR64 ? 8 : 4;
        if (width == 4) value -= (uintptr_t) patch;
        for (size_t j = 0; j < width; ++j) patch[j] = (unsigned char) (value >> (j * 8));
    }
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), image->memory, image->size);
#endif
    return !o->failed;
}

static void *function(Image *image, const char *name) {
    for (size_t i = 0; i < image->object.symbol_count; ++i) {
        NativeSymbol *s = &image->object.symbols[i];
        if (!strcmp(s->name, name) && s->defined) return image->memory + image->offsets[s->section] + s->offset;
    }
    return NULL;
}

#define FUNCTION(type, var, name) type var; do { void *entry = function(&image, name); CHECK(entry); memcpy(&var, &entry, sizeof(var)); } while (0)

typedef int64_t (*Format)(char *, uint64_t);

typedef int64_t (*Parse)(const char *);

typedef void *(*Allocate)(uint64_t);

typedef void *(*Callocate)(uint64_t, uint64_t);

typedef void (*Release)(void *);

typedef int64_t (*Open)(const char *, int64_t, int64_t);

typedef int64_t (*IO)(int64_t, void *, int64_t);

typedef int64_t (*Close)(int64_t);

int main(int argc, char **argv) {
    CHECK(argc == 2);
    Image image = {0};
    CHECK(initialize(&image));
    FUNCTION(Format, floating, "__dmm_format_float");
    FUNCTION(Format, integer, "__dmm_format_integer");
    FUNCTION(Parse, parse, "__dmm_rt_string_to_int");
    FUNCTION(Allocate, allocate, "__dmm_core_malloc");
    FUNCTION(Callocate, callocate, "__dmm_core_calloc");
    FUNCTION(Release, release, "__dmm_core_free");
    FUNCTION(Open, open_file, "__dmm_rt_sys_open");
    FUNCTION(IO, read_file, "__dmm_rt_sys_read");
    FUNCTION(IO, write_file, "__dmm_rt_sys_write");
    FUNCTION(Close, close_file, "__dmm_rt_sys_close");
    char actual[384], expected[384];
    uint64_t bits = 0;
    const double cases[] = {
        0, -0.0, 2.5, -12.125, 0.0000005, 0.0000015, 1.9999995,
        0x1p-1074, 0x1.fffffffffffffp1023, INFINITY, -INFINITY, NAN, 0x1p-7
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        memcpy(&bits, &cases[i], 8);
        int length = snprintf(expected, sizeof(expected), "%.6f", cases[i]);
        CHECK(floating(actual, bits) == length && !strcmp(actual, expected));
    }
    bits = 17;
    for (size_t i = 0; i < 2000; ++i) {
        bits = bits * UINT64_C(6364136223846793005) + 1;
        if (((bits >> 52) & 2047) == 2047) continue;
        double value;
        memcpy(&value, &bits, 8);
        int length = snprintf(expected, sizeof(expected), "%.6f", value);
        CHECK(floating(actual, bits) == length && !strcmp(actual, expected));
    }
    const int64_t integers[] = {0, 1, -1, INT64_MIN, INT64_MAX, 123456789, -987654321};
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); ++i) {
        int length = snprintf(expected, sizeof(expected), "%lld", (long long) integers[i]);
        CHECK(integer(actual, (uint64_t)integers[i]) == length && !strcmp(actual, expected));
        CHECK(parse(actual) == integers[i]);
    }
    CHECK(parse(NULL) == 0 && parse(" \t-42tail") == -42 && parse("invalid") == 0);
    CHECK(parse("9223372036854775808") == INT64_MAX && parse("-9223372036854775809") == INT64_MIN);
    const uint64_t sizes[] = {0, 1, 15, 16, 4095, 4096, 65536};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        unsigned char *p = allocate(sizes[i]);
        CHECK(p && !((uintptr_t)p & 15));
        if (sizes[i]) {
            p[0] = 1;
            p[sizes[i] - 1] = 2;
        }
        release(p);
    }
    release(NULL);
    CHECK(!allocate(UINT64_MAX) && !allocate(INT64_MAX) && !callocate(INT64_MAX, 2));
    unsigned char *p = callocate(19, 17);
    CHECK(p);
    for (size_t i = 0; i < 19 * 17; ++i)
        CHECK(p[i] == 0);
    release(p);
    int64_t fd = open_file(argv[1], 577, 384);
    CHECK(fd >= 0 && write_file(fd, "123", 3) == 3 && close_file(fd) == 0);
    fd = open_file(argv[1], 1089, 384);
    CHECK(fd >= 0 && write_file(fd, "45", 2) == 2 && close_file(fd) == 0);
    fd = open_file(argv[1], 0, 0);
    CHECK(fd >= 0 && read_file(fd, actual, 6) == 5 && !memcmp(actual, "12345", 5) && close_file(fd) == 0);
    CHECK(read_file(-1, actual, 1) < 0 && write_file(-1, actual, 1) < 0 && close_file(-1) < 0);
    CHECK(read_file(0, actual, -1) == -22);
#ifdef _WIN32
    VirtualFree(image.memory, 0, MEM_RELEASE);
#else
    munmap(image.memory, image.size);
#endif
    native_object_free(&image.object);
    return 0;
}
