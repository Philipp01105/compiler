# Raw native bindings

The [language specification](../../LANGUAGE_SPEC.md#native-ffi) defines the native type/call contract;
the [FFI migration plan](../../plans/ffi.md) records implementation status.

These packages describe x86-64 glibc and MinGW-w64 UCRT64 APIs. Import the relevant
package, for example `import "stdlib/native/pthreads";`. `_linux.dmm` and
`_windows.dmm` files are selected by the compiler's output target.

`glibc` contains allocation, error access, sockets, epoll/eventfd, clocks and DNS.
`pthreads` contains thread creation/join and mutex/condition operations.
`kernel32` contains events, waits, process termination, IOCP and cancellation.
`ucrt` contains allocation, errno and `_beginthreadex`.
`winsock` contains socket operations, overlapped transfers and resolver APIs.

Native declarations are a trust boundary. Callers manage pointer validity, lengths,
initialization, synchronization, ownership and lifetime. A pointer cast does not
convert an ordinary DMM aggregate into a native layout. Native function pointers
must refer to matching imports or `export "system"` functions. Callback contexts
must live until all native calls have ended and threads have joined. Do not copy
initialized pthread synchronization objects, release buffers before confirmed
cancellation, or allow foreign exceptions to unwind through DMM frames.

Ordinary DMM arrays retain rounded eight-byte element slots. For a contiguous C
array whose native element size is not a multiple of eight (notably epoll events),
use an array field in a native aggregate or explicitly allocated native byte
storage, with a typed raw pointer and the target's native element stride.

Win32 BOOL uses i32; C _Bool uses bit. UCRT's thread entry returns u32 normally;
join the returned handle and close it. `kernel32.handleFromBits` explicitly
reinterprets uintptr_t handles; `invalidHandle` returns INVALID_HANDLE_VALUE.
Optional pointer arguments can use `core.null<u8>().(*void)` or a typed null pointer.
An initialized, zero-valued native function-pointer variable supplies a null
completion routine. Strings require an explicit pointer to terminated byte storage,
for example `core.core_string_data("0")`; there is no implicit marshaling.

The platform runtime now uses these bindings in `src/runtime/platform/*.dmm`.
The compiler builds and installs a DMM platform object; the remaining network C
implementation is packaged with it in `network-shim.a` until stage 5.
