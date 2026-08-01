# Private executor foundation

`executor.h` defines the internal scheduler contract. It is currently used by
`executor_runtime_unit`, not by compiled DMM programs. Language API, ownership
checking and cancellation lowering still need to be connected to this contract.
The C implementation is the executable contract for that work; it is not linked
into standalone DMM executables. `native_threads.inc` emits the native platform
primitives but does not yet emit the scheduler itself.

## Synchronization and ownership

The executor mutex protects admission, active tasks, the ready queue, polling
state, terminal status, cancellation requests and waiter registration. Callbacks
run outside this lock. Queue membership and a notification received during a poll
are separate bits. A worker clears the notification only when starting a poll;
a subsequent wake survives Pending and queues another poll. Running frames cannot
be queued. Pending without a notification parks until a wake or cancel request.

The scheduler and consuming handle each own a task reference. Cloned wakers own
additional references. Each task owns an executor reference, so consumed executor
owners do not invalidate joins or wakers. Terminal publication occurs after any
cancellation destruction. Scheduler ownership ends after waiter delivery. Ready
results remain in their frames until join consumption or explicit cancellation.
Callbacks move outputs through `take_result`; `destroy(frame, true)` discards an
unclaimed completed output. Cancellation cleanup must destroy captured resources
but must not claim to have produced an output.

`dmm_join_cancel` consumes a join and transfers its reference into a cancellation
operation. Callers poll that operation and finish it only after termination. No
loan or resource may be released just because a request was made. The normal
Ready/cancellation winner is selected under the executor lock after poll returns.
A request that won during a successful normal poll discards that poll's output.
Shutdown Cancel leaves already published Ready results available to their joins.

`dmm_poll_cancel_requested` observes requests made during a running callback. The
future's implementation must use it at cooperative suspension boundaries. The
scheduler never interrupts synchronous code. Cancellation callbacks can suspend
using the same waker while child futures, I/O and asynchronous cleanups finish.
The compiler remains responsible for active-scope tracking and cleanup order.

Consuming Future adapters are provided for direct future cancellation, join
await and shutdown await. Cancelling a cancellation or shutdown adapter continues
its underlying cleanup rather than abandoning it. The join adapter preserves the
Ready/Cancelled result and retains unclaimed output ownership until its result
is consumed; cancellation discards such an output through the ordinary join path.

Waiter wakers are replaced and delivered without holding executor locks across
executors. The I/O adapter separates request from confirmation and synchronizes
confirmation with destruction. Frame and confirmation adapter own independent
references; confirmation retains its reference through wake delivery even when
the resumed frame concurrently consumes its ownership. The adapter must not
access an operation after confirmation or leave other adapter calls in flight.

## Native primitives

The emitted `__dmm_async_thread_create(callback, argument)` and
`__dmm_async_thread_join(handle)` use CreateThread/WaitForSingleObject on COFF and
raw clone/exit/futex on ELF. Linux native workers have no libc TLS and must execute
only generated code or callbacks that do not require libc TLS. Their stack remains
allocated until the kernel clears the child TID, then join frees the stack and
control object. Joining consumes the handle.

`__dmm_async_wait_create`, `__dmm_async_wait`, `__dmm_async_wake`,
`__dmm_async_wait_reset`, and `__dmm_async_wait_destroy` implement manual-reset
events using Win32 events or an atomic word plus private futex. A signal preceding
a wait is retained. Reset must occur under the scheduler predicate lock before
parking; destruction requires that all waiters have stopped. Resource failures
trap. Thread primitives are encoded directly for object, assembly and executable
outputs through the existing runtime emitter.

## Verification

`executor_runtime_unit` forces parallel polling, concurrent wakes during Pending,
self-wakes, cancellation before first poll, cancellation during running Ready and
Pending polls, completed-result cancellation, nested cross-executor awaits and
cancellation, delayed child I/O confirmation, asynchronous cleanup confirmation,
shutdown with open joins, caller-thread blocking and default executor use.
Future adapters cover void cancellation, cancelled join results, discarded join
outputs and shutdown from another executor, including cancellation of shutdown.

`runtime_unit` executes the emitted thread/event instructions on the host OS.
`native_binary_unit` verifies both ELF and COFF encoding/linking. Existing async
tests retain their Stage 1 coverage. Passing these tests does not constitute
Stage 2 language acceptance.
