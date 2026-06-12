#ifndef DMM_RUNTIME_CALLS_H
#define DMM_RUNTIME_CALLS_H
#include <stddef.h>

typedef struct {
    const char *source_name;
    const char *link_name;
    size_t argument_count;
} RuntimeCall;

const RuntimeCall *runtime_call_find(const char *source_name);

size_t runtime_call_count(void);

const RuntimeCall *runtime_call_at(size_t index);
#endif
