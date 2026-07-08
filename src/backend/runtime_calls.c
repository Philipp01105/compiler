#include "runtime_calls.h"
#include "core_intrinsics.h"
#include <string.h>

static const RuntimeCall calls[] = {
#define CORE_CALL(source, link, count, result, a, b, c) {source, link, count},
    DMM_CORE_INTRINSICS(CORE_CALL)
#undef CORE_CALL
    {"strlen", "__dmm_rt_strlen", 1},
    {"io_strlen", "__dmm_rt_strlen", 1},
    {"strcmp", "__dmm_rt_strcmp", 2},
    {"strcpy", "__dmm_rt_strcpy", 2},
    {"strcat", "__dmm_rt_strcat", 2},
    {"strdup", "__dmm_rt_strdup", 1},
    {"malloc", "__dmm_rt_malloc", 1},
    {"scanfInt", "__dmm_rt_scan_int", 0},
    {"scanfChar", "__dmm_rt_scan_char", 0},
    {"scanfString", "__dmm_rt_scan_string", 0},
    {"io_int_to_str", "__dmm_rt_int_to_string", 3},
    {"io_str_to_int", "__dmm_rt_string_to_int", 1},
    {"read", "__dmm_rt_read_value", 2},
    {"sys_read", "__dmm_rt_sys_read", 3},
    {"sys_write", "__dmm_rt_sys_write", 3},
    {"sys_open", "__dmm_rt_sys_open", 3},
    {"sys_close", "__dmm_rt_sys_close", 1}
};
size_t runtime_call_count(void) { return sizeof(calls) / sizeof(calls[0]); }
const RuntimeCall *runtime_call_at(size_t index) { return index < runtime_call_count() ? &calls[index] : NULL; }

const RuntimeCall *runtime_call_find(const char *source_name) {
    for (size_t i = 0; i < runtime_call_count(); i++)
        if (strcmp(calls[i].source_name, source_name) == 0) return &calls[i];
    return NULL;
}
