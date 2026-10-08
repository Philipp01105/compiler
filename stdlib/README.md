# Standard library

Import packages by responsibility. Platform suffix files are selected for the compiler target: ELF/Linux or COFF/Windows. The root import forwards canonical core contracts only.

| Package | Public responsibility |
|---|---|
| [core](core/README.md) | Copy/Send/Sync, Option/Result/propagation, AllocError, Iterator and consuming helpers |
| [collections](collections/README.md) | Owning List, Buffer, Map and Set |
| [iter](iter/README.md) | Eager iterator composition and allocation-free reduction |
| [text](text/README.md) | Validated owning String, checked Text and Builder |
| text/utf8, text/utf16 | Small strict codecs independent of Unicode tables |
| [bytes](bytes/README.md) | Byte search, splitting, copying and replacement |
| [io](io/README.md) | Reader/Writer, standard handles, buffering and complete transfers |
| [fs](fs/README.md), [path](path/README.md) | RAII files/directories and lexical paths |
| [process](process/README.md) | Owned arguments, environment, cwd and direct child execution |
| [net](net/README.md) | TCP streams/listeners, UDP sockets and DNS |
| [async](async/README.md) | join/select/race, sleep and timeout |
| [time](time/README.md) | Duration, Instant, shared Deadline and wall clock |
| [memory](memory/README.md), [sync](sync/README.md) | Box/Shared and mutex guards |
| [algorithms](algorithms/README.md), [binary](binary/README.md) | Borrowed slice algorithms and bounded cursors |
| [numeric](numeric/README.md), [limits](limits/README.md), [math](math/README.md) | Checked integer operations, scalar bounds and floating math |
| [unicode](unicode/README.md), [random](random/README.md) | Unicode properties/normalization and random sources |

Explicit advanced imports are memory/raw (allocation and lifetime), io/raw (descriptors), sync/atomic, async/poll, net/raw, process/native and native platform bindings. Compiler-facing scheduler, reactor, threading and startup providers live under internal; application imports of those packages are rejected.

## Ownership, allocation and errors

Ordinary owners destroy their contents and storage once. Checked getters and adapters retain their backing owner; conflicting mutation, relocation or destruction is rejected. List/Buffer support Copy iteration, shared iteration and per-iteration mutable borrowing. Map policies borrow keys and cannot capture an environment.

| Operation | Allocation and failure |
|---|---|
| list() | Valid allocation-free empty owner |
| append/insert/ensureCapacity | Result; failed growth preserves stored contents and destroys consumed incoming values |
| map()/set(), text construction, Box/Shared/Mutex | Result; no failed-object status |
| get/getMut | Checked reference; bounds violation traps |
| io complete-transfer helpers | Result with total transfer progress; short operations are normal |
| fs readFile/readDir | Result with owning output and explicit bounds |
| process.args/env/cwd | Independent owning UTF-8 snapshots; invalid encoding is an error |
| net operations | Future<Result>; socket/buffer loans persist until completion or acknowledged cancellation |
| async frame construction and runtime resources | Existing fatal allocation contract; ordinary Results do not make these allocations recoverable |

Use core.AllocError.OutOfMemory/CapacityOverflow for allocation-only APIs. Richer packages expose their own Error; success is Result.Ok. EOF/absence uses Option. Primitive string concatenation remains a language operation; use text.StringBuilder for RAII owned construction. Raw generated-allocator pointers must be released through memory/raw, and native malloc pointers through their native allocator.

## Example

~~~dmm
package main;
import ("stdlib/core" "stdlib/collections" "stdlib/io");
func run()->core.Result<void,core.AllocError> {
    var values=collections.list<int>();
    values.append(42)?;
    for(var &value=values){match(io.println(*value)){Ok=>{}Err(error)=>{}}}
    return core.Result<void,core.AllocError>.Ok;
}
func main()->int {match(run()){Ok=>return 0;Err(error)=>return 1;}}
~~~

See [compiled examples](../examples) and [standard-library contracts](../tests/stdlib/README.md).
