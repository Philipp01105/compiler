# stdlib/core

`import "stdlib/core";` loads language-adjacent contracts and the remaining low-level forwarding APIs. It provides
ordinary typed DMM functions. It participates in the module/package visibility model.
Null pointers, pointer offsets, overlap-safe byte copying and filling are implemented in DMM. Atomic types use
explicit native declarations for the four sequentially consistent machine primitives; their function names
receive no special semantic treatment.
The `stdlib/core/raw` package implements allocation, byte operations, raw I/O, and process operations.
Core defines Option, Result, propagation, AllocError and Iterator<T> directly.
Root stdlib publicly re-exports those canonical types. The old AllocationError
status enum is transitional and is not used by the redesigned owning APIs.
The package graph is acyclic, so root containers can use canonical `core.Copy` bounds.
`core.Poll<T>` is an ordinary public sum type with `Pending` and `Ready(T)`;
matching a move-only `Ready` payload transfers it under the normal consuming-pattern rules.
It is a value-level foundation for the future polling API, which is not public yet.
The separate [stdlib/async](../async/README.md) package provides retained Wakers,
Contexts and a structural Poller contract; its Future adapter remains in progress.
The separate `stdlib/core/net` package provides move-only sockets/address lists and typed asynchronous TCP/UDP/DNS
over the private platform runtime. Its API and ownership/completion contract are in
[stdlib/core/net/README.md](net/README.md).

```dmm
package main;
import (
    "stdlib/core"
);

func main() -> int {
    var bytes:*u8 = core.core_alloc(4);
    if (bytes == core.core_null()) { return 1; }
    core.core_copy(bytes, core.core_string_data("abc\n"), 4);
    var written:isize = core.core_write(core.CORE_STDOUT, bytes, 4);
    core.core_release(bytes);
    if (written != 4) { return 2; }
    return 0;
}
```

## Shared ownership and lifetimes

`memory.shared(value)` in `stdlib/memory` takes ownership and returns `core.Result<memory.Shared<T>,core.AllocError>`.
Every successful handle owns a non-null heap block containing an atomic reference count and one initialized payload.
`handle.clone()` borrows the handle and increments only its reference count; `handle.get()` returns a checked `&T`
whose lifetime is tied to that handle. The final release destroys the payload once and frees the block. Reference-count
operations use sequentially consistent compare-exchange loops and trap before overflow. Allocation failure destroys
the incoming value and returns OutOfMemory.

Shared handles are move-only and require explicit initialization. Shared has Send and Sync exactly when its payload
has both. Tasks receive their own clones; synchronized payloads such as AtomicUsize support shared mutation. Borrowed
payload views retain their origins through construction, cloning, and aggregate transfer. Weak references, exclusive
mutation, copy-on-write and channels are not provided. `stdlib/sync` supplies owning mutex guards.

`core.initialize<T>(ptr,value)` starts a lifetime in valid, aligned storage without a live T, taking ownership without
dropping previous contents. `core.destroy<T>(ptr)` invokes the same destructor and field/payload cleanup as ordinary
scope cleanup and ends the lifetime without freeing memory. For a directly tracked whole value, the compiler verifies
state and loans and updates cleanup flags. For untracked heap pointers the caller guarantees these preconditions.
Initialize before reading or destroying that storage again. Byte allocation/zeroing alone does not initialize a type
requiring explicit initialization.

See [the executable Shared Async example](../../examples/shared_async/shared_async.dmm).

## Byte operations

| Function                                                         | Contract                                                                                      |
|------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `core.core_alloc(bytes:usize) -> *u8`                            | Allocate a byte region; return null on failure or unsupported size. Contents are unspecified. |
| `core.core_release(data:*u8)`                                    | Release an allocation exactly once. Null is accepted.                                         |
| `core.core_null() -> *u8`                                        | Produce a typed null pointer.                                                                 |
| `core.core_offset(data:*u8, offset:isize) -> *u8`                | Add a signed byte displacement without bounds checks.                                         |
| `core.core_copy(destination:*u8, source:*u8, bytes:usize)`       | Copy a byte region, including overlapping regions in either direction.                        |
| `core.core_fill(destination:*u8, value:u8, bytes:usize)`         | Fill a byte region.                                                                           |
| `core.core_load(data:*u8, offset:usize) -> u8`                   | Read a byte using ordinary DMM pointer dereference.                                           |
| `core.core_store(data:*u8, offset:usize, value:u8)`              | Write a byte using ordinary DMM pointer dereference.                                          |
| `core.core_string_data(text:string) -> *u8`                      | Borrow the existing NUL-terminated string representation without copying.                     |
| `core.core_string_length(text:string) -> usize`                  | Return the runtime byte length, including embedded NULs. A null string has length zero.                           |
| `core.core_read(fd:int, destination:*u8, bytes:usize) -> isize`  | Return bytes read, zero at EOF, or a negative error.                                          |
| `core.core_write(fd:int, source:*u8, bytes:usize) -> isize`      | Return bytes written or a negative error.                                                     |
| `core.core_open(path:string, flags:int, permissions:int) -> int` | Return a descriptor or a negative error.                                                      |
| `core.core_close(fd:int) -> int`                                 | Return zero or a negative error.                                                              |
| `core.core_exit(status:int) -> never`                            | Exit without returning to the caller.                                                         |
| `core.core_trap() -> never`                                      | Trap without returning to the caller.                                                         |

Copy/fill with zero bytes do not access either pointer, so null pointers are accepted. Nonzero copy/fill traps on null
pointers or counts above `INT64_MAX`. Other region validity, pointer arithmetic, lifetimes and ownership are caller
responsibilities. Only allocation base pointers may be released. A zero-byte allocation may return a releasable
allocation or null. Borrowed string bytes must not be mutated or released through the core API.

Reads and writes expose short operations directly. `stdlib/io` provides Reader/Writer, buffering, complete-transfer loops
and structured results; see [stdlib/io/README.md](../io/README.md). The older stdio package remains pending migration. Error values are platform-dependent; portable callers test for
a negative result. Windows operations currently accept counts up to `UINT32_MAX`, and use the runtime's descriptor
table. Linux uses direct syscalls. The supported portable open flags are `CORE_READ_ONLY`,
`CORE_WRITE_ONLY`, `CORE_READ_WRITE`, `CORE_CREATE`, `CORE_TRUNCATE` and
`CORE_APPEND`; combine one access mode with the desired flags by addition. Permissions are Linux mode bits and are
ignored by the Windows implementation.

Exit and trap have the uninhabited return type `never`. Their calls end the current control-flow path and therefore
satisfy definite-return analysis. Immediate process termination still bypasses normal scope and package cleanup.

## Atomics

`core.atomicBit(initial)` and `core.atomicUsize(initial)` construct move-only `AtomicBit` and `AtomicUsize` owners.
Both provide `load`, `store`, `swap` and `compareExchange(expected,next)` with sequentially consistent ordering.
`swap` and `compareExchange` return the previous value; compare it with `expected` to determine exchange success.
Storage must remain aligned and must not be accessed through a non-atomic alias while shared. These wrappers do not
require the `async` feature. See [LANGUAGE_SPEC.md](../../LANGUAGE_SPEC.md) for the memory and Send/Sync contract.

## Typed allocation and borrowed views

The ordinary generic functions in `stdlib/core/allocation.dmm` provide:

```dmm
var item:*i32 = core.alloc<i32>();
var items:i32[] = core.alloc<i32>(32);
core.release(item);
core.release(items);
```

`alloc<T>` uses `sizeof(T)` and zero-fills successful allocations. Failure returns
`core.null<T>()`. `alloc<T>` checks count multiplication before allocating; zero, overflow and allocation failure return
a null slice with length zero. Its elements use the queried backend layout, including aggregate padding.

Slices can be stored and returned, and `.data` and `.length` expose their pointer and `usize` count. Fixed-array
conversion and `slice(pointer,count)` borrow storage. Copying a slice copies its descriptor; it neither copies elements
nor transfers ownership. Release the original allocation base exactly once. Borrowed array views, subviews and
descriptors copied from an already released allocation must not be released. Bounds checks do not establish region
validity or track lifetimes.

The compiler provides layout queries, descriptor operations, bounds checks and explicit raw pointer casts. Typed
allocation, ownership conventions and future buffers and collections are implemented in DMM. Raw pointers remain
copyable non-owning values in the type system: `NEEDS_DROP` on a wrapper does not implicitly free a raw allocation
unless that wrapper's destructor does so. Native drop glue handles local, by-value, and package-stored owning wrappers,
but the current allocation wrappers do not declare destructors; they must therefore continue to call `release`
explicitly.

## Compiler boundary

The executor, network reactor, DNS queues, thread/event adapters, shared reference counting, typed allocation,
byte-region algorithms and stream policies live in the stdlib. The private `dmm_runtime` native library identifies
primitive symbols supplied in emitted objects; it introduces no external library by itself. Atomic operations
still require actual atomic machine instructions. This is an explicit native ABI, not an intrinsic call syntax.

The remaining source intrinsics are allocation/release, string representation access, raw descriptor I/O,
exit/trap, and generic initialize/destroy. The current allocator is shared with generated async frames and
uses OS-backed regions. Descriptor I/O still has a generated platform adapter, including the Windows descriptor
table. Migrating these requires preserving allocator compatibility and descriptor ownership; declaring an OS
function with an incompatible ABI is not a substitute. Type layout, ownership checking, async state-machine
lowering and bounds traps remain compiler responsibilities.
