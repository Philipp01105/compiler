#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

typedef struct {
    int32_t integer;
    double floating;
} Mixed;

typedef struct {
    uint64_t words[3];
} Large;

typedef union {
    uint64_t word;
    double floating;
    uint8_t bytes[8];
} Overlay;
#pragma pack(push, 1)
typedef struct {
    uint8_t byte;
    uint64_t word;
} Packed;
#pragma pack(pop)
typedef struct __attribute__ ((aligned(16))) { uint64_t word; }
Aligned;

int32_t callback_scalar(int32_t x) { return x + 1; }
int32_t (*callback_address(void))(int32_t) { return callback_scalar; }
int32_t callback_invoke(int32_t(*f)(int32_t)) { return f(-10) == -3; }

int32_t callback_mixed(Mixed(*f)(Mixed, double, int32_t)) {
    Mixed result = f((Mixed){7, 1.25}, 2.5, -4);
    return result.integer == 3 && result.floating == 3.75;
}

int32_t callback_large(Large (*f)(Large)) {
    Large result = f((Large){{10, 20, 30}});
    return result.words[0] == 11 && result.words[1] == 22 && result.words[2] == 33;
}

int32_t callback_exhaust(int64_t(*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t)) {
    return f(1, 2, 3, 4, 5, 6, 7, 8) == 36;
}

Overlay callback_overlay(Overlay value) {
    value.word += 2;
    return value;
}

Packed callback_packed(Packed value) {
    value.byte += 1;
    value.word += 2;
    return value;
}

int32_t callback_layout(uint64_t union_size, uint64_t packed_size, uint64_t packed_align,
                        uint64_t aligned_size, uint64_t aligned_align, const Aligned *address) {
    return union_size == sizeof(Overlay) && packed_size == sizeof(Packed) &&
           packed_align == _Alignof(Packed) && aligned_size == sizeof(Aligned) &&
           aligned_align == _Alignof(Aligned) && ((uintptr_t) address % _Alignof(Aligned)) == 0;
}

typedef struct {
    int32_t value, result;
} Context;
#ifdef _WIN32
int32_t callback_threads(unsigned (*f)(void *)) {
    HANDLE threads[4];
    Context contexts[4];
    unsigned created = 0;
    for (unsigned i = 0; i < 4; ++i) {
        contexts[i] = (Context){(int32_t) i + 1, 0};
        threads[i] = (HANDLE) _beginthreadex(NULL, 0, f, &contexts[i], 0, NULL);
        if (!threads[i]) break;
        ++created;
    }
    if (created) WaitForMultipleObjects(created, threads, TRUE, INFINITE);
    int32_t valid = created == 4;
    for (unsigned i = 0; i < created; ++i) {
        DWORD result = 0;
        valid &= GetExitCodeThread(threads[i], &result) && result == 17 && contexts[i].result == contexts[i].value * 3;
        CloseHandle(threads[i]);
    }
    return valid;
}
/* Test OS unwinding from a nested DMM call with outgoing stack arguments. */
int32_t callback_unwind(void) {
    CONTEXT context;
    RtlCaptureContext(&context);
    unsigned dmm_frames = 0;
    for (unsigned i = 0; i < 32 && context.Rip; ++i) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image, NULL);
        if (!entry) {
            context.Rip = *(DWORD64 *) (uintptr_t) context.Rsp;
            context.Rsp += 8;
            continue;
        }
        if (*(unsigned char *) (uintptr_t)(image + entry->BeginAddress) == 0x55) ++dmm_frames;
        PVOID data = NULL;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &data, &establisher, NULL);
        if (dmm_frames >= 3) return 1;
    }
    return 0;
}
#else
int32_t callback_threads(void *(*f)(void *)) {
    pthread_t threads[4];
    Context contexts[4];
    unsigned created = 0;
    for (unsigned i = 0; i < 4; ++i) {
        contexts[i] = (Context){(int32_t) i + 1, 0};
        if (pthread_create(&threads[i], NULL, f, &contexts[i])) break;
        ++created;
    }
    int32_t valid = created == 4;
    for (unsigned i = 0; i < created; ++i) {
        void *result = NULL;
        valid &= !pthread_join(threads[i], &result) && result == &contexts[i] && contexts[i].result == contexts[i].value
                * 3;
    }
    return valid;
}

int32_t callback_unwind(void) { return 1; }
#endif
