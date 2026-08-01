# DMM Language Specification 2026-09-22-dev

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
[MODULE_SYSTEM.md](MODULE_SYSTEM.md).

The root manifest enables experimental async support with `features = ["async"]`. With that feature enabled,
an `async func (...) -> T` call has type `Future<T>`, while its body returns `T`. `Future<T>` is a compiler-owned type
with exactly one output type; `Future<void>` is valid. The consuming instance method `future.await()` is only valid
inside an async function, takes no arguments, and produces the Future's output. For example,
`var x:Future<int>=compute(); var y=x.await();` consumes `x` and gives `y` type `int`.
The prefix form `await x` is not part of the language and is rejected. Futures are move-only, including when transferred through function values
or generic functions. References to Futures cannot be awaited. Future output types are invariant: `Future<int>` does
not convert to `Future<byte>` even though the corresponding scalar conversion exists.

A live Future must be awaited or transferred on every reachable control-flow path. Discarding a Future expression,
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
Stage 1 conservatively retains all frame storage across suspension. Await evaluates its operand once, polls its child,
suspends on Pending, and resumes at its unique verified state. On Ready it moves the output and cleans up the child
exactly once. Normal return and error propagation run the existing defer/destructor paths exactly once. Invalid
resume states, polling a completed frame, and cleanup of an incomplete frame trap in the private ABI.
Both ELF and COFF native backends support this at O0 and O1. A private deterministic test driver exercises polling;
there is no public executor, spawn/cancel, socket or OS-I/O API in Stage 1. `main` remains synchronous.

`Send` and `Sync` are compiler-derived, structurally through aggregate fields and enum payloads. Shared checked
references are Send/Sync when their referent is Sync; mutable checked references are Send when their referent is Send.
Raw pointers, slices without a checked owner, function values and dynamic interface values are conservative.
A Future constructor receives Send only when its complete retained frame and output are Send. Futures with borrowed
parameters or an instance receiver are not Send and cannot become spawn candidates. Unknown/erased Future captures
remain conservative; `Future<T>` alone does not prove Send from `T`. Futures are not Sync because polling mutates them.
There are no explicit unsafe Send/Sync implementations in Stage 1.

The private Stage 2 runtime foundation is implemented in `src/runtime/executor.c`.
Its ready queue parks idle workers and serializes polling of each frame. A wake
received during a poll is retained across a Pending return. Task completion and
cancellation requests are ordered under the same lock. Cancellation uses a
separate callback and waits for child/I/O termination and awaitable cleanup;
requesting I/O cancellation does not constitute confirmation. A completed,
unclaimed result is destroyed exactly once when its handle is cancelled.
Drain closes admission and waits for normal completion; Cancel additionally
requests cancellation of active tasks. Both wait for worker exit. Outstanding
joins and retained wakers keep task/executor state alive after shutdown. Local
blocking polls run on the calling thread. The private default executor is lazy,
has two workers, and drains at normal process exit. Native ELF/COFF thread and
manual-reset wait-event primitives are emitted without an external linker handoff.

This foundation is not yet connected to language-level Future frames. The public
`Executor`, `JoinHandle<T>`, `ShutdownMode`, `TaskError`, `spawn`, `block_on`, and
`cancel` API, concrete-value spawn checks, cancellation loan transfer, generated
scope cleanup and cancellation IR/verifier transitions remain unimplemented.
The Stage 1 language contract above therefore remains the implemented contract;
the presence of private runtime helpers does not enable these source APIs.

The `core.AtomicBit` and `core.AtomicUsize` types are available independently of `async`. Construct them with
`core.atomicBit(initial)` and `core.atomicUsize(initial)`. Their `load`, `store`, `swap`, and
`compareExchange(expected, next)` methods all use sequentially consistent ordering. `compareExchange` returns the
previous value, whether or not the exchange succeeded; compare that value with `expected` to determine success.
On x86-64, an aligned word load is atomic, while stores and swaps use the implicitly locked memory `xchg` and
compare-exchange uses `lock cmpxchg`. All operations participate in a single sequentially consistent order compatible
with program order. The atomic wrapper types are move-only so they cannot be copied by ordinary aggregate assignment.
Their storage must not be accessed through a non-atomic alias while it may be shared. There is not yet a public
multi-thread executor or thread-spawning API.

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
a move-only pattern element. Erasing a move-only concrete value into a copyable interface container is rejected.

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

The `stdlib` package provides destructor-backed owning collections. `stdlib.bytes(capacity)` creates a growable byte
region; `stdlib.buffer<T>(count)` creates a fixed-length region; and `stdlib.list<T>(capacity)` creates a growable logical
sequence. Each owner is move-only, exposes `ok()` and an `error:AllocationError`, and releases its allocation exactly
once. `Bytes.push` and `List<T>.push` return `AllocationError`; the variants are `None`, `OutOfMemory`, and
`CapacityOverflow`. Their `view()` methods return borrowed slices, so any operation that can relocate or release the
owner is rejected while the view is live. `Buffer<T>` and `List<T>` currently require copyable element types for
`get`, `set`, growth, and destruction; dynamic element drop is not yet part of their contract.

`stdlib.cloneString(text)` creates a move-only owned string and reports allocation failure through the same typed
status. `String.view() -> string` returns a non-owning string view tied to the `String` receiver, and `length()` reports
its byte length. Moving, replacing, or destroying the owner while that view remains live is rejected.

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

`import "stdlib";` exports `stdlib.Option<T>`, `stdlib.Result<T,E>`, `stdlib.Propagation<O,R>`,
`stdlib.NoneResidual`, `stdlib.Propagate<O,R>`, `stdlib.FromResidual<R>`, and `stdlib.Cell<T>` with
`get`/`set` methods, `unwrapOr`, `Printable`, `Equal`, and `printValue`. See
`tests/execution/generics` and `tests/execution/propagation` for executable examples.

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

`stdlib/core` builds typed allocation, release and raw I/O helpers over compiler primitives. Its signatures, failure
values and allocation-base requirements are defined in [CORE_RUNTIME.md](CORE_RUNTIME.md).

Nested fixed arrays use recursive row-major storage with no hidden descriptors. Nested slices retain one two-word
descriptor for each slice value. Layout, generic identity, overload matching, argument and return classification,
ownership properties, and reverse-order destruction all use the complete recursive element type.

`stdlib.print(value)` and `stdlib.println(value)` are ordinary overloaded stdlib functions. Import `"stdlib"` to use
them; an empty line is `stdlib.println("")`. Calls evaluate their arguments before entering the output function.

`"stdlib/core"` exposes low-level byte-region allocation, explicit release, raw I/O and process primitives without
importing higher-level library functions. See [CORE_RUNTIME.md](CORE_RUNTIME.md) for signatures and ownership contracts.

`"stdlib/stdio"` provides synchronous streams, buffered adapters and bounded byte-oriented helpers. Its API, explicit
cleanup requirements and typed transfer/error results are defined in [STDIO.md](STDIO.md).

Function overloads differ by ordered parameter types, never return type. Exact matches beat promotions and other allowed
numeric conversions. A candidate must be no worse in every argument and better in at least one; ties are ambiguous.
`main` cannot be overloaded. Overloaded functions and methods use type-derived link names.

## Implementation limits

Tokens are at most 511 bytes, an expression is at most 512 tokens, and a local object or function stack frame is at most
8 MiB. Exceeding a limit is a compilation error, never silent truncation.

## Diagnostics and output

Normal diagnostics are human-readable. `--formatError` emits a single JSON document with `errors` and `summary`; each
error records its category, code, line, and column. JSON mode emits no ANSI escapes or unrelated output. The compiler
produces GNU x86-64 assembly for ELF/System V or COFF/Windows in Intel or AT&T syntax.
