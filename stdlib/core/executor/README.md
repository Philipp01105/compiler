# stdlib/core/executor

The scheduler is implemented in [executor.dmm](executor.dmm). It
owns ready queues, worker polling, retained wakers, join/cancel/shutdown adapters,
the lazy default executor and I/O acknowledgements. Applications continue using
the language's `Executor`, `spawn`, `block_on`, `cancel` and `Future<T>` API.
Emitted async bodies and executor operations select the platform profile; merely
enabling the async manifest feature does not. GCC or Clang supplies regular CRT
startup. The DMM stdlib packages are compiled into the program object.

The compiler owns async state-machine lowering, pinned-frame layout, ownership,
Send/Sync checks and atomic instructions. The DMM scheduler calls explicitly
declared compiler atomic and memory primitives through FFI. Thread creation,
confirmed join and manual-reset events use the DMM threading package and OS FFI.
Task/context low bit tagging and the 80-byte native Future header remain private
compiler/runtime contracts. All callback types use the target system ABI.

Generated frames use the compiler allocator and its release function. Scheduler
objects and scheduler-created adapters use libc/UCRT allocation and release.
Each task retains its executor; stored wakers retain tasks. Poll, destruction,
I/O acknowledgement and waker callbacks run outside locks. Resetting an event
and checking its queue predicate share the scheduler lock, preventing lost wakes.
Ready/cancellation decisions are published once after polling under that lock.
Cancel never interrupts synchronous code or substitutes for I/O termination.

`nativeFuture(frame)` consumes ownership of a `*void` representation of a native
`Future<void>` frame without a runtime call. It requires the async feature. This
is an FFI trust boundary: the provider must supply valid stable storage, system
ABI callbacks, correct destruction, thread-safe polling/waking, retained context
lifetimes, and cancellation that waits for confirmed termination. The native
frame is promised Send-safe; the compiler still checks the enclosing DMM loans.
There is no by-value FFI marshaling of `Future<T>`. `core/net` uses this general
bridge after its ordinary DMM package `wait` call; there are no network intrinsics.

`stdlib/system` is the default source provider. The compiler retains the ABI functions needed by generated
frames and startup from its normal package graph. There are no required runtime objects or archives.
See [stdlib/system/README.md](../../system/README.md) for installation and manual linking.

The C schedulers under `tests/fixtures` are independent test references, excluded
from the compiler and installed stdlib. Executor, network, lifetime and FFI
contract suites execute the DMM implementation on Linux and Windows.

The thread and event adapters live in [stdlib/native/threading](../../native/threading), with direct glibc/pthreads
or Kernel32/UCRT FFI bindings. They are compiled with the application, using normal package imports.
