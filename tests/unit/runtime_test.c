#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "native_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
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
    native_define(o, "__dmm_package_init", 0, 1);
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

typedef uint64_t (*ThreadCallback)(void *);
typedef void *(*ThreadCreate)(ThreadCallback, void *);
typedef void *(*WaitCreate)(void);
typedef void (*WaitAction)(void *);
typedef struct {
    void *entered, *proceed;
    WaitAction wait, wake;
    atomic_uint finished;
} ThreadJob;
/* This callback intentionally needs no libc or TLS: the ELF primitive creates
   a raw native thread, just as generated standalone code will. */
static uint64_t native_thread_callback(void *pointer) {
    ThreadJob *job = pointer;
    job->wake(job->entered);
    job->wait(job->proceed);
    atomic_fetch_add_explicit(&job->finished, 1, memory_order_seq_cst);
    return 0;
}

typedef struct NativeFrame NativeFrame;
struct NativeFrame {
    uint64_t (*poll)(NativeFrame *,void *);
    void (*destroy)(NativeFrame *,uint64_t);
    int64_t state;
    uint64_t (*cancel_poll)(NativeFrame *,void *);
    void (*result_drop)(void *);
    void *context;
    uint64_t result_present,cancelling;
    void *auxiliary;
    uint64_t size,result[2];
};
typedef void *(*ExecutorCreate)(uint64_t);
typedef NativeFrame *(*Spawn)(void *,NativeFrame *,uint64_t);
typedef NativeFrame *(*Shutdown)(void *,uint64_t);
typedef NativeFrame *(*FutureOperation)(NativeFrame *);
typedef void *(*IoCreate)(void *,Release);
typedef uint64_t (*IoPoll)(void *,void *);
typedef uint64_t (*IoQuery)(void *);
typedef struct {
    atomic_uint entered, polls, cancelling, destroyed, discarded, in_poll, overlap, confirmed;
    atomic_uintptr_t waker;
    WaitAction retain,release,wake;
    Release free_frame;
    unsigned delay;
    void *entered_event,*cancel_event;
    WaitAction event_wake;
    void *io;
    IoPoll io_poll;
    WaitAction io_request,io_destroy;
    atomic_uint resources_released;
    void *proceed_event;
    WaitAction event_wait;
} NativeProbe;
static uint64_t native_probe_poll(NativeFrame *f,void *context) {
    NativeProbe *p=f->auxiliary;
    if(atomic_fetch_add(&p->in_poll,1)) atomic_store(&p->overlap,1);
    unsigned poll=atomic_fetch_add(&p->polls,1);
    if(!atomic_load(&p->waker)) {
        p->retain(context);
        atomic_store(&p->waker,(uintptr_t)context);
    }
    atomic_store(&p->entered,1);
    if(p->entered_event) p->event_wake(p->entered_event);
    if(p->proceed_event) p->event_wait(p->proceed_event);
    if(poll<p->delay) {
        /* Deliberately wake before returning Pending. */
        p->wake(context);
        atomic_fetch_sub(&p->in_poll,1);
        return 0;
    }
    f->result[0]=42; f->state=-1; f->result_present=1;
    atomic_fetch_sub(&p->in_poll,1);
    return 1;
}
static uint64_t native_probe_pending(NativeFrame *f,void *context) {
    NativeProbe *p=f->auxiliary;
    if(p->io) (void)p->io_poll(p->io,context);
    if(!atomic_load(&p->waker)) {
        p->retain(context); atomic_store(&p->waker,(uintptr_t)context);
    }
    atomic_fetch_add(&p->polls,1); atomic_store(&p->entered,1);
    if(p->entered_event) p->event_wake(p->entered_event);
    return 0;
}
static uint64_t native_probe_cancel(NativeFrame *f,void *context) {
    NativeProbe *p=f->auxiliary;
    if(!atomic_load(&p->waker)) {
        p->retain(context); atomic_store(&p->waker,(uintptr_t)context);
    }
    atomic_store(&p->cancelling,1);
    if(p->io) p->io_request(p->io);
    if(p->cancel_event) p->event_wake(p->cancel_event);
    if(p->io ? !p->io_poll(p->io,context):!atomic_load(&p->confirmed)) return 0;
    f->state=-2; return 1;
}
static void native_probe_destroy(NativeFrame *f,uint64_t discard) {
    NativeProbe *p=f->auxiliary;
    if(discard && f->result_present) atomic_fetch_add(&p->discarded,1);
    atomic_fetch_add(&p->destroyed,1);
    if(p->io) p->io_destroy(p->io);
    p->free_frame(f);
}
static void native_probe_release(NativeProbe *p) {
    uintptr_t ctx=atomic_exchange(&p->waker,0);
    p->release((void *)ctx);
}
static void native_probe_resources_release(void *resource) {
    NativeProbe *p=resource;
    atomic_fetch_add(&p->resources_released,1);
}

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
    FUNCTION(ThreadCreate, create_thread, "__dmm_async_thread_create");
    FUNCTION(WaitAction, join_thread, "__dmm_async_thread_join");
    FUNCTION(WaitCreate, create_wait, "__dmm_async_wait_create");
    FUNCTION(WaitAction, wait_event, "__dmm_async_wait");
    FUNCTION(WaitAction, wake_event, "__dmm_async_wake");
    FUNCTION(WaitAction, reset_event, "__dmm_async_wait_reset");
    FUNCTION(WaitAction, destroy_event, "__dmm_async_wait_destroy");
    ThreadJob job = {.entered=create_wait(), .proceed=create_wait(), .wait=wait_event, .wake=wake_event};
    atomic_init(&job.finished, 0);
    CHECK(job.entered && job.proceed);
    for (unsigned round=0; round<20; round++) {
        reset_event(job.entered);
        reset_event(job.proceed);
        void *thread = create_thread(native_thread_callback, &job);
        CHECK(thread);
        wait_event(job.entered);
        CHECK(atomic_load(&job.finished)==round);
        wake_event(job.proceed);
        join_thread(thread);
        CHECK(atomic_load(&job.finished)==round+1);
        /* A signal preceding the wait must remain observable. */
        wait_event(job.proceed);
    }
    destroy_event(job.entered);
    destroy_event(job.proceed);
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
    FUNCTION(ExecutorCreate,executor_create,"__dmm_async_executor_create");
    FUNCTION(Spawn,spawn_future,"__dmm_async_spawn");
    FUNCTION(Shutdown,shutdown_executor,"__dmm_async_shutdown");
    FUNCTION(FutureOperation,block_on_future,"__dmm_async_block_on");
    FUNCTION(FutureOperation,cancel_future,"__dmm_async_cancel");
    FUNCTION(WaitAction,context_retain,"__dmm_async_context_retain");
    FUNCTION(WaitAction,context_release,"__dmm_async_context_release");
    FUNCTION(WaitAction,context_wake,"__dmm_async_context_wake");
    FUNCTION(IoCreate,io_create,"__dmm_async_io_create");
    FUNCTION(IoPoll,io_poll,"__dmm_async_io_poll");
    FUNCTION(WaitAction,io_request,"__dmm_async_io_request_cancel");
    FUNCTION(IoQuery,io_requested,"__dmm_async_io_cancel_requested");
    FUNCTION(WaitAction,io_confirm,"__dmm_async_io_confirm");
    FUNCTION(WaitAction,io_destroy,"__dmm_async_io_destroy");
    for(unsigned round=0;round<20;round++) {
        void *executor=executor_create(2);
        NativeProbe probes[8]={0}; NativeFrame *handles[8];
        for(unsigned i=0;i<8;i++) {
            NativeProbe *probe=&probes[i];
            probe->retain=context_retain; probe->release=context_release; probe->wake=context_wake;
            probe->free_frame=release; probe->delay=30;
            NativeFrame *frame=callocate(1,sizeof(*frame)); CHECK(frame);
            frame->poll=native_probe_poll; frame->cancel_poll=native_probe_cancel;
            frame->destroy=native_probe_destroy; frame->auxiliary=probe;
            handles[i]=spawn_future(executor,frame,8); CHECK(handles[i]);
        }
        NativeFrame *shutdown=shutdown_executor(executor,0);
        CHECK(block_on_future(shutdown)==shutdown); shutdown->destroy(shutdown,0);
        for(unsigned i=0;i<8;i++) {
            NativeFrame *handle=handles[i]; CHECK(block_on_future(handle)==handle);
            CHECK(handle->result[0]==0 && handle->result[1]==42);
            handle->destroy(handle,0);
            CHECK(atomic_load(&probes[i].destroyed)==1 && !atomic_load(&probes[i].overlap));
            native_probe_release(&probes[i]);
        }
    }
    /* The task and frame stay alive until the private adapter confirms I/O
       termination. A wake after Pending schedules the cancellation poll. */
    void *executor=executor_create(2);
    NativeProbe probe={0}; probe.retain=context_retain; probe.release=context_release;
    probe.wake=context_wake; probe.free_frame=release;
    probe.entered_event=create_wait(); probe.cancel_event=create_wait(); probe.event_wake=wake_event;
    probe.io=io_create(&probe,native_probe_resources_release); CHECK(probe.io);
    probe.io_poll=io_poll; probe.io_request=io_request; probe.io_destroy=io_destroy;
    NativeFrame *frame=callocate(1,sizeof(*frame)); CHECK(frame);
    frame->poll=native_probe_pending; frame->cancel_poll=native_probe_cancel;
    frame->destroy=native_probe_destroy; frame->auxiliary=&probe;
    NativeFrame *handle=spawn_future(executor,frame,8);
    wait_event(probe.entered_event);
    NativeFrame *cancellation=cancel_future(handle);
    CHECK(!cancellation->poll(cancellation,NULL));
    wait_event(probe.cancel_event);
    CHECK(!atomic_load(&probe.destroyed));
    CHECK(io_requested(probe.io) && !atomic_load(&probe.resources_released));
    io_confirm(probe.io);
    CHECK(block_on_future(cancellation)==cancellation); cancellation->destroy(cancellation,0);
    CHECK(atomic_load(&probe.destroyed)==1 && !atomic_load(&probe.discarded) && atomic_load(&probe.resources_released)==1);
    native_probe_release(&probe);
    destroy_event(probe.entered_event); destroy_event(probe.cancel_event);
    NativeFrame *shutdown=shutdown_executor(executor,1);
    CHECK(block_on_future(shutdown)==shutdown); shutdown->destroy(shutdown,0);
    /* Two simultaneous polls must enter before either can leave. Request
       cancellation of one while its synchronous code is still running. */
    executor=executor_create(2);
    NativeProbe parallel[2]={0}; NativeFrame *parallel_handles[2];
    for(unsigned i=0;i<2;i++) {
        NativeProbe *p_probe=&parallel[i];
        p_probe->retain=context_retain; p_probe->release=context_release; p_probe->wake=context_wake;
        p_probe->free_frame=release; p_probe->event_wake=wake_event; p_probe->event_wait=wait_event;
        p_probe->entered_event=create_wait(); p_probe->proceed_event=create_wait();
        frame=callocate(1,sizeof(*frame)); CHECK(frame); frame->poll=native_probe_poll;
        frame->cancel_poll=native_probe_cancel; frame->destroy=native_probe_destroy; frame->auxiliary=p_probe;
        parallel_handles[i]=spawn_future(executor,frame,8);
    }
    wait_event(parallel[0].entered_event); wait_event(parallel[1].entered_event);
    cancellation=cancel_future(parallel_handles[0]); CHECK(!cancellation->poll(cancellation,NULL));
    CHECK(!atomic_load(&parallel[0].destroyed));
    wake_event(parallel[0].proceed_event); wake_event(parallel[1].proceed_event);
    CHECK(block_on_future(cancellation)==cancellation); cancellation->destroy(cancellation,0);
    CHECK(atomic_load(&parallel[0].discarded)==1 && atomic_load(&parallel[0].destroyed)==1);
    handle=parallel_handles[1]; CHECK(block_on_future(handle)==handle);
    CHECK(handle->result[0]==0 && handle->result[1]==42); handle->destroy(handle,0);
    shutdown=shutdown_executor(executor,0); CHECK(block_on_future(shutdown)==shutdown); shutdown->destroy(shutdown,0);
    for(unsigned i=0;i<2;i++) {
        CHECK(!atomic_load(&parallel[i].overlap)); native_probe_release(&parallel[i]);
        destroy_event(parallel[i].entered_event); destroy_event(parallel[i].proceed_event);
    }
    /* Shutdown Cancel closes admission but preserves an outstanding join.
       It waits for the I/O confirmation before releasing the frame. */
    executor=executor_create(2); NativeProbe shutdown_probe={0};
    shutdown_probe.retain=context_retain; shutdown_probe.release=context_release;
    shutdown_probe.wake=context_wake; shutdown_probe.free_frame=release; shutdown_probe.event_wake=wake_event;
    shutdown_probe.entered_event=create_wait(); shutdown_probe.cancel_event=create_wait();
    shutdown_probe.io=io_create(&shutdown_probe,native_probe_resources_release);
    shutdown_probe.io_poll=io_poll; shutdown_probe.io_request=io_request; shutdown_probe.io_destroy=io_destroy;
    frame=callocate(1,sizeof(*frame)); CHECK(frame); frame->poll=native_probe_pending;
    frame->cancel_poll=native_probe_cancel; frame->destroy=native_probe_destroy; frame->auxiliary=&shutdown_probe;
    handle=spawn_future(executor,frame,8); wait_event(shutdown_probe.entered_event);
    shutdown=shutdown_executor(executor,1); wait_event(shutdown_probe.cancel_event);
    CHECK(!shutdown->poll(shutdown,NULL) && !atomic_load(&shutdown_probe.destroyed));
    io_confirm(shutdown_probe.io);
    CHECK(block_on_future(shutdown)==shutdown); shutdown->destroy(shutdown,0);
    CHECK(block_on_future(handle)==handle && handle->result[0]==1 && handle->result[1]==0);
    handle->destroy(handle,0); CHECK(atomic_load(&shutdown_probe.destroyed)==1 && atomic_load(&shutdown_probe.resources_released)==1);
    native_probe_release(&shutdown_probe); destroy_event(shutdown_probe.entered_event); destroy_event(shutdown_probe.cancel_event);
#ifdef _WIN32
    VirtualFree(image.memory, 0, MEM_RELEASE);
#else
    munmap(image.memory, image.size);
#endif
    native_object_free(&image.object);
    return 0;
}
