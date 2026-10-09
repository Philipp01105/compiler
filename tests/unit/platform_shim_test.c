#include "platform_shim.h"
#include "runtime_profile.h"
#include "ir.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "platform check failed at %d\n", __LINE__); return 1; } } while (0)
static _Thread_local int thread_value;

typedef struct {
    void *ready, *release;
    int value;
    atomic_int *completed;
} Worker;

static void worker(void *pointer) {
    Worker *work = pointer;
    thread_value = work->value;
    errno = work->value;
    __dmm_async_wake(work->ready);
    __dmm_async_wait(work->release);
    if (thread_value != work->value || errno != work->value) abort();
    atomic_fetch_add(work->completed, 1);
}

int main(void) {
    for (unsigned requirements = 0; requirements < 8; ++requirements) {
        RuntimeProfile profile = runtime_profile_for(requirements);
        CHECK(profile == (requirements ? RUNTIME_PLATFORM : RUNTIME_STANDALONE));
        for (LinkMode mode = LINK_AUTO; mode <= LINK_EXTERNAL; ++mode) {
            LinkMode resolved = LINK_AUTO;
            int supported = runtime_resolve_link(mode, profile, &resolved);
            CHECK(supported == !(mode == LINK_INTERNAL && profile == RUNTIME_PLATFORM));
            if (supported)
                CHECK(resolved == (mode == LINK_AUTO
                ? (profile == RUNTIME_PLATFORM ? LINK_EXTERNAL : LINK_INTERNAL) : mode));
        }
    }
    CHECK(!(runtime_requirements_normalize(RUNTIME_REQUIRE_PLATFORM) & RUNTIME_REQUIRE_NETWORK));
    IrInstruction operations[2] = {{.opcode = IR_OP_AWAIT}, {.opcode = IR_OP_CALL}};
    IrFunction function = {.instructions = operations, .instruction_count = 2};
    IrModule module = {.functions = &function, .function_count = 1};
    CHECK(ir_runtime_requirements(&module) == 0);
    operations[1].runtime_requirements = RUNTIME_REQUIRE_NETWORK;
    CHECK(ir_runtime_requirements(&module) == (RUNTIME_REQUIRE_PLATFORM | RUNTIME_REQUIRE_NETWORK |
        RUNTIME_REQUIRE_EXECUTOR));
    function.instruction_count = 1;
    CHECK(ir_runtime_requirements(&module) == 0);
    void *event = __dmm_async_wait_create();
    __dmm_async_wake(event);
    __dmm_async_wait(event); /* Signal before wait is retained. */
    __dmm_async_wait(event); /* Manual reset, not consumed by waiting. */
    __dmm_async_wait_reset(event);
    atomic_int completed = 0;
    Worker work[2];
    void *threads[2];
    thread_value = 99;
    errno = 99;
    for (int i = 0; i < 2; ++i) {
        work[i] = (Worker){__dmm_async_wait_create(), event, i + 10, &completed};
        threads[i] = __dmm_async_thread_create(worker, &work[i]);
    }
    for (int i = 0; i < 2; ++i) __dmm_async_wait(work[i].ready);
    CHECK(atomic_load(&completed) == 0);
    __dmm_async_wake(event);
    for (int i = 0; i < 2; ++i) {
        __dmm_async_thread_join(threads[i]);
        __dmm_async_wait_destroy(work[i].ready);
    }
    CHECK(atomic_load(&completed) == 2);
    CHECK(thread_value == 99 && errno == 99);
    __dmm_async_wait_destroy(event);
    return 0;
}
