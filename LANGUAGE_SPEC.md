# DMM Language Specification 2026-09-22-dev

- [Source and declarations](#source-and-declarations), [modules and packages](#modules-and-packages)
- [Types](#types), [expressions and control flow](#control-flow-and-expressions), [ownership](#derived-ownership-and-destruction)
- [Generics, interfaces and sum types](#compile-time-abstraction-and-sum-types)
- [Native FFI](#native-ffi), [implementation limits](#implementation-limits) and [grammar](#grammar)

Status: experimental. This document defines the tested source-language contract; undocumented behavior may change.

## Edition and compatibility

The current language edition is `2026-09-22-dev`. Edition identifiers use a real Gregorian date in the form
`YYYY-MM-DD`, optionally followed by `-dev`, `-pr`, `-prerelease`, `-rc`, or `-releasecandidate`. The paired short and
long prerelease and release-candidate suffixes have the same meaning. Years use four digits; months and days use two.
Malformed dates are rejected.

Every module declares its edition with `dmm 2026-09-22-dev`. The compiler accepts only its exact current edition,
including the suffix. DMM is in rapid development and currently provides no backwards-compatibility, migration, or
compatibility-mode guarantee. A stable compatibility policy is deferred until the first stable release.

Experimental features are project-wide and are enabled only by the root module's `dmm.manifest`. An omitted
`features` directive and `features = []` are equivalent. Feature names use lowercase ASCII letters, digits, and
underscores, beginning with a letter. Unknown, malformed, and duplicate feature names are errors. Source files cannot
enable features, and normal command-line builds cannot override the manifest.

## Source and declarations

A source file is UTF-8 text beginning with `package name;`, followed by imports, structs, enums, interfaces, constants,
package variables and functions. Statements and imports end with `;`. `//` introduces a line comment; `/* ... */`
introduces a non-nesting block comment that may span lines. Unclosed block comments are lexical errors. Execution begins
in a parameterless `main` returning `void` or `int` in `package main`; library packages need no entry point.

Projects use `dmm.manifest`; directories define packages, quoted imports bind packages per source file, and `pub`
controls exported declarations and members. Imported nominal types retain their package identity. The complete manifest,
discovery, visibility, `internal`, vendoring and `dmm manifest sync` contract is defined in
[Modules and packages](#modules-and-packages).

The root manifest enables experimental async support with `features = ["async"]`. With that feature enabled,
an `async func (...) -> T` call has type `Future<T>`, while its body returns `T`. `Future<T>` is a compiler-owned type
with exactly one output type; `Future<void>` is valid. The consuming instance method `future.await()` is only valid
inside an async function, takes no arguments, and produces the Future's output. For example,
`var x:Future<int>=compute(); var y=x.await();` consumes `x` and gives `y` type `int`.
The prefix form `await x` is not part of the language and is rejected. Futures are move-only, including when transferred through function values
or generic functions. References to Futures cannot be awaited. Future output types are invariant: `Future<int>` does
not convert to `Future<byte>` even though the corresponding scalar conversion exists.

A live Future must be awaited, transferred, synchronously consumed with `block_on`, or passed to `cancel` on every
reachable control-flow path. The cancellation Future must itself be consumed to completion. Discarding a Future expression,
overwriting a live Future, or leaving its scope without consumption is an error. This requirement also propagates
through aggregate fields and enum payloads. Existing restrictions on partial moves from aggregates still apply.
Futures that capture checked reference parameters retain their loans across handle moves and forwarding calls;
conflicting access to the origin is forbidden until consumption. A Future cannot escape a local borrow's scope or
return a borrow of a local value. A borrowed await result remains tied to its origin. Async instance methods retain
a loan on their receiver. References contained in captured aggregates retain their origins as well. A temporary
owning slice cannot be captured as a borrowed parameter; bind its backing to an owner before constructing the Future.

Each async call allocates its frame on the heap before returning the handle; it does not execute the function body.
The frame is pinned from construction until cleanup. Moving a handle does not move its storage. Lowering emits a
constructor, poll callback and cleanup callback, with persistent state, result storage, local values and drop flags.
Lowering conservatively retains all frame storage across suspension. Await evaluates its operand once, polls its child,
suspends on Pending, and resumes at its unique verified state. On Ready it moves the output and cleans up the child
exactly once. Normal return and error propagation run the existing defer/destructor paths exactly once. Invalid
resume states, polling a completed frame, and cleanup of an incomplete frame trap in the private ABI.
Both ELF and COFF native backends support this at O0 and O1. A private deterministic test driver exercises polling.
`main` remains synchronous. Typed asynchronous TCP/UDP/DNS is available through `stdlib/core/net`; public `stdlib/net`
and asynchronous file APIs remain future work. See [stdlib/core/net/README.md](stdlib/core/net/README.md).

`Send` and `Sync` are compiler-derived, structurally through aggregate fields and enum payloads. Shared checked
references are Send/Sync when their referent is Sync; mutable checked references are Send when their referent is Send.
Raw pointers, slices without a checked owner, function values and dynamic interface values are conservative.
A Future constructor receives Send only when its complete retained frame and output are Send. Futures with borrowed
parameters or an instance receiver are not Send and cannot become spawn candidates. Unknown/erased Future captures
remain conservative; `Future<T>` alone does not prove Send from `T`. Futures are not Sync because polling mutates them.
An enclosing future may nevertheless be Send with **frame-internal loans**: every loan origin must be owned within
the same pinned future ownership graph, referenced storage must remain address-stable for the entire loan, and
ordinary Send/Sync and exclusive-access rules still apply. Checked slice views trace to their storage owner. This
conditional graph proof does not make a borrowed child independently Send; extracting/spawning it, externally
borrowed parameters and caller-stack origins remain excluded. Owned sockets and fixed arrays can therefore be lent
to an embedded I/O future inside a spawnable parent. Move, destruction and conflicting accesses remain forbidden
until confirmed completion or cancellation. See [stdlib/core/net/README.md](stdlib/core/net/README.md).
There are no explicit unsafe Send/Sync implementations.

The executor exposes the following operations with the `async` feature enabled:

| Operation | Result |
| --- | --- |
| `Executor.create(workerCount:usize)` | owned `Executor`, at least one worker |
| `executor.spawn(future:Future<T>)` or `spawn(future)` | owned `JoinHandle<T>` |
| `executor.block_on(future:Future<T>)` or `block_on(future)` | `T`, synchronous functions only |
| `executor.shutdown(ShutdownMode.Drain)` | consuming `Future<void>` |
| `executor.shutdown(ShutdownMode.Cancel)` | consuming `Future<void>` |
| `handle.await()` | `Result<T,TaskError>` |
| `cancel(future:Future<T>)` or `cancel(handle:JoinHandle<T>)` | consuming `Future<void>` |

`TaskError` initially has only `Cancelled`. Join results use `Ok(T)` and `Err(TaskError)`;
`Result<void,TaskError>.Ok` has no payload. A consuming match on the async result transfers
owned payloads to its bindings; every variant must bind its payloads, and bindings inherit
the ordinary consumption requirements. To wait synchronously for a JoinHandle, call a small
async function that awaits it through `block_on`.

Futures, JoinHandles and Executor owners are move-only. Every reachable path must consume them;
a completed but unclaimed JoinHandle cannot be dropped implicitly. Shutdown consumes the Executor
and closes admission immediately. Drain lets admitted tasks finish normally; Cancel requests their
cooperative cancellation. Both operations await I/O confirmation, cleanup and worker termination.
JoinHandles remain independent owners after shutdown and must still be consumed. Published results
remain available; cancelled tasks yield `Err(TaskError.Cancelled)`.

Spawn checks the concrete Future value's complete retained frame and output for Send; the type
`Future<T>` alone is insufficient. Borrowed parameters exclude spawn. Local blocking polls execute
on the caller's thread and admit borrowed and non-Send futures. Cancel transfers existing loans into
its returned operation. The origin remains inaccessible until that operation completes, including
any delayed I/O cancellation and asynchronous cleanup. Requesting cancellation never releases a loan.
Loans also follow pending Futures through `Result` construction, moves, forwarding calls and consuming
matches; cancelling a bound payload releases its loan only after the cancellation Future completes.

The scheduler uses a synchronized ready queue and parks idle workers. A frame is never polled
concurrently. Notifications during a poll survive a Pending return; notifications may coalesce but
cannot be lost. Completion and cancellation requests select one winner under the same lock. Published
Ready results win over later shutdown cancellation; explicit handle cancellation destroys unclaimed
results exactly once, recursively finishing owned child futures before destruction.

Cancellation enters a separate generated cleanup path at a poll/suspension boundary; synchronous
code already running is not forcibly interrupted. Only entered scopes contribute defer actions.
Children and pending I/O terminate before their frames, borrowed buffers or OS handles are released.
Active destructors and defers run once in the existing order. Awaitable defer actions keep polling
until completion, including an action already suspended when cancellation arrives. Cancellation of
a cancellation or shutdown Future still completes its operation. IR validation checks pinned storage,
unique suspend states, cancellation edges, cleanup effects and consuming transitions.

Native scheduling, threads, events and the private I/O handshake are emitted for ELF and COFF by
the standalone runtime, without an external linker handoff. `src/runtime/executor.c` also implements
the private contract for deterministic concurrency tests. I/O request and confirmed termination are
separate events; frame and adapter references keep the operation alive through confirmation delivery.
Free spawn and block_on lazily initialize a default executor with two workers. Normal process exit
drains it before package cleanup. Explicit executors are independent. Resource failures follow the
existing fatal runtime path. There are no detached tasks, forced thread aborts or cancellation deadline:
non-cooperative synchronous code or missing I/O confirmation may delay cancellation/shutdown indefinitely.

The optional platform profile replaces thread/event primitives with pthreads on Linux or UCRT64 `_beginthreadex`
on Windows while retaining the generated scheduler. Used network operations select this profile and its combined
shim automatically; `--link=external` also selects the platform profile without requiring networking. See
[ARCHITECTURE.md](ARCHITECTURE.md#native-x86-64-backend) and [ARCHITECTURE.md](ARCHITECTURE.md#executor-runtime-and-native-abi).

The `core.AtomicBit` and `core.AtomicUsize` types are available independently of `async`. Construct them with
`core.atomicBit(initial)` and `core.atomicUsize(initial)`. Their `load`, `store`, `swap`, and
`compareExchange(expected, next)` methods all use sequentially consistent ordering. `compareExchange` returns the
previous value, whether or not the exchange succeeded; compare that value with `expected` to determine success.
On x86-64, an aligned word load is atomic, while stores and swaps use the implicitly locked memory `xchg` and
compare-exchange uses `lock cmpxchg`. All operations participate in a single sequentially consistent order compatible
with program order. The atomic wrapper types are move-only so they cannot be copied by ordinary aggregate assignment.
Their storage must not be accessed through a non-atomic alias while it may be shared. The async executor uses native
worker threads; a separate public thread-spawning API is not provided.

## Modules and packages

DMM 2026-09-22-dev uses one module per project, one package per directory, and one or more
`.dmm` source files per package. Subdirectories are separate packages.

```text
project/
├── dmm.manifest
├── cmd/compiler/main.dmm       package main;
├── cmd/formatter/main.dmm      package main;
├── lexer/lexer.dmm             package lexer;
├── lexer/scanner.dmm           package lexer;
└── internal/buffer/buffer.dmm  package buffer;
```

The compiler searches upward from the selected source file or package directory for `dmm.manifest`. A manifest is not
DMM source code. Its module path is the project's global identity; the directory relative to its root completes a
package's identity. Nested module roots are excluded from the enclosing module's packages.

```text
module github.com/example/compiler

dmm 2026-09-22-dev

features = []

require (
    github.com/example/collections v1.2.0
)
```

### Manifests and features

There must be exactly one `module` directive and one `dmm` directive. Module and package paths are case-sensitive
slash-separated identifiers. Empty components, `.` and `..`, backslashes and absolute paths are invalid. Edition syntax,
validation and compatibility policy are defined in [Edition and compatibility](#edition-and-compatibility); examples use the current
edition in its examples.

The optional `features = [...]` directive contains quoted experimental feature names. It may span lines, permits a
trailing comma, and is equivalent to omission when empty. Names begin with a lowercase ASCII letter and continue with
lowercase letters, digits, or underscores. Unknown and duplicate features are rejected. The manifest is authoritative;
source files and normal command-line builds cannot override it.

The currently supported experimental feature is `async`. Set `features = ["async"]` in the root manifest to use
`async func`, `Future<T>`, `JoinHandle<T>`, `Executor` and `stdlib/core/net`. Imported source uses the root feature
configuration. See [Source and declarations](#source-and-declarations) for the async source contract.

Dependency versions currently use `vMAJOR.MINOR.PATCH`. Duplicate requirements and malformed directives are rejected.
`//` comments, non-nesting `/* ... */` block comments and single-line `require path version`
are supported. An unclosed block comment invalidates the manifest.

Editor package hover text is read from its enclosing module's manifest. Start a block with the exact source package name as its
first word. Each block documents one package; editor hovers select only the matching block, without merging other
comments:

```text
/* lexer
Tokenizes DMM source files.
Provides tokens and source positions for the parser.
*/
```

Function documentation is the immediately preceding `//` comment group or
`/* ... */` block in the source file. JetBrains hovers show it with the declared signature, including generic
parameters, parameter types and the return type.

### Dependencies and manifest synchronization

External dependencies are local for now. `require github.com/example/collections
v1.2.0` authorizes loading that module from
`vendor/github.com/example/collections/`, which must contain its own matching
`dmm.manifest`. Missing requirements, missing vendored sources and conflicting module identities are errors. Transitive
requirements are read from each dependency's manifest and use the same project-root `vendor/` directory. The version
records the requested dependency; there is no network fetch, version selection, checksum validation or lockfile yet. The
bundled
`stdlib` module is available independently of project requirements.

Use one command to reconcile dependencies:

```sh
dmm manifest sync
dmm manifest sync path/to/package
```

The command finds the enclosing module and scans all of its packages, including libraries and every executable under
`cmd/`. Nested modules, hidden directories and unimported vendor packages are excluded. It adds used transitive modules,
marks them with `// indirect`, promotes directly imported modules to ordinary requirements and removes unused
requirements. Direct means imported by any source package in the project; indirect means imported only by its
dependencies.

Versions come from existing project and dependency requirements. Missing explicit versions, unavailable vendor sources,
invalid packages, import cycles and conflicting exact versions abort synchronization without changing the project
manifest. This command does not fetch packages or select versions. Build commands read manifests and do not rewrite
them. `--formatError` provides structured failure diagnostics.

Synchronization writes a sorted, normalized manifest and replaces the original only after successful graph analysis and
writing. Block comments are preserved and placed before the directives, so package documentation survives
synchronization. Other formatting and comments except `// indirect` are not preserved. Repeated synchronization is
deterministic.

```text
require (
    example.com/direct v1.0.0
    example.com/transitive v2.0.0 // indirect
)
```

### Discovery, imports and visibility

Every source file begins with `package name;` before imports or other declarations. Comments and whitespace may precede
it. All source files directly in a package's directory must have the same local name. Discovery includes every regular
`.dmm`
file directly in that directory subject to target-specific selection below, including an ordinary file named `package.dmm`;
that filename has no special role.
Other extensions and subdirectories are ignored. Files are parsed before package symbol collection, and private
declarations are visible throughout their package, regardless of source-file order.

```dmm
package parser;

import lex "github.com/example/compiler/lexer";

pub func tokenKind(token:lex.Token) -> int {
    return token.kind;
}
```

Imports bind a package in the importing source file. Without an explicit alias, the binding uses the imported package's
declared local name. Access is qualified:
`lexer.Token`, `lexer.tokenize(...)`, or `lex.Token` with an alias. Imports in one source file do not create import
bindings in another file of the same package. The existing grouped form remains available:
`import ("path/a" b "path/b");`. Dot imports, blank aliases, duplicate aliases and file imports are unsupported.

Declarations and members are private by default. `pub` applies to functions, structs, enums, interfaces, constants and
package variables. Fields and methods require their own `pub`; exporting a struct does not export its private members.
Enum variants also require `pub`, for example:

```dmm
package result;

pub enum Result<T,E> { pub Ok(T), pub Err(E), }
pub struct Cell<T> {
    pub var value:T;
    var cached:int;
    pub func get() -> T { return value; }
}
pub var count:int = 0;
```

Ownership is part of the resolved concrete type, not its export spelling. Imported structs therefore retain their
derived `COPYABLE`/`MOVE_ONLY` and independent `NEEDS_DROP` properties across package boundaries. Generic structs
derive those properties separately for each concrete specialization after type substitution.

### Initialization and package identity

For an executable main package, package-variable initialization follows the resolved package graph: dependencies run
before importers, independent packages are ordered by canonical package path, and files/declarations retain their
deterministic loader order. Each package is initialized once and `main` runs last. Direct package-variable dependency
cycles are rejected; import cycles continue to report their complete package path.

Package variables have shared writable storage. Their initializers may contain arbitrary well-typed runtime expressions;
compile-time primitive, string and fixed-array values remain static data, and explicitly typed variables without an
initializer begin as zero. Runtime-created slice backing and move-only values transfer into package storage. Moving an
owner back out of package storage remains rejected; borrowing and in-place reassignment follow the ordinary borrow and
exactly-once drop rules.

For an executable main package, initialized owners participate in compiler-generated normal-exit cleanup in reverse
initialization order. Slice backing owned by a package variable is released by the same cleanup path. Library packages
do not emit executable startup.

Only `package main` is executable. It requires exactly one non-generic, parameterless `main` returning `int` or `void`.
Other packages are libraries and can emit assembly or relocatable objects without an entry point. Executable builds of a
library are rejected; executable packages cannot be imported. Multiple
`cmd/...` packages named `main` can coexist in one module.

For every `internal` path segment, the importing package must be within the subtree of that segment's parent and belong
to the same module. Thus
`example.com/lib/io/internal/syscall` is accessible under `example.com/lib/io`, but not from `example.com/lib/parser` or
another module. Package import cycles are compile-time errors and report the complete path through the cycle.

Symbol identity includes the canonical package path (which includes the module path), the declared name and, where
applicable, the owner type and overload or specialization signature. Identically shaped types from different packages
remain nominally distinct. Backend names encode canonical package identity; public DMM names are not automatically
unmangled C names. Interoperability callers must use the generated link name. Generic identities use declaration names
and signatures, not source-file token positions.


### Target-specific files

Before parsing, the loader selects `_linux.dmm` files for ELF and `_windows.dmm` files for COFF; unsuffixed files are
shared. Selection uses the output target, including cross-compilation. Manifest synchronization scans both variants
so dependencies remain available for either target.

## Types

The primitive types are `int`, `char`, `byte`, `bit`, `float`, `double`, `string`,
`i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `isize`, `usize`, `void`, and `never`. Fixed-width integer names denote
their signedness and width; `isize`/`usize` are signed/unsigned pointer-width integers (64 bits on both supported
targets). Existing `int`, `char` and `byte` retain their 32-bit signed, 8-bit signed and 8-bit unsigned memory/ABI
representations. The new spellings are distinct primitive types for overload resolution and generic specialization.
`void` is valid only as a function return type. `never` is an uninhabited bottom type used as the direct return type of
functions that cannot complete normally. It has no runtime value and is therefore invalid for variables, parameters,
fields, payloads, pointers, arrays, and slices. A `never` expression is compatible with every expected result type
because evaluation cannot reach the consuming operation. A `never` function must not use `return` and every path must
terminate. Such paths satisfy definite-return analysis for surrounding value-returning functions. Fixed arrays use
`var name:type[length];`. Array and slice postfixes may
be repeated and are applied from left to right: `int[2][3]` is an outer length-3 array whose elements are length-2
arrays, while `int[2][]` is a slice of length-2 arrays. Repeated prefix stars form pointers, such as `**int`; grouped
types distinguish `*(int[4])` from `*int[4]`. Whole-array assignment is supported when both sides have the same complete
fixed-array type.

Function types describe non-capturing callable values. A monomorphic type such as
`func(int,string) -> bit` is one machine-word function address. A polymorphic type such as
`func<T:Printable,U>(T,U) -> T` is a compile-time template identity whose type-parameter names are alpha-renamed when
types are compared. A source template is compatible with a target when their parameter and return shapes match and
the source requires no bounds beyond those guaranteed by the target.

A bare top-level function name, static method, or type-qualified instance method is a callable value. An unbound
instance method takes `*Owner` as its first parameter; selecting an instance method from a value remains invalid.
Overloaded names require an expected function type. Generic functions may remain polymorphic (`var f=identity`), be
explicitly specialized (`identity<int>`), or specialize from a monomorphic expected type
(`var f:func(int) -> int=identity`). Calls through a null monomorphic callable trap. Callable return types may be
`never`; calling one terminates the current control-flow path. A `void` call only produces no value and continues
normally, so the two types are not interchangeable.

Monomorphic callable values may be stored in locals, constants, package variables, fields, enum payloads, arrays, and
slices, and may be passed and returned. They compare only with `==` and `!=`, and only at the same signature.
Polymorphic callable identities are compile-time-only: they may occur in inferred or explicitly typed local constants,
compile-time callable parameters, and callable returns, but not in runtime aggregate or package storage. A function
returning a polymorphic callable must return one template identity on every path. Passing such an identity specializes
the receiving function and erases that compile-time parameter from the emitted ABI.

`&T` and `&mut T` are checked non-owning references created by `&value` and `&mut value`. Immutable references permit
reads; mutable references are exclusive and permit reads and writes through `*reference`. While a conflicting borrow is
live, the owner cannot be moved, replaced or accessed incompatibly. Lifetimes end conservatively after the last proven
use. Struct fields and constant array indices are treated as disjoint when that can be proven; dynamic indices and
whole aggregates overlap. A postfix cast such as `reference.(*T)` explicitly crosses from a checked reference to an
unchecked raw pointer.

Checked references cannot be stored in aggregate fields, enum payloads or package variables because those locations do
not express a lifetime. A borrowed return must have one statically provable origin in a borrowed parameter or package
storage. Returning a local borrow or merging incompatible return origins is rejected.

Fixed-size array parameters such as `values:float[2]` accept arrays with exactly the declared lengths and element types
at every nesting level. They borrow the caller's storage, so element mutations are visible to the caller. Their ABI
passes one data pointer; indexing uses the statically known bound and recursively computed element stride. A slice does
not implicitly convert to a fixed-size array parameter.

`T[]` slices are non-owning, copyable views containing a data pointer and an element count. `T` may itself be an array
or slice type. They may be parameters,
local bindings, fields, enum payloads, package variables and return values. A matching fixed array converts to a slice
without copying its elements. Ordinary assignment and return copy the view; they do not duplicate the underlying
storage.
`.length:usize` and `.data:*T` are read-only properties. Indexing checks the current count, including negative indices.
`value[start:end]` creates a non-owning slice over the half-open range `[start,end)`; `value[:end]`,
`value[start:]`, and `value[:]` default the omitted bound to zero or the source length. Fixed arrays and slices may be
sub-sliced. Evaluation traps unless `0 <= start <= end <= length`.

`==` and `!=` are defined for two slices with the same complete type. They compare lengths and then the stored
element representations in sequence; therefore pointer-like elements compare their stored addresses. Slice ordering
with `<`, `<=`, `>`, or `>=` is not defined and is rejected.

Array literals are contextually typed. The surrounding declaration, assignment target, parameter, or return type must
unambiguously provide either `T[N]` or `T[]`; `var values=[1,2,3];` is therefore invalid. Examples:

```dmm
var fixed:int[4]=[0,1,2,3];
var view:int[]=[0,1,2,3,4];
consume([1,2,3]);       // consume's parameter supplies the type
fixed=[4,5,6,7];
```

Every element must convert to `T`. This includes concrete values stored in an interface array or slice when each value
implements the expected interface. `[a,b,c;N]` creates exactly the positive compile-time count `N` by cyclically
repeating the nonempty prefix and truncating its final repetition. For a fixed array, `N` (or the number of ordinary
elements) must equal the declared length; for a slice it determines the hidden backing length.

A local slice literal creates hidden owning backing storage and a visible non-owning slice. The backing lifetime is tied
to the receiving lvalue and remains live for dependent copied slices and sub-slices. A literal used only as a call
argument remains alive through the complete call. A function returning a newly created slice transfers the hidden
backing owner to the receiving context while the visible `T[]` remains a copyable view. Package slice literals use
static package-lifetime backing storage. Package array/slice literal elements currently must be compile-time non-string
primitive values because general runtime package initialization remains unsupported.

The borrow checker propagates a slice view's backing owner through local copies, aggregate fields, returned views and
calls whose returned origin is statically provable. Chained views retain the original owner. That owner cannot be moved,
reassigned or mutated while a dependent view remains live; the view's conservative lifetime ends after its last use.
For mutable borrows, different constant element indices are disjoint regions. Dynamic indices and whole slices are
treated as overlapping. A view-returning instance method on a move-only owner conservatively borrows the whole receiver
when a more precise returned field cannot yet be established.

`slice(pointer, count)` constructs a view from a typed non-void raw pointer and an integral count, including a pointer
to an array or slice element type. Zero permits a null pointer; negative counts, nonzero counts with null pointers and
byte-size overflow trap. The caller ensures the region is valid, aligned and alive. Slice parameters use pointer and
length ABI lanes in source-parameter order; stored and returned slices use a two-word descriptor.

Integral types may convert among themselves or to floating point, and `float`
may widen to `double`. Conversions to fixed-width integers keep the low bits and sign- or zero-extend according to the
target. Conversion from a fixed-width integer to `int` applies its 32-bit representation. Narrowing floating conversions
require an explicit cast. Float-to-integer conversions truncate toward zero; their input must be finite and within the
signed or unsigned 64-bit conversion domain before narrowing to smaller fixed widths.

Decimal integer literals cover `0` through `UINT64_MAX`. They infer `int` through
`INT32_MAX`, `i64` through `INT64_MAX`, and `u64` above that. Explicit postfix casts select another width. With
fixed-width operands, integer operations select the larger width; an unsigned operand wins when its width is at least
the signed operand's width. Identical operand types retain their type. Results wrap to that width, including constant
arithmetic. Legacy-only integral arithmetic retains its existing virtual-slot behavior. Division by zero traps; signed
64-bit minimum divided by -1 also traps. Comparisons and division use the selected signedness.

Casts use `value.(target)`, for example `(amount / 2.0).(int)` or `value.(byte).(int)`. Numeric casts target a numeric
primitive type; explicit pointer-to-pointer casts reinterpret addresses. The old `int(value)` form is rejected. Casts
bind as postfix expressions; parentheses group compound source expressions.

`const name[:type] = expression;` declares a top-level or block constant. Primitive and string initializer expressions
are evaluated during semantic analysis. Earlier evaluated constants may be referenced, including positive `int`
constants used as fixed-array lengths. Constants have no mutable storage and cannot be assigned, incremented, or
addressed. Legacy integer constant overflow and invalid constant division are compilation errors; fixed-width constant
arithmetic uses the wrapping rules above and is evaluated with exact integer bits.

`var name:type = expression;` at package scope may use an arbitrary well-typed runtime expression. Compile-time
primitive, string and fixed-array values are emitted directly; the remaining expressions run once before `main`.
Imported packages initialize before packages that import them, independent packages use canonical package-path order,
and declarations within a package use the loader's deterministic file and declaration order. Direct dependencies among
package-variable initializers must be acyclic. A package variable without an initializer begins as the zero value of its
explicit type.

Runtime-created slice backing and move-only values transfer into package storage. Destruction flags become live only
after an initializer succeeds. On normal return from `main`, initialized package owners are destroyed and owned slice
backing is released in reverse initialization order. The terminating `exit` intrinsic bypasses this normal-exit cleanup.

## Control flow and expressions

DMM supports blocks, `if`/`else`, `for`, `while`, `break`, `continue`, and `return`. `break` and `continue` are valid
only in loops. Every reachable path of a non-void function must return a value of the declared type.

Blocks, `if`/`else`, and `match` can also produce values. A value block ends with an expression without a semicolon:
`var total:int = { var fee:int = 2; fee + 40 };`. A value `if` requires both branches and braces:
`var label = if (ready) { "ready" } else { "pending" };`. Value `match` arms likewise use braces, for example
`var code = match (status) { Ready => { 0 } Failed => { 1 } };`. An arm or branch may instead terminate with
`return`, a `never` call, or another non-fallthrough operation. A reachable branch with no final value, or with a
`void` value, is rejected. Statement forms retain their existing syntax.

All reachable branches must have a common type under the existing implicit-conversion rules. An expected type from a
declaration, assignment, return, or direct-call parameter is applied to every branch first; otherwise the compiler
chooses a branch type to which all reachable branches convert. `never` branches do not participate, and an expression
whose branches all terminate has type `never`. Branch-local bindings leave scope after their value is evaluated;
returning a borrow of such a binding is rejected. Ownership states from only the reachable branches meet at the
join, so a move on any continuing path prevents an unchecked later use.

`defer call(...);` evaluates and retains the callee and arguments immediately, then performs the call when the current
scope exits. `defer func() { ... }` instead retains referenced locals and evaluates its body at scope exit. Deferred
actions run in last-in, first-out order on fallthrough, `return`, `break` and `continue`, before earlier enclosing-scope
cleanup. A deferred anonymous body cannot `return`, `break` or `continue` out of its enclosing control flow. Captured
borrows remain live until the deferred action runs. A borrow formed inside an anonymous deferred body is checked at its
later execution point; every captured move-only value must therefore still be initialized on each relevant scope exit.
If an intervening operation consumes such a value, the diagnostic points at the `defer` and identifies the consuming
operation as a related location.

Postfix `?` performs typed early-return propagation. Its operand is evaluated exactly once by value and must provide a
unique static `branch(Self) -> Propagation<Output,Residual>` method. A `Continue` branch evaluates to its `Output`;
a `Break` branch is converted by the enclosing return type's static `fromResidual(Residual) -> Self` method and
returned. The early return follows the normal cleanup path, so active deferred actions and derived drops run in their
usual LIFO order. Move-only operands are consumed. `?` is valid only directly in functions and methods, not in
constants, package initializers, destructors, or deferred anonymous bodies. Standard `Option` and `Result` residuals
cannot cross container kinds. For `Result<T,E>`, an error conversion is also available when
`E.fromResidual(residual)` exists.

Operator precedence, from low to high, is logical OR, logical AND, comparisons, addition/subtraction,
multiplication/division/remainder, unary operators, and primary expressions. Arithmetic is numeric; `bit` values
participate in conditions and logic but not arithmetic. Remainder is defined only for integral operands. Assignment
requires a mutable lvalue.

`string + value` concatenates strings with `string`, numeric, `char`, or `bit` values and returns an owned string. The
caller may release that result with `free`. Printing accepts scalar and string values; a null string prints as an empty
string.

Static array accesses are checked at compile time when the index is a literal and at runtime otherwise. An out-of-bounds
runtime access traps.

Address-of accepts variables and array elements. Dereference requires a pointer. Pointer arithmetic is not part of DMM
and is rejected.

Struct types are nominal and cannot be assigned merely because their layouts match. A struct with no destructor and
only copyable fields is copyable; its arguments, assignments, and return values have by-value copy semantics, including
nested structs and fixed arrays.

Named struct initializers are expressions of the form `Type{field: expression, ...}`:

```dmm
struct test { var x:int; var y:int; };
var y = test{x: 6, y: 10};
```

The initializer has type `test`, so a variable type annotation is optional. The semicolon after a complete struct
declaration is optional. Every field must occur exactly once; unknown, duplicate, missing, inaccessible, and
incompatible fields are rejected. Fields may appear in any order. Values are evaluated and stored once, in source
order, including a by-value snapshot of aggregate fields before later expressions run. A trailing comma is allowed.
Qualified and explicit generic types are supported (`bindings.Record{...}`, `Box<int>{value: 1}`), as are nested
initializers, context-typed array fields, empty structs, returns, assignments, and function arguments.

Native structs use their target layout; opaque structs cannot be constructed. A native union initializer names exactly
one field. Construction zeros storage, including padding, before field stores. Move-only fields transfer ownership
and retain normal borrow restrictions. Already initialized fields requiring destruction are cleaned up if a later
initializer exits before construction completes; the enclosing struct's destructor runs only for a completed value.
Slice fields contain views: bind temporary owning slice backing to a local owner before placing a view in a struct.
Struct initializers are runtime values, not primitive compile-time constants.

### Derived ownership and destruction

All user-defined structures use the ordinary `struct` declaration. There is no `resource struct` form. A struct may
contain one `destructor { ... }` member. Declaring a destructor makes the type both `MOVE_ONLY` and `NEEDS_DROP`.
These are separate semantic type properties: `COPYABLE` and `MOVE_ONLY` are mutually exclusive, while
`NEEDS_DROP` independently records that destruction logic must run at the end of a live value's lifetime.

Ownership and drop properties are derived transitively from fields and enum variant payloads. A struct is `MOVE_ONLY`
when it declares a destructor or contains a move-only field. It is `COPYABLE` only when every field is copyable and no explicit semantic
rule makes it move-only. A struct is `NEEDS_DROP` when it declares a destructor or contains a field that needs drop.
An enum is move-only or needs drop when any of its variant payloads has the corresponding property.
Consequently a type can be `MOVE_ONLY | NEEDS_DROP`, and move-only does not by itself imply drop. Raw pointers,
checked borrows, slices, and primitive values remain copyable non-owning values.

Properties belong to concrete types. Generic aggregate specializations derive them after substitution, so
`Box<int>` can be copyable while `Box<File>` is move-only and needs drop when `File` does.

Passing, returning, or assigning a move-only value by value transfers ownership. The source becomes moved and
uninitialized. Until a complete assignment reinitializes it, the source cannot be read, borrowed, moved again, or
destroyed. Operations that would copy a move-only value are semantic errors.

Ownership state is path-sensitive. Branch and `match` joins merge the possible live, moved, and uninitialized states.
A loop may carry a move-only owner across its backedge only when every continuing iteration leaves that owner live; a
consumed owner must therefore be completely reassigned before the next iteration. Fixed arrays inherit their element
ownership properties, array literals move move-only elements into their result, and repetition syntax cannot duplicate
a move-only pattern element. Concrete-to-interface conversion transfers move-only implementers into owned payload
storage; interface values are themselves move-only and require deterministic destruction.

Every initialized, non-moved value with `NEEDS_DROP` is destroyed exactly once on each lifetime-ending path, including
scope fallthrough, `return`, `break`, and `continue`. A moved-from value is not destroyed. For a struct with an
explicit destructor, its destructor body runs first; owned fields that need drop then run in reverse declaration order.
A struct without an explicit destructor still destroys such fields in reverse declaration order. Destructors cannot
return or move ownership out of their receiver. Cleanup observes the same checked-borrow and field-sensitive access
rules as ordinary code.

An enum that needs drop destroys only the active variant's owned payloads, in reverse payload order. Constructing such
an enum transfers move-only payload arguments into it. Match payload bindings currently copy extracted payloads, so a
move-only payload binding is rejected until consuming patterns are available.

Package variables with `NEEDS_DROP` remain live until normal executable termination. After `main` returns, the
runtime invokes compiler-generated package cleanup, which destroys initialized package owners exactly once in reverse
declaration order while preserving `main`'s exit status. Reassignment drops the previous live value and marks the new
value initialized. Moving ownership out of package storage is rejected because a single function's move analysis cannot
soundly represent package-wide moved state. The immediate `exit` intrinsic and abnormal process termination bypass
normal package cleanup.

Current implementation status: semantic ownership classification, control-flow ownership-state merging,
move/reinitialization checks, checked borrows, generic-specialization propagation, typed-IR ownership effects,
initialization flags, and native struct/enum drop glue
are implemented. Local and by-value-parameter cleanup covers scope fallthrough, `return`, `break`, and `continue`;
recursive struct-field and fixed-array-element destruction is emitted in reverse order. Package storage participates in
exactly-once cleanup after a normal return from `main`.

Library owners and their borrowed views follow these same rules. See the [stdlib package documentation](stdlib/README.md#owned-collections) for constructors, operations and allocation errors.

Legacy enum member names must be unique within an enum. Legacy enum values are first-class:
they can be stored, compared for equality, passed, and returned. Variant fields are scalar primitive types initialized
by compile-time literals and can be read through member access. Unknown enum members are a compile error.

## Compile-time abstraction and sum types

Functions, structs and enums accept type parameters: `func identity<T>(value:T) -> T`,
`struct Pair<A,B>`, and `enum Option<T> { Some(T), None, }`. Calls infer one consistent substitution from argument
types; the expected return type does not infer missing arguments. Repeated occurrences of a parameter require the same
concrete type. Generic nominal types are invariant, including pointer levels, fixed-array lengths, slice elements, and
nested nominal arguments. An otherwise-equivalent concrete exact overload wins over a generic overload. Ambiguous
substitutions are rejected.

The compiler caches concrete specializations, with identities derived from the declaration and its complete type
arguments. Each module permits at most 256 specializations, declarations at most 16 type parameters, and aggregate
instantiation nesting at most 64 levels. Encoded specialization identities must fit 4095 bytes. By-value recursive
layouts require a pointer to break the cycle. Only concrete specializations reach typed IR and native emission.

`interface Printable { func toString() -> string; }` declares signatures. Interfaces may have type parameters, and
their requirements may be static: `interface FromResidual<R> { static func fromResidual(value:R) -> Self; }`.
A struct or enum implements an interface when its methods have the same names, static/instance form, parameter types and
return types. `Self` in a signature denotes the implementing type. Bounds such as
`T:Printable + Equal` or `T:Propagate<O,R>` check these methods without an implementation declaration. Generic calls
dispatch statically after specialization. Fixed arrays and slices of an interface may hold values of different
implementing structs. Assigning a struct to an interface element transfers a value into owned payload storage, and
calls through that element dispatch dynamically. Interface inheritance, associated types and default methods are
not supported. Concrete-to-interface conversion accepts copyable and move-only implementing structs in variables,
assignments, arguments, returns, fields, variant payloads, and array or slice elements. Erasure moves a move-only
source. Interface values are themselves move-only: passing or assigning one by value transfers it; partial moves
out of aggregate fields, array elements, or variant accessors are not supported. Destruction invokes the concrete
struct's drop glue, then releases the payload. Interface-valued fields, variants, and arrays therefore participate
in their enclosing value's ownership and cleanup.
Dynamic calls through interface values require an instance method with no method-level type parameters and no `Self`
in its parameters or return type. `Self` in these positions remains available through concrete receivers and generic
bounds, where the implementing type is known statically. Static interface requirements likewise require a concrete
type or generic bound. An interface value has a fixed 16-byte, eight-byte-aligned layout: a nonzero 64-bit concrete
type tag followed by an owned payload pointer. Both words are zero in an empty slot. Tags are deterministic FNV-1a
identities of the defining module and struct name (or the concrete specialization identity); collisions among known
structs are rejected. This fixes the layout independently of implementer size and allows recursive interface fields.
Dynamic dispatch is still closed-world: the current toolchain does not define an ABI for independently built dynamic
libraries introducing new implementers.

A sum enum gives each variant its own payload types. Construct values with
`Option<int>.Some(42)` or `Option<int>.None`. The representation stores a tag followed by storage for the largest
variant. Assignment, arguments and returns copy the complete aggregate; copying pointers or strings retains explicit
ownership without destruction. Sum values do not support implicit equality or direct payload member access. For a
variant with exactly one payload, `value.Variant()` returns that payload:
`result.Ok()`, `result.Err()`, and `option.Some()` are instance accessors with no arguments. The result has the
specialized payload type. The receiver is evaluated once, and accessing a variant that is not active traps before
reading its payload. Variants with zero or multiple payloads require `match`. Constructors remain type-qualified, for
example `Result<int,string>.Ok(42)`.

Enums may declare instance or static methods after a semicolon separating them from variants:
`enum Status { Ok, Err,; static func make() -> Status { return Status.Ok; } }`.

`match (value) { Some(v) => return v; None => return fallback; }` is a statement; braced arms with final expressions
make it a value expression. Variant names are relative to the
scrutinee enum. Bindings have the exact payload types and are scoped to their arm. Every variant must be covered unless
`_` provides a wildcard arm. Duplicate variants, wrong binding counts and unreachable arms are errors. Payload
extraction is emitted only in a branch guarded by the corresponding tag test; typed IR verifies that guard. An invalid
runtime tag traps.

The standard propagation types and interfaces are documented in [stdlib](stdlib/README.md#value-types-and-interfaces).

Functions declared inside a struct are invoked as instance methods, while
`static func` members are invoked on the struct type. Top-level and method link names encode canonical package identity,
overload signatures and owner types, so source-level function names remain usable without breaking platform calls. Names
beginning with
`__dmm_` are reserved.

`sizeof(T)` and `alignof(T)` are compile-time `usize` expressions for complete sized non-void types, including
specialized generic types. Pointers are 8 bytes and slice descriptors are 16 bytes with alignment 8. Primitive sizes
follow their widths. The current backend reserves 8-byte storage slots for aggregate fields and fixed-array storage;
these queries reflect that ABI, including padding, rather than a packed C layout.

Types also expose compile-time metadata: `T.name` is a `string`, and `T.size` and `T.align` are `usize` values. These
properties support concrete types, generic parameters after specialization, pointers, arrays, and slices. Named types
include their package identity in their name. For `void`, the size is 0 and the alignment is 1.

`expression.type` accesses the static type associated with an expression without evaluating it; for example,
`value.type.name` or
`make_value().type.size`. Type metadata cannot be stored as a runtime value.

Use `match (T)` or `match (value.type)` with `case Type -> ...` arms to select code at compile time. Only the selected
arm is analyzed for a concrete specialization, so other arms may use operations specific to their own types.
`case _ -> ...` supplies a fallback. Type patterns distinguish primitive types, pointer shapes, array lengths, and
generic specializations. Enum value matches retain their existing syntax and behavior.

Generic functions accept explicit type arguments: `identity<i32>(value)` or `core.alloc<Node>()`. Arguments must satisfy
the function's arity, parameter types and interface bounds. Existing inference remains available when value parameters
determine every type argument; a return type alone does not infer one.

`reserve(type)` zero-initializes one complete sized non-void object and returns a pointer to that type. It accepts a
type rather than a runtime count. `free(value)` releases a raw pointer or owned string, never a slice descriptor or
fixed array directly. Explicit pointer-to-pointer casts such as `data.(*Node)` reinterpret the address; validity and
alignment remain caller responsibilities. Raw allocations require explicit releases; `@gc` and implicit release of
raw allocation pointers have been removed. This does not suppress type-derived destructors for initialized values with
`NEEDS_DROP`.

Nested fixed arrays use recursive row-major storage with no hidden descriptors. Nested slices retain one two-word
descriptor for each slice value. Layout, generic identity, overload matching, argument and return classification,
ownership properties, and reverse-order destruction all use the complete recursive element type.

Standard-library functions use the ordinary call and overload rules. Their APIs are documented in the [package index](stdlib/README.md).

Function overloads differ by ordered parameter types, never return type. Exact matches beat promotions and other allowed
numeric conversions. A candidate must be no worse in every argument and better in at least one; ties are ambiguous.
`main` cannot be overloaded. Overloaded functions and methods use type-derived link names.

## Native FFI

`extern "system" [from "library"] { ... }` contains native function declarations and
native structs. `from` is a logical library ID, for example `c` or `ws2_32`, rather than
a DLL, SONAME, path or linker option. IDs begin with a letter or underscore and then
contain letters, digits, underscores or hyphens. `from` has no effect on type declarations.

```dmm
extern "system" {
    pub struct Handle;
    pub struct Record {
        pub var count:u32;
        pub var bytes:u8[3];
    }
}
extern "system" from "example" {
    pub func transform(value:Record, handle:*Handle) -> Record = "native_transform";
}
```

Imports have explicit return types and no bodies; aliases must be C identifiers.
Imports require `from`. Visibility and package resolution follow ordinary declarations.
Native structs allow only fields, with no generics, methods, destructors or initializers.
Opaque structs are permitted only behind raw pointers. Empty native structs and by-value
cycles are rejected; pointer recursion is allowed.

FFI values are fixed-width integers, `isize`, `usize`, `float`, `double`, `bit`, raw
pointers and complete native structs. `bit` is one byte with 0/1 semantics and corresponds
to C `_Bool`; Win32 `BOOL` is `i32`. `int`, `char` and `byte` are excluded as FFI values.
`void` is only a result or pointer pointee. Fixed arrays are permitted as native fields,
with C element strides, but cannot be direct parameters or results. Strings, slices,
checked references, ordinary DMM aggregates, enums, interfaces and futures cannot cross
the boundary by value. No implicit marshaling occurs.

Native structs use the output target's C size, alignment, field padding and tail padding.
They are copyable and have no drop glue. `sizeof`, `alignof`, `.size` and `.align` use the
same layout calculation as IR. Ordinary DMM aggregate and array storage retains its
eight-byte slots. Native imports are distinct from DMM function implementations in IR.

A native declaration is a trust boundary: the binding package claims that its signature
matches the native symbol. Native functions are not automatically memory-safe. Incorrect
bindings and invalid pointers can invalidate normal memory-safety guarantees. Bindings
own the responsibility for native lifetimes, ownership, alignment and synchronization.
There is no new `unsafe` syntax. References require explicit pointer casts, for example
`((&context).(*void))`. A pointer to an ordinary DMM type may serve as an opaque address;
casting it to a native struct pointer neither converts nor validates its memory layout.

Direct native calls support scalar and struct parameters and results on both x86-64
targets. Linux uses the System V INTEGER/SSE/memory classification; Windows uses its
position-based register convention, shadow space and indirect aggregate copies. Hidden
result pointers are generated when required. Small integer and boolean results are
normalized from their defined width. Struct arguments are captured when each argument
is evaluated, before a later argument can change its source. Calls may change memory.
Native struct fields and copies use byte-exact operations, including structs contained
in DMM aggregates and arrays. Native field arrays retain C element strides.

Only imports referenced by emitted code or global function-pointer initializers
require their logical libraries. Such
imports select the platform runtime and external GCC/Clang linking with `--link=auto`;
`--link=internal` diagnoses the native dependency. `--native-library NAME=PATH` selects
an explicit library/archive/object file, and repeatable `--native-library-dir DIR`
adds search directories. `-c` and `-S` emit unresolved symbols without invoking a
linker. `--dump-native-link FILE` records the target, runtime profile and used imports
for manual linking. A failed external link preserves the previous executable.

Native function values use `extern "system" func(...) -> T`. Their parameters and
results obey the same FFI rules as imports. Import names and native exports produce
such values; ordinary DMM functions require a matching explicit native export and
cannot be implicitly converted. Function-pointer types include the ABI in their
identity. A zero-initialized function pointer represents a null callback; calling it
traps. Native function values can be stored, passed to native functions and returned
by native functions. There are no captured closures or generated closure trampolines.

```dmm
package callbacks;
export "system" func onValue(value:i32) -> i32 { return value+1; }
extern "system" from "example" {
    pub func invoke(callback:extern "system" func(i32) -> i32) -> i32;
}
pub func run() -> i32 { return invoke(onValue); }
```

Exports have bodies, are synchronous and non-generic, and expose their source name
as a stable native entry address. The compiler bridges the native ABI to the DMM
body. Exported functions are retained even without a DMM caller. Native callbacks
may run concurrently on native threads; the caller must initialize those threads
through the appropriate platform API and retain contexts, buffers and resources
until all calls have ended, for example after a successful join. Exceptions and
foreign unwinding across DMM frames are excluded. Windows native entries and their
synchronous DMM function bodies have unwind metadata for OS stack inspection.

Native `union` declarations use the field syntax of native structs. Fields overlap
at offset zero; there is no discriminant or active-field check. `pack(N)` caps field
alignment and `align(N)` raises the aggregate alignment. Both are prefix modifiers
inside an extern block, after optional `pub`, with N equal to 1, 2, 4, 8 or 16:

```dmm
extern "system" {
    pub union Word { pub var integer:u64; pub var floating:double; }
    pub pack(1) struct Event { pub var events:u32; pub var data:Word; }
    pub align(16) struct Aligned { pub var value:u64; }
}
```

Packing and explicit alignment apply to the target C layout and ABI classification.
Unaligned aggregate fields require System V memory passing. Opaque declarations
remain unmodified `struct Handle;`. Native values retain their alignment within
DMM structs, enum payloads and arrays; the DMM slot representation includes padding
where required. Layout modifiers do not change primitive DMM scalar representation.

Package loading selects files ending in `_linux.dmm` for ELF and `_windows.dmm` for
COFF before parsing, based on the output target even during cross-compilation. All
other DMM source files remain shared. An explicitly selected file with the wrong
suffix is rejected. `manifest sync` discovers dependencies from both target variants.
Raw platform bindings live in `stdlib/native/{glibc,pthreads,kernel32,ucrt,winsock}`.
Their layouts target x86-64 glibc or MinGW-w64 UCRT64 specifically; the binding caller
must not copy initialized native synchronization objects or free active operations.
The staged contract is in [plans/ffi.md](plans/ffi.md).

## Implementation limits

`--runtime-component` is an explicit compiler bootstrap mode, not an FFI language feature. It produces a
library object or assembly with no application startup, package initialization/cleanup or automatic runtime
linkage. Only the fixed private platform thread/event/exit exports and `__dmm_platform_thread_entry` are allowed;
other runtime-reserved function names remain rejected. Components use scalar/POD values, raw pointers and
native calls. Compiler base helpers must be declared as explicit native imports if used. Implicit runtime
dependencies, reachable async/drop bodies and runtime global initializers are rejected. This mode supplies
the DMM platform runtime described in [plans/ffi.md](plans/ffi.md).

Tokens are at most 511 bytes, an expression is at most 512 tokens, and a local object or function stack frame is at most
8 MiB. Exceeding a limit is a compilation error, never silent truncation.

## Diagnostics and output

Normal diagnostics are human-readable. `--formatError` emits a single JSON document with `errors` and `summary`; each
error records its category, code, line, and column. JSON mode emits no ANSI escapes or unrelated output. The compiler
produces GNU x86-64 assembly for ELF/System V or COFF/Windows in Intel or AT&T syntax.


## Grammar

The following EBNF defines lexical and syntactic structure; the preceding sections define semantic validity.

### Notation

```ebnf
x | y        (* alternative *)
[x]          (* optional *)
{x}          (* zero or more repetitions *)
(x)          (* grouping *)
"text"       (* terminal text *)
```

Whitespace separates tokens and is otherwise insignificant. A line comment begins with `//` and continues through the
end of the line. A non-nesting block comment begins with `/*` and ends at the next `*/`, and may span lines.

### Lexical grammar

```ebnf
letter          = "A" … "Z" | "a" … "z" | "_" ;
digit           = "0" … "9" ;
identifier      = letter, { letter | digit } ;

integer         = digit, { digit } ;
exponent        = ("e" | "E"), ["+" | "-"], digit, { digit } ;
floating        = digit, { digit }, ".", digit, { digit }, [exponent]
                | digit, { digit }, exponent ;

escape          = "\\", ("n" | "t" | "r" | "0" | "\\" | "'" | '"') ;
character       = "'", (escape | character-byte), "'" ;
string          = '"', { escape | string-byte }, '"' ;

keyword         = "func" | "var" | "return" | "for" | "if" | "else"
                | "while" | "const" | "break" | "continue"
                | "struct" | "enum" | "import" | "static" | "reserve"
                | "free" | "interface" | "match" | "package" | "pub"
                | "sizeof" | "alignof" | "slice" | "case" | "typeof"
                | "destructor" | "defer" | "mut" | "async" | "await"
                | "extern" | "from" | "export" | "union" ;

primitive-type  = "int" | "char" | "byte" | "bit"
                | "float" | "double" | "string" | "void" | "never"
                | "i8" | "u8" | "i16" | "u16" | "i32" | "u32"
                | "i64" | "u64" | "isize" | "usize" ;
```

`character-byte` excludes quote, backslash, and line terminators. `string-byte` excludes double quote, backslash, and
line terminators. A leading sign is parsed as a unary operator rather than as part of a numeric token.

### Program grammar

```ebnf
program         = "package", identifier, ";", { top-level-declaration }, end-of-file ;

top-level-declaration
                = import-declaration
                | extern-block
                | native-export
                | ["pub"], (function-declaration | struct-declaration
                | enum-declaration | constant-declaration | package-variable
                | interface-declaration) ;
package-variable = "var", identifier, [":", type], ["=", expression], ";" ;

import-declaration
                = "import", (import-entry | "(", import-entry, {import-entry}, ")"), ";" ;
import-entry    = [identifier], string ;
qualified-name  = identifier, [".", identifier] ;

function-declaration
                = ["async"], "func", identifier, [generic-parameters], "(", [parameter-list], ")",
                  "->", return-type, block ;
parameter-list  = parameter, { ",", parameter } ;
parameter       = identifier, ":", type ;
return-type     = type ;
type            = ["&", ["mut"]], {"*"},
                  (native-function-type | function-type | async-type | primitive-type | qualified-name, [type-arguments] | "(", type, ")"),
                  {"[", [integer | identifier], "]"} ;
function-type   = "func", [generic-parameters], "(", [type-list], ")", "->", type ;
native-function-type = "extern", "\"system\"", "func", "(", [type-list], ")", "->", type ;
async-type      = ("Future" | "JoinHandle"), "<", type, ">" | "Executor" ;
type-arguments  = "<", type, {",", type}, ">" ;
generic-parameters = "<", generic-parameter, {",", generic-parameter}, ">" ;
generic-parameter = identifier, [":", type, {"+", type}] ;
constant-declaration
                = "const", identifier, [":", type], "=", expression, ";" ;
```

Struct and enum members use the same function and variable declaration forms accepted by their parser contexts:

```ebnf
struct-declaration
                = "struct", identifier, [generic-parameters], "{", { struct-member }, "}", [";"] ;
struct-member   = ["pub"], (field-declaration | ["static"], function-declaration)
                | destructor-declaration ;
destructor-declaration = "destructor", block ;
field-declaration
                = "var", identifier, ":", type, ";" ;

enum-declaration
                = "enum", identifier, [generic-parameters], ["(", enum-field-list, ")"],
                  "{", [enum-value, { ",", enum-value }, [","]],
                  [";", {enum-method}], "}" ;
enum-field-list = enum-field, { ",", enum-field } ;
enum-field      = ["pub"], identifier, ":", type ;
enum-value      = ["pub"], identifier, ["(", [enum-argument-list | type-list], ")"] ;
type-list       = type, {",", type} ;
enum-method     = ["pub"], ["static"], function-declaration ;
interface-declaration = "interface", identifier, [generic-parameters],
                        "{", {interface-method}, "}" ;
interface-method = ["pub"], ["static"], "func", identifier,
                   "(", [parameter-list], ")", "->", type, ";" ;
enum-argument-list
                = enum-argument, { ",", enum-argument } ;
enum-argument   = integer | floating | character | string
                | "true" | "false" ;
```

`resource` is not a keyword or declaration modifier. It is an ordinary identifier, and the former
`resource struct` spelling is rejected. Resource semantics are derived from the normal struct declaration's
destructor and concrete field types.

Enum field and member names are unique. Each value supplies exactly one compatible argument for every declared field.
Without header fields, variant parentheses contain payload types rather than constant arguments. Generic nominal names
followed by `.variant` accept type arguments in constructor expressions.

### Statements

```ebnf
block           = "{", { statement }, "}" ;

statement       = block
                | variable-declaration
                | assignment-statement
                | expression-statement
                | if-statement
                | while-statement
                | for-statement
                | return-statement
                | break-statement
                | continue-statement
                | match-statement
                | defer-statement
                | constant-declaration ;

variable-declaration
                = "var", identifier, [":", type], ["=", expression], ";" ;
variable-declaration-without-semicolon
                = "var", identifier, [":", type], ["=", expression] ;

assignment-statement
                = lvalue, assignment-operator, expression, ";"
                | lvalue, ("++" | "--"), ";" ;
assignment-operator
                = "=" | "+=" | "-=" | "*=" | "/=" ;
lvalue          = expression ; (* semantic analysis requires mutable storage *)

expression-statement  = expression, ";" ;
if-statement    = "if", "(", expression, ")", statement,
                  ["else", (if-statement | statement)] ;
while-statement = "while", "(", expression, ")", statement ;
for-statement   = "for", "(", [for-initializer], ";", [expression], ";",
                  [for-update], ")", statement ;
for-initializer = variable-declaration-without-semicolon
                | lvalue, "=", expression ;
for-update      = lvalue, assignment-operator, expression
                | lvalue, ("++" | "--") ;

return-statement = "return", [expression], ";" ;
break-statement  = "break", ";" ;
continue-statement = "continue", ";" ;
match-statement = "match", "(", (expression | type), ")", "{", {match-arm}, "}" ;
match-arm       = (identifier, ["(", [identifier, {",", identifier}], ")"] | "_"),
                  "=>", statement
                | "case", (type | "_"), "->", statement ;
defer-statement = "defer", (call-expression, ";" | "func", "(", ")", block) ;
```

### Expressions

The grammar encodes precedence from lowest to highest. Binary operators at each level associate left-to-right; unary
operators associate right-to-left.

```ebnf
expression      = logical-or ;
logical-or      = logical-and, { "||", logical-and } ;
logical-and     = comparison, { "&&", comparison } ;
comparison      = additive, { comparison-operator, additive } ;
comparison-operator
                = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
additive        = multiplicative, { ("+" | "-"), multiplicative } ;
multiplicative  = unary, { ("*" | "/" | "%"), unary } ;
unary           = ("!" | "-" | "*"), unary | "&", ["mut"], unary | postfix-expression ;
postfix-expression = primary, {postfix} ;

primary         = struct-literal | integer | floating | character | string
                | "true" | "false"
                | identifier
                | type-metadata
                | ("reserve" | "sizeof" | "alignof"), "(", type, ")"
                | "slice", "(", expression, ",", expression, ")"
                | array-literal
                | value-block | if-expression | match-expression
                | "free"
                | "(", expression, ")" ;

struct-literal  = qualified-name, [type-arguments], "{",
                  [initializer-field, {",", initializer-field}, [","]], "}" ;
initializer-field = identifier, ":", expression ;

array-literal   = "[", expression, {",", expression},
                  [";", expression], "]" ;

identifier-expression
                = identifier, { postfix } ;
postfix         = "(", [argument-list], ")"
                | type-arguments, ["(", [argument-list], ")"]
                 | "[", expression, "]"
                 | "[", [expression], ":", [expression], "]"
                 | ".", identifier
                 | ".", "await", "(", ")"
                 | ".", "(", type, ")"
                 | "?" ;
argument-list   = expression, { ",", expression } ;
call-expression = identifier-expression ;
type-metadata   = type, ".", ("name" | "size" | "align") ;

value-block     = "{", {statement}, [expression], "}" ;
if-expression   = "if", "(", expression, ")", value-block,
                  "else", value-block ;
match-expression = "match", "(", (expression | type), ")", "{", {value-match-arm}, "}" ;
value-match-arm = (identifier, ["(", [identifier, {",", identifier}], ")"] | "_"),
                  "=>", value-block
                | "case", (type | "_"), "->", value-block ;
```

`expression.type` is a member expression denoting compile-time static type metadata. It may be followed by `.name`,
`.size`, or `.align`, or used as a type-match scrutinee. Type matches use `case Type -> statement` and
`case _ -> statement`; enum matches use variant patterns with `=>`. See the language rules above for specialization and
unevaluated-expression rules. `typeof` is currently reserved by the lexer; use `.type` for static type access.

Value blocks use a final expression without a semicolon as their result. Value-producing `if` requires `else`;
value-producing `match` requires exhaustive arms. Branch typing and terminating paths follow the language rules above.

`Future`, `JoinHandle` and `Executor` are compiler-recognized type names, not lexer keywords. Their use and async
declarations require the root manifest's `async` feature. The consuming `.await()` postfix takes no arguments and is
valid only inside async functions. Prefix `await expression` is rejected. Async interface methods are not supported.

Array literals require an expected fixed-array or slice type. Their prefix is nonempty. In the repetition form
`[a,b,c;N]`, semantic analysis requires `N` to be a positive compile-time integer constant and produces exactly `N`
elements by cycling the prefix. An ordinary fixed-array literal must contain exactly the target length.

### Context-sensitive validity

Native declarations extend the top-level declaration alternatives with:

```ebnf
extern-block    = "extern", "\"system\"", ["from", string],
                  "{", {native-declaration}, "}" ;
native-declaration = ["pub"], (native-function | native-aggregate) ;
native-function = "func", identifier, "(", [parameter-list], ")", "->", type,
                  ["=", string], ";" ;
native-aggregate = {native-attribute}, ("struct" | "union"), identifier,
                   (";" | "{", native-field, {native-field}, "}", [";"]) ;
native-attribute = ("pack" | "align"), "(", integer, ")" ;
native-export   = ["pub"], "export", "\"system\"", "func", identifier,
                  "(", [parameter-list], ")", "->", type, block ;
native-field    = ["pub"], "var", identifier, ":", type, ";" ;
```

`from` is required for imports and denotes a logical library ID; it has no effect on types. Only structs may be opaque.
`pack` and `align` accept 1, 2, 4, 8 or 16. Native exports are synchronous and non-generic; native function-pointer
types have no generic parameters. Native signature and layout restrictions are defined in [Native FFI](#native-ffi).

The grammar describes structure only. A valid DMM program must also satisfy the rules above, including
declaration-before-use and scope rules, type compatibility, valid return paths, argument matching, loop-only `break`/
`continue`, object-size limits, and array-bounds requirements.
