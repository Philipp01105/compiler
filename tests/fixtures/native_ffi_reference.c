#ifndef _WIN32
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#else
#include <windows.h>
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t a, b, c;
} Tiny;

typedef struct {
    int32_t x;
    double y;
} Mixed;

typedef struct {
    double y;
    int32_t x;
} Reverse;

typedef struct {
    float a, b, c;
} Floats;

typedef struct {
    uint64_t a, b;
} Pair;

typedef struct {
    Pair pairs[2];
    Tiny tiny[2];
    uint8_t end;
} Large;

typedef struct {
    uint8_t tag;
    uint32_t count;
    uint8_t bytes[3];
} Record;

typedef struct {
    float value;
} SingleFloat;

typedef struct {
    double value;
} SingleDouble;

typedef struct {
    uint16_t value;
} SingleWord;

SingleFloat ffi_single_float(SingleFloat value) {
    value.value += 1.25f;
    return value;
}

SingleDouble ffi_single_double(SingleDouble value) {
    value.value += 2.5;
    return value;
}

SingleWord ffi_single_word(SingleWord value) {
    value.value += 100;
    return value;
}

int8_t ffi_i8(void) { return -117; }
uint8_t ffi_u8(void) { return 241; }
int16_t ffi_i16(void) { return -30123; }
uint16_t ffi_u16(void) { return 60123; }
int32_t ffi_i32(void) { return -1901234567; }
uint32_t ffi_u32(void) { return UINT32_C(3901234567); }
bool ffi_bool(bool value) { return !value; }
float ffi_float(float value) { return value + 1.5f; }

double ffi_mixed(int8_t a, double b, uint16_t c, float d, int32_t e, double f,
                 uint64_t g, float h, int64_t i, double j, uint32_t k, double l,
                 double m, double n, double o, double p, double q) {
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q;
}

void ffi_write(uint32_t *value) { *value = 123456789; }
void *ffi_identity(void *value) { return value; }

Tiny ffi_tiny(Tiny value) {
    value.a += 1;
    value.b += 2;
    value.c += 3;
    return value;
}

Mixed ffi_transform(Mixed value, int32_t delta, double weight) {
    value.x += delta;
    value.y += weight;
    return value;
}

Reverse ffi_reverse(Reverse value) {
    value.x += 7;
    value.y += 2.5;
    return value;
}

Floats ffi_floats(Floats value) {
    value.a += 1;
    value.b += 2;
    value.c += 3;
    return value;
}

Pair ffi_pair(Pair value) {
    value.a += 11;
    value.b += 13;
    return value;
}

Large ffi_large(Large value, int32_t delta) {
    value.pairs[1].b += delta;
    value.tiny[1].c += 3;
    value.end += 1;
    return value;
}

int64_t ffi_exhaust(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, Pair pair, int64_t last) {
    return a + b + c + d + e + pair.a + pair.b + last;
}

double ffi_sse_exhaust(double a, double b, double c, double d, double e, double f, double g,
                       Mixed value, double h) { return a + b + c + d + e + f + g + value.x + value.y + h; }

Record ffi_record(Record value) {
    value.tag += 1;
    value.count += 2;
    value.bytes[2] += 3;
    return value;
}

uint64_t ffi_layout(uint32_t which) {
    switch (which) {
        case 0: return sizeof(Record);
        case 1: return _Alignof(Record);
        case 2: return offsetof(Record, count);
        case 3: return offsetof(Record, bytes);
        case 4: return sizeof(Large);
        case 5: return offsetof(Large, end);
        default: return 0;
    }
}

/* A pointer to a tiny C object must not be overread by DMM copies. */
const Tiny *ffi_tiny_address(void) {
    static Tiny *tiny;
    if (!tiny) {
#ifdef _WIN32
        SYSTEM_INFO info; GetSystemInfo(&info);
        size_t page = info.dwPageSize;
        unsigned char *memory = VirtualAlloc(NULL, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        if (!memory || !VirtualProtect(memory + page, page, PAGE_NOACCESS, &old)) return NULL;
#else
        size_t page = (size_t) sysconf(_SC_PAGESIZE);
        unsigned char *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED || mprotect(memory + page, page, PROT_NONE)) return NULL;
#endif
        tiny = (Tiny *) (memory + page - sizeof(Tiny));
        *tiny = (Tiny){11, 22, 33};
    }
    return tiny;
}

int32_t ffi_mutate(Tiny *value) {
    value->a = 99;
    return 5;
}

int32_t ffi_snapshot(Tiny value, int32_t delta) { return value.a + delta; }

/* Check argument evaluation order and memory invalidation. */
static int32_t sequence;
int32_t ffi_next(void) { return ++sequence; }
int32_t ffi_order(int32_t a, int32_t b, int32_t c) { return a * 100 + b * 10 + c; }
