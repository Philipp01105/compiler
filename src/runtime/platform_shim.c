#include "platform_shim.h"
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#if !defined(__MINGW32__) || !defined(_UCRT)
#error The platform shim requires MinGW-w64 UCRT64
#endif
#else
#include <pthread.h>
#endif

typedef struct {
    DmmPlatformThreadCallback callback;
    void *argument;
#ifdef _WIN32
    HANDLE handle;
#else
    pthread_t handle;
#endif
} PlatformThread;

static void *allocate(size_t size) {
    void *value = calloc(1, size);
    if (!value) abort();
    return value;
}
#ifdef _WIN32
static unsigned __stdcall run_thread(void *pointer) {
    PlatformThread *thread = pointer;
    thread->callback(thread->argument);
    return 0; /* _beginthreadex performs CRT thread cleanup on return. */
}
#else
static void *run_thread(void *pointer) {
    PlatformThread *thread = pointer;
    thread->callback(thread->argument);
    return NULL;
}
#endif

void *__dmm_async_thread_create(DmmPlatformThreadCallback callback, void *argument) {
    PlatformThread *thread = allocate(sizeof(*thread));
    thread->callback = callback;
    thread->argument = argument;
#ifdef _WIN32
    thread->handle = (HANDLE)_beginthreadex(NULL, 0, run_thread, thread, 0, NULL);
    if (!thread->handle) abort();
#else
    if (pthread_create(&thread->handle, NULL, run_thread, thread)) abort();
#endif
    return thread;
}
void __dmm_async_thread_join(void *pointer) {
    PlatformThread *thread = pointer;
#ifdef _WIN32
    if (WaitForSingleObject(thread->handle, INFINITE) != WAIT_OBJECT_0) abort();
    if (!CloseHandle(thread->handle)) abort();
#else
    if (pthread_join(thread->handle, NULL)) abort();
#endif
    free(thread);
}

#ifdef _WIN32
void *__dmm_async_wait_create(void) {
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!event) abort();
    return event;
}
void __dmm_async_wait(void *event) {
    if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) abort();
}
void __dmm_async_wake(void *event) { if (!SetEvent(event)) abort(); }
void __dmm_async_wait_reset(void *event) { if (!ResetEvent(event)) abort(); }
void __dmm_async_wait_destroy(void *event) { if (!CloseHandle(event)) abort(); }
#else
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int signalled;
} PlatformEvent;
void *__dmm_async_wait_create(void) {
    PlatformEvent *event = allocate(sizeof(*event));
    if (pthread_mutex_init(&event->mutex, NULL) || pthread_cond_init(&event->condition, NULL)) abort();
    return event;
}
void __dmm_async_wait(void *pointer) {
    PlatformEvent *event = pointer;
    if (pthread_mutex_lock(&event->mutex)) abort();
    while (!event->signalled)
        if (pthread_cond_wait(&event->condition, &event->mutex)) abort();
    if (pthread_mutex_unlock(&event->mutex)) abort();
}
void __dmm_async_wake(void *pointer) {
    PlatformEvent *event = pointer;
    if (pthread_mutex_lock(&event->mutex)) abort();
    event->signalled = 1;
    if (pthread_cond_broadcast(&event->condition) || pthread_mutex_unlock(&event->mutex)) abort();
}
void __dmm_async_wait_reset(void *pointer) {
    PlatformEvent *event = pointer;
    if (pthread_mutex_lock(&event->mutex)) abort();
    event->signalled = 0;
    if (pthread_mutex_unlock(&event->mutex)) abort();
}
void __dmm_async_wait_destroy(void *pointer) {
    PlatformEvent *event = pointer;
    if (pthread_cond_destroy(&event->condition) || pthread_mutex_destroy(&event->mutex)) abort();
    free(event);
}
#endif

_Noreturn void __dmm_platform_exit(int code) { _Exit(code); }
