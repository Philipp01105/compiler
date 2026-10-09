#include "executor.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION Mutex;
typedef CONDITION_VARIABLE Condition;
typedef HANDLE Thread;
static void mutex_init(Mutex *m) { InitializeCriticalSection(m); }
static void mutex_drop(Mutex *m) { DeleteCriticalSection(m); }
static void lock(Mutex *m) { EnterCriticalSection(m); }
static void unlock(Mutex *m) { LeaveCriticalSection(m); }
static void condition_init(Condition *c) { InitializeConditionVariable(c); }
static void signal_all(Condition *c) { WakeAllConditionVariable(c); }
static void wait_for(Condition *c, Mutex *m) {
    if (!SleepConditionVariableCS(c, m, INFINITE)) abort();
}
static void thread_join(Thread t) {
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0 || !CloseHandle(t)) abort();
}
#else
#include <pthread.h>
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t Condition;
typedef pthread_t Thread;
static void mutex_init(Mutex *m) { if (pthread_mutex_init(m, NULL)) abort(); }
static void mutex_drop(Mutex *m) { if (pthread_mutex_destroy(m)) abort(); }
static void lock(Mutex *m) { if (pthread_mutex_lock(m)) abort(); }
static void unlock(Mutex *m) { if (pthread_mutex_unlock(m)) abort(); }
static void condition_init(Condition *c) { if (pthread_cond_init(c, NULL)) abort(); }
static void signal_all(Condition *c) { if (pthread_cond_broadcast(c)) abort(); }
static void wait_for(Condition *c, Mutex *m) { if (pthread_cond_wait(c, m)) abort(); }
static void thread_join(Thread t) { if (pthread_join(t, NULL)) abort(); }
#endif

struct DmmExecutor {
    atomic_size_t refs;
    Mutex mutex;
    Condition changed;
    Thread *workers;
    size_t worker_count, exited, active;
    int closed;
    DmmTask *head, *tail, *tasks;
    DmmWaker shutdown_waiter;
};

struct DmmTask {
    atomic_size_t refs;
    DmmExecutor *executor;
    DmmRuntimeFuture future;
    DmmTaskStatus status;
    int queued, running, notified, cancel_requested, cancel_handle;
    DmmTask *next_ready, *next_active, *previous_active;
    DmmWaker waiter;
};

struct DmmShutdown {
    DmmExecutor *executor;
    int joined;
};

struct DmmIoOperation {
    atomic_size_t refs;
    Mutex mutex;
    int cancel_requested, confirmed, owner_consumed;
    DmmWaker waiter;
};

static void *allocate(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) abort();
    return p;
}

static void require(int valid) { if (!valid) abort(); }

static void executor_release(DmmExecutor *e) {
    if (atomic_fetch_sub_explicit(&e->refs, 1, memory_order_acq_rel) != 1) return;
    require(e->closed && !e->active && e->exited == e->worker_count);
#ifndef _WIN32
    if (pthread_cond_destroy(&e->changed)) abort();
#endif
    mutex_drop(&e->mutex);
    free(e->workers);
    free(e);
}

static void task_release(DmmTask *t) {
    if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) != 1) return;
    require(t->status != DMM_PENDING && !t->future.frame && !t->waiter.task);
    DmmExecutor *e = t->executor;
    free(t);
    executor_release(e);
}

DmmWaker dmm_waker_clone(DmmWaker w) {
    if (w.task) atomic_fetch_add_explicit(&w.task->refs, 1, memory_order_relaxed);
    return w;
}

void dmm_waker_drop(DmmWaker w) { if (w.task) task_release(w.task); }

int dmm_poll_cancel_requested(const DmmPollContext *context) {
    if (!context) return 0;
    if (context->cancelling) return 1;
    if (!context->waker.task) return 0;
    DmmTask *t = context->waker.task;
    lock(&t->executor->mutex);
    int result = t->cancel_requested;
    unlock(&t->executor->mutex);
    return result;
}

/* All scheduling bits are protected by the executor lock. notified records a
   wake during a poll; removing an item from the queue never clears that bit
   after the callback has begun. There is exactly one scheduler reference. */
static void enqueue(DmmTask *t) {
    DmmExecutor *e = t->executor;
    if (t->queued || t->running || t->status != DMM_PENDING) return;
    t->queued = 1;
    t->next_ready = NULL;
    if (e->tail) e->tail->next_ready = t;
    else e->head = t;
    e->tail = t;
    signal_all(&e->changed);
}

void dmm_waker_wake(DmmWaker w) {
    if (!w.task) return;
    DmmTask *t = w.task;
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    if (t->status == DMM_PENDING) {
        t->notified = 1;
        enqueue(t);
    }
    unlock(&e->mutex);
}

/* Deliver outside the target lock: a waiter may belong to another executor. */
static void deliver(DmmWaker w) {
    dmm_waker_wake(w);
    dmm_waker_drop(w);
}

static DmmExecutor *executor_base(size_t workers) {
    DmmExecutor *e = allocate(1, sizeof(*e));
    atomic_init(&e->refs, 1);
    mutex_init(&e->mutex);
    condition_init(&e->changed);
    e->worker_count = workers;
    if (workers) e->workers = allocate(workers, sizeof(*e->workers));
    return e;
}

/* Must be called with the lock held; returns with the lock held. */
static void poll_next(DmmExecutor *e) {
    DmmTask *t = e->head;
    assert(t && !t->running && t->queued);
    e->head = t->next_ready;
    if (!e->head) e->tail = NULL;
    t->next_ready = NULL;
    t->queued = 0;
    t->running = 1;
    t->notified = 0;
    int cancelling = t->cancel_requested;
    DmmPollContext context = {{t}, cancelling};
    unlock(&e->mutex);
    int ready = cancelling
                    ? t->future.cancel_poll(t->future.frame, &context)
                    : t->future.poll(t->future.frame, &context);
    require(ready == 0 || ready == 1);
    lock(&e->mutex);
    if (!ready) {
        t->running = 0;
        /* Request arriving in normal poll must schedule the cleanup poll.
           Pending cancellation waits for its cleanup/I/O waker. */
        if (t->notified || (!cancelling && t->cancel_requested)) enqueue(t);
        return;
    }

    /* This lock serializes Ready and request_cancel. A request that won while
       poll was running discards the produced output exactly once. A request
       arriving after publication cannot change the completed result. */
    int cancelled = cancelling || t->cancel_requested;
    if (cancelled) {
        void *frame = t->future.frame;
        t->future.frame = NULL;
        unlock(&e->mutex);
        t->future.destroy(frame, !cancelling);
        lock(&e->mutex);
    }
    t->status = cancelled ? DMM_CANCELLED : DMM_READY;
    t->running = 0;
    if (t->previous_active) t->previous_active->next_active = t->next_active;
    else e->tasks = t->next_active;
    if (t->next_active) t->next_active->previous_active = t->previous_active;
    t->previous_active = t->next_active = NULL;
    assert(e->active);
    e->active--;
    DmmWaker waiter = t->waiter;
    t->waiter.task = NULL;
    signal_all(&e->changed);
    unlock(&e->mutex);
    deliver(waiter);
    task_release(t); /* scheduler ownership ends only after cleanup */
    lock(&e->mutex);
}

#ifdef _WIN32
static DWORD WINAPI worker(void *pointer) {

#else
static void *worker(void *pointer) {
#endif
    DmmExecutor *e = pointer;
    lock(&e->mutex);
    for (;;) {
        if (e->head) poll_next(e);
        else if (e->closed && !e->active) break;
        else wait_for(&e->changed, &e->mutex);
    }
    e->exited++;
    DmmWaker waiter = {NULL};
    if (e->exited == e->worker_count) {
        waiter = e->shutdown_waiter;
        e->shutdown_waiter.task = NULL;
    }
    signal_all(&e->changed);
    unlock(&e->mutex);
    deliver(waiter);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

DmmExecutor *dmm_executor_create(size_t workers) {
    if (!workers || workers > SIZE_MAX / sizeof(Thread)) abort();
    DmmExecutor *e = executor_base(workers);
    for (size_t i = 0; i < workers; i++) {
#ifdef _WIN32
        e->workers[i] = CreateThread(NULL, 0, worker, e, 0, NULL);
        if (!e->workers[i]) abort();
#else
        if (pthread_create(&e->workers[i], NULL, worker, e)) abort();
#endif
    }
    return e;
}

DmmTask *dmm_executor_spawn(DmmExecutor *e, DmmRuntimeFuture f) {
    require(e && f.frame && f.poll && f.cancel_poll && f.destroy);
    lock(&e->mutex);
    if (e->closed) {
        unlock(&e->mutex);
        return NULL;
    }
    DmmTask *t = allocate(1, sizeof(*t));
    atomic_init(&t->refs, 2); /* scheduler + consuming handle */
    atomic_fetch_add_explicit(&e->refs, 1, memory_order_relaxed);
    t->executor = e;
    t->future = f;
    t->next_active = e->tasks;
    if (e->tasks) e->tasks->previous_active = t;
    e->tasks = t;
    e->active++;
    enqueue(t);
    unlock(&e->mutex);
    return t;
}

static DmmTaskStatus poll_join(DmmTask *t, const DmmPollContext *context, int cancel) {
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    require(t->cancel_handle == cancel);
    DmmTaskStatus status = t->status;
    DmmWaker old = {NULL};
    if (status == DMM_PENDING && context && context->waker.task != t->waiter.task) {
        old = t->waiter;
        t->waiter = dmm_waker_clone(context->waker);
    }
    unlock(&e->mutex);
    dmm_waker_drop(old);
    return status;
}

DmmTaskStatus dmm_join_poll(DmmTask *t, const DmmPollContext *c) { return poll_join(t, c, 0); }
DmmTaskStatus dmm_cancel_poll(DmmTask *t, const DmmPollContext *c) { return poll_join(t, c, 1); }

DmmTaskStatus dmm_join_take(DmmTask *t, void *output) {
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    require(!t->cancel_handle && t->status != DMM_PENDING);
    DmmTaskStatus status = t->status;
    void *frame = t->future.frame;
    t->future.frame = NULL;
    unlock(&e->mutex);
    if (status == DMM_READY) {
        if (t->future.take_result) t->future.take_result(frame, output);
        t->future.destroy(frame, 0);
    }
    task_release(t);
    return status;
}

static void request_cancel(DmmTask *t) {
    if (t->status != DMM_PENDING) return;
    t->cancel_requested = 1;
    enqueue(t);
}

DmmTask *dmm_join_cancel(DmmTask *t) {
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    require(!t->cancel_handle);
    t->cancel_handle = 1;
    request_cancel(t);
    if (t->status == DMM_READY) {
        void *frame = t->future.frame;
        t->future.frame = NULL;
        /* No poll is possible after terminal publication. The handle is
           consumed, so there is no competing observer of its output. */
        unlock(&e->mutex);
        t->future.destroy(frame, 1);
        lock(&e->mutex);
    }
    unlock(&e->mutex);
    return t;
}

void dmm_cancel_finish(DmmTask *t) {
    lock(&t->executor->mutex);
    require(t->cancel_handle && t->status != DMM_PENDING && !t->future.frame);
    unlock(&t->executor->mutex);
    task_release(t);
}

DmmShutdown *dmm_executor_shutdown(DmmExecutor *e, DmmShutdownMode mode) {
    require(mode == DMM_DRAIN || mode == DMM_CANCEL);
    DmmShutdown *s = allocate(1, sizeof(*s));
    s->executor = e; /* transfer the executor owner's reference */
    lock(&e->mutex);
    require(!e->closed);
    e->closed = 1;
    if (mode == DMM_CANCEL)
        for (DmmTask *t = e->tasks; t; t = t->next_active) request_cancel(t);
    signal_all(&e->changed);
    unlock(&e->mutex);
    return s;
}

int dmm_shutdown_poll(DmmShutdown *s, const DmmPollContext *context) {
    DmmExecutor *e = s->executor;
    lock(&e->mutex);
    int ready = !e->active && e->exited == e->worker_count;
    DmmWaker old = {NULL};
    if (!ready && context && context->waker.task != e->shutdown_waiter.task) {
        old = e->shutdown_waiter;
        e->shutdown_waiter = dmm_waker_clone(context->waker);
    }
    unlock(&e->mutex);
    dmm_waker_drop(old);
    return ready;
}

void dmm_shutdown_finish(DmmShutdown *s) {
    require(dmm_shutdown_poll(s, NULL) && !s->joined);
    s->joined = 1;
    DmmExecutor *e = s->executor;
    for (size_t i = 0; i < e->worker_count; i++) thread_join(e->workers[i]);
    free(s);
    executor_release(e);
}

DmmTaskStatus dmm_join_block_on(DmmTask *t, void *output) {
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    require(!t->cancel_handle);
    while (t->status == DMM_PENDING) wait_for(&e->changed, &e->mutex);
    unlock(&e->mutex);
    return dmm_join_take(t, output);
}

void dmm_cancel_block_on(DmmTask *t) {
    DmmExecutor *e = t->executor;
    lock(&e->mutex);
    require(t->cancel_handle);
    while (t->status == DMM_PENDING) wait_for(&e->changed, &e->mutex);
    unlock(&e->mutex);
    dmm_cancel_finish(t);
}

void dmm_shutdown_block_on(DmmShutdown *s) {
    DmmExecutor *e = s->executor;
    lock(&e->mutex);
    while (e->active || e->exited != e->worker_count) wait_for(&e->changed, &e->mutex);
    unlock(&e->mutex);
    dmm_shutdown_finish(s);
}

static void block_future(DmmRuntimeFuture f, void *output, int cancel) {
    DmmExecutor *e = executor_base(0);
    DmmTask *t = dmm_executor_spawn(e, f);
    if (cancel) dmm_join_cancel(t);
    lock(&e->mutex);
    while (t->status == DMM_PENDING) {
        if (e->head) poll_next(e);
        else wait_for(&e->changed, &e->mutex);
    }
    e->closed = 1;
    unlock(&e->mutex);
    if (cancel) dmm_cancel_finish(t);
    else {
        require(t->status == DMM_READY);
        dmm_join_take(t, output);
    }
    executor_release(e);
}

void dmm_future_block_on(DmmRuntimeFuture f, void *out) { block_future(f, out, 0); }
void dmm_future_cancel_block_on(DmmRuntimeFuture f) { block_future(f, NULL, 1); }

typedef struct {
    DmmRuntimeFuture child;
} CancelFuture;

static int poll_cancel_future(void *pointer, const DmmPollContext *context) {
    CancelFuture *f = pointer;
    require(f->child.frame != NULL);
    DmmPollContext cleanup = *context;
    cleanup.cancelling = 1;
    int ready = f->child.cancel_poll(f->child.frame, &cleanup);
    require(ready == 0 || ready == 1);
    if (!ready) return 0;
    f->child.destroy(f->child.frame, 0);
    f->child.frame = NULL;
    return 1;
}

static void destroy_cancel_future(void *pointer, int discard) {
    CancelFuture *f = pointer;
    (void) discard; /* Future<void> has no output destructor */
    require(!f->child.frame);
    free(f);
}

DmmRuntimeFuture dmm_future_cancel(DmmRuntimeFuture child) {
    require(child.frame && child.poll && child.cancel_poll && child.destroy);
    CancelFuture *f = allocate(1, sizeof(*f));
    f->child = child;
    /* Cancelling cancellation must still finish the original cleanup. */
    return (DmmRuntimeFuture)
    {
        f, poll_cancel_future, poll_cancel_future, NULL, destroy_cancel_future
    };
}

typedef struct {
    DmmTask *task;
    int cancelling;
} JoinFuture;

static int poll_join_future(void *pointer, const DmmPollContext *context) {
    JoinFuture *f = pointer;
    require(f->task && !f->cancelling);
    return dmm_join_poll(f->task, context) != DMM_PENDING;
}

static int cancel_join_future(void *pointer, const DmmPollContext *context) {
    JoinFuture *f = pointer;
    require(f->task != NULL);
    if (!f->cancelling) {
        dmm_join_cancel(f->task);
        f->cancelling = 1;
    }
    if (dmm_cancel_poll(f->task, context) == DMM_PENDING) return 0;
    dmm_cancel_finish(f->task);
    f->task = NULL;
    return 1;
}

static void take_join_future(void *pointer, void *output) {
    JoinFuture *f = pointer;
    DmmJoinResult *result = output;
    require(result && f->task && !f->cancelling);
    result->status = dmm_join_take(f->task, result->value);
    f->task = NULL;
}

static void destroy_join_future(void *pointer, int discard) {
    JoinFuture *f = pointer;
    if (f->task) {
        require(discard && !f->cancelling && dmm_join_poll(f->task, NULL) != DMM_PENDING);
        dmm_cancel_finish(dmm_join_cancel(f->task));
    }
    free(f);
}

DmmRuntimeFuture dmm_join_as_future(DmmTask *task) {
    require(task != NULL);
    JoinFuture *f = allocate(1, sizeof(*f));
    f->task = task;
    return (DmmRuntimeFuture)
    {
        f, poll_join_future, cancel_join_future, take_join_future, destroy_join_future
    };
}

typedef struct {
    DmmShutdown *shutdown;
} ShutdownFuture;

static int poll_shutdown_future(void *pointer, const DmmPollContext *context) {
    ShutdownFuture *f = pointer;
    require(f->shutdown != NULL);
    if (!dmm_shutdown_poll(f->shutdown, context)) return 0;
    dmm_shutdown_finish(f->shutdown);
    f->shutdown = NULL;
    return 1;
}

static void destroy_shutdown_future(void *pointer, int discard) {
    ShutdownFuture *f = pointer;
    (void) discard;
    require(!f->shutdown);
    free(f);
}

DmmRuntimeFuture dmm_shutdown_as_future(DmmShutdown *shutdown) {
    require(shutdown != NULL);
    ShutdownFuture *f = allocate(1, sizeof(*f));
    f->shutdown = shutdown;
    /* A shutdown operation owns the executor and cannot abandon its workers. */
    return (DmmRuntimeFuture)
    {
        f, poll_shutdown_future, poll_shutdown_future, NULL, destroy_shutdown_future
    };
}

static DmmExecutor *default_executor;

static void drain_default(void) {
    dmm_shutdown_block_on(dmm_executor_shutdown(default_executor, DMM_DRAIN));
}

static void create_default(void) {
    default_executor = dmm_executor_create(2);
    if (atexit(drain_default)) abort();
}
#ifdef _WIN32
static INIT_ONCE default_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK default_init(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void) once;
    (void) parameter;
    (void) context;
    create_default();
    return TRUE;
}
#else
static pthread_once_t default_once = PTHREAD_ONCE_INIT;
#endif
DmmTask *dmm_spawn_default(DmmRuntimeFuture f) {
#ifdef _WIN32
    if (!InitOnceExecuteOnce(&default_once, default_init, NULL, NULL)) abort();
#else
    if (pthread_once(&default_once, create_default)) abort();
#endif
    return dmm_executor_spawn(default_executor, f);
}

DmmIoOperation *dmm_io_create(void) {
    DmmIoOperation *io = allocate(1, sizeof(*io));
    atomic_init(&io->refs, 2); /* frame owner + confirmation adapter */
    mutex_init(&io->mutex);
    return io;
}

int dmm_io_poll(DmmIoOperation *io, const DmmPollContext *context) {
    lock(&io->mutex);
    int ready = io->confirmed;
    DmmWaker old = {NULL};
    if (!ready && context && context->waker.task != io->waiter.task) {
        old = io->waiter;
        io->waiter = dmm_waker_clone(context->waker);
    }
    unlock(&io->mutex);
    dmm_waker_drop(old);
    return ready;
}

void dmm_io_request_cancel(DmmIoOperation *io) {
    lock(&io->mutex);
    io->cancel_requested = 1;
    unlock(&io->mutex);
}

int dmm_io_cancel_requested(DmmIoOperation *io) {
    lock(&io->mutex);
    int result = io->cancel_requested;
    unlock(&io->mutex);
    return result;
}

void dmm_io_confirm(DmmIoOperation *io) {
    lock(&io->mutex);
    require(!io->confirmed);
    io->confirmed = 1;
    DmmWaker waiter = io->waiter;
    io->waiter.task = NULL;
    unlock(&io->mutex);
    deliver(waiter);
    /* The frame may have consumed its ownership while being woken. */
    if (atomic_fetch_sub_explicit(&io->refs, 1, memory_order_acq_rel) == 1) {
        mutex_drop(&io->mutex);
        free(io);
    }
}

void dmm_io_destroy(DmmIoOperation *io) {
    lock(&io->mutex);
    require(io->confirmed && !io->waiter.task && !io->owner_consumed);
    io->owner_consumed = 1;
    unlock(&io->mutex);
    if (atomic_fetch_sub_explicit(&io->refs, 1, memory_order_acq_rel) == 1) {
        mutex_drop(&io->mutex);
        free(io);
    }
}
