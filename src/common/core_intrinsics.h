#ifndef DMM_CORE_INTRINSICS_H
#define DMM_CORE_INTRINSICS_H

#include "language_types.h"
#include <stddef.h>
#include <string.h>

/* Target-independent signatures shared by semantic analysis, IR and emission. */
typedef enum { CORE_VOID, CORE_INT, CORE_SIZE, CORE_OFFSET, CORE_BYTES, CORE_STRING } CoreValueKind;
typedef struct {
    const char *source_name;
    const char *link_name;
    size_t argument_count;
    CoreValueKind result;
    CoreValueKind arguments[3];
} CoreIntrinsic;

#define DMM_CORE_INTRINSICS(X) \
    X("__dmm_intrinsic_alloc", "__dmm_core_malloc", 1, CORE_BYTES, CORE_SIZE, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_release", "__dmm_core_free", 1, CORE_VOID, CORE_BYTES, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_null", "__dmm_core_null", 0, CORE_BYTES, CORE_VOID, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_offset", "__dmm_core_offset", 2, CORE_BYTES, CORE_BYTES, CORE_OFFSET, CORE_VOID) \
    X("__dmm_intrinsic_copy", "__dmm_core_copy", 3, CORE_VOID, CORE_BYTES, CORE_BYTES, CORE_SIZE) \
    X("__dmm_intrinsic_fill", "__dmm_core_fill", 3, CORE_VOID, CORE_BYTES, CORE_INT, CORE_SIZE) \
    X("__dmm_intrinsic_string_data", "__dmm_core_string_data", 1, CORE_BYTES, CORE_STRING, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_read", "__dmm_rt_sys_read", 3, CORE_OFFSET, CORE_INT, CORE_BYTES, CORE_SIZE) \
    X("__dmm_intrinsic_write", "__dmm_rt_sys_write", 3, CORE_OFFSET, CORE_INT, CORE_BYTES, CORE_SIZE) \
    X("__dmm_intrinsic_open", "__dmm_rt_sys_open", 3, CORE_INT, CORE_STRING, CORE_INT, CORE_INT) \
    X("__dmm_intrinsic_close", "__dmm_rt_sys_close", 1, CORE_INT, CORE_INT, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_exit", "__dmm_core_exit", 1, CORE_VOID, CORE_INT, CORE_VOID, CORE_VOID) \
    X("__dmm_intrinsic_trap", "__dmm_core_trap", 0, CORE_VOID, CORE_VOID, CORE_VOID, CORE_VOID)

static inline const CoreIntrinsic *core_intrinsic_find(const char *name) {
#define CORE_SIGNATURE(source, link, count, result, a, b, c) {source, link, count, result, {a, b, c}},
    static const CoreIntrinsic signatures[] = { DMM_CORE_INTRINSICS(CORE_SIGNATURE) };
#undef CORE_SIGNATURE
    for (size_t i = 0; i < sizeof(signatures) / sizeof(signatures[0]); ++i)
        if (strcmp(name, signatures[i].source_name) == 0) return &signatures[i];
    return NULL;
}

static inline DataType core_value_type(CoreValueKind kind) {
    return kind == CORE_BYTES ? TYPE_U8 : kind == CORE_STRING ? TYPE_STRING :
           kind == CORE_SIZE ? TYPE_USIZE : kind == CORE_OFFSET ? TYPE_ISIZE :
           kind == CORE_INT ? TYPE_INT : TYPE_VOID;
}

#endif
