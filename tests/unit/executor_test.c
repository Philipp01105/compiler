/* Deterministic private runtime tests. Gates force the relevant interleavings;
   no timing assumptions or sleeps are used. */
#undef NDEBUG
#include "executor.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION TestMutex;
typedef CONDITION_VARIABLE TestCondition;
typedef HANDLE TestThread;
#define THREAD_RESULT DWORD WINAPI
#define THREAD_RETURN 0
static void mutex_init(TestMutex *m) { InitializeCriticalSection(m); }
static void mutex_drop(TestMutex *m) { DeleteCriticalSection(m); }
static void lock(TestMutex *m) { EnterCriticalSection(m); }
static void unlock(TestMutex *m) { LeaveCriticalSection(m); }
static void condition_init(TestCondition *c) { InitializeConditionVariable(c); }
static void notify(TestCondition *c) { WakeAllConditionVariable(c); }
static void wait_condition(TestCondition *c, TestMutex *m) { assert(SleepConditionVariableCS(c,m,INFINITE)); }
static TestThread start(DWORD (WINAPI *fn)(void *), void *p) {
    HANDLE t = CreateThread(NULL,0,fn,p,0,NULL); assert(t); return t;
}
static void join(TestThread t) { assert(WaitForSingleObject(t,INFINITE)==WAIT_OBJECT_0); assert(CloseHandle(t)); }
static size_t thread_id(void) { return GetCurrentThreadId(); }
#else
#include <pthread.h>
typedef pthread_mutex_t TestMutex;
typedef pthread_cond_t TestCondition;
typedef pthread_t TestThread;
#define THREAD_RESULT void *
#define THREAD_RETURN NULL
static void mutex_init(TestMutex *m) { assert(!pthread_mutex_init(m,NULL)); }
static void mutex_drop(TestMutex *m) { assert(!pthread_mutex_destroy(m)); }
static void lock(TestMutex *m) { assert(!pthread_mutex_lock(m)); }
static void unlock(TestMutex *m) { assert(!pthread_mutex_unlock(m)); }
static void condition_init(TestCondition *c) { assert(!pthread_cond_init(c,NULL)); }
static void notify(TestCondition *c) { assert(!pthread_cond_broadcast(c)); }
static void wait_condition(TestCondition *c, TestMutex *m) { assert(!pthread_cond_wait(c,m)); }
static TestThread start(void *(*fn)(void *), void *p) {
    pthread_t t; assert(!pthread_create(&t,NULL,fn,p)); return t;
}
static void join(TestThread t) { assert(!pthread_join(t,NULL)); }
/* pthread_t is opaque; use a thread-local address as a portable identity. */
static _Thread_local unsigned identity;
static size_t thread_id(void) { return (size_t)&identity; }
#endif

typedef struct {
    TestMutex mutex;
    TestCondition changed;
    unsigned arrived;
    int released;
} Gate;
static void gate_init(Gate *g) {
    g->arrived=0; g->released=0; mutex_init(&g->mutex); condition_init(&g->changed);
}
static void gate_arrive(Gate *g, int block) {
    lock(&g->mutex); g->arrived++; notify(&g->changed);
    while (block && !g->released) wait_condition(&g->changed,&g->mutex);
    unlock(&g->mutex);
}
static void gate_wait(Gate *g, unsigned count) {
    lock(&g->mutex);
    while (g->arrived<count) wait_condition(&g->changed,&g->mutex);
    unlock(&g->mutex);
}
static void gate_release(Gate *g) {
    lock(&g->mutex); g->released=1; notify(&g->changed); unlock(&g->mutex);
}
static void gate_drop(Gate *g) {
#ifndef _WIN32
    assert(!pthread_cond_destroy(&g->changed));
#endif
    mutex_drop(&g->mutex);
}

typedef struct {
    atomic_uint polls, cancel_polls, destroyed, discarded, taken, cleanups, defers, active;
    size_t poll_thread;
    DmmWaker retained;
} Counts;
static void counts_init(Counts *c) {
    atomic_init(&c->polls,0); atomic_init(&c->cancel_polls,0);
    atomic_init(&c->destroyed,0); atomic_init(&c->discarded,0);
    atomic_init(&c->taken,0); atomic_init(&c->cleanups,0); atomic_init(&c->active,0);
    atomic_init(&c->defers,0);
    c->poll_thread=0; c->retained.task=NULL;
}
typedef struct {
    Counts *counts;
    Gate *gate;
    DmmIoOperation *io, *cleanup_io;
    DmmTask *child;
    int mode, phase, result, completed, scope_entered, scope_exited;
} Frame;
enum { IMMEDIATE, GATED_READY, GATED_PENDING, IO, NESTED, SELF_WAKE };

static int poll_frame(void *pointer, const DmmPollContext *context) {
    Frame *f=pointer;
    Counts *c=f->counts;
    assert(!context->cancelling);
    assert(atomic_fetch_add(&c->active,1)==0); /* no simultaneous polls */
    unsigned n=atomic_fetch_add(&c->polls,1);
    f->scope_entered=1;
    c->poll_thread=thread_id();
    int ready=1;
    if (f->mode==GATED_READY && !n) gate_arrive(f->gate,1);
    if (f->mode==GATED_PENDING && !n) {
        c->retained=dmm_waker_clone(context->waker);
        gate_arrive(f->gate,1);
        ready=0;
    }
    if (f->mode==SELF_WAKE && !n) { dmm_waker_wake(context->waker); ready=0; }
    if (f->mode==IO) {
        ready=dmm_io_poll(f->io,context);
        if (!n) gate_arrive(f->gate,0);
    }
    if (f->mode==NESTED) {
        DmmTaskStatus status=dmm_join_poll(f->child,context);
        ready=status!=DMM_PENDING;
        if (ready) {
            assert(dmm_join_take(f->child,&f->result)==DMM_READY);
            f->child=NULL;
            f->result++;
        }
    }
    if (f->mode==NESTED && f->gate && !n) gate_arrive(f->gate,0);
    if (ready) {
        f->completed=1; atomic_fetch_add(&c->cleanups,1);
        f->scope_exited=1; atomic_fetch_add(&c->defers,1);
    }
    assert(atomic_fetch_sub(&c->active,1)==1);
    return ready;
}

static int cancel_frame(void *pointer, const DmmPollContext *context) {
    Frame *f=pointer;
    Counts *c=f->counts;
    assert(context->cancelling);
    assert(atomic_fetch_add(&c->active,1)==0);
    atomic_fetch_add(&c->cancel_polls,1);
    int ready=1;
    if (f->child) {
        if (!f->phase) { dmm_join_cancel(f->child); f->phase=1; }
        ready=dmm_cancel_poll(f->child,context)!=DMM_PENDING;
        if (ready) { dmm_cancel_finish(f->child); f->child=NULL; }
    }
    if (f->io) {
        dmm_io_request_cancel(f->io);
        ready=dmm_io_poll(f->io,context);
        if (f->gate) gate_arrive(f->gate,0);
    }
    if (ready && f->cleanup_io) {
        if (!f->phase) { f->phase=1; atomic_fetch_add(&c->cleanups,1); }
        if (f->scope_entered && !f->scope_exited) {
            f->scope_exited=1; atomic_fetch_add(&c->defers,1);
        }
        ready=dmm_io_poll(f->cleanup_io,context);
    }
    if (ready && !f->cleanup_io) {
        atomic_fetch_add(&c->cleanups,1);
        if (f->scope_entered && !f->scope_exited) {
            f->scope_exited=1; atomic_fetch_add(&c->defers,1);
        }
    }
    assert(atomic_fetch_sub(&c->active,1)==1);
    return ready;
}

static void take_frame(void *pointer, void *output) {
    Frame *f=pointer;
    assert(f->completed && output);
    *(int *)output=f->result;
    atomic_fetch_add(&f->counts->taken,1);
}
static void destroy_frame(void *pointer, int discard) {
    Frame *f=pointer;
    assert(!atomic_load(&f->counts->active) && !f->child);
    assert(!discard || f->completed);
    atomic_fetch_add(&f->counts->discarded,(unsigned)discard);
    assert(atomic_fetch_add(&f->counts->destroyed,1)==0);
    if (f->io) dmm_io_destroy(f->io);
    if (f->cleanup_io) dmm_io_destroy(f->cleanup_io);
    free(f);
}
static DmmRuntimeFuture future(Counts *c, int mode, Gate *gate) {
    Frame *f=calloc(1,sizeof(*f)); assert(f);
    f->counts=c; f->mode=mode; f->gate=gate; f->result=42;
    return (DmmRuntimeFuture){f,poll_frame,cancel_frame,take_frame,destroy_frame};
}
static void finish_executor(DmmExecutor *e) { dmm_shutdown_block_on(dmm_executor_shutdown(e,DMM_DRAIN)); }

static void test_parallel_and_lifetime(void) {
    DmmExecutor *e=dmm_executor_create(2);
    Counts c[2]; Gate gate; gate_init(&gate);
    DmmTask *tasks[2];
    for (unsigned i=0;i<2;i++) { counts_init(&c[i]); tasks[i]=dmm_executor_spawn(e,future(&c[i],GATED_READY,&gate)); }
    gate_wait(&gate,2); /* both workers must actually run concurrently */
    DmmShutdown *s=dmm_executor_shutdown(e,DMM_DRAIN);
    assert(!dmm_shutdown_poll(s,NULL));
    Counts rejected; counts_init(&rejected);
    DmmRuntimeFuture f=future(&rejected,IMMEDIATE,NULL);
    assert(!dmm_executor_spawn(e,f));
    dmm_future_cancel_block_on(f); /* rejection retains caller ownership */
    gate_release(&gate);
    dmm_shutdown_block_on(s);
    /* Handles own results independently of the consumed executor. */
    for (unsigned i=0;i<2;i++) {
        assert(!atomic_load(&c[i].destroyed));
        int value=0; assert(dmm_join_block_on(tasks[i],&value)==DMM_READY && value==42);
        assert(atomic_load(&c[i].destroyed)==1 && atomic_load(&c[i].cleanups)==1);
    }
    gate_drop(&gate);
}

static void test_cancel_before_poll(void) {
    DmmExecutor *e=dmm_executor_create(1);
    Counts blocker,c; counts_init(&blocker); counts_init(&c);
    Gate gate; gate_init(&gate);
    DmmTask *first=dmm_executor_spawn(e,future(&blocker,GATED_READY,&gate));
    gate_wait(&gate,1);
    DmmTask *cancel=dmm_join_cancel(dmm_executor_spawn(e,future(&c,IMMEDIATE,NULL)));
    assert(dmm_cancel_poll(cancel,NULL)==DMM_PENDING && !atomic_load(&c.polls));
    gate_release(&gate); int value;
    assert(dmm_join_block_on(first,&value)==DMM_READY);
    dmm_cancel_block_on(cancel);
    assert(!atomic_load(&c.polls) && atomic_load(&c.cancel_polls)==1);
    assert(!atomic_load(&c.defers)); /* scopes never entered have no defers */
    assert(atomic_load(&c.destroyed)==1 && !atomic_load(&c.discarded));
    finish_executor(e); gate_drop(&gate);
}

static void test_cancel_during_poll(void) {
    DmmExecutor *e=dmm_executor_create(2);
    Counts c; counts_init(&c); Gate gate; gate_init(&gate);
    DmmTask *t=dmm_executor_spawn(e,future(&c,GATED_READY,&gate));
    gate_wait(&gate,1); dmm_join_cancel(t);
    DmmShutdown *s=dmm_executor_shutdown(e,DMM_CANCEL);
    assert(!dmm_shutdown_poll(s,NULL) && !atomic_load(&c.destroyed));
    gate_release(&gate); dmm_cancel_block_on(t); dmm_shutdown_block_on(s);
    assert(atomic_load(&c.polls)==1 && !atomic_load(&c.cancel_polls));
    assert(atomic_load(&c.discarded)==1 && atomic_load(&c.destroyed)==1 && atomic_load(&c.cleanups)==1);
    assert(atomic_load(&c.defers)==1);
    gate_drop(&gate);
}

static void test_cancel_completed(void) {
    Counts c; counts_init(&c); DmmExecutor *e=dmm_executor_create(1);
    DmmTask *t=dmm_executor_spawn(e,future(&c,IMMEDIATE,NULL));
    finish_executor(e); /* completed, but its result remains owned by the join */
    assert(!atomic_load(&c.destroyed));
    dmm_cancel_block_on(dmm_join_cancel(t));
    assert(atomic_load(&c.discarded)==1 && atomic_load(&c.destroyed)==1 && !atomic_load(&c.cancel_polls));
}

static void test_shutdown_preserves_completed(void) {
    Counts ready,running; counts_init(&ready); counts_init(&running);
    Gate gate; gate_init(&gate); DmmExecutor *e=dmm_executor_create(1);
    DmmTask *first=dmm_executor_spawn(e,future(&ready,IMMEDIATE,NULL));
    DmmTask *second=dmm_executor_spawn(e,future(&running,GATED_READY,&gate));
    gate_wait(&gate,1); /* the single worker already published first's Ready */
    assert(dmm_join_poll(first,NULL)==DMM_READY);
    DmmShutdown *s=dmm_executor_shutdown(e,DMM_CANCEL);
    gate_release(&gate); dmm_shutdown_block_on(s);
    int value=0; assert(dmm_join_block_on(first,&value)==DMM_READY && value==42);
    assert(dmm_join_block_on(second,NULL)==DMM_CANCELLED);
    assert(!atomic_load(&ready.discarded) && atomic_load(&ready.taken)==1);
    assert(atomic_load(&running.discarded)==1);
    gate_drop(&gate);
}

static void test_io_confirmation(void) {
    Counts c; counts_init(&c); Gate gate; gate_init(&gate);
    DmmExecutor *e=dmm_executor_create(2);
    DmmRuntimeFuture f=future(&c,IO,&gate);
    Frame *frame=f.frame;
    DmmIoOperation *io=frame->io=dmm_io_create();
    DmmIoOperation *cleanup=frame->cleanup_io=dmm_io_create();
    DmmTask *t=dmm_executor_spawn(e,f);
    gate_wait(&gate,1);
    DmmShutdown *s=dmm_executor_shutdown(e,DMM_CANCEL);
    gate_wait(&gate,2);
    assert(dmm_io_cancel_requested(io));
    assert(dmm_join_poll(t,NULL)==DMM_PENDING && !dmm_shutdown_poll(s,NULL));
    assert(!atomic_load(&c.destroyed) && !atomic_load(&c.cleanups));
    dmm_io_confirm(io);
    /* Even confirmed child I/O is insufficient: awaitable cleanup must finish. */
    gate_wait(&gate,3);
    assert(dmm_join_poll(t,NULL)==DMM_PENDING && !dmm_shutdown_poll(s,NULL));
    assert(!atomic_load(&c.destroyed));
    dmm_io_confirm(cleanup);
    dmm_shutdown_block_on(s);
    assert(atomic_load(&c.destroyed)==1 && atomic_load(&c.cleanups)==1);
    assert(atomic_load(&c.defers)==1);
    assert(dmm_join_block_on(t,NULL)==DMM_CANCELLED);
    gate_drop(&gate);
}

typedef struct { DmmWaker waker; } WakeJob;
static THREAD_RESULT wake_many(void *pointer) {
    WakeJob *job=pointer;
    for (unsigned n=0;n<1000;n++) dmm_waker_wake(job->waker);
    return THREAD_RETURN;
}
static void test_concurrent_wakes(void) {
    for (unsigned round=0;round<30;round++) {
        Counts c; counts_init(&c); Gate gate; gate_init(&gate);
        int after_return=(round & 1)!=0;
        DmmExecutor *e=dmm_executor_create(after_return ? 1 : 4);
        DmmTask *t=dmm_executor_spawn(e,future(&c,GATED_PENDING,&gate));
        gate_wait(&gate,1);
        Counts marker_counts; Gate marker_gate; DmmTask *marker=NULL;
        if (after_return) {
            counts_init(&marker_counts); gate_init(&marker_gate);
            marker=dmm_executor_spawn(e,future(&marker_counts,GATED_READY,&marker_gate));
            gate_release(&gate);
            /* The marker cannot enter until first's Pending poll returned. */
            gate_wait(&marker_gate,1);
        }
        WakeJob job={c.retained}; TestThread threads[4];
        for (unsigned i=0;i<4;i++) threads[i]=start(wake_many,&job);
        for (unsigned i=0;i<4;i++) join(threads[i]);
        gate_release(&gate);
        if (marker) gate_release(&marker_gate);
        int value; assert(dmm_join_block_on(t,&value)==DMM_READY && value==42);
        assert(atomic_load(&c.polls)==2);
        if (marker) { assert(dmm_join_block_on(marker,&value)==DMM_READY); gate_drop(&marker_gate); }
        /* A retained waker remains safe after handle consumption and shutdown. */
        finish_executor(e);
        dmm_waker_wake(c.retained); dmm_waker_drop(c.retained);
        gate_drop(&gate);
    }
}

static void test_nested_cross_executor(void) {
    Counts child,parent; counts_init(&child); counts_init(&parent);
    Gate gate; gate_init(&gate);
    DmmExecutor *a=dmm_executor_create(1), *b=dmm_executor_create(1);
    DmmTask *t=dmm_executor_spawn(a,future(&child,GATED_READY,&gate));
    gate_wait(&gate,1);
    DmmRuntimeFuture f=future(&parent,NESTED,NULL); ((Frame *)f.frame)->child=t;
    DmmTask *p=dmm_executor_spawn(b,f);
    gate_release(&gate);
    int value; assert(dmm_join_block_on(p,&value)==DMM_READY && value==43);
    assert(atomic_load(&child.destroyed)==1 && atomic_load(&parent.destroyed)==1);
    finish_executor(a); finish_executor(b); gate_drop(&gate);
}

static void test_wakes_racing_cancellation(void) {
    for (unsigned round=0;round<30;round++) {
        Counts c; counts_init(&c); Gate gate; gate_init(&gate);
        DmmExecutor *e=dmm_executor_create(4);
        DmmTask *task=dmm_executor_spawn(e,future(&c,GATED_PENDING,&gate));
        gate_wait(&gate,1);
        WakeJob job={c.retained}; TestThread threads[4];
        for (unsigned i=0;i<4;i++) threads[i]=start(wake_many,&job);
        dmm_join_cancel(task);
        assert(dmm_cancel_poll(task,NULL)==DMM_PENDING);
        for (unsigned i=0;i<4;i++) join(threads[i]);
        gate_release(&gate); dmm_cancel_block_on(task); finish_executor(e);
        assert(atomic_load(&c.polls)==1 && atomic_load(&c.cancel_polls)==1);
        assert(atomic_load(&c.destroyed)==1 && atomic_load(&c.taken)==0);
        dmm_waker_wake(c.retained); dmm_waker_drop(c.retained); gate_drop(&gate);
    }
}

static void test_cancel_pending_poll(void) {
    Counts c; counts_init(&c); Gate gate; gate_init(&gate);
    DmmExecutor *e=dmm_executor_create(2);
    DmmTask *t=dmm_executor_spawn(e,future(&c,GATED_PENDING,&gate));
    gate_wait(&gate,1);
    DmmPollContext context={c.retained,0};
    assert(!dmm_poll_cancel_requested(&context));
    dmm_join_cancel(t);
    assert(dmm_poll_cancel_requested(&context));
    assert(dmm_cancel_poll(t,NULL)==DMM_PENDING && !atomic_load(&c.destroyed));
    gate_release(&gate);
    dmm_cancel_block_on(t);
    finish_executor(e);
    assert(atomic_load(&c.polls)==1 && atomic_load(&c.cancel_polls)==1);
    assert(atomic_load(&c.defers)==1 && !atomic_load(&c.discarded));
    dmm_waker_wake(c.retained); dmm_waker_drop(c.retained);
    gate_drop(&gate);
}

static void test_nested_cancellation(void) {
    Counts child,parent; counts_init(&child); counts_init(&parent);
    Gate gate; gate_init(&gate);
    DmmExecutor *a=dmm_executor_create(1), *b=dmm_executor_create(1);
    DmmRuntimeFuture cf=future(&child,IO,&gate);
    DmmIoOperation *io=((Frame *)cf.frame)->io=dmm_io_create();
    DmmTask *t=dmm_executor_spawn(a,cf);
    gate_wait(&gate,1);
    DmmRuntimeFuture pf=future(&parent,NESTED,&gate); ((Frame *)pf.frame)->child=t;
    DmmTask *p=dmm_executor_spawn(b,pf);
    gate_wait(&gate,2);
    DmmShutdown *s=dmm_executor_shutdown(b,DMM_CANCEL);
    gate_wait(&gate,3); /* child's cancellation poll registered its I/O waiter */
    assert(dmm_io_cancel_requested(io));
    assert(!atomic_load(&child.destroyed) && !atomic_load(&parent.destroyed));
    assert(!dmm_shutdown_poll(s,NULL));
    dmm_io_confirm(io);
    dmm_shutdown_block_on(s);
    assert(dmm_join_block_on(p,NULL)==DMM_CANCELLED);
    assert(atomic_load(&child.destroyed)==1 && atomic_load(&parent.destroyed)==1);
    assert(atomic_load(&child.defers)==1 && atomic_load(&parent.defers)==1);
    finish_executor(a); gate_drop(&gate);
}

static void test_caller_thread(void) {
    Counts c; counts_init(&c); int output;
    size_t caller=thread_id();
    dmm_future_block_on(future(&c,SELF_WAKE,NULL),&output);
    assert(output==42 && c.poll_thread==caller && atomic_load(&c.polls)==2);
    assert(atomic_load(&c.destroyed)==1);
    counts_init(&c); dmm_future_cancel_block_on(future(&c,IMMEDIATE,NULL));
    assert(!atomic_load(&c.polls) && atomic_load(&c.cancel_polls)==1 && atomic_load(&c.destroyed)==1);
}

static void test_default(void) {
    Counts c; counts_init(&c); int output;
    assert(dmm_join_block_on(dmm_spawn_default(future(&c,SELF_WAKE,NULL)),&output)==DMM_READY && output==42);
}

static void test_future_adapters(void) {
    Counts c; counts_init(&c);
    DmmExecutor *e=dmm_executor_create(2);
    DmmTask *t=dmm_executor_spawn(e,future(&c,SELF_WAKE,NULL));
    int value=0; DmmJoinResult result={DMM_PENDING,&value};
    dmm_future_block_on(dmm_join_as_future(t),&result);
    assert(result.status==DMM_READY && value==42 && atomic_load(&c.destroyed)==1);
    /* Both operations may be awaited without blocking any executor worker. */
    dmm_future_block_on(dmm_shutdown_as_future(dmm_executor_shutdown(e,DMM_DRAIN)),NULL);
    counts_init(&c);
    dmm_future_block_on(dmm_future_cancel(future(&c,IMMEDIATE,NULL)),NULL);
    assert(!atomic_load(&c.polls) && atomic_load(&c.cancel_polls)==1 && atomic_load(&c.destroyed)==1);
    counts_init(&c);
    /* An abandoned cancellation wrapper must still finish its child's cleanup. */
    dmm_future_cancel_block_on(dmm_future_cancel(future(&c,IMMEDIATE,NULL)));
    assert(!atomic_load(&c.polls) && atomic_load(&c.cancel_polls)==1 && atomic_load(&c.destroyed)==1);

    Gate gate; gate_init(&gate); counts_init(&c);
    e=dmm_executor_create(1);
    DmmRuntimeFuture f=future(&c,IO,&gate);
    DmmIoOperation *io=((Frame *)f.frame)->io=dmm_io_create();
    t=dmm_executor_spawn(e,f);
    gate_wait(&gate,1);
    DmmTask *operation=dmm_executor_spawn(e,dmm_future_cancel(dmm_join_as_future(t)));
    gate_wait(&gate,2);
    assert(dmm_io_cancel_requested(io) && !atomic_load(&c.destroyed));
    dmm_io_confirm(io);
    assert(dmm_join_block_on(operation,NULL)==DMM_READY);
    assert(atomic_load(&c.destroyed)==1 && !atomic_load(&c.discarded));
    finish_executor(e); gate_drop(&gate);

    /* Cancelled joins retain their error through the ordinary join adapter. */
    counts_init(&c); gate_init(&gate); e=dmm_executor_create(1);
    f=future(&c,IO,&gate); io=((Frame *)f.frame)->io=dmm_io_create();
    t=dmm_executor_spawn(e,f); gate_wait(&gate,1);
    DmmShutdown *s=dmm_executor_shutdown(e,DMM_CANCEL); gate_wait(&gate,2);
    dmm_io_confirm(io); dmm_shutdown_block_on(s);
    result=(DmmJoinResult){DMM_PENDING,NULL};
    dmm_future_block_on(dmm_join_as_future(t),&result);
    assert(result.status==DMM_CANCELLED && atomic_load(&c.destroyed)==1);
    gate_drop(&gate);

    /* Cancelling an unpolled join adapter drops an already completed T. */
    counts_init(&c); e=dmm_executor_create(1);
    t=dmm_executor_spawn(e,future(&c,IMMEDIATE,NULL));
    finish_executor(e);
    dmm_future_cancel_block_on(dmm_join_as_future(t));
    assert(atomic_load(&c.discarded)==1 && atomic_load(&c.destroyed)==1);

    /* An async shutdown adapter can run on a separate executor and must finish
       its workers even when the adapter itself is cancelled. */
    gate_init(&gate); counts_init(&c);
    DmmExecutor *a=dmm_executor_create(1), *b=dmm_executor_create(1);
    f=future(&c,IO,&gate); io=((Frame *)f.frame)->io=dmm_io_create();
    t=dmm_executor_spawn(b,f); gate_wait(&gate,1);
    operation=dmm_executor_spawn(a,dmm_shutdown_as_future(dmm_executor_shutdown(b,DMM_DRAIN)));
    dmm_join_cancel(operation);
    assert(dmm_cancel_poll(operation,NULL)==DMM_PENDING);
    assert(!dmm_io_cancel_requested(io)); /* cancelling shutdown cannot abandon Drain */
    dmm_io_confirm(io);
    dmm_cancel_block_on(operation);
    value=0; assert(dmm_join_block_on(t,&value)==DMM_READY && value==42);
    finish_executor(a); gate_drop(&gate);
}

int main(void) {
    test_parallel_and_lifetime();
    test_cancel_before_poll();
    test_cancel_during_poll();
    test_cancel_completed();
    test_shutdown_preserves_completed();
    test_io_confirmation();
    test_concurrent_wakes();
    test_wakes_racing_cancellation();
    test_nested_cross_executor();
    test_cancel_pending_poll();
    test_nested_cancellation();
    test_caller_thread();
    test_future_adapters();
    test_default();
    puts("executor runtime tests passed");
    return 0;
}
