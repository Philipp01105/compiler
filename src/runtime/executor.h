#ifndef DMM_EXECUTOR_H
#define DMM_EXECUTOR_H

/* Private Stage 2 runtime contract. These are runtime objects, not a C ABI for
   language-level Future<T>. A waker stored beyond a poll must be cloned. */
#include <stddef.h>

typedef struct DmmExecutor DmmExecutor;
typedef struct DmmTask DmmTask;
typedef struct DmmShutdown DmmShutdown;

typedef struct {
    DmmTask *task;
} DmmWaker;

typedef struct {
    DmmWaker waker;
    int cancelling;
} DmmPollContext;

typedef enum { DMM_PENDING, DMM_READY, DMM_CANCELLED } DmmTaskStatus;

typedef enum { DMM_DRAIN, DMM_CANCEL } DmmShutdownMode;

typedef struct {
    void *frame;
    /* Ready means all normal cleanup has finished. cancel_poll Ready means
       child/I/O termination and all cancellation cleanup have finished. */
    int (*poll)(void *, const DmmPollContext *);

    int (*cancel_poll)(void *, const DmmPollContext *);

    void (*take_result)(void *, void *);

    /* discard_result is true only for a completed, unclaimed output. */
    void (*destroy)(void *, int discard_result);
} DmmRuntimeFuture;

/* Private result destination for a join adapter; value receives T on Ready. */
typedef struct {
    DmmTaskStatus status;
    void *value;
} DmmJoinResult;

/* Consuming adapters let these operations participate in nested async polls. */
DmmRuntimeFuture dmm_future_cancel(DmmRuntimeFuture);

DmmRuntimeFuture dmm_join_as_future(DmmTask *);

DmmRuntimeFuture dmm_shutdown_as_future(DmmShutdown *);

DmmExecutor *dmm_executor_create(size_t workers);

/* The caller transfers the future. NULL means the executor is closed; the
   caller still owns the future in that case. Allocation failures are fatal. */
DmmTask *dmm_executor_spawn(DmmExecutor *, DmmRuntimeFuture);

DmmTaskStatus dmm_join_poll(DmmTask *, const DmmPollContext *);

/* Consumes a terminal join; copies/moves a Ready result into output. */
DmmTaskStatus dmm_join_take(DmmTask *, void *output);

/* Consumes the join, returning the same object as a cancellation operation.
   Poll it until terminal, then consume it with dmm_cancel_finish. */
DmmTask *dmm_join_cancel(DmmTask *);

DmmTaskStatus dmm_cancel_poll(DmmTask *, const DmmPollContext *);

void dmm_cancel_finish(DmmTask *);

DmmShutdown *dmm_executor_shutdown(DmmExecutor *, DmmShutdownMode);

int dmm_shutdown_poll(DmmShutdown *, const DmmPollContext *);

void dmm_shutdown_finish(DmmShutdown *);

/* Polls exclusively on the calling thread; no Send requirement. */
void dmm_future_block_on(DmmRuntimeFuture, void *output);

void dmm_future_cancel_block_on(DmmRuntimeFuture);

DmmTaskStatus dmm_join_block_on(DmmTask *, void *output);

void dmm_cancel_block_on(DmmTask *);

void dmm_shutdown_block_on(DmmShutdown *);

DmmTask *dmm_spawn_default(DmmRuntimeFuture);

DmmWaker dmm_waker_clone(DmmWaker);

void dmm_waker_wake(DmmWaker);

void dmm_waker_drop(DmmWaker);

/* Cooperative check at a suspension boundary, including requests made while
   this poll is running. It never interrupts synchronous user code. */
int dmm_poll_cancel_requested(const DmmPollContext *);

/* Internal I/O handshake: the OS/test adapter owns the operation until it
   confirms termination. Requesting cancellation does not confirm anything. */
typedef struct DmmIoOperation DmmIoOperation;

DmmIoOperation *dmm_io_create(void);

int dmm_io_poll(DmmIoOperation *, const DmmPollContext *);

void dmm_io_request_cancel(DmmIoOperation *);

int dmm_io_cancel_requested(DmmIoOperation *);

void dmm_io_confirm(DmmIoOperation *);

/* Consumes frame ownership after confirmation. The confirmation call holds
   its own reference until waiter delivery has finished. */
void dmm_io_destroy(DmmIoOperation *);

#endif
