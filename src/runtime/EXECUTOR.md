# Executor runtime and native ABI

## Private platform ABI v1

The optional platform profile keeps the emitted scheduler but implements thread/event primitives in
`platform/*.dmm`. The compatibility declarations in `platform_shim.h` use the x86-64 System V or Microsoft x64 C ABI. Thread
creation takes `void (*callback)(void *)` and its argument and returns a non-null opaque handle. The callback returns
normally. Join waits for confirmed thread termination, then consumes the handle. Allocations never cross runtime
ownership boundaries. Resource/API failures are fatal, with no recoverable partial startup contract.

Linux implements threads with pthreads, including libc TLS initialization. Windows is specifically MinGW-w64
UCRT64: `_beginthreadex` starts a native DMM export, normal callback return performs CRT thread cleanup, and join uses
`WaitForSingleObject` followed by `CloseHandle`. Generated workers do not use raw clone or CreateThread in this profile.

`__dmm_async_wait_create` returns an initially unsignalled manual-reset event. `__dmm_async_wake` signals it and
retains that signal until `__dmm_async_wait_reset`; wait does not consume the signal. Linux uses a mutex-protected
predicate and condition-variable loop; Windows uses a manual-reset Win32 event. Reset remains under the scheduler
predicate lock, and destroy requires every waiter to have stopped. Existing no-lost-wake and single-poller invariants
apply identically to standalone and platform profiles.

Regular platform C startup calls the compiler-generated `main`, which forwards to `int __dmm_runtime_main(void)` once.
The generated object owns package init, DMM main, default-executor drain, package cleanup and the preserved return status.
Immediate process exit and traps do not run this normal cleanup path. `__dmm_platform_exit` terminates the whole
process from any thread. This private ABI is not a source-language foreign-function interface.
With NETWORK, `network-shim.a` combines the remaining network C implementation and the DMM platform object,
and supplies reactor/DNS operations through the generated I/O acknowledgement ABI.
Networking remains RUNNING throughout executor Drain; it enters DRAINING immediately before package cleanup and
shuts down after cleanup. The core ownership contract and platform implementations are in
[NETWORK_RUNTIME.md](../../NETWORK_RUNTIME.md).

`executor.h` defines the internal C scheduler contract used by
`executor_runtime_unit`. The standalone backends emit the equivalent scheduler
in `native_executor.inc` and platform primitives in `native_threads.inc`.
The C contract implementation is not linked into standalone DMM executables.
The frontend enforces the public API and ownership; IR and native callbacks
implement cancellation and active-scope cleanup. See [LANGUAGE_SPEC.md](../../LANGUAGE_SPEC.md) for
the source contract.

## Emitted frame ABI

Frames remain pinned. The private header contains poll at byte 0, destroy at 8,
state at 16, cancellation poll at 24, result destructor at 32, poll context at
40, result-present at 48, cancellation state at 56, auxiliary storage at 64,
and result-cancellation callback at 72. Results start at 80. A JoinHandle uses
its result tag at 80 and payload at 88; its auxiliary field owns the task until
join consumption, and byte 56 records the output byte count. Callbacks receive
`(frame, context)`; destroy receives `(frame, discardResult)`.

Context addresses identify retained tasks. Bit 0 suppresses cancellation during
an active defer cleanup; retain, release and wake mask that tag. Result cancellation
walks owned child slots to acknowledged completion, clearing consumed slots so
repeated Pending polls cannot duplicate destruction. Result destruction runs only
after that walk finishes. The ABI is private and not a public foreign-function API.

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

`runtime_unit` executes emitted scheduling, thread, event and private I/O
instructions on the host OS, including parallel polls, wake races, running-poll
cancellation and shutdown with delayed confirmation and open joins.
`async_runtime_contract` exercises generated cancellation before first poll,
Pending cancellation, awaitable defer cleanup, result disposal and asynchronous
child-result disposal. `async_executor_contract` compiles the public API to
standalone ELF and COFF at O0/O1 and executes the host format.
`async_semantic_unit` checks ownership, concrete Send eligibility, loans and
negative cancellation verifier transitions. `native_binary_unit` verifies
both ELF and COFF encoding/linking.
