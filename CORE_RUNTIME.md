# Core runtime boundary

`import "stdlib/core";` loads the low-level DMM package independently of the higher-level standard library. It provides
ordinary typed DMM functions over compiler intrinsics. It participates in the module/package visibility model.
The separate `stdlib/core/net` package provides move-only sockets/address lists and typed asynchronous TCP/UDP/DNS
over the private platform runtime. Its API and ownership/completion contract are in
[NETWORK_RUNTIME.md](NETWORK_RUNTIME.md).

```dmm
package main;
import "stdlib/core";

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

## Primitive interface

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
| `core.core_string_length(text:string) -> usize`                  | Traverse the borrowed string in DMM. A null string has length zero.                           |
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

Reads and writes expose short operations directly. `stdlib/stdio` implements streams, buffering, complete-transfer loops
and structured results in DMM; see [STDIO.md](STDIO.md). Error values are platform-dependent; portable callers test for
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
require the `async` feature. See [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md) for the memory and Send/Sync contract.

## Compiler and library responsibilities

`src/common/core_intrinsics.h` is the target-independent signature table shared by semantic analysis, IR verification
and backend call mapping. Intrinsic source names use the reserved `__dmm_intrinsic_` prefix and cannot be redefined by
DMM functions. Byte pointers have an explicit `u8` pointee; unrelated pointers and arrays/slices are rejected. Numeric
operands use the language's integral conversions, performed at the call boundary. Use the public wrappers to expose
`usize` byte counts and `isize` offsets/results in source APIs.

The compiler owns startup, ABI handling, raw allocation/release, byte copy/fill, platform I/O and traps. Byte access,
string traversal and fixed-width integer output are implemented in DMM. `stdlib/integer.dmm` formats all fixed-width
integers into a stack byte buffer using unsigned division, including signed minimum values, then writes the bytes
through core I/O.

Existing scanning, string concatenation and floating formatting retain their compatibility runtime implementations.
Executable packages still embed that runtime; this first core interface does not yet selectively link runtime routines.

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
