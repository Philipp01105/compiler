# stdlib

Import `"stdlib"` for the common value types, owning collections and compatibility I/O helpers.
These are ordinary DMM functions and types; their ownership follows the
[language specification](../LANGUAGE_SPEC.md).

Import public packages only; platform-specific source files are selected
automatically from the compiler target (ELF/Linux or COFF/Windows):

```dmm
import (
    "stdlib/fs"
    "stdlib/path"
);
```

No platform-specific import or precompiled stdlib bundle is needed.

| Package | Purpose |
|---|---|
| [stdlib/algorithms](algorithms/README.md) | Slice search, heapsort, comparison, transformations and reductions |
| [stdlib/numeric](numeric/README.md) | Checked 64-bit arithmetic, radix parsing and formatting |
| [stdlib/limits](limits/README.md) | Typed minimum/maximum constants for bounded scalar primitives |
| [stdlib/binary](binary/README.md) | Bounded binary cursors and endian encoding |
| [stdlib/collections](collections/README.md) | Deque, HashMap and HashSet |
| [stdlib/memory](memory/README.md) | Checked Box payload borrows and byte arenas |
| [stdlib/text](text/README.md) | Byte-based text operations and StringBuilder |
| [stdlib/unicode](unicode/README.md) | Unicode 17 classification, casing and normalization |
| [stdlib/path](path/README.md) | Lexical POSIX/Windows paths |
| [stdlib/fs](fs/README.md) | Owning files, directories and metadata |
| [stdlib/env](env/README.md) | Process environment |
| [stdlib/time](time/README.md) | Durations, clocks and blocking sleep |
| [stdlib/random](random/README.md) | Deterministic PRNG and OS entropy |
| [stdlib/math](math/README.md) | Double math and IEEE classification |
| [stdlib/core](core/README.md) | Raw memory, typed allocation, atomics, descriptors and process primitives |
| [stdlib/stdio](stdio/README.md) | Synchronous streams, buffering and bounded byte I/O |
| [stdlib/core/net](core/net/README.md) | Async TCP/UDP, addresses, DNS, deadlines and confirmed cancellation |
| [stdlib/core/executor](core/executor/README.md) | DMM scheduler, workers, wakers and Future adapters |
| [stdlib/sync](sync/README.md) | Owning mutexes and checked non-Send guards |
| [stdlib/native](native/README.md) | Raw target-specific OS/CRT bindings |

## Value types and interfaces

[foundation.dmm](foundation.dmm) publicly re-exports Option, Result, Propagation, NoneResidual and AllocationError
from the independent [types package](types/types.dmm), and provides the root interfaces and Cell:

| Declaration | Contract |
|---|---|
| `Option<T>` | `Some(T)` or `None` |
| `Result<T,E>` | `Ok(T)` or `Err(E)` |
| `Propagation<O,R>` | `Continue(O)` or `Break(R)` |
| `NoneResidual` | Residual used by `Option` propagation |
| `Propagate<O,R>` | Static `branch(Self) -> Propagation<O,R>` requirement |
| `FromResidual<R>` | Static `fromResidual(R) -> Self` requirement |
| `Cell<T>` | Public `value:T`, `get() -> T` and `set(T)` |
| `Printable` | `toString() -> string` requirement |
| `Equal` | `equal(Self) -> bit` requirement |
| `unwrapOr<T>(Option<T>, T) -> T` | Extracts `Some`, otherwise returns the fallback; both arguments evaluate before the call |
| `printValue<T:Printable>(T) -> void` | Prints the result of `toString()` |

`Option` and `Result` implement the contracts used by
[`?` propagation](../LANGUAGE_SPEC.md#control-flow-and-expressions).

[functional.dmm](functional.dmm) adds `mapOption<T,U>`,
`andThenOption<T,U>`, `mapResult<T,U,E>`, `mapError<T,E,F>` and
`andThenResult<T,U,E>` with ordinary function values or explicit capturing closures.
Option and Result support owning payloads and consuming matches, including fallible
owner constructors.
`optionOr<T>(value,fallback)`
invokes its `once func()->T` fallback only for None, unlike eagerly evaluated unwrapOr.
All these callbacks accept shared or mutable closures and consume their callback
binding when invoked. Unused callbacks still destroy their owned captures. Moving an
owned capture in the body infers a consuming closure and transfers that capture once.
Monomorphic callback signatures participate in type inference.

## Owned collections

[collections.dmm](collections.dmm) provides move-only owners with destructors that release their allocations exactly
once. Check `ok()` or the public `error:AllocationError` after construction and growth. Error variants are `None`,
`OutOfMemory`, `CapacityOverflow` and `InvalidIndex`.

| Constructor | Owner and operations |
|---|---|
| `bytes(capacity:usize) -> Bytes` | Growable bytes: `length`, `capacity`, `view`, `push(u8)` |
| `buffer<T>(count:usize) -> Buffer<T>` | Fixed-length elements: `length`, `view`, `get`, `set` |
| `list<T>(capacity:usize) -> List<T>` | Growable sequence: `length`, `capacity`, `view`, `get`, `set`, `push` |
| `cloneString(text:string) -> String` | Owned string: `length` in bytes and `view() -> string` |

`Bytes.push` and `List.push` return `AllocationError`. `List<T>`, Deque and HashMap accept owner
payloads; copying accessors require `core.Copy`. `Buffer<T>` remains Copy-only. Checked getRef/getMut
borrows prevent relocation while used. See [collections](collections/README.md).
`stdlib/sync` provides `Mutex<T:core.Send>` and a non-Send owning guard; see [sync](sync/README.md).

## Output and compatibility I/O

[print.dmm](print.dmm) and [integer.dmm](integer.dmm) define `print(value)` and `println(value)` overloads for strings
and numeric primitives. `println` appends LF; `println("")` writes an empty line. Arguments evaluate before output.
These convenience functions do not return I/O errors; use streams when errors or partial progress matter.

[io.dmm](io.dmm) provides legacy input and descriptor helpers:

| Declaration | Contract |
|---|---|
| `Input` | `init()` and `readInt`, `readChar`, `readString` compatibility scanning |
| `System` | `in`, `out`, `err`, with public descriptor field `fd:int` |
| `open_read(path)` / `open_write(path)` | Returns a descriptor or a negative error; write opens with create/truncate |
| `close_file(fd)` | Closes a descriptor and returns the core close result |
| `write_str_to(fd,text)` | Returns bytes written on success or a negative failure |
| `write_file(path,text)` | Writes the string and closes the file; returns zero on success or a negative failure |
| `read_file(path,buffer:char[],capacity:int)` | Reads up to the requested capacity and closes; returns count or a negative failure |

`read_file` rejects negative capacity or capacity beyond the slice length. Strings use their DMM byte length;
use [stdlib/stdio](stdio/README.md) for binary data, explicit transfer status and buffered I/O.
