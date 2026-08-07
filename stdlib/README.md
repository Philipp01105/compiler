# stdlib

Import `"stdlib"` for the common value types, owning collections and compatibility I/O helpers.
These are ordinary DMM functions and types; their ownership follows the
[language specification](../LANGUAGE_SPEC.md).

| Package | Purpose |
|---|---|
| [stdlib/core](core/README.md) | Raw memory, typed allocation, atomics, descriptors and process primitives |
| [stdlib/stdio](stdio/README.md) | Synchronous streams, buffering and bounded byte I/O |
| [stdlib/core/net](core/net/README.md) | Async TCP/UDP, addresses, DNS, deadlines and confirmed cancellation |
| [stdlib/native](native/README.md) | Raw target-specific OS/CRT bindings |

## Value types and interfaces

[foundation.dmm](foundation.dmm) provides:

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

## Owned collections

[collections.dmm](collections.dmm) provides move-only owners with destructors that release their allocations exactly
once. Check `ok()` or the public `error:AllocationError` after construction and growth. Error variants are `None`,
`OutOfMemory` and `CapacityOverflow`.

| Constructor | Owner and operations |
|---|---|
| `bytes(capacity:usize) -> Bytes` | Growable bytes: `length`, `capacity`, `view`, `push(u8)` |
| `buffer<T>(count:usize) -> Buffer<T>` | Fixed-length elements: `length`, `view`, `get`, `set` |
| `list<T>(capacity:usize) -> List<T>` | Growable sequence: `length`, `capacity`, `view`, `get`, `set`, `push` |
| `cloneString(text:string) -> String` | Owned string: `length` in bytes and `view() -> string` |

`Bytes.push` and `List.push` return `AllocationError`. `Buffer<T>` and `List<T>` currently require copyable element
types; dynamic element destruction is unsupported. Slice/string views borrow their owner. Moving, replacing,
destroying or relocating that owner while the view is live is rejected. These owners differ from
[`stdio.Bytes`](stdio/README.md), which requires explicit release.

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
